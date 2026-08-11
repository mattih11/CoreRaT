/**
 * @file evl/threading_impl.hpp
 * @brief EVL (Xenomai 4 / libevl) threading backend for CoreRaT
 *
 * Implements Thread, Mutex, SharedMutex, ConditionVariable using
 * EVL out-of-band (OOB) primitives for hard real-time guarantees.
 *
 * This file is included by corerat/platform/threading.hpp when
 * CORERAT_PLATFORM_EVL is defined.
 *
 * WARNING: Any glibc/kernel syscall from OOB context demotes to in-band.
 * Forbidden in OOB: malloc, new, delete, std::cout, throw, POSIX mutexes.
 * Only libevl functions + memcpy/std::atomic are safe in OOB context.
 */

#pragma once

#include <evl/evl.h>
#include <evl/thread.h>
#include <evl/mutex.h>
#include <evl/rwlock.h>
#include <evl/event.h>
#include <evl/clock.h>
#include <evl/sched.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace corerat {

// ============================================================================
// Thread (pthread + evl_attach_self OOB scheduling)
// ============================================================================

class Thread {
public:
    Thread() : config_{} {}

    template<typename Func>
    explicit Thread(Func&& func)
        : Thread(ThreadConfig{}, std::forward<Func>(func)) {
    }

    template<typename Func>
    Thread(const ThreadConfig& config, Func&& func)
        : config_(config) {
        start(std::forward<Func>(func));
    }

    explicit Thread(const ThreadConfig& config)
        : config_(config) {
    }

    ~Thread() {
        if (joinable()) {
            join();
        }
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&& other) noexcept
        : config_(std::move(other.config_))
        , posix_thread_(other.posix_thread_)
        , joinable_(other.joinable_) {
        other.joinable_ = false;
    }

    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            if (joinable_) join();
            config_        = std::move(other.config_);
            posix_thread_  = other.posix_thread_;
            joinable_      = other.joinable_;
            other.joinable_ = false;
        }
        return *this;
    }

    template<typename Func>
    void start(Func&& func) {
        if (joinable_) {
            return;
        }

        // Heap allocation intentionally only at thread creation (init phase, not hot path)
        auto* ctx = new ThreadContext{config_, std::forward<Func>(func)};

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (config_.stack_size > 0) {
            pthread_attr_setstacksize(&attr, config_.stack_size);
        }

        int ret = pthread_create(&posix_thread_, &attr, &Thread::thread_entry, ctx);
        pthread_attr_destroy(&attr);

        if (ret != 0) {
            delete ctx;
            return;
        }
        joinable_ = true;
    }

    void join() {
        if (joinable_) {
            pthread_join(posix_thread_, nullptr);
            joinable_ = false;
        }
    }

    void detach() {
        if (joinable_) {
            pthread_detach(posix_thread_);
            joinable_ = false;
        }
    }

    bool joinable() const noexcept { return joinable_; }

    pthread_t native_handle() const noexcept { return posix_thread_; }

    const ThreadConfig& config() const noexcept { return config_; }

private:
    struct ThreadContext {
        ThreadConfig config;
        std::function<void()> func;
    };

    static void* thread_entry(void* arg) noexcept {
        auto* ctx = static_cast<ThreadContext*>(arg);
        ThreadConfig cfg = ctx->config;
        auto func = std::move(ctx->func);
        delete ctx;  // Free before entering RT loop

        // Attach to EVL core (enables OOB scheduling)
        char name[64];
        std::snprintf(name, sizeof(name), "corerat-%s:%d",
                      cfg.name.substr(0, 40).c_str(), static_cast<int>(getpid()));
        int efd = evl_attach_self("%s", name);
        if (efd >= 0) {
            if (cfg.priority != ThreadPriority::IDLE) {
                struct evl_sched_attrs attrs;
                std::memset(&attrs, 0, sizeof(attrs));
                attrs.sched_policy   = SCHED_FIFO;
                attrs.sched_priority = static_cast<int>(cfg.priority);
                evl_set_schedattr(efd, &attrs);
            }

            // Enable health monitoring: warn on accidental in-band switches
            evl_set_thread_mode(efd, EVL_T_WOSS | EVL_T_HMSIG, nullptr);
        }

        if (cfg.cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cfg.cpu_affinity, &cpuset);
            sched_setaffinity(0, sizeof(cpuset), &cpuset);
        }

        func();

        if (efd >= 0) {
            evl_detach_self();
        }
        return nullptr;
    }

    ThreadConfig config_;
    pthread_t    posix_thread_{};
    bool         joinable_{false};
};

