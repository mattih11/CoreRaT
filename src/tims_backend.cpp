#include "corerat/ipc/tims/tims_backend.hpp"
#include "corerat/ipc/tims/protocol.hpp"
#include <cerrno>

namespace corerat {

namespace {
constexpr std::string_view kRouterIp   = "127.0.0.1";
constexpr uint16_t         kRouterPort = 2000;
}  // namespace

// ============================================================================
// Construction / move
// ============================================================================

TimsMailbox::TimsMailbox(const TimsConfig& config) : config_(config) {}
TimsMailbox::~TimsMailbox() { shutdown(); }

TimsMailbox::TimsMailbox(TimsMailbox&& other) noexcept
    : config_(std::move(other.config_))
    , socket_(std::move(other.socket_))
    , is_initialized_(other.is_initialized_.load())
    , messages_sent_(other.messages_sent_.load())
    , messages_received_(other.messages_received_.load())
    , sequence_number_(other.sequence_number_) {
    other.is_initialized_ = false;
}

TimsMailbox& TimsMailbox::operator=(TimsMailbox&& other) noexcept {
    if (this != &other) {
        shutdown();
        config_            = std::move(other.config_);
        socket_            = std::move(other.socket_);
        is_initialized_    = other.is_initialized_.load();
        messages_sent_     = other.messages_sent_.load();
        messages_received_ = other.messages_received_.load();
        sequence_number_   = other.sequence_number_;
        other.is_initialized_ = false;
    }
    return *this;
}

// ============================================================================
// Lifecycle
// ============================================================================

TimsResult TimsMailbox::initialize() {
    if (is_initialized_) return TimsResult::SUCCESS;

    using namespace tims_proto;

    if (!socket_.connect(kRouterIp, kRouterPort))
        return TimsResult::ERROR_INIT;

    // Disable router watchdog
    const auto watchdog_frame = make_frame(MSG_ROUTER_DISABLE_WATCHDOG,
                                           0, config_.mailbox_id,
                                           static_cast<uint8_t>(config_.priority), 0, 0);
    if (!socket_.send_all(&watchdog_frame, sizeof(watchdog_frame))) {
        socket_.close(); return TimsResult::ERROR_INIT;
    }

    // Register mailbox; expect MSG_OK reply
    struct __attribute__((packed)) RegMsg {
        FrameHeader    frame;
        MbxInitPayload mbx;
    } reg{
        make_frame(MSG_ROUTER_MBX_INIT_WITH_REPLY,
                   0, config_.mailbox_id,
                   static_cast<uint8_t>(config_.priority), 0,
                   static_cast<uint32_t>(sizeof(MbxInitPayload))),
        { config_.mailbox_id }
    };

    if (!socket_.send_all(&reg, sizeof(reg))) {
        socket_.close(); return TimsResult::ERROR_INIT;
    }

    FrameHeader reply{};
    if (!socket_.recv_all(&reply, sizeof(reply)) || reply.type != MSG_OK) {
        socket_.close(); return TimsResult::ERROR_INIT;
    }

    is_initialized_ = true;
    return TimsResult::SUCCESS;
}

void TimsMailbox::shutdown() {
    if (!is_initialized_) return;
    socket_.close();
    is_initialized_ = false;
}

// ============================================================================
// Send
// ============================================================================

TimsResult TimsMailbox::send_raw(const void* data, std::size_t size,
                                  uint32_t dest_mailbox_id) {
    if (!is_initialized_ || !socket_.is_open()) return TimsResult::ERROR_NOT_INIT;
    if (!data || size == 0 || size > config_.max_msg_size)
        return TimsResult::ERROR_INVALID_MSG;

    using namespace tims_proto;
    const auto h = make_frame(0, dest_mailbox_id, config_.mailbox_id,
                              static_cast<uint8_t>(config_.priority),
                              sequence_number_++,
                              static_cast<uint32_t>(size));

    if (!socket_.send_all(&h, sizeof(h)))  return TimsResult::ERROR_SEND;
    if (!socket_.send_all(data, size))     return TimsResult::ERROR_SEND;

    messages_sent_.fetch_add(1, std::memory_order_relaxed);
    return TimsResult::SUCCESS;
}

// ============================================================================
// Receive
// ============================================================================

ssize_t TimsMailbox::receive_raw(void* buffer, std::size_t buffer_size,
                                  Duration timeout, TimsMetadata* metadata) {
    if (!is_initialized_ || !socket_.is_open()) return -1;

    using namespace tims_proto;

    const int timeout_ms = timeout.is_negative()
                         ? -1
                         : static_cast<int>(timeout.count_ns() / 1'000'000LL);

    for (;;) {
        if (socket_.poll_in(timeout_ms) <= 0) return -ETIMEDOUT;

        FrameHeader frame{};
        if (!socket_.recv_all(&frame, sizeof(frame))) return -1;

        if (frame.type == MSG_ROUTER_GET_STATUS) {
            const auto reply = make_frame(MSG_OK, frame.src, config_.mailbox_id,
                                          0, 0, 0);
            socket_.send_all(&reply, sizeof(reply));  // best-effort
            continue;
        }

        if (frame.msglen <= static_cast<uint32_t>(sizeof(FrameHeader))) continue;
        const auto body_len =
            frame.msglen - static_cast<uint32_t>(sizeof(FrameHeader));

        if (body_len > buffer_size) return -EMSGSIZE;

        if (!socket_.recv_all(buffer, body_len)) return -1;

        if (metadata) {
            metadata->src      = frame.src;
            metadata->dest     = frame.dest;
            metadata->seq_nr   = frame.seq_nr;
            metadata->priority = frame.priority;
            metadata->flags    = frame.flags;
        }

        messages_received_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<ssize_t>(body_len);
    }
}

bool TimsMailbox::has_message() const {
    if (!is_initialized_ || !socket_.is_open()) return false;
    return socket_.poll_in(0) > 0;
}

}  // namespace corerat
