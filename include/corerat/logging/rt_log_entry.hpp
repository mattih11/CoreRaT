/**
 * @file rt_log_entry.hpp
 * @brief RtLogEntry — the fixed-size log record stored in the ring buffer.
 *
 * Parameterized on MaxMsgLen so callers can trade ring memory for message
 * length at compile time.
 *
 * Wire-serialized via sertial::Message<RtLogEntry<N>> — no manual padding
 * or sizeof games required.  On the wire, fixed_string serializes as
 * [uint32 length][MaxMsgLen bytes], so max_buffer_size is known at
 * compile time.
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 */
#pragma once

#include <cstdint>
#include <sertial/containers/fixed_string.hpp>

#include "corerat/platform/timestamp.hpp"
#include "corerat/logging/log_level.hpp"

namespace corerat {

/// @brief A single log record held in the ring buffer.
///
/// @tparam MaxMsgLen  Maximum characters in the message (excl. null terminator).
///                    Typical choices: 128 (tight-loop trace), 256 (default).
template<std::size_t MaxMsgLen = 256>
struct RtLogEntry {
    Timestamp                          timestamp{0};   ///< corerat::Time::now() at commit
    uint32_t                           source_id{0};   ///< Mailbox address (GDOS src field)
    LogLevel                           level{LogLevel::Debug};
    sertial::fixed_string<MaxMsgLen>   message{};      ///< Null-terminated, length-prefixed on wire
};

} // namespace corerat
