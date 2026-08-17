/// Phase 9 §41/§42. Deterministic failure injection and the twelve invariants.
///
/// Nothing here is random. A test that fails one time in twenty is worse than
/// no test: it trains people to re-run rather than to look.

#include <gtest/gtest.h>

#include <set>

#include "mm/oms/OrderManager.hpp"
#include "mm/exchange/paper/PaperReplay.hpp"
#include "support/PaperFixtures.hpp"

namespace mm::exchange::paper {
namespace {

using test::paper_config;
using test::paper_order;
using test::RecordingExecutionSink;

class PaperFailureTest : public ::testing::Test {
protected:
    PaperFailureTest() : clock(millis(10'000), millis(10'000)), venue(paper_config(), clock) {}

    void SetUp() override {
        book = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});
        ASSERT_TRUE(venue.start(sink).is_ok());
        venue.attach_book(Symbol("BTCUSDT"), *book);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
    }

    std::size_t pump(Nanos advance = millis(1)) {
        clock.advance(advance);
        return venue.poll();
    }

    void reprice(const std::vector<std::pair<const char*, const char*>>& bids,
                 const std::vector<std::pair<const char*, const char*>>& asks) {
        test::load_book(*book, bids, asks, ++book_seq);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
    }

    void TearDown() override {
        // Every emitted event must be structurally coherent, even the ones
        // produced by injected faults. A fault should make the venue behave
        // badly, not make it emit nonsense.
        EXPECT_TRUE(sink.invalid.empty())
            << "paper execution emitted " << sink.invalid.size()
            << " self-contradictory event(s), first: "
            << (sink.invalid.empty() ? std::string{} : sink.invalid.front());
    }

    [[nodiscard]] Status cancel_of(const char* client_id) {
        CancelRequest req;
        EXPECT_TRUE(req.client_order_id.assign(client_id));
        req.symbol = Symbol("BTCUSDT");
        return venue.cancel(req);
    }

    ManualClock clock;
    RecordingExecutionSink sink;
    std::unique_ptr<book::OrderBook> book;
    Seq book_seq = 100;
    PaperExecution venue;
};

// ===========================================================================
// §18 — deterministic failure injection
// ===========================================================================

TEST_F(PaperFailureTest, DisconnectedVenueRefusesNewOrders) {
    venue.set_connection(ConnectionState::Disconnected);
    const Status s = venue.submit(paper_order("c1", Side::Buy, "59000.00", "1"));
    // Refused synchronously and definitively: nothing was sent, so no order
    // can exist. That is a materially different answer from silence.
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::Unavailable);
    EXPECT_EQ(venue.order_count(), 0U);
}

TEST_F(PaperFailureTest, DegradedVenueStillAcceptsCancels) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    venue.set_connection(ConnectionState::Reconnecting);

    // §19. Withdrawing must stay possible while the transport is imperfect:
    // refusing to cancel because connectivity degraded traps exposure exactly
    // when releasing it matters most.
    EXPECT_TRUE(cancel_of("c1").is_ok());
    pump();
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::Canceled);
}

TEST_F(PaperFailureTest, FullyDisconnectedVenueRefusesEvenCancels) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    venue.set_connection(ConnectionState::Disconnected);
    // There is no socket to send it on. Reporting success would be a lie the
    // OMS would act on.
    EXPECT_TRUE(cancel_of("c1").is_error());
}

TEST_F(PaperFailureTest, InjectedNewRejectIsDeterministic) {
    venue.faults().reject_next_new = 2;
    ASSERT_TRUE(venue.submit(paper_order("a", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("b", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("c", Side::Buy, "59000.00", "1")).is_ok());
    pump();

    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderReject), 2U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 1U);
    EXPECT_EQ(venue.faults().reject_next_new, 0U) << "the counter is consumed, not re-rolled";
}

TEST_F(PaperFailureTest, InjectedCancelRejectLeavesTheOrderWorking) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    venue.faults().reject_next_cancel = 1;
    ASSERT_TRUE(cancel_of("c1").is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::Rejected);
    // Invariant 6: a cancel request does not guarantee cancellation.
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::New);
    EXPECT_TRUE(venue.find_order(ClientOrderId("c1"))->is_resting());
}

