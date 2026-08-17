#include <gtest/gtest.h>

#include "support/PaperFixtures.hpp"

namespace mm::exchange::paper {
namespace {

using test::make_book;
using test::paper_config;
using test::paper_order;
using test::RecordingExecutionSink;

class PaperTest : public ::testing::Test {
protected:
    PaperTest() : clock(millis(10'000), millis(10'000)), venue(paper_config(), clock) {}

    void SetUp() override {
        book = make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});
        ASSERT_TRUE(venue.start(sink).is_ok());
        venue.attach_book(Symbol("BTCUSDT"), *book);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
    }

    /// Advances time and delivers whatever became due.
    std::size_t pump(Nanos advance = millis(1)) {
        clock.advance(advance);
        return venue.poll();
    }

    void TearDown() override {
        // The shared sink validates every event for self-contradiction as it
        // arrives. Asserting it here means a malformed event fails the test
        // that produced it rather than some later, unrelated assertion.
        //
        // This was invisible until Phase 9's ODR defect was fixed: the
        // duplicate sink this file used to define did no validation at all.
        EXPECT_TRUE(sink.invalid.empty())
            << "paper execution emitted " << sink.invalid.size()
            << " self-contradictory event(s), first: "
            << (sink.invalid.empty() ? std::string{} : sink.invalid.front());
    }

    /// Loads a new image into the *same* book object, so the pointer the venue
    /// holds stays valid, and tells the venue the market moved.
    void reprice(const std::vector<std::pair<const char*, const char*>>& bids,
                 const std::vector<std::pair<const char*, const char*>>& asks) {
        test::load_book(*book, bids, asks, ++book_seq);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
    }

    ManualClock clock;
    RecordingExecutionSink sink;
    std::unique_ptr<book::OrderBook> book;
    Seq book_seq = 100;
    PaperExecution venue;
};

// ===========================================================================
// §5 — asynchronous semantics
// ===========================================================================

TEST_F(PaperTest, SubmitDoesNotCallTheSinkSynchronously) {
    const std::size_t before = sink.events.size();
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59999.00", "1")).is_ok());

    // The whole point of the boundary: an adapter that answered inside submit()
    // would let the engine pass tests it would fail against any real venue.
    EXPECT_EQ(sink.events.size(), before) << "submit() must hand off, not answer";
    // The request is in flight to the venue; the venue has not seen it yet.
    EXPECT_EQ(venue.in_flight_requests(), 1U);
    EXPECT_EQ(venue.order_count(), 0U);

    pump();
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 1U);
}

TEST_F(PaperTest, EventsArriveOnlyAfterTheirSimulatedLatency) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59999.00", "1")).is_ok());

    // request 500us + ack 500us = 1ms. Not a nanosecond earlier.
    clock.advance(micros(999));
    EXPECT_EQ(venue.poll(), 0U);
    clock.advance(micros(1));
    EXPECT_EQ(venue.poll(), 1U);
}

TEST_F(PaperTest, ConnectionTransitionsAreReported) {
    // start() walked Disconnected -> Connecting -> Connected.
    EXPECT_EQ(sink.count_of(ExecutionEventType::Connection), 2U);
    EXPECT_EQ(venue.connection_state(), ConnectionState::Connected);

    venue.set_connection(ConnectionState::Reconnecting);
    const auto* e = sink.last_of(ExecutionEventType::Connection);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.connection.from, ConnectionState::Connected);
    EXPECT_EQ(e->payload.connection.to, ConnectionState::Reconnecting);
}

// ===========================================================================
// §21 — identity
// ===========================================================================

TEST_F(PaperTest, ExchangeIdsAreDistinctFromClientIds) {
    ASSERT_TRUE(venue.submit(paper_order("mm-1-a", Side::Buy, "59999.00", "1")).is_ok());
    pump();

    const PaperOrder* order = venue.find_order(ClientOrderId("mm-1-a"));
    ASSERT_NE(order, nullptr);
    // Different shape entirely. If anything ever assumes the two are
    // interchangeable it fails here, not against a real venue.
    EXPECT_NE(order->exchange_order_id.to_string(), order->client_order_id.to_string());
    EXPECT_EQ(order->exchange_order_id.to_string().rfind("PX", 0), 0U);
    EXPECT_EQ(venue.find_order(order->exchange_order_id), order);
}

