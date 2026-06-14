# CoreRaT Design Document

**Version**: 0.1 (draft for agent)
**Purpose**: Design brief for an agent implementing CoreRaT — the shared foundation library that CommRaT (and future projects) build on top of.

---

## 1. What is CoreRaT?

CoreRaT is a standalone C++20 library that extracts and modernises the shared core of CommRaT:

| Subsystem | Description |
|---|---|
| **Platform** | Thread, Mutex, SharedMutex, ConditionVariable, Time, Duration — backends for STD and EVL (Xenomai 4) |
| **IPC** | Type-safe, compile-time, EVL-native replacement for the RACK/TiMS socket API |
| **Messaging** | Compile-time message ID system, MessageDefinition, registry, SeRTial integration |

CommRaT depends on CoreRaT. CoreRaT has no dependency on CommRaT. Both ship separately.

### Why now?

The current CommRaT TiMS wrapper (`tims_wrapper.cpp`) calls `tims_recvmsg_timed()` which is a standard blocking POSIX socket operation. On the EVL guest this is an in-band syscall — any EVL thread calling it is immediately demoted from OOB to in-band, losing all real-time guarantees. This is the root cause of the CPU-time limit issues and the reason RT tests behave poorly: the threads spin in retry after demotion instead of sleeping.

CoreRaT's IPC layer must replace this with an EVL-native receive path (`evl_poll` / `evl_socket` if available, or a purpose-built lock-free ring backed by `evl_mutex` + `evl_event`).

---

## 2. Architecture

```
CoreRaT/
├── include/corerat/
│   ├── platform/
│   │   ├── platform.hpp          # Backend selection (CORERAT_PLATFORM_STD / EVL)
│   │   ├── duration.hpp          # Duration (int64_t ns_, structural type for NTTP)
│   │   ├── threading.hpp         # Thread, Mutex, SharedMutex, CondVar (backend dispatch)
│   │   ├── timestamp.hpp         # Timestamp (uint64_t ns), Time::now/sleep/sleep_until
│   │   ├── std/
│   │   │   ├── threading_impl.hpp
│   │   │   └── timestamp_impl.hpp
│   │   └── evl/
│   │       ├── threading_impl.hpp
│   │       └── timestamp_impl.hpp
│   ├── messaging/
│   │   ├── message_id.hpp        # MessagePrefix, SystemSubPrefix, UserSubPrefix, make_message_id
│   │   ├── message_def.hpp       # MessageDefinition<Payload, Prefix, SubPrefix, ID, Reply>
│   │   ├── message_registry.hpp  # MessageRegistry<MessageDefs...>, compile-time dispatch
│   │   ├── wire_header.hpp       # WireHeader (formerly TimsHeader) — on-wire struct
│   │   └── wire_message.hpp      # WireMessage<T> = {WireHeader header; T payload;}
│   └── ipc/
│       ├── ipc.hpp               # IPC backend selection
│       ├── mailbox_config.hpp    # MailboxConfig, MailboxResult<T>, MailboxError
│       ├── mailbox.hpp           # Mailbox<Registry> — unified send/receive with compile-time dispatch
│       ├── tims/
│       │   ├── tims_backend.hpp  # RACK/TiMS backend (current behavior, std::)
│       │   └── tims_backend.cpp
│       └── evl/
│           └── evl_backend.hpp   # EVL-native IPC backend (new, OOB-safe)
├── src/
│   └── tims_backend.cpp
├── CMakeLists.txt
└── CMakePresets.json
```

---

## 3. Platform Layer (migrated from CommRaT verbatim)

This is a clean copy of the existing CommRaT platform abstraction. No semantic changes — just namespace `corerat::` instead of `commrat::`.

### 3.1 Duration

Structural C++20 type backed by `int64_t ns_`. Public member enables NTTP use:

```cpp
// NTTP in template parameters:
template<Duration Period>
class PeriodicTask { ... };

using MyTask = PeriodicTask<Milliseconds(10)>;
```

API (unchanged from CommRaT):
```cpp
Duration d = Milliseconds(100);
Duration d = Microseconds(500);
Duration d = Seconds(1);
Duration d = 10_ms;   // UDL

d.count_ns();  // int64_t
d.count_ms();  // int64_t
d.is_zero();   // bool
```

### 3.2 Time

```cpp
Timestamp ts = Time::now();           // uint64_t nanoseconds, monotonic
Time::sleep(Milliseconds(10));        // OOB-safe on EVL (evl_usleep)
Time::sleep_until(ts + 10'000'000);   // OOB-safe on EVL (evl_sleep_until)
Time::yield();                        // OOB-safe on EVL (evl_yield)
```

### 3.3 Threading

```cpp
Thread t(ThreadConfig{.name="sensor", .priority=ThreadPriority::REALTIME,
                       .policy=SchedulingPolicy::FIFO}, []{ /* RT loop */ });

Mutex mtx;
Lock lk(mtx);

SharedMutex rwmtx;
SharedLock slk(rwmtx);  // reader
Lock       wlk(rwmtx);  // writer

ConditionVariable cv;
cv.wait(lk, [&]{ return ready; });
cv.notify_one();
```

