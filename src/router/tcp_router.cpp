/**
 * @file router/tcp_router.cpp
 * @brief CoreRaT TCP message router
 *
 * Drop-in replacement for the RACK TimsRouterTcp daemon.
 * Speaks the same TiMS wire protocol on port 2000 so existing CommRaT/RACK
 * nodes interoperate without modification.
 *
 * Each connecting client gets one Connection object and one std::jthread.
 * A shared MailboxRegistry maps mailbox_id → Connection* for routing.
 *
 * Usage:
 *   corerat-router-tcp [--port PORT] [--max-msg-size BYTES]
 */

#include "corerat/ipc/tims/protocol.hpp"
#include "corerat/ipc/tims/tcp_socket.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace corerat::router {

using namespace corerat::tims_proto;

// ============================================================================
// Forward declarations
// ============================================================================

class Connection;

// ============================================================================
// MailboxRegistry — thread-safe mailbox_id → Connection mapping
// ============================================================================

class MailboxRegistry {
public:
    /// Register mailbox → connection. Returns false if already registered.
    bool add(uint32_t mailbox_id, Connection* con) {
        std::unique_lock lock{mutex_};
        return table_.emplace(mailbox_id, con).second;
    }

    /// Remove a mailbox entry (ignored if not present).
    void remove(uint32_t mailbox_id) {
        std::unique_lock lock{mutex_};
        table_.erase(mailbox_id);
    }

    /// Remove all mailboxes belonging to a connection.
    void remove_all(const Connection* con) {
        std::unique_lock lock{mutex_};
        std::erase_if(table_, [con](const auto& kv){ return kv.second == con; });
    }

    /// Look up a connection by mailbox id; returns nullptr if not found.
    Connection* find(uint32_t mailbox_id) const {
        std::shared_lock lock{mutex_};
        const auto it = table_.find(mailbox_id);
        return it != table_.end() ? it->second : nullptr;
    }

    std::size_t size() const {
        std::shared_lock lock{mutex_};
        return table_.size();
    }

private:
    mutable std::shared_mutex                   mutex_;
    std::unordered_map<uint32_t, Connection*>   table_;
};

// ============================================================================
// ServerSocket — RAII TCP accept socket
// ============================================================================

class ServerSocket {
public:
    explicit ServerSocket(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return;

        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd_, 32) < 0) {
            ::close(fd_); fd_ = -1;
        }
    }

    ~ServerSocket() { if (fd_ >= 0) ::close(fd_); }

    ServerSocket(const ServerSocket&) = delete;
    ServerSocket& operator=(const ServerSocket&) = delete;

    bool is_open() const noexcept { return fd_ >= 0; }

    /// Poll for an incoming connection with a timeout.  Returns > 0 if a
    /// connection is ready, 0 on timeout, < 0 on error.
    int poll_in(int timeout_ms) const noexcept {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        return ::poll(&pfd, 1, timeout_ms);
    }

    /// Accept next connection; non-blocking — caller must poll first.
    std::optional<TcpSocket> accept() const noexcept {
        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);
        const int   cfd = ::accept(fd_,
                                   reinterpret_cast<sockaddr*>(&client_addr),
                                   &addr_len);
        if (cfd < 0) return std::nullopt;
        TcpSocket s{cfd};
        s.disable_nagle();
        return s;
    }

    void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    int fd_{-1};
};

// ============================================================================
// Connection — one per connected client
// ============================================================================

class Connection {
public:
    Connection(TcpSocket socket,
               MailboxRegistry& registry,
               std::size_t max_msg_size,
               int index)
        : socket_(std::move(socket))
        , registry_(registry)
        , max_msg_size_(max_msg_size)
        , index_(index) {}

