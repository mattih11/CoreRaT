/**
 * @file log_sink.hpp
 * @brief ILogSink interface and built-in sink implementations.
 *
 * Sinks are called from the in-band drain thread only — never from OOB
 * context.  They may use printf, TCP sockets, or any blocking I/O.
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 */
#pragma once

#include <cstdio>
#include <cstdint>
#include <sys/socket.h>   // ::send, MSG_NOSIGNAL
#include <unistd.h>

#include "corerat/logging/log_level.hpp"
#include "corerat/logging/rt_log_entry.hpp"

namespace corerat {

// ============================================================================
// Sink interface — type-erased, called in-band only
// ============================================================================

/// @brief Abstract log sink.  All write() implementations run in-band.
struct ILogSink {
    virtual void write(const RtLogEntry<>& entry) noexcept = 0;
    virtual ~ILogSink() = default;
};

// ============================================================================
// TerminalSink — writes to stdout with ANSI colour coding
// ============================================================================

/// @brief Coloured terminal output.
///
/// Format:  [HH:MM:SS.mmm] [LEVEL] [src=0xABCD] message
///
/// In-band only — uses printf.
class TerminalSink final : public ILogSink {
public:
    /// @param min_level  Entries below this level are silently dropped.
    explicit TerminalSink(LogLevel min_level = LogLevel::Trace) noexcept
        : min_level_{min_level} {}

    void write(const RtLogEntry<>& entry) noexcept override {
        if (entry.level > min_level_) return;

        // Decompose timestamp (ns) → HH:MM:SS.mmm — no glibc time functions
        uint64_t ts_ms  = entry.timestamp / 1'000'000ULL;
        uint64_t ms     = ts_ms % 1000ULL;
        uint64_t ts_s   = ts_ms / 1000ULL;
        uint64_t sec    = ts_s % 60ULL;
        uint64_t min    = (ts_s / 60ULL) % 60ULL;
        uint64_t hour   = (ts_s / 3600ULL) % 24ULL;

        const char* color = color_for(entry.level);
        const char* reset = "\033[0m";

        std::printf("%s[%02llu:%02llu:%02llu.%03llu] [%s] [src=%08X] %s%s\n",
                    color,
                    static_cast<unsigned long long>(hour),
                    static_cast<unsigned long long>(min),
                    static_cast<unsigned long long>(sec),
                    static_cast<unsigned long long>(ms),
                    to_string(entry.level),
                    entry.source_id,
                    entry.message.c_str(),
                    reset);
    }

private:
    LogLevel min_level_;

    static constexpr const char* color_for(LogLevel l) noexcept {
        switch (l) {
            case LogLevel::Fatal: return "\033[1;31m"; // bold red
            case LogLevel::Error: return "\033[0;31m"; // red
            case LogLevel::Warn:  return "\033[0;33m"; // yellow
            case LogLevel::Info:  return "\033[0;32m"; // green
            case LogLevel::Debug: return "\033[0;36m"; // cyan
            case LogLevel::Trace: return "\033[0;37m"; // white
        }
        return "";
    }
};

// ============================================================================
// TimsSink — sends as GDOS TiMS wire message for RaTGUI
// ============================================================================

/// @brief Forwards log entries as GDOS messages over the TiMS router.
///
/// Sends WireHeader + raw message bytes to the well-known GDOS mailbox address
/// (RackName GDOS/0 = 0x0C000000).  RaTGUI interprets these exactly like
/// RACK GDOS messages.
///
/// In-band only — uses send() syscall.
///
/// The sink borrows a raw file descriptor (the already-connected TiMS TCP
/// socket from a TimsMailbox or corerat-router-tcp).  Ownership stays with
/// the caller.
class TimsSink final : public ILogSink {
public:
    static constexpr uint32_t kGdosMailbox = 0x0C000000u; ///< RackName::create(GDOS, 0)

    /// @param fd         Connected TiMS TCP socket (borrowed, not closed here).
    /// @param src_id     Source mailbox address (WireHeader::src).
    /// @param min_level  Entries below this level are dropped.
    explicit TimsSink(int fd, uint32_t src_id,
                      LogLevel min_level = LogLevel::Warn) noexcept
        : fd_{fd}, src_id_{src_id}, min_level_{min_level} {}

    void write(const RtLogEntry<>& entry) noexcept override {
        if (entry.level > min_level_ || fd_ < 0) return;

        // Build a minimal RACK tims_msg_head + raw string payload.
        // We avoid sertial here because the GDOS wire format expects the
        // C-string payload without a length prefix — matching rack_gdos.h.
        const char*  msg    = entry.message.c_str();
        uint32_t     msglen = static_cast<uint32_t>(entry.message.size()) + 1; // incl. \0
        uint32_t     total  = kHeadLen + msglen;

        // tims_msg_head — little-endian fields, matches RACK layout
        uint8_t head[kHeadLen];
        write_u32(head,  0, static_cast<uint32_t>(
            static_cast<int8_t>(to_gdos_type(entry.level)))); // msg_type (sign-extended)
        write_u32(head,  4, total);                            // msg_size
        write_u64(head,  8, entry.timestamp);                  // timestamp
        write_u32(head, 16, 0u);                               // seq_number
        write_u32(head, 20, kGdosMailbox);                     // dest
        write_u32(head, 24, src_id_);                          // src
        write_u32(head, 28, 0u);                               // flags

        // Best-effort: drop silently on partial send (drain thread, non-RT)
        if (::send(fd_, head, kHeadLen, MSG_NOSIGNAL) != static_cast<ssize_t>(kHeadLen))
            return;
        ::send(fd_, msg, msglen, MSG_NOSIGNAL);
    }

private:
    static constexpr uint32_t kHeadLen = 32u; // sizeof(tims_msg_head)

    int      fd_;
    uint32_t src_id_;
    LogLevel min_level_;

    static void write_u32(uint8_t* p, int off, uint32_t v) noexcept {
        p[off+0] = static_cast<uint8_t>(v);
        p[off+1] = static_cast<uint8_t>(v >> 8);
        p[off+2] = static_cast<uint8_t>(v >> 16);
        p[off+3] = static_cast<uint8_t>(v >> 24);
    }
    static void write_u64(uint8_t* p, int off, uint64_t v) noexcept {
        write_u32(p, off,   static_cast<uint32_t>(v));
        write_u32(p, off+4, static_cast<uint32_t>(v >> 32));
    }
};

} // namespace corerat