EVL backend mapping:

| CoreRaT | STD | EVL |
|---|---|---|
| `Thread` | `std::thread` | `pthread` + `evl_attach_self()` + `evl_set_schedattr(SCHED_FIFO)` |
| `Mutex` | `std::mutex` | `evl_mutex` (PI) |
| `SharedMutex` | `std::shared_mutex` | `evl_rwlock` |
| `ConditionVariable` | `std::condition_variable` | `evl_event` paired with `evl_mutex` |
| `Time::now()` | `steady_clock::now()` | `evl_read_clock(EVL_CLOCK_MONOTONIC)` |
| `Time::sleep()` | `this_thread::sleep_for` | `evl_usleep()` |
| `Time::sleep_until()` | `this_thread::sleep_until` | `evl_sleep_until()` |
| `Time::yield()` | `this_thread::yield` | `evl_yield()` |

---

## 4. Messaging Layer (migrated from CommRaT, cleaned up)

### 4.1 Wire Format

```cpp
// On-wire header — matches RACK tims_msg_head layout exactly
struct WireHeader {
    uint32_t msg_type;    // MessageDefinition::full_id()
    uint32_t msg_size;    // total serialized bytes (header + payload)
    uint64_t timestamp;   // nanoseconds since epoch (single source of truth)
    uint32_t seq_number;
    uint32_t dest;        // destination mailbox address
    uint32_t src;         // source mailbox address
    uint32_t flags;
};

template<typename PayloadT>
struct WireMessage {
    WireHeader header;
    PayloadT payload;

    using payload_type = PayloadT;
};
```

SeRTial serialization (unchanged from CommRaT):
```cpp
auto result = sertial::Message<WireMessage<T>>::serialize(msg);  // Result with view()
auto msg    = sertial::Message<WireMessage<T>>::deserialize(span); // DeserializeResult
constexpr size_t buf_sz = sertial::Message<WireMessage<T>>::max_buffer_size;
```

### 4.2 Message ID System

32-bit: `[Prefix:8][SubPrefix:8][LocalID:16]`

```cpp
// Auto-increment within (Prefix, SubPrefix) group starting at 1
// Reply IDs = bitwise NOT of request ID (0x0010 -> 0xFFF0)
// 0 = AUTO_ID marker, 0x8000+ reserved for replies

enum class MessagePrefix : uint8_t { System = 0x00, UserDefined = 0x01 };
enum class SystemSubPrefix : uint8_t { Subscription = 0x00, Control = 0x01 };
enum class UserSubPrefix : uint8_t { Data = 0x00, Commands = 0x01, Events = 0x02,
                                      GetData = 0x03, GetNextData = 0x04, Custom = 0x05 };
```

### 4.3 MessageDefinition

```cpp
template<
    typename PayloadT,
    MessagePrefix Prefix,
    auto SubPrefix,
    uint16_t LocalID = AUTO_ID,
    typename ReplyPayloadT = void   // void = no reply
>
struct MessageDefinition {
    using payload_type = PayloadT;
    static constexpr uint32_t full_id();   // compile-time
    static constexpr bool has_reply;
    static constexpr bool is_request;
    using ReplyMessageDef = /* auto-generated with id = ~LocalID */;
};

// Convenience wrappers (CommRaT compatibility):
namespace Message {
    template<typename T> using Data    = MessageDefinition<T, UserDefined, Data, AUTO_ID>;
    template<typename T> using Command = MessageDefinition<T, UserDefined, Commands, AUTO_ID>;
    template<typename T> using Event   = MessageDefinition<T, UserDefined, Events, AUTO_ID>;
}
```

### 4.4 MessageRegistry

```cpp
// Compile-time registry with runtime-dispatch via visitor
template<typename... MessageDefs>
class MessageRegistry {
public:
    static constexpr size_t size = sizeof...(MessageDefs);
    static constexpr size_t max_message_size = /* max of sertial::Message<WireMessage<T>>::max_buffer_size */;

    template<typename T>
    static constexpr bool is_registered = /* tuple membership check */;

    // Runtime dispatch: parse msg_type, deserialize, invoke visitor
    template<typename Visitor>
    static bool visit(uint32_t msg_type, std::span<const std::byte> data, Visitor&& v);

    // Compile-time: get full_id for payload type T
    template<typename T>
    static constexpr uint32_t get_message_id();
};
```

---

## 5. IPC Layer (CoreRaT's main new contribution)

### 5.1 Problem with Current TiMS Wrapper

`tims_recvmsg_timed()` is a blocking POSIX socket call. When called from an EVL thread:
- The thread is demoted from OOB to in-band (loses RT guarantees)
- On demotion the kernel signals `SIGXCPU` when CPU time limit is reached
- The thread does NOT sleep during the wait — it consumes CPU in the in-band path

The fix: an IPC backend that stays in OOB context for the entire send/receive path.

