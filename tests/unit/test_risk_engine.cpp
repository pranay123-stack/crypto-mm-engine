#include <gtest/gtest.h>

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
using test::replace_order;

class RiskTest : public ::testing::Test {
protected:
    RiskTest() : clock(millis(10'000), 0), instrument(btc_instrument()),
                 engine(default_limits(), clock) {}

    void SetUp() override { ASSERT_TRUE(engine.arm().is_ok()); }

    RiskInput input(const char* position = "0", const char* buy = "0", const char* sell = "0") {
        RiskInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &instrument;
        in.position = position_at(position, clock.steady());
        in.exposure = exposure(buy, sell);
        in.bbo = market();
        in.market_data_age_ns = 0;
        in.system_ready = true;
        static_cast<void>(in.expected_owner.assign(kRiskOwner));
        return in;
    }

    ManualClock clock;
    InstrumentSpec instrument;
    RiskEngine engine;
};

// ===========================================================================
// State machine and arming
// ===========================================================================

TEST_F(RiskTest, DisarmedRefusesNewExposure) {
    RiskEngine fresh(default_limits(), clock);
    EXPECT_EQ(fresh.state(), RiskState::Disarmed);
    const RiskDecision d = fresh.evaluate(new_order(Side::Buy, "60000.00", "1"), input());
    EXPECT_EQ(d.verdict, RiskVerdict::Reject);
    EXPECT_EQ(d.reason, RiskReason::RiskDisarmed);
    EXPECT_FALSE(d.permits_new_exposure());
}

TEST_F(RiskTest, ArmingRefusesWhenEssentialLimitsAreUnset) {
    // Zero is "unset", and unset is not enforced. Arming into that state would
    // leave the engine nominally present and actually absent.
    RiskLimits incomplete;
    SymbolLimits btc;
    btc.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(incomplete.add(btc));
    RiskEngine bad(incomplete, clock);

    const Status s = bad.arm();
    ASSERT_TRUE(s.is_error());
    EXPECT_NE(s.message().find("max_position"), std::string_view::npos);
    EXPECT_EQ(bad.state(), RiskState::Disarmed);
}

TEST_F(RiskTest, ArmingRefusesWithNoConfiguredSymbols) {
    RiskEngine empty(RiskLimits{}, clock);
    EXPECT_TRUE(empty.arm().is_error());
}

TEST_F(RiskTest, StateTransitionTable) {
    EXPECT_TRUE(is_legal_transition(RiskState::Disarmed, RiskState::Armed));
    EXPECT_TRUE(is_legal_transition(RiskState::Armed, RiskState::Warning));
    EXPECT_TRUE(is_legal_transition(RiskState::Warning, RiskState::Armed));
    for (const auto from : {RiskState::Disarmed, RiskState::Armed, RiskState::Warning}) {
        EXPECT_TRUE(is_legal_transition(from, RiskState::Killed));
        EXPECT_TRUE(is_legal_transition(from, RiskState::Faulted));
    }
    // A kill is undone only by going back through Disarmed, so arming
    // re-validates the limits and somebody re-checks why it happened.
    EXPECT_FALSE(is_legal_transition(RiskState::Killed, RiskState::Armed));
    EXPECT_FALSE(is_legal_transition(RiskState::Faulted, RiskState::Armed));
    EXPECT_TRUE(is_legal_transition(RiskState::Killed, RiskState::Disarmed));
}

// ===========================================================================
// Position limits and the exposure model
// ===========================================================================

TEST_F(RiskTest, ApprovesAnOrderWithinEveryLimit) {
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input());
    ASSERT_EQ(d.verdict, RiskVerdict::Approve) << to_string(d.reason);
    EXPECT_EQ(d.approved.quantity.to_string(), "1");
    EXPECT_TRUE(d.permits_new_exposure());
}

TEST_F(RiskTest, WorstCaseIncludesExistingWorkingOrders) {
    // The worked example from the docs: position +8, resting bid 1, limit 10.
    // 8 + 1 + 1 = 10 exactly, so this is permitted.
    const RiskDecision at_limit =
        engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input("8", "1", "0"));
    EXPECT_EQ(at_limit.verdict, RiskVerdict::Approve) << to_string(at_limit.reason);
    EXPECT_EQ(at_limit.limits.peak_absolute_exposure.to_string(), "10");