TEST_F(PaperFailureTest, InjectedReplaceRejectPreservesEveryParameter) {
    ASSERT_TRUE(venue.submit(paper_order("r1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    const PaperOrder before = *venue.find_order(ClientOrderId("r1"));

    venue.faults().reject_next_replace = 1;
    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59900.00", req.new_price));
    ASSERT_TRUE(Qty::parse("5", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    // Invariant 8: a refused amendment must leave the order exactly as the
    // venue still has it, including its identity.
    const PaperOrder* after = venue.find_order(ClientOrderId("r1"));
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->price, before.price);
    EXPECT_EQ(after->original_qty, before.original_qty);
    EXPECT_EQ(after->exchange_order_id, before.exchange_order_id);
    EXPECT_EQ(after->arrival_sequence, before.arrival_sequence);
    EXPECT_EQ(venue.find_order(ClientOrderId("r2")), nullptr);
}

TEST_F(PaperFailureTest, DroppedRequestProducesNoAnswerAtAll) {
    venue.faults().drop_next_request = 1;
    ASSERT_TRUE(venue.submit(paper_order("lost", Side::Buy, "59000.00", "1")).is_ok());
    pump(seconds(10));

    // Invariant 7: silence is not a cancellation, a rejection, or anything
    // else. The engine must time out and conclude Unknown on its own.
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 0U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderReject), 0U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderCancel), 0U);
    EXPECT_EQ(venue.metrics().requests_dropped, 1U);
}

TEST_F(PaperFailureTest, DelayedAcknowledgementArrivesLateNotNever) {
    venue.faults().delay_next_request_ns = millis(50);
    ASSERT_TRUE(venue.submit(paper_order("slow", Side::Buy, "59000.00", "1")).is_ok());

    pump(millis(10));
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 0U);
    pump(millis(60));
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 1U);
}

TEST_F(PaperFailureTest, DuplicateEventInjectionEmitsTheSameEventTwice) {
    venue.faults().duplicate_next_event = 1;
    ASSERT_TRUE(venue.submit(paper_order("d1", Side::Buy, "59000.00", "1")).is_ok());
    pump();

    // The engine's idempotency is what must absorb this, not the venue's good
    // manners. The venue's job here is to be badly behaved on cue.
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 2U);
    EXPECT_EQ(venue.metrics().duplicates_injected, 1U);
}

TEST_F(PaperFailureTest, ReorderInjectionDeliversEventsOutOfTheOrderTheVenueProduced) {
    // Regression: this fault was declared, documented and counted by
    // `faults().any()`, but nothing ever consumed it. A fault that silently
    // does nothing is worse than an absent one -- a test could inject it and
    // pass without exercising anything.
    ASSERT_TRUE(venue.submit(paper_order("first", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("second", Side::Buy, "58000.00", "1")).is_ok());
    venue.faults().reorder_next_event = 1;
    pump();

    ASSERT_GE(sink.events.size(), 4U);
    // Produced first-then-second; delivered second-then-first.
    EXPECT_EQ(sink.events[2].payload.ack.client_order_id, ClientOrderId("second"));
    EXPECT_EQ(sink.events[3].payload.ack.client_order_id, ClientOrderId("first"));
    EXPECT_EQ(venue.metrics().reorders_injected, 1U);
    // The venue's own sequence numbers still say which came first, which is
    // what lets the engine detect the reordering.
    EXPECT_GT(sink.events[2].payload.ack.seq, sink.events[3].payload.ack.seq);
    EXPECT_EQ(venue.faults().reorder_next_event, 0U) << "consumed, not re-rolled";
}

TEST_F(PaperFailureTest, ReorderingIsDeterministic) {
    const auto run = []() {
        ManualClock c(millis(10'000), millis(10'000));
        RecordingExecutionSink s;
        PaperExecution v(test::paper_config(), c);
        auto b = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});
        EXPECT_TRUE(v.start(s).is_ok());
        v.attach_book(Symbol("BTCUSDT"), *b);
        for (int i = 0; i < 4; ++i) {
            const std::string id = "z" + std::to_string(i);
            static_cast<void>(v.submit(test::paper_order(id.c_str(), Side::Buy, "58000.00", "1")));
        }
        v.faults().reorder_next_event = 2;
        c.advance(millis(2));
        static_cast<void>(v.poll());
        return test::transcript(s);
    };
    // An injected fault must be as reproducible as the happy path, or the
    // failure tests themselves are unreliable.
    EXPECT_EQ(run(), run());
}

