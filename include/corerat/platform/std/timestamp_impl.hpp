/**
 * @file std/timestamp_impl.hpp
 * @brief Standard C++ clock/time backend for CoreRaT
 *
 * Implements the Time utility class using std::chrono clocks
 * and std::this_thread::sleep_for.
 *
 * This file is included by corerat/platform/timestamp.hpp when
 * CORERAT_PLATFORM_STD is defined (default).
 *
 * Not intended for direct inclusion by user code.
 */

#pragma once

#include <chrono>
#include <thread>
#include <ctime>

namespace corerat {

/**
 * @brief Time utility class (std::chrono backend)
 */
class Time {
public:
    enum class ClockSource {
        SYSTEM_CLOCK,
        STEADY_CLOCK,
        HIGH_RES_CLOCK,
        REALTIME_CLOCK,
        MONOTONIC_CLOCK
    };

    // ========================================================================
    // Timestamp Access
    // ========================================================================

    static Timestamp now() noexcept {
        return get_timestamp(current_clock_source_);
    }

    static Timestamp get_timestamp(ClockSource source = ClockSource::STEADY_CLOCK) noexcept {
        switch (source) {
            case ClockSource::SYSTEM_CLOCK:
                return system_clock_now();
            case ClockSource::STEADY_CLOCK:
            case ClockSource::HIGH_RES_CLOCK:
                return steady_clock_now();
            case ClockSource::REALTIME_CLOCK:
                return posix_clock_now(CLOCK_REALTIME);
            case ClockSource::MONOTONIC_CLOCK:
                return posix_clock_now(CLOCK_MONOTONIC);
            default:
                return steady_clock_now();
        }
    }

    static void set_clock_source(ClockSource source) noexcept {
        current_clock_source_ = source;
    }

    // ========================================================================
    // Conversion Utilities
    // ========================================================================

    static constexpr Timestamp to_nanoseconds(Duration duration) noexcept {
        return static_cast<Timestamp>(duration.count_ns());
    }

    static constexpr Duration from_nanoseconds(Timestamp ns) noexcept {
        return Duration::nanoseconds(static_cast<int64_t>(ns));
    }

    static constexpr Timestamp milliseconds_to_ns(uint64_t ms) noexcept {
        return ms * 1'000'000ULL;
    }

    static constexpr Timestamp microseconds_to_ns(uint64_t us) noexcept {
        return us * 1'000ULL;
    }

    static constexpr uint64_t ns_to_milliseconds(Timestamp ns) noexcept {
        return ns / 1'000'000ULL;
    }

    static constexpr uint64_t ns_to_microseconds(Timestamp ns) noexcept {
        return ns / 1'000ULL;
    }

    // ========================================================================
    // Timestamp Arithmetic
    // ========================================================================

    static constexpr Timestamp diff(Timestamp t1, Timestamp t2) noexcept {
        return (t1 > t2) ? (t1 - t2) : (t2 - t1);
    }

    static constexpr bool is_within_tolerance(Timestamp timestamp,
                                              Timestamp target,
                                              Timestamp tolerance_ns) noexcept {
        return diff(timestamp, target) <= tolerance_ns;
    }

    // ========================================================================
    // Sleep
    // ========================================================================

    static void sleep(Duration duration) noexcept {
        std::this_thread::sleep_for(duration.to_chrono_ns());
    }

    static void sleep_ns(Timestamp ns) noexcept {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }

    static void sleep_until(Timestamp target) noexcept {
        Timestamp now_ts = now();
        if (target > now_ts) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(target - now_ts));
        }
    }

    static void yield() noexcept {
        std::this_thread::yield();
    }

private:
    static Timestamp system_clock_now() noexcept {
        auto now = std::chrono::system_clock::now();
        return static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count());
    }

    static Timestamp steady_clock_now() noexcept {
        auto now = std::chrono::steady_clock::now();
        return static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count());
    }

    static Timestamp posix_clock_now(clockid_t clock_id) noexcept {
        struct timespec ts;
        if (clock_gettime(clock_id, &ts) == 0) {
            return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000ULL +
                   static_cast<Timestamp>(ts.tv_nsec);
        }
        return steady_clock_now();
    }

    static inline ClockSource current_clock_source_ = ClockSource::STEADY_CLOCK;
};

} // namespace corerat
