# GitHub Copilot Instructions for CoreRaT

## Project Overview

**CoreRaT** (Core Runtime) is a standalone C++20 library — the shared foundation for CommRaT and future real-time systems. It provides three subsystems:

| Subsystem | Scope |
|---|---|
| **Platform** | `Thread`, `Mutex`, `SharedMutex`, `ConditionVariable`, `Time`, `Duration` — STD and EVL backends |
| **Messaging** | `WireHeader`, `WireMessage<T>`, `MessageDefinition`, `MessageRegistry` — compile-time message ID system |
| **IPC** | `Mailbox<>` frontend + `TimsMailbox` (TCP/TiMS backend) + `EvlMailbox` (EVL ring-buffer backend, Phase 3) |

**CoreRaT does NOT depend on CommRaT.** CommRaT depends on CoreRaT.

**Current Status**: All three phases complete. Phase 3 (EVL IPC backend) shipped.

## Architecture

```
include/corerat/
  platform/
    platform.hpp           # CORERAT_PLATFORM_STD / CORERAT_PLATFORM_EVL selection
    duration.hpp           # Duration (int64_t ns_, structural NTTP type)
    threading.hpp          # Thread, Mutex, SharedMutex, ConditionVariable
    timestamp.hpp          # Timestamp (uint64_t ns), Time::now/sleep/sleep_until
    std/
      threading_impl.hpp   # std:: backend
      timestamp_impl.hpp
    evl/
      threading_impl.hpp   # EVL backend (pthread + evl_attach_self, evl_mutex, evl_event)
      timestamp_impl.hpp   # evl_read_clock, evl_usleep, evl_sleep_until
  messaging/
    message_id.hpp         # MessagePrefix, UserSubPrefix/SystemSubPrefix, make_message_id,
                           # MessageDefinition<Payload, Prefix, SubPrefix, ID, Reply>
    message_registry.hpp   # MessageRegistry<...> — compile-time dispatch
    registry_utils.hpp     # Compile-time registry filters and lookup helpers
    wire_message.hpp       # WireHeader + WireMessage<T> = {WireHeader header; T payload;}
  ipc/
    mailbox_config.hpp     # MailboxConfig, MailboxResult<T>, MailboxError
    mailbox.hpp            # Mailbox<MessageDefs...> — type-safe frontend;
                           # selects IpcMailbox backend via CORERAT_IPC_EVL macro
    tims/
      tcp_socket.hpp       # TcpSocket — POSIX TCP socket wrapper
      protocol.hpp         # TiMS router wire protocol helpers
      tims_backend.hpp     # TimsMailbox — RACK-compatible TCP backend
    evl/
      evl_backend.hpp      # EvlMailbox — EVL ring-buffer + OOB UDP backend (header-only)
src/
  tims_backend.cpp         # TimsMailbox implementation
  router/
    tcp_router.cpp         # corerat-router-tcp binary
    evl_router.cpp         # corerat-router-evl binary
cmake/
  CoreRaTConfig.cmake.in
  isar-sdk-toolchain.cmake
CMakeLists.txt
CMakePresets.json
.corerat.env               # Non-secret config defaults (RATOS_RELEASE_TAG, QEMU settings)
scripts/
  evl-dev.sh               # EVL dev workflow: boot QEMU, build, test, run, shell
test/
  CMakeLists.txt
  test_message_id.cpp
  test_registry_utils.cpp
  test_router_tcp.cpp
  test_wire_message_sizing.cpp
  ping_pong/               # End-to-end EVL IPC test (ping_node, pong_node, evl_pingpong)
```

**External dependencies** (installed system-wide):
- **SeRTial** — zero-allocation serialization (`find_package(SeRTial REQUIRED)`)
- **reflectcpp** — reflection, found transitively via SeRTial
- **libevl** — Xenomai 4 RT primitives (only when `CORERAT_PLATFORM=EVL`)

**Namespace**: All CoreRaT symbols live in `corerat::`.
**Macro prefix**: `CORERAT_PLATFORM_*`, `CORERAT_IPC_*`, `CORERAT_HAS_*`.

## Platform Backends

Selected at compile time via `CORERAT_PLATFORM` CMake variable:

