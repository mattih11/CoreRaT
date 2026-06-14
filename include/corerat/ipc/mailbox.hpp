#pragma once

#include "corerat/ipc/mailbox_config.hpp"
#ifdef CORERAT_IPC_EVL
#  include "corerat/ipc/evl/evl_backend.hpp"
#else
#  include "corerat/ipc/tims/tims_backend.hpp"
#endif
#include "corerat/messaging/wire_message.hpp"
#include "corerat/messaging/message_registry.hpp"
#include "corerat/messaging/registry_utils.hpp"
#include "corerat/messaging/message_id.hpp"
#include "corerat/platform/threading.hpp"
#include "corerat/platform/timestamp.hpp"
#include <optional>
#include <functional>
#include <concepts>
#include <span>
#include <array>
#include <cstring>

namespace corerat {

// Backend type alias — switches between TimsMailbox (STD) and EvlMailbox (EVL)
#ifdef CORERAT_IPC_EVL
using IpcMailbox = EvlMailbox;
#else
using IpcMailbox = TimsMailbox;
#endif

/**
 * @brief Strongly-typed mailbox for message-based communication
 *
 * A C++20 mailbox interface that provides compile-time type safety for
 * message operations over the TiMS IPC backend.
 *
 * @tparam MessageDefs Pack of MessageDefinition types this mailbox handles.
 */
template<typename... MessageDefs>
class Mailbox {
private:
    using Registry = MessageRegistry<MessageDefs...>;

    template<typename T, typename = void>
    struct is_message_definition : std::false_type {};

    template<typename T>
    struct is_message_definition<T, std::void_t<typename T::is_message_definition_tag>>
        : std::true_type {};

    static_assert((is_message_definition<MessageDefs>::value && ...),
                  "All template parameters must be MessageDefinition types");

public:
    // ========================================================================
    // Construction / Lifecycle
    // ========================================================================

    explicit Mailbox(const MailboxConfig& config)
        : config_(config)
#ifdef CORERAT_IPC_EVL
        , tims_(create_backend_config(config),
                config.cross_process ? IpcMailbox::Mode::Public
                                     : IpcMailbox::Mode::Local)
#else
        , tims_(create_backend_config(config))
#endif
        , running_(false) {
    }

    ~Mailbox() { stop(); }

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    Mailbox(Mailbox&& other) noexcept
        : config_(std::move(other.config_))
        , tims_(std::move(other.tims_))
        , running_(other.running_.load()) {
        other.running_ = false;
    }

    Mailbox& operator=(Mailbox&& other) noexcept {
        if (this != &other) {
            stop();
            config_  = std::move(other.config_);
            tims_    = std::move(other.tims_);
            running_ = other.running_.load();
            other.running_ = false;
        }
        return *this;
    }

    auto start() -> MailboxResult<void> {
        if (running_) {
            return MailboxError::AlreadyRunning;
        }
        if (tims_.initialize() != TimsResult::SUCCESS) {
            return MailboxError::NotInitialized;
        }
        running_ = true;
        return MailboxResult<void>();
    }

    void stop() {
        if (running_) {
            running_ = false;
            tims_.shutdown();
        }
    }

    bool is_running() const { return running_; }

    // ========================================================================
    // Type Registration Query
    // ========================================================================

    template<typename T>
    static constexpr bool is_registered = []() constexpr {
        if constexpr (requires { typename T::payload_type; }) {
            return Registry::template is_registered<typename T::payload_type>;
        } else {
            return Registry::template is_registered<T>;
        }
    }();

    static constexpr size_t num_message_types() { return sizeof...(MessageDefs); }

    // ========================================================================
    // Send Operations
    // ========================================================================

    /**
     * @brief Send a WireMessage<T> to a destination mailbox.
     * @tparam T Payload type — must be registered.
     */
    template<typename T>
        requires is_registered<T>
    auto send(WireMessage<T>& message, uint32_t dest_mailbox) -> MailboxResult<void> {
        if (!running_) {
            return MailboxError::NotRunning;
        }
        if (dest_mailbox == 0) {
            return MailboxError::InvalidDestination;
        }

        [[maybe_unused]] auto result = Registry::serialize(message);

        if (tims_.send(message, dest_mailbox) != TimsResult::SUCCESS) {
            return MailboxError::NetworkError;
        }
        return MailboxResult<void>();
    }