### 5.2 IPC Backend Interface

The IPC layer is selected the same way as the platform layer:

```cpp
// CORERAT_IPC_TIMS  — use RACK/TiMS sockets (compatible with existing RACK nodes)
// CORERAT_IPC_EVL   — use EVL-native ring buffers (requires EVL kernel, no RACK)
// Default: TIMS when CORERAT_PLATFORM_STD, EVL when CORERAT_PLATFORM_EVL
```

Both backends implement the same `IpcMailbox` concept:

```cpp
concept IpcMailbox = requires(T mbx, uint32_t id, size_t slots, size_t max_sz) {
    { mbx.create(id, slots, max_sz) } -> std::same_as<bool>;
    { mbx.destroy() } -> std::same_as<void>;
    { mbx.send(dest_id, header, payload_span) } -> std::same_as<bool>;
    // Blocking receive — MUST be OOB-safe on EVL backend
    { mbx.receive(buffer_span, timeout_ns, out_metadata) } -> std::same_as<ssize_t>;
    { mbx.peek() } -> std::same_as<bool>;
};
```

### 5.3 MailboxConfig

`MailboxConfig` is defined in `include/corerat/ipc/mailbox_config.hpp`. It is the only configuration type passed to `Mailbox<>`. The `Mailbox<>` constructor maps its fields to the correct backend mode.

```cpp
struct MailboxConfig {
    uint32_t mailbox_id;
    size_t   message_slots    = 10;
    size_t   max_message_size = 4096;
    uint8_t  send_priority    = 10;
    bool     realtime         = false;

    // EVL only: use Mode::Public (cross-process POSIX SHM ring).
    // TIMS: ignored — the TCP router handles routing regardless.
    bool     cross_process    = false;

    // EVL only: use Mode::Network (OOB UDP/IPv4, cross-machine RT).
    // TIMS: ignored.
    bool     network          = false;

    // Must match top byte of mailbox_id (system_id field).
    uint8_t  local_system_id  = 0;

    // Route table: one entry per remote host (keyed on system_id).
    struct NetworkRoute {
        uint8_t system_id{0};
        char    ip[16]{};   // IPv4 dotted-decimal
    };
    static constexpr uint8_t kMaxNetworkRoutes = 8;
    std::array<NetworkRoute, kMaxNetworkRoutes> network_routes{};
    uint8_t  network_route_count{0};
};
```

Mode selection priority (EVL platform, in `Mailbox<>` constructor):
```
network = true        → Mode::Network  (OOB UDP, cross-machine)
cross_process = true  → Mode::Public   (SHM ring, cross-process)
default               → Mode::Local    (heap ring, same-process)
```

On the STD/TIMS platform, `cross_process`, `network`, and all network fields are ignored.

### 5.4 TiMS Backend (compatibility, STD platform)

Thin wrapper over POSIX TCP sockets implementing the RACK `TimsRouterTcp` wire protocol directly — **no RACK library dependency**. Used when `CORERAT_PLATFORM_STD`.

```cpp
// include/corerat/ipc/tims/tims_backend.hpp
class TimsMailbox {
public:
    explicit TimsMailbox(const TimsConfig& config);
    TimsResult initialize();
    void       shutdown();

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id);

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout);
    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout, Metadata*);
};
```

Connects to `corerat-router-tcp` (or the RACK `TimsRouterTcp` daemon) on `localhost:2000`. Wire protocol identical to RACK — existing RACK and CommRaT nodes connect without modification.

### 5.5 Mailbox addressing

Mailbox IDs follow the RACK/TiMS convention. The 32-bit address is split into four 8-bit fields:

```
 31      24  23      16  15       8  7        0
┌──────────┬──────────┬──────────┬──────────┐
│ system   │  class   │ instance │  local   │
│   id     │   id     │   id     │   id     │
└──────────┴──────────┴──────────┴──────────┘
```

- **system_id** (top byte) — identifies the host machine. Used as the routing key in `Mode::Network` to map mailbox destinations to IP addresses. All mailboxes on one machine share the same `system_id`.
- **class_id** — the component class (e.g. `0x01` = Ladar, `0x02` = Chassis).
- **instance_id** — disambiguates multiple components of the same class on the same machine.
- **local_id** — 0 = command mailbox, others = data/reply mailboxes within a component.

Helper:
```cpp
static constexpr uint8_t system_id_of(uint32_t mailbox_id) {
    return (mailbox_id >> 24) & 0xFF;
}
```

### 5.5 EVL Backend (OOB-safe, EVL platform)

EVL does not provide a kernel IPC primitive equivalent to TiMS. The EVL-native backend (`EvlMailbox`) has three operating modes selected at `initialize()` time via `MailboxConfig`:

#### Mode::Local (default)
- Ring buffer in heap memory, `evl_mutex` + `evl_event` created with `EVL_CLONE_PRIVATE`
- Routing via a process-local pointer registry (`evl_detail::registry_slot[]`)
- Use case: single process, multiple threads (CommRaT modules in one binary)

