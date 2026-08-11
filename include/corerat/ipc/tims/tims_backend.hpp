#pragma once

#include "corerat/ipc/ipc_types.hpp"  // TimsConfig (= IpcConfig), TimsResult (= IpcResult)
#include "corerat/ipc/tims/tcp_socket.hpp"
#include "corerat/messaging/wire_message.hpp"
#include "corerat/platform/duration.hpp"
#include "corerat/platform/timestamp.hpp"
#include <atomic>
#include <cstddef>
#include <span>
#include <array>
#include <optional>

namespace corerat {

// ============================================================================
// TimsMailbox — TCP-socket IPC backend (CORERAT_IPC_TIMS)
//
// Speaks the TiMS router wire protocol directly over POSIX TCP sockets.
// No RACK/TiMS library dependency.
//
// NOTE: receive_raw_bytes() uses POSIX poll()/recv() — in-band syscalls that
// demote EVL threads from OOB to in-band. Use EvlMailbox on the EVL platform.
// ============================================================================

class TimsMailbox {
public:
    explicit TimsMailbox(const TimsConfig& config);
    ~TimsMailbox();

    TimsMailbox(const TimsMailbox&)            = delete;
    TimsMailbox& operator=(const TimsMailbox&) = delete;
    TimsMailbox(TimsMailbox&&) noexcept;
    TimsMailbox& operator=(TimsMailbox&&) noexcept;

    TimsResult initialize();
    void       shutdown();

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id) {
        static_assert(is_wire_message_v<T>, "T must be a CoreRaT WireMessage type");
        if (!is_initialized_) return TimsResult::ERROR_NOT_INIT;
        auto result = corerat::serialize(message);
        auto view   = result.view();
        return send_raw(view.data(), view.size(), dest_mailbox_id);
    }

    bool has_message() const;

    uint32_t get_mailbox_id()          const { return config_.mailbox_id; }
    bool     is_initialized()          const { return is_initialized_; }
    uint64_t get_messages_sent()       const { return messages_sent_.load(); }
    uint64_t get_messages_received()   const { return messages_received_.load(); }

    struct TimsMetadata {
        uint32_t src{0};
        uint32_t dest{0};
        uint32_t seq_nr{0};
        uint8_t  priority{0};
        uint8_t  flags{0};
    };
    using Metadata = TimsMetadata;

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout) {
        return receive_raw(buffer.data(), buffer.size(), timeout, nullptr);
    }
    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout,
                              TimsMetadata* metadata) {
        return receive_raw(buffer.data(), buffer.size(), timeout, metadata);
    }

private:
    TimsResult send_raw(const void* data, std::size_t size, uint32_t dest_mailbox_id);
    ssize_t    receive_raw(void* buffer, std::size_t buffer_size, Duration timeout,
                           TimsMetadata* metadata = nullptr);

    TimsConfig            config_;
    TcpSocket             socket_;
    std::atomic<bool>     is_initialized_{false};
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_received_{0};
    uint32_t              sequence_number_{0};
};

}  // namespace corerat
