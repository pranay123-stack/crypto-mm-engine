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

class QuoteManagerTest : public ::testing::Test {
protected:
    QuoteManagerTest()
        : clock(millis(1'000), 0),
          spec(btc_spec()),
          caps(caps_with_replace(true)),
          manager(StrategyName(kOwner), QuoteManagerConfig{}, clock) {}

    QuoteManagerInput input() {
        QuoteManagerInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.capabilities = &caps;
        in.market_data_age_ns = 0;
        in.system_ready = true;
        return in;
    }

    /// Finds the action for a slot, or null.
    static const OrderAction* action_for(const QuoteManagerResult& r, QuoteSlot slot,
                                         OrderActionType type) {
        for (std::uint8_t i = 0; i < r.action_count; ++i) {
            if (r.actions[i].slot == slot && r.actions[i].type == type) {
                return &r.actions[i];
            }
        }
        return nullptr;
    }

    ManualClock clock;
    InstrumentSpec spec;
    exchange::ExchangeCapabilities caps;
    QuoteManager manager;
};

// ===========================================================================
// The diff table (§24)
// ===========================================================================

TEST_F(QuoteManagerTest, NoOrderPlusDesiredQuoteYieldsNew) {
    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"),
                                                  input());
    ASSERT_TRUE(r.intent_accepted);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::New);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::New);
    EXPECT_EQ(r.action_count, 2);

    const OrderAction* bid = action_for(r, QuoteSlot::Bid, OrderActionType::New);
    ASSERT_NE(bid, nullptr);
    EXPECT_EQ(bid->side, Side::Buy);
    EXPECT_EQ(bid->price.to_string(), "60000");
    EXPECT_EQ(bid->quantity.to_string(), "0.001");
    EXPECT_EQ(bid->reason, ActionReason::NoQuoteResting);
    // Provenance travels with the action, so a fill months later is attributable.
    EXPECT_EQ(bid->identity.name, StrategyName(kOwner));
    EXPECT_EQ(bid->generation, 1U);
}

TEST_F(QuoteManagerTest, CurrentEqualsDesiredYieldsKeep) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Keep);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Keep);
    EXPECT_EQ(r.action_count, 0) << "an order that already says what we mean is left alone";
    EXPECT_EQ(manager.metrics().keeps, 2U);
}

TEST_F(QuoteManagerTest, PriceChangeYieldsReplace) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.05", "60000.20"), in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Replace);
    EXPECT_EQ(r.bid.reason, ActionReason::PriceChanged);

    const OrderAction* replace = action_for(r, QuoteSlot::Bid, OrderActionType::Replace);
    ASSERT_NE(replace, nullptr);
    EXPECT_EQ(replace->price.to_string(), "60000.05");
    // Addresses the existing order rather than guessing at one.
    EXPECT_EQ(replace->target_client_order_id, ClientOrderId("mm-bid-1"));
    EXPECT_EQ(replace->target_exchange_order_id, ExchangeOrderId("EX-1"));
}

TEST_F(QuoteManagerTest, QuantityChangeYieldsReplace) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    ASSERT_TRUE(Qty::parse("0.002", intent.bid_quantity));
    const QuoteManagerResult r = manager.evaluate(intent, in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Replace);
    EXPECT_EQ(r.bid.reason, ActionReason::QuantityChanged);
}

TEST_F(QuoteManagerTest, DesiredSideDisabledYieldsCancel) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");

    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    intent.quote_bid = false;  // ask only

    const QuoteManagerResult r = manager.evaluate(intent, in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Cancel);
    EXPECT_EQ(r.bid.reason, ActionReason::QuoteDisabled);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Keep);
    ASSERT_NE(action_for(r, QuoteSlot::Bid, OrderActionType::Cancel), nullptr);
    EXPECT_EQ(r.action_count, 1);
}

TEST_F(QuoteManagerTest, PullYieldsCancelBothSides) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");

    const QuoteManagerResult r = manager.evaluate(pull_intent(1), in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Cancel);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Cancel);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 2U);
}

TEST_F(QuoteManagerTest, PullWithNothingRestingIsIdle) {
    const QuoteManagerResult r = manager.evaluate(pull_intent(1), input());
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Idle);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Idle);
    EXPECT_FALSE(r.has_actions());
}

