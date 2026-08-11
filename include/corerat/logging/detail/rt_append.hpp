/**
 * @file detail/rt_append.hpp
 * @brief OOB-safe append helpers for sertial::fixed_string.
 *
 * All functions are noexcept and use only memcpy, stack arrays and
 * integer arithmetic — no glibc float formatting, no throw, no heap.
 *
 * Uses fixed_string::data_unsafe() + set_size_unsafe() to bypass the
 * throwing capacity checks in the public API, silently truncating when
 * the buffer is full.
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 * @realtime OOB-safe on EVL platform.
 */
#pragma once

#include <sertial/containers/fixed_string.hpp>
#include <cstring>
#include <cstdint>

namespace corerat::detail {

// ============================================================================
// Core: bounded memcpy into a fixed_string — never throws, never allocates
// ============================================================================

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, const char* src, std::size_t len) noexcept {
    std::size_t avail = (N > dst.size() + 1) ? (N - 1 - dst.size()) : 0;
    std::size_t copy  = len < avail ? len : avail;
    if (copy == 0) return;
    std::memcpy(dst.data_unsafe() + dst.size(), src, copy);
    dst.set_size_unsafe(dst.size() + copy);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, const char* cstr) noexcept {
    if (!cstr) return;
    std::size_t len = 0;
    while (cstr[len]) ++len;
    append_rt(dst, cstr, len);
}

// ============================================================================
// Unsigned integer — reverse-digit trick, fully OOB-safe
// ============================================================================

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, uint64_t v) noexcept {
    char tmp[20];
    int  i = 20;
    do { tmp[--i] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    append_rt(dst, tmp + i, static_cast<std::size_t>(20 - i));
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, uint32_t v) noexcept {
    append_rt(dst, static_cast<uint64_t>(v));
}

// ============================================================================
// Signed integers
// ============================================================================

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, int64_t v) noexcept {
    if (v < 0) {
        append_rt(dst, "-", 1u);
        // careful: INT64_MIN negation overflows; use unsigned cast
        append_rt(dst, static_cast<uint64_t>(-static_cast<uint64_t>(v)));
    } else {
        append_rt(dst, static_cast<uint64_t>(v));
    }
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, int32_t v) noexcept {
    append_rt(dst, static_cast<int64_t>(v));
}

// ============================================================================
// Floating point — fixed-point representation, no glibc, fully OOB-safe.
//
// Renders as:  [-]<integer>.<Decimals digits of fraction>
// Decimals is a compile-time constant (default 3).
//
// Special values (NaN, Inf) are rendered as text without glibc calls.
// Precision is sufficient for sensor telemetry; not for scientific notation.
// ============================================================================

namespace impl {

// constexpr power-of-10 table
inline constexpr uint64_t pow10_table[10] = {
    1, 10, 100, 1'000, 10'000, 100'000,
    1'000'000, 10'000'000, 100'000'000, 1'000'000'000
};

// Detect NaN without <cmath> — OOB-safe since it's just a bit comparison
inline bool rt_isnan(double v) noexcept { return v != v; }

// Detect infinity: finite values satisfy |v| < 2^1023
inline bool rt_isinf(double v) noexcept {
    return !rt_isnan(v) && (v > 1.7976931348623157e+308 || v < -1.7976931348623157e+308);
}

} // namespace impl

template<std::size_t N, int Decimals = 3>
void append_rt_double(sertial::fixed_string<N>& dst, double v) noexcept {
    static_assert(Decimals >= 0 && Decimals <= 9, "Decimals must be 0..9");

    if (impl::rt_isnan(v)) { append_rt(dst, "nan", 3u); return; }
    if (impl::rt_isinf(v)) {
        if (v < 0.0) append_rt(dst, "-inf", 4u);
        else         append_rt(dst, "inf",  3u);
        return;
    }

    if (v < 0.0) {
        append_rt(dst, "-", 1u);
        v = -v;
    }

    // Split into integer and fractional parts
    uint64_t int_part  = static_cast<uint64_t>(v);
    append_rt(dst, int_part);

    if constexpr (Decimals > 0) {
        append_rt(dst, ".", 1u);

        const uint64_t scale = impl::pow10_table[Decimals];
        double frac = v - static_cast<double>(int_part);
        // Round to nearest at the last decimal place
        uint64_t frac_int = static_cast<uint64_t>(frac * static_cast<double>(scale) + 0.5);
        // Overflow guard: rounding may push frac_int to scale (e.g. 0.9995 → 1.000)
        if (frac_int >= scale) frac_int = scale - 1;

        // Write with leading zeros to fill Decimals digits
        char tmp[10];
        for (int i = Decimals - 1; i >= 0; --i) {
            tmp[i] = static_cast<char>('0' + frac_int % 10);
            frac_int /= 10;
        }
        append_rt(dst, tmp, static_cast<std::size_t>(Decimals));
    }
}

// Default overload uses Decimals=3; call append_rt_double<N, D> for custom precision
template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, double v) noexcept {
    append_rt_double<N, 3>(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, float v) noexcept {
    append_rt_double<N, 3>(dst, static_cast<double>(v));
}

// ============================================================================
// bool
// ============================================================================

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, bool v) noexcept {
    if (v) append_rt(dst, "true",  4u);
    else   append_rt(dst, "false", 5u);
}

// ============================================================================
// Hex formatting helper — e.g. for fault codes / register dumps
//
//   detail::append_rt_hex(dst, 0xDEADBEEFu);  // "DEADBEEF"
// ============================================================================

template<std::size_t N>
void append_rt_hex(sertial::fixed_string<N>& dst, uint64_t v, bool prefix = false) noexcept {
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (prefix) append_rt(dst, "0x", 2u);
    char tmp[16];
    int i = 16;
    do { tmp[--i] = kHex[v & 0xF]; v >>= 4; } while (v);
    append_rt(dst, tmp + i, static_cast<std::size_t>(16 - i));
}

template<std::size_t N>
void append_rt_hex(sertial::fixed_string<N>& dst, uint32_t v, bool prefix = false) noexcept {
    append_rt_hex(dst, static_cast<uint64_t>(v), prefix);
}

} // namespace corerat::detail