    // With the resting bid already at 2, the worst case is 8 + 2 = 10 before
    // this order is even considered. There is no room for any quantity, so the
    // only correct answer is refusal -- reduction has nothing to reduce to.
    const RiskDecision no_room =
        engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input("8", "2", "0"));
    EXPECT_EQ(no_room.verdict, RiskVerdict::Reject);
    EXPECT_EQ(no_room.reason, RiskReason::MaxPosition);
    EXPECT_FALSE(no_room.permits_new_exposure());

    // With a little room left, the same overshoot is reduced instead.
    const RiskDecision partial =
        engine.evaluate(new_order(Side::Buy, "60000.00", "2"), input("8", "1", "0"));
    EXPECT_EQ(partial.verdict, RiskVerdict::Reduce) << to_string(partial.reason);
    EXPECT_EQ(partial.approved.quantity.to_string(), "1") << "8 + 1 + 1 = 10, exactly";
}

TEST_F(RiskTest, BidAndAskAreNotNetted) {
    // A resting ask does not offset a resting bid: nothing guarantees the ask
    // fills. Netting them would approve an order that could take the position
    // past its limit.
    const RiskDecision d =
        engine.evaluate(new_order(Side::Buy, "60000.00", "2"), input("9", "0", "9"));
    // worst_case_long = 9 + 0 + 2 = 11 > 10, even though the net of the two
    // sides looks comfortable.
    EXPECT_NE(d.verdict, RiskVerdict::Approve);
    EXPECT_EQ(d.reason, RiskReason::MaxPosition);
}

TEST_F(RiskTest, ShortPositionIsBoundedByTheSameLimit) {
    // worst_case_short = -9 - 2 = -11, absolute 11 > 10.
    const RiskDecision d =
        engine.evaluate(new_order(Side::Sell, "60000.10", "2"), input("-9", "0", "0"));
    EXPECT_NE(d.verdict, RiskVerdict::Approve);
    EXPECT_EQ(d.reason, RiskReason::MaxPosition);
}

TEST_F(RiskTest, ASellDoesNotAffectTheLongWorstCase) {
    // Long 9 with a resting bid of 1: worst_case_long is already 10, at the
    // limit. A SELL touches only worst_case_short, so it passes.
    const RiskDecision d =
        engine.evaluate(new_order(Side::Sell, "60000.10", "1"), input("9", "1", "0"));
    EXPECT_EQ(d.verdict, RiskVerdict::Approve) << to_string(d.reason);
}

TEST_F(RiskTest, SimultaneousBidAndAskAreBothEvaluated) {
    const RiskInput in = input("0", "0", "0");
    const RiskDecision bid = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
    const RiskDecision ask = engine.evaluate(new_order(Side::Sell, "60000.10", "1"), in);
    EXPECT_EQ(bid.verdict, RiskVerdict::Approve);
    EXPECT_EQ(ask.verdict, RiskVerdict::Approve);
}

TEST_F(RiskTest, PositionNotionalLimit) {
    // max_position_notional is 700,000 and mid is ~60,000, so about 11.6 BTC
    // -- above the 10 BTC quantity limit, meaning quantity binds first. Tighten
    // the notional limit so it is the binding constraint.
    RiskLimits limits = default_limits();
    ASSERT_TRUE(Notional::parse("120000", limits.symbols[0].max_position_notional));
    RiskEngine local(limits, clock);
    ASSERT_TRUE(local.arm().is_ok());

    const RiskDecision d = local.evaluate(new_order(Side::Buy, "60000.00", "2"), input("1"));
    // 3 BTC x 60,000 = 180,000 > 120,000
    EXPECT_NE(d.verdict, RiskVerdict::Approve);
    EXPECT_TRUE(d.reason == RiskReason::MaxPositionNotional ||
                d.verdict == RiskVerdict::Reduce);
}

