#include <gtest/gtest.h>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "support/RecordingSinks.hpp"

namespace mm::exchange::mock {
namespace {

using test::RecordingExecutionSink;

class MockExecutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock.set_steady(millis(1'000));
        clock.set_wall(seconds(1'786'924'800));
        ASSERT_TRUE(venue.start(sink).is_ok());
        sink.clear();
    }

    OrderRequest order(const char* id = "mm-1-1") {
        OrderRequest r;
        r.client_order_id = ClientOrderId(id);
        r.symbol = Symbol("BTCUSDT");
        r.side = Side::Buy;
        r.type = OrderType::Limit;
        EXPECT_TRUE(Px::parse("60000.00", r.price));
        EXPECT_TRUE(Qty::parse("0.010", r.quantity));
        r.created_ns = clock.steady();
        return r;
    }

    /// Advance and deliver everything now due.
    std::size_t advance(Nanos delta) {
        clock.advance(delta);
        return venue.pump();
    }

    ManualClock clock{0, 0};
    MockExchangeExecution venue{clock};
    RecordingExecutionSink sink;
};

TEST_F(MockExecutionTest, DeliversNothingBeforeTimeAdvances) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    EXPECT_EQ(venue.pump(), 0U) << "an event must not arrive before its latency elapses";
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(MockExecutionTest, AcceptedOrderProducesAnAck) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    EXPECT_EQ(advance(millis(1)), 1U);

    const ExecutionEvent* ack = sink.first_of(ExecutionEventType::OrderAck);
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->payload.ack.client_order_id, ClientOrderId("mm-1-1"));
    EXPECT_FALSE(ack->payload.ack.exchange_order_id.empty());
    EXPECT_EQ(ack->payload.ack.status, OrderStatus::New);
    EXPECT_EQ(ack->outcome(), RequestOutcome::Acknowledged);
    EXPECT_TRUE(sink.invalid.empty());
}

TEST_F(MockExecutionTest, RejectedOrderIsDefinitivelyAbsent) {
    venue.script_submit(SubmitBehaviour::Reject);
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));

    const ExecutionEvent* reject = sink.first_of(ExecutionEventType::OrderReject);
    ASSERT_NE(reject, nullptr);
    EXPECT_EQ(reject->outcome(), RequestOutcome::Rejected);
    EXPECT_TRUE(reject->payload.reject.error.order_definitively_absent());
    EXPECT_FALSE(reject->payload.reject.error.needs_reconciliation());
    EXPECT_FALSE(venue.has_order(ClientOrderId("mm-1-1")));
}

// ---------------------------------------------------------------------------
// Acceptance Test D: an unknown result must stay UNKNOWN.
// ---------------------------------------------------------------------------

TEST_F(MockExecutionTest, TimeoutYieldsUnknownNeverRejected) {
    venue.script_submit(SubmitBehaviour::Timeout);
    ASSERT_TRUE(venue.submit(order()).is_ok());

    // Nothing at the normal ack latency: the venue simply never answers.
    EXPECT_EQ(advance(millis(1)), 0U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderReject), 0U);

    advance(millis(6'000));
    const ExecutionEvent* failure = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->payload.failure.kind, RequestKind::Submit);
    EXPECT_EQ(failure->payload.failure.error.category, ExchangeErrorCategory::Timeout);

    // The whole point: unknown, therefore reconcilable, therefore never dropped.
    EXPECT_EQ(failure->outcome(), RequestOutcome::Unknown);
    EXPECT_TRUE(failure->payload.failure.error.needs_reconciliation());
    EXPECT_FALSE(failure->payload.failure.error.order_definitively_absent());

    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderReject), 0U)
        << "a timeout must never be reported as a rejection";
}

TEST_F(MockExecutionTest, TransportLossYieldsUnknown) {
    venue.script_submit(SubmitBehaviour::TransportLoss);
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));

    const ExecutionEvent* failure = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->payload.failure.error.category, ExchangeErrorCategory::Transport);
    EXPECT_EQ(failure->outcome(), RequestOutcome::Unknown);
}

TEST_F(MockExecutionTest, DisconnectMakesInFlightRequestsUnknownNotRejected) {
    venue.script_submit(SubmitBehaviour::Timeout);
    ASSERT_TRUE(venue.submit(order()).is_ok());

    venue.disconnect("socket closed");

    const ExecutionEvent* failure = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(failure, nullptr);
    // The order may be resting on the venue right now. Assuming otherwise is
    // how an untracked position appears.
    EXPECT_EQ(failure->outcome(), RequestOutcome::Unknown);
    EXPECT_EQ(failure->payload.failure.error.category, ExchangeErrorCategory::ConnectionFailure);
}

// ---------------------------------------------------------------------------
// Local refusal: definitively NOT sent
// ---------------------------------------------------------------------------