#### Mode::Public (cross_process = true)
- Ring buffer in a POSIX SHM region (`/corerat_mbx_<id>`)
- `evl_mutex` + `evl_event` created with `EVL_CLONE_PUBLIC` — any process can open them by name
- **SHM layout:** `[ShmLayout 16B][SlotMeta × capacity][payload × capacity × slot_size]`
  - `SlotMeta` is inside the SHM so senders in other processes post directly (no IPC copy)
- Names are deterministic from `mailbox_id` — no name-service lookup on the data path:
  - SHM: `/corerat_mbx_<id>`
  - mutex: `corerat-ring-mtx-<id>`
  - event: `corerat-ring-evt-<id>`
- Sender opens remote SHM on first send (cached in `RemoteHandle`, up to 16 entries)
- Use case: multiple processes on the same machine

Hot-path for both Local and Public:
```
send():
  evl_lock(ring.mutex)         // OOB-safe
  find free slot / evict lowest-priority
  memcpy(slot, msg)            // OOB-safe
  mark slot in_use
  evl_unlock(ring.mutex)       // OOB-safe
  evl_signal(ring.event)       // OOB-safe wake

receive():
  evl_lock(ring.mutex)
  evl_timedwait(ring.event)    // OOB-safe block, no CPU burn
  find highest-priority slot
  memcpy(out, slot)            // OOB-safe
  mark slot free
  evl_unlock(ring.mutex)       // OOB-safe
```

#### Mode::Network (network = true)
See §5.6.

#### Class summary

```cpp
// include/corerat/ipc/evl/evl_backend.hpp
class EvlMailbox {
public:
    enum class Mode { Local, Public, Network };

    explicit EvlMailbox(const TimsConfig& config,
                        Mode mode = Mode::Local,
                        const EvlNetworkConfig& net_config = {});

    TimsResult initialize();
    void       shutdown();

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id);

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout);
    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout, Metadata*);
};
```

### 5.6 Network Mode (OOB UDP, cross-machine)

`Mode::Network` uses the EVL out-of-band UDP/IPv4 stack for cross-machine real-time
communication — the CoreRaT equivalent of what RACK used RTnet for on Xenomai 3.
No special NIC driver required; EVL offloads TX to in-band if the driver is not
OOB-capable, but the calling thread **never demotes** from the OOB stage.

#### EvlNetworkConfig

```cpp
struct EvlNetworkConfig {
    static constexpr uint16_t kOobBasePort = 42000;
    static constexpr uint8_t  kMaxRoutes   = 8;

    struct Route {
        uint8_t system_id{0};
        char    ip[16]{};        // IPv4 dotted-decimal, e.g. "10.10.10.11"
    };

    uint8_t                       local_system_id{0};
    std::array<Route, kMaxRoutes> routes{};
    uint8_t                       route_count{0};

    // Port = kOobBasePort + (mailbox_id & 0x7FFF)  → range 42000–74767
    static constexpr uint16_t port_for(uint32_t mailbox_id) noexcept;
    // system_id = (mailbox_id >> 24) & 0xFF
    static constexpr uint8_t  system_id_of(uint32_t mailbox_id) noexcept;
};
```

Route table is **one entry per remote host** (keyed on `system_id`), not per mailbox.
Sending to any `0x01XXXXXX` mailbox automatically routes to the host with `system_id = 1`.

#### MailboxConfig for Network mode

```cpp
MailboxConfig cfg;
cfg.mailbox_id        = 0x00012000;   // system_id=0 (this host)
cfg.local_system_id   = 0;
cfg.network           = true;
cfg.network_route_count = 1;
cfg.network_routes[0] = { .system_id = 1, .ip = "10.10.10.11" };
```

#### Hot path

```
initialize():
  socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0)
  bind(INADDR_ANY : port_for(my_mailbox_id))
  for each route: evl_net_solicit(peer_ip, EVL_NEIGH_PERMANENT)  // prime ARP/route cache once

send(dest):
  dest_sys = system_id_of(dest)
  look up Route by dest_sys → get IP
  dst_port = port_for(dest)
  oob_sendmsg(sock, {ip, dst_port, wire_data})   // stays OOB

receive():
  oob_recvmsg(sock, buffer, timeout)             // stays OOB
```

#### Required host setup (once per machine)

```bash
evl net -ei eth0               # dedicated RT NIC
# or, to share a NIC:
ip link add link eth0 name eth0.42 type vlan id 42
evl net -ei eth0.42
```

### 5.7 Mailbox<> — Type-safe front-end (unchanged from CommRaT)

```cpp
template<typename... MessageDefs>
class Mailbox {
    using Registry = MessageRegistry<MessageDefs...>;
    using Backend  = /* TimsMailbox or EvlMailbox selected at compile time */;

    Backend backend_;
    bool running_ = false;

public:
    explicit Mailbox(const MailboxConfig& cfg);

    MailboxResult<void> start();
    void stop();

    // Type-safe send — payload must be registered
    template<typename T> requires Registry::template is_registered<T>
    MailboxResult<void> send(TimsMessage<T>& msg, uint32_t dest);

    // Blocking receive with visitor dispatch
    template<typename Visitor>
    MailboxResult<void> receive_any(Visitor&& v);           // 1s internal timeout

    template<typename Visitor>
    MailboxResult<void> receive_any_for(Duration timeout, Visitor&& v);
};
```

