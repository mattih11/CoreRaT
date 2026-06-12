/**
 * @file platform.hpp
 * @brief Platform backend selection for CoreRaT
 *
 * Selects between std:: (default Linux) and EVL (Xenomai 4 hard-RT) backends
 * at compile time via CMake-defined macros.
 *
 * CMake usage:
 *   target_compile_definitions(my_target PRIVATE CORERAT_PLATFORM_EVL)
 *
 * If no platform is explicitly selected, defaults to CORERAT_PLATFORM_STD.
 */

#pragma once

// ============================================================================
// Platform Selection
// ============================================================================

// Default to std:: backend if nothing is explicitly set
#if !defined(CORERAT_PLATFORM_STD) && !defined(CORERAT_PLATFORM_EVL)
    #define CORERAT_PLATFORM_STD
#endif

// Sanity check: exactly one platform must be selected
#if defined(CORERAT_PLATFORM_STD) && defined(CORERAT_PLATFORM_EVL)
    #error "Cannot define both CORERAT_PLATFORM_STD and CORERAT_PLATFORM_EVL"
#endif

// ============================================================================
// Platform Feature Macros
// ============================================================================

#if defined(CORERAT_PLATFORM_EVL)
    /// EVL out-of-band scheduling available
    #define CORERAT_HAS_OOB 1
    /// Priority inheritance mutexes by default
    #define CORERAT_HAS_PI_MUTEX 1
    /// Absolute-time sleep (evl_sleep_until) available
    #define CORERAT_HAS_SLEEP_UNTIL 1
#else
    #define CORERAT_HAS_OOB 0
    #define CORERAT_HAS_PI_MUTEX 0
    #define CORERAT_HAS_SLEEP_UNTIL 0
#endif
