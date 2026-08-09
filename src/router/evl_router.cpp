/**
 * @file router/evl_router.cpp
 * @brief CoreRaT EVL message router with xbuf bridge
 *
 * Full TCP message router (same TiMS wire protocol as corerat-router-tcp)
 * that also bridges in-band (STD) senders/receivers to OOB (EVL) mailboxes
 * via the EVL cross-buffer (xbuf) facility.
 *
 * Protocol: TiMS FrameHeader (16 bytes) + body — identical to RACK/TiMS.
 * Port: 2000 (same as corerat-router-tcp; run one or the other per host).
 *
 * Mailbox kinds:
 *   Tcp  — normal TCP-connected mailbox (STD nodes, or EVL nodes on remote host)
 *   Xbuf — local EVL mailbox bridged via /dev/evl/xbuf/corerat-xbuf-<id>
 *
 * Registration (MSG_ROUTER_MBX_INIT_WITH_REPLY from any TCP client):
 *   Router probes /dev/evl/xbuf/corerat-xbuf-<mbx_id>.
 *   If the xbuf exists → Kind::Xbuf, start XbufReader thread.
 *   Otherwise         → Kind::Tcp, normal TCP forwarding.
 *
 * Routing:
 *   TCP→Xbuf (STD→EVL, Scenario D):
 *     Strip FrameHeader, write(xbuf_fd, body, body_len).
 *     EVL mailbox receives via oob_read(evl_xbuf_fd).
 *
 *   Xbuf→TCP (EVL→STD, Scenario C):
 *     XbufReader reads read(xbuf_fd, buf) — OOB side oob_write feeds this.
 *     Parse WireHeader.dest from bytes[20..23] → look up connection → TCP forward.
 *
 * Usage:
 *   corerat-router-evl [--port PORT] [--max-msg-size BYTES]
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

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace corerat::evl_router {

using namespace corerat::tims_proto;

// ============================================================================
// Forward declarations
// ============================================================================

class Connection;
class XbufEntry;
class EvlRouter;

// ============================================================================
// WireHeader — minimal definition matching corerat/messaging/wire_message.hpp
// Used to extract dest/src from xbuf data without pulling in the full header.
// ============================================================================

struct WireHeader {
    uint32_t msg_type;
    uint32_t msg_size;
    uint64_t timestamp;
    uint32_t seq_number;
    uint32_t dest;   // byte offset 20
    uint32_t src;    // byte offset 24
    uint32_t flags;
};
static_assert(sizeof(WireHeader) == 32);
static_assert(offsetof(WireHeader, dest) == 20);

// ============================================================================
// MailboxEntry — discriminated union: TCP connection or xbuf fd
// ============================================================================

enum class MailboxKind : uint8_t { Tcp, Xbuf, Peer };

struct MailboxEntry {
    MailboxKind                 kind{MailboxKind::Tcp};
    Connection*                 con{nullptr};        // Kind::Tcp or Kind::Peer
    std::shared_ptr<XbufEntry>  xbuf_entry{};        // Kind::Xbuf
};

// ============================================================================
// MailboxRegistry — thread-safe mailbox_id → MailboxEntry
// ============================================================================

class MailboxRegistry {
public:
    bool add_tcp(uint32_t mailbox_id, Connection* c) {
        std::unique_lock lock{mutex_};
        MailboxEntry e; e.kind = MailboxKind::Tcp; e.con = c;
        return table_.emplace(mailbox_id, e).second;
    }

    bool add_xbuf(uint32_t mailbox_id, std::shared_ptr<XbufEntry> xe) {
        std::unique_lock lock{mutex_};
        MailboxEntry e; e.kind = MailboxKind::Xbuf; e.xbuf_entry = std::move(xe);
        return table_.emplace(mailbox_id, std::move(e)).second;
    }

    bool add_peer(uint32_t mailbox_id, Connection* c) {
        std::unique_lock lock{mutex_};
        MailboxEntry e; e.kind = MailboxKind::Peer; e.con = c;
        return table_.emplace(mailbox_id, e).second;
    }

    void remove(uint32_t mailbox_id) {
        std::unique_lock lock{mutex_};
        table_.erase(mailbox_id);
    }

    /// Remove all Tcp and Peer entries for a given connection (on disconnect).
    void remove_all_for_con(const Connection* c) {
        std::unique_lock lock{mutex_};
        std::erase_if(table_, [c](const auto& kv){
            const auto& e = kv.second;
            return (e.kind == MailboxKind::Tcp || e.kind == MailboxKind::Peer)
                   && e.con == c;
        });
    }

    std::optional<MailboxEntry> find(uint32_t mailbox_id) const {
        std::shared_lock lock{mutex_};
        const auto it = table_.find(mailbox_id);
        if (it != table_.end()) return it->second;
        return std::nullopt;
    }

private:
    mutable std::shared_mutex                   mutex_;
    std::unordered_map<uint32_t, MailboxEntry>  table_;
};

// ============================================================================
// ServerSocket — RAII TCP accept socket (same as tcp_router.cpp)
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

    int poll_in(int timeout_ms) const noexcept {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        return ::poll(&pfd, 1, timeout_ms);
    }

    std::optional<TcpSocket> accept() const noexcept {
        sockaddr_in addr{};
        socklen_t   len = sizeof(addr);
        const int   cfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (cfd < 0) return std::nullopt;
        TcpSocket s{cfd};
        s.disable_nagle();
        return s;
    }

private:
    int fd_{-1};
};

// ============================================================================
// Connection — one per TCP client
// ============================================================================

class Connection {
public:
    Connection(TcpSocket socket, MailboxRegistry& registry,
               std::size_t max_msg_size, int index,
               bool is_peer, EvlRouter* router)
        : socket_(std::move(socket))
        , registry_(registry)
        , max_msg_size_(max_msg_size)
        , index_(index)
        , is_peer_(is_peer)
        , router_(router) {}

    ~Connection() { registry_.remove_all_for_con(this); }
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
    std::mutex       send_mutex_;
    EvlRouter*       router_{nullptr};
};

// ============================================================================
// XbufEntry — owns the in-band side of one EVL xbuf device.
//
// /dev/evl/xbuf/corerat-xbuf-<id> is opened with plain open(O_RDWR).
// Two directions via the same fd:
//   write(fd, body, n)  → EVL mailbox reads via oob_read()  [STD→EVL]
//   read(fd, buf, n)   ← EVL mailbox writes via oob_write() [EVL→STD]
//
// XbufReader thread handles the EVL→TCP direction.
// TCP→EVL forwarding is done inline by Connection::handle_frame().
// ============================================================================

class XbufEntry {
public:
    XbufEntry(int xbuf_fd, uint32_t mailbox_id,
              MailboxRegistry& registry, std::size_t max_msg_size)
        : fd_(xbuf_fd)
        , mailbox_id_(mailbox_id)
        , registry_(registry)
        , max_msg_size_(max_msg_size) {}

    ~XbufEntry() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    XbufEntry(const XbufEntry&) = delete;
    XbufEntry& operator=(const XbufEntry&) = delete;

    int  fd()          const noexcept { return fd_; }
    uint32_t mailbox_id() const noexcept { return mailbox_id_; }

    /// Write body (WireHeader + payload) into the xbuf inbound ring.
    /// Called by Connection when routing TCP→Xbuf (STD→EVL, Scenario D).
    bool write_body(const void* body, std::size_t body_len) noexcept {
        if (fd_ < 0 || body_len == 0) return false;
        const auto* p = static_cast<const std::byte*>(body);
        std::size_t written = 0;
        while (written < body_len) {
            const ssize_t n = ::write(fd_, p + written, body_len - written);
            if (n <= 0) return false;
            written += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Start the reader thread that handles EVL→TCP (Scenario C).
    void start_reader() {
        reader_thread_ = std::jthread{[this](std::stop_token st){
            run_reader(st);
        }};
    }

private:
    void run_reader(std::stop_token stop) {
        std::vector<std::byte> buf(max_msg_size_);

        while (!stop.stop_requested()) {
            struct pollfd pfd{fd_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr <= 0) continue;

            const ssize_t n = ::read(fd_, buf.data(), buf.size());
            if (n <= 0) continue;

            if (static_cast<std::size_t>(n) < sizeof(WireHeader)) continue;

            // Extract dest and src from the WireHeader at the start of data.
            WireHeader wh{};
            std::memcpy(&wh, buf.data(), sizeof(WireHeader));
            const uint32_t dest = wh.dest;
            const uint32_t src  = wh.src;

            const auto entry = registry_.find(dest);
            if (!entry || (entry->kind != MailboxKind::Tcp &&
                           entry->kind != MailboxKind::Peer) || !entry->con) {
                // Destination not found or not a TCP/Peer connection — drop
                std::fprintf(stderr,
                    "[EVL router] xbuf[%08x]: no TCP/peer route for dest %08x, dropping\n",
                    mailbox_id_, dest);
                continue;
            }

            const auto frame = make_frame(0, dest, src, 0, 0,
                                          static_cast<uint32_t>(n));
            entry->con->forward(frame, buf.data(),
                                static_cast<uint32_t>(n));
        }
    }

    int              fd_;
    uint32_t         mailbox_id_;
    MailboxRegistry& registry_;
    std::size_t      max_msg_size_;
    std::jthread     reader_thread_;
};

// ============================================================================
// Connection method bodies — defined after EvlRouter (forward ref resolved below)
// ============================================================================

// ============================================================================
// EvlRouter — accept loop + connection lifetime management
// ============================================================================

struct PeerConfig {
    std::string host;
    uint16_t    port{2000};
};

class EvlRouter {
public:
    EvlRouter(uint16_t port, std::size_t max_msg_size,
              std::vector<PeerConfig> peers = {})
        : server_(port), max_msg_size_(max_msg_size), port_(port)
        , peer_configs_(std::move(peers)) {}

    bool is_ready() const noexcept { return server_.is_open(); }
    void stop()                    { server_.close(); }

    void on_local_registered(uint32_t mbx_id) {
        {
            std::lock_guard lock{local_mutex_};
            local_mailboxes_.insert(mbx_id);
        }
        broadcast_to_peers([mbx_id](Connection* pc){
            pc->send_peer_register(mbx_id);
        });
    }

    void on_local_deleted(uint32_t mbx_id) {
        {
            std::lock_guard lock{local_mutex_};
            local_mailboxes_.erase(mbx_id);
        }
        broadcast_to_peers([mbx_id](Connection* pc){
            pc->send_peer_delete(mbx_id);
        });
    }

    std::vector<uint32_t> local_mailboxes_snapshot() const {
        std::lock_guard lock{local_mutex_};
        return {local_mailboxes_.begin(), local_mailboxes_.end()};
    }

    void run(std::stop_token stop) {
        connect_to_peers();

        std::printf("[EVL router] listening on TCP port %u "
                    "(xbuf bridge + peer routing enabled)\n",
                    static_cast<unsigned>(port_));

        while (!stop.stop_requested()) {
            if (server_.poll_in(200) <= 0) continue;

            auto sock = server_.accept();
            if (!sock) continue;

            const int idx = next_index_++;
            std::printf("[EVL router] new connection[%d]\n", idx);

            auto con = std::make_unique<Connection>(
                std::move(*sock), registry_, max_msg_size_, idx,
                false /* not peer yet */, this);

            Connection* raw = con.get();
            auto thread = std::jthread{[raw](std::stop_token st){
                raw->run(st);
            }};

            std::lock_guard lock{connections_mutex_};
            connections_.emplace_back(std::move(con), std::move(thread));
        }

        std::printf("[EVL router] shutting down\n");
        server_.close();
    }

    static constexpr uint16_t kDefaultPort = 2000;