TEST_F(PaperTest, ExchangeIdsAreUnique) {
    std::set<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        const std::string cid = "mm-" + std::to_string(i);
        ASSERT_TRUE(venue.submit(paper_order(cid.c_str(), Side::Buy, "59000.00", "1")).is_ok());
        pump();
        const PaperOrder* o = venue.find_order(ClientOrderId(cid.c_str()));
        ASSERT_NE(o, nullptr);
        EXPECT_TRUE(ids.insert(o->exchange_order_id.to_string()).second);
    }
    EXPECT_EQ(ids.size(), 20U);
}

TEST_F(PaperTest, DuplicateClientIdIsRefused) {
    ASSERT_TRUE(venue.submit(paper_order("dup", Side::Buy, "59999.00", "1")).is_ok());
    pump();
    // Accepting a second would make every subsequent event ambiguous.
    const Status second = venue.submit(paper_order("dup", Side::Buy, "59998.00", "1"));
    EXPECT_TRUE(second.is_error());
    EXPECT_EQ(second.code(), ErrorCode::AlreadyExists);
}

// ===========================================================================
// §8, §9 — fill model and partial fills
// ===========================================================================

TEST_F(PaperTest, ARestingOrderAwayFromTheMarketDoesNotFill) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    ASSERT_EQ(sink.count_of(ExecutionEventType::OrderAck), 1U);

    // The book moves, but not to us.
    reprice({{"59500.00", "5"}}, {{"59500.10", "5"}});
    pump();
    EXPECT_EQ(sink.count_of(ExecutionEventType::Fill), 0U)
        << "a bid at 59000 must not fill against an ask of 59500.10";
}

TEST_F(PaperTest, PartialFillsAccumulateAcrossMarketMoves) {
    // BUY 10 against a market that arrives in pieces — the §9 example.
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "10")).is_ok());
    pump();

    reprice({{"59990.00", "1"}}, {{"60000.00", "3"}});
    pump();
    const PaperOrder* order = venue.find_order(ClientOrderId("c1"));
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->cumulative_qty.to_string(), "3");
    EXPECT_EQ(order->remaining().to_string(), "7");
    EXPECT_EQ(order->status, OrderStatus::PartiallyFilled);

    reprice({{"59990.00", "1"}}, {{"60000.00", "4"}});
    pump();
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->cumulative_qty.to_string(), "7");

    reprice({{"59990.00", "1"}}, {{"60000.00", "3"}});
    pump();
    order = venue.find_order(ClientOrderId("c1"));
    EXPECT_EQ(order->cumulative_qty.to_string(), "10");
    EXPECT_EQ(order->status, OrderStatus::Filled);
    EXPECT_TRUE(order->remaining().is_zero());

    // Three partials then a completion, reported as such.
    EXPECT_EQ(sink.count_of(ExecutionEventType::Fill), 3U);
    EXPECT_EQ(venue.metrics().partial_fills, 2U);
    EXPECT_EQ(venue.metrics().fills, 1U);
}

TEST_F(PaperTest, ASweepReportsEveryLevelItConsumed) {
    // `levels_consumed` exists so a caller can tell a one-level fill from a
    // sweep. Asserted here so the field is load-bearing rather than decorative.
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "6")).is_ok());
    pump();
    reprice({{"59000.00", "1"}}, {{"59500.00", "2"}, {"59700.00", "2"}, {"59900.00", "2"}});
    pump();

    const PaperOrder* order = venue.find_order(ClientOrderId("c1"));
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->cumulative_qty.to_string(), "6");

    std::vector<PriceLevel> available = executable_side(*book, Side::Buy);
    PaperOrder probe;
    probe.side = Side::Buy;
    ASSERT_TRUE(Px::parse("60000.00", probe.price));
    ASSERT_TRUE(Qty::parse("6", probe.original_qty));
    probe.status = OrderStatus::New;
    const MatchResult swept = match_and_consume(probe, available, FillModel::DisplayedLiquidity,
                                                10'000);
    EXPECT_EQ(swept.levels_consumed, 3U) << "three levels were needed to fill 6";

    // And the volume-weighted price reflects all three, not just the touch.
    EXPECT_EQ(swept.price.to_string(), "59700");
}

