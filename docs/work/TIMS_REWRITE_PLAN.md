# TiMS Backend Rewrite Plan

**Status**: Proposal — awaiting approval before implementation  
**Scope**: `src/tims_backend.cpp` + `include/corerat/ipc/tims/tims_backend.hpp` (STD path) +
          `include/corerat/ipc/evl/evl_backend.hpp` (EVL path)  
**Goal**: Eliminate the RACK library dependency and design a modern EVL-native IPC backend.

---

## 1. Full TiMS architecture — what we learned

Reading all TiMS source files reveals **three distinct transport paths**.

### 1.1 Linux / non-RT path (`tims/linux/tims_api_linux.c`)

Each process opens a TCP connection to a router daemon (`TimsRouterTcp`, port 2000).
The `RACK::rack` dependency exists only to wrap these POSIX operations:

| Function | What it actually does |
|---|---|
| `tims_mbx_create` | TCP `connect()` to `127.0.0.1:2000`; exchange `TIMS_MSG_ROUTER_MBX_INIT_WITH_REPLY` handshake |
| `tims_sendmsg` | `send()` 16-byte `tims_msg_head` then iovec body |
| `tims_recvmsg_timed` | `select()` + `recv()` loop; silently handles router watchdog probes |
| `tims_mbx_remove` | `close(fd)` |
| `tims_mbx_clean` | `recv()` drain with `MSG_DONTWAIT` |
| `tims_peek_timed` / `tims_peek_end` | **Not available on Linux** — RTDM-only |

### 1.2 Xenomai 3 / RTDM path (`tims/xenomai/`, `tims/xenomai_kmod/`)

A **kernel module** (`tims_driver.c`) registers an RTDM protocol family `PF_TIMS`.
User-space RT tasks interact via `rt_dev_*` system calls, which are OOB-safe (no Linux
kernel entry, no mode switch).

**Kernel-side data model** (`tims_driver.h`, `tims_driver.c`):

```
td (driver global)
├── ctx_lock               rtdm_lock for the mailbox registry
├── ctx_list               linked list of all open tims_ctx
├── ctxCache[64]           LRU address→ctx lookup cache
├── pipeToClient           RT_PIPE to tims_client daemon (kernel→TCP)
├── pipeFromClient         RT_PIPE from tims_client daemon (TCP→kernel)
└── pipeRecvTask           RT task draining pipeFromClient

tims_ctx (per open fd / per mailbox):
└── p_mbx → tims_mbx
    ├── address            uint32 mailbox ID
    ├── msg_size           max per-message bytes
    ├── slot_count         ring depth
    ├── slot[]             array of tims_mbx_slot (kernel-allocated)
    ├── free_list          slots available for writing
    ├── write_list         slots being filled
    ├── read_list          slots ready to read, priority-sorted
    ├── peek (p_peek)      slot locked for zero-copy read
    ├── readSem            rtdm_sem — reader blocks here
    └── list_lock          rtdm_lock for slot state machine
```

**Local send path** (RT task → RT task, same machine):
```
rt_dev_sendmsg(fd, msghdr{iov[]}) [OOB syscall]
  → kernel tims_ioctl / tims_sendmsg handler
  → tims_ctx_get(dest_address)    // cache lookup, O(1) for hot paths
  → tims_get_write_slot(dest_mbx, priority)
      if no free slot AND read_list has lower-priority msg → drop that msg
  → copy_msg_into_slot()          // rtdm_copy_from_user for iov[]
  → move slot: write_list → read_list (priority-ordered insertion)
  → rtdm_sem_up(&dest_mbx->readSem)   // wake the blocked receiver
```

**Local receive path** (zero-copy peek):
```
rt_dev_ioctl(fd, TIMS_RTIOC_RECVBEGIN, &peek_buf) [OOB syscall]
  → tims_peek_intern()
  → rtdm_sem_timeddown(&mbx->readSem, timeout_ns)  // OOB-blocking wait
  → move highest-priority slot: read_list → peek (locked)
  → rtdm_copy_to_user(peek_buf, slot)               // zero-copy: one copy out

rt_dev_ioctl(fd, TIMS_RTIOC_RECVEND)
  → move slot: peek → free_list
```

