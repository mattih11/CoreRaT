#pragma once
/**
 * @file ping_pong/messages.hpp
 * @brief Shared message definitions for the CoreRaT ping-pong test.
 *
 * Two mailboxes communicate via corerat-router-tcp:
 *   ping_node (0x3001) → MSG_PING → pong_node (0x3002)
 *   pong_node (0x3002) → MSG_PONG → ping_node (0x3001)
 *
 * The payload structs are packed and appended after the TiMS FrameHeader.
 * Timestamps use std::chrono::steady_clock nanoseconds for RTT measurement.
 */

#include "corerat/ipc/tims/protocol.hpp"
#include <cstdint>

namespace corerat::pingpong {

// Mailbox IDs — in the user-defined range (0x3000–0x3FFF)
inline constexpr uint32_t PING_MBX_ID = 0x3001;
inline constexpr uint32_t PONG_MBX_ID = 0x3002;

// Frame type codes (application-defined, non-conflicting with tims_proto)
inline constexpr int8_t MSG_PING = 1;
inline constexpr int8_t MSG_PONG = 2;

// Ping payload — sent by ping_node to pong_node
struct PingPayload {
    uint64_t send_ns;   ///< steady_clock timestamp at send (ns)
    uint32_t seq;       ///< sequence number
} __attribute__((packed));

// Pong payload — sent by pong_node back to ping_node
struct PongPayload {
    uint64_t echo_ns;   ///< echo of PingPayload::send_ns (for RTT calc)
    uint64_t pong_ns;   ///< steady_clock timestamp when pong was sent (ns)
    uint32_t seq;       ///< echo of PingPayload::seq
} __attribute__((packed));

}  // namespace corerat::pingpong
