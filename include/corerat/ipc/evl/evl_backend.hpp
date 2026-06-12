#pragma once

/**
 * @file evl/evl_backend.hpp
 * @brief EVL-native IPC backend for CoreRaT
 *
 * Two operating modes selected at initialize() time:
 *
 *   LOCAL  — single-process / multi-thread (default).
 *            Ring buffer lives in heap memory.
 *            `evl_detail::registry_slot[]` is the entire routing table.
 *
 *   PUBLIC — cross-process.
 *            Ring buffer lives in a POSIX SHM region ("/corerat_mbx_<id>").
 *            EVL mutex/event created with EVL_CLONE_PUBLIC — any process can
 *            open them by name via evl_open_mutex / evl_open_event.
 *            The EvlRouter name-service daemon maps mailbox_id → resource names
 *            so senders in other processes can attach without prior coordination.
 *
 * Hot-path OOB operations (no mode switch):
 *   send:    evl_lock → memcpy → mark in_use → evl_unlock → evl_signal
 *   receive: evl_timedwait/evl_wait → memcpy → mark free  → evl_unlock
 *
 * Priority model mirrors the TiMS kernel module:
 *   Higher value = higher priority.
 *   When the ring is full, the lowest-priority message is evicted if the new
 *   message has equal or higher priority.
 */

#include "corerat/ipc/tims/tims_backend.hpp"   // TimsConfig, TimsResult
#include "corerat/messaging/wire_message.hpp"
#include "corerat/platform/duration.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#ifdef CORERAT_PLATFORM_EVL
#include <evl/evl.h>
#include <evl/mutex.h>
#include <evl/event.h>
#include <evl/clock.h>
#include <evl/factory-abi.h>
#endif

namespace corerat {

// ============================================================================
// Forward declaration for process-local registry
// ============================================================================

class EvlMailbox;

namespace evl_detail {

inline constexpr uint32_t kMaxMailboxes = 256;

/// Process-local registry: mailbox_id → EvlMailbox* (LOCAL mode only).
/// Written only at initialize()/shutdown() — not hot path.
inline EvlMailbox*& registry_slot(uint32_t id) noexcept {
    static EvlMailbox* reg[kMaxMailboxes]{};
    return reg[id % kMaxMailboxes];
}

/// Build an EVL object name into a fixed buffer.
/// Returns a null-terminated string_view into buf.
inline std::string_view evl_name(char (&buf)[64],
                                  std::string_view prefix,
                                  uint32_t id) noexcept {
    const int n = std::snprintf(buf, sizeof(buf), "%.*s%u",
                                static_cast<int>(prefix.size()),
                                prefix.data(), id);
    return { buf, static_cast<std::string_view::size_type>(n) };
}

/// SHM path for a mailbox (POSIX shm_open name).
inline std::string_view shm_name(char (&buf)[64], uint32_t id) noexcept {
    return evl_name(buf, "/corerat_mbx_", id);
}

}  // namespace evl_detail

// ============================================================================
// EvlMailbox
// ============================================================================

class EvlMailbox {
public:
    enum class Mode { Local, Public };

    /// Metadata compatible with TimsMailbox::Metadata
    struct Metadata {
        uint32_t src{0};
        uint32_t dest{0};
        uint32_t seq_nr{0};
        uint8_t  priority{0};
        uint8_t  flags{0};
    };

    explicit EvlMailbox(const TimsConfig& config,
                        Mode mode = Mode::Local)
        : config_(config), mode_(mode) {}

    ~EvlMailbox() { shutdown(); }

    EvlMailbox(const EvlMailbox&)            = delete;
    EvlMailbox& operator=(const EvlMailbox&) = delete;
    EvlMailbox(EvlMailbox&&)                 = delete;
    EvlMailbox& operator=(EvlMailbox&&)      = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    TimsResult initialize() {
        if (created_) return TimsResult::SUCCESS;

        constexpr uint32_t kSlots = 10;
        const auto slot_size = static_cast<uint32_t>(config_.max_msg_size);

        if (!allocate_ring(kSlots, slot_size)) return TimsResult::ERROR_INIT;
        if (!create_evl_objects())             return TimsResult::ERROR_INIT;

        if (mode_ == Mode::Local)
            evl_detail::registry_slot(config_.mailbox_id) = this;

        created_ = true;
        return TimsResult::SUCCESS;
    }

