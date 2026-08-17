#include <gtest/gtest.h>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "support/OmsFixtures.hpp"

namespace mm::oms {
namespace {

using exchange::mock::CancelBehaviour;
using exchange::mock::MockExchangeExecution;
using exchange::mock::SubmitBehaviour;
using test::kOmsOwner;
using test::oms_action;
using test::oms_config;
using test::RiskApprover;

class OmsTest : public ::testing::Test {
protected:
    OmsTest()
        : clock(millis(10'000), 0),
          venue(clock),
          approver(clock),
          oms(oms_config(), journal, clock) {}

    void SetUp() override {
        ASSERT_TRUE(venue.start(oms).is_ok());
        oms.attach_execution(venue);
        static_cast<void>(oms.process_events());
    }

    /// Runs the venue's scheduled events into the OMS and applies them.
    std::size_t pump(Nanos advance = millis(1)) {
        clock.advance(advance);
        static_cast<void>(venue.pump());
        return oms.process_events();
    }

    [[nodiscard]] SubmitResult submit_new(const char* price = "60000.00",
                                          const char* quantity = "1",
                                          Side side = Side::Buy,
                                          std::uint64_t generation = 1) {
        const auto action =
            oms_action(quote::OrderActionType::New, side, price, quantity, generation);
        return oms.submit(approver.approve(action));
    }

    ManualClock clock;
    MockExchangeExecution venue;
    RiskApprover approver;
    InMemoryOrderJournal journal;
    OrderManager oms;
};

// ===========================================================================
// New lifecycle
// ===========================================================================

TEST_F(OmsTest, NewOrderIsPendingUntilAcknowledged) {
    const SubmitResult result = submit_new();
    ASSERT_TRUE(result.accepted) << to_string(result.reject);

    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_NE(order, nullptr);
    // An order is not on the book because we asked for it to be.
    EXPECT_EQ(order->state, OrderState::PendingNew);
    EXPECT_FALSE(order->exchange_order_id.empty() && order->state == OrderState::Working);
    EXPECT_TRUE(order->exchange_order_id.empty()) << "no venue identity until acknowledged";
    EXPECT_TRUE(order->has_pending_request());

    pump();
    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Working);
    EXPECT_FALSE(oms.find(result.logical_id)->exchange_order_id.empty());
    EXPECT_FALSE(oms.find(result.logical_id)->has_pending_request());
}

TEST_F(OmsTest, ThreeIdentitiesAreDistinct) {
    const SubmitResult result = submit_new();
    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_NE(order, nullptr);

    // Logical id exists from creation; client id from the request; exchange id
    // only after the venue answers.
    EXPECT_TRUE(is_valid(order->logical_id));
    EXPECT_FALSE(order->client_order_id.empty());
    EXPECT_TRUE(order->exchange_order_id.empty());

    pump();
    const OrderRecord* acked = oms.find(result.logical_id);
    EXPECT_FALSE(acked->exchange_order_id.empty());
    // And all three resolve to the same record.
    EXPECT_EQ(oms.find(acked->client_order_id), acked);
    EXPECT_EQ(oms.find_by_exchange_id(acked->exchange_order_id), acked);
}

TEST_F(OmsTest, RejectedNewBecomesTerminal) {
    venue.script_submit(SubmitBehaviour::Reject);
    const SubmitResult result = submit_new();
    ASSERT_TRUE(result.accepted);
    pump();

    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Rejected);
    EXPECT_TRUE(order->is_terminal());
    EXPECT_FALSE(order->consumes_exposure());
}

TEST_F(OmsTest, ImmediateFillWithAckIsHandled) {
    venue.script_submit(SubmitBehaviour::AckThenFullFill);
    const SubmitResult result = submit_new();
    ASSERT_TRUE(result.accepted);
    pump(millis(5));

    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_EQ(order->cumulative_quantity.to_string(), "1");
    EXPECT_TRUE(order->remaining().is_zero());
}

// ===========================================================================
// Fills
// ===========================================================================

TEST_F(OmsTest, PartialFillsAccumulateToFilled) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_EQ(order->state, OrderState::Working);
    const ClientOrderId id = order->client_order_id;

    Px px;
    Qty half;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.4", half));