**Remote send path** (RT task → different machine):
```
rt_dev_sendmsg() → kernel: no local ctx found for dest
  → rt_pipe_send(&td.pipeToClient, ...)   // kernel → tims_client daemon
    → tims_client daemon receives from pipe
    → send() over TCP to tims_router_tcp on remote host
    → remote tims_router_tcp routes to remote tims_client
    → remote tims_client writes to remote pipeFromClient
    → remote kernel pipeRecvTask → copy into remote RT mailbox
```

**Optional: RTnet path** (`tims/xenomai_kmod/tims_rtnet.c`):
Direct real-time Ethernet (RTnet) bypasses TCP entirely for time-critical remote messages.
This is the highest-RT cross-machine path. Not used in current CommRaT deployments.

### 1.3 TiMS router daemon (`tims/router/tims_router_tcp.c`)

A pure-Linux daemon that serves as TCP message broker:
- `mbxList[MAX_MBX=1024]`: maps mailbox address → TCP connection index
- One thread per connected client (up to 32 connections)
- Routes by looking up `msg.dest` in `mbxList`, forwarding to that connection's socket
- Also handles `TIMS_MSG_ROUTER_MBX_INIT` (registration) and `TIMS_MSG_ROUTER_MBX_DELETE`
- Watchdog: pings clients periodically, drops connections on timeout

### 1.4 tims_client (`tims/xenomai/tims_client.c`)

Linux-space bridge between the Xenomai kernel module and the TCP router:
- Two RT pipes: `PIPE_TIMS_TO_CLIENT (6)`, `PIPE_CLIENT_TO_TIMS (7)`
- `tcpRecvThread`: reads from TCP router, writes into the RT pipe (→ kernel module)
- Main loop: reads from RT pipe (from kernel module), writes to TCP router
- `watchdogThread`: monitors TCP connection health, auto-reconnects
- Registered mailboxes are announced to the router so it knows where to route replies

---

## 2. What this means for CoreRaT

| Capability | Xenomai 3 (RTDM) | EVL (Xenomai 4) | STD (Linux) |
|---|---|---|---|
| Local IPC mechanism | Kernel RTDM socket, `rt_dev_*` | **SHM ring + evl_mutex + evl_event** | TCP via router daemon |
| Kernel module required | Yes (`tims_driver.ko`) | **No** | No |
| Router daemon required | For remote only | **No (local)** | Yes (all IPC) |
| OOB receive (no mode switch) | Yes (`rtdm_sem_timeddown`) | **Yes (`evl_timedwait_event`)** | No |
| Priority-sorted mailbox | Yes (kernel `read_list`) | **Yes (ring with priority field)** | No (FIFO TCP) |
| Zero-copy receive | Yes (`RECVBEGIN`/`RECVEND`) | Optional (peek pointer) | No |
| Cross-machine IPC | TCP via tims_client+router | TCP forwarder (non-OOB) | TCP via router |

The core architectural insight: **EVL replaces the RTDM kernel module entirely.**  
The kernel module's job was to provide priority-sorted, OOB-capable, shared message queues.
EVL provides all the necessary user-space primitives to replicate this without a kernel module.

---

## 3. Wire format

### 3.1 TCP transport framing (STD backend only)

```
┌─────────────────────────────────────────────────────────────┐
│  FrameHeader  (16 bytes, packed — identical to tims_msg_head)│
│    flags:uint8  type:int8  priority:uint8  seq_nr:uint8      │
│    dest:uint32  src:uint32  msglen:uint32                     │
├─────────────────────────────────────────────────────────────┤
│  WireMessage<T> body  (32-byte WireHeader + T payload)       │
└─────────────────────────────────────────────────────────────┘
msglen = sizeof(FrameHeader) + sizeof(WireMessage<T>)
```

The `FrameHeader` is the TCP routing envelope — only the STD backend uses it.

### 3.2 Shared-memory framing (EVL backend)

