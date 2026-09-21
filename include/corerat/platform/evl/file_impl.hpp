/**
 * @file evl/file_impl.hpp
 * @brief EVL file proxy backend for CoreRaT — OOB-safe file I/O
 *
 * Implements File using an EVL file proxy (evl_create_proxy).  The proxy
 * offloads actual read/write syscalls to an in-band worker thread so the
 * OOB caller never issues an in-band syscall and is never demoted from
 * out-of-band context.
 *
 * Write path (OOB → file):
 *   oob_write(proxy_fd) → EVL proxy ring buffer → worker → file_fd
 *
 * Read path (file → OOB):
 *   worker pulls from file_fd → EVL proxy ring buffer → oob_read(proxy_fd)
 *   Data must fit in the proxy ring buffer ahead of the OOB reader.
 *
 * This file is included by corerat/platform/file.hpp when
 * CORERAT_PLATFORM_EVL is defined.
 *
 * Not intended for direct inclusion by user code.
 *
 * @note evl_write_proxy / evl_read_proxy are used instead of raw
 *       oob_write / oob_read so that File works correctly from both
 *       in-band and OOB calling contexts.
 *
 * @note seek() and sync() are in-band syscalls — call only from
 *       non-RT context.
 */

#pragma once

#include <evl/proxy.h>
#include <evl/syscall.h>
#include <evl/thread.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>

namespace corerat {

/**
 * @brief EVL file proxy wrapper — OOB-safe file I/O
 *
 * write() and read() route through an EVL proxy and are safe to call
 * from OOB threads without causing in-band demotion.
 *
 * Not thread-safe — callers are responsible for synchronisation.
 */
class File {
public:
    File() = default;

    ~File() { close(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept
        : file_fd_(other.file_fd_), proxy_fd_(other.proxy_fd_) {
        other.file_fd_ = -1;
        other.proxy_fd_ = -1;
    }

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            file_fd_ = other.file_fd_;
            proxy_fd_ = other.proxy_fd_;
            other.file_fd_ = -1;
            other.proxy_fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Open a file and create an EVL proxy for OOB-safe I/O.
     *
     * The proxy direction (input / output / both) is derived from flags:
     * - O_RDONLY → EVL_CLONE_INPUT (proxy buffers data read from file)
     * - O_WRONLY → EVL_CLONE_OUTPUT (proxy buffers data to write to file)
     * - O_RDWR   → EVL_CLONE_INPUT | EVL_CLONE_OUTPUT (both directions)
     *
     * @param path   File path.
     * @param flags  POSIX open(2) flags.
     * @param mode   Permission bits when creating a new file.
     * @param config FileConfig::proxy_buf_size sets the ring buffer size per
     *               direction. Must be > 0 for I/O access.
     * @return true on success; false if either open(2) or proxy creation fails
     *         (errno reflects the error).
     */
    bool open(const char* path, int flags, mode_t mode = 0644,
              const FileConfig& config = {}) noexcept {
        close();

        file_fd_ = ::open(path, flags, mode);
        if (file_fd_ < 0) return false;

        int proxy_flags = EVL_CLONE_PRIVATE;
        const int acc = flags & O_ACCMODE;
        if (acc == O_RDONLY) {
            proxy_flags |= EVL_CLONE_INPUT;
        } else if (acc == O_WRONLY) {
            proxy_flags |= EVL_CLONE_OUTPUT;
        } else {
            proxy_flags |= EVL_CLONE_INPUT | EVL_CLONE_OUTPUT;
        }

        // Proxy name must be unique per process; file_fd_ is unique.
        const int ret = evl_create_proxy(file_fd_,
                                          config.proxy_buf_size,
                                          /*granularity=*/0,
                                          proxy_flags,
                                          "corerat-file:%d", file_fd_);
        if (ret < 0) {
            errno = -ret;
            ::close(file_fd_);
            file_fd_ = -1;
            return false;
        }

        proxy_fd_ = ret;
        return true;
    }

    void close() noexcept {
        // Proxy must be closed before the underlying file it wraps.
        if (proxy_fd_ >= 0) {
            ::close(proxy_fd_);
            proxy_fd_ = -1;
        }
        if (file_fd_ >= 0) {
            ::close(file_fd_);
            file_fd_ = -1;
        }
    }

    bool is_open() const noexcept { return file_fd_ >= 0; }

    /// Returns the underlying POSIX file descriptor (-1 if not open).
    int fd() const noexcept { return file_fd_; }

    /// Returns the EVL proxy file descriptor (-1 if not open).
    int proxy_fd() const noexcept { return proxy_fd_; }

    // ========================================================================
    // OOB-safe sequential I/O (via EVL proxy)
    // ========================================================================

    /**
     * @brief Write — OOB-safe when called from OOB context.
     *
     * OOB: oob_write() to the proxy ring; the in-band worker drains to the
     *      file without demoting the caller.
     * In-band: write(2) directly to file_fd_ (no proxy indirection needed).
     *
     * @return Bytes written, or -1 on failure (errno set).
     */
    ssize_t write(const void* buf, size_t count) noexcept {
        if (!evl_is_inband())
            return oob_write(proxy_fd_, buf, count);
        return ::write(file_fd_, buf, count);
    }

    /**
     * @brief Read — OOB-safe when called from OOB context.
     *
     * OOB: oob_read() from the proxy ring; blocks OOB-safe until data arrives.
     * In-band: read(2) directly from file_fd_.
     *
     * @return Bytes read, or -1 on failure (errno set).
     */
    ssize_t read(void* buf, size_t count) noexcept {
        if (!evl_is_inband())
            return oob_read(proxy_fd_, buf, count);
        return ::read(file_fd_, buf, count);
    }

    // ========================================================================
    // In-band only: not OOB-safe — call only from non-RT context
    // ========================================================================

    /**
     * @note In-band syscall — demotes the calling OOB thread. Use only from
     *       non-RT context.
     */
    ssize_t pwrite(const void* buf, size_t count, off_t offset) noexcept {
        return ::pwrite(file_fd_, buf, count, offset);
    }

    /**
     * @note In-band syscall — demotes the calling OOB thread. Use only from
     *       non-RT context.
     */
    ssize_t pread(void* buf, size_t count, off_t offset) noexcept {
        return ::pread(file_fd_, buf, count, offset);
    }

    /**
     * @note In-band syscall — demotes the calling OOB thread. Use only from
     *       non-RT context.
     */
    off_t seek(off_t offset, int whence) noexcept {
        return ::lseek(file_fd_, offset, whence);
    }

    /**
     * @note In-band syscall — demotes the calling OOB thread. Use only from
     *       non-RT context.
     */
    int sync() noexcept {
        return ::fsync(file_fd_);
    }

private:
    int file_fd_{-1};
    int proxy_fd_{-1};
};

} // namespace corerat
