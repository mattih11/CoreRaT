/**
 * @file duration.hpp
 * @brief Platform-independent Duration type for CoreRaT
 *
 * Custom Duration wrapper class backed by int64_t nanoseconds internally.
 * Designed as a structural type (C++20 NTTP compatible) for use in
 * template parameters like Period<Milliseconds(100)>.
 *
 * Replaces the old type aliases (using Milliseconds = std::chrono::milliseconds)
 * with constexpr free functions that return Duration values.
 *
 * Provides conversion utilities to/from std::chrono and timespec for
 * backend interoperability (std:: and EVL).
 */

#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <ctime>

namespace corerat {

/**
 * @brief Fixed-precision duration type backed by nanoseconds
 *
 * Structural type (public member) for C++20 NTTP compatibility.
 * All operations are constexpr for compile-time evaluation.
 *
 * Usage:
 *   Duration timeout = Milliseconds(100);
 *   Duration period  = 50_ms;
 *   int64_t ms_val   = timeout.count_ms();  // 100
 *   int64_t ns_val   = timeout.count_ns();  // 100'000'000
 */
class Duration {
public:
    /// Internal nanosecond count (public for structural type / NTTP)
    int64_t ns_ = 0;

    // ========================================================================
    // Construction
    // ========================================================================

    constexpr Duration() noexcept = default;

    /// Explicit construction from raw nanosecond count
    constexpr explicit Duration(int64_t nanoseconds) noexcept : ns_(nanoseconds) {}

    // ========================================================================
    // Named Factory Methods
    // ========================================================================

    static constexpr Duration nanoseconds(int64_t v) noexcept { return Duration(v); }
    static constexpr Duration microseconds(int64_t v) noexcept { return Duration(v * 1'000); }
    static constexpr Duration milliseconds(int64_t v) noexcept { return Duration(v * 1'000'000); }
    static constexpr Duration seconds(int64_t v) noexcept { return Duration(v * 1'000'000'000); }
    static constexpr Duration minutes(int64_t v) noexcept { return Duration(v * 60'000'000'000LL); }
    static constexpr Duration hours(int64_t v) noexcept { return Duration(v * 3'600'000'000'000LL); }

    /// Zero duration
    static constexpr Duration zero() noexcept { return Duration(0); }

    // ========================================================================
    // Named Accessors (count at specific resolution)
    // ========================================================================

    constexpr int64_t count_ns() const noexcept { return ns_; }
    constexpr int64_t count_us() const noexcept { return ns_ / 1'000; }
    constexpr int64_t count_ms() const noexcept { return ns_ / 1'000'000; }
    constexpr int64_t count_s()  const noexcept { return ns_ / 1'000'000'000; }

    // ========================================================================
    // Predicates
    // ========================================================================

    constexpr bool is_zero()     const noexcept { return ns_ == 0; }
    constexpr bool is_negative() const noexcept { return ns_ < 0; }
    constexpr bool is_positive() const noexcept { return ns_ > 0; }

    // ========================================================================
    // Arithmetic
    // ========================================================================

    constexpr Duration operator+(Duration other) const noexcept { return Duration(ns_ + other.ns_); }
    constexpr Duration operator-(Duration other) const noexcept { return Duration(ns_ - other.ns_); }
    constexpr Duration operator-() const noexcept { return Duration(-ns_); }
    constexpr Duration& operator+=(Duration other) noexcept { ns_ += other.ns_; return *this; }
    constexpr Duration& operator-=(Duration other) noexcept { ns_ -= other.ns_; return *this; }
    constexpr Duration operator*(int64_t factor) const noexcept { return Duration(ns_ * factor); }
    constexpr Duration operator/(int64_t divisor) const noexcept { return Duration(ns_ / divisor); }
    constexpr int64_t operator/(Duration other) const noexcept { return ns_ / other.ns_; }
    constexpr Duration operator%(Duration other) const noexcept { return Duration(ns_ % other.ns_); }

    friend constexpr Duration operator*(int64_t factor, Duration d) noexcept {
        return Duration(factor * d.ns_);
    }

    // ========================================================================
    // Comparison (C++20 three-way)
    // ========================================================================

    constexpr auto operator<=>(const Duration&) const noexcept = default;
    constexpr bool operator==(const Duration&) const noexcept = default;

    // ========================================================================
    // Conversion: std::chrono interop (for std:: backend)
    // ========================================================================

    constexpr std::chrono::nanoseconds to_chrono_ns() const noexcept {
        return std::chrono::nanoseconds(ns_);
    }

    constexpr std::chrono::microseconds to_chrono_us() const noexcept {
        return std::chrono::microseconds(ns_ / 1'000);
    }

    constexpr std::chrono::milliseconds to_chrono_ms() const noexcept {
        return std::chrono::milliseconds(ns_ / 1'000'000);
    }

    /// Construct Duration from any std::chrono::duration
    template<typename Rep, typename Period>
    static constexpr Duration from_chrono(std::chrono::duration<Rep, Period> d) noexcept {
        return Duration(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    }

    // ========================================================================
    // Conversion: POSIX timespec (for EVL / kernel interface)
    // ========================================================================

    constexpr struct timespec to_timespec() const noexcept {
        return {
            .tv_sec  = static_cast<time_t>(ns_ / 1'000'000'000),
            .tv_nsec = static_cast<long>(ns_ % 1'000'000'000)
        };
    }

    static constexpr Duration from_timespec(const struct timespec& ts) noexcept {
        return Duration(static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec);
    }
};

// ============================================================================
// Free Function Constructors
// ============================================================================

constexpr Duration Nanoseconds(int64_t v) noexcept  { return Duration::nanoseconds(v); }
constexpr Duration Microseconds(int64_t v) noexcept { return Duration::microseconds(v); }
constexpr Duration Milliseconds(int64_t v) noexcept { return Duration::milliseconds(v); }
constexpr Duration Seconds(int64_t v) noexcept      { return Duration::seconds(v); }
constexpr Duration Minutes(int64_t v) noexcept      { return Duration::minutes(v); }
constexpr Duration Hours(int64_t v) noexcept        { return Duration::hours(v); }

// ============================================================================
// User-Defined Literals
// ============================================================================

namespace literals {
    constexpr Duration operator""_ns(unsigned long long v) noexcept {
        return Duration::nanoseconds(static_cast<int64_t>(v));
    }
    constexpr Duration operator""_us(unsigned long long v) noexcept {
        return Duration::microseconds(static_cast<int64_t>(v));
    }
    constexpr Duration operator""_ms(unsigned long long v) noexcept {
        return Duration::milliseconds(static_cast<int64_t>(v));
    }
    constexpr Duration operator""_s(unsigned long long v) noexcept {
        return Duration::seconds(static_cast<int64_t>(v));
    }
} // namespace literals

} // namespace corerat