```
Ring slot:
┌─────────────────────────────────────────────────────────────┐
│  WireMessage<T>  (directly — no FrameHeader wrapping)        │
│    WireHeader: msg_type, msg_size, timestamp, seq,           │
│                dest, src, flags  (32 bytes)                  │
│    Payload: T                                                │
└─────────────────────────────────────────────────────────────┘
```

No routing envelope. The mailbox ID is the identity of the shared memory segment itself.

---

## 4. Plan A — STD backend rewrite (drop RACK, keep TCP)

### 4.1 New internal header `include/corerat/ipc/tims/protocol.hpp`

Replaces all RACK includes in `tims_backend.cpp`:

```cpp
namespace corerat::tims_proto {

inline constexpr int8_t MSG_OK                         =  0;
inline constexpr int8_t MSG_ROUTER_MBX_INIT_WITH_REPLY = 13;
inline constexpr int8_t MSG_ROUTER_GET_STATUS          = 17;
inline constexpr int8_t MSG_ROUTER_DISABLE_WATCHDOG    = 19;

struct FrameHeader {
    uint8_t  flags;     // 0x03 = little-endian head+body
    int8_t   type;
    uint8_t  priority;
    uint8_t  seq_nr;
    uint32_t dest;
    uint32_t src;
    uint32_t msglen;    // total length including this header
} __attribute__((packed));
static_assert(sizeof(FrameHeader) == 16);

inline constexpr uint8_t FRAME_LE_FLAG = 0x03;

}  // namespace corerat::tims_proto
```

### 4.2 `TimsMailbox` implementation — `src/tims_backend.cpp`

The **public API** (`tims_backend.hpp`) is **unchanged**.
Internally `TimsMailbox` directly owns the TCP socket:

```cpp
int      fd_{-1};        // TCP socket to router
uint8_t  seq_{0};        // send sequence counter
uint32_t mailbox_id_{0};
```

**`initialize()`**:
```
1. socket(AF_INET, SOCK_STREAM) + connect(127.0.0.1:2000)
2. setsockopt(TCP_NODELAY)
3. send MSG_ROUTER_DISABLE_WATCHDOG frame  (16 bytes)
4. send MSG_ROUTER_MBX_INIT_WITH_REPLY frame  (16 + 4 bytes)
5. recv reply; check type == MSG_OK
```

**`send_raw()`**:
```
1. build FrameHeader on stack
2. send(fd_, &header, 16, MSG_NOSIGNAL)
3. send(fd_, wire_msg_bytes, size, MSG_NOSIGNAL)
```

**`receive_raw()`** (replaces `select` with `poll`):
```
1. poll(fd_, POLLIN, timeout_ms)
2. recv loop: fill FrameHeader (16 bytes)
3. if frame.type == MSG_ROUTER_GET_STATUS → send MSG_OK reply, goto 1
4. recv loop: fill body (msglen − 16 bytes)
5. return bytes received, populate TimsMetadata
```

**`has_message()`**: `poll(fd_, POLLIN, 0)` — non-blocking check.

**`shutdown()` / `clean()`**: `close(fd_)` and drain loop — trivial.

### 4.3 What is removed

| Removed | Replaced by |
|---|---|
| `find_package(RACK REQUIRED)` in CMakeLists | — |
| `target_link_libraries(... RACK::rack)` | — |
| `#include <main/tims/tims.h>` | `#include "protocol.hpp"` (internal) |
| `tims_fill_head()`, `tims_parse_head_byteorder()` | inline in `TimsMailbox` |
| `struct iovec` in send | direct `send()` on `WireMessage` bytes |
| `tims_peek_timed()` / `tims_peek_end()` | `poll(fd_, POLLIN, 0)` |
| `select()` | `poll()` |

### 4.4 Files affected (STD)

| File | Change |
|---|---|
| `include/corerat/ipc/tims/protocol.hpp` | **New** — internal constants + `FrameHeader` |
| `include/corerat/ipc/tims/tims_backend.hpp` | Remove RACK includes |
| `src/tims_backend.cpp` | **Rewritten** (~180 lines, no RACK C API) |
| `CMakeLists.txt` | Remove `find_package(RACK)` / `RACK::rack` |