TEST_F(PaperFailureTest, FillFeeIsAFeeNotTheTradeValue) {
    // Regression: the fee field was set to the full executed notional, so every
    // consumer would have believed the whole trade was cost.
    PaperExecutionConfig c = test::paper_config();
    c.maker_fee_bps = 10;  // 0.10%
    PaperExecution priced(c, clock);
    RecordingExecutionSink s;
    ASSERT_TRUE(priced.start(s).is_ok());
    priced.attach_book(Symbol("BTCUSDT"), *book);
    priced.on_market_update(Symbol("BTCUSDT"), clock.steady());

    ASSERT_TRUE(priced.submit(test::paper_order("f1", Side::Buy, "60000.00", "1")).is_ok());
    clock.advance(millis(2));
    static_cast<void>(priced.poll());
    test::load_book(*book, {{"59990.00", "1"}}, {{"60000.00", "1"}}, ++book_seq);
    priced.on_market_update(Symbol("BTCUSDT"), clock.steady());
    clock.advance(millis(2));
    static_cast<void>(priced.poll());

    const auto* fill = s.last_of(ExecutionEventType::Fill);
    ASSERT_NE(fill, nullptr);
    const Notional value = notional_of(fill->payload.fill.price, fill->payload.fill.quantity);
    EXPECT_EQ(value.to_string(), "60000");
    // 10 bps of 60000 is 60, not 60000.
    EXPECT_EQ(fill->payload.fill.fee.to_string(), "60");
    EXPECT_LT(fill->payload.fill.fee, value);
}

TEST_F(PaperFailureTest, TakerPaysTheTakerRate) {
    PaperExecutionConfig c = test::paper_config();
    c.maker_fee_bps = 10;
    c.taker_fee_bps = 40;
    PaperExecution priced(c, clock);
    RecordingExecutionSink s;
    ASSERT_TRUE(priced.start(s).is_ok());
    priced.attach_book(Symbol("BTCUSDT"), *book);
    priced.on_market_update(Symbol("BTCUSDT"), clock.steady());

    // Marketable on arrival: crosses the 60000.10 ask, so it takes.
    ASSERT_TRUE(priced.submit(test::paper_order("t1", Side::Buy, "60000.20", "1")).is_ok());
    clock.advance(millis(3));
    static_cast<void>(priced.poll());

    const auto* fill = s.last_of(ExecutionEventType::Fill);
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->payload.fill.liquidity, Liquidity::Taker);
    // 40 bps of 60000.10.
    EXPECT_EQ(fill->payload.fill.fee.to_string(), "240.0004");
}