TEST_F(PaperTest, FillsAreCappedByDisplayedLiquidity) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "10")).is_ok());
    pump();
    // Only 2 displayed at a price we can pay, however far through it trades.
    reprice({{"59990.00", "1"}}, {{"60000.00", "2"}});
    pump();

    // "The price touched my level so I am filled" is the assumption that most
    // flatters a paper run. 2 was shown; 2 is what we get.
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->cumulative_qty.to_string(), "2");
}

TEST_F(PaperTest, QueueShareLimitsWhatWeTakeOfDisplayedSize) {
    PaperExecutionConfig c = paper_config();
    c.queue_share_bps = 5'000;  // half the displayed size is ahead of us
    PaperExecution queued(c, clock);
    RecordingExecutionSink queued_sink;
    ASSERT_TRUE(queued.start(queued_sink).is_ok());
    queued.attach_book(Symbol("BTCUSDT"), *book);

    ASSERT_TRUE(queued.submit(paper_order("q1", Side::Buy, "60000.00", "10")).is_ok());
    clock.advance(millis(1));
    static_cast<void>(queued.poll());

    reprice({{"59990.00", "1"}}, {{"60000.00", "4"}});
    queued.on_market_update(Symbol("BTCUSDT"), clock.steady());
    clock.advance(millis(1));
    static_cast<void>(queued.poll());

    // 4 displayed, half assumed ahead of us.
    EXPECT_EQ(queued.find_order(ClientOrderId("q1"))->cumulative_qty.to_string(), "2");
}

TEST_F(PaperTest, ExecutionHappensAtTheBookPriceNotTheLimit) {
    // A buy limit at 60000 meeting an ask of 59900 pays 59900. Crediting the
    // limit price would invent price improvement that never happened.
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "1")).is_ok());
    pump();
    reprice({{"59800.00", "5"}}, {{"59900.00", "5"}});
    pump();

    const auto* fill = sink.last_of(ExecutionEventType::Fill);
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->payload.fill.price.to_string(), "59900");
}

TEST_F(PaperTest, SellsFillAgainstBids) {
    ASSERT_TRUE(venue.submit(paper_order("s1", Side::Sell, "60000.10", "2")).is_ok());
    pump();
    reprice({{"60000.20", "2"}}, {{"60000.30", "5"}});
    pump();

    const PaperOrder* order = venue.find_order(ClientOrderId("s1"));
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->cumulative_qty.to_string(), "2");
    EXPECT_EQ(sink.last_of(ExecutionEventType::Fill)->payload.fill.price.to_string(), "60000.2");
}

TEST_F(PaperTest, NeverOverfills) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "1")).is_ok());
    pump();
    // Vastly more liquidity than the order wants, repeatedly.
    for (int i = 0; i < 5; ++i) {
        reprice({{"58000.00", "1"}}, {{"59000.00", "1000"}});
        pump();
        const PaperOrder* o = venue.find_order(ClientOrderId("c1"));
        ASSERT_NE(o, nullptr);
        EXPECT_LE(o->cumulative_qty, o->original_qty);
    }
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->cumulative_qty.to_string(), "1");
    EXPECT_EQ(sink.count_of(ExecutionEventType::Fill), 1U) << "a filled order stops filling";
}

// ===========================================================================
// §11, §12 — post-only and marketable limits
// ===========================================================================

TEST_F(PaperTest, PostOnlyThatWouldCrossIsRejected) {
    // Book is 60000.00 / 60000.10. A post-only buy at 60000.20 would take.
    ASSERT_TRUE(venue.submit(paper_order("po", Side::Buy, "60000.20", "1", true)).is_ok());
    pump();

    const auto* reject = sink.last_of(ExecutionEventType::OrderReject);
    ASSERT_NE(reject, nullptr) << "a post-only order must never silently become a taker";
    EXPECT_EQ(reject->payload.reject.error.reason, RejectReason::PostOnlyWouldCross);
    EXPECT_EQ(venue.find_order(ClientOrderId("po")), nullptr);
    EXPECT_EQ(venue.metrics().post_only_rejections, 1U);
}

TEST_F(PaperTest, PostOnlyThatRestsIsAccepted) {
    ASSERT_TRUE(venue.submit(paper_order("po", Side::Buy, "59999.00", "1", true)).is_ok());
    pump();
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderAck), 1U);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderReject), 0U);
}

