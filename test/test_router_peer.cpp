/**
 * @file test_router_peer.cpp
 * @brief Integration test for inter-router TCP peering (Gap 5)
 *
 * Two corerat-router-tcp instances run on different ports (2000, 2001).
 * They connect as peers via --peer. This test verifies:
 *
 *   1. A mailbox registered on router A is discoverable (via PEER_REGISTER)
 *      by router B so messages sent via B are forwarded to A.
 *   2. A mailbox registered on router B is discoverable by router A so messages
 *      sent via A are forwarded to B.
 *   3. Deleting a mailbox (MSG_ROUTER_MBX_DELETE) removes it from the peer's
 *      registry (PEER_DELETE), and subsequent sends receive no forwarding.
 *
 * Invoked by run_router_peer_test.sh which starts both routers before this
 * binary runs.
 *
 * Ports: router A = 2000, router B = 2001.
 */

#include "corerat/ipc/tims/protocol.hpp"
#include "corerat/ipc/tims/tcp_socket.hpp"
#include <corerat/logging/logging.hpp>
#include <chrono>
#include <cstring>
#include <thread>

using namespace corerat;
using namespace corerat::tims_proto;

static corerat::TerminalSink g_sink{};
static corerat::RtLogger<64> g_logger{0x00000005u, corerat::LogLevel::Trace};

static bool ok(bool cond, const char* msg) {
    if (!cond) RTLOG_ERROR(g_logger) << "FAIL: " << msg;
    return cond;
}

static TcpSocket connect_with_retry(const char* host, uint16_t port,
                                    int attempts = 20) {
    TcpSocket s;
    for (int i = 0; i < attempts && !s.is_open(); ++i) {
        s.connect(host, port);
        if (!s.is_open())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return s;
}

static bool send_register(TcpSocket& sock, uint32_t mbx_id) {
    struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } reg{
        make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY, 0, mbx_id, 0, 0,
                   static_cast<uint32_t>(sizeof(MbxInitPayload))),
        { mbx_id }
    };
    if (!sock.send_all(&reg, sizeof(reg))) return false;
    FrameHeader reply{};
    if (!sock.recv_all(&reply, sizeof(reply))) return false;
    return reply.type == MSG_OK;
}

static bool send_delete(TcpSocket& sock, uint32_t mbx_id) {
    struct __attribute__((packed)) { FrameHeader frame; MbxInitPayload mbx; } del{
        make_frame(MSG_ROUTER_MBX_DELETE, 0, mbx_id, 0, 0,
                   static_cast<uint32_t>(sizeof(MbxInitPayload))),
        { mbx_id }
    };
    return sock.send_all(&del, sizeof(del));
}

