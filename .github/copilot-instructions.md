# GitHub Copilot Instructions for CoreRaT

## Project Overview

**CoreRaT** (Core Runtime) is a standalone C++20 library — the shared foundation for CommRaT and future real-time systems. It provides three subsystems:

| Subsystem | Scope |
|---|---|
| **Platform** | `Thread`, `Mutex`, `SharedMutex`, `ConditionVariable`, `Time`, `Duration` — STD and EVL backends |
| **Messaging** | `WireHeader`, `WireMessage<T>`, `MessageDefinition`, `MessageRegistry` — compile-time message ID system |
| **IPC** | `Mailbox<>` frontend + `TimsMailbox` (TCP/TiMS backend) + `EvlMailbox` (EVL ring-buffer backend, Phase 3) |

**CoreRaT does NOT depend on CommRaT.** CommRaT depends on CoreRaT.

**Current Status**: Phases 1 (platform), 2 (IPC TiMS backend) complete. Phase 3 (EVL IPC backend) in progress.

**Next**: `EvlMailbox` implementation (`include/corerat/ipc/evl/evl_backend.hpp`).

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
    message_id.hpp         # MessagePrefix, UserSubPrefix, make_message_id
    message_def.hpp        # MessageDefinition<Payload, Prefix, SubPrefix, ID, Reply>
    message_registry.hpp   # MessageRegistry<...> — compile-time dispatch
    registry_utils.hpp     # Compile-time registry filters and lookup helpers
    wire_header.hpp        # WireHeader — on-wire struct (RACK-compatible)
    wire_message.hpp       # WireMessage<T> = {WireHeader header; T payload;}
  ipc/
    ipc.hpp                # IPC backend selection
    mailbox_config.hpp     # MailboxConfig, MailboxResult<T>, MailboxError
    mailbox.hpp            # Mailbox<Registry> — type-safe frontend
    tims/
      tims_backend.hpp     # TimsMailbox — RACK-compatible TCP backend
    evl/
      evl_backend.hpp      # EvlMailbox — EVL ring-buffer backend (Phase 3)
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
  ping_pong/               # End-to-end EVL IPC test
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

**Problem**: `tims_recvmsg_timed()` is a blocking POSIX socket call. When called from an EVL thread, it demotes the thread from OOB to in-band, losing all real-time guarantees. This is why the EVL IPC backend (Phase 3) is needed.

### EVL IPC Backend (Phase 3, in progress)

**Goal**: A receive path that stays entirely in OOB context.

**Design**: Lock-free ring buffer per mailbox, backed by `evl_mutex` + `evl_event`:

```cpp
// include/corerat/ipc/evl/evl_backend.hpp
class EvlMailbox {
public:
    enum class Mode {
        Local,    // Same-process heap ring (fastest, no SHM)
        Public,   // Cross-process POSIX SHM ring (evl_mutex on SHM)
        Network   // OOB UDP/IPv4, cross-machine RT
    };

    EvlMailbox(uint32_t mailbox_id, size_t slots, size_t max_msg_size, Mode mode);
    ~EvlMailbox();

    bool    create();
    void    destroy();
    bool    send(uint32_t dest_id, std::span<const std::byte> payload);
    ssize_t receive(std::span<std::byte> buffer, int64_t timeout_ns);
    bool    peek();

private:
    // Ring buffer: producer writes to head, consumer reads from tail
    // Protected by evl_mutex (OOB-safe)
    struct alignas(64) Slot {
        std::atomic<uint32_t> seq;
        uint32_t              size;
        std::array<std::byte, kMaxMessageSize> data;
    };
    // ...
};
```

**Key EVL API calls for EvlMailbox**:
```cpp
evl_new_mutex(&ring_mutex_, "corerat-mbx-%u", mailbox_id_);
evl_new_event(&ring_event_,  "corerat-evt-%u", mailbox_id_);

// Send (OOB-safe):
evl_lock_mutex(&ring_mutex_);
// write to ring slot
evl_unlock_mutex(&ring_mutex_);
evl_signal_event(&ring_event_);

// Receive (blocking, OOB-safe):
evl_lock_mutex(&ring_mutex_);
evl_timedwait_event(&ring_event_, &ring_mutex_, &abs_ts);
// read from ring slot
evl_unlock_mutex(&ring_mutex_);
```

Full EVL API reference: `CommRaT/docs/work/EVL_API_REFERENCE.md` (via symlink).

## Messaging Layer

### Wire Format

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

template<typename T>
struct WireMessage {
    WireHeader header;
    T          payload;
    using payload_type = T;
};
```

### MessageDefinition

```cpp
template<
    typename Payload,
    MessagePrefix Prefix,
    auto SubPrefix,
    uint16_t LocalID,
    typename ReplyPayload = void>
struct MessageDefinition {
    using payload_type = Payload;
    static constexpr uint32_t full_id();       // Prefix | SubPrefix | local_id
    static constexpr bool has_reply;
    static constexpr bool is_request;
    using ReplyMessageDef = /* auto-generated */;
};
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
template<typename Registry>
class Mailbox {
public:
    explicit Mailbox(const MailboxConfig& config);
    ~Mailbox();

    MailboxResult<void> initialize();
    void                shutdown();

    // Send — compile-time type check
    template<typename T>
        requires Registry::template contains<T>
    MailboxResult<void> send(const T& payload, uint32_t dest_mailbox_id);

    // Blocking receive with visitor pattern
    template<typename Handler>
    MailboxResult<void> receive(Duration timeout, Handler&& handler);
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

Install targets:
- `CoreRaT::corerat` — header-only core (platform + messaging)
- `CoreRaT::corerat_tims` — + TiMS IPC backend (links `tims_backend.cpp`)

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
| 3 | in progress | EVL IPC backend (EvlMailbox — ring buffer + evl_mutex + evl_event) |

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
