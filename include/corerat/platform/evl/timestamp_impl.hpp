/**
 * @file evl/timestamp_impl.hpp
 * @brief EVL (Xenomai 4 / libevl) clock/time backend for CoreRaT
 *
 * Implements the Time utility class using EVL clock and sleep primitives
 * for hard real-time guarantees.
 *
 * This file is included by corerat/platform/timestamp.hpp when
 * CORERAT_PLATFORM_EVL is defined.
 *
 * EVL Primitives Used:
 * - Time::now():         evl_read_clock(EVL_CLOCK_MONOTONIC) - OOB-safe
 * - Time::sleep():       evl_usleep() for <= 1s, evl_sleep_until() for longer
 * - Time::sleep_until(): evl_sleep_until() - jitter-free periodic timing
 * - Time::yield():       evl_yield() - OOB-safe round-robin yield
 */

#pragma once

#include <evl/clock.h>
#include <evl/sched.h>
#include <cstdint>
#include <ctime>

namespace corerat {

/**
 * @brief Time utility class (EVL/libevl backend)
 *
 * All operations are OOB-safe: evl_read_clock() and evl_sleep_until()
 * do not cause in-band demotion, maintaining hard real-time guarantees.
 */
class Time {
public:
    // ========================================================================
    // Timestamp Access
    // ========================================================================

    static Timestamp now() noexcept {
        struct timespec ts;
        evl_read_clock(EVL_CLOCK_MONOTONIC, &ts);
        return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<Timestamp>(ts.tv_nsec);
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
        if (duration.is_zero() || duration.is_negative()) {
            return;
        }
        sleep_ns(static_cast<Timestamp>(duration.count_ns()));
    }

    static void sleep_ns(Timestamp ns) noexcept {
        if (ns == 0) return;
        // evl_usleep() max is 1,000,000 us (1 second); use sleep_until for longer
        if (ns <= 1'000'000'000ULL) {
            const int r = evl_usleep(static_cast<useconds_t>(ns / 1'000U));
            if (r != 0) {
                // -EPERM (-1): thread not EVL-attached — fall back to nanosleep
                // -EINTR (-4): interrupted by signal — return immediately (caller retries)
                if (r == -EPERM) {
                    char ebuf[96];
                    const int n = std::snprintf(ebuf, sizeof(ebuf),
                        "[corerat] evl_usleep(%u us) failed: %d (thread not OOB-attached)\n",
                        static_cast<unsigned>(ns / 1'000U), r);
                    ::write(STDERR_FILENO, ebuf, static_cast<std::size_t>(n));
                    struct timespec ts{
                        static_cast<time_t>(ns / 1'000'000'000ULL),
                        static_cast<long>(ns % 1'000'000'000ULL)
                    };
                    nanosleep(&ts, nullptr);
                }
            }
        } else {
            Timestamp target = now() + ns;
            sleep_until(target);
        }
    }

    static void sleep_until(Timestamp target) noexcept {
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(target / 1'000'000'000ULL);
        ts.tv_nsec = static_cast<long>(target % 1'000'000'000ULL);
        evl_sleep_until(EVL_CLOCK_MONOTONIC, &ts);
    }

    static void yield() noexcept {
        evl_yield();
    }
};

} // namespace corerat