| Value | Backend |
|---|---|
| `STD` (default) | `std::thread`, `std::mutex`, `std::chrono::steady_clock` |
| `EVL` | `pthread` + `evl_attach_self()`, `evl_mutex`, `evl_event`, `evl_read_clock` |

**ALWAYS use CoreRaT abstractions** instead of std:: types:

```cpp
// NEVER:
#include <thread>
#include <mutex>
#include <chrono>
std::thread, std::mutex, std::chrono::steady_clock::now()

// ALWAYS:
#include <corerat/platform/threading.hpp>
#include <corerat/platform/timestamp.hpp>
corerat::Thread, corerat::Mutex, corerat::SharedMutex
corerat::Lock, corerat::SharedLock
corerat::Timestamp ts = corerat::Time::now();
corerat::Duration timeout = corerat::Milliseconds(100);
corerat::Time::sleep(corerat::Milliseconds(10));
```

### EVL Platform — OOB Context Rules

EVL threads attach to the EVL core via `evl_attach_self()`. Once attached they can run **out-of-band (OOB)** with ultra-low latency.

**FORBIDDEN in OOB context:**
- `malloc`, `new`, `delete`, `free` — any heap allocation
- `std::cout`, `std::cerr`, `printf` — glibc I/O (goes in-band)
- `throw` — exception mechanism is in-band
- `pthread_mutex_lock` — POSIX mutexes are in-band (use `evl_mutex`)
- Any syscall not listed as OOB-safe in libevl docs

**OOB-safe operations:**
- `evl_lock_mutex` / `evl_unlock_mutex`
- `evl_wait_event` / `evl_signal_event`
- `evl_read_clock` / `evl_usleep` / `evl_sleep_until`
- `memcpy`, `strcpy`, `memmove`
- `std::atomic` load/store
- `sertial::Message<T>::serialize/deserialize` (if T is trivially copyable)

### Platform Backend — EVL to CoreRaT Mapping

| CoreRaT | STD | EVL |
|---|---|---|
| `Thread` | `std::thread` | `pthread_create` + `evl_attach_self()` |
| `Mutex` | `std::mutex` | `evl_mutex` (PI by default) |
| `SharedMutex` | `std::shared_mutex` | `evl_rwlock` |
| `ConditionVariable` | `std::condition_variable` | `evl_event` (paired with `evl_mutex`) |
| `Time::now()` | `steady_clock::now()` | `evl_read_clock(EVL_CLOCK_MONOTONIC)` |
| `Time::sleep()` | `this_thread::sleep_for` | `evl_usleep()` |
| `Time::sleep_until()` | `this_thread::sleep_until` | `evl_sleep_until()` |
| `Time::yield()` | `this_thread::yield` | `evl_yield()` |

## IPC Backends

Selected via `CORERAT_IPC` (AUTO = follows platform):

| Value | Backend | When |
|---|---|---|
| `TIMS` (default on STD) | RACK/TiMS TCP sockets | STD platform, RACK-compatible |
| `EVL` (default on EVL) | EVL ring buffer | EVL platform, OOB-safe (Phase 3) |

### TiMS Backend (Phase 2, complete)

Thin wrapper over POSIX TCP sockets. Connects to `corerat-router-tcp` on `localhost:2000`. Wire protocol identical to RACK — existing RACK nodes and CommRaT nodes connect without modification.

**Problem**: `receive_raw_bytes()` uses POSIX `poll()`/`recv()` — in-band syscalls that demote EVL threads from OOB to in-band, losing all real-time guarantees. This is why the EVL IPC backend (Phase 3) is needed.

### EVL IPC Backend (Phase 3, complete)

**Goal**: A receive path that stays entirely in OOB context.

**Design**: Per-mailbox ring buffer (heap for Local, POSIX SHM for Public) protected by `evl_mutex` + `evl_event`. Network mode uses OOB UDP sockets instead of a ring buffer. The entire backend is **header-only** — no compiled TU.

