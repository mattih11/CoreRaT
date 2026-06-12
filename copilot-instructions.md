# CoreRaT — Copilot Agent Instructions

## What is CoreRaT?

CoreRaT is a standalone C++20 library — the shared foundation that CommRaT (and future projects) build on top of. It owns:

| Subsystem | Files |
|---|---|
| **Platform** | `include/corerat/platform/` — Thread, Mutex, SharedMutex, ConditionVariable, Time, Duration; STD and EVL backends |
| **Messaging** | `include/corerat/messaging/` — WireHeader, WireMessage, MessageDefinition, MessageRegistry |
| **IPC** | `include/corerat/ipc/` — Mailbox<>, TimsMailbox (TiMS backend), EvlMailbox (EVL backend stub) |

CommRaT depends on CoreRaT. CoreRaT has **no dependency on CommRaT**.

## Repository layout

```
include/corerat/
  platform/
    platform.hpp          # CORERAT_PLATFORM_STD / CORERAT_PLATFORM_EVL selection
    duration.hpp          # Duration (int64_t ns_, structural NTTP type)
    threading.hpp         # Thread, Mutex, SharedMutex, ConditionVariable
    timestamp.hpp         # Timestamp (uint64_t ns), Time::now/sleep/sleep_until
    std/
      threading_impl.hpp  # std:: backend
      timestamp_impl.hpp
    evl/
      threading_impl.hpp  # EVL backend (evl_mutex, evl_event, pthread+evl_attach)
      timestamp_impl.hpp
  messaging/
    message_id.hpp        # MessagePrefix, UserSubPrefix, MessageDefinition<>
    wire_message.hpp      # WireHeader, WireMessage<T> (replaces CommRaT TimsMessage)
    message_registry.hpp  # MessageRegistry<...> — auto-ID, collision detection, visit()
    registry_utils.hpp    # Compile-time registry filters and lookup helpers
  ipc/
    mailbox_config.hpp    # MailboxError, MailboxResult<T>, MailboxConfig
    mailbox.hpp           # Mailbox<MessageDefs...> — type-safe frontend
    tims/
      tims_backend.hpp    # TimsMailbox — RACK/TiMS backend
    evl/
      evl_backend.hpp     # EvlMailbox — EVL ring-buffer backend (Phase 3)
src/
  tims_backend.cpp        # TimsMailbox implementation
cmake/
  CoreRaTConfig.cmake.in
  isar-sdk-toolchain.cmake
CMakeLists.txt
CMakePresets.json
CommRaT/                  # read-only symlink to CommRaT repo (reference only)
```

## Design document

Full design brief: `docs/work/CORERAT_DESIGN.md`

## Namespace

All CoreRaT symbols live in namespace `corerat::`.
Macro prefix: `CORERAT_PLATFORM_*`, `CORERAT_IPC_*`, `CORERAT_HAS_*`.

## Key types

| CoreRaT type | CommRaT equivalent | Notes |
|---|---|---|
| `WireHeader` | `TimsHeader` | wire format unchanged (RACK-compatible) |
| `WireMessage<T>` | `TimsMessage<T>` | aggregate: {WireHeader, T payload} |
| `TimsMailbox` | `TimsWrapper` | TIMS IPC backend |
| `MessageRegistry<>` | `MessageRegistry<>` | no GetData expansion (CommRaT adds that) |
| `Mailbox<>` | `Mailbox<>` | uses TimsMailbox backend |

## Platform backends

Selected at compile time via `CORERAT_PLATFORM`:

| Symbol | Backend |
|---|---|
| `CORERAT_PLATFORM_STD` (default) | `std::thread`, `std::mutex`, `std::chrono` |
| `CORERAT_PLATFORM_EVL` | `pthread` + `evl_attach_self`, `evl_mutex`, `evl_event`, `evl_read_clock` |

## IPC backends

Selected via `CORERAT_IPC` (AUTO = follows platform):

| Symbol | Backend |
|---|---|
| `CORERAT_IPC_TIMS` (default on STD) | RACK/TiMS sockets — `tims_mbx_create`, `tims_recvmsg_timed` |
| `CORERAT_IPC_EVL` (default on EVL) | EVL ring buffer — `evl_mutex` + `evl_event` + `memfd`/`mmap` (**Phase 3**) |

## CMake usage (downstream)

```cmake
find_package(CoreRaT REQUIRED)

# TiMS IPC (STD platform):
target_link_libraries(mylib PUBLIC CoreRaT::corerat_tims)

# EVL IPC (EVL platform, Phase 3):
target_link_libraries(mylib PUBLIC CoreRaT::corerat)
```

## Build presets

| Preset | Platform | IPC |
|---|---|---|
| `default` | STD | TIMS |
| `debug` | STD | TIMS |
| `evl` | EVL | EVL (compile-check in container) |
| `evl-cross` | EVL | EVL (RaTOS SDK cross-compile) |

## EVL API reference

Key libevl primitives used in CoreRaT:

```cpp
// Threading
evl_attach_self("name:%d", id)           // attach pthread to EVL core
evl_set_schedattr(efd, &attrs)            // set SCHED_FIFO priority
evl_set_thread_mode(efd, EVL_T_WOSS, ..) // warn on OOB→inband switch
evl_detach_self()

// Mutex (PI)
evl_new_mutex(&m, "%s", name)
evl_lock_mutex(&m)  /  evl_unlock_mutex(&m)
evl_trylock_mutex(&m)
evl_close_mutex(&m)

// Event (condition variable)
evl_new_event(&e, "%s", name)
evl_wait_event(&e, &m)
evl_timedwait_event(&e, &m, &abs_ts)
evl_signal_event(&e)  /  evl_broadcast_event(&e)
evl_close_event(&e)

// Clock / sleep
evl_read_clock(EVL_CLOCK_MONOTONIC, &ts)
evl_usleep(us)
evl_sleep_until(EVL_CLOCK_MONOTONIC, &abs_ts)
evl_yield()
```

Full EVL API reference: `CommRaT/docs/work/EVL_API_REFERENCE.md`

## Real-time constraints (must be respected in all code)

- No heap allocation (`new`, `malloc`, `std::vector`, `std::string`) in hot paths
- No `throw`, `std::cout`, POSIX mutexes in OOB context
- Compile-time sized buffers: `sertial::Message<WireMessage<T>>::max_buffer_size`
- Timestamps via `WireHeader.timestamp` only — not payload fields
- `std::atomic` and `memcpy` are OOB-safe

## SeRTial API

```cpp
sertial::Message<T>::serialize(T&)           // → Result (has .view() → span<byte>)
sertial::Message<T>::deserialize(span<byte>) // → DeserializeResult<T> (has .value())
sertial::Message<T>::max_buffer_size         // constexpr size_t
sertial::fixed_vector<T, N>                  // stack-allocated bounded vector
sertial::fixed_string<N>                     // stack-allocated bounded string
```

## Implementation phases

| Phase | Status | Description |
|---|---|---|
| 1 | ✅ done | Platform layer + Messaging layer (verbatim from CommRaT, namespace corerat::) |
| 2 | ✅ done | IPC TiMS backend (TimsMailbox from TimsWrapper) + Mailbox<> frontend |
| 3 | 🔲 todo | EVL IPC backend (EvlMailbox — ring buffer + evl_mutex + evl_event) |

## What CoreRaT does NOT know

- CommRaT `Module2`, `IOSpec`, subscription protocol
- CommRaT `GetData` / `GetNextData` expansion (CommRaT adds that on top of CoreRaT's registry)
- CommRaT application lifecycle (`CommRaT<>` template)
