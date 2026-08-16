#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "mm/common/Seqlock.hpp"

namespace mm {
namespace {

struct Snapshot {
    std::uint64_t version = 0;
    std::array<std::uint64_t, 32> fields{};

    [[nodiscard]] bool consistent() const noexcept {
        for (const auto f : fields) {
            if (f != version) {
                return false;
            }
        }
        return true;
    }
};

TEST(Seqlock, StoreLoadRoundTrip) {
    Seqlock<Snapshot> lock;
    Snapshot in;
    in.version = 5;
    in.fields.fill(5);
    lock.store(in);

    Snapshot out;
    ASSERT_TRUE(lock.load(out));
    EXPECT_EQ(out.version, 5U);
    EXPECT_TRUE(out.consistent());
}

TEST(Seqlock, VersionCountsCompletedWrites) {
    Seqlock<Snapshot> lock;
    EXPECT_EQ(lock.version(), 0U);
    lock.store(Snapshot{});
    EXPECT_EQ(lock.version(), 1U);
    lock.store(Snapshot{});
    EXPECT_EQ(lock.version(), 2U);
}

TEST(Seqlock, DefaultLoadBeforeAnyStore) {
    Seqlock<Snapshot> lock;
    Snapshot out;
    out.version = 99;
    ASSERT_TRUE(lock.load(out));
    EXPECT_EQ(out.version, 0U) << "must observe the default-constructed value";
}

// The load-bearing guarantee: readers never observe a half-written snapshot,
// and -- critically -- readers never stall the writer. If they could, a slow
// dashboard would become backpressure on the trading thread.
TEST(Seqlock, ReadersNeverSeeTornSnapshotsAndNeverBlockTheWriter) {
    Seqlock<Snapshot> lock;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> writes{0};
    std::atomic<std::uint64_t> torn_reads{0};
    std::atomic<std::uint64_t> successful_reads{0};

    std::thread writer([&] {
        Snapshot s;
        for (std::uint64_t v = 1; !stop.load(std::memory_order_relaxed); ++v) {
            s.version = v;
            s.fields.fill(v);
            lock.store(s);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            Snapshot out;
            while (!stop.load(std::memory_order_relaxed)) {
                if (lock.load(out)) {
                    successful_reads.fetch_add(1, std::memory_order_relaxed);
                    if (!out.consistent()) {
                        torn_reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Let the threads actually contend rather than trusting a fixed sleep.
    while (writes.load(std::memory_order_relaxed) < 200'000) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto& r : readers) {
        r.join();
    }

    EXPECT_EQ(torn_reads.load(), 0U) << "a reader observed a partially written snapshot";
    EXPECT_GT(successful_reads.load(), 0U);
    EXPECT_GT(writes.load(), 199'999U) << "the writer was throttled by its readers";
}

}  // namespace
}  // namespace mm
