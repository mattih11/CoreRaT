#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>

namespace corerat {

// ============================================================================
// Error types
// ============================================================================

enum class MailboxError {
    NotInitialized,
    InvalidMessage,
    Timeout,
    QueueFull,
    QueueEmpty,
    SerializationError,
    NetworkError,
    InvalidDestination,
    AlreadyRunning,
    NotRunning
};

constexpr const char* to_string(MailboxError error) {
    switch (error) {
        case MailboxError::NotInitialized:     return "Mailbox not initialized";
        case MailboxError::InvalidMessage:     return "Invalid message";
        case MailboxError::Timeout:            return "Operation timed out";
        case MailboxError::QueueFull:          return "Message queue is full";
        case MailboxError::QueueEmpty:         return "Message queue is empty";
        case MailboxError::SerializationError: return "Serialization failed";
        case MailboxError::NetworkError:       return "Network error";
        case MailboxError::InvalidDestination: return "Invalid destination mailbox";
        case MailboxError::AlreadyRunning:     return "Mailbox is already running";
        case MailboxError::NotRunning:         return "Mailbox is not running";
    }
    return "Unknown error";
}

// ============================================================================
// Result type (C++20 alternative to std::expected)
// ============================================================================

template<typename T>
class MailboxResult {
private:
    std::optional<T>            value_;
    std::optional<MailboxError> error_;

public:
    MailboxResult(T value) : value_(std::move(value)), error_(std::nullopt) {}
    MailboxResult(MailboxError error) : value_(std::nullopt), error_(error) {}

    explicit operator bool() const { return value_.has_value(); }
    bool has_value() const { return value_.has_value(); }

    T& operator*() &              { return *value_; }
    const T& operator*() const &  { return *value_; }
    T&& operator*() &&            { return std::move(*value_); }

    T* operator->()               { return &(*value_); }
    const T* operator->() const   { return &(*value_); }

    T& value() &                  { return *value_; }
    const T& value() const &      { return *value_; }
    T&& value() &&                { return std::move(*value_); }

    MailboxError error() const    { return *error_; }
};

template<>
class MailboxResult<void> {
private:
    std::optional<MailboxError> error_;

public:
    MailboxResult() : error_(std::nullopt) {}
    MailboxResult(MailboxError error) : error_(error) {}

    static MailboxResult<void> ok()                { return MailboxResult<void>(); }
    static MailboxResult<void> error(MailboxError e) { return MailboxResult<void>(e); }

    explicit operator bool() const { return !error_.has_value(); }
    bool has_value() const { return !error_.has_value(); }

    MailboxError get_error() const { return *error_; }
};

// ============================================================================
// Mailbox Configuration
// ============================================================================

struct MailboxConfig {
    uint32_t mailbox_id;
    size_t   message_slots    = 10;
    size_t   max_message_size = 4096;
    uint8_t  send_priority    = 10;
    bool     realtime         = false;
    /// EVL only: use Mode::Public (cross-process SHM ring).
    /// On STD/TIMS this field is ignored — the TCP router handles routing.
    bool     cross_process    = false;

    /// EVL only: use Mode::Network (OOB UDP over IPv4, cross-machine real-time).
    /// Requires `evl net -ei <iface>` on the network device before use.
    /// On STD/TIMS this field is ignored.
    /// Port assignment: 42000 + mailbox_id (matches EvlNetworkConfig::kOobBasePort).
    bool     network          = false;

    struct NetworkPeer {
        uint32_t mailbox_id{0};
        char     ip[16]{};  ///< IPv4 dotted-decimal, e.g. "10.10.10.11"
    };
    static constexpr uint8_t kMaxNetworkPeers = 8;
    std::array<NetworkPeer, kMaxNetworkPeers> network_peers{};
    uint8_t  network_peer_count{0};
};

} // namespace corerat
