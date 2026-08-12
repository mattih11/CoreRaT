/**
 * @file std/threading_impl.hpp
 * @brief Standard C++ threading backend for CoreRaT
 *
 * Implements Thread, Mutex, SharedMutex, ConditionVariable using
 * std::thread, std::mutex, std::shared_mutex, std::condition_variable_any.
 *
 * This file is included by corerat/platform/threading.hpp when
 * CORERAT_PLATFORM_STD is defined (default).
 *
 * Not intended for direct inclusion by user code.
 */

#pragma once

#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <functional>
#include <string>
#include <cstdint>

#include <pthread.h>
#include <sched.h>

namespace corerat {

// ============================================================================
// Thread (std::thread wrapper)
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
        : config_(config),
          thread_([cfg = config, f = std::forward<Func>(func)]() mutable {
              apply_config(cfg);
              f();
          }) {
    }

    explicit Thread(const ThreadConfig& config)
        : config_(config) {
    }

    ~Thread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // std::thread move already clears the source (leaves it non-joinable).
    Thread(Thread&& other) noexcept
        : config_(std::move(other.config_))
        , thread_(std::move(other.thread_)) {}

    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) thread_.join();
            config_ = std::move(other.config_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    template<typename Func>
    void start(Func&& func) {
        if (thread_.joinable()) {
            return;
        }
        auto cfg = config_;
        thread_ = std::thread([cfg, f = std::forward<Func>(func)]() mutable {
            apply_config(cfg);
            f();
        });
    }

    template<typename Func>
    void start(const ThreadConfig& config, Func&& func) {
        config_ = config;
        start(std::forward<Func>(func));
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void detach() {
        if (thread_.joinable()) {
            thread_.detach();
        }
    }

    bool joinable() const noexcept { return thread_.joinable(); }

    auto native_handle() { return thread_.native_handle(); }

    auto get_id() const noexcept { return thread_.get_id(); }

    const ThreadConfig& config() const noexcept { return config_; }

private:
    static void apply_config(const ThreadConfig& config) {
        pthread_t handle = pthread_self();

#ifdef __linux__
        if (!config.name.empty()) {
            pthread_setname_np(handle, config.name.substr(0, 15).c_str());
        }
#endif

        if (config.policy != SchedulingPolicy::NORMAL ||
            config.priority != ThreadPriority::NORMAL) {

            int policy = SCHED_OTHER;
            switch (config.policy) {
                case SchedulingPolicy::FIFO:        policy = SCHED_FIFO; break;
                case SchedulingPolicy::ROUND_ROBIN: policy = SCHED_RR;   break;
                default:                            policy = SCHED_OTHER; break;
            }

            struct sched_param param{};
            param.sched_priority = static_cast<int>(config.priority);
            pthread_setschedparam(handle, policy, &param);
        }

        if (config.cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(config.cpu_affinity, &cpuset);
            pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
        }
    }

    ThreadConfig config_;
    std::thread  thread_;
};

// ============================================================================
// Mutex (std::mutex wrapper)
// ============================================================================

class Mutex {
public:
    Mutex() = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock()           { mutex_.lock(); }
    bool try_lock()       { return mutex_.try_lock(); }
    void unlock()         { mutex_.unlock(); }

private:
    std::mutex mutex_;
};

// ============================================================================
// SharedMutex (std::shared_mutex wrapper)
// ============================================================================

class SharedMutex {
public:
    SharedMutex() = default;
    ~SharedMutex() = default;

    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator=(const SharedMutex&) = delete;

    void lock()              { mutex_.lock(); }
    void lock_shared()       { mutex_.lock_shared(); }
    bool try_lock()          { return mutex_.try_lock(); }
    bool try_lock_shared()   { return mutex_.try_lock_shared(); }
    void unlock()            { mutex_.unlock(); }
    void unlock_shared()     { mutex_.unlock_shared(); }

private:
    std::shared_mutex mutex_;
};

// ============================================================================
// ConditionVariable (std::condition_variable_any)
// ============================================================================

class ConditionVariable {
public:
    ConditionVariable() = default;
    ~ConditionVariable() = default;

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void notify_one() noexcept { cv_.notify_one(); }
    void notify_all() noexcept { cv_.notify_all(); }

    void wait(std::unique_lock<Mutex>& lock) {
        cv_.wait(lock);
    }

    template<typename Predicate>
    void wait(std::unique_lock<Mutex>& lock, Predicate pred) {
        cv_.wait(lock, std::move(pred));
    }

    CvStatus wait_for(std::unique_lock<Mutex>& lock, Duration timeout) {
        auto status = cv_.wait_for(lock, timeout.to_chrono_ns());
        return (status == std::cv_status::timeout) ? CvStatus::TIMEOUT : CvStatus::NO_TIMEOUT;
    }

    template<typename Predicate>
    bool wait_for(std::unique_lock<Mutex>& lock, Duration timeout, Predicate pred) {
        return cv_.wait_for(lock, timeout.to_chrono_ns(), std::move(pred));
    }

private:
    std::condition_variable_any cv_;
};

} // namespace corerat