```cpp
// include/corerat/ipc/evl/evl_backend.hpp
class EvlMailbox {
public:
    enum class Mode {
        Local,    // Same-process heap ring (fastest, no SHM)
        Public,   // Cross-process POSIX SHM ring (evl_mutex/event with EVL_CLONE_PUBLIC)
        Network   // OOB UDP/IPv4, cross-machine RT (oob_sendmsg / oob_recvmsg)
    };

    explicit EvlMailbox(const TimsConfig& config,
                        Mode mode = Mode::Local,
                        const EvlNetworkConfig& net_config = {});

    TimsResult initialize();
    void       shutdown();

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id);

    ssize_t receive_raw_bytes(std::span<std::byte> buffer,
                              Duration timeout, Metadata* metadata = nullptr);
};

// Network routing — system_id-based, matching RACK/TiMS convention
struct EvlNetworkConfig {
    static constexpr uint16_t kOobBasePort = 42000;
    // port_for(mailbox_id) = kOobBasePort + (mailbox_id & 0x7FFF)
    // system_id = (mailbox_id >> 24) & 0xFF
    uint8_t local_system_id{0};
    struct Route { uint8_t system_id; char ip[16]; };
    std::array<Route, 8> routes{};
    uint8_t route_count{0};
};

// SHM layout header (PUBLIC mode) — stored at offset 0 of the SHM region
struct ShmLayout {
    uint32_t capacity;
    uint32_t slot_size;
    uint32_t _pad[2];    // pad to 16 bytes
};
```

**Key EVL API calls for EvlMailbox**:
```cpp
// Ring buffer modes (Local / Public):
evl_create_mutex(&ring_mutex_, EVL_CLOCK_MONOTONIC, 0, flags, "%s", name);
evl_create_event(&ring_event_, EVL_CLOCK_MONOTONIC, eflags, "%s", name);

// Send (OOB-safe):
evl_lock_mutex(&ring_mutex_);
// write to ring slot
evl_signal_event(&ring_event_);   // signal before unlock for OOB wake-up
evl_unlock_mutex(&ring_mutex_);

// Receive (blocking, OOB-safe):
evl_timedwait_event(&ring_event_, &ring_mutex_, &abs_ts);
// read from ring slot
evl_unlock_mutex(&ring_mutex_);

// Network mode:
socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0)   // OOB UDP socket
evl_net_solicit(...)                          // prime ARP/route cache at init
oob_sendmsg(...)                             // OOB send
oob_recvmsg(...)                             // OOB receive
```

Full EVL API reference: `CommRaT/docs/work/EVL_API_REFERENCE.md` (via symlink).

## Messaging Layer

### Wire Format

```cpp
// On-wire header — matches RACK tims_msg_head layout exactly
// Defined in wire_message.hpp (alongside WireMessage<T>)
struct WireHeader {
    uint32_t msg_type;    // MessageDefinition::full_id()
    uint32_t msg_size;    // total serialized bytes (header + payload)
    uint64_t timestamp;   // nanoseconds since epoch (single source of truth)
    uint32_t seq_number;
    uint32_t dest;        // destination mailbox address
    uint32_t src;         // source mailbox address
    uint32_t flags;
};

template<typename T>
struct WireMessage {
    WireHeader header;
    T          payload;
    using payload_type = T;
};
```

### MessageDefinition

Defined in `message_id.hpp`. All fields are `static constexpr`.

```cpp
template<
    typename PayloadT,
    MessagePrefix Prefix_ = MessagePrefix::UserDefined,
    auto SubPrefix_        = UserSubPrefix::Data,
    uint16_t ID_           = 0,   // 0 = auto-assigned by MessageRegistry
    typename ReplyT        = void>
struct MessageDefinition {
    using Payload        = PayloadT;
    using ReplyPayload   = ReplyT;
    using ReplyMessageDef = /* auto-derived — ID flipped to negative */;

    static constexpr MessagePrefix prefix    = Prefix_;
    static constexpr uint8_t       subprefix = /* from SubPrefix_ */;
    static constexpr uint16_t      local_id  = ID_;
    static constexpr uint32_t      full_id();    // Prefix | SubPrefix | local_id
    static constexpr bool          is_request;   // ReplyT != void
    static constexpr bool          is_reply;     // negative ID (auto-derived reply)
    static constexpr bool          has_reply;    // is_request alias
    static constexpr bool          needs_auto_id; // ID_ == 0
};

// Convenience wrappers in namespace Message::
namespace Message {
    template<typename T> using Data    = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Data>;
    template<typename T> using Command = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Commands>;
    template<typename T> using Event   = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Events>;
}
```

