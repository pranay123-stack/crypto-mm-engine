#include <gtest/gtest.h>

#include <set>

#include "mm/exchange/common/ReconnectBackoff.hpp"

namespace mm::exchange {
namespace {

ReconnectBackoff::Config config(std::uint32_t jitter = 25, std::uint32_t max_attempts = 0) {
    ReconnectBackoff::Config c;
    c.initial_delay = millis(200);
    c.max_delay = seconds(30);
    c.jitter_percent = jitter;
    c.max_attempts = max_attempts;
    return c;
}

TEST(ReconnectBackoff, GrowsExponentially) {
    ReconnectBackoff b(config(/*jitter=*/0));
    EXPECT_EQ(b.next_delay(), millis(200));
    EXPECT_EQ(b.next_delay(), millis(400));
    EXPECT_EQ(b.next_delay(), millis(800));
    EXPECT_EQ(b.next_delay(), millis(1'600));
}

TEST(ReconnectBackoff, SaturatesAtTheCap) {
    ReconnectBackoff b(config(/*jitter=*/0));
    for (int i = 0; i < 40; ++i) {
        const Nanos d = b.next_delay();
        EXPECT_LE(d, seconds(30)) << "attempt " << i;
        EXPECT_GT(d, 0);
    }
    // Well past saturation it must sit exactly at the cap, not overflow.
    EXPECT_EQ(b.next_delay(), seconds(30));
}

TEST(ReconnectBackoff, JitterOnlySubtracts) {
    // Never delay longer than the cap promises; an operator reading "max 30s"
    // must be able to rely on it.
    ReconnectBackoff b(config(/*jitter=*/25), /*seed=*/12345);
    for (int i = 0; i < 40; ++i) {
        const Nanos d = b.next_delay();
        EXPECT_LE(d, seconds(30));
        EXPECT_GT(d, 0);
    }
}

TEST(ReconnectBackoff, JitterStaysWithinTheConfiguredFraction) {
    ReconnectBackoff jittered(config(/*jitter=*/25), /*seed=*/7);
    ReconnectBackoff plain(config(/*jitter=*/0));
    for (int i = 0; i < 10; ++i) {
        const Nanos with = jittered.next_delay();
        const Nanos without = plain.next_delay();
        EXPECT_LE(with, without);
        EXPECT_GE(with, without - (without / 4) - 1) << "attempt " << i;
    }
}

TEST(ReconnectBackoff, IsDeterministicForAGivenSeed) {
    // A schedule that cannot be asserted on is a schedule nobody checks.
    ReconnectBackoff a(config(), 42);
    ReconnectBackoff b(config(), 42);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(a.next_delay(), b.next_delay()) << "attempt " << i;
    }
}

TEST(ReconnectBackoff, DifferentSeedsDecorrelateClients) {
    // The point of jitter: two processes recovering from the same venue outage
    // must not retry in lockstep.
    std::set<Nanos> distinct;
    for (std::uint64_t seed = 0; seed < 8; ++seed) {
        ReconnectBackoff b(config(), seed);
        static_cast<void>(b.next_delay());
        static_cast<void>(b.next_delay());
        distinct.insert(b.next_delay());
    }
    EXPECT_GT(distinct.size(), 1U) << "all seeds produced an identical schedule";
}

TEST(ReconnectBackoff, ResetRestartsTheSchedule) {
    ReconnectBackoff b(config(/*jitter=*/0));
    static_cast<void>(b.next_delay());
    static_cast<void>(b.next_delay());
    EXPECT_EQ(b.attempts(), 2U);

    b.reset();
    EXPECT_EQ(b.attempts(), 0U);
    // A long-lived session that blips once must not inherit an old backoff.
    EXPECT_EQ(b.next_delay(), millis(200));
}

TEST(ReconnectBackoff, ExhaustsAfterTheConfiguredAttempts) {
    ReconnectBackoff b(config(/*jitter=*/0, /*max_attempts=*/3));
    EXPECT_FALSE(b.exhausted());
    static_cast<void>(b.next_delay());
    static_cast<void>(b.next_delay());
    EXPECT_FALSE(b.exhausted());
    static_cast<void>(b.next_delay());
    EXPECT_TRUE(b.exhausted()) << "retrying forever hides the real fault";
}

TEST(ReconnectBackoff, UnlimitedByDefault) {
    ReconnectBackoff b(config(/*jitter=*/0, /*max_attempts=*/0));
    for (int i = 0; i < 100; ++i) {
        static_cast<void>(b.next_delay());
    }
    EXPECT_FALSE(b.exhausted());
}

}  // namespace
}  // namespace mm::exchange