TEST_F(QuoteManagerTest, OneSidedIntentQuotesOnlyThatSide) {
    QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    intent.quote_ask = false;
    const QuoteManagerResult r = manager.evaluate(intent, input());
    EXPECT_EQ(r.bid.outcome, SlotOutcome::New);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Idle);
    EXPECT_EQ(r.action_count, 1);
}

TEST_F(QuoteManagerTest, NoChangeLeavesRestingQuotesAlone) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteIntent intent = pull_intent(1);
    intent.action = QuoteAction::NoChange;
    const QuoteManagerResult r = manager.evaluate(intent, in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Keep) << "NoChange must not withdraw anything";
    EXPECT_FALSE(r.has_actions());
}

// ===========================================================================
// Idempotency (§12, §26)
// ===========================================================================

TEST_F(QuoteManagerTest, SameIntentTwiceIsIdempotent) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");
    const QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");

    const QuoteManagerResult first = manager.evaluate(intent, in);
    const QuoteManagerResult second = manager.evaluate(intent, in);

    // The example from the brief: bid 100 x 1 desired against bid 100 x 1
    // resting stays KEEP, never CANCEL + NEW.
    EXPECT_EQ(first.bid.outcome, SlotOutcome::Keep);
    EXPECT_EQ(second.bid.outcome, SlotOutcome::Keep);
    EXPECT_EQ(first.action_count, 0);
    EXPECT_EQ(second.action_count, 0);
    EXPECT_EQ(manager.metrics().duplicate_intents, 1U);
}

TEST_F(QuoteManagerTest, RepeatedNewIsNotIssuedTwiceBeforeTheWorkingStateCatchesUp) {
    // The dangerous case: the same intent evaluated twice before the OMS has
    // reported the first order. Without the manager remembering what it asked
    // for, this would create two orders.
    const QuoteIntent intent = quoting_intent(1, "60000.00", "60000.10");
    const QuoteManagerResult first = manager.evaluate(intent, input());
    ASSERT_EQ(first.count_of(OrderActionType::New), 2U);

    const QuoteManagerResult second = manager.evaluate(intent, input());
    EXPECT_EQ(second.action_count, 0) << "a duplicate NEW would create a second order";
    EXPECT_EQ(second.bid.outcome, SlotOutcome::AwaitingPending);
}

TEST_F(QuoteManagerTest, SameInputsProduceSameActions) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteManager a(StrategyName(kOwner), QuoteManagerConfig{}, clock);
    QuoteManager b(StrategyName(kOwner), QuoteManagerConfig{}, clock);
    const QuoteIntent intent = quoting_intent(5, "60000.05", "60000.20");

    const QuoteManagerResult ra = a.evaluate(intent, in);
    const QuoteManagerResult rb = b.evaluate(intent, in);
    ASSERT_EQ(ra.action_count, rb.action_count);
    for (std::uint8_t i = 0; i < ra.action_count; ++i) {
        EXPECT_EQ(ra.actions[i].type, rb.actions[i].type);
        EXPECT_EQ(ra.actions[i].slot, rb.actions[i].slot);
        EXPECT_EQ(ra.actions[i].price.raw(), rb.actions[i].price.raw());
        EXPECT_EQ(ra.actions[i].quantity.raw(), rb.actions[i].quantity.raw());
    }
}

// ===========================================================================
// Generation safety (§10, §11, §26)
// ===========================================================================

TEST_F(QuoteManagerTest, OlderGenerationCannotSupersedeNewer) {
    QuoteManagerInput in = input();
    // Generation 10 lands first.
    const QuoteManagerResult newer = manager.evaluate(quoting_intent(10, "60000.00", "60000.10"),
                                                      in);
    ASSERT_EQ(newer.count_of(OrderActionType::New), 2U);
    EXPECT_EQ(manager.last_generation(), 10U);

    // Generation 9 arrives late, wanting quite different prices.
    const QuoteManagerResult older = manager.evaluate(quoting_intent(9, "59000.00", "61000.00"),
                                                      in);
    EXPECT_FALSE(older.intent_accepted);
    EXPECT_EQ(older.rejection, QuoteRejection::GenerationRegression);
    EXPECT_FALSE(older.has_actions()) << "a stale intent must not resurrect anything";
    EXPECT_EQ(manager.last_generation(), 10U) << "and must not move the watermark backwards";
    EXPECT_EQ(manager.metrics().generation_regressions, 1U);
}