TEST_F(PaperTest, MarketableLimitTakesImmediatelyAfterItsAck) {
    // Not post-only: crossing is legitimate taker execution, and the venue
    // reports the ack first, then the fill, as a real venue would.
    ASSERT_TRUE(venue.submit(paper_order("t1", Side::Buy, "60000.20", "2")).is_ok());
    pump(millis(2));

    ASSERT_EQ(sink.events.size(), 4U) << "2 connection + ack + fill";
    EXPECT_EQ(sink.events[2].type, ExecutionEventType::OrderAck);
    EXPECT_EQ(sink.events[3].type, ExecutionEventType::Fill);
    EXPECT_EQ(sink.events[3].payload.fill.liquidity, Liquidity::Taker);
    EXPECT_EQ(sink.events[3].payload.fill.price.to_string(), "60000.1");
}

TEST_F(PaperTest, PostOnlyPolicyExpireReportsExpiryNotRejection) {
    PaperExecutionConfig c = paper_config();
    c.post_only_policy = PostOnlyPolicy::Expire;
    PaperExecution expiring(c, clock);
    RecordingExecutionSink expiring_sink;
    ASSERT_TRUE(expiring.start(expiring_sink).is_ok());
    expiring.attach_book(Symbol("BTCUSDT"), *book);

    ASSERT_TRUE(expiring.submit(paper_order("po", Side::Buy, "60000.20", "1", true)).is_ok());
    clock.advance(millis(1));
    static_cast<void>(expiring.poll());

    const auto* e = expiring_sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.order_status, OrderStatus::Expired);
    EXPECT_EQ(expiring.metrics().expiries, 1U);
}

// ===========================================================================
// §13 — order types
// ===========================================================================

TEST_F(PaperTest, MarketOrdersAreRefusedRatherThanApproximated) {
    OrderRequest r = paper_order("m1", Side::Buy, "60000.00", "1");
    r.type = OrderType::Market;
    // Only LIMIT is justified by the current architecture. Inventing market
    // support would be untested surface pretending to be a feature.
    const Status s = venue.submit(r);
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::FailedPrecondition);
}

// ===========================================================================
// §10 — price-time priority
// ===========================================================================

TEST_F(PaperTest, BetterPricedOrderFillsFirst) {
    ASSERT_TRUE(venue.submit(paper_order("worse", Side::Buy, "59900.00", "2")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("better", Side::Buy, "59950.00", "2")).is_ok());
    pump();

    // Only 2 available; the better-priced order should take it.
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});
    pump();

    EXPECT_EQ(venue.find_order(ClientOrderId("better"))->cumulative_qty.to_string(), "2");
    EXPECT_TRUE(venue.find_order(ClientOrderId("worse"))->cumulative_qty.is_zero());
}

TEST_F(PaperTest, AtTheSamePriceEarlierArrivalFillsFirst) {
    ASSERT_TRUE(venue.submit(paper_order("first", Side::Buy, "59900.00", "2")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("second", Side::Buy, "59900.00", "2")).is_ok());
    pump();

    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});
    pump();

    EXPECT_EQ(venue.find_order(ClientOrderId("first"))->cumulative_qty.to_string(), "2");
    EXPECT_TRUE(venue.find_order(ClientOrderId("second"))->cumulative_qty.is_zero());
}

// ===========================================================================
// §15 — cancel semantics
// ===========================================================================

TEST_F(PaperTest, CancelIsAsynchronousAndConfirmed) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "59999.00", "1")).is_ok());
    pump();

    CancelRequest req;
    ASSERT_TRUE(req.client_order_id.assign("c1"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(venue.cancel(req).is_ok());

    // Still resting: the request is not the result, and the venue has not
    // answered yet.
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::New);
    EXPECT_EQ(sink.count_of(ExecutionEventType::OrderCancel), 0U);

    pump();
    const auto* e = sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::Accepted);
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::Canceled);
}