TEST_F(PaperFailureTest, InjectedExecutionErrorReportsUnknownOutcome) {
    venue.faults().error_next_request = 1;
    ASSERT_TRUE(venue.submit(paper_order("e1", Side::Buy, "59000.00", "1")).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(e, nullptr);
    // A transport error mid-flight cannot say whether the venue saw the
    // request. Unknown is the only honest answer.
    EXPECT_EQ(e->payload.failure.error.outcome, RequestOutcome::Unknown);
    EXPECT_EQ(venue.metrics().execution_errors, 1U);
}

TEST_F(PaperFailureTest, HiddenSnapshotOrderCreatesAReconciliationDiscrepancy) {
    ASSERT_TRUE(venue.submit(paper_order("visible", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("hidden", Side::Buy, "58000.00", "1")).is_ok());
    pump();
    ASSERT_EQ(venue.resting_order_count(), 2U);

    venue.faults().hide_next_snapshot_orders = 1;
    ASSERT_TRUE(venue.query_open_orders(Symbol("BTCUSDT")).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OpenOrdersSnapshot);
    ASSERT_NE(e, nullptr);
    // The venue is resting two orders but reports one. This is the shape of
    // the divergence reconciliation exists to find.
    EXPECT_EQ(e->payload.open_orders.count, 1U);
    EXPECT_EQ(venue.resting_order_count(), 2U);
}

// ===========================================================================
// §42 — invariants
// ===========================================================================

TEST_F(PaperFailureTest, Invariant1_NoOrderExistsWithoutAValidRequest) {
    // Every path that would create an order refuses first.
    EXPECT_TRUE(venue.submit(paper_order("", Side::Buy, "59000.00", "1")).is_error());
    OrderRequest bad_price = paper_order("p", Side::Buy, "59000.00", "1");
    bad_price.price = Px{};
    EXPECT_TRUE(venue.submit(bad_price).is_error());
    OrderRequest bad_qty = paper_order("q", Side::Buy, "59000.00", "1");
    bad_qty.quantity = Qty{};
    EXPECT_TRUE(venue.submit(bad_qty).is_error());

    pump();
    EXPECT_EQ(venue.order_count(), 0U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 0U);
}

TEST_F(PaperFailureTest, Invariant4_ExecutedQuantityNeverExceedsOriginal) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "3")).is_ok());
    pump();
    for (int i = 0; i < 10; ++i) {
        reprice({{"59000.00", "1"}}, {{"59500.00", "100"}});
        pump();
        const PaperOrder* o = venue.find_order(ClientOrderId("c1"));
        ASSERT_NE(o, nullptr);
        EXPECT_LE(o->cumulative_qty, o->original_qty);
        EXPECT_FALSE(o->remaining().is_negative());
    }
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->cumulative_qty.to_string(), "3");
}

TEST_F(PaperFailureTest, Invariant4_TotalFillsNeverExceedDisplayedLiquidity) {
    // Five orders at the same price against two units of displayed size. If
    // each matched the untouched book independently the venue would print more
    // volume than the market ever showed.
    for (int i = 0; i < 5; ++i) {
        const std::string id = "o" + std::to_string(i);
        ASSERT_TRUE(venue.submit(paper_order(id.c_str(), Side::Buy, "59900.00", "2")).is_ok());
    }
    pump();
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});
    pump();

    Qty total{};
    for (int i = 0; i < 5; ++i) {
        const std::string id = "o" + std::to_string(i);
        const PaperOrder* o = venue.find_order(ClientOrderId(id.c_str()));
        ASSERT_NE(o, nullptr);
        ASSERT_TRUE(checked_add(total, o->cumulative_qty, total));
    }
    EXPECT_EQ(total.to_string(), "2") << "the market showed 2; the venue cannot print more";
}

TEST_F(PaperFailureTest, Invariant2And3_IdentitiesAreUnique) {
    std::set<std::string> exchange_ids;
    std::set<std::string> client_ids;
    for (int i = 0; i < 25; ++i) {
        const std::string cid = "u" + std::to_string(i);
        ASSERT_TRUE(venue.submit(paper_order(cid.c_str(), Side::Buy, "58000.00", "1")).is_ok());
        pump();
        const PaperOrder* o = venue.find_order(ClientOrderId(cid.c_str()));
        ASSERT_NE(o, nullptr);
        EXPECT_TRUE(exchange_ids.insert(o->exchange_order_id.to_string()).second);
        EXPECT_TRUE(client_ids.insert(o->client_order_id.to_string()).second);
    }
    // Replacement mints a fresh venue identity, which must also be unique.
    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("u0"));
    ASSERT_TRUE(req.new_client_order_id.assign("u0b"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("58500.00", req.new_price));
    ASSERT_TRUE(Qty::parse("1", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();
    EXPECT_TRUE(
        exchange_ids.insert(venue.find_order(ClientOrderId("u0b"))->exchange_order_id.to_string())
            .second);
}

TEST_F(PaperFailureTest, Invariant9_RepeatedRunsProduceIdenticalTranscripts) {
    const auto run = [](std::uint32_t reject_after) {
        ManualClock c(millis(10'000), millis(10'000));
        RecordingExecutionSink s;
        PaperExecution v(paper_config(), c);
        auto b = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});
        EXPECT_TRUE(v.start(s).is_ok());
        v.attach_book(Symbol("BTCUSDT"), *b);
        v.on_market_update(Symbol("BTCUSDT"), c.steady());
        v.faults().reject_next_new = reject_after;

        Seq seq = 100;
        for (int i = 0; i < 4; ++i) {
            const std::string id = "x" + std::to_string(i);
            static_cast<void>(v.submit(paper_order(id.c_str(), Side::Buy,
                                                   (std::to_string(59000 + i * 10) + ".00").c_str(),
                                                   "2")));
        }
        c.advance(millis(1));
        static_cast<void>(v.poll());

        test::load_book(*b, {{"58900.00", "1"}}, {{"59000.00", "3"}}, ++seq);
        v.on_market_update(Symbol("BTCUSDT"), c.steady());
        c.advance(millis(1));
        static_cast<void>(v.poll());

        CancelRequest req;
        EXPECT_TRUE(req.client_order_id.assign("x2"));
        req.symbol = Symbol("BTCUSDT");
        static_cast<void>(v.cancel(req));
        c.advance(millis(2));
        static_cast<void>(v.poll());
        return test::transcript(s);
    };

    // Including the failure path: injected faults must be as reproducible as
    // the happy path, or the failure tests themselves are unreliable.
    EXPECT_EQ(run(0), run(0));
    EXPECT_EQ(run(2), run(2));
    EXPECT_NE(run(0), run(2)) << "the fault must actually change the outcome";
}