    /**
     * @brief Send a reply to a received request message.
     *
     * Destination is taken from request.header.src automatically.
     */
    template<typename RequestPayload, typename ReplyPayload>
        requires registry::is_request_payload_v<RequestPayload, Registry> &&
                 is_registered<ReplyPayload>
    auto send_reply(const WireMessage<RequestPayload>& request, ReplyPayload& reply)
        -> MailboxResult<void> {

        using RequestDef   = registry::find_message_def_t<RequestPayload, Registry>;
        using ExpectedReply = typename RequestDef::ReplyMessageDef::Payload;
        static_assert(std::is_same_v<ReplyPayload, ExpectedReply>,
                      "ReplyPayload must match RequestDef::ReplyMessageDef::Payload");

        WireMessage<ReplyPayload> reply_message;
        reply_message.payload           = reply;
        reply_message.header.timestamp  = Time::now();
        reply_message.header.src        = config_.mailbox_id;
        reply_message.header.dest       = request.header.src;

        return send(reply_message, request.header.src);
    }

    // ========================================================================
    // Receive Operations
    // ========================================================================

    /**
     * @brief Receive into a pre-allocated WireMessage<T> (zero-copy).
     */
    template<typename T>
        requires is_registered<T>
    bool receive(WireMessage<T>& message, Duration timeout = Seconds(1)) {
        if (!running_) return false;

        typename sertial::Message<WireMessage<T>>::buffer_type buffer;
        IpcMailbox::Metadata meta;
        auto bytes = tims_.receive_raw_bytes(
            std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size()),
            timeout, &meta);

        if (bytes <= 0) return false;

        auto result = sertial::Message<WireMessage<T>>::deserialize(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()),
                                       static_cast<size_t>(bytes)));
        if (!result) return false;

        message = std::move(*result);
        message.header.src  = meta.src;
        message.header.dest = meta.dest;
        return true;
    }

    /**
     * @brief Receive any registered message type using a visitor (1 s default timeout).
     */
    template<size_t BufferSize = Registry::max_message_size, typename Visitor>
    auto receive_any(Visitor&& visitor) -> MailboxResult<void> {
        return receive_any_for<BufferSize>(Seconds(1), std::forward<Visitor>(visitor));
    }

    /**
     * @brief Receive any registered message type with explicit timeout.
     */
    template<size_t BufferSize = Registry::max_message_size, typename Visitor>
    auto receive_any_for(Duration timeout, Visitor&& visitor) -> MailboxResult<void> {
        if (!running_) {
            return MailboxError::NotRunning;
        }

        std::array<std::byte, BufferSize> buffer;
        IpcMailbox::Metadata meta;
        auto bytes = tims_.receive_raw_bytes(buffer, timeout, &meta);

        if (bytes <= 0) {
            if (!timeout.is_positive() || bytes == -1) {
                return MailboxError::Timeout;
            }
            return MailboxError::NetworkError;
        }

        if (static_cast<size_t>(bytes) < sizeof(WireHeader)) {
            return MailboxError::InvalidMessage;
        }

        WireHeader header;
        std::memcpy(&header, buffer.data(), sizeof(WireHeader));

        bool success = Registry::visit(
            static_cast<MessageType>(header.msg_type),
            std::span<const std::byte>(buffer.data(), static_cast<size_t>(bytes)),
            [&visitor, &meta](auto&& wire_msg) {
                wire_msg.header.src  = meta.src;
                wire_msg.header.dest = meta.dest;
                std::forward<Visitor>(visitor)(std::forward<decltype(wire_msg)>(wire_msg));
            });

        return success ? MailboxResult<void>() : MailboxResult<void>(MailboxError::InvalidMessage);
    }

    // ========================================================================
    // Utility
    // ========================================================================

    auto clean() -> MailboxResult<void> {
        if (!running_) return MailboxError::NotRunning;

        std::array<std::byte, Registry::max_message_size> buffer;
        while (tims_.receive_raw_bytes(buffer, Milliseconds(10)) > 0) {
            // drain
        }
        return MailboxResult<void>();
    }

    uint32_t mailbox_id()         const { return config_.mailbox_id; }
    uint64_t messages_sent()      const { return tims_.get_messages_sent(); }
    uint64_t messages_received()  const { return tims_.get_messages_received(); }

private:
    static TimsConfig create_backend_config(const MailboxConfig& config) {
        TimsConfig tc;
        tc.mailbox_id   = config.mailbox_id;
        tc.max_msg_size = config.max_message_size;
        tc.priority     = config.send_priority;
        tc.realtime     = config.realtime;

        // Build name "mailbox_<id>" without heap allocation
        tc.mailbox_name = sertial::fixed_string<32>("mailbox_");
        char digits[11];
        int pos = 0;
        uint32_t v = config.mailbox_id;
        if (v == 0) { digits[pos++] = '0'; }
        else { while (v > 0) { digits[pos++] = '0' + static_cast<char>(v % 10); v /= 10; } }
        for (int i = pos - 1; i >= 0; --i) tc.mailbox_name.push_back(digits[i]);
        return tc;
    }

    MailboxConfig       config_;
    IpcMailbox          tims_;
    std::atomic<bool>   running_;
};

} // namespace corerat