// ===========================================================================
// Order-level limits
// ===========================================================================

TEST_F(RiskTest, MaxOrderQuantityReducesRatherThanRefuses) {
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "5"), input());
    ASSERT_EQ(d.verdict, RiskVerdict::Reduce) << to_string(d.reason);
    EXPECT_EQ(d.reason, RiskReason::MaxOrderQuantity);
    EXPECT_EQ(d.approved.quantity.to_string(), "2") << "reduced to exactly the order limit";
    EXPECT_LT(d.approved.quantity, d.requested.quantity);
}

TEST_F(RiskTest, MaxOrderNotionalBinds) {
    RiskLimits limits = default_limits();
    ASSERT_TRUE(Notional::parse("60000", limits.symbols[0].max_order_notional));
    RiskEngine local(limits, clock);
    ASSERT_TRUE(local.arm().is_ok());

    const RiskDecision d = local.evaluate(new_order(Side::Buy, "60000.00", "2"), input());
    ASSERT_EQ(d.verdict, RiskVerdict::Reduce);
    // 60,000 notional at a price of 60,000 is exactly 1 BTC.
    EXPECT_EQ(d.approved.quantity.to_string(), "1");
}

TEST_F(RiskTest, MaxOpenOrdersRefusesNewButNotReplace) {
    RiskInput in = input();
    in.exposure.open_order_count = 8;  // limit is 8

    const RiskDecision blocked = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
    EXPECT_EQ(blocked.verdict, RiskVerdict::Reject);
    EXPECT_EQ(blocked.reason, RiskReason::MaxOpenOrders);

    // A replace swaps an order rather than adding one, so the count is unchanged.
    const RiskDecision replaced = engine.evaluate(replace_order(Side::Buy, "60000.00", "1"), in);
    EXPECT_EQ(replaced.verdict, RiskVerdict::Approve) << to_string(replaced.reason);
}

TEST_F(RiskTest, SideAndGrossWorkingExposureLimits) {
    // Side limit is 12.
    const RiskDecision side_bound =
        engine.evaluate(new_order(Side::Buy, "60000.00", "2"), input("0", "11", "0"));
    EXPECT_NE(side_bound.verdict, RiskVerdict::Approve);

    // Gross limit is 20.
    const RiskDecision gross_bound =
        engine.evaluate(new_order(Side::Buy, "60000.00", "2"), input("0", "9", "10"));
    EXPECT_NE(gross_bound.verdict, RiskVerdict::Approve);
}

// ===========================================================================
// Price bands
// ===========================================================================

TEST_F(RiskTest, RejectsAPriceOutsideTheBand) {
    // The band is 500 bps around the mid of ~60,000, so about +/- 3,000.
    const RiskDecision far = engine.evaluate(new_order(Side::Buy, "50000.00", "1"), input());
    EXPECT_EQ(far.verdict, RiskVerdict::Reject);
    EXPECT_EQ(far.reason, RiskReason::PriceOutsideBand);
    // A quote far from the market is almost always arithmetic gone wrong, and
    // trading less of it does not make it correct.
    EXPECT_NE(far.verdict, RiskVerdict::Reduce);
}

TEST_F(RiskTest, RejectsNonPositivePriceAndQuantity) {
    OrderAction zero_price = new_order(Side::Buy, "60000.00", "1");
    zero_price.price = Px::zero();
    EXPECT_EQ(engine.evaluate(zero_price, input()).reason, RiskReason::InvalidPrice);

    OrderAction zero_qty = new_order(Side::Buy, "60000.00", "1");
    zero_qty.quantity = Qty::zero();
    EXPECT_EQ(engine.evaluate(zero_qty, input()).reason, RiskReason::InvalidQuantity);

    OrderAction negative = new_order(Side::Buy, "60000.00", "1");
    negative.quantity = Qty::from_raw(-1);
    EXPECT_EQ(engine.evaluate(negative, input()).reason, RiskReason::InvalidQuantity);
}

// ===========================================================================
// Cancellation is always available
// ===========================================================================

