/**
 * @file ping_pong/pong_node.cpp
 * @brief Pong node — receives PingPayload and replies with PongPayload.
 *
 * Uses Mailbox<PingDef, PongDef> so the same source compiles for both backends:
 *   CORERAT_IPC_TIMS — connects to corerat-router-tcp on port 2000
 *   CORERAT_IPC_EVL  — uses cross-process EvlMailbox (PUBLIC mode, no router)
 *
 * Usage: pong_node [--count N] [--port PORT]
 * Launched by scripts/run_pingpong_test.sh (or manually).
 */

#include "messages.hpp"
#include "corerat/platform/duration.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>

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
    }

    std::printf("pong_node: ready for %d ping(s) [IPC=%s]\n",
#ifdef CORERAT_IPC_EVL
                count, "EVL");
#else
                count, "TIMS/TCP");
#endif

    MailboxConfig cfg;
    cfg.mailbox_id     = PONG_MBX_ID;
    cfg.max_message_size = kMsgBufSize;
    cfg.cross_process  = true;

    PingPongMailbox mbx{cfg};
    if (!mbx.start()) {
        std::fprintf(stderr, "pong_node FAIL: could not start mailbox 0x%04x\n", PONG_MBX_ID);
        return 1;
    }
    std::printf("pong_node: registered mailbox 0x%04x\n", PONG_MBX_ID);

    int ponged = 0;
    while (ponged < count) {
        // 5 s timeout per ping — gives ping_node time to start
        const auto result = mbx.receive_any_for(Seconds(5), [&](auto&& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, WireMessage<PingPayload>>) {
                WireMessage<PongPayload> reply;
                reply.payload = PongPayload{
                    msg.payload.send_ns,
                    now_ns(),
                    msg.payload.seq
                };
                // Reply to sender's mailbox (src is filled in by the transport)
                mbx.send(reply, msg.header.src);
                ++ponged;
            }
        });

        if (!result && result.get_error() == MailboxError::Timeout) {
            std::fprintf(stderr, "pong_node FAIL: timeout waiting for ping %d\n", ponged);
            break;
        }
    }

    std::printf("pong_node: replied to %d/%d pings\n", ponged, count);
    return (ponged == count) ? 0 : 1;
}
