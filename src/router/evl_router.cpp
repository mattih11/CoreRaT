/**
 * @file router/evl_router.cpp
 * @brief CoreRaT EVL name-service router
 *
 * Enables cross-process EvlMailbox communication by acting as a name service:
 * processes register their mailbox resources (SHM path, EVL object names) and
 * query remote mailbox locations via a Unix domain socket.
 *
 * Protocol (Unix domain socket, SOCK_STREAM):
 *   All messages are fixed-size structs preceded by a 1-byte opcode.
 *
 *   REGISTER  (client → router): announce a Public EvlMailbox
 *   LOOKUP    (client → router): find a mailbox by id
 *   UNREGISTER(client → router): remove a mailbox
 *   REPLY_OK  (router → client): success + MailboxInfo
 *   REPLY_ERR (router → client): failure
 *
 * A Public EvlMailbox calls evl_create_mutex/event with EVL_CLONE_PUBLIC so
 * any process can evl_open_mutex/event by name after a successful LOOKUP.
 * The ring buffer data is in a POSIX SHM region opened via shm_open.
 *
 * Usage:
 *   corerat-router-evl [--socket PATH]
 */

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace corerat::evl_router {

// ============================================================================
// Wire protocol
// ============================================================================

inline constexpr std::string_view kDefaultSocket = "/var/run/corerat/evl-router.sock";

enum class Opcode : uint8_t {
    Register   = 0x01,
    Lookup     = 0x02,
    Unregister = 0x03,
    ReplyOk    = 0x80,
    ReplyErr   = 0x81,
};

/// Resource names for one Public EvlMailbox.
/// All strings are null-terminated, max 63 chars + NUL.
struct MailboxInfo {
    uint32_t mailbox_id{0};
    uint32_t slot_count{0};
    uint32_t slot_size{0};
    char     shm_name[64]{};       ///< POSIX shm_open path, e.g. "/corerat_mbx_5"
    char     mutex_name[64]{};     ///< EVL mutex name, e.g. "corerat-ring-mtx-5"
    char     event_name[64]{};     ///< EVL event name, e.g. "corerat-ring-evt-5"
};

struct Request {
    Opcode      opcode{};
    MailboxInfo info{};
};

struct Reply {
    Opcode      opcode{};
    MailboxInfo info{};
};

// ============================================================================
// UnixSocket — RAII Unix domain socket fd
// ============================================================================

class UnixSocket {
public:
    UnixSocket() = default;
    explicit UnixSocket(int fd) noexcept : fd_(fd) {}
    ~UnixSocket() { close(); }

    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;
    UnixSocket(UnixSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UnixSocket& operator=(UnixSocket&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    bool is_open() const noexcept { return fd_ >= 0; }
    int  native_fd() const noexcept { return fd_; }

    bool send_all(const void* buf, std::size_t len) const noexcept {
        const auto* p = static_cast<const std::byte*>(buf);
        for (std::size_t sent = 0; sent < len; ) {
            const ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool recv_all(void* buf, std::size_t len) const noexcept {
        auto* p = static_cast<std::byte*>(buf);
        for (std::size_t got = 0; got < len; ) {
            const ssize_t n = ::recv(fd_, p + got, len - got, 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    int poll_in(int timeout_ms) const noexcept {
        pollfd pfd{ fd_, POLLIN, 0 };
        return ::poll(&pfd, 1, timeout_ms);
    }

private:
    int fd_{-1};
};

// ============================================================================
// ServerSocket (Unix domain)
// ============================================================================

class ServerSocket {
public:
    explicit ServerSocket(std::string_view path) : path_(path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return;

        // Create parent directory if it does not exist
        const auto sep = path_.rfind('/');
        if (sep != std::string::npos && sep > 0) {
            const std::string dir = path_.substr(0, sep);
            ::mkdir(dir.c_str(), 0755);
        }

        ::unlink(path_.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd_, 32) < 0) {
            ::close(fd_); fd_ = -1;
        }
    }

    ~ServerSocket() {
        if (fd_ >= 0) ::close(fd_);
        ::unlink(path_.c_str());
    }

    ServerSocket(const ServerSocket&) = delete;
    ServerSocket& operator=(const ServerSocket&) = delete;

    bool is_open() const noexcept { return fd_ >= 0; }

    std::optional<UnixSocket> accept() const noexcept {
        sockaddr_un client_addr{};
        socklen_t   len = sizeof(client_addr);
        const int   cfd = ::accept(fd_,
                                   reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (cfd < 0) return std::nullopt;
        return UnixSocket{cfd};
    }

    void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    std::string path_;
    int         fd_{-1};
};

// ============================================================================
// Registry
// ============================================================================

class Registry {
public:
    bool add(const MailboxInfo& info) {
        std::lock_guard lock{mutex_};
        return table_.emplace(info.mailbox_id, info).second;
    }

    void remove(uint32_t id) {
        std::lock_guard lock{mutex_};
        table_.erase(id);
    }

    std::optional<MailboxInfo> find(uint32_t id) const {
        std::lock_guard lock{mutex_};
        if (const auto it = table_.find(id); it != table_.end())
            return it->second;
        return std::nullopt;
    }

private:
    mutable std::mutex                         mutex_;
    std::unordered_map<uint32_t, MailboxInfo>  table_;
};

// ============================================================================
// ClientHandler — one per connected process
// ============================================================================

class ClientHandler {
public:
    ClientHandler(UnixSocket socket, Registry& registry, int idx)
        : socket_(std::move(socket)), registry_(registry), index_(idx) {}

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (socket_.poll_in(200) <= 0) continue;

            Request req{};
            if (!socket_.recv_all(&req, sizeof(req))) break;

            handle(req);
        }
    }

private:
    void handle(const Request& req) {
        switch (req.opcode) {
            case Opcode::Register: {
                const bool ok = registry_.add(req.info);
                if (ok) {
                    printf("[EVL router] registered mailbox %08x "
                           "(shm=%s, mutex=%s, event=%s)\n",
                           req.info.mailbox_id,
                           req.info.shm_name,
                           req.info.mutex_name,
                           req.info.event_name);
                }
                reply(ok ? Opcode::ReplyOk : Opcode::ReplyErr, req.info);
                break;
            }

            case Opcode::Lookup: {
                const auto info = registry_.find(req.info.mailbox_id);
                if (info) {
                    reply(Opcode::ReplyOk, *info);
                } else {
                    reply(Opcode::ReplyErr, req.info);
                }
                break;
            }

            case Opcode::Unregister:
                registry_.remove(req.info.mailbox_id);
                printf("[EVL router] unregistered mailbox %08x\n",
                       req.info.mailbox_id);
                reply(Opcode::ReplyOk, req.info);
                break;

            default:
                reply(Opcode::ReplyErr, req.info);
                break;
        }
    }

    void reply(Opcode opcode, const MailboxInfo& info) {
        const Reply r{ opcode, info };
        socket_.send_all(&r, sizeof(r));
    }

    UnixSocket  socket_;
    Registry&   registry_;
    int         index_;
};

// ============================================================================
// EvlRouter
// ============================================================================

class EvlRouter {
public:
    explicit EvlRouter(std::string_view socket_path)
        : server_(socket_path) {}

    bool is_ready() const noexcept { return server_.is_open(); }

    void run(std::stop_token stop) {
        printf("[EVL router] listening on %s\n", kDefaultSocket.data());

        while (!stop.stop_requested()) {
            auto sock = server_.accept();
            if (!sock) break;

            const int idx = next_index_++;
            printf("[EVL router] new client[%d]\n", idx);

            auto handler = std::make_unique<ClientHandler>(
                std::move(*sock), registry_, idx);

            ClientHandler* raw = handler.get();
            auto thread = std::jthread{[raw](std::stop_token st){
                raw->run(st);
            }};

            std::lock_guard lock{clients_mutex_};
            clients_.emplace_back(std::move(handler), std::move(thread));
        }

        printf("[EVL router] shutting down\n");
    }

private:
    struct Entry {
        std::unique_ptr<ClientHandler> handler;
        std::jthread                   thread;
    };

    ServerSocket         server_;
    Registry             registry_;
    std::atomic<int>     next_index_{0};
    std::mutex           clients_mutex_;
    std::vector<Entry>   clients_;
};

}  // namespace corerat::evl_router

// ============================================================================
// main
// ============================================================================

namespace {
    std::atomic<bool> g_shutdown{false};
}

int main(int argc, char* argv[]) {
    std::string socket_path{corerat::evl_router::kDefaultSocket};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if ((arg == "--socket" || arg == "-s") && i + 1 < argc)
            socket_path = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            printf("Usage: corerat-router-evl [--socket PATH]\n");
            return 0;
        }
    }

    corerat::evl_router::EvlRouter router{socket_path};
    if (!router.is_ready()) {
        fprintf(stderr, "Failed to bind socket %s\n", socket_path.c_str());
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
    return 0;
}