TEST_F(MockExecutionTest, SubmitWhileDisconnectedNeverReachesTheVenue) {
    venue.disconnect("down");
    sink.clear();

    const Status s = venue.submit(order());
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(venue.received_submits(), 0U) << "the request must not have reached the venue";
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(MockExecutionTest, DuplicateClientOrderIdIsRefusedLocally) {
    ASSERT_TRUE(venue.submit(order("dup")).is_ok());
    advance(millis(1));
    EXPECT_TRUE(venue.submit(order("dup")).is_error());
}

TEST_F(MockExecutionTest, ReconnectRestoresService) {
    venue.disconnect("down");
    EXPECT_EQ(venue.connection_state(), ConnectionState::Disconnected);
    EXPECT_FALSE(venue.is_authenticated());

    venue.reconnect();
    EXPECT_EQ(venue.connection_state(), ConnectionState::Connected);
    EXPECT_TRUE(venue.is_authenticated());
    EXPECT_TRUE(venue.submit(order()).is_ok());
}

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

TEST_F(MockExecutionTest, PartialThenFullFillAccumulates) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    Px px;
    Qty half;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.005", half));

    ASSERT_TRUE(venue.deliver_fill(ClientOrderId("mm-1-1"), px, half, Liquidity::Maker,
                                   TradeId("T1")).is_ok());
    const ExecutionEvent* first = sink.last_of(ExecutionEventType::Fill);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->payload.fill.cumulative_qty.to_string(), "0.005");
    EXPECT_EQ(first->payload.fill.leaves_qty.to_string(), "0.005");
    EXPECT_EQ(first->payload.fill.order_status, OrderStatus::PartiallyFilled);

    ASSERT_TRUE(venue.deliver_fill(ClientOrderId("mm-1-1"), px, half, Liquidity::Maker,
                                   TradeId("T2")).is_ok());
    const ExecutionEvent* second = sink.last_of(ExecutionEventType::Fill);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->payload.fill.cumulative_qty.to_string(), "0.01");
    EXPECT_TRUE(second->payload.fill.leaves_qty.is_zero());
    EXPECT_EQ(second->payload.fill.order_status, OrderStatus::Filled);
    EXPECT_EQ(venue.order_status(ClientOrderId("mm-1-1")), OrderStatus::Filled);
    EXPECT_TRUE(sink.invalid.empty());
}

TEST_F(MockExecutionTest, OverfillIsRefusedRatherThanScripted) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));

    Px px;
    Qty too_much;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.02", too_much));
    // A venue cannot fill more than the order; letting a test script it would
    // prove nothing about the code under test.
    EXPECT_TRUE(venue.deliver_fill(ClientOrderId("mm-1-1"), px, too_much, Liquidity::Maker,
                                   TradeId("T1")).is_error());
}

TEST_F(MockExecutionTest, FillsCarryDistinctTradeIdsForIdempotency) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    ASSERT_TRUE(venue.deliver_full_fill(ClientOrderId("mm-1-1"), Liquidity::Maker,
                                        TradeId("T-42")).is_ok());
    const ExecutionEvent* fill = sink.last_of(ExecutionEventType::Fill);
    ASSERT_NE(fill, nullptr);
    // Without a trade id there is no idempotency key and a redelivered report
    // would double-count into position and PnL.
    EXPECT_EQ(fill->payload.fill.trade_id, TradeId("T-42"));
}

TEST_F(MockExecutionTest, RedeliveredEventIsByteIdentical) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();
    ASSERT_TRUE(venue.deliver_full_fill(ClientOrderId("mm-1-1"), Liquidity::Maker,
                                        TradeId("T-42")).is_ok());
    ASSERT_EQ(sink.events.size(), 1U);

    // Venues resend execution reports after a private-stream reconnect.
    ASSERT_TRUE(venue.redeliver_last_event().is_ok());
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].payload.fill.trade_id, sink.events[1].payload.fill.trade_id);
    EXPECT_EQ(sink.events[0].payload.fill.seq, sink.events[1].payload.fill.seq);
}

// ---------------------------------------------------------------------------
// Cancels
// ---------------------------------------------------------------------------

CancelRequest cancel_for(const char* id) {
    CancelRequest c;
    c.client_order_id = ClientOrderId(id);
    c.symbol = Symbol("BTCUSDT");
    return c;
}

TEST_F(MockExecutionTest, AcceptedCancelTerminatesTheOrder) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    ASSERT_TRUE(venue.cancel(cancel_for("mm-1-1")).is_ok());
    advance(millis(1));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::Accepted);
    EXPECT_EQ(e->payload.cancel.order_status, OrderStatus::Canceled);
    EXPECT_EQ(venue.live_order_count(), 0U);
}

TEST_F(MockExecutionTest, RejectedCancelLeavesTheOrderWorking) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    venue.script_cancel(CancelBehaviour::Reject);
    ASSERT_TRUE(venue.cancel(cancel_for("mm-1-1")).is_ok());
    advance(millis(1));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::Rejected);
    // The order is still live; releasing exposure here would lose track of it.
    EXPECT_FALSE(is_terminal(e->payload.cancel.order_status));
    EXPECT_EQ(venue.live_order_count(), 1U);
    EXPECT_TRUE(sink.invalid.empty());
}