Wire format is byte-identical to the original — existing CommRaT/RACK nodes interoperate unchanged.

---

## 5. Plan B — EVL backend (`EvlMailbox`)

This is the replacement for the Xenomai 3 RTDM path. It uses EVL user-space OOB primitives
to provide the same guarantees the kernel module provided — without any kernel module.

### 5.1 EVL primitives used

| EVL primitive | Replaces (Xenomai 3 RTDM) |
|---|---|
| `evl_mutex` (PI) | `rtdm_lock_t` + `rtdm_sem` in kernel |
| `evl_event` | `rtdm_sem_t readSem` — blocking receive |
| `evl_timedwait_event()` | `rtdm_sem_timeddown()` |
| `memfd_create()` + `mmap()` | `kmalloc()` slot buffer in kernel |
| `evl_read_clock(EVL_CLOCK_MONOTONIC)` | `rtdm_clock_read()` |

All EVL primitives are OOB-capable: an RT thread waiting in `evl_timedwait_event()` does
**not** cause a mode switch. This matches the `rtdm_sem_timeddown()` behaviour exactly.

### 5.2 Shared memory layout

Each mailbox corresponds to one anonymous shared memory region opened via `memfd_create`.

```
EvlRing (mapped into all communicating processes):
┌──────────────────────────────────────────────┐
│ evl_mutex   ring_lock  (PI mutex)             │  ← OOB lockable
│ evl_event   data_ready (condition)            │  ← OOB waitable
│ uint32_t    head                              │  ← read index
│ uint32_t    tail                              │  ← write index
│ uint32_t    slot_count                        │
│ uint32_t    slot_size                         │
│ Slot[N]:                                      │
│   int8_t    priority                          │  ← for ordering
│   uint8_t   in_use                            │
│   uint8_t   data[slot_size]                   │  ← WireMessage bytes
└──────────────────────────────────────────────┘
```

Priority ordering: on insert, the new slot is placed before any existing slot of lower
priority — matching the kernel module's `_move_write_to_read()` behaviour.

### 5.3 Operations

**`create(mailbox_id, slots, max_msg_size)`**:
```
1. memfd_create("corerat_mbx_<id>")  [or shm_open for cross-process]
2. ftruncate to sizeof(EvlRing) + slots * max_msg_size
3. mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
4. evl_new_mutex_any(&ring->ring_lock, EVL_CLOCK_MONOTONIC, 0,
                     EVL_T_WOSS|EVL_T_HMSIG, "ring_lock_%u", id)
5. evl_new_event_any(&ring->data_ready, EVL_CLOCK_MONOTONIC,
                     EVL_EVENT_PULSE, "ring_event_%u", id)
6. ring->slot_count = slots; ring->slot_size = max_msg_size
```

**`send(wire_msg_bytes, size, priority)`**:
```
1. evl_lock_mutex(&ring->ring_lock)        [OOB-capable]
2. find free slot; if none: drop lowest-priority slot if prio_new >= prio_old
3. memcpy(slot->data, wire_msg_bytes, size)
4. slot->priority = priority; slot->in_use = 1
5. insert slot before first lower-priority slot (priority ordering)
6. evl_unlock_mutex(&ring->ring_lock)
7. evl_signal_event(&ring->data_ready)     [wake any waiting reader]
```

**`receive(buffer, timeout_ns, out_header)`** (copying receive):
```
1. evl_lock_mutex(&ring->ring_lock)
2. if no in_use slot:
       evl_unlock_mutex(&ring->ring_lock)
       evl_timedwait_event(&ring->data_ready, &ring->ring_lock, &abs_timeout)
3. pop highest-priority slot (head of priority-sorted list)
4. memcpy(buffer, slot->data, slot->slot_size)
5. slot->in_use = 0
6. evl_unlock_mutex(&ring->ring_lock)
7. parse WireHeader from buffer → out_header
```