TEST_F(RiskTest, CancelIsApprovedInEveryBlockingState) {
    struct Case {
        const char* what;
        std::function<void(RiskEngine&, RiskInput&)> setup;
    };
    const Case cases[] = {
        {"killed", [](RiskEngine& e, RiskInput&) { e.kill(RiskReason::RiskKilled); }},
        {"faulted", [](RiskEngine& e, RiskInput&) { e.fault(RiskReason::ArithmeticOverflow); }},
        {"stale market", [](RiskEngine&, RiskInput& i) { i.market_data_age_ns = seconds(30); }},
        {"system not ready", [](RiskEngine&, RiskInput& i) { i.system_ready = false; }},
        {"no position", [](RiskEngine&, RiskInput& i) { i.position.valid = false; }},
        {"unknown exposure", [](RiskEngine&, RiskInput& i) { i.exposure.determinate = false; }},
        {"no market", [](RiskEngine&, RiskInput& i) { i.bbo = BestBidAsk{}; }},
    };

    for (const auto& c : cases) {
        RiskEngine local(default_limits(), clock);
        ASSERT_TRUE(local.arm().is_ok());
        RiskInput in = input();
        c.setup(local, in);

        // The moment you most need to withdraw is the moment things are least
        // certain. A risk engine that blocks an exit turns a problem into a
        // position.
        const RiskDecision d = local.evaluate(cancel_order(Side::Buy), in);
        EXPECT_EQ(d.verdict, RiskVerdict::Approve) << c.what;
        EXPECT_EQ(d.approved.type, OrderActionType::Cancel) << c.what;

        // And new exposure is refused in the same state.
        const RiskDecision blocked = local.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
        EXPECT_FALSE(blocked.permits_new_exposure()) << c.what;
    }
}

TEST_F(RiskTest, CancelIsRefusedOnlyForAForeignOrder) {
    OrderAction foreign = cancel_order(Side::Buy);
    static_cast<void>(foreign.identity.name.assign("someone_else_v1"));
    const RiskDecision d = engine.evaluate(foreign, input());
    // Cancelling another subsystem's order is not withdrawal, it is interference.
    EXPECT_EQ(d.verdict, RiskVerdict::Reject);
    EXPECT_EQ(d.reason, RiskReason::OwnershipViolation);
}

// ===========================================================================
// Kill switch
// ===========================================================================

TEST_F(RiskTest, KillBlocksNewExposureAndDoesNotSelfRecover) {
    engine.kill(RiskReason::RiskKilled);
    EXPECT_EQ(engine.state(), RiskState::Killed);

    for (int i = 0; i < 5; ++i) {
        const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input());
        EXPECT_EQ(d.reason, RiskReason::RiskKilled);
        EXPECT_FALSE(d.permits_new_exposure());
    }
    // Arming is refused outright; recovery is a deliberate two-step.
    EXPECT_TRUE(engine.arm().is_error());
    EXPECT_EQ(engine.state(), RiskState::Killed);

    ASSERT_TRUE(engine.rearm().is_ok());
    EXPECT_EQ(engine.state(), RiskState::Disarmed);
    ASSERT_TRUE(engine.arm().is_ok());
    EXPECT_EQ(engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input()).verdict,
              RiskVerdict::Approve);
}

TEST_F(RiskTest, RearmIsRefusedWhenNotKilled) {
    EXPECT_TRUE(engine.rearm().is_error());
}

TEST_F(RiskTest, ReduceOnlyPermitsUnwindingButNotAdding) {
    engine.warn(RiskReason::MaxPosition);
    ASSERT_EQ(engine.state(), RiskState::Warning);

    // Long 5: buying adds to the position, selling unwinds it.
    const RiskInput in = input("5");
    const RiskDecision adding = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in);
    EXPECT_EQ(adding.verdict, RiskVerdict::Reject);
    EXPECT_EQ(adding.reason, RiskReason::ReduceOnly);

    const RiskDecision unwinding = engine.evaluate(new_order(Side::Sell, "60000.10", "1"), in);
    EXPECT_EQ(unwinding.verdict, RiskVerdict::Approve) << to_string(unwinding.reason);
}