TEST_F(QuoteManagerTest, GenerationRegressionDoesNotWithdrawExistingQuotes) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(manager.evaluate(quoting_intent(10, "60000.00", "60000.10"), in));

    const QuoteManagerResult older = manager.evaluate(pull_intent(3), in);
    // A superseded intent says nothing about the present, so it changes nothing
    // -- it must neither create nor withdraw.
    EXPECT_FALSE(older.has_actions());
}

TEST_F(QuoteManagerTest, NewerGenerationsAdvanceTheWatermark) {
    QuoteManagerInput in = input();
    for (std::uint64_t generation : {1U, 2U, 7U, 100U}) {
        static_cast<void>(manager.evaluate(quoting_intent(generation, "60000.00", "60000.10"), in));
        EXPECT_EQ(manager.last_generation(), generation);
    }
}

// ===========================================================================
// Ownership (§14, §26)
// ===========================================================================

TEST_F(QuoteManagerTest, NeverActsOnAnOrderItDoesNotOwn) {
    QuoteManagerInput in = input();
    WorkingOrder foreign = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(foreign.owner.strategy.assign("someone_else_v1"));
    in.working.slot(QuoteSlot::Bid) = foreign;

    const QuoteManagerResult r = manager.evaluate(pull_intent(1), in);
    // Cancelling somebody else's order -- an operator's, or an emergency
    // flattening order -- is the manager reaching outside its mandate.
    EXPECT_FALSE(r.has_actions());
    EXPECT_EQ(r.bid.outcome, SlotOutcome::FrozenUnknown);
    EXPECT_EQ(manager.metrics().ownership_violations, 1U);
}

TEST_F(QuoteManagerTest, DoesNotQuoteAlongsideAForeignOrderInItsSlot) {
    QuoteManagerInput in = input();
    WorkingOrder foreign = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(foreign.owner.strategy.assign("someone_else_v1"));
    in.working.slot(QuoteSlot::Bid) = foreign;

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.05", "60000.20"), in);
    // Doubling up alongside an order we did not create is exposure nobody asked
    // for; standing still is the safe response to the inconsistency.
    EXPECT_EQ(r.bid.outcome, SlotOutcome::FrozenUnknown);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::New) << "the other side is unaffected";
    EXPECT_EQ(r.action_count, 1);
}

TEST_F(QuoteManagerTest, AnOrderOnTheWrongSlotIsNotOurs) {
    QuoteManagerInput in = input();
    WorkingOrder mislabelled = resting(QuoteSlot::Bid, "60000.00", "0.001");
    mislabelled.owner.slot = QuoteSlot::Ask;  // right strategy, wrong slot
    in.working.slot(QuoteSlot::Bid) = mislabelled;

    const QuoteManagerResult r = manager.evaluate(pull_intent(1), in);
    EXPECT_FALSE(r.has_actions());
    EXPECT_EQ(manager.metrics().ownership_violations, 1U);
}

TEST_F(QuoteManagerTest, ForeignOrdersOnTheSymbolAreCountedNotTouched) {
    QuoteManagerInput in = input();
    in.working.foreign_order_count = 3;
    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.count_of(OrderActionType::New), 2U) << "our own slots still work";
    EXPECT_EQ(manager.metrics().foreign_orders_seen, 3U);
}

// ===========================================================================
// Pending operations (§16)
// ===========================================================================

TEST_F(QuoteManagerTest, DoesNotDuplicateAPendingRequest) {
    for (const auto pending : {PendingOperation::New, PendingOperation::Cancel,
                               PendingOperation::Replace}) {
        QuoteManager local(StrategyName(kOwner), QuoteManagerConfig{}, clock);
        QuoteManagerInput in = input();
        WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
        order.pending = pending;
        in.working.slot(QuoteSlot::Bid) = order;

        const QuoteManagerResult r =
            local.evaluate(quoting_intent(1, "60000.90", "60001.00"), in);
        EXPECT_EQ(r.bid.outcome, SlotOutcome::AwaitingPending) << to_string(pending);
        EXPECT_EQ(r.count_of(OrderActionType::Cancel), 0U) << to_string(pending);
        EXPECT_EQ(r.count_of(OrderActionType::Replace), 0U) << to_string(pending);
    }
}

