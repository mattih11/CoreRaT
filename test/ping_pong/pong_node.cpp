/**
 * @file ping_pong/pong_node.cpp
 * @brief Pong node — receives MSG_PING frames and sends MSG_PONG replies.
 *
 * Connects to corerat-router-tcp on 127.0.0.1:2000, registers mailbox
 * PONG_MBX_ID (0x3002), and replies to every MSG_PING with a MSG_PONG
 * containing the echo timestamp plus its own send timestamp.
 *
 * Exits with code 0 after receiving --count pings, or 1 on error/timeout.
 *
 * Usage: pong_node [--count N] [--port PORT]
 *
 * Launched by scripts/run_pingpong_test.sh (or manually).
 */

#include "messages.hpp"
#include "corerat/ipc/tims/protocol.hpp"
#include "corerat/ipc/tims/tcp_socket.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

using namespace corerat;
using namespace corerat::tims_proto;
using namespace corerat::pingpong;

static uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char* argv[]) {
    int      count = 100;
    uint16_t port  = 2000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--count" && i + 1 < argc) count = std::atoi(argv[++i]);
        if (arg == "--port"  && i + 1 < argc) port  = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    std::printf("pong_node: ready to receive %d ping(s) on port %u\n", count, port);

    // Connect (retry while router starts up)
    TcpSocket sock;
    bool connected = false;
    for (int attempt = 0; attempt < 30 && !connected; ++attempt) {
        connected = sock.connect("127.0.0.1", port);
        if (!connected)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
        std::fprintf(stderr, "pong_node FAIL: could not connect to router on port %u\n", port);
        return 1;
    }

    // Disable watchdog
    {
        const auto h = make_frame(MSG_ROUTER_DISABLE_WATCHDOG, 0, PONG_MBX_ID, 0, 0, 0);
        if (!sock.send_all(&h, sizeof(h))) {
            std::fprintf(stderr, "pong_node FAIL: send watchdog\n"); return 1;
        }
    }

    // Register mailbox PONG_MBX_ID
    {
        struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
            make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, PONG_MBX_ID, 0, 0,
                       static_cast<uint32_t>(sizeof(MbxInitPayload))),
            { PONG_MBX_ID }
        };
        if (!sock.send_all(&reg, sizeof(reg))) {
            std::fprintf(stderr, "pong_node FAIL: send register\n"); return 1;
        }
        FrameHeader reply{};
        if (!sock.recv_all(&reply, sizeof(reply)) || reply.type != MSG_OK) {
            std::fprintf(stderr, "pong_node FAIL: register NACK (type=%d)\n",
                         static_cast<int>(reply.type));
            return 1;
        }
    }
    std::printf("pong_node: registered mailbox 0x%04x\n", PONG_MBX_ID);

    int ponged = 0;
    std::vector<char> drain_buf;

    while (ponged < count) {
        // 5 s timeout per ping — gives ping_node time to start
        if (sock.poll_in(5000) <= 0) {
            std::fprintf(stderr, "pong_node FAIL: timeout waiting for ping %d\n", ponged);
            return 1;
        }

        FrameHeader frame{};
        if (!sock.recv_all(&frame, sizeof(frame))) break;

        const uint32_t body = (frame.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
                            ? frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader))
                            : 0u;

        if (frame.type == MSG_PING) {
            // Read ping payload
            PingPayload ping{};
            const uint32_t to_read = std::min(body, static_cast<uint32_t>(sizeof(ping)));
            if (to_read > 0 && !sock.recv_all(&ping, to_read)) break;
            if (body > to_read) {
                drain_buf.resize(body - to_read);
                if (!sock.recv_all(drain_buf.data(), drain_buf.size())) break;
            }

            // Send pong back to the sender's mailbox
            struct __attribute__((packed)) {
                FrameHeader h;
                PongPayload p;
            } pong_msg{
                make_frame(MSG_PONG, frame.src, PONG_MBX_ID, 0,
                           frame.seq_nr,
                           static_cast<uint32_t>(sizeof(PongPayload))),
                { ping.send_ns, now_ns(), ping.seq }
            };
            if (!sock.send_all(&pong_msg, sizeof(pong_msg))) break;
            ++ponged;
        } else {
            // Drain body of any unexpected frame type
            if (body > 0) {
                drain_buf.resize(body);
                if (!sock.recv_all(drain_buf.data(), body)) break;
            }
        }
    }

    std::printf("pong_node: replied to %d/%d pings\n", ponged, count);
    return (ponged == count) ? 0 : 1;
}