TEST_F(PaperTest, CancelOfAnUnknownOrderReportsNotFound) {
    CancelRequest req;
    ASSERT_TRUE(req.client_order_id.assign("never"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(venue.cancel(req).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::OrderNotFound);
}

TEST_F(PaperTest, CancelLosesTheRaceWhenTheOrderFillsFirst) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "1")).is_ok());
    pump();

    CancelRequest req;
    ASSERT_TRUE(req.client_order_id.assign("c1"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(venue.cancel(req).is_ok());

    // The market comes to the order while the cancel is still in flight. The
    // venue must not pretend the cancel won just because it was asked first.
    reprice({{"59990.00", "1"}}, {{"60000.00", "5"}});
    pump(millis(2));

    const auto* e = sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::TooLate);
    EXPECT_EQ(e->payload.cancel.order_status, OrderStatus::Filled);
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::Filled);
}

TEST_F(PaperTest, PartialFillDuringCancelStillCancelsTheRemainder) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "10")).is_ok());
    pump();

    CancelRequest req;
    ASSERT_TRUE(req.client_order_id.assign("c1"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(venue.cancel(req).is_ok());
    reprice({{"59990.00", "1"}}, {{"60000.00", "3"}});
    pump(millis(2));

    // Not a lost race: 3 filled, 7 genuinely cancelled, and the cumulative
    // quantity the engine has to reconcile against is reported.
    const auto* e = sink.last_of(ExecutionEventType::OrderCancel);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.cancel.cancel_status, CancelStatus::Accepted);
    EXPECT_EQ(e->payload.cancel.cumulative_qty.to_string(), "3");
    EXPECT_EQ(e->payload.cancel.leaves_qty.to_string(), "7");
}