---

## 6. Compile-Time API Design (with SeRTial + reflectcpp)

### 6.1 SeRTial Integration

SeRTial provides zero-allocation, compile-time-sized serialization. CoreRaT uses it exactly as CommRaT does:

```cpp
// Fixed buffer size computed at compile time:
constexpr size_t buf = sertial::Message<WireMessage<MySensorData>>::max_buffer_size;
std::array<std::byte, buf> buffer;

// Serialize:
auto result = sertial::Message<WireMessage<MySensorData>>::serialize(msg);
std::span<const std::byte> wire = result.view();

// Deserialize (zero-copy when possible):
auto r = sertial::Message<WireMessage<MySensorData>>::deserialize(buffer_span);
WireMessage<MySensorData>& msg = r.value();
```

User payload structs must be SeRTial-compatible: plain aggregates (no virtual, no heap members, bounded arrays only). `sertial::fixed_vector<T,N>` and `sertial::fixed_string<N>` are the approved collection types.

### 6.2 reflectcpp Integration

reflectcpp (`rfl::`) provides struct reflection. CoreRaT uses it for:
- Named field access in `Mailbox::receive_any` visitors
- `MessageRegistry::visit` dispatch without manual type-switch
- Debug/introspection serialisation (JSON via `rfl::json::write`)

Key pattern in CommRaT (carried forward):
```cpp
// Iterate over registered types at compile time via tuple:
template<typename Visitor, size_t... I>
static bool visit_impl(uint32_t id, std::span<const std::byte> data,
                        Visitor&& v, std::index_sequence<I...>) {
    return (try_visit<std::tuple_element_t<I, MessageTuple>>(id, data, v) || ...);
}
```

### 6.3 Compile-Time Guarantees

All of these are checked at compile time (static_assert or requires-clause):

| Property | How enforced |
|---|---|
| Payload fits in mailbox buffer | `static_assert(sizeof(WireMessage<T>) <= max_msg_size)` |
| Type registered before send | `requires Registry::template is_registered<T>` |
| Message ID uniqueness | auto-increment with `static_assert(id <= MAX_MESSAGE_ID)` |
| No heap in payload | SeRTial concept `sertial::serializable<T>` |
| Reply ID = ~request ID | `static_assert(ReplyDef::local_id == ~RequestDef::local_id)` |

---

## 7. CMake Structure

```cmake
# CMakePresets.json — same pattern as CommRaT
# Presets: default (STD), evl (in-container check), evl-cross (SDK cross-compile)

cmake_minimum_required(VERSION 3.25)
project(CoreRaT VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform selection (same as CommRaT)
set(CORERAT_PLATFORM "STD" CACHE STRING "Platform backend (STD or EVL)")
set_property(CACHE CORERAT_PLATFORM PROPERTY STRINGS STD EVL)

# IPC backend selection
set(CORERAT_IPC "AUTO" CACHE STRING "IPC backend (TIMS, EVL, AUTO)")
if(CORERAT_IPC STREQUAL "AUTO")
    if(CORERAT_PLATFORM STREQUAL "EVL")
        set(CORERAT_IPC "EVL")
    else()
        set(CORERAT_IPC "TIMS")
    endif()
endif()

find_package(SeRTial REQUIRED)
find_package(reflectcpp REQUIRED)

if(CORERAT_PLATFORM STREQUAL "EVL")
    find_library(EVL_LIBRARY NAMES evl REQUIRED)
    find_path(EVL_INCLUDE_DIR NAMES evl/thread.h REQUIRED)
endif()

if(CORERAT_IPC STREQUAL "TIMS")
    find_package(RACK REQUIRED)
endif()

add_library(corerat INTERFACE)
target_include_directories(corerat INTERFACE include/)
target_link_libraries(corerat INTERFACE SeRTial::sertial reflectcpp::reflectcpp)
target_compile_definitions(corerat INTERFACE
    CORERAT_PLATFORM_${CORERAT_PLATFORM}
    CORERAT_IPC_${CORERAT_IPC})

if(CORERAT_PLATFORM STREQUAL "EVL")
    target_link_libraries(corerat INTERFACE ${EVL_LIBRARY})
    target_include_directories(corerat INTERFACE ${EVL_INCLUDE_DIR})
endif()

if(CORERAT_IPC STREQUAL "TIMS")
    # tims_backend.cpp is the only compiled TU
    add_library(corerat_tims STATIC src/tims_backend.cpp)
    target_link_libraries(corerat_tims PUBLIC corerat RACK::rack)
    # Users link corerat_tims instead of corerat when using TIMS IPC
endif()
```

