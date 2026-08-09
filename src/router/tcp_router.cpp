/**
 * @file router/tcp_router.cpp
 * @brief CoreRaT TCP message router with inter-router peering and upstream forwarding
 *
 * Drop-in replacement for the RACK TimsRouterTcp daemon.
 * Speaks the same TiMS wire protocol on port 2000 so existing CommRaT/RACK
 * nodes interoperate without modification.
 *
 * Inter-router peering: use --peer HOST[:PORT] to connect to a peer router
 * (corerat-router-evl on an EVL host, or another corerat-router-tcp instance).
 * Peers exchange mailbox registrations using MSG_ROUTER_PEER_HELLO/REGISTER/DELETE.
 *
 * Upstream router (Gap 1): use --upstream HOST[:PORT] to connect this router
 * as a leaf to a parent router (matching RACK's Router -> System Router link).
 * Unknown destinations are forwarded upstream rather than answered with MSG_ERROR.
 *
 * Usage:
 *   corerat-router-tcp [--port PORT] [--max-msg-size BYTES]
 *                      [--peer HOST[:PORT]] ...
 *                      [--upstream HOST[:PORT]]
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
#include <unordered_set>
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
class TcpRouter;

// ============================================================================
// MailboxRegistry — thread-safe mailbox_id → Connection* mapping.
// The same Connection* represents both local-node and peer-router routes.
// remove_all() cleans up all entries for a connection on disconnect.
// ============================================================================

class MailboxRegistry {
public:
    bool add(uint32_t mailbox_id, Connection* con) {
        std::unique_lock lock{mutex_};
        return table_.emplace(mailbox_id, con).second;
    }

    void remove(uint32_t mailbox_id) {
        std::unique_lock lock{mutex_};
        table_.erase(mailbox_id);
    }

    void remove_all(const Connection* con) {
        std::unique_lock lock{mutex_};
        std::erase_if(table_, [con](const auto& kv){ return kv.second == con; });
    }

    Connection* find(uint32_t mailbox_id) const {
        std::shared_lock lock{mutex_};
        const auto it = table_.find(mailbox_id);
        return it != table_.end() ? it->second : nullptr;
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
    void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    int poll_in(int ms) const noexcept {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        return ::poll(&pfd, 1, ms);
    }
    std::optional<TcpSocket> accept() const noexcept {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int cfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (cfd < 0) return std::nullopt;
        TcpSocket s{cfd};
        s.disable_nagle();
        return s;
    }
private:
    int fd_{-1};
};

// ============================================================================
// Connection — one per TCP connection (client node OR peer router)
//
// is_peer_ is false for inbound client connections until MSG_ROUTER_PEER_HELLO
// is received, at which point the connection switches to peer mode and begins
// advertising. Outbound connections (--peer flag) start with is_peer_=true.
//
// Peer mode behaviour:
//   MSG_ROUTER_PEER_REGISTER(mbx) → registry_.add(mbx, this)
//   MSG_ROUTER_PEER_DELETE(mbx)   → registry_.remove(mbx)
//   data messages                 → route to local dest (same as client msgs)
// ============================================================================

class Connection {
public:
    Connection(TcpSocket socket,
               MailboxRegistry& registry,
               std::size_t max_msg_size,
               int index,
               bool is_peer,
               TcpRouter* router)
        : socket_(std::move(socket))
        , registry_(registry)
        , max_msg_size_(max_msg_size)
        , index_(index)
        , is_peer_(is_peer)
        , router_(router) {}

    ~Connection() { registry_.remove_all(this); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool is_peer() const noexcept { return is_peer_; }

    bool forward(const FrameHeader& hdr, const void* body, uint32_t body_len) {
        std::lock_guard lock{send_mutex_};
        return socket_.send_all(&hdr, sizeof(hdr)) &&
               socket_.send_all(body, body_len);
    }

    void send_peer_register(uint32_t mbx_id) {
        MbxInitPayload p{mbx_id};
        const auto fr = make_frame(MSG_ROUTER_PEER_REGISTER, 0, 0, 0, 0,
                                   static_cast<uint32_t>(sizeof(p)));
        std::lock_guard lock{send_mutex_};
        socket_.send_all(&fr, sizeof(fr));
        socket_.send_all(&p, sizeof(p));
    }

    void send_peer_delete(uint32_t mbx_id) {
        MbxInitPayload p{mbx_id};
        const auto fr = make_frame(MSG_ROUTER_PEER_DELETE, 0, 0, 0, 0,
                                   static_cast<uint32_t>(sizeof(p)));
        std::lock_guard lock{send_mutex_};
        socket_.send_all(&fr, sizeof(fr));
        socket_.send_all(&p, sizeof(p));
    }

    void run(std::stop_token stop);

private:
    void advertise_as_peer();
    void handle_frame(const FrameHeader& frame, std::vector<std::byte>& buf);

    TcpSocket        socket_;
    MailboxRegistry& registry_;
    std::size_t      max_msg_size_;
    int              index_;
    bool             is_peer_{false};
    bool             watchdog_enabled_{true};
    std::mutex       send_mutex_;
    TcpRouter*       router_{nullptr};
};

// ============================================================================
// PeerConfig — address of a peer router daemon
// UpstreamConfig — address of a parent router (RACK System Router or another
//                  corerat-router-* at a higher tier)
// ============================================================================

struct PeerConfig {
    std::string host;
    uint16_t    port{2000};
};

struct UpstreamConfig {
    std::string host;
    uint16_t    port{2000};
};

// ============================================================================
// TcpRouter — accept loop + outbound peer connections + mailbox broadcast
// ============================================================================

class TcpRouter {
public:
    TcpRouter(uint16_t port, std::size_t max_msg_size,
              std::vector<PeerConfig> peers = {},
              std::optional<UpstreamConfig> upstream = {})
        : server_(port)
        , max_msg_size_(max_msg_size)
        , port_(port)
        , peer_configs_(std::move(peers))
        , upstream_config_(std::move(upstream)) {}

    bool is_ready() const noexcept { return server_.is_open(); }
    void stop()                    { server_.close(); }

    /// Called by Connection when a local client mailbox registers.
    void on_local_registered(uint32_t mbx_id) {
        {
            std::lock_guard lock{local_mutex_};
            local_mailboxes_.insert(mbx_id);
        }
        broadcast_to_peers([mbx_id](Connection* pc){
            pc->send_peer_register(mbx_id);
        });
    }

    /// Called by Connection when a local client mailbox deletes.
    void on_local_deleted(uint32_t mbx_id) {
        {
            std::lock_guard lock{local_mutex_};
            local_mailboxes_.erase(mbx_id);
        }
        broadcast_to_peers([mbx_id](Connection* pc){
            pc->send_peer_delete(mbx_id);
        });
    }

    /// Snapshot of locally registered mailboxes (sent to new peers at connect).
    std::vector<uint32_t> local_mailboxes_snapshot() const {
        std::lock_guard lock{local_mutex_};
        return {local_mailboxes_.begin(), local_mailboxes_.end()};
    }

    /**
     * @brief Forward a frame to the upstream router (Gap 1).
     *
     * Called by Connection::handle_frame() when the destination is not in the
     * local registry and the sender is a local client (not a peer).
     * If the upstream socket is not connected, returns false so the caller
     * can fall back to sending MSG_ERROR.
     */
    bool forward_to_upstream(const FrameHeader& hdr,
                             const void* body, uint32_t body_len) {
        std::lock_guard lock{upstream_send_mutex_};
        if (!upstream_socket_.is_open()) return false;
        return upstream_socket_.send_all(&hdr, sizeof(hdr)) &&
               (body_len == 0 || upstream_socket_.send_all(body, body_len));
    }

    void run(std::stop_token stop) {
        connect_to_peers();
        connect_to_upstream();

        std::printf("[TCP router] listening on port %u\n",
                    static_cast<unsigned>(port_));

        while (!stop.stop_requested()) {
            if (server_.poll_in(200) <= 0) continue;
            auto sock = server_.accept();
            if (!sock) continue;

            const int idx = next_index_++;
            std::printf("[TCP router] new connection[%d]\n", idx);

            auto con = std::make_unique<Connection>(
                std::move(*sock), registry_, max_msg_size_, idx,
                false /* not peer yet */, this);
            Connection* raw = con.get();
            auto thread = std::jthread{[raw](std::stop_token st){ raw->run(st); }};

            std::lock_guard lock{connections_mutex_};
            connections_.emplace_back(std::move(con), std::move(thread));
        }

        std::printf("[TCP router] shutting down\n");
        server_.close();
    }

    static constexpr uint16_t kDefaultPort = 2000;