TEST_F(RiskTest, ReduceOnlyFromFlatRefusesBothSides) {
    engine.warn(RiskReason::MaxPosition);
    const RiskInput in = input("0");
    // From flat, any fill opens a position, so nothing reduces.
    EXPECT_EQ(engine.evaluate(new_order(Side::Buy, "60000.00", "1"), in).reason,
              RiskReason::ReduceOnly);
    EXPECT_EQ(engine.evaluate(new_order(Side::Sell, "60000.10", "1"), in).reason,
              RiskReason::ReduceOnly);
}

// ===========================================================================
// Replace
// ===========================================================================

TEST_F(RiskTest, NonAtomicReplaceCountsTheFullNewQuantityOnTop) {
    // The old order is still live until the cancel confirms, so both can be
    // working at once. Position 8, old bid 1 still counted, new bid 2 => 11.
    RiskInput in = input("8", "1", "0");
    in.atomic_replace = false;
    ASSERT_TRUE(Qty::parse("1", in.replaced_order_remaining));

    const RiskDecision d = engine.evaluate(replace_order(Side::Buy, "60000.00", "2"), in);
    EXPECT_NE(d.verdict, RiskVerdict::Approve);
}

TEST_F(RiskTest, AtomicReplaceSubtractsTheOrderItSupersedes) {
    // With an atomic swap the old order is gone the instant the new one exists,
    // so the delta is new minus old: 8 - 1 + 1 + 2 = 10, exactly at the limit.
    RiskInput in = input("8", "1", "0");
    in.atomic_replace = true;
    ASSERT_TRUE(Qty::parse("1", in.replaced_order_remaining));

    const RiskDecision d = engine.evaluate(replace_order(Side::Buy, "60000.00", "2"), in);
    EXPECT_EQ(d.verdict, RiskVerdict::Approve) << to_string(d.reason);
}

// ===========================================================================
// Rate limits
// ===========================================================================

TEST_F(RiskTest, RateLimitRefusesNewExposureButNeverCancels) {
    RiskLimits limits = default_limits();
    limits.global.max_new_orders_per_second = 5;
    limits.global.burst_capacity = 5;
    RiskEngine local(limits, clock);
    ASSERT_TRUE(local.arm().is_ok());

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(local.evaluate(new_order(Side::Buy, "60000.00", "1"), input()).verdict,
                  RiskVerdict::Approve)
            << "order " << i;
    }
    const RiskDecision throttled =
        local.evaluate(new_order(Side::Buy, "60000.00", "1"), input());
    EXPECT_EQ(throttled.verdict, RiskVerdict::Reject);
    EXPECT_EQ(throttled.reason, RiskReason::RateLimit);

    // A burst must never prevent an exit.
    EXPECT_EQ(local.evaluate(cancel_order(Side::Buy), input()).verdict, RiskVerdict::Approve);

    // The bucket refills.
    clock.advance(seconds(1));
    EXPECT_EQ(local.evaluate(new_order(Side::Buy, "60000.00", "1"), input()).verdict,
              RiskVerdict::Approve);
}

TEST_F(RiskTest, BurstCapacityAbsorbsAClusterThenBounds) {
    RiskLimits limits = default_limits();
    limits.global.max_new_orders_per_second = 2;
    limits.global.burst_capacity = 10;
    RiskEngine local(limits, clock);
    ASSERT_TRUE(local.arm().is_ok());

    // A market-data burst legitimately produces a cluster; the reserve absorbs it.
    int approved = 0;
    for (int i = 0; i < 20; ++i) {
        if (local.evaluate(new_order(Side::Buy, "60000.00", "1"), input()).verdict ==
            RiskVerdict::Approve) {
            ++approved;
        }
    }
    EXPECT_EQ(approved, 10) << "the burst allowance, and not one more";
}

// ===========================================================================
// Reduction
// ===========================================================================