**`peek()` / `peek_end()`** (zero-copy, analogous to `RECVBEGIN`/`RECVEND`):
```
peek():     lock ring, return pointer to highest-priority slot->data (keep locked)
peek_end(): slot->in_use = 0, evl_unlock_mutex
```

### 5.4 Cross-process access

For IPC between separate processes (not just threads), the `memfd` file descriptor must be
shared. Options:

- **Preferred**: a lightweight `CoreRaT name service` — a small Unix domain socket server
  that hands out `memfd` fds via `SCM_RIGHTS` when a process creates or opens a named mailbox.
  This is the EVL equivalent of the TIMS kernel module's `ctx_list` (the address registry).

- **Alternative**: `shm_open("/corerat_mbx_<id>", ...)` — simpler, no daemon, but requires
  POSIX SHM which is backed by tmpfs rather than anonymous memory.

For the first implementation (Phase 3 of CoreRaT), **single-process / multi-thread** is
sufficient — all `EvlMailbox` instances share the same process, so `memfd` + `mmap` works
directly without any name service.

### 5.5 Remote IPC (cross-machine, EVL)

Real-time hard deadlines exist only within a single machine. Cross-machine messages are
inherently non-deterministic (network). Therefore:

- An OOB RT task writes to a local `EvlMailbox` (fast, OOB)
- A dedicated **bridge thread** (non-OOB) reads from the mailbox and forwards via TCP
- The bridge is analogous to the old `tims_client` daemon, but simpler: no RT pipes,
  no kernel module, just `EvlMailbox::receive()` + `send()` to a TCP socket
- The same `TimsRouterTcp`-style daemon (or a simplified successor) handles the remote side

### 5.6 Files affected (EVL)

| File | Change |
|---|---|
| `include/corerat/ipc/evl/evl_backend.hpp` | **Implement** ring struct + operations (currently stub) |
| `include/corerat/ipc/mailbox.hpp` | Conditional: select `EvlMailbox` when `CORERAT_IPC=EVL` |
| `CMakeLists.txt` | `find_package(evl)` only when `CORERAT_IPC STREQUAL "EVL"` |

---

## 6. What is NOT ported

| Item | Reason |
|---|---|
| Xenomai 3 RTDM kernel module (`tims_driver.ko`) | Replaced entirely by EVL SHM approach |
| `tims_client` daemon (RT pipe bridge) | No RT pipes in EVL; bridge thread handles TCP |
| RTnet integration (`tims_rtnet.c`) | RTnet is Xenomai 3 specific; EVL uses standard network stack |
| RT_PIPE API (`native/pipe.h`) | Xenomai 3 API; not available in EVL |

---

## 7. Sequencing

**Phase 1 (this plan, STD only)**:
1. Implement `protocol.hpp`
2. Rewrite `tims_backend.cpp` — direct POSIX sockets, no RACK
3. Remove `find_package(RACK)` from `CMakeLists.txt`
4. Verify with existing CommRaT/RACK router

**Phase 2 (EVL backend)**:
1. Implement `EvlMailbox` — single-process SHM ring
2. Wire into `Mailbox<>` frontend when `CORERAT_IPC=EVL`
3. Add EVL CMake preset

**Phase 3 (cross-process EVL, if needed)**:
1. Decide: `shm_open` vs `memfd` + name service
2. Implement cross-process fd sharing
3. Optional: lightweight TCP bridge for remote cross-machine IPC

---

## 8. The `tims/` source directory

