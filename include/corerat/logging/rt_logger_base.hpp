/**
 * @file rt_logger_base.hpp
 * @brief Type-erased base class for RtLogger<RingSize, MaxMsgLen>.
 *
 * RtLogStream holds a reference to RtLoggerBase so its template parameters
 * do not leak into every translation unit that uses the stream API.
 *
 * Concrete RtLogger<> overrides commit() and level().
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 * @realtime commit() is OOB-safe on EVL platform.
 */
#pragma once

#include <cstddef>
#include <sertial/containers/fixed_string.hpp>

#include "corerat/logging/log_level.hpp"

namespace corerat {

/// @brief Type-erased interface for the RtLogStream → RtLogger commit path.
///
/// Keeping this non-template means RtLogStream<MaxMsgLen> can store a plain
/// reference without dragging in RingSize as a second template argument.
class RtLoggerBase {
public:
    RtLoggerBase() = default;
    virtual ~RtLoggerBase() = default;

    RtLoggerBase(const RtLoggerBase&) = delete;
    RtLoggerBase& operator=(const RtLoggerBase&) = delete;

    /// @brief Current minimum log level.  Checked by RtLogStream before
    ///        calling operator<< — filtered streams are zero-cost.
    [[nodiscard]] virtual LogLevel level() const noexcept = 0;

    /// @brief Commit a completed message to the ring buffer.
    ///
    /// Called from ~RtLogStream() with the fully assembled message.
    /// Must be OOB-safe: no heap, no throw, no glibc I/O.
    ///
    /// @param lvl   Severity of the entry.
    /// @param msg   Null-terminated message string.
    /// @param len   Number of characters in msg (excl. null terminator).
    virtual void commit(LogLevel lvl, const char* msg, std::size_t len) noexcept = 0;
};

} // namespace corerat