TEST_F(PaperTest, CancelAllTouchesOnlyRestingOrders) {
    ASSERT_TRUE(venue.submit(paper_order("a", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("b", Side::Sell, "61000.00", "1")).is_ok());
    pump();
    ASSERT_EQ(venue.resting_order_count(), 2U);

    ASSERT_TRUE(venue.cancel_all(Symbol("BTCUSDT")).is_ok());
    pump();
    EXPECT_EQ(venue.resting_order_count(), 0U);
    EXPECT_EQ(venue.metrics().cancel_acks, 2U);
}

// ===========================================================================
// §16 — replace semantics
// ===========================================================================

TEST_F(PaperTest, AtomicReplaceSwapsIdentityAndParameters) {
    ASSERT_TRUE(venue.submit(paper_order("r1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    const ExchangeOrderId original = venue.find_order(ClientOrderId("r1"))->exchange_order_id;

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59500.00", req.new_price));
    ASSERT_TRUE(Qty::parse("2", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderReplace);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->payload.replace.accepted);
    EXPECT_EQ(e->payload.replace.original_exchange_order_id, original);
    EXPECT_NE(e->payload.replace.new_exchange_order_id, original)
        << "a replaced order gets a new venue identity";

    EXPECT_EQ(venue.find_order(ClientOrderId("r1")), nullptr);
    const PaperOrder* order = venue.find_order(ClientOrderId("r2"));
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->price.to_string(), "59500");
    EXPECT_EQ(order->original_qty.to_string(), "2");
}

TEST_F(PaperTest, ReplaceCarriesFilledQuantityForward) {
    ASSERT_TRUE(venue.submit(paper_order("r1", Side::Buy, "60000.00", "10")).is_ok());
    pump();
    reprice({{"59990.00", "1"}}, {{"60000.00", "4"}});
    pump();
    ASSERT_EQ(venue.find_order(ClientOrderId("r1"))->cumulative_qty.to_string(), "4");

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59000.00", req.new_price));
    ASSERT_TRUE(Qty::parse("10", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    // Filled quantity is a fact about the past; an amendment cannot undo it.
    const PaperOrder* order = venue.find_order(ClientOrderId("r2"));
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->cumulative_qty.to_string(), "4");
    EXPECT_EQ(order->remaining().to_string(), "6");
    EXPECT_EQ(order->status, OrderStatus::PartiallyFilled);
}

TEST_F(PaperTest, ReplaceBelowFilledQuantityIsRefused) {
    ASSERT_TRUE(venue.submit(paper_order("r1", Side::Buy, "60000.00", "10")).is_ok());
    pump();
    reprice({{"59990.00", "1"}}, {{"60000.00", "6"}});
    pump();

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59000.00", req.new_price));
    ASSERT_TRUE(Qty::parse("2", req.new_quantity));  // below the 6 already filled
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderReplace);
    ASSERT_NE(e, nullptr);
    EXPECT_FALSE(e->payload.replace.accepted);
    EXPECT_EQ(venue.find_order(ClientOrderId("r1"))->original_qty.to_string(), "10");
}

TEST_F(PaperTest, RepricedReplaceSurrendersQueuePosition) {
    ASSERT_TRUE(venue.submit(paper_order("early", Side::Buy, "59900.00", "2")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("late", Side::Buy, "59900.00", "2")).is_ok());
    pump();
    const std::uint64_t before = venue.find_order(ClientOrderId("early"))->arrival_sequence;

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("early"));
    ASSERT_TRUE(req.new_client_order_id.assign("early2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59950.00", req.new_price));
    ASSERT_TRUE(Qty::parse("2", req.new_quantity));
    ASSERT_TRUE(venue.replace(req).is_ok());
    pump();

    // Moving the price means going to the back of the queue. Keeping priority
    // would model a free improvement no venue offers.
    EXPECT_GT(venue.find_order(ClientOrderId("early2"))->arrival_sequence, before);
}

TEST_F(PaperTest, UnsupportedReplaceIsRefusedNotDecomposed) {
    PaperExecutionConfig c = paper_config();
    c.replace_mode = ReplaceMode::Unsupported;
    PaperExecution novenue(c, clock);
    RecordingExecutionSink s2;
    ASSERT_TRUE(novenue.start(s2).is_ok());
    novenue.attach_book(Symbol("BTCUSDT"), *book);
    EXPECT_FALSE(novenue.capabilities().supports_replace);

    ASSERT_TRUE(novenue.submit(paper_order("r1", Side::Buy, "59000.00", "1")).is_ok());
    clock.advance(millis(1));
    static_cast<void>(novenue.poll());

    ReplaceRequest req;
    ASSERT_TRUE(req.original_client_order_id.assign("r1"));
    ASSERT_TRUE(req.new_client_order_id.assign("r2"));
    req.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("59500.00", req.new_price));
    ASSERT_TRUE(Qty::parse("1", req.new_quantity));

    // Refused synchronously rather than silently decomposed: the two have
    // different exposure profiles and that choice belongs to the quote manager.
    EXPECT_TRUE(novenue.replace(req).is_error());
    EXPECT_EQ(novenue.find_order(ClientOrderId("r1"))->price.to_string(), "59000");
}

// ===========================================================================
// §20 — reconciliation snapshot
// ===========================================================================

TEST_F(PaperTest, OpenOrdersSnapshotCarriesWhatReconciliationNeeds) {
    ASSERT_TRUE(venue.submit(paper_order("s1", Side::Buy, "59000.00", "3")).is_ok());
    pump();
    reprice({{"58900.00", "1"}}, {{"59000.00", "1"}});
    pump();

    ASSERT_TRUE(venue.query_open_orders(Symbol("BTCUSDT")).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OpenOrdersSnapshot);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->payload.open_orders.count, 1U);
    const OrderStatusReport& row = e->payload.open_orders.orders[0];
    EXPECT_EQ(row.client_order_id, ClientOrderId("s1"));
    EXPECT_FALSE(row.exchange_order_id.empty());
    EXPECT_EQ(row.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(row.price.to_string(), "59000");
    EXPECT_EQ(row.original_qty.to_string(), "3");
    EXPECT_EQ(row.cumulative_qty.to_string(), "1");
    EXPECT_TRUE(e->payload.open_orders.is_last);
}

TEST_F(PaperTest, EmptySnapshotStillTerminates) {
    ASSERT_TRUE(venue.query_open_orders(Symbol("BTCUSDT")).is_ok());
    pump();
    const auto* e = sink.last_of(ExecutionEventType::OpenOrdersSnapshot);
    ASSERT_NE(e, nullptr);
    // Reconciliation may only conclude an order is absent after `is_last`, so
    // a last chunk has to exist even when there is nothing to report.
    EXPECT_EQ(e->payload.open_orders.count, 0U);
    EXPECT_TRUE(e->payload.open_orders.is_first);
    EXPECT_TRUE(e->payload.open_orders.is_last);
}

TEST_F(PaperTest, SnapshotIsChunkedLikeAPagedVenueAnswer) {
    for (int i = 0; i < 6; ++i) {
        const std::string id = "s" + std::to_string(i);
        ASSERT_TRUE(venue.submit(paper_order(id.c_str(), Side::Buy,
                                             (std::to_string(58000 + i) + ".00").c_str(), "1"))
                        .is_ok());
    }
    pump();
    ASSERT_TRUE(venue.query_open_orders(Symbol("BTCUSDT")).is_ok());
    pump();

    EXPECT_EQ(sink.count_of(ExecutionEventType::OpenOrdersSnapshot), 2U);
    const auto* first_chunk = sink.first_of(ExecutionEventType::OpenOrdersSnapshot);
    const auto* last_chunk = sink.last_of(ExecutionEventType::OpenOrdersSnapshot);
    ASSERT_NE(first_chunk, nullptr);
    ASSERT_NE(last_chunk, nullptr);
    EXPECT_TRUE(first_chunk->payload.open_orders.is_first);
    EXPECT_FALSE(first_chunk->payload.open_orders.is_last);
    EXPECT_TRUE(last_chunk->payload.open_orders.is_last);
}

TEST_F(PaperTest, QueryOrderAnswersWithStatus) {
    ASSERT_TRUE(venue.submit(paper_order("q1", Side::Buy, "59000.00", "1")).is_ok());
    pump();
    ASSERT_TRUE(venue.query_order(ClientOrderId("q1"), ExchangeOrderId()).is_ok());
    pump();

    const auto* e = sink.last_of(ExecutionEventType::OrderStatus);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.order_status.status, OrderStatus::New);
    EXPECT_EQ(e->payload.order_status.price.to_string(), "59000");
}