    ASSERT_TRUE(venue.deliver_fill(id, px, half, Liquidity::Maker, TradeId("T1")).is_ok());
    static_cast<void>(oms.process_events());
    order = oms.find(result.logical_id);
    EXPECT_EQ(order->state, OrderState::PartiallyFilled);
    EXPECT_EQ(order->cumulative_quantity.to_string(), "0.4");
    // Remaining is computed, never assumed to be the original.
    EXPECT_EQ(order->remaining().to_string(), "0.6");

    Qty rest;
    ASSERT_TRUE(Qty::parse("0.6", rest));
    ASSERT_TRUE(venue.deliver_fill(id, px, rest, Liquidity::Maker, TradeId("T2")).is_ok());
    static_cast<void>(oms.process_events());
    order = oms.find(result.logical_id);
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_TRUE(order->remaining().is_zero());
}

TEST_F(OmsTest, DuplicateFillIsIgnored) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    Px px;
    Qty half;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.5", half));
    ASSERT_TRUE(venue.deliver_fill(id, px, half, Liquidity::Maker, TradeId("T1")).is_ok());
    static_cast<void>(oms.process_events());
    ASSERT_EQ(oms.find(result.logical_id)->cumulative_quantity.to_string(), "0.5");

    // Venues redeliver execution reports after a private-stream reconnect.
    ASSERT_TRUE(venue.redeliver_last_event().is_ok());
    static_cast<void>(oms.process_events());

    // Double-counting a fill corrupts position and PnL at the same moment, and
    // nothing downstream can tell afterwards.
    EXPECT_EQ(oms.find(result.logical_id)->cumulative_quantity.to_string(), "0.5");
    EXPECT_EQ(oms.metrics().duplicate_fills, 1U);
}

TEST_F(OmsTest, AverageFillPriceIsVolumeWeighted) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    Px first;
    Px second;
    Qty half;
    ASSERT_TRUE(Px::parse("60000.00", first));
    ASSERT_TRUE(Px::parse("60002.00", second));
    ASSERT_TRUE(Qty::parse("0.5", half));
    ASSERT_TRUE(venue.deliver_fill(id, first, half, Liquidity::Maker, TradeId("T1")).is_ok());
    ASSERT_TRUE(venue.deliver_fill(id, second, half, Liquidity::Maker, TradeId("T2")).is_ok());
    static_cast<void>(oms.process_events());

    EXPECT_EQ(oms.find(result.logical_id)->average_fill_price.to_string(), "60001");
}

// ===========================================================================
// Cancel
// ===========================================================================

TEST_F(OmsTest, CancelLifecycle) {
    const SubmitResult created = submit_new();
    pump();
    ASSERT_EQ(oms.find(created.logical_id)->state, OrderState::Working);

    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(created.logical_id)->client_order_id;
    const SubmitResult cancelled = oms.submit(approver.approve(cancel));
    ASSERT_TRUE(cancelled.accepted) << to_string(cancelled.reject);

    // Exposure is NOT released here: a cancel is a request, not a result.
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::PendingCancel);
    EXPECT_TRUE(oms.find(created.logical_id)->consumes_exposure());

    pump();
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::Cancelled);
    EXPECT_FALSE(oms.find(created.logical_id)->consumes_exposure());
}

TEST_F(OmsTest, CancelRejectLeavesTheOrderWorking) {
    const SubmitResult created = submit_new();
    pump();
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(created.logical_id)->client_order_id;

    venue.script_cancel(CancelBehaviour::Reject);
    ASSERT_TRUE(oms.submit(approver.approve(cancel)).accepted);
    pump();

    // The order is still live; assuming otherwise would silently drop exposure.
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::Working);
    EXPECT_TRUE(oms.find(created.logical_id)->consumes_exposure());
}

TEST_F(OmsTest, OrderFillingWhileCancelPendingBecomesFilled) {
    const SubmitResult created = submit_new();
    pump();
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(created.logical_id)->client_order_id;

    venue.script_cancel(CancelBehaviour::TooLate);
    ASSERT_TRUE(oms.submit(approver.approve(cancel)).accepted);
    pump();

    // The final event sequence decides the state, not which request we sent.
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::Filled);
}

TEST_F(OmsTest, DuplicateCancelIsSuppressed) {
    const SubmitResult created = submit_new();
    pump();
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(created.logical_id)->client_order_id;

    ASSERT_TRUE(oms.submit(approver.approve(cancel)).accepted);
    const SubmitResult second = oms.submit(approver.approve(cancel));
    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(second.reject, OmsReject::DuplicateRequest);
    EXPECT_EQ(oms.metrics().cancel_requests, 1U) << "only one request reached the venue";
}

