#pragma once

#include <cstdint>
#include <type_traits>
#include <span>
#include <cstring>

#include <sertial/sertial.hpp>

namespace corerat {

// ============================================================================
// Wire Header — matches RACK tims_msg_head layout exactly
// ============================================================================

struct WireHeader {
    uint32_t msg_type;    ///< MessageDefinition::full_id()
    uint32_t msg_size;    ///< total serialized bytes (header + payload)
    uint64_t timestamp;   ///< nanoseconds since epoch (single source of truth)
    uint32_t seq_number;
    uint32_t dest;        ///< destination mailbox address
    uint32_t src;         ///< source mailbox address
    uint32_t flags;
};

using MessageType = uint32_t;

// ============================================================================
// WireMessage<PayloadT> — header + payload aggregate
// ============================================================================

template<typename PayloadT>
struct WireMessage {
    WireHeader header;
    PayloadT   payload;

    using payload_type = PayloadT;
};

// ============================================================================
// Type Traits
// ============================================================================

template<typename T>
struct is_wire_message : std::false_type {};

template<typename P>
struct is_wire_message<WireMessage<P>> : std::true_type {};

template<typename T>
inline constexpr bool is_wire_message_v = is_wire_message<T>::value;

template<typename T>
struct wire_message_payload;

template<typename P>
struct wire_message_payload<WireMessage<P>> {
    using type = P;
};

template<typename T>
using wire_message_payload_t = typename wire_message_payload<T>::type;

// ============================================================================
// Serialization helpers
// ============================================================================

template<typename T>
auto serialize(T& message) -> typename sertial::Message<T>::Result {
    static_assert(is_wire_message_v<T>, "T must be a CoreRaT WireMessage type");

    if constexpr (requires { T::message_type; }) {
        message.header.msg_type = static_cast<uint32_t>(T::message_type);
    }

    auto result = sertial::Message<T>::serialize(message);
    message.header.msg_size = static_cast<uint32_t>(result.size);
    return result;
}

template<typename T>
auto deserialize(std::span<const std::byte> data) -> sertial::DeserializeResult<T> {
    static_assert(is_wire_message_v<T>, "T must be a CoreRaT WireMessage type");
    return sertial::Message<T>::deserialize(data);
}

} // namespace corerat
