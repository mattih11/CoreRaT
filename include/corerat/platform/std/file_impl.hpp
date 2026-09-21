/**
 * @file std/file_impl.hpp
 * @brief Standard POSIX file I/O backend for CoreRaT
 *
 * Implements File using direct POSIX syscalls (open/read/write/close).
 * All operations are in-band — there is no proxy layer on this backend.
 *
 * This file is included by corerat/platform/file.hpp when
 * CORERAT_PLATFORM_STD is defined (default).
 *
 * Not intended for direct inclusion by user code.
 */

#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstddef>

namespace corerat {

/**
 * @brief POSIX file I/O wrapper (STD backend)
 *
 * Thin non-owning RAII wrapper around a POSIX file descriptor.
 * Not thread-safe — callers are responsible for synchronisation.
 */
class File {
public:
    File() = default;

    ~File() { close(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept : file_fd_(other.file_fd_) {
        other.file_fd_ = -1;
    }

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            file_fd_ = other.file_fd_;
            other.file_fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Open a file.
     *
     * @param path   File path.
     * @param flags  POSIX open(2) flags (O_RDONLY, O_WRONLY, O_RDWR, etc.).
     * @param mode   Permission bits used when creating a new file.
     * @param config FileConfig — proxy_buf_size is unused on this backend.
     * @return true on success; false on failure (errno set by open(2)).
     */
    bool open(const char* path, int flags, mode_t mode = 0644,
              const FileConfig& /*config*/ = {}) noexcept {
        close();
        file_fd_ = ::open(path, flags, mode);
        return file_fd_ >= 0;
    }

    void close() noexcept {
        if (file_fd_ >= 0) {
            ::close(file_fd_);
            file_fd_ = -1;
        }
    }

    bool is_open() const noexcept { return file_fd_ >= 0; }

    /// Returns the underlying POSIX file descriptor (-1 if not open).
    int fd() const noexcept { return file_fd_; }

    // ========================================================================
    // Sequential I/O
    // ========================================================================

    ssize_t write(const void* buf, size_t count) noexcept {
        return ::write(file_fd_, buf, count);
    }

    ssize_t read(void* buf, size_t count) noexcept {
        return ::read(file_fd_, buf, count);
    }

    // ========================================================================
    // Positional I/O (does not advance the file offset)
    // ========================================================================

    ssize_t pwrite(const void* buf, size_t count, off_t offset) noexcept {
        return ::pwrite(file_fd_, buf, count, offset);
    }

    ssize_t pread(void* buf, size_t count, off_t offset) noexcept {
        return ::pread(file_fd_, buf, count, offset);
    }

    // ========================================================================
    // Seek / Sync (in-band; safe on this backend)
    // ========================================================================

    off_t seek(off_t offset, int whence) noexcept {
        return ::lseek(file_fd_, offset, whence);
    }

    int sync() noexcept {
        return ::fsync(file_fd_);
    }

private:
    int file_fd_{-1};
};

} // namespace corerat