### MessageRegistry

```cpp
template<typename... MessageDefs>
struct MessageRegistry {
    // Compile-time ID collision detection
    static_assert(no_id_collisions_v<MessageDefs...>);

    // visit() — type-safe receive dispatch
    template<typename Handler>
    static bool visit(uint32_t msg_type, std::span<const std::byte> buf, Handler&& h);

    // find_def<Payload>() — lookup by payload type
    template<typename Payload>
    using find_def = /* MessageDefinition for Payload */;
};
```

## Mailbox API

```cpp
// Type-safe frontend — all message types checked at compile time
// Takes MessageDefinition types directly; builds MessageRegistry internally
template<typename... MessageDefs>
class Mailbox {
public:
    explicit Mailbox(const MailboxConfig& config);
    ~Mailbox();

    MailboxResult<void> start();   // initialize backend, mark running
    void                stop();    // shutdown backend

    bool is_running() const;

    // Send — compile-time type check; takes WireMessage<T>& directly
    template<typename T>
        requires is_registered<T>
    MailboxResult<void> send(WireMessage<T>& message, uint32_t dest_mailbox_id);

    // Send a reply — dest taken from request.header.src automatically
    template<typename RequestPayload, typename ReplyPayload>
    MailboxResult<void> send_reply(const WireMessage<RequestPayload>& request,
                                   ReplyPayload& reply);

    // Blocking receive with visitor (1 s default timeout)
    template<typename Visitor>
    MailboxResult<void> receive_any(Visitor&& visitor);

    // Blocking receive with explicit timeout and visitor
    template<typename Visitor>
    MailboxResult<void> receive_any_for(Duration timeout, Visitor&& visitor);

    // Typed receive into a pre-allocated WireMessage<T> (zero-copy)
    template<typename T>
        requires is_registered<T>
    bool receive(WireMessage<T>& message, Duration timeout = Seconds(1));
};
```

### MailboxConfig

```cpp
struct MailboxConfig {
    uint32_t mailbox_id;
    size_t   message_slots    = 10;
    size_t   max_message_size = 4096;
    uint8_t  send_priority    = 10;
    bool     realtime         = false;

    // EVL only — ignored on STD/TIMS
    bool     cross_process    = false;   // Mode::Public (cross-process SHM ring)
    bool     network          = false;   // Mode::Network (OOB UDP, cross-machine)
    uint8_t  local_system_id  = 0;      // must match mailbox_id[31:24]

    struct NetworkRoute { uint8_t system_id; char ip[16]; };
    static constexpr uint8_t kMaxNetworkRoutes = 8;
    std::array<NetworkRoute, kMaxNetworkRoutes> network_routes{};
    uint8_t  network_route_count{0};
};
```

## Real-Time Constraints

**FORBIDDEN in hot paths:**
```cpp
new/delete, malloc/free
std::vector::push_back()  // May allocate
std::string operations    // May allocate
std::cout in loops        // Blocking I/O
throw exceptions          // Unpredictable timing
```

**ALWAYS use:**
```cpp
std::array<T, N>                     // Fixed-size
sertial::fixed_vector<T, N>          // Fixed capacity
std::atomic<T>                       // Lock-free
constexpr / static_assert            // Compile-time
sertial::Message<T>::max_buffer_size // Compile-time buffer sizing
```

## CMake Build System

```bash
cmake --preset default    # STD platform, TiMS IPC (build + test natively)
cmake --preset debug      # STD platform, debug build
cmake --preset evl        # EVL platform (requires ratos-dev-image or guest)
cmake --preset evl-cross  # EVL cross-compile for QEMU (requires RaTOS SDK)
```

CMake options:
- `CORERAT_PLATFORM` — `STD` or `EVL`
- `CORERAT_IPC` — `TIMS`, `EVL`, or `AUTO`
- `CORERAT_BUILD_TESTS` — `ON`/`OFF`
- `CORERAT_BUILD_ROUTERS` — `ON`/`OFF`

