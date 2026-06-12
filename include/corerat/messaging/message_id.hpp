#pragma once

#include <cstdint>
#include <type_traits>

namespace corerat {

// ============================================================================
// Message ID Structure: 0xPSMM
// P  = Prefix (1 byte)      - System(0x00) or UserDefined(0x01+)
// S  = SubPrefix (1 byte)   - Category within prefix
// MM = Message ID (2 bytes) - Specific message within category
// ============================================================================

enum class MessagePrefix : uint8_t {
    System = 0x00,
    UserDefined = 0x01
};

enum class SystemSubPrefix : uint8_t {
    Subscription = 0x00,
    Control = 0x01,
    Reserved = 0xFF
};

enum class UserSubPrefix : uint8_t {
    Data = 0x00,
    Commands = 0x01,
    Events = 0x02,
    GetData = 0x03,
    GetNextData = 0x04,
    Custom = 0x05
};

constexpr uint32_t make_message_id(uint8_t prefix, uint8_t subprefix, uint16_t id) {
    return (static_cast<uint32_t>(prefix) << 24) |
           (static_cast<uint32_t>(subprefix) << 16) |
           static_cast<uint32_t>(id);
}

constexpr uint32_t system_message_id(SystemSubPrefix subprefix, uint16_t id) {
    return make_message_id(
        static_cast<uint8_t>(MessagePrefix::System),
        static_cast<uint8_t>(subprefix),
        id
    );
}

constexpr uint32_t user_message_id(UserSubPrefix subprefix, uint16_t id) {
    return make_message_id(
        static_cast<uint8_t>(MessagePrefix::UserDefined),
        static_cast<uint8_t>(subprefix),
        id
    );
}

// ============================================================================
// Message Definition - Compile-time message metadata
// ============================================================================

struct DefaultMessageDef {
    static constexpr MessagePrefix prefix = MessagePrefix::UserDefined;
    static constexpr UserSubPrefix user_subprefix = UserSubPrefix::Data;
    static constexpr SystemSubPrefix system_subprefix = SystemSubPrefix::Reserved;
    static constexpr uint16_t id = 0;
};

static constexpr uint16_t MAX_MESSAGE_ID = 0x7FFF;

template<
    typename PayloadT,
    MessagePrefix Prefix_ = DefaultMessageDef::prefix,
    auto SubPrefix_ = DefaultMessageDef::user_subprefix,
    uint16_t ID_ = DefaultMessageDef::id,
    typename ReplyT = void
>
struct MessageDefinition {
    using is_message_definition_tag = void;

    using Payload = PayloadT;
    static constexpr MessagePrefix prefix = Prefix_;
    static constexpr uint16_t local_id = ID_;

    static constexpr uint8_t subprefix = []() constexpr {
        if constexpr (std::is_same_v<decltype(SubPrefix_), uint8_t>) {
            return SubPrefix_;
        } else if constexpr (Prefix_ == MessagePrefix::System) {
            if constexpr (std::is_same_v<decltype(SubPrefix_), SystemSubPrefix>) {
                return static_cast<uint8_t>(SubPrefix_);
            } else {
                return static_cast<uint8_t>(DefaultMessageDef::system_subprefix);
            }
        } else {
            if constexpr (std::is_same_v<decltype(SubPrefix_), UserSubPrefix>) {
                return static_cast<uint8_t>(SubPrefix_);
            } else {
                return static_cast<uint8_t>(DefaultMessageDef::user_subprefix);
            }
        }
    }();

    static_assert(
        (Prefix_ == MessagePrefix::System && std::is_same_v<decltype(SubPrefix_), SystemSubPrefix>) ||
        (Prefix_ == MessagePrefix::UserDefined && std::is_same_v<decltype(SubPrefix_), UserSubPrefix>) ||
        (std::is_same_v<decltype(SubPrefix_), std::nullptr_t>) ||
        (std::is_same_v<decltype(SubPrefix_), uint8_t>),
        "SubPrefix type must match Prefix type (or be uint8_t for internal use)");

    static_assert(
        std::is_same_v<ReplyT, void> ||
        (Prefix_ == MessagePrefix::System && subprefix == static_cast<uint8_t>(SystemSubPrefix::Control)) ||
        (Prefix_ == MessagePrefix::System && subprefix == static_cast<uint8_t>(SystemSubPrefix::Subscription)) ||
        (Prefix_ == MessagePrefix::UserDefined && subprefix == static_cast<uint8_t>(UserSubPrefix::Commands)) ||
        (Prefix_ == MessagePrefix::UserDefined && subprefix == static_cast<uint8_t>(UserSubPrefix::GetData)) ||
        (Prefix_ == MessagePrefix::UserDefined && subprefix == static_cast<uint8_t>(UserSubPrefix::GetNextData)),
        "non-void Reply type only allowed for System::Control, System::Subscription, "
        "UserDefined::Commands, UserDefined::GetData, or UserDefined::GetNextData messages");

    static_assert(std::is_same_v<ReplyT, void> || ID_ == 0 || ID_ <= MAX_MESSAGE_ID,
                  "Message ID for request messages must be <= 0x7FFF");
    static_assert(std::is_same_v<ReplyT, void> || static_cast<int16_t>(ID_) >= 0,
                  "Manually specified reply IDs not allowed (derived automatically)");

    using ReplyPayload = ReplyT;
    using ReplyMessageDef = std::conditional_t<!std::is_same_v<ReplyT, void>,
        MessageDefinition<ReplyT, Prefix_, SubPrefix_,
                          static_cast<uint16_t>(-static_cast<int16_t>(ID_)), void>,
        void>;
    static constexpr bool is_request = !std::is_same_v<ReplyT, void>;
    static constexpr bool is_reply   = std::is_same_v<ReplyT, void>
                                       && (ID_ != 0)
                                       && (static_cast<int16_t>(ID_) < 0);
    static constexpr bool has_reply  = !std::is_same_v<ReplyT, void>;

    static constexpr uint16_t request_id = is_reply
        ? static_cast<uint16_t>(-static_cast<int16_t>(ID_)) : 0;

    static constexpr bool needs_auto_id = (ID_ == 0);
};

// ============================================================================
// Convenience namespace wrappers
// ============================================================================

namespace Message {
    template<typename T>
    using Data = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Data>;

    template<typename T>
    using Command = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Commands>;

    template<typename T>
    using Event = MessageDefinition<T, MessagePrefix::UserDefined, UserSubPrefix::Events>;
} // namespace Message

} // namespace corerat