int main() {
    g_logger.add_sink(&g_sink);
    g_logger.start_drain();

    RTLOG_INFO(g_logger) << "=== test_router_peer ===";
    bool all_pass = true;

    // ── Connect to both routers ───────────────────────────────────────────
    TcpSocket a = connect_with_retry("127.0.0.1", 2000);
    all_pass &= ok(a.is_open(), "connect to router A (port 2000)");

    TcpSocket b = connect_with_retry("127.0.0.1", 2001);
    all_pass &= ok(b.is_open(), "connect to router B (port 2001)");

    if (!a.is_open() || !b.is_open()) {
        RTLOG_FATAL(g_logger) << "FAILED — cannot connect to routers";
        g_logger.stop_drain();
        return 1;
    }

    // Disable watchdog on both
    {
        const auto h = make_frame(MSG_ROUTER_DISABLE_WATCHDOG, 0, 0xA001, 0, 0, 0);
        a.send_all(&h, sizeof(h));
    }
    {
        const auto h = make_frame(MSG_ROUTER_DISABLE_WATCHDOG, 0, 0xB001, 0, 0, 0);
        b.send_all(&h, sizeof(h));
    }

    // ── Test 1: A→B forwarding ────────────────────────────────────────────
    // Register 0xA001 on router A, register 0xB001 on router B.
    // Send from A to 0xB001 — router A must forward to router B.
    RTLOG_INFO(g_logger) << "[1] Cross-router forwarding A->B";

    all_pass &= ok(send_register(a, 0xA001), "register 0xA001 on A");
    all_pass &= ok(send_register(b, 0xB001), "register 0xB001 on B");

    // Give peering advertisement time to propagate (PEER_REGISTER exchange)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Open a second connection to B to act as the receive socket for 0xB001
    TcpSocket b_rx = connect_with_retry("127.0.0.1", 2001);
    all_pass &= ok(b_rx.is_open(), "connect rx to router B");
    if (b_rx.is_open()) {
        all_pass &= ok(send_register(b_rx, 0xB002), "register 0xB002 on B (rx socket)");
    }

    // Actually: the message receiver is the socket that registered 0xB001.
    // That is 'b'. Send from a connection on A targeting 0xB001.
    TcpSocket a_tx = connect_with_retry("127.0.0.1", 2000);
    all_pass &= ok(a_tx.is_open(), "connect tx to router A");
    if (a_tx.is_open()) {
        all_pass &= ok(send_register(a_tx, 0xA002), "register 0xA002 on A (tx socket)");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const uint32_t payload = 0xCAFEBABE;
        const auto h = make_frame(0, 0xB001, 0xA002, 0, 1,
                                  static_cast<uint32_t>(sizeof(payload)));
        all_pass &= ok(a_tx.send_all(&h, sizeof(h)), "send data A→B");
        all_pass &= ok(a_tx.send_all(&payload, sizeof(payload)), "send payload A→B");

        // 'b' (which registered 0xB001) should receive the forwarded frame
        if (b.poll_in(2000) > 0) {
            FrameHeader fwd{};
            all_pass &= ok(b.recv_all(&fwd, sizeof(fwd)), "recv forwarded header on B");
            all_pass &= ok(fwd.dest == 0xB001, "forwarded dest == 0xB001");
            all_pass &= ok(fwd.src  == 0xA002, "forwarded src  == 0xA002");

            uint32_t fwd_payload = 0;
            const uint32_t body_len = fwd.msglen > sizeof(FrameHeader)
                ? fwd.msglen - static_cast<uint32_t>(sizeof(FrameHeader)) : 0u;
            if (body_len == sizeof(uint32_t)) {
                b.recv_all(&fwd_payload, sizeof(fwd_payload));
                all_pass &= ok(fwd_payload == 0xCAFEBABE, "forwarded payload matches");
                RTLOG_INFO(g_logger) << "  fwd_payload = " << corerat::RtHex32{fwd_payload} << " (expected 0xCAFEBABE)";
            } else {
                all_pass &= ok(false, "unexpected body_len for forwarded frame");
            }
        } else {
            all_pass &= ok(false, "timeout waiting for forwarded message on B");
        }
    }

    // ── Test 2: B→A forwarding ────────────────────────────────────────────
    RTLOG_INFO(g_logger) << "[2] Cross-router forwarding B->A";

    TcpSocket b_tx = connect_with_retry("127.0.0.1", 2001);
    all_pass &= ok(b_tx.is_open(), "connect tx to router B");
    if (b_tx.is_open()) {
        all_pass &= ok(send_register(b_tx, 0xB003), "register 0xB003 on B (tx2)");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const uint32_t payload2 = 0x12345678;
        const auto h2 = make_frame(0, 0xA001, 0xB003, 0, 2,
                                   static_cast<uint32_t>(sizeof(payload2)));
        all_pass &= ok(b_tx.send_all(&h2, sizeof(h2)), "send data B→A");
        all_pass &= ok(b_tx.send_all(&payload2, sizeof(payload2)), "send payload B→A");

        // 'a' (which registered 0xA001) should receive the forwarded frame
        if (a.poll_in(2000) > 0) {
            FrameHeader fwd2{};
            all_pass &= ok(a.recv_all(&fwd2, sizeof(fwd2)), "recv forwarded header on A");
            all_pass &= ok(fwd2.dest == 0xA001, "forwarded dest == 0xA001");
            all_pass &= ok(fwd2.src  == 0xB003, "forwarded src  == 0xB003");

            uint32_t fwd_payload2 = 0;
            const uint32_t body_len2 = fwd2.msglen > sizeof(FrameHeader)
                ? fwd2.msglen - static_cast<uint32_t>(sizeof(FrameHeader)) : 0u;
            if (body_len2 == sizeof(uint32_t)) {
                a.recv_all(&fwd_payload2, sizeof(fwd_payload2));
                all_pass &= ok(fwd_payload2 == 0x12345678, "forwarded payload2 matches");
                RTLOG_INFO(g_logger) << "  fwd_payload2 = " << corerat::RtHex32{fwd_payload2} << " (expected 0x12345678)";
            } else {
                all_pass &= ok(false, "unexpected body_len for forwarded frame 2");
            }
        } else {
            all_pass &= ok(false, "timeout waiting for forwarded message on A");
        }
    }

    // ── Test 3: PEER_DELETE propagation ──────────────────────────────────
    RTLOG_INFO(g_logger) << "[3] Mailbox delete propagates across peer";

    // Register 0xA003 on A, wait for peer advertisement, delete it, verify
    // sending to it from B gives no forwarded frame (times out).
    all_pass &= ok(send_register(a, 0xA003), "register 0xA003 on A");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    all_pass &= ok(send_delete(a, 0xA003), "delete 0xA003 on A");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (b_tx.is_open()) {
        const uint32_t payload3 = 0xDEAD0003;
        const auto h3 = make_frame(0, 0xA003, 0xB003, 0, 3,
                                   static_cast<uint32_t>(sizeof(payload3)));
        b_tx.send_all(&h3, sizeof(h3));
        b_tx.send_all(&payload3, sizeof(payload3));

        // 'a' should NOT receive anything — 0xA003 was deleted.
        // poll_in with 300ms timeout; expect timeout.
        const bool no_spurious = (a.poll_in(300) <= 0);
        all_pass &= ok(no_spurious, "no forwarded frame after delete (expected timeout)");
        if (no_spurious) RTLOG_INFO(g_logger) << "  correctly timed out -- 0xA003 not found";
    }

    RTLOG_INFO(g_logger) << (all_pass ? "ALL PASS" : "SOME TESTS FAILED");
    g_logger.stop_drain();
    return all_pass ? 0 : 1;
}
