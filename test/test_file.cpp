/**
 * @file test_file.cpp
 * @brief Functional test for corerat::File — STD and EVL backends.
 *
 * Scenarios:
 *
 *  [1] Open / is_open / close lifecycle
 *  [2] Write to /dev/null (no-data-verify, exercises the write path)
 *  [3] Write to a temp file, read back and verify content
 *  [4] Move semantics — source becomes closed, destination owns the fd
 *  [5] RT thread write — a corerat::Thread writes from an RT context;
 *      exercises oob_write via the proxy on EVL, std::thread on STD.
 *  [6] EVL-only: proxy_fd() is valid after open
 *
 * Exit code: 0 = all pass, 1 = any failure.
 */

#include <corerat/platform/file.hpp>
#include <corerat/platform/threading.hpp>
#include <corerat/platform/timestamp.hpp>
#include <corerat/logging/logging.hpp>

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(CORERAT_PLATFORM_EVL)
#include <evl/evl.h>
#endif

// ============================================================================
// Pass/fail bookkeeping via RtLogger (OOB-safe on EVL)
// ============================================================================

static corerat::TerminalSink g_sink{corerat::LogLevel::Trace};
static corerat::RtLogger<64> g_logger{0x00FF0000u, corerat::LogLevel::Trace};
static bool g_all_pass = true;

static void check(bool cond, const char* name) {
    if (cond) {
        RTLOG_INFO(g_logger) << "  PASS: " << name;
    } else {
        RTLOG_ERROR(g_logger) << "  FAIL: " << name;
        g_all_pass = false;
    }
}

// ============================================================================
// Helpers
// ============================================================================

static const char* tmppath() noexcept {
    static char path[64];
    std::snprintf(path, sizeof(path), "/tmp/corerat_file_test_%d", static_cast<int>(getpid()));
    return path;
}

static void remove_tmp() { ::unlink(tmppath()); }

// ============================================================================
// [1] Lifecycle
// ============================================================================

static void test_lifecycle() {
    RTLOG_INFO(g_logger) << "[1] Lifecycle";

    corerat::File f;
    check(!f.is_open(), "default-constructed: not open");
    check(f.fd() == -1, "default-constructed: fd == -1");

    bool ok = f.open("/dev/null", O_WRONLY);
    check(ok, "open /dev/null succeeds");
    check(f.is_open(), "is_open() true after open");
    check(f.fd() >= 0, "fd() >= 0 after open");

    f.close();
    check(!f.is_open(), "is_open() false after close");
    check(f.fd() == -1, "fd() == -1 after close");

    // Double-close must be harmless
    f.close();
    check(!f.is_open(), "double-close: still not open");
}

// ============================================================================
// [2] Write to /dev/null
// ============================================================================

static void test_write_devnull() {
    RTLOG_INFO(g_logger) << "[2] Write to /dev/null";

    corerat::File f;
    f.open("/dev/null", O_WRONLY);

    static const char kMsg[] = "CoreRaT file test";
    ssize_t n = f.write(kMsg, sizeof(kMsg) - 1);
    check(n == static_cast<ssize_t>(sizeof(kMsg) - 1), "write returns byte count");
}

// ============================================================================
// [3] Write + read-back verification
// ============================================================================

static void test_write_read() {
    RTLOG_INFO(g_logger) << "[3] Write / read-back";

    remove_tmp();

    {
        corerat::File w;
        bool ok = w.open(tmppath(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        check(ok, "open for write succeeds");

        static const char kData[] = "hello corerat file";
        ssize_t n = w.write(kData, sizeof(kData) - 1);
        check(n == static_cast<ssize_t>(sizeof(kData) - 1), "write byte count");
        // w closes here, flushing the proxy buffer (EVL) or the fd (STD)
    }

    // On EVL the proxy worker may still be draining — give it a moment.
    corerat::Time::sleep(corerat::Milliseconds(50));

    {
        corerat::File r;
        bool ok = r.open(tmppath(), O_RDONLY);
        check(ok, "open for read succeeds");

        char buf[64] = {};
        ssize_t n = r.read(buf, sizeof(buf) - 1);
        check(n > 0, "read returns data");
        check(std::strcmp(buf, "hello corerat file") == 0, "read-back content matches");
    }

    remove_tmp();
}

// ============================================================================
// [4] Move semantics
// ============================================================================

static void test_move() {
    RTLOG_INFO(g_logger) << "[4] Move semantics";

    corerat::File src;
    src.open("/dev/null", O_WRONLY);
    check(src.is_open(), "src open before move");

    corerat::File dst = std::move(src);
    check(!src.is_open(), "src closed after move");  // NOLINT(bugprone-use-after-move)
    check(dst.is_open(), "dst open after move");

    corerat::File assigned;
    assigned = std::move(dst);
    check(!dst.is_open(), "dst closed after move-assign");  // NOLINT
    check(assigned.is_open(), "assigned open after move-assign");
}

// ============================================================================
// [5] RT thread write
// ============================================================================

static void test_rt_thread_write() {
    RTLOG_INFO(g_logger) << "[5] RT thread write";

    remove_tmp();

    static constexpr int kWrites = 100;
    std::atomic<int> written{0};

    corerat::ThreadConfig cfg;
    cfg.name = "file-test-rt";
    cfg.policy = corerat::SchedulingPolicy::FIFO;
    cfg.priority = corerat::ThreadPriority::HIGH;

    {
        corerat::File f;
        bool ok = f.open(tmppath(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        check(ok, "open for RT write");

        {
            // Thread destructs (joins) before f closes, ensuring all writes land.
            corerat::Thread t{cfg, [&f, &written]() {
                static const char kLine[] = "x";
                for (int i = 0; i < kWrites; ++i) {
                    ssize_t n = f.write(kLine, 1);
                    if (n == 1) written.fetch_add(1, std::memory_order_relaxed);
                }
            }};
        } // t joins here

    } // f closes here — proxy ring flushed to file

    // Let the EVL proxy worker drain to disk.
    corerat::Time::sleep(corerat::Milliseconds(50));

    check(written.load() == kWrites, "RT thread: all writes returned success");

    struct stat st{};
    ::stat(tmppath(), &st);
    check(st.st_size == kWrites, "RT thread: file size matches write count");

    remove_tmp();
}

// ============================================================================
// [6] EVL-only: proxy_fd validity
// ============================================================================

#if defined(CORERAT_PLATFORM_EVL)
static void test_evl_proxy_fd() {
    RTLOG_INFO(g_logger) << "[6] EVL proxy_fd validity";

    corerat::File f;
    f.open("/dev/null", O_WRONLY);
    check(f.proxy_fd() >= 0, "proxy_fd() >= 0 after open");

    f.close();
    check(f.proxy_fd() == -1, "proxy_fd() == -1 after close");
}
#endif

// ============================================================================
// main
// ============================================================================

int main() {
#if defined(CORERAT_PLATFORM_EVL)
    // Attach main so evl_is_inband() is reliable and evl_usleep() works in-band.
    evl_attach_self("corerat-test-main");
#endif

    g_logger.add_sink(&g_sink);
    g_logger.start_drain();

    RTLOG_INFO(g_logger) << "=== corerat::File test ===";

    test_lifecycle();
    test_write_devnull();
    test_write_read();
    test_move();
    test_rt_thread_write();

#if defined(CORERAT_PLATFORM_EVL)
    test_evl_proxy_fd();
#endif

    RTLOG_INFO(g_logger) << (g_all_pass ? "RESULT: PASS" : "RESULT: FAIL");
    g_logger.stop_drain();
    return g_all_pass ? 0 : 1;
}
