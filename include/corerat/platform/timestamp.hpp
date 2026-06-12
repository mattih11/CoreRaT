/**
 * @file timestamp.hpp
 * @brief Unified timestamp and time utility abstractions for CoreRaT
 *
 * Provides platform-selectable abstractions for:
 * - Getting current time (via Time::now())
 * - Sleeping (Time::sleep(), Time::sleep_until(), Time::yield())
 * - Converting between time units
 * - Timestamp comparisons and arithmetic
 *
 * Includes duration.hpp which defines the Duration type.
 *
 * Backend selected at compile time via CORERAT_PLATFORM_STD or CORERAT_PLATFORM_EVL.
 */

#pragma once

#include "duration.hpp"
#include "platform.hpp"

#include <cstdint>

namespace corerat {

/**
 * @brief Timestamp type - uint64_t nanoseconds since epoch
 *
 * Compatible with WireHeader timestamp format.
 * Range: ~584 years from epoch.
 */
using Timestamp = uint64_t;

} // namespace corerat

// ============================================================================
// Backend Selection (Time class implementation)
// ============================================================================

#if defined(CORERAT_PLATFORM_EVL)
    #include "corerat/platform/evl/timestamp_impl.hpp"
#else
    #include "corerat/platform/std/timestamp_impl.hpp"
#endif
