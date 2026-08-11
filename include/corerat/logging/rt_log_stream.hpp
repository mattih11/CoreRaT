/**
 * @file rt_log_stream.hpp
 * @brief RtLogStream — the RAII << chain that assembles a log entry.
 *
 * Typical usage (via macros defined in logging.hpp):
 * @code
 *   RTLOG_INFO(logger_)  << "speed=" << speed_ << " hdg=" << hdg_;
 *   RTLOG_ERROR(logger_) << "fault code=0x" << corerat::RtHex{code};
 * @endcode
 *
 * The temporary RtLogStream is constructed, operator<< chains fill its
 * internal sertial::fixed_string<MaxMsgLen>, then the semicolon triggers
 * the destructor which calls RtLoggerBase::commit() atomically.
 *
 * When the entry's level is above the logger's current minimum the
 * stream is marked inactive: all operator<< overloads become no-ops and
 * commit() is skipped — zero cost in the hot path.
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 * @realtime OOB-safe on EVL platform — no heap, no throw, no glibc I/O.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <sertial/containers/fixed_string.hpp>

#include "corerat/logging/log_level.hpp"
#include "corerat/logging/rt_logger_base.hpp"
#include "corerat/logging/detail/rt_append.hpp"

namespace corerat {

// ============================================================================
// RtHex / RtHex32 — wrapper tag for hex-formatted integers
// ============================================================================

/// @brief Tag: render uint64_t as uppercase hex in an RtLogStream.
/// @code
///   rtlog << "code=" << RtHex{fault_code};
/// @endcode
struct RtHex   { uint64_t value; bool prefix{true}; };
struct RtHex32 { uint32_t value; bool prefix{true}; };

// ============================================================================
// RtLogStream
// ============================================================================

/// @brief RAII log-record builder.
///
/// @tparam MaxMsgLen  Maximum message length — must match the RtLogger's
///                    MaxMsgLen parameter.
template<std::size_t MaxMsgLen = 256>
class RtLogStream {
public:
    /// @brief Construct an active or inactive stream.
    ///
    /// Inactive when lvl > logger.level() — all << calls are no-ops.
    RtLogStream(RtLoggerBase& logger, LogLevel lvl) noexcept
        : logger_{logger}
        , level_{lvl}
        , active_{lvl <= logger.level()}
    {}

    /// @brief Commit on destruction — the only place commit() is called.
    ~RtLogStream() noexcept {
        if (active_) {
            logger_.commit(level_, buf_.c_str(), buf_.size());
        }
    }

    // Non-copyable, non-movable — always a temporary
    RtLogStream(const RtLogStream&)            = delete;
    RtLogStream& operator=(const RtLogStream&) = delete;
    RtLogStream(RtLogStream&&)                 = delete;
    RtLogStream& operator=(RtLogStream&&)      = delete;

    // -------------------------------------------------------------------------
    // operator<< overloads — OOB-safe
    // -------------------------------------------------------------------------

    RtLogStream& operator<<(const char* s) noexcept {
        if (active_) detail::append_rt(buf_, s);
        return *this;
    }

    RtLogStream& operator<<(int32_t v) noexcept {
        if (active_) detail::append_rt(buf_, static_cast<int64_t>(v));
        return *this;
    }

    RtLogStream& operator<<(uint32_t v) noexcept {
        if (active_) detail::append_rt(buf_, static_cast<uint64_t>(v));
        return *this;
    }

    RtLogStream& operator<<(int64_t v) noexcept {
        if (active_) detail::append_rt(buf_, v);
        return *this;
    }

    RtLogStream& operator<<(uint64_t v) noexcept {
        if (active_) detail::append_rt(buf_, v);
        return *this;
    }

    RtLogStream& operator<<(float v) noexcept {
        if (active_) detail::append_rt(buf_, v);
        return *this;
    }

    RtLogStream& operator<<(double v) noexcept {
        if (active_) detail::append_rt(buf_, v);
        return *this;
    }

    RtLogStream& operator<<(bool v) noexcept {
        if (active_) detail::append_rt(buf_, v);
        return *this;
    }

    /// @brief Hex-formatted integer.
    RtLogStream& operator<<(RtHex h) noexcept {
        if (active_) detail::append_rt_hex(buf_, h.value, h.prefix);
        return *this;
    }

    RtLogStream& operator<<(RtHex32 h) noexcept {
        if (active_) detail::append_rt_hex(buf_, static_cast<uint64_t>(h.value), h.prefix);
        return *this;
    }

    /// @brief Append from an existing sertial::fixed_string of any capacity.
    template<std::size_t OtherN>
    RtLogStream& operator<<(const sertial::fixed_string<OtherN>& s) noexcept {
        if (active_) detail::append_rt(buf_, s.c_str(), s.size());
        return *this;
    }

private:
    RtLoggerBase&                    logger_;
    LogLevel                         level_;
    bool                             active_;
    sertial::fixed_string<MaxMsgLen> buf_{};
};

} // namespace corerat