TEST_F(MockExecutionTest, CancelTimeoutIsUnknownAndKeepsTheOrderLive) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    venue.script_cancel(CancelBehaviour::Timeout);
    ASSERT_TRUE(venue.cancel(cancel_for("mm-1-1")).is_ok());
    advance(millis(6'000));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.failure.kind, RequestKind::Cancel);
    EXPECT_EQ(e->outcome(), RequestOutcome::Unknown);
    EXPECT_EQ(venue.live_order_count(), 1U) << "an unconfirmed cancel must not retire the order";
}

TEST_F(MockExecutionTest, CancelLosingTheRaceToAFillIsReportedAsTooLate) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    sink.clear();

    venue.script_cancel(CancelBehaviour::TooLate);
    ASSERT_TRUE(venue.cancel(cancel_for("mm-1-1")).is_ok());
    advance(millis(1));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::TooLate);
    EXPECT_EQ(e->payload.cancel.order_status, OrderStatus::Filled);
}

TEST_F(MockExecutionTest, CancellingAnUnknownOrderIsUnknownNotAbsent) {
    ASSERT_TRUE(venue.cancel(cancel_for("never-existed")).is_ok());
    advance(millis(1));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::OrderNotFound);
    EXPECT_EQ(e->payload.cancel.error.outcome, RequestOutcome::Unknown);
}

// ---------------------------------------------------------------------------
// Queries and capabilities
// ---------------------------------------------------------------------------

TEST_F(MockExecutionTest, OpenOrdersSnapshotIsChunkedAndTerminated) {
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(venue.submit(order(("mm-1-" + std::to_string(i)).c_str())).is_ok());
    }
    advance(millis(1));
    sink.clear();

    ASSERT_TRUE(venue.query_open_orders(Symbol{}).is_ok());
    advance(millis(1));

    std::size_t total = 0;
    std::size_t last_markers = 0;
    for (const auto& e : sink.events) {
        if (e.type != ExecutionEventType::OpenOrdersSnapshot) {
            continue;
        }
        total += e.payload.open_orders.count;
        if (e.payload.open_orders.is_last) {
            ++last_markers;
        }
    }
    EXPECT_EQ(total, 20U);
    // Reconciliation may only conclude an order is absent after a COMPLETE
    // listing, so exactly one chunk carries the terminator.
    EXPECT_EQ(last_markers, 1U);
    EXPECT_GT(sink.count_of(ExecutionEventType::OpenOrdersSnapshot), 1U) << "expected chunking";
}

TEST_F(MockExecutionTest, QueryingAnAbsentOrderReportsUnknownNotEmpty) {
    ASSERT_TRUE(venue.query_order(ClientOrderId("ghost"), ExchangeOrderId{}).is_ok());
    advance(millis(1));

    const ExecutionEvent* e = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.failure.error.category, ExchangeErrorCategory::UnknownOrderState);
    EXPECT_EQ(e->outcome(), RequestOutcome::Unknown);
}

TEST_F(MockExecutionTest, UnsupportedOperationsAreRefused) {
    ManualClock c{0, 0};
    MockExchangeExecution minimal(c, minimal_mock_capabilities());
    RecordingExecutionSink s;
    ASSERT_TRUE(minimal.start(s).is_ok());

    ReplaceRequest r;
    r.original_client_order_id = ClientOrderId("a");
    r.new_client_order_id = ClientOrderId("b");
    EXPECT_TRUE(minimal.replace(r).is_error());
    EXPECT_TRUE(minimal.cancel_all(Symbol{}).is_error());
    EXPECT_TRUE(minimal.query_positions().is_error());
}

TEST_F(MockExecutionTest, StopDoesNotCancelRestingOrders) {
    ASSERT_TRUE(venue.submit(order()).is_ok());
    advance(millis(1));
    ASSERT_EQ(venue.live_order_count(), 1U);

    venue.stop();
    // Closing a socket is not a trading decision, and a venue does not forget
    // orders because we disconnected.
    EXPECT_EQ(venue.live_order_count(), 1U);
}

TEST_F(MockExecutionTest, IsDeterministicAcrossRuns) {
    const auto run = [] {
        ManualClock c{0, 0};
        MockExchangeExecution v(c);
        RecordingExecutionSink s;
        EXPECT_TRUE(v.start(s).is_ok());
        for (int i = 0; i < 10; ++i) {
            OrderRequest r;
            r.client_order_id = ClientOrderId(("o" + std::to_string(i)).c_str());
            r.symbol = Symbol("BTCUSDT");
            EXPECT_TRUE(Px::parse("60000.00", r.price));
            EXPECT_TRUE(Qty::parse("0.010", r.quantity));
            EXPECT_TRUE(v.submit(r).is_ok());
        }
        c.advance(millis(10));
        v.pump();
        std::string trace;
        for (const auto& e : s.events) {
            trace += to_string(e.type);
            trace += ':';
            trace += e.client_order_id().to_string();
            trace += ';';
        }
        return trace;
    };
    EXPECT_EQ(run(), run()) << "the mock must produce identical event sequences every run";
}

}  // namespace
}  // namespace mm::exchange::mock
