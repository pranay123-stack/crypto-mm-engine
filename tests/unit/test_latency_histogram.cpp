#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "mm/common/LatencyHistogram.hpp"

namespace mm {
namespace {

TEST(LatencyHistogram, EmptyIsWellDefined) {
    const LatencyHistogram h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.p50(), 0);
    EXPECT_EQ(h.max(), 0);
    EXPECT_EQ(h.min(), 0);
    EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
    LatencyHistogram h;
    for (Nanos v = 0; v < 64; ++v) {
        h.record(v);
    }
    // Below the sub-bucket count every value has its own slot.
    for (std::uint64_t v = 0; v < 64; ++v) {
        EXPECT_EQ(LatencyHistogram::slot_of(v), v);
        EXPECT_EQ(LatencyHistogram::slot_lower_bound(static_cast<std::size_t>(v)), v);
        EXPECT_EQ(LatencyHistogram::slot_upper_bound(static_cast<std::size_t>(v)), v);
    }
    EXPECT_EQ(h.count(), 64U);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 63);
}

TEST(LatencyHistogram, SlotBoundsBracketTheValue) {
    std::mt19937_64 rng(20260817);
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t v = rng() % 100'000'000ULL;
        const std::size_t slot = LatencyHistogram::slot_of(v);
        EXPECT_LE(LatencyHistogram::slot_lower_bound(slot), v) << "v=" << v;
        EXPECT_GE(LatencyHistogram::slot_upper_bound(slot), v) << "v=" << v;
    }
}

TEST(LatencyHistogram, RelativeErrorStaysWithinResolution) {
    // 64 sub-buckets per octave => bucket width is under 1.6% of the value.
    std::mt19937_64 rng(7);
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t v = 1000 + rng() % 10'000'000ULL;
        const std::size_t slot = LatencyHistogram::slot_of(v);
        const auto upper = static_cast<double>(LatencyHistogram::slot_upper_bound(slot));
        const auto actual = static_cast<double>(v);
        EXPECT_LE((upper - actual) / actual, 0.016) << "v=" << v;
    }
}

TEST(LatencyHistogram, PercentilesTrackAKnownDistribution) {
    LatencyHistogram h;
    // 1..10000 ns uniformly; p50 ~ 5000, p99 ~ 9900.
    for (Nanos v = 1; v <= 10'000; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 10'000U);
    EXPECT_NEAR(static_cast<double>(h.p50()), 5000.0, 5000.0 * 0.02);
    EXPECT_NEAR(static_cast<double>(h.p90()), 9000.0, 9000.0 * 0.02);
    EXPECT_NEAR(static_cast<double>(h.p99()), 9900.0, 9900.0 * 0.02);
    EXPECT_EQ(h.max(), 10'000);
    EXPECT_EQ(h.min(), 1);
}

TEST(LatencyHistogram, PercentilesAreNeverOptimistic) {
    LatencyHistogram h;
    for (int i = 0; i < 9'980; ++i) {
        h.record(1'000);  // 1 us typical
    }
    for (int i = 0; i < 20; ++i) {
        h.record(50'000'000);  // 0.2% of requests stall for 50 ms
    }
    // The mean reports ~101 us and makes the system look healthy. The tail is
    // where the money is lost, and only the tail shows it.
    EXPECT_LT(h.mean(), 200'000.0);
    EXPECT_NEAR(static_cast<double>(h.p50()), 1'000.0, 1'000.0 * 0.016);
    EXPECT_GE(h.p999(), 50'000'000);
    EXPECT_EQ(h.max(), 50'000'000);
}

TEST(LatencyHistogram, PercentileIsMonotonic) {
    LatencyHistogram h;
    std::mt19937_64 rng(11);
    for (int i = 0; i < 50'000; ++i) {
        h.record(static_cast<Nanos>(rng() % 1'000'000));
    }
    Nanos previous = 0;
    for (const double p : {0.0, 10.0, 25.0, 50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 100.0}) {
        const Nanos v = h.percentile(p);
        EXPECT_GE(v, previous) << "percentile " << p << " went backwards";
        previous = v;
    }
    EXPECT_EQ(previous, h.max()) << "p100 must be exactly the largest observation";
}

TEST(LatencyHistogram, PercentilesStayWithinObservedRange) {
    // The log-linear bucketing overstates by up to one bucket width; a reported
    // percentile must still never exceed a value the system actually produced.
    LatencyHistogram h;
    std::mt19937_64 rng(99);
    for (int i = 0; i < 50'000; ++i) {
        h.record(static_cast<Nanos>(1 + rng() % 987'654));
    }
    for (const double p : {0.0, 1.0, 50.0, 99.0, 99.9, 100.0}) {
        EXPECT_GE(h.percentile(p), h.min()) << "p=" << p;
        EXPECT_LE(h.percentile(p), h.max()) << "p=" << p;
    }
}

TEST(LatencyHistogram, PercentileClampsOutOfRangeInputs) {
    LatencyHistogram h;
    h.record(100);
    h.record(200);
    EXPECT_EQ(h.percentile(-5.0), h.percentile(0.0));
    EXPECT_EQ(h.percentile(500.0), h.percentile(100.0));
}

TEST(LatencyHistogram, NegativeSamplesClampToZero) {
    // A backwards duration means the clock misbehaved; record it as zero rather
    // than corrupting the distribution with a huge unsigned value.
    LatencyHistogram h;
    h.record(-1'000);
    EXPECT_EQ(h.count(), 1U);
    EXPECT_EQ(h.max(), 0);
}

TEST(LatencyHistogram, MergeCombinesDistributions) {
    LatencyHistogram a, b;
    for (Nanos v = 1; v <= 1'000; ++v) {
        a.record(v);
    }
    for (Nanos v = 1'001; v <= 2'000; ++v) {
        b.record(v);
    }
    a.merge(b);
    EXPECT_EQ(a.count(), 2'000U);
    EXPECT_EQ(a.max(), 2'000);
    EXPECT_EQ(a.min(), 1);
    EXPECT_NEAR(static_cast<double>(a.p50()), 1000.0, 1000.0 * 0.02);
}

TEST(LatencyHistogram, ResetClears) {
    LatencyHistogram h;
    h.record(1'234);
    h.reset();
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.max(), 0);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.p99(), 0);
}

TEST(LatencySummary, IsSnapshotSafe) {
    LatencyHistogram h;
    for (Nanos v = 1; v <= 100; ++v) {
        h.record(v);
    }
    const LatencySummary s = LatencySummary::from(h);
    EXPECT_EQ(s.count, 100U);
    EXPECT_EQ(s.max_ns, 100);
    EXPECT_GE(s.p99_ns, s.p50_ns);
    EXPECT_GE(s.p999_ns, s.p99_ns);
}

TEST(ScopedLatency, RecordsScopeDuration) {
    LatencyHistogram h;
    {
        const ScopedLatency timer(h);
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i) {
            sink = sink + i;
        }
        static_cast<void>(sink);
    }
    EXPECT_EQ(h.count(), 1U);
    EXPECT_GT(h.max(), 0);
}

}  // namespace
}  // namespace mm
