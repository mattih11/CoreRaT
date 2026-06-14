/**
 * @file ping_pong/ping_node.cpp
 * @brief Ping node — sends N pings, measures round-trip time.
 *
 * Uses Mailbox<PingDef, PongDef> so the same source compiles for both backends:
 *   CORERAT_IPC_TIMS — connects to corerat-router-tcp on port 2000
 *   CORERAT_IPC_EVL  — uses cross-process EvlMailbox (PUBLIC mode, no router)
 *
 * Transport is selected at cmake configure time via CORERAT_IPC.
 *
 * Usage: ping_node [--count N] [--port PORT]
 * Launched by scripts/run_pingpong_test.sh (or manually).
 */

#include "messages.hpp"
#include "corerat/platform/duration.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

using namespace corerat;
using namespace corerat::pingpong;

static uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char* argv[]) {
    int count = 100;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
        // --port is accepted but ignored on EVL (no router); kept for script compat
    }

    std::printf("ping_node: %d ping(s) [IPC=%s]\n",
#ifdef CORERAT_IPC_EVL
                count, "EVL");
#else
                count, "TIMS/TCP");
#endif

    MailboxConfig cfg;
    cfg.mailbox_id     = PING_MBX_ID;
    cfg.max_message_size = kMsgBufSize;
    cfg.cross_process  = true;   // EVL: Mode::Public; TIMS: ignored (TCP router)

    PingPongMailbox mbx{cfg};
    if (!mbx.start()) {
        std::fprintf(stderr, "ping_node FAIL: could not start mailbox 0x%04x\n", PING_MBX_ID);
        return 1;
    }
    std::printf("ping_node: registered mailbox 0x%04x\n", PING_MBX_ID);

    // Give pong_node time to register its mailbox
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<uint64_t> rtt_ns(count, 0);
    int received = 0;

    for (int seq = 0; seq < count; ++seq) {
        const uint64_t send_t = now_ns();

        WireMessage<PingPayload> ping;
        ping.payload = PingPayload{ send_t, static_cast<uint32_t>(seq) };

        if (!mbx.send(ping, PONG_MBX_ID)) {
            std::fprintf(stderr, "ping_node FAIL: send ping %d\n", seq);
            break;
        }

        WireMessage<PongPayload> pong;
        if (!mbx.receive(pong, Milliseconds(2000))) {
            std::fprintf(stderr, "ping_node FAIL: timeout on pong %d\n", seq);
            break;
        }

        rtt_ns[received++] = now_ns() - send_t;
    }

    if (received == 0) {
        std::fprintf(stderr, "ping_node FAIL: no replies received\n");
        return 1;
    }

    const auto [lo, hi] = std::minmax_element(rtt_ns.begin(), rtt_ns.begin() + received);
    uint64_t sum = 0;
    for (int i = 0; i < received; ++i) sum += rtt_ns[i];
    const double avg_us = static_cast<double>(sum) / received / 1000.0;

    std::printf("ping_node: %d/%d replies  RTT min/avg/max = %.1f / %.1f / %.1f µs\n",
                received, count,
                static_cast<double>(*lo) / 1000.0, avg_us,
                static_cast<double>(*hi) / 1000.0);

    return (received == count) ? 0 : 1;
}