    ~Connection() { registry_.remove_all(this); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Forward a message to this connection's socket (thread-safe).
    bool forward(const FrameHeader& hdr, const std::byte* body,
                 uint32_t body_len) {
        std::lock_guard lock{send_mutex_};
        return socket_.send_all(&hdr, sizeof(hdr)) &&
               socket_.send_all(body, body_len);
    }

    /// Main receive loop — runs in its own jthread.
    void run(std::stop_token stop) {
        std::vector<std::byte> buf(max_msg_size_);

        while (!stop.stop_requested()) {
            if (socket_.poll_in(200) <= 0) continue;

            FrameHeader frame{};
            if (!socket_.recv_all(&frame, sizeof(frame))) break;

            handle_frame(frame, buf);
        }

        registry_.remove_all(this);
        std::printf("[TCP router] connection[%d] closed\n", index_);
    }

private:
    void handle_frame(const FrameHeader& frame, std::vector<std::byte>& buf) {
        const uint32_t body_len = (frame.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
                                ? frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader))
                                : 0u;

        if (body_len > max_msg_size_) {
            std::fprintf(stderr, "[TCP router] connection[%d] oversized message "
                     "(%u bytes) from %08x\n", index_, frame.msglen, frame.src);
            socket_.close(); return;
        }

        if (body_len > 0 && !socket_.recv_all(buf.data(), body_len)) return;

        switch (frame.type) {
            case MSG_ROUTER_DISABLE_WATCHDOG:
                watchdog_enabled_ = false;
                break;

            case MSG_ROUTER_MBX_INIT_WITH_REPLY: {
                if (body_len < sizeof(MbxInitPayload)) break;
                MbxInitPayload payload{};
                std::memcpy(&payload, buf.data(), sizeof(payload));
                const bool ok = registry_.add(payload.mbx, this);
                const auto reply = make_frame(ok ? MSG_OK : MSG_ERROR,
                                              frame.src, 0, 0, frame.seq_nr, 0);
                std::lock_guard lock{send_mutex_};
                socket_.send_all(&reply, sizeof(reply));
                if (ok) std::printf("[TCP router] registered mailbox %08x "
                                    "on connection[%d]\n", payload.mbx, index_);
                break;
            }

            case MSG_ROUTER_MBX_DELETE: {
                if (body_len < sizeof(MbxInitPayload)) break;
                MbxInitPayload payload{};
                std::memcpy(&payload, buf.data(), sizeof(payload));
                registry_.remove(payload.mbx);
                std::printf("[TCP router] deleted mailbox %08x from "
                            "connection[%d]\n", payload.mbx, index_);
                break;
            }

            default: {
                // Route to destination
                Connection* dest = registry_.find(frame.dest);
                if (dest) {
                    dest->forward(frame, buf.data(), body_len);
                } else {
                    // Unknown destination — send error back to sender
                    const auto err = make_frame(MSG_ERROR, frame.src, 0,
                                                0, frame.seq_nr, 0);
                    std::lock_guard lock{send_mutex_};
                    socket_.send_all(&err, sizeof(err));
                }
                break;
            }
        }
    }

    TcpSocket        socket_;
    MailboxRegistry& registry_;
    std::size_t      max_msg_size_;
    int              index_;
    std::mutex       send_mutex_;
    bool             watchdog_enabled_{true};
};

// ============================================================================
// TcpRouter — accept loop + connection lifetime management
// ============================================================================

class TcpRouter {
public:
    TcpRouter(uint16_t port, std::size_t max_msg_size)
        : server_(port), max_msg_size_(max_msg_size), port_(port) {}

    bool is_ready() const noexcept { return server_.is_open(); }

    /// Close the server socket so a blocked accept() call returns immediately.
    /// Call this after request_stop() to ensure the run() thread exits promptly.
    void stop() { server_.close(); }

    void run(std::stop_token stop) {
        std::printf("[TCP router] listening on port %u\n",
                    static_cast<unsigned>(port_));

        while (!stop.stop_requested()) {
            // Poll with 200 ms timeout so we can check stop_requested regularly
            // without relying on close() to interrupt a blocked accept() call.
            if (server_.poll_in(200) <= 0) continue;

            auto sock = server_.accept();
            if (!sock) continue;   // spurious wakeup or transient error

            const int idx = next_index_++;
            std::printf("[TCP router] new connection[%d]\n", idx);

            auto con = std::make_unique<Connection>(
                std::move(*sock), registry_, max_msg_size_, idx);

            // Launch per-connection jthread; holds a raw pointer safe because
            // we join before removing from connections_
            Connection* raw = con.get();
            auto thread = std::jthread{[raw](std::stop_token st){
                raw->run(st);
            }};

            std::lock_guard lock{connections_mutex_};
            connections_.emplace_back(std::move(con), std::move(thread));
        }

        std::printf("[TCP router] shutting down\n");
        server_.close();
    }

    static constexpr uint16_t kDefaultPort = 2000;

private:
    struct Entry {
        std::unique_ptr<Connection> con;
        std::jthread                thread;
    };

    ServerSocket             server_;
    MailboxRegistry          registry_;
    std::size_t              max_msg_size_;
    uint16_t                 port_;
    std::atomic<int>         next_index_{0};
    std::mutex               connections_mutex_;
    std::vector<Entry>       connections_;
};

// ============================================================================
// MSG_ERROR constant (not in protocol.hpp — router-only)
// ============================================================================

}  // namespace corerat::router

// ============================================================================
// main
// ============================================================================

namespace {
    std::atomic<bool> g_shutdown{false};
}

int main(int argc, char* argv[]) {
    uint16_t    port         = corerat::router::TcpRouter::kDefaultPort;
    std::size_t max_msg_size = 256 * 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if ((arg == "--max-msg-size" || arg == "-m") && i + 1 < argc)
            max_msg_size = std::stoul(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: corerat-router-tcp [--port PORT] [--max-msg-size BYTES]\n");
                         
            return 0;
        }
    }

    ::signal(SIGPIPE, SIG_IGN);

    corerat::router::TcpRouter router{port, max_msg_size};
    if (!router.is_ready()) {
        std::fprintf(stderr, "Failed to bind port %u\n", static_cast<unsigned>(port));
        return 1;
    }

    std::stop_source stop;
    ::signal(SIGINT,  [](int){ g_shutdown = true; });
    ::signal(SIGTERM, [](int){ g_shutdown = true; });

    std::jthread router_thread{[&router, &stop]{
        router.run(stop.get_token());
    }};

    while (!g_shutdown) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.request_stop();
    router.stop();   // close server socket so accept() unblocks immediately
    return 0;
}