Reference material — not compiled by CoreRaT after the rewrite. Keep it in the repo tree
(it's the source of record for the wire protocol and the Xenomai 3 design).

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Byte order in FrameHeader | On LE targets always set `flags = 0x03`; verified against `tims_byteorder.h` |
| Router watchdog probe during recv | Probes arrive between complete messages (not mid-stream); handle with goto-loop in recv |
| `TCP_NODELAY` | Already set in original `tims_mbx_create`; keep it |
| EVL `evl_mutex` starvation | Use `EVL_T_WOSS` (wait on same schedulable) mode to ensure bounded wait |
| EVL OOB thread creation | Each RT thread must call `evl_attach_self()` before using `evl_lock_mutex` |
| Priority inversion in ring | `evl_mutex` with `EVL_T_PI` (priority inheritance) prevents this |
| SHM layout versioning | Embed a `uint32_t magic` + `uint32_t version` at ring offset 0 |

---

## 10. Payload encoding — native layout vs SeRTial packing *(future investigation — not implementing now)*

> SeRTial already does `memcpy` internally wherever `can_memcpy_whole_v<T>` is true, so
> there is no immediate performance gap to close. The `WireEncoding::Native` override is
> documented here for reference but will not be added to `MessageDefinition` until
> profiling shows a measurable benefit.

### 10.1 The problem

SeRTial serializes a struct field-by-field, stripping compiler-inserted padding.
This is correct for cross-architecture portability but costs CPU cycles.
For local IPC on a fixed architecture the in-memory layout **is** the wire layout —
a plain `memcpy` of `sizeof(T)` bytes is sufficient and zero-overhead.

However, for padded types the opposite is also true: packing *reduces* wire load.
A struct like `{ uint8_t a; uint32_t b; }` is 8 bytes in memory but only 5 bytes packed —
always sending 8 bytes wastes 37% of the wire capacity for that message.

### 10.2 SeRTial already has the right tools

`TypeTraits<T>` in `<sertial/core/traits/type_info.hpp>` provides all the necessary
compile-time analysis — no need to roll our own concepts:

| Trait | Meaning |
|---|---|
| `TypeTraits<T>::can_memcpy_whole` | `true` iff no padding, trivially copyable, static size — safe to `memcpy(sizeof(T))` |
| `TypeTraits<T>::has_padding` | `true` if `sizeof(T) > packed_size` (detected via `rfl` reflection) |
| `TypeTraits<T>::packed_size` | sum of field sizes without padding (compile-time) |
| `TypeTraits<T>::unpacked_size` | `sizeof(T)` |
| `has_padding_v<T>` | shorthand for `TypeTraits<T>::has_padding` |
| `can_memcpy_whole_v<T>` | shorthand for `TypeTraits<T>::can_memcpy_whole` |

### 10.3 Automatic dispatch

`send<T>()` / `receive<T>()` in `Mailbox<>` select the path at compile time:

```cpp
if constexpr (sertial::can_memcpy_whole_v<T>) {
    // Fast path: no padding, trivially copyable, static size
    // → write sizeof(T) bytes directly, no SeRTial overhead
    backend.send_raw(reinterpret_cast<const std::byte*>(&payload), sizeof(T));
} else {
    // SeRTial path: strips padding, handles dynamic types, portable
    auto buf = sertial::serialize(payload);
    backend.send_raw(buf.data(), buf.size());
}
```

**What actually reaches the memcpy path** — `can_memcpy_whole` requires all three:
- `!has_padding` — no padding bytes (SeRTial detects this via `rfl` field reflection)
- `is_trivially_copyable` — no user-defined copy/move, no virtuals
- `category == SizeCategory::Static` — fixed size known at compile time

`TypeTraits` currently returns `SizeCategory::Dynamic` for **all** user-defined struct
types (comment in source: *"would need field recursion for Static"*). This means
`can_memcpy_whole = false` for all structs — they always go through SeRTial.
In practice this is correct and safe:

| Type | `category` | `can_memcpy_whole` | What `sizeof(T)` would give | Who handles it |
|---|---|---|---|---|
| `int`, `float`, `uint32_t` | `Static` | `true` | exact | memcpy |
| `std::array<float, 3>` | `Static` (recursive) | `true` if no padding | exact | memcpy |
| Any struct / class | `Dynamic` | `false` | includes padding | SeRTial |
| `fixed_vector<T,N>` | `Dynamic` | `false` | **capacity**, not actual size | SeRTial |
| `fixed_string<N>` | `Dynamic` | `false` | **capacity**, not actual size | SeRTial |

**Nested structs**: `has_padding_v<T>` is shallow — it sums `sizeof(field)` per field,
not `packed_size(field)`, so it does not detect padding *inside* a nested struct. Example:

```cpp
struct Inner { uint8_t a; uint32_t b; };  // sizeof=8, packed=5 — has internal padding
struct Outer { Inner i; float f; };       // sizeof=12 = sizeof(Inner)+sizeof(float)
                                          // has_padding_v<Outer> == false (no inter-field gap)
                                          // but Inner's 3 padding bytes would be sent by memcpy
```

This is a real trap — but it is fully avoided because `std::is_class_v<Outer>` → `Dynamic`
→ `can_memcpy_whole = false`. SeRTial recurses into `Inner` and strips its padding correctly.

**Variable-length containers** (`fixed_vector<T,N>`, `fixed_string<N>`): `sizeof` is the
max-capacity memory footprint. Serializing with `sizeof` would waste most of the wire capacity
whenever the container is less than full. `can_memcpy_whole = false` forces SeRTial, which
writes only the actual populated elements.

### 10.4 Manual override on `MessageDefinition`

For the rare case where the automatic choice needs to be overridden:

```cpp
enum class WireEncoding { Auto, Native, Packed };

template<typename PayloadT,
         uint8_t Prefix, uint8_t SubPrefix, uint16_t ID,
         typename ReplyT = void,
         WireEncoding Encoding = WireEncoding::Auto>
struct MessageDefinition { ... };
```

Resolution at the `send<T>()` call site:

```cpp
constexpr WireEncoding enc = MessageDefinition<T>::encoding; // from registry lookup

if constexpr (enc == WireEncoding::Native ||
              (enc == WireEncoding::Auto && sertial::can_memcpy_whole_v<T>)) {
    // memcpy path
} else {
    // SeRTial path
}
```

| Value | Behaviour |
|---|---|
| `Auto` (default) | `can_memcpy_whole_v<T>` decides — backward-compatible |
| `Native` | Always `memcpy(sizeof(T))` — user asserts no padding matters or type is not reflectable |
| `Packed` | Always SeRTial — forced portability or explicit wire-size minimization |

### 10.5 Wire size impact

| Type example | `sizeof` | `packed_size` | `can_memcpy_whole` | Auto wire bytes |
|---|---|---|---|---|
| `float` | 4 | 4 | `true` | 4 (memcpy) |
| `std::array<float, 3>` | 12 | 12 | `true` | 12 (memcpy) |
| Any struct / class | varies | varies | **always `false`** | packed_size (SeRTial) |
| `fixed_vector<float, 64>` (3 elements) | 256+ | — | `false` | 12 (SeRTial, actual data) |
| `WireHeader` | 32 | 32 | `false` (struct) | 32 (SeRTial, same result) |

The `WireHeader` line illustrates that for padding-free structs SeRTial and memcpy produce
identical bytes — the only cost is SeRTial's field-iteration overhead, which is negligible
for small headers.

**`WireEncoding::Native` override — hazards**:

The `Native` tag forces `memcpy(sizeof(T))` and bypasses all of the above. This is only
safe when:
1. `T` has no padding at any nesting level (verified manually or via `static_assert(!has_padding_v<T>)`)
2. `T` contains **no** variable-length containers — `sizeof(fixed_vector<float,64>)` is the
   max-capacity footprint, completely wrong as a wire size
3. Sender and receiver share the same ABI (same toolchain, same target arch)

Recommendation: only use `WireEncoding::Native` for leaf arithmetic structs where profiling
shows SeRTial overhead is measurable. For anything containing a container, `Native` is unsafe.

### 10.6 Backend interaction

The EVL SHM backend allocates slots of `slot_size` bytes. `slot_size` must be sized for
the worst case of the encoding in use:

- `Auto` / `Packed` → `slot_size = TypeTraits<T>::packed_size` (or `registry_max_packed_size`)
- `Native` → `slot_size = sizeof(T)` (or `registry_max_sizeof`)

`max_size_for_types<Ts...>` in `wire_message.hpp` should be updated to account for this
(currently uses `sizeof` — a separate `max_packed_size_for_types<Ts...>` variant is needed
for the EVL ring slot sizing).

---

**Awaiting approval before implementation of either phase.**

