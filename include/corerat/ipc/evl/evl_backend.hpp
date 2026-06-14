#pragma once

/**
 * @file evl/evl_backend.hpp
 * @brief EVL-native IPC backend for CoreRaT
 *
 * Two operating modes selected at initialize() time:
 *
 *   LOCAL  — single-process / multi-thread (default).
 *            Ring buffer lives in heap memory.
 *            Routing via evl_detail::registry_slot[] (process-local pointer table).
 *
 *   PUBLIC — cross-process.
 *            Ring buffer lives in a POSIX SHM region ("/corerat_mbx_<id>").
 *            SlotMeta[] is stored inside the SHM so senders in other processes
 *            can post directly without any IPC copy step.
 *            EVL mutex/event created with EVL_CLONE_PUBLIC — any process can
 *            open them by name.  Names are deterministic from mailbox_id so
 *            no name-service (evl-router) lookup is needed on the data path.
 *
 * SHM layout (PUBLIC mode):
 *   [ShmLayout (16 bytes)] [SlotMeta × capacity] [payload × capacity × slot_size]
 *
 * Hot-path OOB operations (no mode switch):
 *   send:    evl_lock → find/evict slot → memcpy → mark in_use → evl_unlock → evl_signal
 *   receive: evl_timedwait/evl_wait → memcpy → mark free → evl_unlock
 */

#include "corerat/ipc/tims/tims_backend.hpp"   // TimsConfig, TimsResult
#include "corerat/messaging/wire_message.hpp"
#include "corerat/platform/duration.hpp"
#include <array>
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
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef CORERAT_PLATFORM_EVL
#include <evl/evl.h>
#include <evl/mutex.h>
#include <evl/event.h>
#include <evl/clock.h>
#include <evl/factory-abi.h>
#include <evl/net.h>
#endif

namespace corerat {

// ============================================================================
// Forward declaration for process-local registry
// ============================================================================

class EvlMailbox;

namespace evl_detail {

inline constexpr uint32_t kMaxMailboxes = 256;

/// Process-local registry: mailbox_id → EvlMailbox* (LOCAL mode only).
inline EvlMailbox*& registry_slot(uint32_t id) noexcept {
    static EvlMailbox* reg[kMaxMailboxes]{};
    return reg[id % kMaxMailboxes];
}

/// Build an EVL object name into a fixed buffer.
inline std::string_view evl_name(char (&buf)[64],
                                  std::string_view prefix,
                                  uint32_t id) noexcept {
    const int n = std::snprintf(buf, sizeof(buf), "%.*s%u",
                                static_cast<int>(prefix.size()),
                                prefix.data(), id);
    return { buf, static_cast<std::string_view::size_type>(n) };
}

/// POSIX shm_open name for a mailbox (PUBLIC mode).
inline std::string_view shm_name(char (&buf)[64], uint32_t id) noexcept {
    return evl_name(buf, "/corerat_mbx_", id);
}

}  // namespace evl_detail

// ============================================================================
// SHM layout header
//
// Stored at offset 0 of the SHM region in PUBLIC mode.  Written once by the
// creator; senders in other processes read it to learn capacity/slot_size.
// ============================================================================

struct ShmLayout {
    uint32_t capacity;
    uint32_t slot_size;
    uint32_t _pad[2];    // pad to 16 bytes
};

static_assert(sizeof(ShmLayout) == 16, "ShmLayout must be 16 bytes");

// ============================================================================
// ============================================================================
// EvlNetworkConfig — system-id-based route table for Mode::Network
//
// Routing follows the RACK/TiMS convention:
//   mailbox_id[31:24] = system_id  (identifies the host)
//   mailbox_id[23:0]  = class/instance/local fields
//
// Port assignment: kOobBasePort + (mailbox_id & 0x7FFF)
//   → ports 42000–74767, safe range for registered/ephemeral split.
//
// On send:  extract dest_sys = (dest_mailbox_id >> 24) & 0xFF
//           look up Route by system_id → get destination IP
//           destination port = kOobBasePort + (dest_mailbox_id & 0x7FFF)
//
// evl_net_solicit() is called once per remote host at initialize() so
// EVL's ARP/route front-caches are primed and every subsequent
// oob_sendmsg() stays fully on the out-of-band stage.
// ============================================================================

struct EvlNetworkConfig {
    static constexpr uint16_t kOobBasePort  = 42000;
    static constexpr uint8_t  kMaxRoutes    = 8;

