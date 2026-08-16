/// \file test_quote_manager_failure.cpp
/// Every way an evaluation can go wrong, and the assertion that none of them
/// produces an invalid order action.

#include <gtest/gtest.h>

#include "mm/quote/QuoteManager.hpp"
#include "support/QuoteFixtures.hpp"

namespace mm::quote {
namespace {

using strategy::QuoteAction;
using test::btc_spec;
using test::caps_with_replace;
using test::kOwner;
using test::pull_intent;
using test::quoting_intent;
using test::resting;

class QuoteFailureTest : public ::testing::Test {
protected:
    QuoteFailureTest()
        : clock(millis(1'000), 0),
          spec(btc_spec()),
          caps(caps_with_replace(true)),
          manager(StrategyName(kOwner), QuoteManagerConfig{}, clock) {}

    QuoteManagerInput input() {
        QuoteManagerInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.capabilities = &caps;
        in.system_ready = true;
        return in;
    }

    /// The invariant every case below must satisfy.
    static void expect_no_order_creating_action(const QuoteManagerResult& r) {
        for (std::uint8_t i = 0; i < r.action_count; ++i) {
            EXPECT_FALSE(r.actions[i].adds_exposure())
                << "an invalid state produced a " << to_string(r.actions[i].type);
        }
    }

    ManualClock clock;
    InstrumentSpec spec;
    exchange::ExchangeCapabilities caps;
    QuoteManager manager;
};

// ===========================================================================
// Invalid quotes (§25) — none may become an order action
// ===========================================================================

TEST_F(QuoteFailureTest, InvalidQuotesAreBlockedAndReported) {
    struct Case {
        const char* what;
        std::function<void(QuoteIntent&)> mutate;
        RejectReason expected;
    };
    const Case cases[] = {
        {"zero price", [](QuoteIntent& i) { i.bid_price = Px::zero(); },
         RejectReason::PriceOutOfBounds},
        {"zero quantity", [](QuoteIntent& i) { i.bid_quantity = Qty::zero(); },
         RejectReason::QuantityOutOfBounds},
        {"off-tick price",
         [](QuoteIntent& i) { EXPECT_TRUE(Px::parse("60000.005", i.bid_price)); },
         RejectReason::TickSizeViolation},
        {"off-lot quantity",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("0.000015", i.bid_quantity)); },
         RejectReason::LotSizeViolation},
        {"below min notional",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("0.00001", i.bid_quantity)); },
         RejectReason::NotionalTooSmall},
        {"above venue max quantity",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("2000", i.bid_quantity)); },
         RejectReason::QuantityOutOfBounds},
    };

    for (const auto& c : cases) {
        QuoteManager local(StrategyName(kOwner), QuoteManagerConfig{}, clock);
        QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
        intent.quote_ask = false;
        c.mutate(intent);

        const QuoteManagerResult r = local.evaluate(intent, input());
        EXPECT_EQ(r.bid.outcome, SlotOutcome::BlockedInvalid) << c.what;
        EXPECT_EQ(r.bid.invalid_reason, c.expected) << c.what;
        EXPECT_FALSE(r.has_actions()) << c.what;
        expect_no_order_creating_action(r);
        EXPECT_EQ(local.metrics().invalid_quotes_blocked, 1U) << c.what;
    }
}

TEST_F(QuoteFailureTest, AnInvalidSideDoesNotBlockTheValidOne) {
    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    ASSERT_TRUE(Px::parse("60000.005", intent.bid_price));  // off tick

    const QuoteManagerResult r = manager.evaluate(intent, input());
    EXPECT_EQ(r.bid.outcome, SlotOutcome::BlockedInvalid);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::New) << "one bad side must not silence the other";
    EXPECT_EQ(r.action_count, 1);
}

TEST_F(QuoteFailureTest, AnInvalidQuoteDoesNotDisturbAnExistingOrder) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    ASSERT_TRUE(Px::parse("60000.005", intent.bid_price));

    const QuoteManagerResult r = manager.evaluate(intent, in);
    // Cancelling on the strength of a quote we could not validate would leave
    // the maker out of the market because of a strategy bug.
    EXPECT_EQ(r.bid.outcome, SlotOutcome::BlockedInvalid);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 0U);
}

// ===========================================================================
// Staleness and readiness (§9)
// ===========================================================================

