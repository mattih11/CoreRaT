/**
 * @file detail/rt_append.hpp
 * @brief OOB-safe append helpers — thin delegation to sertial::rt.
 *
 * All implementation now lives in sertial/containers/rt_format.hpp
 * (sertial::rt::append / append_double / append_hex).  This header
 * re-exports those under the corerat::detail names used by
 * rt_log_stream.hpp so call-sites need no changes.
 *
 * @note Not intended for direct inclusion by user code. Include logging.hpp.
 * @realtime OOB-safe on EVL platform — delegates to sertial::rt which
 *           uses only memcpy, stack arrays and integer arithmetic.
 */
#pragma once

#include <sertial/containers/rt_format.hpp>

namespace corerat::detail {

// Delegate every append_rt overload to sertial::rt::append

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, const char* src, std::size_t len) noexcept {
    sertial::rt::append(dst, src, len);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, const char* cstr) noexcept {
    sertial::rt::append(dst, cstr);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, uint64_t v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, uint32_t v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, int64_t v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, int32_t v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, bool v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, double v) noexcept {
    sertial::rt::append(dst, v);
}

template<std::size_t N>
void append_rt(sertial::fixed_string<N>& dst, float v) noexcept {
    sertial::rt::append(dst, v);
}

/// Explicit decimal-place control — delegates to sertial::rt::append_double.
template<std::size_t N, int Decimals = 3>
void append_rt_double(sertial::fixed_string<N>& dst, double v) noexcept {
    sertial::rt::append_double<N, Decimals>(dst, v);
}

/// Hex output — delegates to sertial::rt::append_hex.
template<std::size_t N>
void append_rt_hex(sertial::fixed_string<N>& dst, uint64_t v, bool prefix = false) noexcept {
    sertial::rt::append_hex(dst, v, prefix);
}

template<std::size_t N>
void append_rt_hex(sertial::fixed_string<N>& dst, uint32_t v, bool prefix = false) noexcept {
    sertial::rt::append_hex(dst, v, prefix);
}

} // namespace corerat::detail
