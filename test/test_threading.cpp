#include <corerat/platform/threading.hpp>

#include <atomic>
#include <cassert>

int main() {
    corerat::SharedMutex mutex;
    std::atomic<bool> read_locked{false};
    std::atomic<bool> write_locked{false};

    corerat::Thread reader;
    reader.start([&] {
        corerat::SharedLock lock(mutex);
        read_locked.store(true, std::memory_order_release);
    });
    reader.join();
    assert(read_locked.load(std::memory_order_acquire));

    corerat::Thread writer;
    writer.start([&] {
        corerat::UniqueLockShared lock(mutex);
        write_locked.store(true, std::memory_order_release);
    });
    writer.join();
    assert(write_locked.load(std::memory_order_acquire));
}