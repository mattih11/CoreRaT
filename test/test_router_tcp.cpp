/**
 * @file test_router_tcp.cpp
 * @brief Smoke test for corerat-router-tcp
 *
 * Connects to a running corerat-router-tcp instance on 127.0.0.1:2000,
 * registers a mailbox, and verifies the MSG_OK reply.
 * Launched by run_router_test.cmake which starts/stops the router process.
 */

#include "corerat/ipc/tims/protocol.hpp"
#include "corerat/ipc/tims/tcp_socket.hpp"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

using namespace corerat;
using namespace corerat::tims_proto;

static bool check(bool cond, const char* msg) {
    if (!cond) std::printf("FAIL: %s\n", msg);
    return cond;
}

int main() {
    std::printf("=== test_router_tcp ===\n");
    bool all_pass = true;

    // ------------------------------------------------------------------
    // 1. Connect
    // ------------------------------------------------------------------
    TcpSocket sock;
    // Router may take a moment to start; retry a few times
    bool connected = false;
    for (int attempt = 0; attempt < 10 && !connected; ++attempt) {
        connected = sock.connect("127.0.0.1", 2000);
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    all_pass &= check(connected, "connect to router");
    if (!connected) { std::printf("FAILED\n"); return 1; }

    // ------------------------------------------------------------------
    // 2. Disable watchdog
    // ------------------------------------------------------------------
    {
        const auto h = make_frame(MSG_ROUTER_DISABLE_WATCHDOG, 0, 0x1001, 0, 0, 0);
        all_pass &= check(sock.send_all(&h, sizeof(h)), "send DISABLE_WATCHDOG");
    }

    // ------------------------------------------------------------------
    // 3. Register mailbox 0x1001, expect MSG_OK
    // ------------------------------------------------------------------
    {
        struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
            make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, 0x1001, 0, 1,
                       static_cast<uint32_t>(sizeof(MbxInitPayload))),
            { 0x1001 }
        };
        all_pass &= check(sock.send_all(&reg, sizeof(reg)), "send MBX_INIT");

        FrameHeader reply{};
        all_pass &= check(sock.recv_all(&reply, sizeof(reply)), "recv reply");
        all_pass &= check(reply.type == MSG_OK, "reply is MSG_OK");
        std::printf("  reply.type = %d (expected %d)\n",
                    static_cast<int>(reply.type), static_cast<int>(MSG_OK));
    }

    // ------------------------------------------------------------------
    // 4. Register mailbox 0x1002, expect MSG_OK
    // ------------------------------------------------------------------
    {
        struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
            make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, 0x1002, 0, 2,
                       static_cast<uint32_t>(sizeof(MbxInitPayload))),
            { 0x1002 }
        };
        all_pass &= check(sock.send_all(&reg, sizeof(reg)), "send MBX_INIT 2");

        FrameHeader reply{};
        all_pass &= check(sock.recv_all(&reply, sizeof(reply)), "recv reply 2");
        all_pass &= check(reply.type == MSG_OK, "reply 2 is MSG_OK");
    }

    // ------------------------------------------------------------------
    // 5. Send a message from 0x1001 → 0x1002 (loopback via router)
    //    We need a second connection for the receiver side.
    // ------------------------------------------------------------------
    TcpSocket rx;
    all_pass &= check(rx.connect("127.0.0.1", 2000), "connect rx socket");
    if (rx.is_open()) {
        // Register rx mailbox 0x1002 on the rx connection
        struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
            make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, 0x2002, 0, 3,
                       static_cast<uint32_t>(sizeof(MbxInitPayload))),
            { 0x2002 }
        };
        rx.send_all(&reg, sizeof(reg));
        FrameHeader ack{};
        rx.recv_all(&ack, sizeof(ack));

        // Send a tiny payload from 0x1001 → 0x2002
        const uint32_t payload = 0xDEADBEEF;
        const auto h = make_frame(0, 0x2002, 0x1001, 0, 4,
                                  static_cast<uint32_t>(sizeof(payload)));
        sock.send_all(&h, sizeof(h));
        sock.send_all(&payload, sizeof(payload));

        // Receive on rx
        FrameHeader fwd{};
        all_pass &= check(rx.recv_all(&fwd, sizeof(fwd)), "recv forwarded frame header");
        all_pass &= check(fwd.src == 0x1001, "forwarded src is 0x1001");
        all_pass &= check(fwd.dest == 0x2002, "forwarded dest is 0x2002");

        uint32_t fwd_payload = 0;
        all_pass &= check(rx.recv_all(&fwd_payload, sizeof(fwd_payload)),
                         "recv forwarded payload");
        all_pass &= check(fwd_payload == 0xDEADBEEF, "forwarded payload matches");
        std::printf("  forwarded payload = 0x%08X (expected 0xDEADBEEF)\n",
                    fwd_payload);
    }

    std::printf("\n%s\n", all_pass ? "ALL PASS" : "SOME TESTS FAILED");
    return all_pass ? 0 : 1;
}
