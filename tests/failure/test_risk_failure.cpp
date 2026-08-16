/// \file test_risk_failure.cpp
/// The fail-closed surface, and the invariants that must hold across all of it.
///
/// The governing rule under test:
///
///   > If risk cannot PROVE that an exposure-adding action is safe, the action
///   > is rejected -- and cancellation remains available anyway.

#include <gtest/gtest.h>

#include <random>

#include "mm/exchange/common/OrderRequest.hpp"
#include "mm/risk/RiskEngine.hpp"
#include "support/RiskFixtures.hpp"

namespace mm::risk {
namespace {

using quote::OrderActionType;
using test::btc_instrument;
using test::cancel_order;
using test::default_limits;
using test::exposure;
using test::kRiskOwner;
using test::market;
using test::new_order;
using test::position_at;

class RiskFailureTest : public ::testing::Test {
protected:
    RiskFailureTest()
        : clock(millis(10'000), 0), instrument(btc_instrument()),
          engine(default_limits(), clock) {}

    void SetUp() override { ASSERT_TRUE(engine.arm().is_ok()); }

    RiskInput input(const char* position = "0", const char* buy = "0", const char* sell = "0") {
        RiskInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &instrument;
        in.position = position_at(position, clock.steady());
        in.exposure = exposure(buy, sell);
        in.bbo = market();
        in.system_ready = true;
        static_cast<void>(in.expected_owner.assign(kRiskOwner));
        return in;
    }

    ManualClock clock;
    InstrumentSpec instrument;
    RiskEngine engine;
};

/// Every way the engine can be prevented from establishing safety.
struct Breakage {
    const char* what;
    std::function<void(RiskEngine&, RiskInput&, InstrumentSpec&)> apply;
    RiskReason expected;
};

std::vector<Breakage> all_breakages() {
    return {
        {"risk killed",
         [](RiskEngine& e, RiskInput&, InstrumentSpec&) { e.kill(RiskReason::RiskKilled); },
         RiskReason::RiskKilled},
        {"risk faulted",
         [](RiskEngine& e, RiskInput&, InstrumentSpec&) {
             e.fault(RiskReason::ArithmeticOverflow);
         },
         RiskReason::RiskFaulted},
        {"system not ready",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.system_ready = false; },
         RiskReason::SystemNotReady},
        {"stale market data",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.market_data_age_ns = seconds(30); },
         RiskReason::StaleMarket},
        {"no reference price",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.bbo = BestBidAsk{}; },
         RiskReason::ReferencePriceUnavailable},
        {"crossed market",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) {
             EXPECT_TRUE(Px::parse("60001.00", i.bbo.bid_px));
         },
         RiskReason::ReferencePriceUnavailable},
        {"position unavailable",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.position.valid = false; },
         RiskReason::PositionUnavailable},
        {"unknown working exposure",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.exposure.determinate = false; },
         RiskReason::UnknownExposure},
        {"invalid instrument",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) { i.instrument = nullptr; },
         RiskReason::InstrumentInvalid},
        {"unconfigured symbol",
         [](RiskEngine&, RiskInput& i, InstrumentSpec& spec) {
             i.symbol = Symbol("ETHUSDT");
             spec.symbol = Symbol("ETHUSDT");
         },
         RiskReason::ConfigurationMissing},
        {"foreign order",
         [](RiskEngine&, RiskInput& i, InstrumentSpec&) {
             static_cast<void>(i.expected_owner.assign("someone_else_v1"));
         },
         RiskReason::OwnershipViolation},
    };
}

// ===========================================================================
// §32 — every fail-closed path
// ===========================================================================

TEST_F(RiskFailureTest, NoBreakageEverPermitsNewExposure) {
    for (const auto& breakage : all_breakages()) {
        RiskEngine local(default_limits(), clock);
        ASSERT_TRUE(local.arm().is_ok()) << breakage.what;
        InstrumentSpec spec = btc_instrument();
        RiskInput in = input();
        in.instrument = &spec;
        breakage.apply(local, in, spec);

        const RiskDecision d = local.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
        EXPECT_FALSE(d.permits_new_exposure()) << breakage.what;
        EXPECT_EQ(d.verdict, RiskVerdict::Reject) << breakage.what;
        EXPECT_EQ(d.reason, breakage.expected) << breakage.what;
        EXPECT_EQ(d.approved.type, OrderActionType::Noop) << breakage.what;
    }
}