TEST_F(QuoteManagerTest, PendingCancelDoesNotProduceAnotherCancel) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.pending = PendingOperation::Cancel;
    in.working.slot(QuoteSlot::Bid) = order;

    for (std::uint64_t generation = 1; generation <= 5; ++generation) {
        const QuoteManagerResult r = manager.evaluate(pull_intent(generation), in);
        EXPECT_EQ(r.count_of(OrderActionType::Cancel), 0U) << "cycle " << generation;
    }
    EXPECT_GE(manager.metrics().pending_waits, 5U);
}

// ===========================================================================
// Unknown order state (§17)
// ===========================================================================

TEST_F(QuoteManagerTest, UnknownOrderFreezesTheSlot) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.status = exchange::OrderStatus::Unknown;
    in.working.slot(QuoteSlot::Bid) = order;

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.50", "60000.60"), in);
    // The order may still be working. Creating a competing one, or replacing
    // something that might not exist, are both worse than waiting.
    EXPECT_EQ(r.bid.outcome, SlotOutcome::FrozenUnknown);
    EXPECT_EQ(r.count_of(OrderActionType::New), 1U) << "only the ask";
    EXPECT_EQ(r.count_of(OrderActionType::Replace), 0U);
    EXPECT_EQ(manager.metrics().unknown_order_states, 1U);
}

TEST_F(QuoteManagerTest, UnknownOrderMayBeCancelledOnceWhenNoQuoteIsWanted) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.status = exchange::OrderStatus::Unknown;
    in.working.slot(QuoteSlot::Bid) = order;

    // The documented safe exception: cancelling an order that may not exist is
    // harmless, whereas creating one alongside it is not.
    const QuoteManagerResult first = manager.evaluate(pull_intent(1), in);
    EXPECT_EQ(first.bid.outcome, SlotOutcome::Cancel);
    EXPECT_EQ(first.count_of(OrderActionType::Cancel), 1U);

    // And it stays one attempt, not one per cycle.
    in.working.slot(QuoteSlot::Bid).pending = PendingOperation::Cancel;
    const QuoteManagerResult second = manager.evaluate(pull_intent(2), in);
    EXPECT_EQ(second.count_of(OrderActionType::Cancel), 0U);
}

TEST_F(QuoteManagerTest, UnknownIsNeverTreatedAsFilledOrCancelled) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.status = exchange::OrderStatus::Unknown;
    in.working.slot(QuoteSlot::Bid) = order;

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    // Treating it as gone would produce a NEW; treating it as filled would
    // produce a NEW as well. Neither happens.
    EXPECT_EQ(r.count_of(OrderActionType::New), 1U) << "the ask only";
    for (std::uint8_t i = 0; i < r.action_count; ++i) {
        EXPECT_NE(r.actions[i].slot, QuoteSlot::Bid);
    }
}

// ===========================================================================
// Partial fills (§15)
// ===========================================================================

TEST_F(QuoteManagerTest, PartialFillReplenishesToTheIntendedSize) {
    QuoteManagerInput in = input();
    // 0.001 submitted, 0.0004 filled, 0.0006 resting.
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001", "0.0004");

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    // Assuming the original quantity still rests would leave the book quoting
    // 0.0006 while the strategy asked for 0.001.
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Replace);
    EXPECT_EQ(r.bid.reason, ActionReason::PartialFillReplenish);
    const OrderAction* replace = action_for(r, QuoteSlot::Bid, OrderActionType::Replace);
    ASSERT_NE(replace, nullptr);
    EXPECT_EQ(replace->quantity.to_string(), "0.001");
}

TEST_F(QuoteManagerTest, PartialFillCanBeLeftAloneByConfiguration) {
    QuoteManagerConfig config;
    config.replenish_partial_fills = false;
    QuoteManager local(StrategyName(kOwner), config, clock);

    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001", "0.0004");

    const QuoteManagerResult r = local.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Keep);
}

TEST_F(QuoteManagerTest, AFullyFilledOrderIsNotResting) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001", "0.001");
    order.status = exchange::OrderStatus::Filled;
    in.working.slot(QuoteSlot::Bid) = order;

    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::New) << "a filled order leaves nothing resting";
}