Install targets:
- `CoreRaT::corerat` — header-only core (platform + messaging)
- `CoreRaT::corerat_tims` — + TiMS IPC backend (links `tims_backend.cpp`; only when `CORERAT_IPC=TIMS`)

The STD-platform preset also produces a `corerat-std_<version>_amd64.deb` package via CPack.

## EVL Development Workflow

```bash
# First time — build in QEMU and cache binaries locally:
scripts/evl-dev.sh --build evl --test

# Subsequent runs — deploy cached build/evl/, reconfigure, run ctest:
scripts/evl-dev.sh --test

# Cross-compile on host (requires RaTOS SDK):
scripts/evl-dev.sh --cross --test

# Interactive shell with deployed binaries:
scripts/evl-dev.sh --shell

# Build, deploy, open shell:
scripts/evl-dev.sh --build evl --shell
```

The script reads `.corerat.env` for `RATOS_RELEASE_TAG`, `QEMU_MEMORY`, `QEMU_CPUS`. Override per-machine in `.corerat.env.local` (gitignored).

After `--build evl`, binaries are cached in `build/evl/` on the host. Subsequent `--test` runs skip recompilation (rsync + cmake configure + ctest only).

## Code Style

```cpp
// Doxygen-style public API docs
/**
 * @brief Blocking receive — OOB-safe on EVL platform
 *
 * @tparam Handler  Callable: void(const WireMessage<T>&)
 * @param  timeout  Maximum wait duration (0 = non-blocking peek)
 * @return          MailboxResult<void>: ok, timeout, or error
 *
 * @note Real-time safe: uses evl_timedwait_event on EVL platform
 */
template<typename Handler>
MailboxResult<void> receive(Duration timeout, Handler&& handler);
```

No emojis anywhere — not in source, headers, docs, or cout output.

## Implementation Phases

| Phase | Status | Description |
|---|---|---|
| 1 | done | Platform layer (Thread, Mutex, CondVar, Time) — STD + EVL backends |
| 2 | done | Messaging layer (WireHeader, WireMessage, MessageRegistry) + TiMS IPC backend |
| 3 | done | EVL IPC backend (EvlMailbox — ring buffer + evl_mutex + evl_event) |

## What CoreRaT Does NOT Own

- CommRaT `Module2`, `IOSpec`, subscription protocol, `GetData`/`GetNextData`
- CommRaT application lifecycle (`CommRaT<>` template)
- CommRaT mailbox address calculation helpers
- Any user-facing module APIs

## Design Constraints

1. No heap allocation in any hot path (send, receive, timestamp, lock/unlock)
2. The `IpcMailbox` concept must be satisfied by both `TimsMailbox` and `EvlMailbox`
3. Wire format unchanged from RACK — existing CommRaT nodes must connect without modification
4. `CORERAT_PLATFORM_STD` must build and test on any Linux host without libevl
5. All public APIs validated at compile time — no runtime type dispatch
6. `CoreRaTConfig.cmake` must export `EVL::evl` imported target for downstream consumers when built with `CORERAT_PLATFORM=EVL`

## Future Considerations

### Shared evl-dev.sh (post-Phase-3)

Currently both CoreRaT and CommRaT maintain their own copy of `scripts/evl-dev.sh`.
The two copies share ~95% of their logic; differences are:

| Aspect | CoreRaT | CommRaT |
|---|---|---|
| Env file | `.corerat.env` | `.commrat.env` |
| Guest source path | `/root/CoreRaT/` | `/root/CommRaT/` |
| Work dir prefix | `/tmp/corerat-evl-*` | `/tmp/commrat-evl-*` |
| rsync excludes | `CommRaT`, `tims` | none extra |

When CoreRaT is installed as a proper system package (post-Phase-3), the script
should move to CoreRaT and accept project-level configuration so downstream projects
(CommRaT, etc.) invoke it without carrying their own copy:

```bash
# Downstream project invocation (future):
corerat-evl-dev \
    --project-name CommRaT \
    --env-file .commrat.env \
    --guest-dir /root/CommRaT \
    "$@"
```

Until then: sync improvements manually between the two copies. Changes that belong
in both scripts should be applied to CoreRaT first, then ported to CommRaT.