CommRaT's `CMakeLists.txt` then becomes:
```cmake
find_package(CoreRaT REQUIRED)
target_link_libraries(commrat PUBLIC CoreRaT::corerat)
# Remove: find_package(RACK), find_library(EVL_LIBRARY), platform source duplication
```

---

## 8. Migration Path (CommRaT -> CoreRaT)

> **Status as of this document:** Phases 1–3 are **complete** in CoreRaT. CoreRaT is
> a fully working library with tests passing. The CommRaT agent's job is to update
> CommRaT to depend on CoreRaT and remove the migrated code.

### Phase 1 ✅ Extract platform + messaging (complete in CoreRaT)
All of CommRaT's `platform/` and `messaging/` layers are in CoreRaT under `corerat::`.
No behavior change — same types, same API, renamed namespace.

### Phase 2 ✅ IPC TiMS backend (complete in CoreRaT)
`TimsMailbox` in CoreRaT replaces CommRaT's `TimsWrapper` with a **self-contained TCP socket
implementation** — no RACK library (`libtims`) dependency. Wire protocol is identical to RACK's
`TimsRouterTcp`, so existing RACK nodes and `corerat-router-tcp` are interchangeable.

### Phase 3 ✅ EVL IPC backend (complete in CoreRaT)
`EvlMailbox` implements three modes:
- `Mode::Local` — heap ring + EVL mutex/event (single process)
- `Mode::Public` — POSIX SHM ring + public EVL mutex/event (cross-process, same machine)
- `Mode::Network` — OOB UDP/IPv4 with system_id-based routing (cross-machine RT)

The three CommRaT tests that were failing due to `tims_recvmsg_timed()` demotion
(`test_3input_fusion`, `test_address_collisions`, `test_timestamp_logic`) should pass
once CommRaT uses `EvlMailbox` instead of `TimsWrapper` on the EVL platform.

### Phase 4 — CommRaT agent: wire CommRaT to CoreRaT

This is the remaining work. For the CommRaT agent:

1. **Add `find_package(CoreRaT REQUIRED)` to CommRaT's `CMakeLists.txt`**
   Remove: `find_package(RACK)`, `find_library(EVL_LIBRARY)`, `find_path(EVL_INCLUDE_DIR)`,
   `COMMRAT_PLATFORM_*` cmake logic, duplicated platform compile-definitions.

2. **Replace CommRaT's `Mailbox<>` with CoreRaT's**
   `#include "commrat/mailbox/mailbox.hpp"` → `#include "corerat/ipc/mailbox.hpp"` (alias
   `using commrat::Mailbox = corerat::Mailbox` in a shim header for backwards compat).

3. **Replace `commrat::TimsConfig` / `TimsWrapper` with `corerat::TimsConfig` / `TimsMailbox`**
   The `MailboxConfig` → `TimsConfig` conversion already exists in `Mailbox<>::create_backend_config()`.

4. **Update includes for messaging and platform types**
   `commrat::MessageDefinition`, `WireMessage`, `Duration`, `Time`, etc. all exist in
   `corerat::` — add aliases or update includes project-wide.

5. **Confirm CommRaT tests pass** on STD (TiMS) platform first, then EVL platform.

6. **Delete migrated files** from CommRaT per §11.

---

## 9. Test Strategy

### Tests that live in CoreRaT (`test/`)

These test CoreRaT primitives directly — no CommRaT application layer required:

| CoreRaT test | Adapted from CommRaT | What it covers |
|---|---|---|
| `test_message_id.cpp` | `test_auto_id_validation.cpp` | `MessageDefinition`, auto-increment IDs, reply derivation (`ID = -request_id`), `MessageRegistry` registration |
| `test_registry_utils.cpp` | `test_registry_utils.cpp` | `registry_utils.hpp` filters (`data_messages_t`, `command_messages_t`, `filter_requests_t`, etc.), `RegistryStats`, `find_message_def_t`, `is_request_payload_v` |
| `test_wire_message_sizing.cpp` | `test_mailbox_sizing.cpp` | `WireMessage<T>` serialised sizes, `MessageRegistry::max_message_size`, `max_size_for_types<>` subset optimisation |

**Key difference from CommRaT tests:** CoreRaT tests use `MessageRegistry<>` directly. There is no `CommRaT<>` application wrapper, no `GetData`/`GetNextData` expansion, and no subscription message inflation — those are CommRaT-layer additions.

### Tests that stay in CommRaT (`test/`)

These require the CommRaT application layer (Module, CommRaT<>, subscription protocol, etc.):

