#pragma once

/**
 * @file tims/protocol.hpp
 * @brief TiMS TCP router protocol constants and frame header.
 *
 * Internal header — not part of the public CoreRaT API.
 * Replaces all RACK/TiMS C headers in tims_backend.cpp.
 *
 * Wire layout: FrameHeader (16 bytes) followed by WireMessage<T> bytes.
 * The FrameHeader is the TCP routing envelope; it is not part of the
 * CoreRaT message model and is stripped on receive.
 */

#include <cstdint>
#include <cstring>

namespace corerat::tims_proto {

// ============================================================================
// Router message types (from tims_router.h)
// ============================================================================

inline constexpr int8_t MSG_OK                         =  0;
inline constexpr int8_t MSG_ERROR                      = -1;
inline constexpr int8_t MSG_ROUTER_MBX_INIT_WITH_REPLY = 13;
inline constexpr int8_t MSG_ROUTER_MBX_DELETE          = 14;
inline constexpr int8_t MSG_ROUTER_GET_STATUS          = 17;
inline constexpr int8_t MSG_ROUTER_DISABLE_WATCHDOG    = 19;

// Inter-router peer protocol (Gap 5)
// Sent only between router daemons, never by client nodes.
inline constexpr int8_t MSG_ROUTER_PEER_HELLO    = 20;  ///< Identify as peer router; no payload
inline constexpr int8_t MSG_ROUTER_PEER_REGISTER = 21;  ///< Advertise local mailbox; payload: MbxInitPayload
inline constexpr int8_t MSG_ROUTER_PEER_DELETE   = 22;  ///< De-advertise local mailbox; payload: MbxInitPayload

// ============================================================================
// Frame header — identical layout to tims_msg_head (16 bytes, packed)
// ============================================================================

inline constexpr uint8_t FRAME_LE_FLAG = 0x03;  // head + body are little-endian

struct FrameHeader {
    uint8_t  flags;     // FRAME_LE_FLAG on LE targets
    int8_t   type;      // MSG_* constant
    uint8_t  priority;
    uint8_t  seq_nr;
    uint32_t dest;      // destination mailbox ID
    uint32_t src;       // source mailbox ID
    uint32_t msglen;    // total bytes including this header
} __attribute__((packed));

static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

// ============================================================================
// MBX_INIT payload (4-byte body appended after FrameHeader)
// ============================================================================

struct MbxInitPayload {
    uint32_t mbx;
} __attribute__((packed));

// ============================================================================
// Helper: build a FrameHeader on the stack
// ============================================================================

inline FrameHeader make_frame(int8_t   type,
                               uint32_t dest,
                               uint32_t src,
                               uint8_t  priority,
                               uint8_t  seq,
                               uint32_t body_len) noexcept {
    FrameHeader h{};
    h.flags    = FRAME_LE_FLAG;
    h.type     = type;
    h.priority = priority;
    h.seq_nr   = seq;
    h.dest     = dest;
    h.src      = src;
    h.msglen   = static_cast<uint32_t>(sizeof(FrameHeader)) + body_len;
    return h;
}

}  // namespace corerat::tims_proto