TEST_F(OmsTest, CancelOfAnUnknownOrderIsRefused) {
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    static_cast<void>(cancel.target_client_order_id.assign("never-existed"));
    const SubmitResult result = oms.submit(approver.approve(cancel));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reject, OmsReject::UnknownOrder);
}

// ===========================================================================
// Replace and cancel-then-new
// ===========================================================================

TEST_F(OmsTest, ReplaceLifecycle) {
    const SubmitResult created = submit_new("60000.00", "1");
    pump();
    auto replace = oms_action(quote::OrderActionType::Replace, Side::Buy, "60001.00", "2", 2);
    replace.target_client_order_id = oms.find(created.logical_id)->client_order_id;

    ASSERT_TRUE(oms.submit(approver.approve(replace)).accepted);
    // Live parameters are untouched while the request is outstanding.
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::PendingReplace);
    EXPECT_EQ(oms.find(created.logical_id)->price.to_string(), "60000");

    pump();
    const OrderRecord* order = oms.find(created.logical_id);
    EXPECT_EQ(order->state, OrderState::Working);
    EXPECT_EQ(order->price.to_string(), "60001");
    EXPECT_EQ(order->original_quantity.to_string(), "2");
}

TEST_F(OmsTest, CancelThenNewDoesNotCreateTheNewOrderEarly) {
    const SubmitResult created = submit_new("60000.00", "1");
    pump();
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(created.logical_id)->client_order_id;
    ASSERT_TRUE(oms.submit(approver.approve(cancel)).accepted);
    ASSERT_EQ(oms.find(created.logical_id)->state, OrderState::PendingCancel);

    // The quote manager's follow-up New arrives while the cancel is still in
    // flight. Two live orders on one slot is exactly the exposure the
    // sequencing exists to avoid.
    const SubmitResult replacement = submit_new("60001.00", "1");
    EXPECT_TRUE(replacement.accepted);
    EXPECT_TRUE(replacement.deferred) << "the new order must wait for the cancellation boundary";
    EXPECT_EQ(oms.order_count(), 1U) << "no second record yet";
    EXPECT_EQ(oms.metrics().deferred_news, 1U);

    // Once the cancel confirms, the replacement is created -- and only then.
    pump();
    EXPECT_EQ(oms.order_count(), 2U);
    EXPECT_EQ(oms.find(created.logical_id)->state, OrderState::Cancelled);
    EXPECT_EQ(oms.live_order_count(), 0U) << "the replacement is not yet confirmed at the venue";

    // Exposure is what actually matters: one order's worth, never two, at any
    // point in the sequence.
    EXPECT_EQ(oms.exposure(Symbol("BTCUSDT")).working_buy.to_string(), "1");

    pump();
    EXPECT_EQ(oms.live_order_count(), 1U);
    EXPECT_EQ(oms.exposure(Symbol("BTCUSDT")).working_buy.to_string(), "1");
}

TEST_F(OmsTest, StaleGenerationCannotReplaceANewerOrder) {
    const SubmitResult created = submit_new("60000.00", "1", Side::Buy, 10);
    pump();
    auto stale = oms_action(quote::OrderActionType::Replace, Side::Buy, "59000.00", "1", 5);
    stale.target_client_order_id = oms.find(created.logical_id)->client_order_id;

    const SubmitResult result = oms.submit(approver.approve(stale));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reject, OmsReject::StaleGeneration);
    EXPECT_EQ(oms.find(created.logical_id)->price.to_string(), "60000");
}

// ===========================================================================
// Pending operations and duplicates
// ===========================================================================

TEST_F(OmsTest, ASecondNewOnAPendingSlotIsRefused) {
    const SubmitResult first = submit_new();
    ASSERT_TRUE(first.accepted);
    // A slow acknowledgement must not become two orders.
    const SubmitResult second = submit_new();
    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(second.reject, OmsReject::PendingOperation);
    EXPECT_EQ(oms.order_count(), 1U);
}

