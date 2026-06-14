#pragma once
/**
 * @file ping_pong/messages.hpp
 * @brief Shared message definitions for the CoreRaT ping-pong test.
 *
 * Uses MessageDefinition<> and Mailbox<> so the same source compiles against
 * both IPC backends:
 *
 *   CORERAT_IPC_TIMS  — TCP socket → corerat-router-tcp (two processes, port 2000)
 *   CORERAT_IPC_EVL   — EVL OOB ring buffer → cross-process SHM (no router on
 *                        data path; deterministic names from mailbox_id)
 *
 * The transport is selected at cmake configure time via CORERAT_IPC.
 * Module code calls Mailbox<>::send() / receive_any_for() regardless of backend.
 */

#include "corerat/ipc/mailbox.hpp"
#include "corerat/ipc/mailbox_config.hpp"
#include "corerat/messaging/message_id.hpp"
#include "corerat/messaging/wire_message.hpp"
#include <cstdint>

namespace corerat::pingpong {

// Mailbox IDs — user-defined range (0x3000–0x3FFF)
inline constexpr uint32_t PING_MBX_ID = 0x3001;
inline constexpr uint32_t PONG_MBX_ID = 0x3002;

// ---- Payloads ----------------------------------------------------------------

struct PingPayload {
    uint64_t send_ns;   ///< steady_clock ns at send (for RTT)
    uint32_t seq;       ///< sequence number
};

struct PongPayload {
    uint64_t echo_ns;   ///< echo of PingPayload::send_ns
    uint64_t pong_ns;   ///< steady_clock ns when pong was sent
    uint32_t seq;       ///< echo of PingPayload::seq
};

// ---- MessageDefinition types -------------------------------------------------

using PingDef = MessageDefinition<PingPayload,
    MessagePrefix::UserDefined, UserSubPrefix::Data, 1>;

using PongDef = MessageDefinition<PongPayload,
    MessagePrefix::UserDefined, UserSubPrefix::Data, 2>;

// ---- Mailbox type alias ------------------------------------------------------

using PingPongMailbox = Mailbox<PingDef, PongDef>;

// ---- Generous buffer size for both payload types ----------------------------
inline constexpr std::size_t kMsgBufSize = 256;

}  // namespace corerat::pingpong