// ===========================================================================
// §30 — stale market
// ===========================================================================

TEST_F(PaperTest, AStaleBookProducesNoFills) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "5")).is_ok());
    pump();

    // The book now says we are executable, but the data is older than the
    // configured tolerance. Inventing a fill from data we already know is
    // stale would put a position on the books that never existed anywhere.
    test::load_book(*book, {{"59990.00", "1"}}, {{"60000.00", "5"}}, ++book_seq);
    clock.advance(seconds(2));
    venue.on_market_update(Symbol("BTCUSDT"), clock.steady() - seconds(2));
    pump();

    EXPECT_EQ(sink.count_of(ExecutionEventType::Fill), 0U);
    EXPECT_GE(venue.metrics().stale_book_skips, 1U);
    EXPECT_TRUE(venue.find_order(ClientOrderId("c1"))->cumulative_qty.is_zero());
}

TEST_F(PaperTest, StaleMarketDoesNotBlockCancellation) {
    ASSERT_TRUE(venue.submit(paper_order("c1", Side::Buy, "60000.00", "5")).is_ok());
    pump();
    clock.advance(seconds(2));
    venue.on_market_update(Symbol("BTCUSDT"), clock.steady() - seconds(2));

    // Gating cancellation on fresh data would trap exposure exactly when
    // releasing it matters most.
    CancelRequest req;
    ASSERT_TRUE(req.client_order_id.assign("c1"));
    req.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(venue.cancel(req).is_ok());
    pump();
    EXPECT_EQ(venue.find_order(ClientOrderId("c1"))->status, OrderStatus::Canceled);
}

// ===========================================================================
// §31 — determinism
// ===========================================================================

TEST_F(PaperTest, SameInputsProduceTheSameEventSequence) {
    const auto run = []() {
        ManualClock run_clock(millis(10'000), millis(10'000));
        RecordingExecutionSink run_sink;
        PaperExecution run_venue(paper_config(), run_clock);
        auto run_book =
            test::make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});
        EXPECT_TRUE(run_venue.start(run_sink).is_ok());
        run_venue.attach_book(Symbol("BTCUSDT"), *run_book);
        run_venue.on_market_update(Symbol("BTCUSDT"), run_clock.steady());

        Seq seq = 100;
        static_cast<void>(run_venue.submit(paper_order("d1", Side::Buy, "60000.00", "4")));
        static_cast<void>(run_venue.submit(paper_order("d2", Side::Sell, "60000.10", "4")));
        run_clock.advance(millis(1));
        static_cast<void>(run_venue.poll());

        test::load_book(*run_book, {{"59999.00", "2"}}, {{"60000.00", "2"}}, ++seq);
        run_venue.on_market_update(Symbol("BTCUSDT"), run_clock.steady());
        run_clock.advance(millis(1));
        static_cast<void>(run_venue.poll());

        CancelRequest req;
        EXPECT_TRUE(req.client_order_id.assign("d2"));
        req.symbol = Symbol("BTCUSDT");
        static_cast<void>(run_venue.cancel(req));
        run_clock.advance(millis(2));
        static_cast<void>(run_venue.poll());
        return test::transcript(run_sink);
    };

    const auto a = run();
    const auto b = run();
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b) << "the same inputs must produce the same events, every time";
}