TEST_F(QuoteManagerTest, TerminalOrdersLeaveNothingToCancel) {
    for (const auto status : {exchange::OrderStatus::Filled, exchange::OrderStatus::Canceled,
                              exchange::OrderStatus::Rejected, exchange::OrderStatus::Expired}) {
        QuoteManager local(StrategyName(kOwner), QuoteManagerConfig{}, clock);
        QuoteManagerInput in = input();
        WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
        order.status = status;
        in.working.slot(QuoteSlot::Bid) = order;

        const QuoteManagerResult r = local.evaluate(pull_intent(1), in);
        EXPECT_EQ(r.bid.outcome, SlotOutcome::Idle) << exchange::to_string(status);
        EXPECT_FALSE(r.has_actions()) << exchange::to_string(status);
    }
}

// ===========================================================================
// Replace decomposition
// ===========================================================================

TEST_F(QuoteManagerTest, DecomposesReplaceWhereTheVenueLacksIt) {
    exchange::ExchangeCapabilities no_replace = caps_with_replace(false);
    QuoteManagerInput in = input();
    in.capabilities = &no_replace;
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    QuoteIntent intent = quoting_intent(1, "60000.05", "60000.20");
    intent.quote_ask = false;
    const QuoteManagerResult r = manager.evaluate(intent, in);

    // Done consciously and reported as such: exposure is the union of both
    // orders during the window, not a clean swap.
    EXPECT_EQ(r.bid.outcome, SlotOutcome::CancelThenNew);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 1U);
    EXPECT_EQ(r.count_of(OrderActionType::New), 1U);
    EXPECT_EQ(r.count_of(OrderActionType::Replace), 0U);
    EXPECT_EQ(r.actions[0].type, OrderActionType::Cancel) << "cancel is ordered first";
}

// ===========================================================================
// Churn controls (§20)
// ===========================================================================

TEST_F(QuoteManagerTest, SubThresholdPriceMoveIsNotWorthAReplacement) {
    QuoteManagerConfig config;
    config.min_price_move_ticks = 5;  // 5 ticks = 0.05
    QuoteManager local(StrategyName(kOwner), config, clock);

    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");

    // Two ticks away: real, but not worth a round trip.
    const QuoteManagerResult small =
        local.evaluate(quoting_intent(1, "60000.02", "60000.10"), in);
    EXPECT_EQ(small.bid.outcome, SlotOutcome::Keep);

    // Six ticks: material.
    const QuoteManagerResult large =
        local.evaluate(quoting_intent(2, "60000.06", "60000.10"), in);
    EXPECT_EQ(large.bid.outcome, SlotOutcome::Replace);
}

TEST_F(QuoteManagerTest, MinimumReplaceIntervalThrottlesRewrites) {
    QuoteManagerConfig config;
    config.min_replace_interval_ns = millis(50);
    QuoteManager local(StrategyName(kOwner), config, clock);

    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(local.evaluate(quoting_intent(1, "60000.05", "60000.10"), in));

    // Immediately afterwards, another move.
    clock.advance(millis(10));
    const QuoteManagerResult throttled =
        local.evaluate(quoting_intent(2, "60000.09", "60000.10"), in);
    EXPECT_EQ(throttled.bid.outcome, SlotOutcome::SuppressedByChurnControl);
    EXPECT_EQ(local.metrics().churn_suppressions, 1U);

    clock.advance(millis(60));
    const QuoteManagerResult allowed =
        local.evaluate(quoting_intent(3, "60000.09", "60000.10"), in);
    EXPECT_EQ(allowed.bid.outcome, SlotOutcome::Replace);
}

TEST_F(QuoteManagerTest, CooldownAfterCancelDelaysRequoting) {
    QuoteManagerConfig config;
    config.cooldown_after_cancel_ns = millis(100);
    QuoteManager local(StrategyName(kOwner), config, clock);

    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    const QuoteManagerResult cancelled = local.evaluate(pull_intent(1), in);
    ASSERT_EQ(cancelled.bid.outcome, SlotOutcome::Cancel);

    in.working.slot(QuoteSlot::Bid) = WorkingOrder{};  // gone
    clock.advance(millis(20));
    const QuoteManagerResult cooling =
        local.evaluate(quoting_intent(2, "60000.00", "60000.10"), in);
    EXPECT_EQ(cooling.bid.outcome, SlotOutcome::SuppressedByChurnControl);

    clock.advance(millis(120));
    const QuoteManagerResult ready =
        local.evaluate(quoting_intent(3, "60000.00", "60000.10"), in);
    EXPECT_EQ(ready.bid.outcome, SlotOutcome::New);
}

