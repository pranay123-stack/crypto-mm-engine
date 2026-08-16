#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "mm/common/SpscRing.hpp"

namespace mm {
namespace {

struct Payload {
    std::uint64_t seq = 0;
    std::uint64_t checksum = 0;
};

TEST(SpscRing, PushPopSingleThreaded) {
    SpscRing<int, 4> ring;
    int out = 0;
    EXPECT_FALSE(ring.try_pop(out));

    EXPECT_TRUE(ring.try_push(1));
    EXPECT_TRUE(ring.try_push(2));
    EXPECT_EQ(ring.size_approx(), 2U);

    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, UsesFullCapacity) {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.try_push(i)) << "slot " << i;
    }
    EXPECT_FALSE(ring.try_push(99)) << "must refuse rather than overwrite";
    EXPECT_EQ(ring.size_approx(), 4U);

    int out = 0;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SpscRing, NeverBlocksOnFull) {
    // The overflow policy belongs to the caller; the ring only reports it.
    SpscRing<int, 2> ring;
    EXPECT_TRUE(ring.try_push(1));
    EXPECT_TRUE(ring.try_push(2));
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(ring.try_push(i));
    }
    EXPECT_EQ(ring.size_approx(), 2U);
}

TEST(SpscRing, WrapsAroundRepeatedly) {
    SpscRing<int, 4> ring;
    int out = 0;
    for (int round = 0; round < 1000; ++round) {
        ASSERT_TRUE(ring.try_push(round));
        ASSERT_TRUE(ring.try_pop(out));
        ASSERT_EQ(out, round);
    }
}

TEST(SpscRing, EmplaceWithAvoidsTemporary) {
    SpscRing<Payload, 8> ring;
    ASSERT_TRUE(ring.emplace_with([](Payload& p) {
        p.seq = 7;
        p.checksum = 49;
    }));
    Payload out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.seq, 7U);
    EXPECT_EQ(out.checksum, 49U);
}

TEST(SpscRing, PeekDoesNotConsume) {
    SpscRing<int, 4> ring;
    EXPECT_EQ(ring.peek(), nullptr);
    ASSERT_TRUE(ring.try_push(42));
    int* front = ring.peek();
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(*front, 42);
    EXPECT_EQ(ring.size_approx(), 1U);
    ring.pop_peeked();
    EXPECT_EQ(ring.size_approx(), 0U);
}

TEST(SpscRing, HighWaterMarkTracksPeak) {
    SpscRing<int, 8> ring;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        ring.observe_high_water();
    }
    int out = 0;
    while (ring.try_pop(out)) {
    }
    EXPECT_EQ(ring.high_water_mark(), 5U);
}

// The property that actually matters: under genuine concurrency, every element
// arrives exactly once and in order, with no torn payloads.
TEST(SpscRing, ConcurrentTransferPreservesOrderAndIntegrity) {
    constexpr std::uint64_t kMessages = 200'000;
    SpscRing<Payload, 1024> ring;
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> pushes_rejected{0};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kMessages; ++i) {
            Payload p{i, i * 2654435761ULL};
            while (!ring.try_push(p)) {
                pushes_rejected.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t received = 0;
    std::uint64_t torn = 0;
    std::uint64_t out_of_order = 0;

    Payload got;
    while (received < kMessages) {
        if (ring.try_pop(got)) {
            if (got.seq != received) {
                ++out_of_order;
            }
            if (got.checksum != got.seq * 2654435761ULL) {
                ++torn;
            }
            ++received;
        } else if (producer_done.load(std::memory_order_acquire) && ring.size_approx() == 0) {
            break;
        }
    }

    producer.join();
    EXPECT_EQ(received, kMessages);
    EXPECT_EQ(out_of_order, 0U) << "SPSC ordering violated";
    EXPECT_EQ(torn, 0U) << "payload observed mid-write";
}

}  // namespace
}  // namespace mm