private:
    void connect_to_upstream() {
        if (!upstream_config_) return;
        if (!upstream_socket_.connect(upstream_config_->host, upstream_config_->port)) {
            std::fprintf(stderr,
                "[TCP router] failed to connect to upstream %s:%u — "
                "unknown dests will get MSG_ERROR\n",
                upstream_config_->host.c_str(),
                static_cast<unsigned>(upstream_config_->port));
            return;
        }
        std::printf("[TCP router] connected to upstream %s:%u\n",
                    upstream_config_->host.c_str(),
                    static_cast<unsigned>(upstream_config_->port));
        upstream_thread_ = std::jthread{[this](std::stop_token st){
            run_upstream_reader(st);
        }};
    }

    /**
     * @brief Read frames coming back from the upstream router (Gap 1).
     *
     * The upstream router may send data (forwarded from a remote host) or
     * MSG_ERROR replies.  Both are routed into the local registry just like
     * any inbound frame, so the originating local client receives its reply.
     */
    void run_upstream_reader(std::stop_token stop) {
        std::vector<std::byte> buf(max_msg_size_);
        while (!stop.stop_requested()) {
            if (upstream_socket_.poll_in(200) <= 0) continue;
            FrameHeader frame{};
            if (!upstream_socket_.recv_all(&frame, sizeof(frame))) break;

            const uint32_t body_len =
                (frame.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
                ? frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader)) : 0u;
            if (body_len > max_msg_size_) { upstream_socket_.close(); break; }
            if (body_len > 0 &&
                !upstream_socket_.recv_all(buf.data(), body_len)) break;

            // Route the reply back to the local client that originated it.
            Connection* dest = registry_.find(frame.dest);
            if (dest) dest->forward(frame, buf.data(), body_len);
        }
        std::printf("[TCP router] upstream connection closed\n");
        upstream_socket_.close();
    }

    void connect_to_peers() {
        for (const auto& pc : peer_configs_) {
            TcpSocket sock;
            if (!sock.connect(pc.host, pc.port)) {
                std::fprintf(stderr,
                    "[TCP router] failed to connect to peer %s:%u — "
                    "will route unknown dests to MSG_ERROR\n",
                    pc.host.c_str(), static_cast<unsigned>(pc.port));
                continue;
            }
            const int idx = next_index_++;
            auto con = std::make_unique<Connection>(
                std::move(sock), registry_, max_msg_size_, idx,
                true /* is_peer */, this);
            Connection* raw = con.get();
            auto thread = std::jthread{[raw](std::stop_token st){ raw->run(st); }};
            {
                std::lock_guard lock{connections_mutex_};
                connections_.emplace_back(std::move(con), std::move(thread));
            }
            std::printf("[TCP router] connected to peer %s:%u (connection[%d])\n",
                        pc.host.c_str(), static_cast<unsigned>(pc.port), idx);
        }
    }

    template<typename F>
    void broadcast_to_peers(F&& fn) {
        // Snapshot peer pointers under lock; send without holding lock.
        std::vector<Connection*> peers;
        {
            std::lock_guard lock{connections_mutex_};
            for (const auto& e : connections_)
                if (e.con->is_peer()) peers.push_back(e.con.get());
        }
        for (auto* pc : peers) fn(pc);
    }

    struct Entry {
        std::unique_ptr<Connection> con;
        std::jthread                thread;
    };

    ServerSocket                   server_;
    MailboxRegistry                registry_;
    std::size_t                    max_msg_size_;
    uint16_t                       port_;
    std::vector<PeerConfig>        peer_configs_;
    std::optional<UpstreamConfig>  upstream_config_;
    TcpSocket                      upstream_socket_;
    std::mutex                     upstream_send_mutex_;
    std::jthread                   upstream_thread_;
    std::atomic<int>               next_index_{0};
    std::mutex                     connections_mutex_;
    std::vector<Entry>             connections_;
    mutable std::mutex             local_mutex_;
    std::unordered_set<uint32_t>   local_mailboxes_;
};