| CommRaT test | Reason |
|---|---|
| `test_auto_id_validation` | Superseded by CoreRaT `test_message_id` |
| `test_mailbox_sizing` | Superseded by CoreRaT `test_wire_message_sizing`; CommRaT version also validates GetData overhead |
| `test_registry_utils` | Superseded by CoreRaT version; CommRaT version validates subscription/GetData counts |
| `test_3input_fusion` | Requires Module, CommRaT<>, multi-input machinery |
| `test_address_collisions` | Requires Module lifecycle |
| `test_timestamp_logic` | Requires Module timestamp propagation |
| `test_subscription_protocol` | CommRaT subscription layer |
| `test_getdata_registration` | CommRaT GetData message expansion |
| `test_system_messages` | CommRaT system/subscription message definitions |
| `test_typed_mailbox` | CommRaT TypedMailbox |
| `test_workmailbox` | CommRaT WorkMailbox |
| `test_timestamped_ring_buffer` | CommRaT `TimestampedRingBuffer` (output buffer) |
| `test_historical_mailbox` | CommRaT historical output infrastructure |
| `test_multi_input*` / `test_multi_output` | CommRaT multi-input/output module system |
| `test_process_signature` | CommRaT process() signature checking |
| `test_synced_wrapper` | CommRaT `Synced<>` wrapper |
| `test_module_output_getdata` | CommRaT module output + GetData |
| `test_config_generator` | CommRaT config generation from IOSpec |

---

## 10. Current CommRaT Files to Migrate

| CommRaT file | CoreRaT destination | Notes |
|---|---|---|
| `include/commrat/platform/platform.hpp` | `include/corerat/platform/platform.hpp` | rename macros |
| `include/commrat/platform/duration.hpp` | `include/corerat/platform/duration.hpp` | verbatim |
| `include/commrat/platform/threading.hpp` | `include/corerat/platform/threading.hpp` | verbatim |
| `include/commrat/platform/timestamp.hpp` | `include/corerat/platform/timestamp.hpp` | verbatim |
| `include/commrat/platform/std/*.hpp` | `include/corerat/platform/std/*.hpp` | verbatim |
| `include/commrat/platform/evl/*.hpp` | `include/corerat/platform/evl/*.hpp` | verbatim |
| `include/commrat/messages.hpp` | `include/corerat/messaging/wire_message.hpp` | rename structs |
| `include/commrat/messaging/message_id.hpp` | `include/corerat/messaging/message_id.hpp` | verbatim |
| `include/commrat/messaging/message_def.hpp` | `include/corerat/messaging/message_def.hpp` | verbatim |
| `include/commrat/messaging/message_registry.hpp` | `include/corerat/messaging/message_registry.hpp` | verbatim |
| `include/commrat/platform/tims_wrapper.hpp` | `include/corerat/ipc/tims/tims_backend.hpp` | refactor to concept |
| `src/tims_wrapper.cpp` | `src/tims_backend.cpp` | refactor |
| `include/commrat/mailbox/mailbox.hpp` | `include/corerat/ipc/mailbox.hpp` | backend-templated |
| `include/commrat/mailbox/typed_mailbox.hpp` | `include/corerat/ipc/typed_mailbox.hpp` | verbatim |

CommRaT keeps: `module2.hpp`, `module/`, `CommRaT<>` template, examples, tests.

---

## 11. CommRaT Cleanup After Migration

Once each CoreRaT phase is complete and CommRaT's tests pass against the CoreRaT dependency, the following files and CMake logic are deleted from CommRaT:

### Phase 1 complete — delete from CommRaT:
```
include/commrat/platform/                    # entire directory (all of platform/ moves to CoreRaT)
include/commrat/messages.hpp                 # replaced by corerat/messaging/wire_message.hpp
include/commrat/messaging/message_id.hpp     # moved to corerat/messaging/message_id.hpp
include/commrat/messaging/message_registry.hpp  # moved to corerat/messaging/message_registry.hpp
include/commrat/messaging/registry_utils.hpp    # moved to corerat/messaging/registry_utils.hpp
```

NOTE: `include/commrat/messaging/system/` (subscription_messages, data_request_messages, system_registry)
stays in CommRaT — it implements the CommRaT subscription protocol, which is not part of CoreRaT.

NOTE: `include/commrat/messaging/message_helpers.hpp` and `include/commrat/messaging/data_with_commands.hpp`
stay in CommRaT — they provide CommRaT-specific helpers (GetData protocol, DataWithCommands).

### Phase 2 complete — delete from CommRaT:
```
include/commrat/platform/tims_wrapper.hpp    # replaced by corerat/ipc/tims/tims_backend.hpp
src/tims_wrapper.cpp                         # replaced by CoreRaT src/tims_backend.cpp
include/commrat/mailbox/mailbox.hpp          # replaced by corerat/ipc/mailbox.hpp
include/commrat/mailbox/typed_mailbox.hpp    # replaced by corerat/ipc/typed_mailbox.hpp (Phase 2 TODO)
include/commrat/mailbox/mailbox_type.hpp     # MailboxType enum — review if still needed after migration
```

NOTE: `include/commrat/mailbox/timestamped_ring_buffer.hpp` stays in CommRaT (output buffer infrastructure).