TEST_F(RiskTest, ReductionIsExplicitAndNeverIncreasesExposure) {
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "9"), input("5"));
    ASSERT_EQ(d.verdict, RiskVerdict::Reduce) << to_string(d.reason);
    // Never silent: the decision names the transformation and the limit that
    // forced it.
    EXPECT_NE(d.reason, RiskReason::None);
    EXPECT_LT(d.approved.quantity, d.requested.quantity);
    EXPECT_TRUE(d.approved.quantity.is_positive());
}

TEST_F(RiskTest, ReducedQuantityLandsOnTheLotGrid) {
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "9"), input("5"));
    ASSERT_EQ(d.verdict, RiskVerdict::Reduce);
    EXPECT_TRUE(is_on_step(d.approved.quantity, instrument.lot_size))
        << d.approved.quantity.to_string();
}

TEST_F(RiskTest, ReductionThatWouldBeInvalidIsRejectedInstead) {
    // Room for only a sliver, well under the venue's minimum notional of 5.
    RiskLimits limits = default_limits();
    ASSERT_TRUE(Qty::parse("5.00005", limits.symbols[0].max_position));
    RiskEngine local(limits, clock);
    ASSERT_TRUE(local.arm().is_ok());

    const RiskDecision d = local.evaluate(new_order(Side::Buy, "60000.00", "2"), input("5"));
    // Reducing is not always possible: a quantity below the venue minimum is
    // not a smaller order, it is an invalid one.
    EXPECT_NE(d.verdict, RiskVerdict::Approve);
    if (d.verdict == RiskVerdict::Reduce) {
        EXPECT_GE(notional_of(d.approved.price, d.approved.quantity), instrument.min_notional);
    }
}

TEST_F(RiskTest, NonReducibleFailuresAreNotReduced) {
    // A bad price is not made acceptable by trading less of it.
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "50000.00", "1"), input());
    EXPECT_EQ(d.verdict, RiskVerdict::Reject);
    EXPECT_EQ(d.reason, RiskReason::PriceOutsideBand);
}

// ===========================================================================
// Auditability
// ===========================================================================

TEST_F(RiskTest, DecisionsCarryTheirContext) {
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input("3", "2"));
    ASSERT_EQ(d.verdict, RiskVerdict::Approve);
    EXPECT_EQ(d.state, RiskState::Armed);
    EXPECT_EQ(d.limits.max_position.to_string(), "10");
    EXPECT_EQ(d.limits.peak_absolute_exposure.to_string(), "6") << "3 + 2 + 1";
    EXPECT_EQ(d.limits.order_notional.to_string(), "60000");
    EXPECT_EQ(d.generation, 1U);
    EXPECT_GT(d.decided_ns, 0);
}

TEST_F(RiskTest, RejectedDecisionsCarryANoopApprovedAction) {
    engine.kill(RiskReason::RiskKilled);
    const RiskDecision d = engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input());
    // A caller that forwards `approved` without checking the verdict must still
    // send nothing.
    EXPECT_EQ(d.approved.type, OrderActionType::Noop);
    EXPECT_TRUE(d.approved.quantity.is_zero());
}

TEST_F(RiskTest, MetricsTallyPerCause) {
    engine.kill(RiskReason::RiskKilled);
    static_cast<void>(engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input()));
    static_cast<void>(engine.evaluate(new_order(Side::Buy, "60000.00", "1"), input()));
    static_cast<void>(engine.evaluate(cancel_order(Side::Buy), input()));

    EXPECT_EQ(engine.metrics().count(RiskReason::RiskKilled), 2U);
    EXPECT_EQ(engine.metrics().rejections, 2U);
    EXPECT_EQ(engine.metrics().approvals, 1U);
    EXPECT_EQ(engine.metrics().cancels_permitted_while_blocked, 1U);
}

TEST_F(RiskTest, EveryEnumHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(RiskState::Faulted); ++i) {
        EXPECT_NE(to_string(static_cast<RiskState>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(RiskVerdict::Reject); ++i) {
        EXPECT_NE(to_string(static_cast<RiskVerdict>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(RiskReason::ReductionInvalid); ++i) {
        EXPECT_NE(to_string(static_cast<RiskReason>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::risk
