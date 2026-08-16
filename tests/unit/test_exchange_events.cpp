#include <gtest/gtest.h>

#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"
#include "mm/exchange/common/OrderRequest.hpp"

namespace mm::exchange {
namespace {

ExecutionEvent make_ack() {
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderAck;
    e.payload.ack.client_order_id = ClientOrderId("mm-1-1");
    e.payload.ack.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Qty::parse("0.01", e.payload.ack.original_qty));
    e.payload.ack.status = OrderStatus::New;
    return e;
}

ExecutionEvent make_fill() {
    ExecutionEvent e;
    e.type = ExecutionEventType::Fill;
    e.payload.fill.client_order_id = ClientOrderId("mm-1-1");
    e.payload.fill.trade_id = TradeId("T1");
    e.payload.fill.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse("60000.00", e.payload.fill.price));
    EXPECT_TRUE(Qty::parse("0.005", e.payload.fill.quantity));
    EXPECT_TRUE(Qty::parse("0.005", e.payload.fill.cumulative_qty));
    EXPECT_TRUE(Qty::parse("0.005", e.payload.fill.leaves_qty));
    return e;
}

// ---------------------------------------------------------------------------
// Self-contradiction detection
// ---------------------------------------------------------------------------

TEST(ValidateExecutionEvent, AcceptsWellFormedEvents) {
    EXPECT_TRUE(validate_execution_event(make_ack()).is_ok());
    EXPECT_TRUE(validate_execution_event(make_fill()).is_ok());
}

TEST(ValidateExecutionEvent, RejectEventMustCarryRejected) {
    // The load-bearing check. An adapter that is not certain the venue refused
    // the order must emit a RequestFailureEvent instead, or the OMS will
    // discard an order that may be live.
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderReject;
    e.payload.reject.client_order_id = ClientOrderId("mm-1-1");
    e.payload.reject.error = ExchangeError::make(ExchangeErrorCategory::Timeout,
                                                 RequestOutcome::Unknown, "no answer");
    const Status s = validate_execution_event(e);
    ASSERT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::Internal);
    EXPECT_NE(s.message().find("RequestFailureEvent"), std::string_view::npos);
}

TEST(ValidateExecutionEvent, RejectEventWithAProperRejectionPasses) {
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderReject;
    e.payload.reject.client_order_id = ClientOrderId("mm-1-1");
    e.payload.reject.error =
        ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                            "insufficient balance", RejectReason::InsufficientBalance);
    EXPECT_TRUE(validate_execution_event(e).is_ok());
    EXPECT_EQ(e.outcome(), RequestOutcome::Rejected);
}

TEST(ValidateExecutionEvent, RequestFailureCannotClaimSuccess) {
    ExecutionEvent e;
    e.type = ExecutionEventType::RequestFailure;
    e.payload.failure.kind = RequestKind::Submit;
    e.payload.failure.error.category = ExchangeErrorCategory::Timeout;
    e.payload.failure.error.outcome = RequestOutcome::Acknowledged;  // hand-forged
    const Status s = validate_execution_event(e);
    ASSERT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::Internal);
}

TEST(ValidateExecutionEvent, RequestFailureNeedsARequestKind) {
    ExecutionEvent e;
    e.type = ExecutionEventType::RequestFailure;
    e.payload.failure.error = ExchangeError::from_category(ExchangeErrorCategory::Timeout);
    // Without the kind, recovery cannot tell an unknown submit (may have made
    // an order) from an unknown cancel (may have left one working).
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, RejectedCancelCannotReportTheOrderTerminal) {
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderCancel;
    e.payload.cancel.client_order_id = ClientOrderId("mm-1-1");
    e.payload.cancel.cancel_status = CancelStatus::Rejected;
    e.payload.cancel.order_status = OrderStatus::Canceled;  // contradiction
    const Status s = validate_execution_event(e);
    ASSERT_TRUE(s.is_error());
    EXPECT_NE(s.message().find("still working"), std::string_view::npos);
}

TEST(ValidateExecutionEvent, RejectedCancelMayCoincideWithAFill) {
    // The order filling while the cancel was refused is a real race, not a
    // contradiction.
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderCancel;
    e.payload.cancel.client_order_id = ClientOrderId("mm-1-1");
    e.payload.cancel.cancel_status = CancelStatus::Rejected;
    e.payload.cancel.order_status = OrderStatus::Filled;
    EXPECT_TRUE(validate_execution_event(e).is_ok());
}

TEST(ValidateExecutionEvent, FillWithoutATradeIdIsRejected) {
    ExecutionEvent e = make_fill();
    e.payload.fill.trade_id.clear();
    // No idempotency key means a redelivered report double-counts into both
    // position and PnL.
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, FillCumulativeCannotBeBelowTheFillItself) {
    ExecutionEvent e = make_fill();
    ASSERT_TRUE(Qty::parse("0.001", e.payload.fill.cumulative_qty));
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, FillQuantityAndPriceMustBePositive) {
    ExecutionEvent zero_qty = make_fill();
    zero_qty.payload.fill.quantity = Qty::zero();
    EXPECT_TRUE(validate_execution_event(zero_qty).is_error());

    ExecutionEvent zero_px = make_fill();
    zero_px.payload.fill.price = Px::zero();
    EXPECT_TRUE(validate_execution_event(zero_px).is_error());
}

