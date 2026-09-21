/**
 * @file file.hpp
 * @brief Platform-selectable file I/O abstractions for CoreRaT
 *
 * Provides OOB-safe file access for both STD and EVL backends:
 *
 * - STD backend: thin POSIX wrappers (open/read/write/close)
 * - EVL backend: EVL file proxy — write/read route through an in-band worker
 *   thread so the OOB caller never issues an in-band syscall and is never
 *   demoted from OOB context.
 *
 * Usage:
 * @code
 *   corerat::FileConfig cfg{.proxy_buf_size = 128 * 1024};
 *   corerat::File f;
 *   f.open("/tmp/rt.log", O_WRONLY | O_CREAT | O_TRUNC, 0644, cfg);
 *   f.write(buf, len);   // OOB-safe on EVL
 * @endcode
 *
 * @note seek() and sync() are in-band syscalls — call only from non-RT context.
 */

#pragma once

#include "platform.hpp"

#include <cstddef>
#include <sys/types.h>

namespace corerat {

// ============================================================================
// FileConfig — shared by both backends
// ============================================================================

struct FileConfig {
    /// Size of the EVL proxy ring buffer in bytes (EVL backend only).
    /// Must be non-zero for write or read access; ignored on STD backend.
    size_t proxy_buf_size{64 * 1024};
};

} // namespace corerat

// ============================================================================
// Backend Selection
// ============================================================================

#if defined(CORERAT_PLATFORM_EVL)
    #include "corerat/platform/evl/file_impl.hpp"
#else
    #include "corerat/platform/std/file_impl.hpp"
#endif
