# CoreRaT

High performance realtime communication framework based on EVL, modern C++ and SeRTIal's compiletime-reflection

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

---

## Overview

CoreRaT provides three independently usable layers:

| Layer | Headers | What it does |
|---|---|---|
| **Platform** | `include/corerat/platform/` | `Thread`, `Mutex`, `Timestamp`, `Duration` — backed by `std::` (default) or Xenomai 4 EVL primitives |
| **Messaging** | `include/corerat/messaging/` | Compile-time `MessageRegistry<>`, `WireMessage<T>`, `WireHeader` (RACK-compatible wire format), `MessageDefinition<>` |
| **IPC** | `include/corerat/ipc/` | `Mailbox<>` with three backends: TiMS TCP, EVL SHM, EVL OOB UDP |

---

## Architecture

```
CommRaT
  └── CoreRaT
        ├── Platform layer   (std:: or EVL — selected at compile time)
        ├── Messaging layer  (WireMessage / MessageRegistry — header-only)
        └── IPC layer        (Mailbox<> — backend selected at compile + config time)
```

### Platform backends

| `CORERAT_PLATFORM` | Threading | Time | IPC backend |
|---|---|---|---|
| `STD` (default) | `std::thread`, `std::mutex` | `std::chrono` | TiMS TCP sockets |
| `EVL` | `pthread` + `evl_attach_self`, `evl_mutex`, `evl_event` | `evl_read_clock` | EVL OOB ring buffer or OOB UDP |

### IPC modes

Three modes are available through `MailboxConfig` — same `Mailbox<>` API for all:

| Mode | `MailboxConfig` | Transport | Use case |
|---|---|---|---|
| **Local** | default | EVL ring in heap | Single process, multiple threads |
| **Public** | `cross_process = true` | EVL ring in POSIX SHM | Multiple processes, same machine |
| **Network** | `network = true` | EVL OOB UDP (IPv4) | Multiple machines, cross-machine RT |
| *(TIMS)* | *(STD platform)* | TCP sockets via router | Non-RT / compatibility with RACK |

### Mailbox addressing

Mailbox IDs follow the RACK/TiMS convention — the 32-bit address is split into four 8-bit fields:

```
 31      24  23      16  15       8  7        0
┌──────────┬──────────┬──────────┬──────────┐
│ system   │  class   │ instance │  local   │
│   id     │   id     │   id     │   id     │
└──────────┴──────────┴──────────┴──────────┘
```

- **system_id** — identifies the host machine (used for OOB UDP routing)
- **class_id** — the component class (e.g. LadarScanner, Chassis)
- **instance_id** — disambiguates multiple components of the same class
- **local_id** — 0 = command mailbox, others = data/reply mailboxes within a component

### Wire format

`WireHeader` / `WireMessage<T>` is byte-compatible with **RACK**'s `tims_msg_head`.
Existing RACK and CommRaT nodes connect to `corerat-router-tcp` without modification.

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
# Standard platform (POSIX sockets, no EVL kernel required)
cmake --preset debug
cmake --build --preset debug --parallel $(nproc)

# Run tests
cd build/debug && ctest --output-on-failure
```

### Build for EVL (Xenomai 4)

Use the RaTOS SDK and `evl-dev.sh` — see [EVL Testing with RaTOS](#evl-testing-with-ratos).

```bash
scripts/evl-dev.sh --cross evl-cross
```

---

## Quick API Reference

### Defining messages

```cpp
#include "corerat/messaging/wire_message.hpp"
#include "corerat/messaging/message_id.hpp"

// Define payloads (must be SeRTial-serializable)
struct PingPayload { uint32_t seq; uint64_t timestamp_ns; };
struct PongPayload { uint32_t seq; uint64_t timestamp_ns; uint64_t latency_ns; };

// Wrap in MessageDefinition: <Payload, Prefix, SubPrefix, LocalID>
using PingDef = MessageDefinition<PingPayload, MessagePrefix::UserDefined,
                                  UserSubPrefix::Data, 1>;
using PongDef = MessageDefinition<PongPayload, MessagePrefix::UserDefined,
                                  UserSubPrefix::Data, 2>;
```

### Creating a mailbox

```cpp
#include "corerat/ipc/mailbox.hpp"

// Mailbox<> is typed by the message definitions it handles
using PingPongMailbox = Mailbox<PingDef, PongDef>;

MailboxConfig cfg;
cfg.mailbox_id = 0x00012000;   // system 0, class 1, instance 2, local 0

PingPongMailbox mbx(cfg);
mbx.start();
```

### Sending and receiving

```cpp
// Send
WireMessage<PingPayload> msg;
msg.payload = { .seq = 42, .timestamp_ns = Time::now() };
mbx.send(msg, DEST_MAILBOX_ID);

// Receive a specific type
WireMessage<PongPayload> reply;
if (mbx.receive(reply, Milliseconds(100))) {
    // reply.payload.latency_ns etc.
}

