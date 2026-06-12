/**
 * @file threading.hpp
 * @brief Unified threading and synchronization abstractions for CoreRaT
 *
 * Provides platform-selectable abstractions for:
 * - Thread creation and management (Thread)
 * - Mutexes and locks (Mutex, SharedMutex)
 * - Condition variables (ConditionVariable)
 * - Thread priorities and CPU affinity (ThreadConfig)
 *
 * Backend selected at compile time via CORERAT_PLATFORM_STD or CORERAT_PLATFORM_EVL.
 * Default: std:: (Linux). EVL: Xenomai 4 hard real-time.
 */

#pragma once

#include "duration.hpp"
#include "platform.hpp"

#include <atomic>
#include <string>
#include <cstdint>

namespace corerat {

// ============================================================================
// Common Types (shared across all backends)
// ============================================================================

enum class ThreadPriority {
    IDLE = 0,
    LOW = 10,
    NORMAL = 50,
    HIGH = 75,
    REALTIME = 99
};

enum class SchedulingPolicy {
    NORMAL,
    FIFO,
    ROUND_ROBIN,
    DEADLINE
};

struct ThreadConfig {
    std::string name{"unnamed"};
    ThreadPriority priority = ThreadPriority::NORMAL;
    SchedulingPolicy policy = SchedulingPolicy::NORMAL;
    int cpu_affinity = -1;  ///< -1 = no affinity, >= 0 = pin to CPU
    size_t stack_size = 0;  ///< 0 = default
};

enum class CvStatus {
    NO_TIMEOUT,
    TIMEOUT
};

} // namespace corerat

// ============================================================================
// Backend Selection
// ============================================================================

#if defined(CORERAT_PLATFORM_EVL)
    #include "corerat/platform/evl/threading_impl.hpp"
#else
    #include "corerat/platform/std/threading_impl.hpp"
#endif

namespace corerat {

// ============================================================================
// Lock Type Aliases (depend on backend-defined Mutex/SharedMutex)
// ============================================================================

using Lock             = std::lock_guard<Mutex>;
using UniqueLock       = std::unique_lock<Mutex>;
using SharedLock       = std::shared_lock<SharedMutex>;
using UniqueLockShared = std::unique_lock<SharedMutex>;

// ============================================================================
// Convenience Macros
// ============================================================================

#define Synchronized(mutex) \
    if (corerat::Lock _lock_##__LINE__{mutex}; true)

#define ReadLocked(mutex) \
    if (corerat::SharedLock _lock_##__LINE__{mutex}; true)

#define WriteLocked(mutex) \
    if (corerat::UniqueLockShared _lock_##__LINE__{mutex}; true)

} // namespace corerat