TEST_F(QuoteFailureTest, StaleMarketDataWithdrawsQuotes) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");
    in.market_data_age_ns = millis(600);

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.rejection, QuoteRejection::MarketDataStale);
    // Withdrawn rather than merely ignored: whatever rests no longer reflects a
    // market that exists.
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 2U);
    expect_no_order_creating_action(r);
    EXPECT_EQ(manager.metrics().stale_intents, 1U);
}

TEST_F(QuoteFailureTest, ExpiredIntentWithdrawsQuotes) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    intent.computed_ns = clock.steady();
    clock.advance(millis(400));  // limit is 250ms

    const QuoteManagerResult r = manager.evaluate(intent, in);
    EXPECT_EQ(r.rejection, QuoteRejection::IntentExpired);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 1U);
    expect_no_order_creating_action(r);
}

TEST_F(QuoteFailureTest, ExpiredIntentNeverCreatesAQuote) {
    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    intent.computed_ns = clock.steady();
    clock.advance(millis(400));

    const QuoteManagerResult r = manager.evaluate(intent, input());
    EXPECT_EQ(r.rejection, QuoteRejection::IntentExpired);
    EXPECT_FALSE(r.has_actions());
}

TEST_F(QuoteFailureTest, SystemNotReadyWithdrawsQuotes) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.system_ready = false;

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.rejection, QuoteRejection::NotRunning);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 1U);
}

TEST_F(QuoteFailureTest, MissingInstrumentRulesBlockEverything) {
    QuoteManagerInput in = input();
    in.instrument = nullptr;
    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.rejection, QuoteRejection::InstrumentNotLoaded);
    EXPECT_FALSE(r.has_actions()) << "nothing can be validated without venue rules";
}

TEST_F(QuoteFailureTest, AnIntentForAnotherStrategyIsIgnored) {
    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    static_cast<void>(intent.identity.name.assign("other_strategy_v1"));

    const QuoteManagerResult r = manager.evaluate(intent, input());
    EXPECT_EQ(r.rejection, QuoteRejection::IdentityMismatch);
    EXPECT_FALSE(r.has_actions());
}

// ===========================================================================
// Conflicting and out-of-order situations
// ===========================================================================

TEST_F(QuoteFailureTest, StaleWithdrawalDoesNotResurrectOnAnOlderIntent) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    // A stale evaluation at generation 10 withdraws.
    in.market_data_age_ns = millis(600);
    const QuoteManagerResult withdrawn =
        manager.evaluate(quoting_intent(10, "60000.00", "60000.10"), in);
    ASSERT_EQ(withdrawn.count_of(OrderActionType::Cancel), 1U);
    EXPECT_EQ(manager.last_generation(), 10U) << "the withdrawal is the newest statement";

    // Generation 8 then arrives with fresh data and wants quotes back.
    in.market_data_age_ns = 0;
    const QuoteManagerResult older =
        manager.evaluate(quoting_intent(8, "60000.00", "60000.10"), in);
    EXPECT_EQ(older.rejection, QuoteRejection::GenerationRegression);
    EXPECT_FALSE(older.has_actions()) << "an older intent must not undo a newer withdrawal";
}

TEST_F(QuoteFailureTest, ConflictingPendingOperationNeverProducesASecondRequest) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.pending = PendingOperation::Replace;
    in.working.slot(QuoteSlot::Bid) = order;

    // Ten cycles of wildly varying intent while a replace is unresolved.
    for (std::uint64_t generation = 1; generation <= 10; ++generation) {
        const QuoteManagerResult r =
            manager.evaluate(quoting_intent(generation, "60000.50", "60001.00"), in);
        EXPECT_EQ(r.bid.outcome, SlotOutcome::AwaitingPending) << "cycle " << generation;
        EXPECT_EQ(r.count_of(OrderActionType::Replace), 0U) << "cycle " << generation;
    }
}

TEST_F(QuoteFailureTest, UnknownOrderNeverProducesAConflictingNewOrder) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.status = exchange::OrderStatus::Unknown;
    in.working.slot(QuoteSlot::Bid) = order;

    for (std::uint64_t generation = 1; generation <= 10; ++generation) {
        const QuoteManagerResult r =
            manager.evaluate(quoting_intent(generation, "60000.50", "60001.00"), in);
        for (std::uint8_t i = 0; i < r.action_count; ++i) {
            EXPECT_NE(r.actions[i].slot, QuoteSlot::Bid)
                << "cycle " << generation << " touched an order of unknown state";
        }
    }
    EXPECT_EQ(manager.metrics().unknown_order_states, 10U);
}