TEST_F(PaperFailureTest, Invariant11_OmsCannotMutateVenueState) {
    // The venue exposes its orders as `const` and by value-free pointer only;
    // there is no setter, no handle, and no shared object (§24).
    static_assert(
        std::is_same_v<decltype(std::declval<const PaperExecution&>().find_order(ClientOrderId())),
                       const PaperOrder*>,
        "venue order state must not be reachable for mutation");

    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    const PaperOrder* order = venue.find_order(ClientOrderId("c1"));
    ASSERT_NE(order, nullptr);

    // The only way to change it is to send a request and wait for an answer,
    // which is exactly the property the simulator exists to preserve.
    EXPECT_TRUE(cancel_of("c1").is_ok());
    EXPECT_EQ(order->status, OrderStatus::New) << "unchanged until the venue answers";
    pump();
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::Canceled);
}

TEST_F(PaperFailureTest, Invariant10_VenueDoesNotReachIntoTheOms) {
    // The venue's only outbound channel is the sink it was handed. It holds no
    // reference to the OMS, no callback into it, and cannot name its types.
    oms::OmsConfig cfg;
    oms::NullOrderJournal journal;
    oms::OrderManager oms(cfg, journal, clock);
    oms.attach_execution(venue);
    ASSERT_TRUE(venue.start(oms).is_ok());

    // A request the OMS never made produces no OMS state, because the only
    // thing the venue can do is emit an event the OMS chooses to consume.
    ASSERT_TRUE(venue.submit(paper_order("direct", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    // The OMS enqueues on the io thread and applies on its own; nothing the
    // venue does reaches OMS state without the OMS choosing to drain.
    EXPECT_EQ(oms.order_count(), 0U) << "the venue cannot create an order in the OMS";
    static_cast<void>(oms.process_events());
    EXPECT_EQ(oms.order_count(), 0U) << "and still cannot, once the event is applied";
    // The event was delivered and quarantined -- attributed to nothing, and
    // never used to invent a record.
    EXPECT_GE(oms.metrics().quarantined_events + oms.metrics().orphan_orders, 1U);
}

TEST_F(PaperFailureTest, Invariant12_NoNetworkIsReachableFromThisAdapter) {
    // Structural, and enforced mechanically by tools/check_exchange_boundary.py
    // and tests/architecture. Asserted here too so the intent is visible where
    // the behaviour is tested: the paper venue's answers come from its own
    // state and the engine's book, and from nowhere else.
    EXPECT_EQ(venue.venue(), VenueName("paper"));
    EXPECT_TRUE(venue.is_authenticated()) << "no credential is required or consulted";
}

// ===========================================================================
// §41 — remaining failure paths
// ===========================================================================

TEST_F(PaperFailureTest, InvalidPricesAndQuantitiesAreRefusedSynchronously) {
    OrderRequest negative = paper_order("n1", Side::Buy, "59000.00", "1");
    negative.price = Px::from_raw(-1);
    EXPECT_TRUE(venue.submit(negative).is_error());

    OrderRequest zero_qty = paper_order("n2", Side::Buy, "59000.00", "1");
    zero_qty.quantity = Qty::from_raw(0);
    EXPECT_TRUE(venue.submit(zero_qty).is_error());
}

TEST_F(PaperFailureTest, ClientIdTooLongForTheVenueIsRefused) {
    // Two layers, and both refuse rather than truncate. Truncation is the
    // dangerous outcome: two orders sharing an id makes every subsequent event
    // for either of them ambiguous.
    //
    // The type is the first layer -- ClientOrderId holds 40 characters and
    // `assign` fails rather than cutting.
    OrderRequest too_long_for_the_type = paper_order("x", Side::Buy, "59000.00", "1");
    EXPECT_FALSE(too_long_for_the_type.client_order_id.assign(
        "this-client-order-id-is-far-too-long-for-the-inline-string-type"));

    // The venue's declared limit is the second, and it is stricter (36). An id
    // that fits the type but not the venue is still refused.
    OrderRequest fits_type_not_venue = paper_order("x", Side::Buy, "59000.00", "1");
    ASSERT_TRUE(fits_type_not_venue.client_order_id.assign("0123456789012345678901234567890123456"));
    ASSERT_GT(fits_type_not_venue.client_order_id.size(), venue.capabilities().max_client_order_id_len);
    EXPECT_TRUE(venue.submit(fits_type_not_venue).is_error());
    EXPECT_EQ(venue.order_count(), 0U);
}

TEST_F(PaperFailureTest, CapacityIsRefusedNotGrown) {
    PaperExecutionConfig c = paper_config();
    c.max_orders = 4;
    PaperExecution small(c, clock);
    RecordingExecutionSink s;
    ASSERT_TRUE(small.start(s).is_ok());
    small.attach_book(Symbol("BTCUSDT"), *book);

    std::size_t accepted = 0;
    for (int i = 0; i < 8; ++i) {
        const std::string id = "k" + std::to_string(i);
        if (small.submit(paper_order(id.c_str(), Side::Buy, "58000.00", "1")).is_ok()) {
            ++accepted;
        }
        clock.advance(millis(1));
        static_cast<void>(small.poll());
    }
    EXPECT_EQ(accepted, 4U);
    EXPECT_EQ(small.order_count(), 4U);
}

TEST_F(PaperFailureTest, ReplaceOfATerminalOrderIsRefused) {
    ASSERT_TRUE(venue.submit(paper_order("r1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    ASSERT_TRUE(cancel_of("r1").is_ok());
    pump();
    ASSERT_EQ(venue.find_order(ClientOrderId("r1"))->status, OrderStatus::Canceled);

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59500.00", req.new_price));
    ASSERT_TRUE(Qty::parse("1", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderReplace);
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(e->payload.replace.accepted);
    EXPECT_EQ(venue.find_order(ClientOrderId("r2")), nullptr);
}

TEST_F(PaperFailureTest, StoppingDoesNotCancelRestingOrders) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    venue.stop();

    // Closing a socket is not a trading decision, and a venue does not forget
    // your orders because you disconnected. Cancelling here would hide from
    // the engine that it must cancel deliberately during shutdown.
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::New);
    EXPECT_EQ(venue.resting_order_count(), 1U);
}

}  // namespace
}  // namespace mm::exchange::paper

namespace mm::exchange::paper {
namespace {

/// §43. Record a session, replay it, and require the venue to do exactly the
/// same thing again.
class PaperReplayTest : public ::testing::Test {
protected:
    /// Builds a session exercising every request kind and a market move.
    static RecordedSession build_session(std::uint32_t reject_next_new) {
        exchange::BookLevels initial;
        initial.bids[initial.bid_count++] = {Px::from_raw(6'000'000'000'000), Qty::from_raw(500'000'000)};
        initial.asks[initial.ask_count++] = {Px::from_raw(6'000'010'000'000), Qty::from_raw(500'000'000)};

        PaperExecutionConfig config = test::paper_config();
        config.faults.reject_next_new = reject_next_new;

        SessionRecorder recorder(config, Symbol("BTCUSDT"), initial, 100);
        recorder.record_submit(0, test::paper_order("a", Side::Buy, "59900.00", "2"));
        recorder.record_submit(0, test::paper_order("b", Side::Sell, "60100.00", "2"));
        recorder.record_poll(millis(2));

        exchange::BookLevels moved;
        moved.bids[moved.bid_count++] = {Px::from_raw(5'980'000'000'000), Qty::from_raw(100'000'000)};
        moved.asks[moved.ask_count++] = {Px::from_raw(5'990'000'000'000), Qty::from_raw(300'000'000)};
        recorder.record_market(millis(3), Symbol("BTCUSDT"), moved, 101);
        recorder.record_poll(millis(5));

        CancelRequest cancel;
        static_cast<void>(cancel.client_order_id.assign("b"));
        cancel.symbol = Symbol("BTCUSDT");
        recorder.record_cancel(millis(6), cancel);
        recorder.record_poll(millis(9));

        ReplaceRequest replace;
        static_cast<void>(replace.original_client_order_id.assign("a"));
        static_cast<void>(replace.new_client_order_id.assign("a2"));
        replace.symbol = Symbol("BTCUSDT");
        replace.new_price = Px::from_raw(5'985'000'000'000);
        replace.new_quantity = Qty::from_raw(300'000'000);
        recorder.record_replace(millis(10), replace);
        recorder.record_poll(millis(14));
        return recorder.session();
    }
};

TEST_F(PaperReplayTest, ReplayingASessionReproducesItExactly) {
    const RecordedSession session = build_session(0);
    ASSERT_GT(session.size(), 5U);

    const ReplayResult first = replay(session);
    const ReplayResult second = replay(session);

    ASSERT_FALSE(first.transcript.empty());
    EXPECT_TRUE(first.matches(second)) << first.first_difference(second);
    EXPECT_EQ(first.metrics.events_emitted, second.metrics.events_emitted);
    EXPECT_EQ(first.metrics.fills, second.metrics.fills);
}

TEST_F(PaperReplayTest, ReplayReproducesFailurePathsToo) {
    // A session that only replays cleanly when nothing goes wrong is not much
    // use for debugging an incident, which is the case this exists for.
    const RecordedSession session = build_session(1);
    const ReplayResult first = replay(session);
    const ReplayResult second = replay(session);
    EXPECT_TRUE(first.matches(second)) << first.first_difference(second);

    bool saw_reject = false;
    for (const std::string& line : first.transcript) {
        if (line.rfind("ORDER_REJECT", 0) == 0) {
            saw_reject = true;
        }
    }
    EXPECT_TRUE(saw_reject) << "the injected fault must actually appear in the transcript";
}

TEST_F(PaperReplayTest, ADifferentSessionProducesADifferentTranscript) {
    // Otherwise "identical" would be trivially true and the test would prove
    // nothing at all.
    const ReplayResult clean = replay(build_session(0));
    const ReplayResult faulted = replay(build_session(1));
    EXPECT_FALSE(clean.matches(faulted));
    EXPECT_FALSE(clean.first_difference(faulted).empty());
}

TEST_F(PaperReplayTest, FinalVenueStateIsPartOfWhatMustMatch) {
    const RecordedSession session = build_session(0);
    const ReplayResult first = replay(session);
    const ReplayResult second = replay(session);

    ASSERT_FALSE(first.final_orders.empty());
    ASSERT_EQ(first.final_orders.size(), second.final_orders.size());
    for (std::size_t i = 0; i < first.final_orders.size(); ++i) {
        EXPECT_EQ(first.final_orders[i].client_order_id, second.final_orders[i].client_order_id);
        EXPECT_EQ(first.final_orders[i].exchange_order_id,
                  second.final_orders[i].exchange_order_id);
        EXPECT_EQ(first.final_orders[i].status, second.final_orders[i].status);
        EXPECT_EQ(first.final_orders[i].cumulative_qty, second.final_orders[i].cumulative_qty);
    }
}

}  // namespace
}  // namespace mm::exchange::paper