    void shutdown() {
        if (!created_) return;

        if (mode_ == Mode::Local &&
            evl_detail::registry_slot(config_.mailbox_id) == this)
            evl_detail::registry_slot(config_.mailbox_id) = nullptr;

        destroy_evl_objects();
        free_ring();
        created_ = false;
    }

    bool     is_initialized()        const noexcept { return created_; }
    uint32_t get_mailbox_id()        const noexcept { return config_.mailbox_id; }
    Mode     mode()                  const noexcept { return mode_; }
    uint64_t get_messages_sent()     const noexcept {
        return messages_sent_.load(std::memory_order_relaxed);
    }
    uint64_t get_messages_received() const noexcept {
        return messages_received_.load(std::memory_order_relaxed);
    }

    // ========================================================================
    // Send — same template interface as TimsMailbox
    // ========================================================================

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id) {
        static_assert(is_wire_message_v<T>, "T must be a CoreRaT WireMessage type");
        if (!created_) return TimsResult::ERROR_NOT_INIT;
        auto result = corerat::serialize(message);
        auto view   = result.view();
        return send_raw(view.data(), view.size(), dest_mailbox_id,
                        static_cast<int8_t>(config_.priority));
    }

    // ========================================================================
    // Receive — same interface as TimsMailbox
    // ========================================================================

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout) {
        return receive_impl(buffer, timeout, nullptr);
    }

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout,
                              Metadata* metadata) {
        return receive_impl(buffer, timeout, metadata);
    }

private:
    // ========================================================================
    // Slot metadata — stored separately from payload data for cache locality
    // ========================================================================

    struct SlotMeta {
        int8_t   priority{0};
        bool     in_use{false};
        uint32_t src{0};
        uint32_t size{0};
    };

    // ========================================================================
    // Ring buffer allocation
    // ========================================================================

    bool allocate_ring(uint32_t slots, uint32_t slot_size) noexcept {
        if (mode_ == Mode::Public) {
            char name_buf[64];
            const auto name = evl_detail::shm_name(name_buf, config_.mailbox_id);

            shm_fd_ = ::shm_open(name.data(), O_CREAT | O_RDWR, 0600);
            if (shm_fd_ < 0) return false;

            const std::size_t data_sz = static_cast<std::size_t>(slots) * slot_size;
            if (::ftruncate(shm_fd_, static_cast<off_t>(data_sz)) < 0) {
                ::close(shm_fd_); shm_fd_ = -1;
                return false;
            }
            void* p = ::mmap(nullptr, data_sz,
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
            if (p == MAP_FAILED) {
                ::close(shm_fd_); shm_fd_ = -1;
                return false;
            }
            shm_data_      = static_cast<uint8_t*>(p);
            shm_data_size_ = data_sz;
            slot_meta_     = std::make_unique<SlotMeta[]>(slots);
        } else {
            // LOCAL: plain heap
            slot_meta_ = std::make_unique<SlotMeta[]>(slots);
            heap_data_ = std::make_unique<uint8_t[]>(
                static_cast<std::size_t>(slots) * slot_size);
        }
        capacity_  = slots;
        slot_size_ = slot_size;
        return true;
    }

    void free_ring() noexcept {
        if (mode_ == Mode::Public && shm_data_) {
            ::munmap(shm_data_, shm_data_size_);
            shm_data_ = nullptr;
            char name_buf[64];
            const auto name = evl_detail::shm_name(name_buf, config_.mailbox_id);
            ::shm_unlink(name.data());
        }
        if (shm_fd_ >= 0) { ::close(shm_fd_); shm_fd_ = -1; }
        slot_meta_.reset();
        heap_data_.reset();
        capacity_ = slot_size_ = 0;
    }

    uint8_t* slot_ptr(int idx) noexcept {
        auto* base = (mode_ == Mode::Public) ? shm_data_ : heap_data_.get();
        return base + static_cast<std::size_t>(idx) * slot_size_;
    }

    // ========================================================================
    // EVL object creation / destruction
    // ========================================================================

    bool create_evl_objects() noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_init();
        const int flags = (mode_ == Mode::Public)
                        ? (EVL_MUTEX_NORMAL | EVL_CLONE_PUBLIC)
                        : (EVL_MUTEX_NORMAL | EVL_CLONE_PRIVATE);
        const int eflags = (mode_ == Mode::Public)
                         ? EVL_CLONE_PUBLIC
                         : EVL_CLONE_PRIVATE;

        char name_buf[64];
        evl_detail::evl_name(name_buf, "corerat-ring-mtx-", config_.mailbox_id);
        if (evl_create_mutex(&ring_mutex_, EVL_CLOCK_MONOTONIC,
                             0, flags, "%s", name_buf) < 0)
            return false;

        evl_detail::evl_name(name_buf, "corerat-ring-evt-", config_.mailbox_id);
        if (evl_create_event(&ring_event_, EVL_CLOCK_MONOTONIC,
                             eflags, "%s", name_buf) < 0) {
            evl_close_mutex(&ring_mutex_);
            return false;
        }
#endif
        return true;
    }

    void destroy_evl_objects() noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_close_event(&ring_event_);
        evl_close_mutex(&ring_mutex_);
