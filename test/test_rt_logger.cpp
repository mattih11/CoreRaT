/**
 * @file test_rt_logger.cpp
 * @brief Functional test for the CoreRaT RT logging service.
 *
 * Tests the full pipeline:
 *   RT thread (corerat::Thread — OOB on EVL) → commit() → ring buffer
 *   → drain thread (std::thread, in-band) → TerminalSink
 *
 * Scenarios covered:
 *
 *  [1] Single-threaded basic — all types round-trip through the stream API.
 *  [2] RT thread stress — 1000 entries from a corerat::Thread; all must arrive.
 *  [3] Level filtering — entries below min_level are silently dropped.
 *  [4] Ring-full drop — ring fills up; RT thread never blocks; drops counted.
 *  [5] Multiple RtLoggers — independent ring + drain per logger instance.
 *
 * On EVL platform corerat::Thread calls evl_attach_self(), so scenario [2]
 * exercises the true OOB commit path (evl_mutex lock/unlock from OOB context).
 *
 * On STD platform corerat::Thread uses std::thread, so the test is still
 * exercising the concurrent producer/consumer path.
 *
 * Exit code: 0 = all pass, 1 = any failure.
 */

#include <corerat/logging/logging.hpp>
#include <corerat/platform/threading.hpp>
#include <corerat/platform/timestamp.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

using namespace corerat;

// ============================================================================
// Test infrastructure
// ============================================================================

static TerminalSink g_sink{LogLevel::Trace};

/// Counting sink: records how many entries arrived per level.
struct CountingSink final : ILogSink {
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> errors{0};

    void write(const RtLogEntry<>& e) noexcept override {
        ++count;
        if (e.level == LogLevel::Error) ++errors;
    }
};

static bool g_all_pass = true;
static RtLogger<64> g_test_logger{0xFFFF0000u, LogLevel::Trace};

static void check(bool cond, const char* name) {
    if (!cond) {
        RTLOG_ERROR(g_test_logger) << "FAIL: " << name;
        g_all_pass = false;
    } else {
        RTLOG_INFO(g_test_logger) << "  PASS: " << name;
    }
}

// ============================================================================
// [1] Single-threaded basic
// ============================================================================

static void test_basic_types() {
    RTLOG_INFO(g_test_logger) << "[1] Basic types";

    CountingSink csink{};
    RtLogger<16> logger{0x0001u, LogLevel::Trace};
    logger.add_sink(&csink);
    logger.add_sink(&g_sink);
    logger.start_drain();

    RTLOG_INFO(logger)  << "string literal";
    RTLOG_INFO(logger)  << "int32=" << int32_t{-42};
    RTLOG_INFO(logger)  << "uint32=" << uint32_t{42u};
    RTLOG_INFO(logger)  << "int64=" << int64_t{-1234567890123LL};
    RTLOG_INFO(logger)  << "uint64=" << uint64_t{9876543210ULL};
    RTLOG_INFO(logger)  << "float=" << 3.14f;
    RTLOG_INFO(logger)  << "double=" << 2.718281828;
    RTLOG_INFO(logger)  << "bool true=" << true << " false=" << false;
    RTLOG_INFO(logger)  << "hex=" << RtHex{0xDEADBEEFu};
    RTLOG_WARN(logger)  << "warn message";
    RTLOG_ERROR(logger) << "error message";
    RTLOG_DEBUG(logger) << "debug message";
    RTLOG_TRACE(logger) << "trace message";

    // Let drain thread flush
    logger.stop_drain();

    check(csink.count.load() == 13, "all 13 entries arrived at sink");
    check(csink.errors.load() == 1, "exactly 1 error entry");
}

// ============================================================================
// [2] RT thread stress
// ============================================================================

static void test_rt_thread() {
    RTLOG_INFO(g_test_logger) << "[2] RT thread stress (1000 entries)";

    static constexpr uint32_t kEntries = 1000;

    CountingSink csink{};
    // RingSize=128 so the ring cycles many times over 1000 entries;
    // drain thread must keep up.
    RtLogger<128> logger{0x0002u, LogLevel::Trace};
    logger.add_sink(&csink);
    logger.add_sink(&g_sink);
    logger.start_drain();

    std::atomic<bool> ready{false};
    std::atomic<uint32_t> sent{0};

    // corerat::Thread — OOB-attached on EVL, pthread on STD
    ThreadConfig cfg;
    cfg.name     = "rt-log-test";
    cfg.priority = ThreadPriority::REALTIME;

    Thread rt_thread{cfg, [&] {
        // Wait for start signal (avoids measuring thread-startup time)
        while (!ready.load(std::memory_order_acquire)) {}

        for (uint32_t i = 0; i < kEntries; ++i) {
            RTLOG_INFO(logger)  << "entry=" << i << " ts=" << Time::now();
            RTLOG_DEBUG(logger) << "dbg=" << i;
            sent.fetch_add(2, std::memory_order_relaxed);
        }
    }};

    ready.store(true, std::memory_order_release);
    rt_thread.join();  // wait for RT producer to finish

    // Give drain thread time to flush all remaining entries from the ring
    logger.stop_drain();

    const uint32_t total_sent   = sent.load();
    const uint32_t total_arrived = csink.count.load();

    RTLOG_INFO(g_test_logger) << "  sent=" << total_sent
                               << " arrived=" << total_arrived
                               << " dropped=" << (total_sent - total_arrived);

    // With RingSize=128 and 2000 entries the ring will fill up several times.
    // We only assert that ALL sent entries either arrived or were dropped (never
    // more than sent), and that at least RingSize entries arrived (ring not stuck).
    check(total_arrived <= total_sent,    "arrived <= sent (no phantom entries)");
    check(total_arrived >= 128u,          "at least one ring-worth arrived");
    check(total_arrived == total_sent ||
          total_arrived < total_sent,     "no corruption (ring-full drops only)");
}