TEST_F(PaperTest, EqualDueTimesResolveByEmissionOrder) {
    // Two requests in the same instant. Their answers are due at the same
    // nanosecond, and the order must still be the order they were made in --
    // not whatever a container happens to hold.
    ASSERT_TRUE(venue.submit(paper_order("first", Side::Buy, "59000.00", "1")).is_ok());
    ASSERT_TRUE(venue.submit(paper_order("second", Side::Buy, "58000.00", "1")).is_ok());
    pump();

    ASSERT_GE(sink.events.size(), 4U);
    EXPECT_EQ(sink.events[2].payload.ack.client_order_id, ClientOrderId("first"));
    EXPECT_EQ(sink.events[3].payload.ack.client_order_id, ClientOrderId("second"));
}

// ===========================================================================
// §38 — configuration
// ===========================================================================

TEST_F(PaperTest, ConfigIsTranslatedFromYamlWithoutSilentSubstitution) {
    PaperConfig yaml;
    yaml.fill_model = "full_on_cross";
    yaml.post_only_policy = "expire";
    yaml.replace_mode = "unsupported";
    yaml.queue_share_bps = 2'500;
    yaml.ack_latency_us = 3'000;
    yaml.max_book_age_ms = 250;

    const auto built = paper_config_from(yaml);
    ASSERT_TRUE(built.is_ok()) << built.status().to_string();
    EXPECT_EQ(built.value().fill_model, FillModel::FullOnCross);
    EXPECT_EQ(built.value().post_only_policy, PostOnlyPolicy::Expire);
    EXPECT_EQ(built.value().replace_mode, ReplaceMode::Unsupported);
    EXPECT_EQ(built.value().queue_share_bps, 2'500U);
    EXPECT_EQ(built.value().latency.ack_ns, micros(3'000));
    EXPECT_EQ(built.value().max_book_age_ns, millis(250));
}

TEST_F(PaperTest, AnUnknownFillModelIsRefusedRatherThanDefaulted) {
    PaperConfig yaml;
    yaml.fill_model = "optimistic";
    const auto built = paper_config_from(yaml);
    // A paper run whose fill model was quietly not the one asked for is worse
    // than one that refused to start.
    ASSERT_TRUE(built.is_error());
    EXPECT_NE(built.status().message().find("fill_model"), std::string::npos);
}

TEST_F(PaperTest, DefaultConfigIsConservativeAndDeterministic) {
    const auto built = paper_config_from(PaperConfig{});
    ASSERT_TRUE(built.is_ok());
    EXPECT_EQ(built.value().fill_model, FillModel::DisplayedLiquidity)
        << "the conservative model must be what you get without asking";
    EXPECT_EQ(built.value().post_only_policy, PostOnlyPolicy::Reject);
    EXPECT_EQ(built.value().deterministic_seed, 0U);
    EXPECT_FALSE(built.value().faults.any())
        << "failure injection is a testing instrument, never configuration";
}

TEST_F(PaperTest, ShippedPaperConfigBuildsAWorkingVenue) {
    const auto loaded = load_config_file(std::string(MMX_CONFIG_DIR) + "/paper.yaml");
    ASSERT_TRUE(loaded.is_ok()) << loaded.status().to_string();
    const auto built = paper_config_from(loaded.value().paper);
    ASSERT_TRUE(built.is_ok()) << built.status().to_string();

    PaperExecution shipped(built.value(), clock);
    RecordingExecutionSink s2;
    ASSERT_TRUE(shipped.start(s2).is_ok());
    shipped.attach_book(Symbol("BTCUSDT"), *book);
    ASSERT_TRUE(shipped.submit(paper_order("c1", Side::Buy, "59000.00", "1")).is_ok());
    clock.advance(millis(5));
    static_cast<void>(shipped.poll());
    EXPECT_EQ(s2.count_of(ExecutionEventType::OrderAck), 1U);
}

}  // namespace
}  // namespace mm::exchange::paper
