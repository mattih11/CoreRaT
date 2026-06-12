# CoreRaT

High performance realtime communication framework based on EVL, modern C++ and SeRTIal's compiletime-reflection

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

---

## Overview

CoreRaT provides three independently usable layers:

| Layer | Headers | What it does |
|---|---|---|
| **Platform** | `include/corerat/platform/` | `Thread`, `Mutex`, `SharedMutex`, `ConditionVariable`, `Timestamp`, `Duration` — backed by `std::` (default) or Xenomai 4 EVL primitives |
| **Messaging** | `include/corerat/messaging/` | Compile-time `MessageRegistry<>`, `WireMessage<T>`, `WireHeader` (RACK-compatible wire format), `MessageDefinition<>` |
| **IPC** | `include/corerat/ipc/` | `TimsMailbox` (RACK/TiMS TCP sockets), `EvlMailbox` (EVL OOB ring buffer) |

---

## Architecture

```
CommRaT
  └── CoreRaT
        ├── Platform layer   (std:: or EVL — selected at compile time)
        ├── Messaging layer  (WireMessage / MessageRegistry — header-only)
        └── IPC layer        (TimsMailbox via POSIX TCP  /  EvlMailbox via libevl)
```

### Platform backends

| `CORERAT_PLATFORM` | Threading | Time | IPC (AUTO) |
|---|---|---|---|
| `STD` (default) | `std::thread`, `std::mutex` | `std::chrono` | TiMS TCP sockets |
| `EVL` | `pthread` + `evl_attach_self`, `evl_mutex`, `evl_event` | `evl_read_clock` | EVL OOB ring buffer |

### Wire format

The `WireHeader` / `WireMessage<T>` layout is byte-compatible with **RACK**'s `tims_msg_head`.
Existing RACK and CommRaT nodes connect to `corerat-router-tcp` without any modification.

---

## Getting Started

### Prerequisites

| Dependency | Notes |
|---|---|
| **CMake ≥ 3.25** | Build system |
| **GCC ≥ 13** or **Clang ≥ 17** | C++20 required |
| **[SeRTial](https://github.com/mattih11/SeRTial)** | Zero-allocation compile-time serialization |
| **libevl** *(EVL platform only)* | Xenomai 4 EVL runtime; provided by the RaTOS SDK |

### Build

```bash
# Clone
git clone https://github.com/mattih11/CoreRaT.git
cd CoreRaT

# Standard platform (POSIX sockets, no EVL kernel required)
cmake --preset debug
cmake --build --preset debug --parallel $(nproc)

# Run unit tests
cd build/debug && ctest -L unit -V
```

### Build for EVL (Xenomai 4)

Use the RaTOS SDK and `evl-dev.sh` — see [EVL Testing with RaTOS](#evl-testing-with-ratos).

```bash
# Cross-compile against the RaTOS SDK (auto-downloaded on first run)
scripts/evl-dev.sh --cross evl-cross
```

---

## TCP Router

`corerat-router-tcp` is a drop-in replacement for the RACK `TimsRouterTcp` daemon.

```bash
# Start on the default TiMS port
./build/debug/corerat-router-tcp

# Custom port / message size limit
./build/debug/corerat-router-tcp --port 2001 --max-msg-size 65536
```

Nodes connect over TCP, register their mailbox IDs, and the router forwards frames by destination mailbox.

---

## Ping-Pong Test

Two binaries communicate through the router — a basic RTT benchmark:

```bash
# Start router (in one terminal)
./build/debug/corerat-router-tcp

# Start pong node (in another terminal)
./build/debug/test/pong_node --count 1000

# Start ping node (measures round-trip time)
./build/debug/test/ping_node --count 1000
```

Expected output from `ping_node`:
```
ping_node: sending 1000 ping(s) via corerat-router-tcp
ping_node: registered mailbox 0x3001
ping_node: 1000/1000 replies  RTT min/avg/max = 42.3 / 87.1 / 320.5 µs
```

Run both together via CTest:
```bash
cd build/debug && ctest -L integration -V
```

---

## CMake Integration (downstream)

```cmake
find_package(CoreRaT REQUIRED)

# STD platform (TiMS IPC)
target_link_libraries(my_module PUBLIC CoreRaT::corerat_tims)

# EVL platform (EVL IPC — header-only, links libevl via corerat)
target_link_libraries(my_module PUBLIC CoreRaT::corerat)
```

---

## EVL Testing with RaTOS

`scripts/evl-dev.sh` boots a [RaTOS](https://github.com/mattih11/RaTOS) EVL kernel in QEMU,
cross-compiles CoreRaT against the RaTOS SDK, deploys, and runs the full test suite on the EVL kernel.

```bash
# Default: cross-compile (evl-cross preset) + deploy + run all tests in QEMU
scripts/evl-dev.sh

# Cross-compile only (no QEMU — fast CI compile-check)
scripts/evl-dev.sh --cross evl-cross

# Cross-compile, deploy, open interactive shell on EVL guest
scripts/evl-dev.sh --cross --shell

# Run a specific binary on the EVL guest
scripts/evl-dev.sh --cross --run /root/corerat/test/ping_node
```

**Prerequisites**: `qemu-system-x86_64`, `rsync`, `ssh-keygen`, and `gh` (GitHub CLI, authenticated).

Pin the RaTOS release in `.corerat.env`:
```bash
RATOS_RELEASE_TAG=v0.0.7   # update when upgrading RaTOS
```

Override locally without touching the committed file:
```bash
echo "RATOS_RELEASE_TAG=v0.0.8" >> .corerat.env.local
```

---

## Credits

### RACK — Robotics Application Construction Kit

CoreRaT's TCP IPC layer is **wire-compatible with RACK**, an open-source robotics middleware developed at the
**University of Hannover** (Institute for Systems Engineering - RTS, Professor Bernardo Wagner).

RACK defines:
- The **TiMS** (TiMS IPC Message System) wire protocol — the 16-byte `tims_msg_head` frame that `WireHeader` mirrors exactly
- The **TiMS router** TCP protocol that `corerat-router-tcp` implements (port 2000, `MSG_ROUTER_MBX_INIT_WITH_REPLY`, watchdog, routing by mailbox ID)
- The mailbox ID addressing scheme that `MessageDefinition<>` preserves

RACK authors: Joerg Langenberg, Marko Reimer, Jan Kiszka, Oliver Wulf, Sebastian Smolorz and contributors.
RACK source: https://github.com/smolorz/RACK — licensed GPL v2 (or later).

CoreRaT introduces no RACK library dependency; the wire protocol is re-implemented from scratch in C++20.

### SeRTial

Zero-allocation compile-time serialization by [@mattih11](https://github.com/mattih11).
Provides `sertial::Message<T>::serialize()`, `fixed_string<N>`, `fixed_vector<T,N>` used throughout CoreRaT.
Source: https://github.com/mattih11/SeRTial

### RaTOS

Xenomai 4 / EVL kernel image and SDK used for EVL testing, by [@mattih11](https://github.com/mattih11).
Source: https://github.com/mattih11/RaTOS

---

## License

GPL v2 — see LICENSE for the full text.
The RACK wire protocol constants re-implemented here are also GPL v2.