TEST_F(OmsTest, ClientOrderIdsAreUniqueAndBounded) {
    std::set<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        const SubmitResult r = submit_new("60000.00", "1", i % 2 == 0 ? Side::Buy : Side::Sell);
        if (!r.accepted) {
            continue;
        }
        pump();
        const OrderRecord* order = oms.find(r.logical_id);
        ASSERT_NE(order, nullptr);
        EXPECT_LE(order->client_order_id.size(), 36U);
        EXPECT_TRUE(ids.insert(order->client_order_id.to_string()).second)
            << "duplicate client order id " << order->client_order_id.to_string();
        auto cancel = oms_action(quote::OrderActionType::Cancel, order->side, nullptr, nullptr);
        cancel.target_client_order_id = order->client_order_id;
        static_cast<void>(oms.submit(approver.approve(cancel)));
        pump();
    }
    EXPECT_GE(ids.size(), 10U);
}

// ===========================================================================
// Views consumed by the quote manager and risk
// ===========================================================================

TEST_F(OmsTest, WorkingStateReflectsOwnedSlotsOnly) {
    const SubmitResult bid = submit_new("60000.00", "1", Side::Buy);
    pump();
    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kOmsOwner));

    EXPECT_TRUE(state.slot(quote::QuoteSlot::Bid).present);
    EXPECT_FALSE(state.slot(quote::QuoteSlot::Ask).present);
    EXPECT_EQ(state.slot(quote::QuoteSlot::Bid).price.to_string(), "60000");
    EXPECT_EQ(state.foreign_order_count, 0U);

    // Another strategy sees ours as foreign, never as one of its slots.
    const quote::WorkingQuoteState other =
        oms.working_state(Symbol("BTCUSDT"), StrategyName("someone_else_v1"));
    EXPECT_FALSE(other.slot(quote::QuoteSlot::Bid).present);
    EXPECT_EQ(other.foreign_order_count, 1U);
    static_cast<void>(bid);
}

TEST_F(OmsTest, ExposureCountsPendingCancelsAndPendingNews) {
    const SubmitResult first = submit_new("60000.00", "1", Side::Buy);
    // Still PendingNew: it may already be working at the venue.
    risk::ExposureSnapshot exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_TRUE(exposure.determinate);
    EXPECT_EQ(exposure.working_buy.to_string(), "1");

    pump();
    auto cancel = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    cancel.target_client_order_id = oms.find(first.logical_id)->client_order_id;
    ASSERT_TRUE(oms.submit(approver.approve(cancel)).accepted);

    // A cancel is a request, not a result: exposure stays until confirmed.
    exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_EQ(exposure.working_buy.to_string(), "1");

    pump();
    exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_TRUE(exposure.working_buy.is_zero());
}

TEST_F(OmsTest, UnknownOrderMakesExposureIndeterminate) {
    venue.script_submit(SubmitBehaviour::Timeout);
    const SubmitResult result = submit_new();
    ASSERT_TRUE(result.accepted);

    clock.advance(seconds(10));
    oms.on_timer();
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::Unknown);

    // Risk must not add to an exposure it cannot measure.
    const risk::ExposureSnapshot exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_FALSE(exposure.determinate);
    EXPECT_EQ(exposure.indeterminate_order_count, 1U);
}

// ===========================================================================
// Journal
// ===========================================================================

TEST_F(OmsTest, EveryTransitionIsJournalled) {
    const SubmitResult result = submit_new();
    pump();
    const auto records = journal.for_order(result.logical_id);
    ASSERT_GE(records.size(), 3U);

    EXPECT_EQ(records[0].type, OrderEventType::Created);
    EXPECT_EQ(records[1].type, OrderEventType::RequestSent);
    EXPECT_EQ(records[2].type, OrderEventType::Acknowledged);
    // Engine sequence, not a venue timestamp from a clock we do not control.
    for (std::size_t i = 1; i < records.size(); ++i) {
        EXPECT_GT(records[i].sequence, records[i - 1].sequence);
    }
    // Enough to reconstruct the transition, which is what makes replay possible.
    EXPECT_EQ(records[2].from_state, OrderState::PendingNew);
    EXPECT_EQ(records[2].to_state, OrderState::Working);
}

TEST_F(OmsTest, EveryEnumHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(OrderState::Orphaned); ++i) {
        EXPECT_NE(to_string(static_cast<OrderState>(i)), "UNKNOWN_STATE") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(OrderEventType::EventQuarantined);
         ++i) {
        EXPECT_NE(to_string(static_cast<OrderEventType>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(OmsReject::IdentityCollision); ++i) {
        EXPECT_NE(to_string(static_cast<OmsReject>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::oms
