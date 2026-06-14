/**
 * @file ping_pong/evl_pingpong.cpp
 * @brief Single-binary EVL ping-pong latency benchmark.
 *
 * Runs ping and pong in two std::jthreads within the same process.
 * Uses EvlMailbox in LOCAL mode — no router, no SHM, no cross-process
 * coordination required.
 *
 * On CORERAT_PLATFORM_EVL the ring uses EVL OOB mutex/event so the send and
 * receive paths stay out-of-band.  This measures raw EVL ring-buffer RTT.
 *
 * Only built when CORERAT_PLATFORM=EVL (gated in test/CMakeLists.txt).
 *
 * Usage: evl_pingpong [--count N]
 */

#include "messages.hpp"
#include "corerat/platform/duration.hpp"

#include <algorithm>
#include <atomic>
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

// Pong thread: receives PingPayload, sends PongPayload back.
static void pong_loop(std::stop_token stop, int count) {
    MailboxConfig cfg;
    cfg.mailbox_id     = PONG_MBX_ID;
    cfg.max_message_size = kMsgBufSize;
    cfg.cross_process  = false;  // LOCAL mode — heap ring, same process

    PingPongMailbox mbx{cfg};
    if (!mbx.start()) {
        std::fprintf(stderr, "pong FAIL: could not start mailbox\n");
        return;
    }

    int received = 0;
    while (!stop.stop_requested() && received < count) {
        mbx.receive_any_for(Milliseconds(100), [&](auto&& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, WireMessage<PingPayload>>) {
                WireMessage<PongPayload> reply;
                reply.payload = PongPayload{
                    msg.payload.send_ns,
                    now_ns(),
                    msg.payload.seq
                };
                mbx.send(reply, PING_MBX_ID);
                ++received;
            }
        });
    }
    std::printf("pong: replied to %d/%d pings\n", received, count);
}

int main(int argc, char* argv[]) {
    int count = 100;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
    }

    std::printf("evl_pingpong: %d ping(s), LOCAL mode [IPC=%s]\n",
#ifdef CORERAT_IPC_EVL
                count, "EVL OOB ring");
#else
                count, "N/A — EVL only");
    (void)count;
    std::fprintf(stderr, "evl_pingpong: only meaningful with CORERAT_IPC_EVL\n");
    return 1;
#endif

    // Launch pong thread first so its mailbox is registered before ping sends
    std::jthread pt{pong_loop, count};
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Ping mailbox (main thread)
    MailboxConfig cfg;
    cfg.mailbox_id     = PING_MBX_ID;
    cfg.max_message_size = kMsgBufSize;
    cfg.cross_process  = false;

    PingPongMailbox mbx{cfg};
    if (!mbx.start()) {
        std::fprintf(stderr, "ping FAIL: could not start mailbox\n");
        return 1;
    }

    std::vector<uint64_t> rtt_ns(count, 0);
    int received = 0;

    for (int seq = 0; seq < count; ++seq) {
        const uint64_t send_t = now_ns();

        WireMessage<PingPayload> ping;
        ping.payload = PingPayload{ send_t, static_cast<uint32_t>(seq) };

        if (!mbx.send(ping, PONG_MBX_ID)) {
            std::fprintf(stderr, "ping FAIL: send %d\n", seq);
            break;
        }

        WireMessage<PongPayload> pong;
        if (!mbx.receive(pong, Milliseconds(2000))) {
            std::fprintf(stderr, "ping FAIL: timeout on %d\n", seq);
            break;
        }

        rtt_ns[received++] = now_ns() - send_t;
    }

    pt.request_stop();

    if (received == 0) {
        std::fprintf(stderr, "evl_pingpong FAIL: no replies\n");
        return 1;
    }

    const auto [lo, hi] = std::minmax_element(rtt_ns.begin(), rtt_ns.begin() + received);
    uint64_t sum = 0;
    for (int i = 0; i < received; ++i) sum += rtt_ns[i];
    const double avg_us = static_cast<double>(sum) / received / 1000.0;

    std::printf("evl_pingpong: %d/%d replies  RTT min/avg/max = %.1f / %.1f / %.1f µs\n",
                received, count,
                static_cast<double>(*lo) / 1000.0, avg_us,
                static_cast<double>(*hi) / 1000.0);

    return (received == count) ? 0 : 1;
}