// Receive any registered type via a visitor lambda
mbx.receive_any_for(Seconds(1), [&](auto&& msg) {
    using T = std::decay_t<decltype(msg)>;
    if constexpr (std::is_same_v<T, WireMessage<PingPayload>>) { ... }
    if constexpr (std::is_same_v<T, WireMessage<PongPayload>>) { ... }
});
```

### Cross-process (same machine, EVL)

```cpp
cfg.cross_process = true;   // uses POSIX SHM ring instead of heap ring
// everything else identical
```

---

## Cross-machine RT (EVL Network Mode)

Mode::Network uses the EVL out-of-band UDP stack for zero-demotion cross-machine
real-time communication — the EVL equivalent of what RACK used RTnet for on Xenomai 3.

### How it works

- Each mailbox binds an OOB UDP socket to `INADDR_ANY` on port `42000 + (mailbox_id & 0x7FFF)`
- Routing is keyed on **system_id** (top byte of mailbox_id): one IP per host, not one per mailbox
- `evl_net_solicit()` primes EVL's ARP/route front-caches at startup so every `oob_sendmsg()` stays on the OOB stage
- No router process on the RT data path

### Host setup (once per machine)

```bash
# Enable OOB port on the network interface
evl net -ei eth0

# Optional: manually solicit a peer (also done automatically at mailbox start)
evl net -Si eth0 10.10.10.11
```

For a **dedicated RT link** (one NIC per machine, direct or via switch): use `eth0` directly.
For a **shared NIC** (RT + non-RT traffic on one interface): use a VLAN device:

```bash
ip link add link eth0 name eth0.42 type vlan id 42
ip addr add 10.10.10.10/24 dev eth0.42
evl net -ei eth0.42
```

### Configuration

```cpp
MailboxConfig cfg;
cfg.mailbox_id       = 0x00012000;  // system_id=0, rest of address...
cfg.local_system_id  = 0;           // this host's system_id (= top byte of mailbox_id)
cfg.network          = true;

// Route table: one entry per remote host
cfg.network_route_count = 1;
cfg.network_routes[0] = {
    .system_id = 1,           // remote host's system_id
    .ip        = "10.10.10.11"
};

// On the remote host (system_id=1, IP 10.10.10.11):
// cfg.mailbox_id      = 0x01034000;
// cfg.local_system_id = 1;
// cfg.network_routes[0] = { .system_id = 0, .ip = "10.10.10.10" };
```

Sending to `0x01034000` from host 0 automatically routes to `10.10.10.11`, port `42000 + (0x3400 & 0x7FFF) = 55296`.

### Real-time guarantees

| NIC driver | OOB send latency | Thread stays OOB? |
|---|---|---|
| OOB-capable (adapted) | Hardware-limited, fully deterministic | Yes |
| Standard (non-adapted) | Depends on in-band kernel latency | Yes — EVL offloads TX but does not demote the caller |

With a PREEMPT_RT kernel and a standard NIC the latency is typically in the low hundreds of microseconds range. With an OOB-capable driver it is hardware-limited (sub-10 µs over a dedicated link).

---

## TCP Router

`corerat-router-tcp` is a drop-in replacement for the RACK `TimsRouterTcp` daemon.

```bash
./build/debug/corerat-router-tcp           # default port 2000
./build/debug/corerat-router-tcp --port 2001 --max-msg-size 65536
```

Nodes connect over TCP, register their mailbox IDs, and the router forwards frames by destination mailbox address. The TCP router is used by the STD (TIMS) platform and the EVL `cross_process = false` path is only local. The EVL Network mode bypasses the router entirely.

---

## Ping-Pong Test

```bash
./build/debug/corerat-router-tcp &
./build/debug/test/pong_node
./build/debug/test/ping_node
```

Or via CTest:

```bash
cd build/debug && ctest -L integration -V
```

---

## CMake Integration

```cmake
find_package(CoreRaT REQUIRED)
target_link_libraries(my_module PUBLIC CoreRaT::corerat_tims)
```

---

## EVL Testing with RaTOS

`scripts/evl-dev.sh` boots an EVL kernel in QEMU, cross-compiles, deploys, and runs the full test suite.

```bash
scripts/evl-dev.sh                          # cross-compile + deploy + run all tests
scripts/evl-dev.sh --cross evl-cross        # cross-compile only (fast CI check)
scripts/evl-dev.sh --cross --shell          # interactive shell on EVL guest
```

Pin the RaTOS release in `.corerat.env`:

```bash
RATOS_RELEASE_TAG=v0.0.7
```

---

## Credits

### RACK — Robotics Application Construction Kit

CoreRaT is **wire-compatible with RACK**, an open-source robotics middleware developed at the
**University of Hannover** (Institute for Systems Engineering - RTS, Professor Bernardo Wagner).

RACK defines the **TiMS** wire protocol — the 16-byte `tims_msg_head` frame that `WireHeader` mirrors exactly, the TCP router protocol that `corerat-router-tcp` implements, and the mailbox addressing scheme that `MessageDefinition<>` preserves.

RACK authors: Joerg Langenberg, Marko Reimer, Jan Kiszka, Oliver Wulf, Sebastian Smolorz and contributors.  
RACK source: <https://github.com/smolorz/RACK> — licensed GPL v2 (or later).

CoreRaT introduces no RACK library dependency; the wire protocol is re-implemented from scratch in C++20.

### SeRTial

Zero-allocation compile-time serialization by [@mattih11](https://github.com/mattih11).  
Source: <https://github.com/mattih11/SeRTial>

### RaTOS

Xenomai 4 / EVL kernel image and SDK used for EVL testing, by [@mattih11](https://github.com/mattih11).  
Source: <https://github.com/mattih11/RaTOS>

---

## License

GPL v3 — see LICENSE for the full text.
The RACK wire protocol constants re-implemented here are also GPL v2.