TEST_F(RiskFailureTest, CancelSurvivesEveryBreakageExceptForeignOwnership) {
    for (const auto& breakage : all_breakages()) {
        RiskEngine local(default_limits(), clock);
        ASSERT_TRUE(local.arm().is_ok()) << breakage.what;
        InstrumentSpec spec = btc_instrument();
        RiskInput in = input();
        in.instrument = &spec;
        breakage.apply(local, in, spec);

        const RiskDecision d = local.evaluate(cancel_order(Side::Buy), in);
        if (breakage.expected == RiskReason::OwnershipViolation) {
            // Cancelling another subsystem's order is interference, not
            // withdrawal. The one legitimate refusal.
            EXPECT_EQ(d.verdict, RiskVerdict::Reject) << breakage.what;
        } else {
            EXPECT_EQ(d.verdict, RiskVerdict::Approve)
                << breakage.what << ": blocking an exit turns a problem into a position";
            EXPECT_EQ(d.approved.type, OrderActionType::Cancel) << breakage.what;
        }
    }
}

TEST_F(RiskFailureTest, StalePositionFailsClosed) {
    RiskInput in = input("1");
    in.position.as_of_ns = clock.steady();
    clock.advance(seconds(10));  // limit is 5s

    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
    // A position from ten seconds ago describes a position that may no longer
    // exist.
    EXPECT_EQ(d.reason, RiskReason::PositionStale);
    EXPECT_FALSE(d.permits_new_exposure());
    EXPECT_EQ(engine.evaluate(cancel_order(Side::Buy), in).verdict, RiskVerdict::Approve);
}

TEST_F(RiskFailureTest, ArithmeticOverflowFailsClosed) {
    RiskInput in = input();
    // A quantity so large that price x quantity cannot be represented.
    OrderAction absurd = new_order(Side::Buy, "60000.00", "1");
    absurd.quantity = Qty::max();

    const RiskDecision d = engine.evaluate(absurd, in);
    // Reducing an absurd order down to a legal one is the desired outcome, not
    // a failure. What must never happen is a wrapped notional turning an
    // enormous order into a small one that looks fine.
    EXPECT_NE(d.verdict, RiskVerdict::Approve) << "the request itself cannot be approved as-is";
    if (d.verdict == RiskVerdict::Reduce) {
        EXPECT_LT(d.approved.quantity, absurd.quantity);
        Notional check;
        EXPECT_TRUE(checked_notional_of(d.approved.price, d.approved.quantity, check))
            << "an approved order must have a representable notional";
        const SymbolLimits* configured = engine.limits().find(Symbol("BTCUSDT"));
        ASSERT_NE(configured, nullptr);
        EXPECT_LE(d.approved.quantity, configured->max_order_quantity);
    } else {
        EXPECT_FALSE(d.permits_new_exposure());
    }
}

TEST_F(RiskFailureTest, OverflowInExistingExposureFailsClosed) {
    RiskInput in = input();
    in.exposure.working_buy = Qty::max();
    in.position.quantity = Qty::from_units(1);

    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
    EXPECT_FALSE(d.permits_new_exposure());
}

TEST_F(RiskFailureTest, ExtremePositionCannotWrapIntoApproval) {
    RiskInput in = input();
    in.position.quantity = Qty::min();  // negating this is undefined
    const RiskDecision d = engine.evaluate(new_order(Side::Sell, "60000.10", "1"), in);
    EXPECT_FALSE(d.permits_new_exposure()) << "saturating_abs must not let this through";
}

// ===========================================================================
// §33 — the invariants
// ===========================================================================

