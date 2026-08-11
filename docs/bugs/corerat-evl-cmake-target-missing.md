# Bug: `corerat_evl` CMake target missing — EVL IPC backend never selected

**Repository**: CoreRaT  
**Date**: 2026-08-11  
**Severity**: High — all EVL runtime tests hang indefinitely

---

## Describe the bug

`evl_backend.hpp` (`include/corerat/ipc/evl/`) is fully implemented and
`mailbox.hpp` selects it via `#ifdef CORERAT_IPC_EVL`, but no CMake target
ever defines that macro or links libevl. `CMakeLists.txt` only builds
`corerat_tims`.

As a result, builds with `CORERAT_PLATFORM=EVL` silently fall back to the
TiMS TCP backend, which calls `tims_recvmsg_timed()` from an EVL thread —
causing an **in-band demotion** and hanging indefinitely on `receive`.

## To reproduce

```bash
# Cross-compile CommRaT for EVL guest (requires RaTOS SDK sourced)
cmake --preset evl-cross
cmake --build --preset evl-cross

# Deploy to EVL guest and run any test that blocks on mailbox receive
./test_3input_fusion   # hangs after module start, never progresses
```

Observed output:

```
Creating modules...
[CommRaT] Platform: EVL (Xenomai 4 / libevl) | OOB=1 | PI-mutex=1
Starting modules...
[corerat] evl_create_mutex OK
[corerat] evl_create_event OK
...
^C   (process does not exit — stuck in tims_recvmsg_timed inside an EVL thread)
```

## Root cause

`mailbox.hpp` backend selection:

```cpp
#ifdef CORERAT_IPC_EVL
#  include "corerat/ipc/evl/evl_backend.hpp"
using IpcMailbox = EvlMailbox;
#else
#  include "corerat/ipc/tims/tims_backend.hpp"
using IpcMailbox = TimsMailbox;
#endif
```

`CORERAT_IPC_EVL` is never defined because the CMake target that would do so
does not exist.  Every consumer (CommRaT included) therefore gets
`TimsMailbox` regardless of platform.

## Expected behaviour

When `CORERAT_PLATFORM=EVL`, the build should produce a `corerat_evl` CMake
target that:

1. Defines `CORERAT_IPC_EVL`
2. Links `libevl`
3. Is exported as `CoreRaT::corerat_evl`

## Suggested fix

### CoreRaT `CMakeLists.txt`

Add alongside the existing `corerat_tims` block:

```cmake
if(CORERAT_PLATFORM STREQUAL "EVL")
    add_library(corerat_evl INTERFACE)
    add_library(CoreRaT::corerat_evl ALIAS corerat_evl)
    target_compile_definitions(corerat_evl INTERFACE CORERAT_IPC_EVL)
    target_link_libraries(corerat_evl INTERFACE corerat evl)
    install(TARGETS corerat_evl EXPORT CoreRaTTargets)
endif()
```

### CommRaT `CMakeLists.txt` (secondary, ~3 lines)

Update the backend selection to prefer `corerat_evl` when available:

```cmake
if(TARGET CoreRaT::corerat_evl)
    target_link_libraries(commrat INTERFACE CoreRaT::corerat_evl)
    message(STATUS "CommRaT: using CoreRaT EVL backend")
elseif(TARGET CoreRaT::corerat_tims)
    target_link_libraries(commrat INTERFACE CoreRaT::corerat_tims)
    message(STATUS "CommRaT: using CoreRaT TiMS backend (STD platform)")
else()
    target_link_libraries(commrat INTERFACE CoreRaT::corerat)
    message(STATUS "CommRaT: using CoreRaT header-only backend")
endif()
```

## Affected tests (CommRaT EVL guest)

All tests that instantiate a `Module2` and call `start()` block:

- `test_3input_fusion`
- `test_address_collisions`
- `test_timestamp_logic`
