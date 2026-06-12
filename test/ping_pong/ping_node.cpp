/**
 * @file ping_pong/ping_node.cpp
 * @brief Ping node — sends N MSG_PING frames to pong_node, measures RTT.
 *
 * Connects to corerat-router-tcp on 127.0.0.1:2000, registers mailbox
 * PING_MBX_ID (0x3001), sends --count pings to PONG_MBX_ID (0x3002),
 * and waits for MSG_PONG replies.
 *
 * Prints min/avg/max RTT in microseconds.
 * Exit code: 0 if all replies received, 1 otherwise.
 *
 * Usage: ping_node [--count N] [--port PORT]
 *
 * Launched by scripts/run_pingpong_test.sh (or manually).
 */

#include "messages.hpp"
#include "corerat/ipc/tims/protocol.hpp"
#include "corerat/ipc/tims/tcp_socket.hpp"

#include <algorithm>
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

    std::printf("ping_node: sending %d ping(s) via corerat-router-tcp port %u\n",
                count, port);

    // Connect (retry while router/pong_node start up)
    TcpSocket sock;
    bool connected = false;
    for (int attempt = 0; attempt < 30 && !connected; ++attempt) {
        connected = sock.connect("127.0.0.1", port);
        if (!connected)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
        std::fprintf(stderr, "ping_node FAIL: could not connect to router on port %u\n", port);
        return 1;
    }

    // Disable watchdog
    {
        const auto h = make_frame(MSG_ROUTER_DISABLE_WATCHDOG, 0, PING_MBX_ID, 0, 0, 0);
        if (!sock.send_all(&h, sizeof(h))) {
            std::fprintf(stderr, "ping_node FAIL: send watchdog\n"); return 1;
        }
    }

    // Register mailbox PING_MBX_ID
    {
        struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
            make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, PING_MBX_ID, 0, 0,
                       static_cast<uint32_t>(sizeof(MbxInitPayload))),
            { PING_MBX_ID }
        };
        if (!sock.send_all(&reg, sizeof(reg))) {
            std::fprintf(stderr, "ping_node FAIL: send register\n"); return 1;
        }
        FrameHeader reply{};
        if (!sock.recv_all(&reply, sizeof(reply)) || reply.type != MSG_OK) {
            std::fprintf(stderr, "ping_node FAIL: register NACK (type=%d)\n",
                         static_cast<int>(reply.type));
            return 1;
        }
    }
    std::printf("ping_node: registered mailbox 0x%04x\n", PING_MBX_ID);

    // Give pong_node time to register its mailbox if it started simultaneously
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<uint64_t> rtt_ns(count, 0);
    int received = 0;

    for (int seq = 0; seq < count; ++seq) {
        const uint64_t send = now_ns();

        // Build and send ping frame
        struct __attribute__((packed)) {
            FrameHeader  h;
            PingPayload  p;
        } ping_msg{
            make_frame(MSG_PING, PONG_MBX_ID, PING_MBX_ID, 0,
                       static_cast<uint8_t>(seq & 0xFF),
                       static_cast<uint32_t>(sizeof(PingPayload))),
            { send, static_cast<uint32_t>(seq) }
        };
        if (!sock.send_all(&ping_msg, sizeof(ping_msg))) {
            std::fprintf(stderr, "ping_node FAIL: send ping %d\n", seq);
            break;
        }

        // Wait for pong reply (2 s timeout per ping)
        if (sock.poll_in(2000) <= 0) {
            std::fprintf(stderr, "ping_node FAIL: timeout waiting for pong %d\n", seq);
            break;
        }

        FrameHeader reply{};
        if (!sock.recv_all(&reply, sizeof(reply))) break;

        // Drain body (read as much as fits in PongPayload; discard excess)
        const uint32_t body = (reply.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
                            ? reply.msglen - static_cast<uint32_t>(sizeof(FrameHeader))
                            : 0u;
        PongPayload pong{};
        const uint32_t to_read = std::min(body, static_cast<uint32_t>(sizeof(pong)));
        if (to_read > 0 && !sock.recv_all(&pong, to_read)) break;
        if (body > to_read) {
            std::vector<char> trash(body - to_read);
            if (!sock.recv_all(trash.data(), trash.size())) break;
        }

        rtt_ns[received++] = now_ns() - send;
    }

    if (received == 0) {
        std::fprintf(stderr, "ping_node FAIL: no replies received\n");
        return 1;
    }

    const auto [lo, hi] = std::minmax_element(rtt_ns.begin(), rtt_ns.begin() + received);
    uint64_t sum = 0;
    for (int i = 0; i < received; ++i) sum += rtt_ns[i];
    const double avg_us = static_cast<double>(sum) / received / 1000.0;
    const double min_us = static_cast<double>(*lo) / 1000.0;
    const double max_us = static_cast<double>(*hi) / 1000.0;

    std::printf("ping_node: %d/%d replies  RTT min/avg/max = %.1f / %.1f / %.1f µs\n",
                received, count, min_us, avg_us, max_us);

    return (received == count) ? 0 : 1;
}
