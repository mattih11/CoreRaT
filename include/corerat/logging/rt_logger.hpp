/**
 * @file rt_logger.hpp
 * @brief RtLogger<RingSize, MaxMsgLen> — the ring-buffer log engine.
 *
 * Thread model:
 *   - RT side  (commit):  called from OOB / RT threads → corerat::Mutex (EVL or std)
 *   - Drain side (run):   a plain std::thread, always in-band → printf, TCP send
 *
 * The drain thread is deliberately a std::thread, NOT corerat::Thread, because
 * corerat::Thread on EVL calls evl_attach_self() making it OOB-capable — which
 * would then forbid the glibc I/O the sinks need.
 *
 * Ring-full policy: newest entry is dropped (RT thread never blocks or allocates).
 *
 * @tparam RingSize   Number of entry slots (must be power-of-2, default 64).
 * @tparam MaxMsgLen  Maximum characters per message (default 256).
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 * @realtime commit() is OOB-safe on EVL platform.
 */
#pragma once

#include <array>
#include <atomic>
#include <thread>           // std::thread — in-band drain, NOT corerat::Thread
#include <mutex>            // std::mutex  — guards sink array (init-time only)
#include <cstddef>

#include "corerat/platform/threading.hpp"  // corerat::Mutex, corerat::ConditionVariable
#include "corerat/platform/timestamp.hpp"  // corerat::Time::now()
#include "corerat/logging/log_level.hpp"
#include "corerat/logging/rt_log_entry.hpp"
#include "corerat/logging/rt_logger_base.hpp"
#include "corerat/logging/rt_log_stream.hpp"
#include "corerat/logging/log_sink.hpp"
#include <sertial/containers/rt_format.hpp>

namespace corerat {

/// @brief RT-safe ring-buffer logger.
///
/// Typical module setup:
/// @code
///   corerat::RtLogger<64> logger_{mailbox_id_};
///   corerat::TerminalSink terminal_sink_{};
///   // in moduleInit():
///   logger_.add_sink(&terminal_sink_);
///   logger_.start_drain();
///   // in moduleCleanup():
///   logger_.stop_drain();
/// @endcode
template<std::size_t RingSize = 64, std::size_t MaxMsgLen = 256>
class RtLogger final : public RtLoggerBase {
    static_assert((RingSize & (RingSize - 1)) == 0, "RingSize must be a power of 2");

    using Entry = RtLogEntry<MaxMsgLen>;

public:
    /// @param source_id  Mailbox address used as GDOS src field.
    /// @param min_level  Initial minimum log level.
    explicit RtLogger(uint32_t source_id,
                      LogLevel min_level = LogLevel::Debug) noexcept
        : source_id_{source_id}
        , level_{min_level}
    {}

    ~RtLogger() override {
        stop_drain();
    }

    RtLogger(const RtLogger&)            = delete;
    RtLogger& operator=(const RtLogger&) = delete;

    // -------------------------------------------------------------------------
    // RtLoggerBase interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogLevel level() const noexcept override {
        return level_.load(std::memory_order_relaxed);
    }

    /// @brief Commit a message to the ring buffer.
    ///
    /// OOB-safe: uses corerat::Mutex (→ evl_mutex on EVL).
    /// Drops entry silently when ring is full — never blocks the RT thread.
    void commit(LogLevel lvl, const char* msg, std::size_t len) noexcept override {
        UniqueLock lock{ring_mutex_};

        if (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed) >= RingSize) {
            return;  // ring full — drop newest, preserve RT timing
        }

        Entry& e     = ring_[head_.load(std::memory_order_relaxed) & (RingSize - 1)];
        e.timestamp  = Time::now();
        e.source_id  = source_id_;
        e.level      = lvl;
        e.message.clear();
        sertial::rt::append(e.message, msg, len);