TEST(ValidateExecutionEvent, AckCumulativeCannotExceedOriginal) {
    ExecutionEvent e = make_ack();
    ASSERT_TRUE(Qty::parse("0.02", e.payload.ack.cumulative_qty));
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, ReplaceCannotBeBothAcceptedAndFailed) {
    ExecutionEvent e;
    e.type = ExecutionEventType::OrderReplace;
    e.payload.replace.new_client_order_id = ClientOrderId("mm-1-2");
    e.payload.replace.accepted = true;
    e.payload.replace.error = ExchangeError::from_category(ExchangeErrorCategory::Timeout);
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, BalanceComponentsCannotBeNegative) {
    ExecutionEvent e;
    e.type = ExecutionEventType::BalanceUpdate;
    e.payload.balance.asset = Asset("USDT");
    e.payload.balance.free = Qty::from_raw(-1);
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, UntypedEventIsRejected) {
    const ExecutionEvent e;
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

TEST(ValidateExecutionEvent, OpenOrdersCountCannotExceedCapacity) {
    ExecutionEvent e;
    e.type = ExecutionEventType::OpenOrdersSnapshot;
    e.payload.open_orders.count = kMaxOrdersPerSnapshotEvent + 1;
    EXPECT_TRUE(validate_execution_event(e).is_error());
}

// ---------------------------------------------------------------------------
// Envelope semantics
// ---------------------------------------------------------------------------

TEST(ExecutionEvent, OutcomeReflectsWhatTheVenueActuallySaid) {
    EXPECT_EQ(make_ack().outcome(), RequestOutcome::Acknowledged);
    EXPECT_EQ(make_fill().outcome(), RequestOutcome::Acknowledged);

    ExecutionEvent failure;
    failure.type = ExecutionEventType::RequestFailure;
    failure.payload.failure.kind = RequestKind::Submit;
    failure.payload.failure.error = ExchangeError::from_category(ExchangeErrorCategory::Timeout);
    EXPECT_EQ(failure.outcome(), RequestOutcome::Unknown);
}

TEST(ExecutionEvent, AccessorsResolveIdentityAcrossTypes) {
    EXPECT_EQ(make_ack().client_order_id(), ClientOrderId("mm-1-1"));
    EXPECT_EQ(make_fill().symbol(), Symbol("BTCUSDT"));

    ExecutionEvent connection;
    connection.type = ExecutionEventType::Connection;
    EXPECT_TRUE(connection.client_order_id().empty());
    EXPECT_TRUE(connection.symbol().empty());
}

TEST(Events, AreRingTransportable) {
    // Both envelopes cross an SPSC ring by value between the I/O and trading
    // threads; anything owning heap memory would break that.
    static_assert(std::is_trivially_copyable_v<MarketDataEvent>);
    static_assert(std::is_trivially_copyable_v<ExecutionEvent>);
    static_assert(std::is_trivially_copyable_v<OrderRequest>);
    static_assert(std::is_trivially_copyable_v<ExchangeError>);
    SUCCEED();
}

TEST(Events, EnvelopesStayWithinAReasonableRingSlot) {
    // A fat event makes every ring copy expensive. This is a tripwire, not a
    // hard requirement: if it fires, revisit kMaxLevelsPerEvent deliberately
    // rather than letting the slot grow unnoticed.
    EXPECT_LE(sizeof(MarketDataEvent), 768U) << "actual: " << sizeof(MarketDataEvent);
    EXPECT_LE(sizeof(ExecutionEvent), 768U) << "actual: " << sizeof(ExecutionEvent);
}

TEST(Events, EveryEventTypeHasAName) {
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(MarketDataEventType::InstrumentUpdate);
         ++i) {
        EXPECT_NE(to_string(static_cast<MarketDataEventType>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(ExecutionEventType::OrderStatus); ++i) {
        EXPECT_NE(to_string(static_cast<ExecutionEventType>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(RequestKind::MassCancel); ++i) {
        EXPECT_NE(to_string(static_cast<RequestKind>(i)), "UNKNOWN") << int{i};
    }
}

TEST(BookLevels, CountSanityGuardsTheArrayBound) {
    BookLevels l;
    EXPECT_TRUE(l.counts_are_sane());
    EXPECT_TRUE(l.empty());
    l.bid_count = kMaxLevelsPerEvent;
    EXPECT_TRUE(l.counts_are_sane());
    l.bid_count = kMaxLevelsPerEvent + 1;
    EXPECT_FALSE(l.counts_are_sane());
}

}  // namespace
}  // namespace mm::exchange