### CMake lines to remove from CommRaT's CMakeLists.txt:
```cmake
# Remove:
set(COMMRAT_PLATFORM ...)
find_library(EVL_LIBRARY ...)
find_path(EVL_INCLUDE_DIR ...)
add_library(EVL::evl ...)
target_link_libraries(commrat PUBLIC EVL::evl)
find_package(RACK REQUIRED)
target_link_libraries(commrat PUBLIC RACK::rack)
target_compile_definitions(commrat PUBLIC COMMRAT_PLATFORM_${COMMRAT_PLATFORM})

# Replace with:
find_package(CoreRaT REQUIRED)
target_link_libraries(commrat PUBLIC CoreRaT::corerat)
```

### What remains in CommRaT after migration:
```
include/commrat/
  module2.hpp
  module/
    io/           (io_spec, synced, output/input infrastructure)
    services/     (io_handler, command_handler)
    traits/       (type_extraction, processor_bases)
    helpers/      (address_helpers, command_extraction)
  commrat.hpp     (CommRaT<> application template)
src/              (empty or removed)
examples/
test/
CMakeLists.txt    (thin — just find CoreRaT, build modules/examples/tests)
```

CommRaT's include surface visible to users shrinks to essentially: `commrat/commrat.hpp`, `commrat/module2.hpp`, and the IO spec types.

---

## 12. Key Design Constraints

All constraints from CommRaT's real-time philosophy apply:

- No heap allocation in hot paths (`receive_any`, `send`, RT loops)
- No `std::vector`, `std::string`, `throw` in OOB context
- Bounded everything: `sertial::fixed_vector<T,N>`, `sertial::fixed_string<N>`, `std::array`
- Compile-time message sizes: `sertial::Message<WireMessage<T>>::max_buffer_size`
- OOB-safe on EVL: only `libevl` functions + `memcpy` + `std::atomic` in hot path
- Timestamps via `WireHeader.timestamp` only — no payload timestamp fields

---

## 13. Key External APIs (for implementer reference)

### SeRTial
```cpp
sertial::Message<T>::serialize(T&)            -> Result (has .view() -> span<byte>)
sertial::Message<T>::deserialize(span<byte>)  -> DeserializeResult<T> (has .value())
sertial::Message<T>::max_buffer_size          // constexpr size_t
sertial::Message<T>::packed_size              // constexpr size_t
sertial::fixed_vector<T, N>                   // stack-allocated bounded vector
sertial::fixed_string<N>                      // stack-allocated bounded string
```

### EVL threading primitives (for EvlMailbox implementation)
```cpp
evl_mutex_init(&m, nullptr, 0, "name")   // PI mutex
evl_lock_mutex(&m)                        // OOB-safe lock
evl_unlock_mutex(&m)                      // OOB-safe unlock
evl_event_init(&e, &m, 0, "name")        // condition event
evl_wait_event(&e)                        // OOB-safe blocking wait
evl_signal_event(&e)                      // OOB-safe signal
evl_broadcast_event(&e)                   // OOB-safe broadcast
evl_timedwait_event(&e, clock, ts)        // OOB-safe timed wait
```

### EVL thread attach (for Thread implementation, already in CommRaT)
```cpp
evl_attach_self("thread-name:%d", id)    // attach calling thread to EVL core
evl_set_schedattr(efd, &attr)             // set SCHED_FIFO priority
evl_set_thread_mode(efd, EVL_T_WOSS, NULL) // warn on OOB->inband switch
evl_detach_self()                         // detach before thread exit
```

Full EVL API reference: `docs/work/EVL_API_REFERENCE.md` (in CommRaT repo).

---

## 14. Agent Strategy

### Two separate agents, not one supervisor

CoreRaT and CommRaT each get their own dedicated Copilot agent (their own repo and `copilot-instructions.md`). A shared supervisor agent is not used.

Reasons:
- CoreRaT is a standalone library with its own repo, CI, CMakePresets, and versioning. It has no knowledge of CommRaT's module system.
- CommRaT's instructions will reference CoreRaT as an external dependency (like RACK or SeRTial) — not as internal code to understand.
- A supervisor with full context of both projects simultaneously would have an enormous, unfocused instructions file.
- The interface between them is narrow and stable: `find_package(CoreRaT)` + the `IpcMailbox` concept + the `WireMessage<T>` wire format. That boundary is the contract.

### What each agent knows

**CoreRaT agent** (`CoreRaT/copilot-instructions.md`):
- This document (CORERAT_DESIGN.md) is its primary design brief
- Knows: platform layer, IPC backends (TiMS + EVL), messaging system, SeRTial, reflectcpp, EVL API
- Does not know: CommRaT module system, `Module2`, `IOSpec`, subscription protocol

**CommRaT agent** (`CommRaT/.github/copilot-instructions.md`):
- Knows CoreRaT as a black-box dependency (same as RACK/SeRTial)
- Knows which CommRaT files to delete per migration phase (Section 10 of this doc)
- Knows the replacement `find_package(CoreRaT)` CMake line
- Does not re-implement anything that moved to CoreRaT

### Shared reference during transition

This document (`CORERAT_DESIGN.md`) is the shared contract. Both agents should be pointed at it:
- CoreRaT agent: implements Section 3–9
- CommRaT agent: executes Section 10 (cleanup) after each CoreRaT phase ships
