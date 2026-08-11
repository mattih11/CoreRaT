/**
 * @file log_level.hpp
 * @brief Log level enum and GDOS wire-type mapping for CoreRaT RT logging.
 */
#pragma once

#include <cstdint>

namespace corerat {

/// @brief Log severity levels — compatible with GDOS negative message types.
///
/// Fatal maps to GDOS_MSG_PRINT (-124), the highest GDOS level.
/// Debug and Trace both map to GDOS_MSG_DBG_DETAIL (-128) on the wire.
enum class LogLevel : uint8_t {
    Fatal = 0,  ///< Always shown — system-critical condition
    Error = 1,  ///< Recoverable error
    Warn  = 2,  ///< Unexpected but non-fatal condition
    Info  = 3,  ///< Normal operational status
    Debug = 4,  ///< Developer detail
    Trace = 5,  ///< Fine-grained trace (hot-path)
};

/// @brief Map to GDOS/TiMS negative message types for RaTGUI wire compatibility.
///
/// RACK GDOS uses int8_t message types: PRINT=-124 ... DBG_DETAIL=-128.
/// When packed into WireHeader::msg_type (uint32_t) the sign-extension
/// gives 0xFFFFFF84 ... 0xFFFFFF80 — the TiMS router preserves these.
constexpr int8_t to_gdos_type(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Fatal: return -124;  // GDOS_MSG_PRINT
        case LogLevel::Error: return -125;  // GDOS_MSG_ERROR
        case LogLevel::Warn:  return -126;  // GDOS_MSG_WARNING
        case LogLevel::Info:  return -127;  // GDOS_MSG_DBG_INFO
        default:              return -128;  // GDOS_MSG_DBG_DETAIL (Debug + Trace)
    }
}

constexpr const char* to_string(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
    }
    return "?????";
}

} // namespace corerat