TEST_F(QuoteFailureTest, OwnershipViolationNeverProducesAnyAction) {
    QuoteManagerInput in = input();
    WorkingOrder foreign = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(foreign.owner.strategy.assign("someone_else_v1"));
    in.working.slot(QuoteSlot::Bid) = foreign;

    for (std::uint64_t generation = 1; generation <= 5; ++generation) {
        for (const auto& intent : {quoting_intent(generation, "60000.50", "60001.00"),
                                   pull_intent(generation + 100)}) {
            const QuoteManagerResult r = manager.evaluate(intent, in);
            for (std::uint8_t i = 0; i < r.action_count; ++i) {
                EXPECT_NE(r.actions[i].slot, QuoteSlot::Bid) << "touched a foreign order";
            }
        }
    }
    EXPECT_GT(manager.metrics().ownership_violations, 0U);
}

TEST_F(QuoteFailureTest, AnUnmanagedOrderIsNeverCancelledEvenByTheKillSwitch) {
    QuoteManagerInput in = input();
    WorkingOrder foreign = resting(QuoteSlot::Ask, "60000.10", "0.001");
    foreign.owner.strategy.clear();  // wholly unmanaged
    in.working.slot(QuoteSlot::Ask) = foreign;

    const QuoteManagerResult r = manager.disable_all_quotes(in, ActionReason::OperatorDisabled);
    EXPECT_FALSE(r.has_actions());
    EXPECT_EQ(r.ask.outcome, SlotOutcome::FrozenUnknown);
}

// ===========================================================================
// The invariants (§26)
// ===========================================================================

TEST_F(QuoteFailureTest, NoFailureModeEverProducesAnExposureAddingAction) {
    // A sweep over the failure surface: whatever goes wrong, nothing that
    // creates or moves a resting order may come out of it.
    const std::vector<std::function<void(QuoteManagerInput&, QuoteIntent&)>> breakages = {
        [](QuoteManagerInput& in, QuoteIntent&) { in.market_data_age_ns = seconds(5); },
        [](QuoteManagerInput& in, QuoteIntent&) { in.system_ready = false; },
        [](QuoteManagerInput& in, QuoteIntent&) { in.instrument = nullptr; },
        [](QuoteManagerInput&, QuoteIntent& i) { i.bid_price = Px::zero(); },
        [](QuoteManagerInput&, QuoteIntent& i) { i.bid_quantity = Qty::zero(); },
        [](QuoteManagerInput&, QuoteIntent& i) {
            static_cast<void>(Px::parse("60000.005", i.bid_price));
        },
        [](QuoteManagerInput& in, QuoteIntent&) {
            WorkingOrder o = resting(QuoteSlot::Bid, "60000.00", "0.001");
            o.status = exchange::OrderStatus::Unknown;
            in.working.slot(QuoteSlot::Bid) = o;
        },
        [](QuoteManagerInput& in, QuoteIntent&) {
            WorkingOrder o = resting(QuoteSlot::Bid, "60000.00", "0.001");
            static_cast<void>(o.owner.strategy.assign("someone_else_v1"));
            in.working.slot(QuoteSlot::Bid) = o;
        },
        [](QuoteManagerInput& in, QuoteIntent&) {
            WorkingOrder o = resting(QuoteSlot::Bid, "60000.00", "0.001");
            o.pending = PendingOperation::Replace;
            in.working.slot(QuoteSlot::Bid) = o;
        },
    };

    for (std::size_t i = 0; i < breakages.size(); ++i) {
        QuoteManager local(StrategyName(kOwner), QuoteManagerConfig{}, clock);
        QuoteManagerInput in = input();
        QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
        intent.quote_ask = false;  // isolate the bid
        breakages[i](in, intent);

        const QuoteManagerResult r = local.evaluate(intent, in);
        for (std::uint8_t a = 0; a < r.action_count; ++a) {
            if (r.actions[a].slot != QuoteSlot::Bid) {
                continue;
            }
            EXPECT_FALSE(r.actions[a].adds_exposure())
                << "breakage " << i << " produced " << to_string(r.actions[a].type);
        }
    }
}

}  // namespace
}  // namespace mm::quote