// ============================================================================
// Mutex (evl_mutex with priority inheritance)
// ============================================================================

class Mutex {
public:
    Mutex() {
        evl_init();
        char name[64];
        static std::atomic<int> counter{0};
        std::snprintf(name, sizeof(name), "corerat-mtx-%d:%d",
                      counter.fetch_add(1), static_cast<int>(getpid()));
        evl_new_mutex(&evl_mutex_, "%s", name);
    }

    ~Mutex() {
        evl_close_mutex(&evl_mutex_);
    }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock()      { evl_lock_mutex(&evl_mutex_); }
    bool try_lock()  { return evl_trylock_mutex(&evl_mutex_) == 0; }
    void unlock()    { evl_unlock_mutex(&evl_mutex_); }

    struct evl_mutex* native_handle() noexcept { return &evl_mutex_; }

private:
    struct evl_mutex evl_mutex_;
};

// ============================================================================
// SharedMutex (evl_rwlock - writer-biased)
// ============================================================================

class SharedMutex {
public:
    SharedMutex() {
        evl_init();
        evl_create_rwlock(&evl_rwlock_);
    }

    ~SharedMutex() {
        evl_destroy_rwlock(&evl_rwlock_);
    }

    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator=(const SharedMutex&) = delete;

    void lock()              { evl_lock_write(&evl_rwlock_); }
    void lock_shared()       { evl_lock_read(&evl_rwlock_); }
    bool try_lock()          { return evl_trylock_write(&evl_rwlock_) == 0; }
    bool try_lock_shared()   { return evl_trylock_read(&evl_rwlock_) == 0; }
    void unlock()            { evl_unlock_write(&evl_rwlock_); }
    void unlock_shared()     { evl_unlock_read(&evl_rwlock_); }

private:
    struct evl_rwlock evl_rwlock_;
};

// ============================================================================
// ConditionVariable (evl_event paired with evl_mutex)
// ============================================================================

class ConditionVariable {
public:
    ConditionVariable() {
        evl_init();
        char name[64];
        static std::atomic<int> counter{0};
        std::snprintf(name, sizeof(name), "corerat-evt-%d:%d",
                      counter.fetch_add(1), static_cast<int>(getpid()));
        evl_new_event(&evl_event_, "%s", name);
    }

    ~ConditionVariable() {
        evl_close_event(&evl_event_);
    }

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void notify_one() noexcept { evl_signal_event(&evl_event_); }
    void notify_all() noexcept { evl_broadcast_event(&evl_event_); }

    void wait(std::unique_lock<Mutex>& lock) {
        evl_wait_event(&evl_event_, lock.mutex()->native_handle());
    }

    template<typename Predicate>
    void wait(std::unique_lock<Mutex>& lock, Predicate pred) {
        while (!pred()) {
            evl_wait_event(&evl_event_, lock.mutex()->native_handle());
        }
    }

    CvStatus wait_for(std::unique_lock<Mutex>& lock, Duration timeout) {
        struct timespec now_ts;
        evl_read_clock(EVL_CLOCK_MONOTONIC, &now_ts);
        int64_t deadline_ns = static_cast<int64_t>(now_ts.tv_sec) * 1'000'000'000LL
                            + static_cast<int64_t>(now_ts.tv_nsec)
                            + timeout.count_ns();
        struct timespec abs_ts;
        abs_ts.tv_sec  = static_cast<time_t>(deadline_ns / 1'000'000'000LL);
        abs_ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000LL);

        int ret = evl_timedwait_event(&evl_event_, lock.mutex()->native_handle(), &abs_ts);
        return (ret == -ETIMEDOUT) ? CvStatus::TIMEOUT : CvStatus::NO_TIMEOUT;
    }

    template<typename Predicate>
    bool wait_for(std::unique_lock<Mutex>& lock, Duration timeout, Predicate pred) {
        if (pred()) return true;

        struct timespec now_ts;
        evl_read_clock(EVL_CLOCK_MONOTONIC, &now_ts);
        int64_t deadline_ns = static_cast<int64_t>(now_ts.tv_sec) * 1'000'000'000LL
                            + static_cast<int64_t>(now_ts.tv_nsec)
                            + timeout.count_ns();
        struct timespec abs_ts;
        abs_ts.tv_sec  = static_cast<time_t>(deadline_ns / 1'000'000'000LL);
        abs_ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000LL);

        while (!pred()) {
            int ret = evl_timedwait_event(&evl_event_, lock.mutex()->native_handle(), &abs_ts);
            if (ret == -ETIMEDOUT) return pred();
        }
        return true;
    }

private:
    struct evl_event evl_event_;
};

} // namespace corerat
