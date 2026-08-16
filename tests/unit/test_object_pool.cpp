#include <gtest/gtest.h>

#include <set>

#include "mm/common/ObjectPool.hpp"

namespace mm {
namespace {

struct Item {
    int value = 0;
    bool flag = false;
};

using Pool = ObjectPool<Item, 4>;

TEST(ObjectPool, StartsEmpty) {
    Pool pool;
    EXPECT_EQ(pool.in_use(), 0U);
    EXPECT_EQ(pool.available(), 4U);
    EXPECT_EQ(Pool::capacity(), 4U);
}

TEST(ObjectPool, AcquireReleaseCycle) {
    Pool pool;
    const auto a = pool.acquire();
    ASSERT_NE(a, Pool::kInvalidIndex);
    EXPECT_TRUE(pool.is_live(a));
    EXPECT_EQ(pool.in_use(), 1U);

    pool[a].value = 42;
    ASSERT_NE(pool.get(a), nullptr);
    EXPECT_EQ(pool.get(a)->value, 42);

    EXPECT_TRUE(pool.release(a));
    EXPECT_FALSE(pool.is_live(a));
    EXPECT_EQ(pool.get(a), nullptr) << "a released handle must not resolve";
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(ObjectPool, HandsOutDistinctIndices) {
    Pool pool;
    std::set<Pool::Index> seen;
    for (int i = 0; i < 4; ++i) {
        const auto idx = pool.acquire();
        ASSERT_NE(idx, Pool::kInvalidIndex);
        EXPECT_TRUE(seen.insert(idx).second) << "index " << idx << " handed out twice";
    }
    EXPECT_EQ(seen.size(), 4U);
}

TEST(ObjectPool, ExhaustionIsReportedNotFatal) {
    Pool pool;
    for (int i = 0; i < 4; ++i) {
        ASSERT_NE(pool.acquire(), Pool::kInvalidIndex);
    }
    // Exhaustion is a real operating condition -- the caller stops quoting.
    EXPECT_EQ(pool.acquire(), Pool::kInvalidIndex);
    EXPECT_EQ(pool.available(), 0U);
    EXPECT_DOUBLE_EQ(pool.utilisation(), 1.0);
}

TEST(ObjectPool, AcquireYieldsFreshlyConstructedObject) {
    Pool pool;
    const auto a = pool.acquire();
    pool[a].value = 999;
    pool[a].flag = true;
    ASSERT_TRUE(pool.release(a));

    const auto b = pool.acquire();
    ASSERT_EQ(b, a) << "expected the slot to be recycled";
    EXPECT_EQ(pool[b].value, 0) << "recycled slot leaked the previous order's state";
    EXPECT_FALSE(pool[b].flag);
}

TEST(ObjectPool, DoubleReleaseIsRefused) {
    Pool pool;
    const auto a = pool.acquire();
    ASSERT_TRUE(pool.release(a));
    // A second release would push the same index twice and later hand one
    // order object to two owners.
    EXPECT_FALSE(pool.release(a));
    EXPECT_EQ(pool.invalid_releases(), 1U);
    EXPECT_EQ(pool.available(), 4U);
}

TEST(ObjectPool, OutOfRangeReleaseIsRefused) {
    Pool pool;
    EXPECT_FALSE(pool.release(99));
    EXPECT_FALSE(pool.release(Pool::kInvalidIndex));
    EXPECT_EQ(pool.invalid_releases(), 2U);
}

TEST(ObjectPool, FreeListSurvivesChurn) {
    Pool pool;
    for (int round = 0; round < 1000; ++round) {
        const auto a = pool.acquire();
        const auto b = pool.acquire();
        ASSERT_NE(a, Pool::kInvalidIndex);
        ASSERT_NE(b, Pool::kInvalidIndex);
        ASSERT_NE(a, b);
        ASSERT_TRUE(pool.release(a));
        ASSERT_TRUE(pool.release(b));
    }
    EXPECT_EQ(pool.available(), 4U);
    EXPECT_EQ(pool.acquired_total(), 2000U);
}

TEST(ObjectPool, HighWaterMarkTracksPeakUsage) {
    Pool pool;
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    const auto c = pool.acquire();
    ASSERT_TRUE(pool.release(a));
    ASSERT_TRUE(pool.release(b));
    ASSERT_TRUE(pool.release(c));
    EXPECT_EQ(pool.high_water_mark(), 3U);
}

TEST(ObjectPool, ForEachLiveVisitsOnlyLiveSlots) {
    Pool pool;
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    pool[a].value = 1;
    pool[b].value = 2;
    ASSERT_TRUE(pool.release(a));

    int visits = 0;
    int sum = 0;
    pool.for_each_live([&](Pool::Index, Item& it) {
        ++visits;
        sum += it.value;
    });
    EXPECT_EQ(visits, 1);
    EXPECT_EQ(sum, 2);
}

}  // namespace
}  // namespace mm