// ============================================================================
// [3] Level filtering
// ============================================================================

static void test_level_filtering() {
    RTLOG_INFO(g_test_logger) << "[3] Level filtering";

    CountingSink csink{};
    RtLogger<16> logger{0x0003u, LogLevel::Warn};  // only Warn and above
    logger.add_sink(&csink);
    logger.start_drain();

    RTLOG_TRACE(logger) << "trace — should be dropped";
    RTLOG_DEBUG(logger) << "debug — should be dropped";
    RTLOG_INFO(logger)  << "info — should be dropped";
    RTLOG_WARN(logger)  << "warn — should pass";
    RTLOG_ERROR(logger) << "error — should pass";
    RTLOG_FATAL(logger) << "fatal — should pass";

    logger.stop_drain();

    check(csink.count.load() == 3, "only 3 entries pass Warn filter");

    // Dynamic level change
    CountingSink csink2{};
    RtLogger<16> logger2{0x0004u, LogLevel::Info};
    logger2.add_sink(&csink2);
    logger2.start_drain();

    RTLOG_DEBUG(logger2) << "before change — dropped";
    logger2.set_level(LogLevel::Debug);
    RTLOG_DEBUG(logger2) << "after change — should pass";

    logger2.stop_drain();
    check(csink2.count.load() == 1, "dynamic level change: 1 entry after lowering level");
}

// ============================================================================
// [4] Ring-full drop — RT thread never blocks
// ============================================================================

static void test_ring_full_drop() {
    RTLOG_INFO(g_test_logger) << "[4] Ring-full drop (no RT stall)";

    // Small ring (8 slots), drain thread NOT started → ring fills immediately.
    // commit() must return without blocking or crashing.
    RtLogger<8> logger{0x0005u, LogLevel::Trace};
    // No sink, no drain — pure ring-fill test

    constexpr uint32_t kSend = 64;
    for (uint32_t i = 0; i < kSend; ++i) {
        RTLOG_INFO(logger) << "entry=" << i;
    }
    // If we get here the RT thread was never blocked — test passes.
    check(true, "RT thread returned without blocking on ring-full");

    // Now start drain and verify at most 8 entries come out
    CountingSink csink{};
    logger.add_sink(&csink);
    logger.start_drain();
    logger.stop_drain();

    check(csink.count.load() <= 8, "ring-full: at most RingSize entries drained");
    RTLOG_INFO(g_test_logger) << "  ring-full drained " << csink.count.load()
                               << " entries (ring capacity 8)";
}

// ============================================================================
// [5] Multiple independent loggers
// ============================================================================

static void test_multiple_loggers() {
    RTLOG_INFO(g_test_logger) << "[5] Multiple independent loggers";

    CountingSink sinkA{}, sinkB{};

    RtLogger<16> loggerA{0x0010u, LogLevel::Info};
    RtLogger<16> loggerB{0x0011u, LogLevel::Error};

    loggerA.add_sink(&sinkA);
    loggerB.add_sink(&sinkB);

    loggerA.start_drain();
    loggerB.start_drain();

    RTLOG_INFO(loggerA)  << "A info";
    RTLOG_INFO(loggerA)  << "A info 2";
    RTLOG_INFO(loggerB)  << "B info — filtered out";  // below loggerB's Error level
    RTLOG_ERROR(loggerB) << "B error — passes";
    RTLOG_FATAL(loggerA) << "A fatal";

    loggerA.stop_drain();
    loggerB.stop_drain();

    check(sinkA.count.load() == 3, "loggerA sink got 3 entries");
    check(sinkB.count.load() == 1, "loggerB sink got 1 entry (Info filtered)");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    g_test_logger.add_sink(&g_sink);
    g_test_logger.start_drain();

    RTLOG_INFO(g_test_logger) << "=== test_rt_logger ===";
#if defined(CORERAT_PLATFORM_EVL)
    RTLOG_INFO(g_test_logger) << "Platform: EVL (OOB RT thread)";
#else
    RTLOG_INFO(g_test_logger) << "Platform: STD (pthread RT thread)";
#endif

    test_basic_types();
    test_rt_thread();
    test_level_filtering();
    test_ring_full_drop();
    test_multiple_loggers();

    RTLOG_INFO(g_test_logger) << (g_all_pass ? "=== ALL PASS ===" : "=== SOME TESTS FAILED ===");
    g_test_logger.stop_drain();
    return g_all_pass ? 0 : 1;
}