    /// Route table entry: one per remote host.
    struct Route {
        uint8_t system_id{0};
        char    ip[16]{};  ///< IPv4 dotted-decimal, e.g. "10.10.10.11"
    };

    uint8_t                         local_system_id{0};
    std::array<Route, kMaxRoutes>   routes{};
    uint8_t                         route_count{0};

    static constexpr uint16_t port_for(uint32_t mailbox_id) noexcept {
        return static_cast<uint16_t>(kOobBasePort + (mailbox_id & 0x7FFFu));
    }
    static constexpr uint8_t system_id_of(uint32_t mailbox_id) noexcept {
        return static_cast<uint8_t>((mailbox_id >> 24) & 0xFFu);
    }
};

// ============================================================================
// EvlMailbox
// ============================================================================

class EvlMailbox {
public:
    enum class Mode { Local, Public, Network };

    /// Metadata compatible with TimsMailbox::Metadata
    struct Metadata {
        uint32_t src{0};
        uint32_t dest{0};
        uint32_t seq_nr{0};
        uint8_t  priority{0};
        uint8_t  flags{0};
    };

    explicit EvlMailbox(const TimsConfig& config,
                        Mode mode = Mode::Local,
                        const EvlNetworkConfig& net_config = {})
        : config_(config), mode_(mode), net_config_(net_config) {}

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

        // Network mode: OOB UDP socket, no ring buffer needed
        if (mode_ == Mode::Network) {
            if (!create_oob_socket()) return TimsResult::ERROR_INIT;
            created_ = true;
            return TimsResult::SUCCESS;
        }

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

        // Network mode: just close the OOB socket
        if (mode_ == Mode::Network) {
            if (oob_sock_ >= 0) { ::close(oob_sock_); oob_sock_ = -1; }
            created_ = false;
            return;
        }

        close_all_remotes();

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
    // SlotMeta — stored in ring (SHM in PUBLIC mode, heap in LOCAL mode)
    // ========================================================================

    struct SlotMeta {
        int8_t   priority{0};
        bool     in_use{false};
        uint32_t src{0};
        uint32_t size{0};
    };

    // ========================================================================
    // Accessors that dispatch on mode
    // ========================================================================

    SlotMeta& slot_meta_at(uint32_t idx) noexcept {
        if (mode_ == Mode::Public && shm_base_) {
            return reinterpret_cast<SlotMeta*>(
                static_cast<uint8_t*>(shm_base_) + sizeof(ShmLayout))[idx];
        }
        return slot_meta_[idx];
    }

    uint8_t* slot_ptr(uint32_t idx) noexcept {
        if (mode_ == Mode::Public && shm_base_) {
            return static_cast<uint8_t*>(shm_base_)
                   + sizeof(ShmLayout)
                   + capacity_ * sizeof(SlotMeta)
                   + idx * slot_size_;
        }
        return heap_data_.get() + idx * slot_size_;
    }

    static std::size_t shm_total(uint32_t cap, uint32_t slot_sz) noexcept {
        return sizeof(ShmLayout)
             + static_cast<std::size_t>(cap) * sizeof(SlotMeta)
             + static_cast<std::size_t>(cap) * slot_sz;
    }

    // ========================================================================
    // Ring buffer allocation
    // ========================================================================

