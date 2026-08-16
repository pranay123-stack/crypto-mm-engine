#include <gtest/gtest.h>

#include "mm/common/Time.hpp"

namespace mm {
namespace {

TEST(Time, SteadyIsMonotonic) {
    const Nanos a = steady_ns();
    const Nanos b = steady_ns();
    EXPECT_GE(b, a);
    EXPECT_GT(a, 0);
}

TEST(Time, WallIsPlausible) {
    // Sanity: after 2020-01-01 and before 2100.
    const Nanos w = wall_ns();
    EXPECT_GT(w, 1'577'836'800LL * kNanosPerSecond);
    EXPECT_LT(w, 4'102'444'800LL * kNanosPerSecond);
}

TEST(Time, DurationHelpers) {
    EXPECT_EQ(millis(5), 5'000'000);
    EXPECT_EQ(micros(5), 5'000);
    EXPECT_EQ(seconds(2), 2'000'000'000);
    EXPECT_DOUBLE_EQ(to_millis(1'500'000), 1.5);
    EXPECT_DOUBLE_EQ(to_micros(1'500), 1.5);
}

TEST(Time, Iso8601Formatting) {
    // 1786924800 == 2026-08-17T00:00:00Z
    const Nanos epoch = 1'786'924'800LL * kNanosPerSecond + 317 * kNanosPerMilli;
    EXPECT_EQ(format_wall_iso8601(epoch), "2026-08-17T00:00:00.317Z");
    // Sub-second component must be zero-padded, not right-trimmed.
    EXPECT_EQ(format_wall_iso8601(1'786'924'800LL * kNanosPerSecond + 7 * kNanosPerMilli),
              "2026-08-17T00:00:00.007Z");
}

TEST(ManualClock, DrivesTimeDeterministically) {
    ManualClock clock(1'000, 2'000);
    EXPECT_EQ(clock.steady(), 1'000);
    EXPECT_EQ(clock.wall(), 2'000);
    clock.advance(millis(250));
    EXPECT_EQ(clock.steady(), 1'000 + millis(250));
    EXPECT_EQ(clock.wall(), 2'000 + millis(250));
}

TEST(SystemClock, MatchesFreeFunctions) {
    const Clock& c = SystemClock::instance();
    const Nanos before = steady_ns();
    const Nanos via_clock = c.steady();
    const Nanos after = steady_ns();
    EXPECT_GE(via_clock, before);
    EXPECT_LE(via_clock, after);
}

}  // namespace
}  // namespace mm