TEST_F(QuoteManagerTest, ChurnControlsNeverAlterTheRequestedQuote) {
    // The distinction that keeps these infrastructure controls rather than
    // strategy logic: they decide *whether* to rewrite, never *what* to write.
    QuoteManagerConfig config;
    config.min_price_move_ticks = 10;
    QuoteManager local(StrategyName(kOwner), config, clock);

    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    const QuoteManagerResult r = local.evaluate(quoting_intent(1, "60000.50", "60000.60"), in);
    ASSERT_EQ(r.bid.outcome, SlotOutcome::Replace);
    const OrderAction* replace = action_for(r, QuoteSlot::Bid, OrderActionType::Replace);
    ASSERT_NE(replace, nullptr);
    EXPECT_EQ(replace->price.to_string(), "60000.5") << "exactly what the strategy asked for";
}

// ===========================================================================
// Quote kill (§18)
// ===========================================================================

TEST_F(QuoteManagerTest, DisableAllQuotesCancelsEveryManagedQuote) {
    QuoteManagerInput in = input();
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, "60000.00", "0.001");
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");

    const QuoteManagerResult r = manager.disable_all_quotes(in, ActionReason::OperatorDisabled);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 2U);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::Cancel);
    EXPECT_EQ(r.bid.reason, ActionReason::OperatorDisabled);
}

TEST_F(QuoteManagerTest, DisableAllQuotesRespectsOwnership) {
    QuoteManagerInput in = input();
    WorkingOrder foreign = resting(QuoteSlot::Bid, "60000.00", "0.001");
    static_cast<void>(foreign.owner.strategy.assign("someone_else_v1"));
    in.working.slot(QuoteSlot::Bid) = foreign;
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, "60000.10", "0.001");

    const QuoteManagerResult r = manager.disable_all_quotes(in, ActionReason::OperatorDisabled);
    // Even a kill switch does not reach outside what this manager owns.
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 1U);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::FrozenUnknown);
    EXPECT_EQ(r.ask.outcome, SlotOutcome::Cancel);
}

TEST_F(QuoteManagerTest, DisableAllQuotesIsIdempotentWhilePending) {
    QuoteManagerInput in = input();
    WorkingOrder order = resting(QuoteSlot::Bid, "60000.00", "0.001");
    order.pending = PendingOperation::Cancel;
    in.working.slot(QuoteSlot::Bid) = order;

    const QuoteManagerResult r = manager.disable_all_quotes(in, ActionReason::OperatorDisabled);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 0U);
    EXPECT_EQ(r.bid.outcome, SlotOutcome::AwaitingPending);
}

// ===========================================================================
// Reset
// ===========================================================================

TEST_F(QuoteManagerTest, ResetClearsTheGenerationWatermark) {
    QuoteManagerInput in = input();
    static_cast<void>(manager.evaluate(quoting_intent(50, "60000.00", "60000.10"), in));
    ASSERT_EQ(manager.last_generation(), 50U);

    manager.reset();
    EXPECT_EQ(manager.last_generation(), 0U);
    // After a resync the strategy restarts, so an earlier generation is valid
    // again rather than a regression.
    const QuoteManagerResult r = manager.evaluate(quoting_intent(1, "60000.00", "60000.10"), in);
    EXPECT_TRUE(r.intent_accepted);
}

TEST_F(QuoteManagerTest, EveryEnumHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(SlotOutcome::BlockedInvalid); ++i) {
        EXPECT_NE(to_string(static_cast<SlotOutcome>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(QuoteRejection::NotRunning); ++i) {
        EXPECT_NE(to_string(static_cast<QuoteRejection>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(ActionReason::ReplaceUnsupported);
         ++i) {
        EXPECT_NE(to_string(static_cast<ActionReason>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(PendingOperation::Replace); ++i) {
        EXPECT_NE(to_string(static_cast<PendingOperation>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::quote