// ============================================================================
// Connection method implementations
// (placed after TcpRouter definition to resolve the forward reference)
// ============================================================================

void Connection::advertise_as_peer() {
    {
        const auto hello = make_frame(MSG_ROUTER_PEER_HELLO, 0, 0, 0, 0, 0);
        std::lock_guard lock{send_mutex_};
        socket_.send_all(&hello, sizeof(hello));
    }
    if (router_) {
        for (auto mbx : router_->local_mailboxes_snapshot())
            send_peer_register(mbx);
    }
}

void Connection::run(std::stop_token stop) {
    if (is_peer_) advertise_as_peer();

    std::vector<std::byte> buf(max_msg_size_);
    while (!stop.stop_requested()) {
        if (socket_.poll_in(200) <= 0) continue;
        FrameHeader frame{};
        if (!socket_.recv_all(&frame, sizeof(frame))) break;
        handle_frame(frame, buf);
    }

    registry_.remove_all(this);
    std::printf("[TCP router] connection[%d] closed%s\n", index_,
                is_peer_ ? " (peer)" : "");
}

void Connection::handle_frame(const FrameHeader& frame,
                               std::vector<std::byte>& buf) {
    const uint32_t body_len =
        (frame.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
        ? frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader)) : 0u;

    if (body_len > max_msg_size_) {
        std::fprintf(stderr, "[TCP router] connection[%d] oversized (%u bytes)\n",
                     index_, frame.msglen);
        socket_.close(); return;
    }
    if (body_len > 0 && !socket_.recv_all(buf.data(), body_len)) return;

    switch (frame.type) {
        // ── Peer protocol ─────────────────────────────────────────────────
        case MSG_ROUTER_PEER_HELLO:
            if (!is_peer_) {
                is_peer_ = true;
                advertise_as_peer();
                std::printf("[TCP router] connection[%d] upgraded to peer\n",
                            index_);
            }
            break;

        case MSG_ROUTER_PEER_REGISTER: {
            if (!is_peer_ || body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload p{};
            std::memcpy(&p, buf.data(), sizeof(p));
            if (registry_.add(p.mbx, this))
                std::printf("[TCP router] peer[%d] registered mailbox %08x\n",
                            index_, p.mbx);
            break;
        }

        case MSG_ROUTER_PEER_DELETE: {
            if (!is_peer_ || body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload p{};
            std::memcpy(&p, buf.data(), sizeof(p));
            registry_.remove(p.mbx);
            std::printf("[TCP router] peer[%d] deleted mailbox %08x\n",
                        index_, p.mbx);
            break;
        }

        // ── Client protocol ───────────────────────────────────────────────
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
            {
                std::lock_guard lock{send_mutex_};
                socket_.send_all(&reply, sizeof(reply));
            }
            if (ok) {
                std::printf("[TCP router] registered mailbox %08x "
                            "on connection[%d]\n", payload.mbx, index_);
                if (router_) router_->on_local_registered(payload.mbx);
            }
            break;
        }

        case MSG_ROUTER_MBX_DELETE: {
            if (body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload payload{};
            std::memcpy(&payload, buf.data(), sizeof(payload));
            registry_.remove(payload.mbx);
            std::printf("[TCP router] deleted mailbox %08x from "
                        "connection[%d]\n", payload.mbx, index_);
            if (router_) router_->on_local_deleted(payload.mbx);
            break;
        }

        // ── Data routing ──────────────────────────────────────────────────
        default: {
            Connection* dest = registry_.find(frame.dest);
            if (dest) {
                dest->forward(frame, buf.data(), body_len);
            } else if (!is_peer_) {
                // Gap 1: try upstream router before answering MSG_ERROR.
                // Peer routers are excluded — they handle their own errors.
                if (router_ &&
                    router_->forward_to_upstream(frame, buf.data(), body_len)) {
                    // Forwarded upstream; reply (if any) arrives via the
                    // upstream reader thread and is routed back to frame.src.
                } else {
                    const auto err = make_frame(MSG_ERROR, frame.src, 0,
                                                0, frame.seq_nr, 0);
                    std::lock_guard lock{send_mutex_};
                    socket_.send_all(&err, sizeof(err));
                }
            }
            break;
        }
    }
}

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
    std::vector<corerat::router::PeerConfig>    peers;
    std::optional<corerat::router::UpstreamConfig> upstream_config;

    auto parse_host_port = [](const std::string& s, uint16_t default_port)
        -> std::pair<std::string, uint16_t> {
        const auto colon = s.rfind(':');
        if (colon != std::string::npos)
            return {s.substr(0, colon),
                    static_cast<uint16_t>(std::stoul(s.substr(colon + 1)))};
        return {s, default_port};
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if ((arg == "--max-msg-size" || arg == "-m") && i + 1 < argc)
            max_msg_size = std::stoul(argv[++i]);
        else if ((arg == "--peer" || arg == "-r") && i + 1 < argc) {
            auto [host, pport] = parse_host_port(argv[++i],
                                                 corerat::router::TcpRouter::kDefaultPort);
            peers.push_back({std::move(host), pport});
        }
        else if ((arg == "--upstream" || arg == "-u") && i + 1 < argc) {
            auto [host, uport] = parse_host_port(argv[++i],
                                                 corerat::router::TcpRouter::kDefaultPort);
            upstream_config = corerat::router::UpstreamConfig{std::move(host), uport};
        }
        else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: corerat-router-tcp [--port PORT] "
                        "[--max-msg-size BYTES] [--peer HOST[:PORT]] ..."
                        " [--upstream HOST[:PORT]]\n");
            return 0;
        }
    }

    ::signal(SIGPIPE, SIG_IGN);

    corerat::router::TcpRouter router{port, max_msg_size,
                                      std::move(peers), std::move(upstream_config)};
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
    router.stop();
    return 0;
}
