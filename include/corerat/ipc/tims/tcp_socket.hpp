#pragma once

/**
 * @file tims/tcp_socket.hpp
 * @brief RAII TCP socket helper used by TimsMailbox and TcpRouter.
 *
 * Internal header — not part of the public CoreRaT API.
 */

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

namespace corerat {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    TcpSocket& operator=(TcpSocket&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    /// Wrap an already-open fd (takes ownership).
    explicit TcpSocket(int fd) noexcept : fd_(fd) {}

    /// Connect to host:port. Returns true on success.
    bool connect(std::string_view host, uint16_t port) noexcept {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.data(), &addr.sin_addr) != 1) {
            close(); return false;
        }
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            close(); return false;
        }
        disable_nagle();
        return true;
    }

    void disable_nagle() const noexcept {
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool is_open()   const noexcept { return fd_ >= 0; }
    int  native_fd() const noexcept { return fd_; }

    /// Blocking send of exactly len bytes. Returns false on error.
    bool send_all(const void* buf, std::size_t len) const noexcept {
        const auto* p = static_cast<const std::byte*>(buf);
        for (std::size_t sent = 0; sent < len; ) {
            const ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Blocking recv of exactly len bytes. Returns false on error/close.
    bool recv_all(void* buf, std::size_t len) const noexcept {
        auto* p = static_cast<std::byte*>(buf);
        for (std::size_t got = 0; got < len; ) {
            const ssize_t n = ::recv(fd_, p + got, len - got, 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// poll(POLLIN, timeout_ms). -1 = wait forever, 0 = non-blocking.
    /// Returns > 0 if data ready, 0 on timeout, < 0 on error.
    int poll_in(int timeout_ms) const noexcept {
        pollfd pfd{ fd_, POLLIN, 0 };
        return ::poll(&pfd, 1, timeout_ms);
    }

private:
    int fd_{-1};
};

}  // namespace corerat