    bool allocate_ring(uint32_t slots, uint32_t slot_size) noexcept {
        if (mode_ == Mode::Public) {
            char name_buf[64];
            const auto name = evl_detail::shm_name(name_buf, config_.mailbox_id);

            shm_fd_ = ::shm_open(name.data(), O_CREAT | O_RDWR, 0600);
            if (shm_fd_ < 0) return false;

            const std::size_t total = shm_total(slots, slot_size);
            if (::ftruncate(shm_fd_, static_cast<off_t>(total)) < 0) {
                ::close(shm_fd_); shm_fd_ = -1;
                return false;
            }
            void* p = ::mmap(nullptr, total,
                             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
            if (p == MAP_FAILED) {
                ::close(shm_fd_); shm_fd_ = -1;
                return false;
            }
            shm_base_ = p;
            shm_size_ = total;

            // Write layout header (rest is zero-initialised by mmap)
            auto* layout = static_cast<ShmLayout*>(shm_base_);
            layout->capacity  = slots;
            layout->slot_size = slot_size;
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
        if (shm_base_) {
            ::munmap(shm_base_, shm_size_);
            shm_base_ = nullptr;
            char name_buf[64];
            const auto name = evl_detail::shm_name(name_buf, config_.mailbox_id);
            ::shm_unlink(name.data());
        }
        if (shm_fd_ >= 0) { ::close(shm_fd_); shm_fd_ = -1; }
        slot_meta_.reset();
        heap_data_.reset();
        capacity_ = slot_size_ = 0;
    }

    // ========================================================================
    // EVL object creation / destruction
    // ========================================================================

    bool create_evl_objects() noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_init();
        const int flags  = (mode_ == Mode::Public)
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
    // Ring slot helpers (use slot_meta_at() for mode-transparent access)
    // ========================================================================

    int find_free_slot() noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (!slot_meta_at(i).in_use) return static_cast<int>(i);
        return -1;
    }

    bool has_in_use_slot() noexcept {
        for (uint32_t i = 0; i < capacity_; ++i)
            if (slot_meta_at(i).in_use) return true;
        return false;
    }

    int find_highest_priority_slot() noexcept {
        int    best = -1;
        int8_t prio = INT8_MIN;
        for (uint32_t i = 0; i < capacity_; ++i) {
            auto& m = slot_meta_at(i);
            if (m.in_use && m.priority > prio) {
                prio = m.priority;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    int evict_lowest(int8_t prio_new) noexcept {
        int    worst = -1;
        int8_t prio  = INT8_MAX;
        for (uint32_t i = 0; i < capacity_; ++i) {
            auto& m = slot_meta_at(i);
            if (m.in_use && m.priority < prio) {
                prio  = m.priority;
                worst = static_cast<int>(i);
            }
        }
        if (worst >= 0 && prio_new >= prio) {
            slot_meta_at(worst).in_use = false;
            return worst;
        }
        return -1;
    }

    // ========================================================================
    // post() — write into THIS mailbox's ring (OOB-safe)
    // Called by the owning process or by send_raw() for LOCAL same-process sends.
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
        slot_meta_at(idx) = SlotMeta{ priority, true, src, static_cast<uint32_t>(size) };

#ifdef CORERAT_PLATFORM_EVL
        evl_unlock_mutex(&ring_mutex_);
        evl_signal_event(&ring_event_);
#endif
        return true;
    }

    // ========================================================================
    // OOB UDP socket — Mode::Network implementation
    //
    // Bind port: kOobBasePort + (my mailbox_id & 0x7FFF)
    // Send port: kOobBasePort + (dest_mailbox_id & 0x7FFF) at dest host's IP
    // Route key: dest_system_id = (dest_mailbox_id >> 24) & 0xFF
    //
    // evl_net_solicit() is called once per remote host (not per mailbox) at
    // initialize() time, priming EVL's ARP/route front-caches so every
    // subsequent oob_sendmsg() stays fully on the out-of-band stage.
    // ========================================================================

    bool create_oob_socket() noexcept {
#ifdef CORERAT_PLATFORM_EVL
        evl_init();

        oob_sock_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0);
        if (oob_sock_ < 0) return false;

        // Bind to INADDR_ANY : kOobBasePort + (mailbox_id & 0x7FFF)
        struct sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(EvlNetworkConfig::port_for(config_.mailbox_id));
        local.sin_addr.s_addr = INADDR_ANY;
        if (::bind(oob_sock_,
                   reinterpret_cast<struct sockaddr*>(&local),
                   sizeof(local)) < 0) {
            ::close(oob_sock_); oob_sock_ = -1;
            return false;
        }

        // Build route table and prime EVL ARP/route front-caches once per host.
        // Port used for solicitation is the base port; the actual per-mailbox
        // port is resolved at send time from the dest mailbox_id.
        const uint8_t count = std::min(net_config_.route_count,
                                       EvlNetworkConfig::kMaxRoutes);
        for (uint8_t i = 0; i < count; ++i) {
            const auto& r = net_config_.routes[i];
            struct sockaddr_in peer{};
            peer.sin_family = AF_INET;
            peer.sin_port   = htons(EvlNetworkConfig::kOobBasePort); // for solicitation
            if (::inet_pton(AF_INET, r.ip, &peer.sin_addr) <= 0) continue;

            net_routes_[i].system_id = r.system_id;
            net_routes_[i].ip_addr   = peer.sin_addr;
            net_routes_[i].valid     = true;

            // EVL_NEIGH_PERMANENT — ARP entry never ages out.
            // EVL_NEIGH_MAYROUTE  — allows path through a gateway (switch OK).
            evl_net_solicit(oob_sock_,
                            reinterpret_cast<const struct sockaddr*>(&peer),
                            EVL_NEIGH_PERMANENT | EVL_NEIGH_MAYROUTE);
        }
        return true;
#else
        return false;
#endif
    }

    TimsResult send_oob_udp(const void* data, std::size_t size,
                            uint32_t dest) noexcept {
#ifdef CORERAT_PLATFORM_EVL
        const uint8_t dest_sys = EvlNetworkConfig::system_id_of(dest);

        const NetworkRouteEntry* route = nullptr;
        for (const auto& r : net_routes_)
            if (r.valid && r.system_id == dest_sys) { route = &r; break; }
        if (!route) return TimsResult::ERROR_SEND;

        // Build destination address: route IP + mailbox-specific port
        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(EvlNetworkConfig::port_for(dest));
        dst.sin_addr   = route->ip_addr;

        struct iovec iov{};
        iov.iov_base = const_cast<void*>(data);
        iov.iov_len  = size;

        struct oob_msghdr msghdr{};
        msghdr.msg_name    = &dst;
        msghdr.msg_namelen = sizeof(dst);
        msghdr.msg_iov     = &iov;
        msghdr.msg_iovlen  = 1;

        if (oob_sendmsg(oob_sock_, &msghdr, nullptr, 0) < 0)
            return TimsResult::ERROR_SEND;
        messages_sent_.fetch_add(1, std::memory_order_relaxed);
        return TimsResult::SUCCESS;
#else
        (void)data; (void)size; (void)dest;
        return TimsResult::ERROR_SEND;
#endif
    }

    ssize_t receive_oob_udp(std::span<std::byte> buffer,
                            Duration timeout, Metadata* meta) noexcept {
#ifdef CORERAT_PLATFORM_EVL
        struct iovec iov{};
        iov.iov_base = buffer.data();
        iov.iov_len  = buffer.size();

        struct oob_msghdr msghdr{};
        msghdr.msg_iov    = &iov;
        msghdr.msg_iovlen = 1;

        // oob_recvmsg timeout is a relative duration (not absolute deadline)
        struct timespec ts{};
        const struct timespec* ts_ptr = nullptr;
        if (!timeout.is_negative()) {
            const int64_t ns = timeout.count_ns();
            ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000LL);
            ts.tv_nsec = static_cast<long>(ns  % 1'000'000'000LL);
            ts_ptr = &ts;
        }

        const ssize_t ret = oob_recvmsg(oob_sock_, &msghdr, ts_ptr, 0);
        if (ret <= 0) return ret;

        if (meta) {
            WireHeader hdr{};
            if (static_cast<std::size_t>(ret) >= sizeof(WireHeader))
                std::memcpy(&hdr, buffer.data(), sizeof(WireHeader));
            meta->src      = hdr.src;
            meta->dest     = hdr.dest;
            meta->seq_nr   = hdr.seq_number;
            meta->priority = 0;
            meta->flags    = static_cast<uint8_t>(hdr.flags);
        }

        messages_received_.fetch_add(1, std::memory_order_relaxed);
        return ret;
#else
        (void)buffer; (void)timeout; (void)meta;
        return -1;
#endif
    }

    // ========================================================================
    // Remote handle — opened on demand for PUBLIC cross-process sends.
    //
    // Names are fully deterministic from dest_id, so no evl-router lookup is
    // needed on the data path:
    //   SHM:   /corerat_mbx_<dest_id>
    //   mutex: corerat-ring-mtx-<dest_id>
    //   event: corerat-ring-evt-<dest_id>
    //
    // ShmLayout is mapped to discover the ring dimensions, then SlotMeta[] and
    // payload[] are accessed inside the same mmap region.
    // ========================================================================

    struct RemoteHandle {
        uint32_t    dest_id{0};
        void*       shm_base{nullptr};  // mmap of remote ring
        std::size_t shm_size{0};
        int         shm_fd{-1};
        bool        valid{false};
#ifdef CORERAT_PLATFORM_EVL
        struct evl_mutex mutex{};
        struct evl_event event{};
#endif
    };

    static constexpr uint32_t kMaxRemoteHandles = 16;
    std::array<RemoteHandle, kMaxRemoteHandles> remote_handles_{};

    RemoteHandle* get_or_open_remote(uint32_t dest) noexcept {
        for (auto& h : remote_handles_)
            if (h.valid && h.dest_id == dest) return &h;
        for (auto& h : remote_handles_)
            if (!h.valid) { return open_remote(h, dest) ? &h : nullptr; }
        return nullptr;  // cache full
    }

    bool open_remote(RemoteHandle& h, uint32_t dest) noexcept {
        char name_buf[64];
        evl_detail::shm_name(name_buf, dest);

        // Receiver must have called initialize() already
        const int fd = ::shm_open(name_buf, O_RDWR, 0);
        if (fd < 0) return false;

        // Read layout header to learn ring dimensions
        ShmLayout layout{};
        if (::pread(fd, &layout, sizeof(layout), 0)
                != static_cast<ssize_t>(sizeof(layout))) {
            ::close(fd); return false;
        }

        const std::size_t total = shm_total(layout.capacity, layout.slot_size);
        void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return false; }

#ifdef CORERAT_PLATFORM_EVL
        evl_detail::evl_name(name_buf, "corerat-ring-mtx-", dest);
        if (evl_open_mutex(&h.mutex, "%s", name_buf) < 0) {
            ::munmap(p, total); ::close(fd); return false;
        }
        evl_detail::evl_name(name_buf, "corerat-ring-evt-", dest);
        if (evl_open_event(&h.event, "%s", name_buf) < 0) {
            evl_close_mutex(&h.mutex); ::munmap(p, total); ::close(fd); return false;
        }
#endif
        h.dest_id  = dest;
        h.shm_base = p;
        h.shm_size = total;
        h.shm_fd   = fd;
        h.valid    = true;
        return true;
    }

    void close_all_remotes() noexcept {
        for (auto& h : remote_handles_) {
            if (!h.valid) continue;
#ifdef CORERAT_PLATFORM_EVL
            evl_close_event(&h.event);
            evl_close_mutex(&h.mutex);
#endif
            ::munmap(h.shm_base, h.shm_size);
            ::close(h.shm_fd);
            h = RemoteHandle{};
        }
    }

    /// Post directly into a remote process's ring buffer via its mapped SHM.
    bool post_to_remote(RemoteHandle& h, const void* data,
                        std::size_t size, int8_t priority) noexcept {
        auto* layout  = static_cast<ShmLayout*>(h.shm_base);
        auto* meta    = reinterpret_cast<SlotMeta*>(
                            static_cast<uint8_t*>(h.shm_base) + sizeof(ShmLayout));
        auto* pbase   = static_cast<uint8_t*>(h.shm_base)
                      + sizeof(ShmLayout)
                      + layout->capacity * sizeof(SlotMeta);

#ifdef CORERAT_PLATFORM_EVL
        evl_lock_mutex(&h.mutex);
#endif
        // Find free slot or evict lowest-priority message
        int idx = -1;
        for (uint32_t i = 0; i < layout->capacity; ++i) {
            if (!meta[i].in_use) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) {
            int worst = -1; int8_t wprio = INT8_MAX;
            for (uint32_t i = 0; i < layout->capacity; ++i) {
                if (meta[i].in_use && meta[i].priority < wprio) {
                    wprio = meta[i].priority;
                    worst = static_cast<int>(i);
                }
            }
            if (worst >= 0 && priority >= wprio) {
                meta[worst].in_use = false;
                idx = worst;
            }
        }
        if (idx < 0) {
#ifdef CORERAT_PLATFORM_EVL
            evl_unlock_mutex(&h.mutex);
#endif
            return false;
        }

        const auto sz = std::min(size, static_cast<std::size_t>(layout->slot_size));
        std::memcpy(pbase + idx * layout->slot_size, data, sz);
        meta[idx] = SlotMeta{ priority, true, config_.mailbox_id,
                              static_cast<uint32_t>(sz) };

#ifdef CORERAT_PLATFORM_EVL
        evl_unlock_mutex(&h.mutex);
        evl_signal_event(&h.event);
#endif
        return true;
    }

    // ========================================================================
    // send_raw() — dispatches LOCAL (registry lookup) vs PUBLIC (cross-process)
    // ========================================================================

    TimsResult send_raw(const void* data, std::size_t size,
                        uint32_t dest, int8_t priority) noexcept {
        if (!data || size == 0 || size > config_.max_msg_size)
            return TimsResult::ERROR_INVALID_MSG;

        if (mode_ == Mode::Network)
            return send_oob_udp(data, size, dest);

        bool ok = false;

        if (mode_ == Mode::Public) {
            // Same-process shortcut (both mailboxes are PUBLIC, same process)
            EvlMailbox* local = evl_detail::registry_slot(dest);
            if (local && local->get_mailbox_id() == dest && local->created_) {
                ok = local->post(data, size, priority, config_.mailbox_id);
            } else {
                // Cross-process: open remote SHM + EVL handles by deterministic name
                RemoteHandle* rh = get_or_open_remote(dest);
                if (!rh) return TimsResult::ERROR_SEND;
                ok = post_to_remote(*rh, data, size, priority);
            }
        } else {
            // LOCAL: process-local registry only
            EvlMailbox* target = evl_detail::registry_slot(dest);
            if (!target || target->get_mailbox_id() != dest || !target->created_)
                return TimsResult::ERROR_SEND;
            ok = target->post(data, size, priority, config_.mailbox_id);
        }

        if (!ok) return TimsResult::ERROR_SEND;
        messages_sent_.fetch_add(1, std::memory_order_relaxed);
        return TimsResult::SUCCESS;
    }

    // ========================================================================
    // receive_impl()
    // ========================================================================

    ssize_t receive_impl(std::span<std::byte> buffer,
                         Duration timeout, Metadata* meta) noexcept {
        if (!created_) return -1;

        if (mode_ == Mode::Network)
            return receive_oob_udp(buffer, timeout, meta);

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

        const int      idx = find_highest_priority_slot();
        const auto&    sm  = slot_meta_at(idx);
        const auto     sz  = static_cast<std::size_t>(sm.size);

        if (sz > buffer.size()) {
            evl_unlock_mutex(&ring_mutex_);
            return -EMSGSIZE;
        }

        std::memcpy(buffer.data(), slot_ptr(idx), sz);

        if (meta) {
            WireHeader hdr{};
            if (sz >= sizeof(WireHeader)) std::memcpy(&hdr, buffer.data(), sizeof(WireHeader));
            meta->src      = sm.src;
            meta->dest     = config_.mailbox_id;
            meta->seq_nr   = hdr.seq_number;
            meta->priority = static_cast<uint8_t>(sm.priority);
            meta->flags    = static_cast<uint8_t>(hdr.flags);
        }

        slot_meta_at(idx).in_use = false;
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

    // Ring storage
    std::unique_ptr<SlotMeta[]> slot_meta_;   // LOCAL: heap meta array
    std::unique_ptr<uint8_t[]>  heap_data_;   // LOCAL: heap payload
    void*                       shm_base_{nullptr};  // PUBLIC: mmap base (header+meta+payload)
    std::size_t                 shm_size_{0};
    int                         shm_fd_{-1};

    uint32_t capacity_{0};
    uint32_t slot_size_{0};

    // Remote handle cache for PUBLIC cross-process sends
    std::array<RemoteHandle, kMaxRemoteHandles> remote_handles_{};

    // ── Network mode (Mode::Network) ─────────────────────────────────────────
    EvlNetworkConfig net_config_{};
    int              oob_sock_{-1};

    /// One entry per remote host (not per mailbox).
    struct NetworkRouteEntry {
        uint8_t          system_id{0};
        struct in_addr   ip_addr{};
        bool             valid{false};
    };
    std::array<NetworkRouteEntry, EvlNetworkConfig::kMaxRoutes> net_routes_{};
};

}  // namespace corerat