        head_.fetch_add(1, std::memory_order_release);  // atomic: visible to drain thread
        // No notify needed — drain thread polls head_ with a 1ms sleep.
    }

    // -------------------------------------------------------------------------
    // Stream factory — returns a temporary RtLogStream
    // -------------------------------------------------------------------------

    /// @brief Build a stream for the given level.
    ///
    /// Prefer the RTLOG_* macros in logging.hpp over calling this directly.
    [[nodiscard]] RtLogStream<MaxMsgLen> stream(LogLevel lvl) noexcept {
        return RtLogStream<MaxMsgLen>{*this, lvl};
    }

    // -------------------------------------------------------------------------
    // Sink management — call before start_drain(), not RT-safe
    // -------------------------------------------------------------------------

    static constexpr std::size_t kMaxSinks = 4;

    /// @brief Register a sink.  Must be called before start_drain().
    /// @param sink  Borrowed pointer — lifetime must exceed the logger's.
    void add_sink(ILogSink* sink) noexcept {
        std::lock_guard<std::mutex> g{sink_mutex_};
        if (sink_count_ < kMaxSinks && sink) {
            sinks_[sink_count_++] = sink;
        }
    }

    // -------------------------------------------------------------------------
    // Drain thread control
    // -------------------------------------------------------------------------

    /// @brief Start the in-band drain thread.
    ///
    /// Must be called from non-RT context after all sinks are registered.
    /// Uses std::thread so it stays in-band even on EVL builds.
    void start_drain() {
        if (drain_running_.exchange(true)) return;  // already running
        drain_thread_ = std::thread([this] { drain_loop(); });
    }

    /// @brief Stop the drain thread and flush remaining entries.
    ///
    /// Blocks until the drain thread exits.  Safe to call multiple times.
    void stop_drain() noexcept {
        if (!drain_running_.exchange(false)) return;

        if (drain_thread_.joinable()) {
            drain_thread_.join();
        }
    }

    // -------------------------------------------------------------------------
    // Runtime level adjustment
    // -------------------------------------------------------------------------

    void set_level(LogLevel l) noexcept {
        level_.store(l, std::memory_order_relaxed);
    }

private:
    // ---- Ring buffer --------------------------------------------------------
    std::array<Entry, RingSize>  ring_{};
    std::atomic<std::size_t>     head_{0};  // written by commit() (OOB); read by drain (in-band)
    std::atomic<std::size_t>     tail_{0};  // written by drain thread; read by commit() for full-check

    // ring_mutex_ guards concurrent OOB writers in commit().
    // The drain thread does NOT lock ring_mutex_ — it reads head_ atomically.
    corerat::Mutex               ring_mutex_;

    // ---- Metadata -----------------------------------------------------------
    uint32_t                     source_id_;
    std::atomic<LogLevel>        level_;

    // ---- Sinks — fixed array, no heap --------------------------------------
    std::array<ILogSink*, kMaxSinks> sinks_{};
    std::size_t                      sink_count_{0};
    std::mutex                       sink_mutex_{};  // guards add_sink only

    // ---- Drain thread — plain std::thread (in-band) ------------------------
    std::thread       drain_thread_;
    std::atomic<bool> drain_running_{false};

    void drain_loop() {
        while (drain_running_.load(std::memory_order_relaxed)) {
            // head_ is updated atomically by commit(); no evl_mutex needed here.
            if (head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const std::size_t t = tail_.load(std::memory_order_relaxed);
            Entry local = ring_[t & (RingSize - 1)];
            tail_.store(t + 1, std::memory_order_release);
            dispatch(local);
        }

        // Drain any remaining entries after stop signal
        while (head_.load(std::memory_order_acquire) != tail_.load(std::memory_order_relaxed)) {
            const std::size_t t = tail_.load(std::memory_order_relaxed);
            Entry local = ring_[t & (RingSize - 1)];
            tail_.store(t + 1, std::memory_order_release);
            dispatch(local);
        }
    }

    void dispatch(const Entry& e) noexcept {
        // ILogSink::write takes RtLogEntry<256>.  When MaxMsgLen != 256 we
        // build a RtLogEntry<256> copy — this copy happens in-band so heap
        // allocation or stack usage is fine.
        RtLogEntry<256> out{};
        out.timestamp  = e.timestamp;
        out.source_id  = e.source_id;
        out.level      = e.level;
        sertial::rt::append(out.message, e.message.c_str(), e.message.size());

        for (std::size_t i = 0; i < sink_count_; ++i) {
            sinks_[i]->write(out);
        }
    }
};

} // namespace corerat