#endif
    }

    // ========================================================================
    // Slot helpers
    // ========================================================================

    int find_free_slot() const noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (!slot_meta_[i].in_use) return static_cast<int>(i);
        return -1;
    }

    bool has_in_use_slot() const noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (slot_meta_[i].in_use) return true;
        return false;
    }

    int find_highest_priority_slot() const noexcept {
        int    best = -1;
        int8_t prio = INT8_MIN;
        for (uint32_t i = 0; i < capacity_; ++i) {
            if (slot_meta_[i].in_use && slot_meta_[i].priority > prio) {
                prio = slot_meta_[i].priority;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    int evict_lowest(int8_t prio_new) noexcept {
        int    worst = -1;
        int8_t prio  = INT8_MAX;
        for (uint32_t i = 0; i < capacity_; ++i) {
            if (slot_meta_[i].in_use && slot_meta_[i].priority < prio) {
                prio  = slot_meta_[i].priority;
                worst = static_cast<int>(i);
            }
        }
        if (worst >= 0 && prio_new >= prio) {
            slot_meta_[worst].in_use = false;
            return worst;
        }
        return -1;
    }

    // ========================================================================
    // post() — write into THIS ring; called by sender via send_raw()
    // OOB-safe: only EVL primitives + memcpy inside the lock.
    // ========================================================================

    bool post(const void* data, std::size_t size,
              int8_t priority, uint32_t src) noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_lock_mutex(&ring_mutex_);
#endif
        int idx = find_free_slot();
        if (idx < 0) idx = evict_lowest(priority);
        if (idx < 0) {
#ifdef CORERAT_PLATFORM_EVL
            evl_unlock_mutex(&ring_mutex_);
#endif
            return false;
        }

        std::memcpy(slot_ptr(idx), data, size);
        slot_meta_[idx] = SlotMeta{ priority, true, src, static_cast<uint32_t>(size) };

#ifdef CORERAT_PLATFORM_EVL
        evl_unlock_mutex(&ring_mutex_);
        evl_signal_event(&ring_event_);
#endif
        return true;
    }

    // ========================================================================
    // send_raw() — look up dest ring and call post()
    // LOCAL: direct registry_slot lookup.
    // PUBLIC: registry_slot lookup first; name-based open as fallback (Phase 3).
    // ========================================================================

    TimsResult send_raw(const void* data, std::size_t size,
                        uint32_t dest, int8_t priority) noexcept {
        if (!data || size == 0 || size > config_.max_msg_size)
            return TimsResult::ERROR_INVALID_MSG;

        EvlMailbox* target = evl_detail::registry_slot(dest);
        if (!target || target->get_mailbox_id() != dest || !target->created_)
            return TimsResult::ERROR_SEND;

        if (!target->post(data, size, priority, config_.mailbox_id))
            return TimsResult::ERROR_SEND;

        messages_sent_.fetch_add(1, std::memory_order_relaxed);
        return TimsResult::SUCCESS;
    }

    // ========================================================================
    // receive_impl()
    // ========================================================================

    ssize_t receive_impl(std::span<std::byte> buffer,
                         Duration timeout, Metadata* meta) noexcept {
        if (!created_) return -1;

#ifdef CORERAT_PLATFORM_EVL
        evl_lock_mutex(&ring_mutex_);

        if (!has_in_use_slot()) {
            if (timeout.is_negative()) {
                evl_wait_event(&ring_event_, &ring_mutex_);
            } else {
                struct timespec now{};
                evl_read_clock(EVL_CLOCK_MONOTONIC, &now);
                const int64_t deadline =
                    static_cast<int64_t>(now.tv_sec)  * 1'000'000'000LL
                  + static_cast<int64_t>(now.tv_nsec)
                  + timeout.count_ns();
                const struct timespec abs_ts{
                    static_cast<time_t>(deadline / 1'000'000'000LL),
                    static_cast<long>(deadline   % 1'000'000'000LL)
                };

                if (evl_timedwait_event(&ring_event_, &ring_mutex_, &abs_ts)
                        == -ETIMEDOUT) {
                    evl_unlock_mutex(&ring_mutex_);
                    return -ETIMEDOUT;
                }
            }
            if (!has_in_use_slot()) {
                evl_unlock_mutex(&ring_mutex_);
                return -1;
            }
        }

        const int idx      = find_highest_priority_slot();
        const uint8_t* src = slot_ptr(idx);
        const auto     sz  = static_cast<std::size_t>(slot_meta_[idx].size);

        if (sz > buffer.size()) {
            evl_unlock_mutex(&ring_mutex_);
            return -EMSGSIZE;
        }

        std::memcpy(buffer.data(), src, sz);

        if (meta) {
            WireHeader hdr{};
            if (sz >= sizeof(WireHeader)) std::memcpy(&hdr, src, sizeof(WireHeader));
            meta->src      = slot_meta_[idx].src;
            meta->dest     = config_.mailbox_id;
            meta->seq_nr   = hdr.seq_number;
            meta->priority = static_cast<uint8_t>(slot_meta_[idx].priority);
            meta->flags    = static_cast<uint8_t>(hdr.flags);
        }

        slot_meta_[idx].in_use = false;
        evl_unlock_mutex(&ring_mutex_);

        messages_received_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<ssize_t>(sz);
#else
        (void)buffer; (void)timeout; (void)meta;
        return -1;
#endif
    }

    // ========================================================================
    // Data members
    // ========================================================================

    TimsConfig            config_;
    Mode                  mode_;
    bool                  created_{false};
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_received_{0};

#ifdef CORERAT_PLATFORM_EVL
    struct evl_mutex ring_mutex_{};
    struct evl_event ring_event_{};
#endif

    // Ring buffer — heap (LOCAL) or SHM (PUBLIC)
    std::unique_ptr<SlotMeta[]> slot_meta_;
    std::unique_ptr<uint8_t[]>  heap_data_;   // LOCAL only
    uint8_t*                    shm_data_{nullptr};  // PUBLIC only
    std::size_t                 shm_data_size_{0};
    int                         shm_fd_{-1};
    uint32_t                    capacity_{0};
    uint32_t                    slot_size_{0};
};

}  // namespace corerat

 *
 * Drop-in replacement for TimsMailbox when CORERAT_IPC=EVL.
 *
 * Each EvlMailbox owns a priority-sorted slot ring (heap-allocated at initialize()).
 * All hot-path operations are OOB-safe:
 *   send:    evl_lock → memcpy → mark in_use → evl_unlock → evl_signal
 *   receive: evl_timedwait/evl_wait → memcpy → mark free  → evl_unlock
 *
 * The ring is stored in heap memory (single-process).
 * Cross-process IPC (memfd + name service) is deferred to Phase 3.
 *
 * Priority model: mirrors the TiMS kernel module.
 *   Higher priority value = higher priority.
 *   When the ring is full, the lowest-priority message is dropped if the new
 *   message has equal or higher priority.
 *
 * STATUS: Phase 2 — single-process implementation complete.
 */

// EvlMailbox reuses TimsConfig and TimsResult so Mailbox<> needs no changes.
#include "corerat/ipc/tims/tims_backend.hpp"
#include "corerat/messaging/wire_message.hpp"
#include "corerat/platform/duration.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <span>
#include <unistd.h>

#ifdef CORERAT_PLATFORM_EVL
#include <evl/evl.h>
#include <evl/mutex.h>
#include <evl/event.h>
#include <evl/clock.h>
#endif

namespace corerat {

// ============================================================================
// Forward declaration for process-local registry
// ============================================================================

class EvlMailbox;

namespace evl_detail {

inline constexpr uint32_t kMaxMailboxes = 256;

/// Process-local registry mapping mailbox_id → EvlMailbox* (Meyers singleton).
/// Written only at initialize()/shutdown() (init/teardown, not hot path).
inline EvlMailbox*& registry_slot(uint32_t id) noexcept {
    static EvlMailbox* reg[kMaxMailboxes]{};
    return reg[id % kMaxMailboxes];
}

}  // namespace evl_detail

// ============================================================================
// EvlMailbox
// ============================================================================

class EvlMailbox {
public:
    /// Metadata compatible with TimsMailbox::TimsMetadata / Metadata
    struct Metadata {
        uint32_t src{0};
        uint32_t dest{0};
        uint32_t seq_nr{0};
        uint8_t  priority{0};
        uint8_t  flags{0};
    };

    explicit EvlMailbox(const TimsConfig& config) : config_(config) {}

    ~EvlMailbox() { shutdown(); }

    EvlMailbox(const EvlMailbox&) = delete;
    EvlMailbox& operator=(const EvlMailbox&) = delete;

    EvlMailbox(EvlMailbox&& other) noexcept
        : config_(std::move(other.config_))
        , created_(other.created_)
        , messages_sent_(other.messages_sent_.load())
        , messages_received_(other.messages_received_.load())
        , slot_meta_(other.slot_meta_)
        , slot_data_(other.slot_data_)
        , capacity_(other.capacity_)
        , slot_size_(other.slot_size_) {
        // EVL objects cannot be moved after creation; only allow pre-create moves
        other.created_   = false;
        other.slot_meta_ = nullptr;
        other.slot_data_ = nullptr;
    }

    EvlMailbox& operator=(EvlMailbox&& other) noexcept {
        if (this != &other) {
            shutdown();
            config_            = std::move(other.config_);
            created_           = other.created_;
            messages_sent_     = other.messages_sent_.load();
            messages_received_ = other.messages_received_.load();
            slot_meta_         = other.slot_meta_;
            slot_data_         = other.slot_data_;
            capacity_          = other.capacity_;
            slot_size_         = other.slot_size_;
            other.created_   = false;
            other.slot_meta_ = nullptr;
            other.slot_data_ = nullptr;
        }
        return *this;
    }

    // ========================================================================
    // Lifecycle — same interface as TimsMailbox
    // ========================================================================

    TimsResult initialize() {
        if (created_) return TimsResult::SUCCESS;

        constexpr uint32_t kSlots = 10;
        const uint32_t slot_size  = static_cast<uint32_t>(config_.max_msg_size);

        slot_meta_ = new SlotMeta[kSlots]{};
        slot_data_ = new uint8_t[kSlots * slot_size]{};
        capacity_  = kSlots;
        slot_size_ = slot_size;

#ifdef CORERAT_PLATFORM_EVL
        evl_init();
        char name[64];
        std::snprintf(name, sizeof(name), "corerat-ring-mtx-%u:%d",
                      config_.mailbox_id, static_cast<int>(getpid()));
        if (evl_new_mutex(&ring_mutex_, "%s", name) < 0) {
            delete[] slot_meta_; slot_meta_ = nullptr;
            delete[] slot_data_; slot_data_ = nullptr;
            return TimsResult::ERROR_INIT;
        }
        std::snprintf(name, sizeof(name), "corerat-ring-evt-%u:%d",
                      config_.mailbox_id, static_cast<int>(getpid()));
        if (evl_new_event(&ring_event_, "%s", name) < 0) {
            evl_close_mutex(&ring_mutex_);
            delete[] slot_meta_; slot_meta_ = nullptr;
            delete[] slot_data_; slot_data_ = nullptr;
            return TimsResult::ERROR_INIT;
        }
#endif

        evl_detail::registry_slot(config_.mailbox_id) = this;
        created_ = true;
        return TimsResult::SUCCESS;
    }

    void shutdown() {
        if (!created_) return;

        if (evl_detail::registry_slot(config_.mailbox_id) == this)
            evl_detail::registry_slot(config_.mailbox_id) = nullptr;

#ifdef CORERAT_PLATFORM_EVL
        evl_close_event(&ring_event_);
        evl_close_mutex(&ring_mutex_);
#endif
        delete[] slot_meta_; slot_meta_ = nullptr;
        delete[] slot_data_; slot_data_ = nullptr;
        created_ = false;
    }

    bool     is_initialized()      const noexcept { return created_; }
    uint32_t get_mailbox_id()      const noexcept { return config_.mailbox_id; }
    uint64_t get_messages_sent()   const noexcept {
        return messages_sent_.load(std::memory_order_relaxed);
    }
    uint64_t get_messages_received() const noexcept {
        return messages_received_.load(std::memory_order_relaxed);
    }

    // ========================================================================
    // Send — same template interface as TimsMailbox
    // ========================================================================

    template<typename T>
    TimsResult send(T& message, uint32_t dest_mailbox_id) {
        static_assert(is_wire_message_v<T>, "T must be a CoreRaT WireMessage type");
        if (!created_) return TimsResult::ERROR_NOT_INIT;
        auto result = corerat::serialize(message);
        auto view   = result.view();
        return send_raw(view.data(), view.size(), dest_mailbox_id,
                        static_cast<int8_t>(config_.priority));
    }

    // ========================================================================
    // Receive — same interface as TimsMailbox
    // ========================================================================

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout) {
        return receive_impl(buffer, timeout, nullptr);
    }

    ssize_t receive_raw_bytes(std::span<std::byte> buffer, Duration timeout,
                              Metadata* metadata) {
        return receive_impl(buffer, timeout, metadata);
    }

private:
    // ========================================================================
    // Slot metadata
    // ========================================================================

    struct SlotMeta {
        int8_t   priority{0};
        bool     in_use{false};
        uint32_t src{0};   // source mailbox ID (for Metadata)
        uint32_t size{0};  // actual bytes stored in this slot
    };

    int find_free_slot() const noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (!slot_meta_[i].in_use) return static_cast<int>(i);
        return -1;
    }

    bool has_in_use_slot() const noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (slot_meta_[i].in_use) return true;
        return false;
    }

    int find_highest_priority_slot() const noexcept {
        int    best = -1;
        int8_t prio = INT8_MIN;
        for (uint32_t i = 0; i < capacity_; ++i) {
            if (slot_meta_[i].in_use && slot_meta_[i].priority > prio) {
                prio = slot_meta_[i].priority;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    /// Evict the lowest-priority in-use slot only if prio_new >= its priority.
    int evict_lowest(int8_t prio_new) noexcept {
        int    worst = -1;
        int8_t prio  = INT8_MAX;
        for (uint32_t i = 0; i < capacity_; ++i) {
            if (slot_meta_[i].in_use && slot_meta_[i].priority < prio) {
                prio  = slot_meta_[i].priority;
                worst = static_cast<int>(i);
            }
        }
        if (worst >= 0 && prio_new >= prio) {
            slot_meta_[worst].in_use = false;
            return worst;
        }
        return -1;
    }

    // ========================================================================
    // post() — write into THIS ring (called by sender via send_raw)
    // OOB-safe: only EVL primitives + memcpy inside the lock.
    // ========================================================================

    bool post(const void* data, size_t size, int8_t priority,
              uint32_t src) noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_lock_mutex(&ring_mutex_);
#endif
        int idx = find_free_slot();
        if (idx < 0) idx = evict_lowest(priority);
        if (idx < 0) {
#ifdef CORERAT_PLATFORM_EVL
            evl_unlock_mutex(&ring_mutex_);
#endif
            return false;  // ring full with higher/equal priority messages
        }

        uint8_t* slot = slot_data_ + static_cast<size_t>(idx) * slot_size_;
        std::memcpy(slot, data, size);
        slot_meta_[idx] = SlotMeta{ priority, true, src,
                                    static_cast<uint32_t>(size) };

#ifdef CORERAT_PLATFORM_EVL
        evl_unlock_mutex(&ring_mutex_);
        evl_signal_event(&ring_event_);
#endif
        return true;
    }

    // ========================================================================
    // send_raw() — look up dest ring and call post()
    // ========================================================================

    TimsResult send_raw(const void* data, size_t size,
                        uint32_t dest, int8_t priority) noexcept {
        if (!data || size == 0 || size > config_.max_msg_size)
            return TimsResult::ERROR_INVALID_MSG;

        EvlMailbox* target = evl_detail::registry_slot(dest);
        if (!target || target->get_mailbox_id() != dest || !target->created_)
            return TimsResult::ERROR_SEND;

        if (!target->post(data, size, priority, config_.mailbox_id))
            return TimsResult::ERROR_SEND;

        messages_sent_.fetch_add(1, std::memory_order_relaxed);
        return TimsResult::SUCCESS;
    }

    // ========================================================================
    // receive_impl() — block on THIS ring until a message arrives
    // ========================================================================

    ssize_t receive_impl(std::span<std::byte> buffer,
                         Duration timeout, Metadata* meta) noexcept {
        if (!created_) return -1;

#ifdef CORERAT_PLATFORM_EVL
        evl_lock_mutex(&ring_mutex_);

        if (!has_in_use_slot()) {
            if (timeout.is_negative()) {
                evl_wait_event(&ring_event_, &ring_mutex_);
            } else {
                struct timespec now{};
                evl_read_clock(EVL_CLOCK_MONOTONIC, &now);
                const int64_t deadline =
                    static_cast<int64_t>(now.tv_sec)  * 1'000'000'000LL
                  + static_cast<int64_t>(now.tv_nsec)
                  + timeout.count_ns();
                struct timespec abs_ts{};
                abs_ts.tv_sec  = static_cast<time_t>(deadline / 1'000'000'000LL);
                abs_ts.tv_nsec = static_cast<long>(deadline % 1'000'000'000LL);

                if (evl_timedwait_event(&ring_event_, &ring_mutex_, &abs_ts)
                        == -ETIMEDOUT) {
                    evl_unlock_mutex(&ring_mutex_);
                    return -ETIMEDOUT;
                }
            }
            if (!has_in_use_slot()) {
                evl_unlock_mutex(&ring_mutex_);
                return -1;
            }
        }

        const int idx = find_highest_priority_slot();
        const uint8_t* slot = slot_data_ + static_cast<size_t>(idx) * slot_size_;
        const size_t   sz   = slot_meta_[idx].size;

        if (sz > buffer.size()) {
            evl_unlock_mutex(&ring_mutex_);
            return -EMSGSIZE;
        }

        std::memcpy(buffer.data(), slot, sz);

        if (meta) {
            WireHeader hdr{};
            if (sz >= sizeof(WireHeader)) std::memcpy(&hdr, slot, sizeof(WireHeader));
            meta->src      = slot_meta_[idx].src;
            meta->dest     = config_.mailbox_id;
            meta->seq_nr   = hdr.seq_number;
            meta->priority = static_cast<uint8_t>(slot_meta_[idx].priority);
            meta->flags    = static_cast<uint8_t>(hdr.flags);
        }

        slot_meta_[idx].in_use = false;
        evl_unlock_mutex(&ring_mutex_);

        messages_received_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<ssize_t>(sz);
#else
        (void)buffer; (void)timeout; (void)meta;
        return -1;  // EVL platform not enabled
#endif
    }

    // ========================================================================
    // Data members
    // ========================================================================

    TimsConfig            config_;
    bool                  created_{false};
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_received_{0};

#ifdef CORERAT_PLATFORM_EVL
    struct evl_mutex ring_mutex_{};
    struct evl_event ring_event_{};
#endif

    SlotMeta* slot_meta_{nullptr};
    uint8_t*  slot_data_{nullptr};
    uint32_t  capacity_{0};
    uint32_t  slot_size_{0};
};

}  // namespace corerat