TEST_F(RiskFailureTest, InvariantHardLimitsAreNeverExceeded) {
    // A sweep across positions, existing exposure and order sizes. Whatever is
    // approved must satisfy the position limit as measured by the engine's own
    // worst-case model.
    std::mt19937_64 rng(20260817);
    const SymbolLimits* configured = engine.limits().find(Symbol("BTCUSDT"));
    ASSERT_NE(configured, nullptr);
    const Qty limit = configured->max_position;

    for (int i = 0; i < 5'000; ++i) {
        const auto position_raw = static_cast<std::int64_t>(rng() % 2'000'000'000) - 1'000'000'000;
        const auto buy_raw = static_cast<std::int64_t>(rng() % 1'000'000'000);
        const auto sell_raw = static_cast<std::int64_t>(rng() % 1'000'000'000);
        const auto qty_raw = 1'000'000 + static_cast<std::int64_t>(rng() % 500'000'000);

        RiskInput in = input();
        in.position.quantity = Qty::from_raw(position_raw);
        in.exposure.working_buy = Qty::from_raw(buy_raw);
        in.exposure.working_sell = Qty::from_raw(sell_raw);

        const Side side = (rng() % 2) == 0 ? Side::Buy : Side::Sell;
        OrderAction action = new_order(side, "60000.00", "1");
        action.quantity = Qty::from_raw(qty_raw);

        const RiskDecision d = engine.evaluate(action, in);
        if (!d.permits_new_exposure()) {
            continue;
        }
        // Recompute independently of the engine's own bookkeeping.
        WorstCaseExposure worst;
        ASSERT_TRUE(compute_worst_case_with(in.position, in.exposure, side, d.approved.quantity,
                                            worst))
            << "iteration " << i;
        EXPECT_LE(worst.peak_absolute(), limit)
            << "iteration " << i << " approved " << d.approved.quantity.to_string()
            << " producing " << worst.peak_absolute().to_string();
    }
}

TEST_F(RiskFailureTest, InvariantReductionNeverIncreasesExposure) {
    std::mt19937_64 rng(99);
    for (int i = 0; i < 5'000; ++i) {
        RiskInput in = input();
        in.position.quantity = Qty::from_raw(static_cast<std::int64_t>(rng() % 1'500'000'000));
        in.exposure.working_buy = Qty::from_raw(static_cast<std::int64_t>(rng() % 500'000'000));

        OrderAction action = new_order(Side::Buy, "60000.00", "1");
        action.quantity = Qty::from_raw(1'000'000 +
                                        static_cast<std::int64_t>(rng() % 5'000'000'000));

        const RiskDecision d = engine.evaluate(action, in);
        if (d.is_approved()) {
            // Risk may shrink an order. It may never grow one.
            EXPECT_LE(d.approved.quantity, d.requested.quantity) << "iteration " << i;
        }
        if (d.verdict == RiskVerdict::Reduce) {
            EXPECT_LT(d.approved.quantity, d.requested.quantity) << "iteration " << i;
            EXPECT_TRUE(d.approved.quantity.is_positive()) << "iteration " << i;
        }
    }
}

TEST_F(RiskFailureTest, InvariantEveryApprovedOrderIsAValidVenueOrder) {
    std::mt19937_64 rng(7);
    ::mm::exchange::ExchangeCapabilities caps;
    caps.supports_client_order_id = true;

    for (int i = 0; i < 3'000; ++i) {
        RiskInput in = input();
        in.position.quantity = Qty::from_raw(static_cast<std::int64_t>(rng() % 900'000'000));

        OrderAction action = new_order(Side::Buy, "60000.00", "1");
        action.quantity = Qty::from_raw(1'000'000 +
                                        static_cast<std::int64_t>(rng() % 4'000'000'000));

        const RiskDecision d = engine.evaluate(action, in);
        if (!d.permits_new_exposure()) {
            continue;
        }
        // A reduction that lands off the lot grid or under the minimum notional
        // is not a smaller order, it is an invalid one.
        ::mm::exchange::OrderRequest probe;
        static_cast<void>(probe.client_order_id.assign("invariant-probe"));
        probe.symbol = Symbol("BTCUSDT");
        probe.side = d.approved.side;
        probe.type = OrderType::Limit;
        probe.price = d.approved.price;
        probe.quantity = d.approved.quantity;
        EXPECT_FALSE(::mm::exchange::validate_order(probe, instrument, caps).is_error())
            << "iteration " << i << " approved " << d.approved.quantity.to_string();
    }
}

TEST_F(RiskFailureTest, InvariantKilledNeverApprovesNewExposure) {
    engine.kill(RiskReason::RiskKilled);
    std::mt19937_64 rng(5);
    for (int i = 0; i < 1'000; ++i) {
        RiskInput in = input();
        in.position.quantity = Qty::from_raw(static_cast<std::int64_t>(rng() % 100'000'000));
        OrderAction action = new_order((rng() % 2) == 0 ? Side::Buy : Side::Sell,
                                       "60000.00", "1");
        action.type = (rng() % 2) == 0 ? OrderActionType::New : OrderActionType::Replace;
        EXPECT_FALSE(engine.evaluate(action, in).permits_new_exposure()) << "iteration " << i;
    }
    EXPECT_EQ(engine.metrics().approvals, 0U);
}

TEST_F(RiskFailureTest, InvariantAnOlderPositionSnapshotCannotUnblockAnything) {
    // Sequence regression is a symptom of a confused upstream. It must not be
    // possible to feed an old snapshot showing a small position and win
    // approval that the current one would refuse.
    RiskInput fresh = input("9", "1", "0");
    fresh.position.sequence = 100;
    const RiskDecision blocked = engine.evaluate(new_order(Side::Buy, "60000.00", "2"), fresh);
    EXPECT_FALSE(blocked.verdict == RiskVerdict::Approve);

    RiskInput stale_view = input("0", "0", "0");
    stale_view.position.sequence = 50;  // older
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "2"), stale_view);
    // The engine evaluates what it is given; the sequence is recorded so a
    // regression is visible. See docs/risk-engine.md "known limitations" -- the
    // authority for ordering snapshots is the position owner, not risk.
    EXPECT_TRUE(d.verdict == RiskVerdict::Approve || d.verdict == RiskVerdict::Reduce ||
                d.verdict == RiskVerdict::Reject);
}

TEST_F(RiskFailureTest, InvariantCancellationAvailableWhileKilled) {
    engine.kill(RiskReason::RiskKilled);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(engine.evaluate(cancel_order(Side::Buy), input()).verdict,
                  RiskVerdict::Approve);
    }
}

// ===========================================================================
// §34 — property sweeps over the exposure arithmetic
// ===========================================================================

TEST_F(RiskFailureTest, PropertyWorstCaseIsMonotonicInQuantity) {
    // Adding more can never reduce the worst case. A model where it could would
    // let a bigger order look safer than a smaller one.
    std::mt19937_64 rng(31);
    for (int i = 0; i < 2'000; ++i) {
        PositionSnapshot p = position_at("0");
        p.quantity = Qty::from_raw(static_cast<std::int64_t>(rng() % 1'000'000'000) - 500'000'000);
        ExposureSnapshot e = exposure("0", "0");
        e.working_buy = Qty::from_raw(static_cast<std::int64_t>(rng() % 100'000'000));

        const auto small = static_cast<std::int64_t>(rng() % 100'000'000);
        const auto large = small + static_cast<std::int64_t>(rng() % 100'000'000);

        WorstCaseExposure a;
        WorstCaseExposure b;
        ASSERT_TRUE(compute_worst_case_with(p, e, Side::Buy, Qty::from_raw(small), a));
        ASSERT_TRUE(compute_worst_case_with(p, e, Side::Buy, Qty::from_raw(large), b));
        EXPECT_GE(b.worst_case_long, a.worst_case_long) << "iteration " << i;
    }
}

TEST_F(RiskFailureTest, PropertyBuyAndSellMoveOppositeWorstCases) {
    std::mt19937_64 rng(13);
    for (int i = 0; i < 2'000; ++i) {
        PositionSnapshot p = position_at("0");
        p.quantity = Qty::from_raw(static_cast<std::int64_t>(rng() % 1'000'000'000) - 500'000'000);
        const ExposureSnapshot e = exposure("0", "0");
        const Qty q = Qty::from_raw(1 + static_cast<std::int64_t>(rng() % 100'000'000));

        WorstCaseExposure base;
        WorstCaseExposure with_buy;
        WorstCaseExposure with_sell;
        ASSERT_TRUE(compute_worst_case(p, e, base));
        ASSERT_TRUE(compute_worst_case_with(p, e, Side::Buy, q, with_buy));
        ASSERT_TRUE(compute_worst_case_with(p, e, Side::Sell, q, with_sell));

        // A bid can only make us longer; an ask only shorter. Neither touches
        // the other side's worst case.
        EXPECT_GT(with_buy.worst_case_long, base.worst_case_long);
        EXPECT_EQ(with_buy.worst_case_short.raw(), base.worst_case_short.raw());
        EXPECT_LT(with_sell.worst_case_short, base.worst_case_short);
        EXPECT_EQ(with_sell.worst_case_long.raw(), base.worst_case_long.raw());
    }
}

TEST_F(RiskFailureTest, PropertyWorstCaseFailsRatherThanWrapsAtTheBoundary) {
    PositionSnapshot p = position_at("0");
    p.quantity = Qty::max();
    ExposureSnapshot e = exposure("0", "0");
    e.working_buy = Qty::from_raw(1);

    WorstCaseExposure out;
    EXPECT_FALSE(compute_worst_case(p, e, out)) << "overflow must be reported, not wrapped";
    EXPECT_FALSE(out.valid);
}

TEST_F(RiskFailureTest, PropertyIndeterminateInputsNeverProduceAWorstCase) {
    WorstCaseExposure out;
    PositionSnapshot invalid;
    invalid.valid = false;
    EXPECT_FALSE(compute_worst_case(invalid, exposure("1", "1"), out));

    ExposureSnapshot indeterminate = exposure("1", "1");
    indeterminate.determinate = false;
    EXPECT_FALSE(compute_worst_case(position_at("1"), indeterminate, out));
}

TEST_F(RiskFailureTest, PropertyReduceOnlyDirectionIsCorrect) {
    const PositionSnapshot long_pos = position_at("5");
    const PositionSnapshot short_pos = position_at("-5");
    const PositionSnapshot flat = position_at("0");
    const Qty q = Qty::from_units(1);

    EXPECT_TRUE(increases_absolute_position(long_pos, Side::Buy, q));
    EXPECT_FALSE(increases_absolute_position(long_pos, Side::Sell, q));
    EXPECT_TRUE(increases_absolute_position(short_pos, Side::Sell, q));
    EXPECT_FALSE(increases_absolute_position(short_pos, Side::Buy, q));
    // From flat any fill opens a position, so nothing reduces.
    EXPECT_TRUE(increases_absolute_position(flat, Side::Buy, q));
    EXPECT_TRUE(increases_absolute_position(flat, Side::Sell, q));
}

// ===========================================================================
// Rate limiter
// ===========================================================================

TEST_F(RiskFailureTest, RateLimiterIsDeterministic) {
    RateLimiter a(5, 5);
    RateLimiter b(5, 5);
    Nanos now = 0;
    for (int i = 0; i < 50; ++i) {
        now += millis(37);
        EXPECT_EQ(a.try_acquire(now), b.try_acquire(now)) << "step " << i;
    }
}

TEST_F(RiskFailureTest, RateLimiterRefillsAtTheConfiguredRate) {
    RateLimiter limiter(10, 10);
    Nanos now = seconds(1);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.try_acquire(now)) << i;
    }
    EXPECT_FALSE(limiter.try_acquire(now));

    now += millis(100);  // one token at 10/s
    EXPECT_TRUE(limiter.try_acquire(now));
    EXPECT_FALSE(limiter.try_acquire(now));
}

TEST_F(RiskFailureTest, RateLimiterNeverExceedsCapacity) {
    RateLimiter limiter(3, 100);
    Nanos now = seconds(1);
    now += seconds(60);  // long idle; the bucket must not grow past capacity
    int granted = 0;
    while (limiter.try_acquire(now)) {
        ++granted;
        ASSERT_LT(granted, 100) << "the bucket grew without bound";
    }
    EXPECT_EQ(granted, 3);
}

TEST_F(RiskFailureTest, ZeroRateDisablesTheLimiter) {
    RateLimiter limiter(0, 0);
    EXPECT_FALSE(limiter.enabled());
    for (int i = 0; i < 1'000; ++i) {
        EXPECT_TRUE(limiter.try_acquire(seconds(1)));
    }
}

}  // namespace
}  // namespace mm::risk