private:
    void connect_to_peers() {
        for (const auto& pc : peer_configs_) {
            TcpSocket sock;
            if (!sock.connect(pc.host, pc.port)) {
                std::fprintf(stderr,
                    "[EVL router] failed to connect to peer %s:%u\n",
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
            std::printf("[EVL router] connected to peer %s:%u (connection[%d])\n",
                        pc.host.c_str(), static_cast<unsigned>(pc.port), idx);
        }
    }

    template<typename F>
    void broadcast_to_peers(F&& fn) {
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

    ServerSocket                 server_;
    MailboxRegistry              registry_;
    std::size_t                  max_msg_size_;
    uint16_t                     port_;
    std::vector<PeerConfig>      peer_configs_;
    std::atomic<int>             next_index_{0};
    std::mutex                   connections_mutex_;
    std::vector<Entry>           connections_;
    mutable std::mutex           local_mutex_;
    std::unordered_set<uint32_t> local_mailboxes_;
};

// ============================================================================
// Connection method bodies — EvlRouter now complete, all methods resolvable
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

    registry_.remove_all_for_con(this);
    std::printf("[EVL router] connection[%d] closed%s\n", index_,
                is_peer_ ? " (peer)" : "");
}

void Connection::handle_frame(const FrameHeader& frame,
                               std::vector<std::byte>& buf) {
    const uint32_t body_len =
        (frame.msglen > static_cast<uint32_t>(sizeof(FrameHeader)))
        ? frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader)) : 0u;

    if (body_len > max_msg_size_) {
        std::fprintf(stderr,
            "[EVL router] connection[%d] oversized message (%u bytes)\n",
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
                std::printf("[EVL router] connection[%d] upgraded to peer\n",
                            index_);
            }
            break;

        case MSG_ROUTER_PEER_REGISTER: {
            if (!is_peer_ || body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload p{};
            std::memcpy(&p, buf.data(), sizeof(p));
            if (registry_.add_peer(p.mbx, this))
                std::printf("[EVL router] peer[%d] registered mailbox %08x\n",
                            index_, p.mbx);
            break;
        }

        case MSG_ROUTER_PEER_DELETE: {
            if (!is_peer_ || body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload p{};
            std::memcpy(&p, buf.data(), sizeof(p));
            registry_.remove(p.mbx);
            std::printf("[EVL router] peer[%d] deleted mailbox %08x\n",
                        index_, p.mbx);
            break;
        }

        // ── Client protocol ───────────────────────────────────────────────
        case MSG_ROUTER_DISABLE_WATCHDOG:
            break;

        case MSG_ROUTER_MBX_INIT_WITH_REPLY: {
            if (body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload payload{};
            std::memcpy(&payload, buf.data(), sizeof(payload));
            const uint32_t mbx_id = payload.mbx;

            char xbuf_path[64];
            std::snprintf(xbuf_path, sizeof(xbuf_path),
                          "/dev/evl/xbuf/corerat-xbuf-%u", mbx_id);
            const int xfd = ::open(xbuf_path, O_RDWR | O_NONBLOCK);

            bool ok = false;
            if (xfd >= 0) {
                auto xe = std::make_shared<XbufEntry>(
                    xfd, mbx_id, registry_, max_msg_size_);
                ok = registry_.add_xbuf(mbx_id, xe);
                if (ok) {
                    xe->start_reader();
                    std::printf("[EVL router] registered xbuf mailbox %08x "
                                "(%s)\n", mbx_id, xbuf_path);
                    if (router_) router_->on_local_registered(mbx_id);
                }
            } else {
                ok = registry_.add_tcp(mbx_id, this);
                if (ok) {
                    std::printf("[EVL router] registered TCP mailbox %08x "
                                "on connection[%d]\n", mbx_id, index_);
                    if (router_) router_->on_local_registered(mbx_id);
                }
            }

            const auto reply = make_frame(ok ? MSG_OK : MSG_ERROR,
                                          frame.src, 0, 0, frame.seq_nr, 0);
            {
                std::lock_guard lock{send_mutex_};
                socket_.send_all(&reply, sizeof(reply));
            }
            break;
        }

        case MSG_ROUTER_MBX_DELETE: {
            if (body_len < sizeof(MbxInitPayload)) break;
            MbxInitPayload payload{};
            std::memcpy(&payload, buf.data(), sizeof(payload));
            registry_.remove(payload.mbx);
            std::printf("[EVL router] deleted mailbox %08x from "
                        "connection[%d]\n", payload.mbx, index_);
            if (router_) router_->on_local_deleted(payload.mbx);
            break;
        }

        // ── Data routing ──────────────────────────────────────────────────
        default: {
            const auto entry = registry_.find(frame.dest);
            if (!entry) {
                if (!is_peer_) {
                    const auto err = make_frame(MSG_ERROR, frame.src, 0,
                                                0, frame.seq_nr, 0);
                    std::lock_guard lock{send_mutex_};
                    socket_.send_all(&err, sizeof(err));
                }
                break;
            }
            if ((entry->kind == MailboxKind::Tcp ||
                 entry->kind == MailboxKind::Peer) && entry->con) {
                entry->con->forward(frame, buf.data(), body_len);
            } else if (entry->kind == MailboxKind::Xbuf && entry->xbuf_entry) {
                if (!entry->xbuf_entry->write_body(buf.data(), body_len)) {
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

}  // namespace corerat::evl_router

// ============================================================================
// main
// ============================================================================

namespace {
    std::atomic<bool> g_shutdown{false};
}

int main(int argc, char* argv[]) {
    uint16_t    port         = corerat::evl_router::EvlRouter::kDefaultPort;
    std::size_t max_msg_size = 256 * 1024;
    std::vector<corerat::evl_router::PeerConfig> peers;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if ((arg == "--max-msg-size" || arg == "-m") && i + 1 < argc)
            max_msg_size = std::stoul(argv[++i]);
        else if ((arg == "--peer" || arg == "-r") && i + 1 < argc) {
            std::string peer_str{argv[++i]};
            const auto colon = peer_str.rfind(':');
            corerat::evl_router::PeerConfig pc;
            if (colon != std::string::npos) {
                pc.host = peer_str.substr(0, colon);
                pc.port = static_cast<uint16_t>(std::stoul(peer_str.substr(colon + 1)));
            } else {
                pc.host = peer_str;
            }
            peers.push_back(std::move(pc));
        }
        else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: corerat-router-evl [--port PORT] "
                        "[--max-msg-size BYTES] [--peer HOST[:PORT]] ...\n");
            return 0;
        }
    }

    ::signal(SIGPIPE, SIG_IGN);

    corerat::evl_router::EvlRouter router{port, max_msg_size, std::move(peers)};
    if (!router.is_ready()) {
        std::fprintf(stderr, "Failed to bind TCP port %u\n",
                     static_cast<unsigned>(port));
        return 1;
    }

    std::stop_source stop;
    ::signal(SIGINT,  [](int){ g_shutdown = true; });
    ::signal(SIGTERM, [](int){ g_shutdown = true; });

    std::jthread router_thread{[&router, &stop]{
        router.run(stop.get_token());
    }};

    while (!g_shutdown)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.request_stop();
    router.stop();
    return 0;
}
