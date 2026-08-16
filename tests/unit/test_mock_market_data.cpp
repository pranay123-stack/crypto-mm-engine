#include <gtest/gtest.h>

#include "mm/exchange/mock/MockExchangeExecution.hpp"  // capability presets
#include "mm/exchange/mock/MockExchangeMarketData.hpp"
#include "support/RecordingSinks.hpp"

namespace mm::exchange::mock {
namespace {

using test::RecordingMarketDataSink;

PriceLevel level(const char* px, const char* qty) {
    PriceLevel l;
    EXPECT_TRUE(Px::parse(px, l.price));
    EXPECT_TRUE(Qty::parse(qty, l.quantity));
    return l;
}

class MockMarketDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock.set_steady(millis(1'000));
        ASSERT_TRUE(venue.start(sink).is_ok());
        SubscriptionRequest req;
        req.symbol = Symbol("BTCUSDT");
        ASSERT_TRUE(venue.subscribe(req).is_ok());
        sink.clear();
    }

    ManualClock clock{0, 0};
    MockExchangeMarketData venue{clock};
    RecordingMarketDataSink sink;
    const Symbol btc{"BTCUSDT"};
};

TEST_F(MockMarketDataTest, SubscriptionWalksTheDocumentedPath) {
    RecordingMarketDataSink s;
    ManualClock c{0, 0};
    MockExchangeMarketData v(c);
    ASSERT_TRUE(v.start(s).is_ok());

    SubscriptionRequest req;
    req.symbol = Symbol("ETHUSDT");
    ASSERT_TRUE(v.subscribe(req).is_ok());

    std::vector<SessionState> path;
    for (const auto& e : s.events) {
        if (e.type == MarketDataEventType::SessionState) {
            path.push_back(e.payload.session.to);
        }
    }
    // Nothing skips a step, including the mock itself.
    ASSERT_EQ(path.size(), 3U);
    EXPECT_EQ(path[0], SessionState::Connecting);
    EXPECT_EQ(path[1], SessionState::Connected);
    EXPECT_EQ(path[2], SessionState::Subscribed);
    EXPECT_EQ(v.session_state(Symbol("ETHUSDT")), SessionState::Subscribed);
    EXPECT_FALSE(is_quotable(v.session_state(Symbol("ETHUSDT"))));
}

TEST_F(MockMarketDataTest, IllegalTransitionIsRefusedNotApplied) {
    // Subscribed -> Ready would let a strategy quote against a book that has
    // never had a snapshot applied.
    const Status s = venue.set_session_state(btc, SessionState::Ready);
    EXPECT_TRUE(s.is_error());
    EXPECT_NE(s.message().find("illegal session transition"), std::string_view::npos);
    EXPECT_EQ(venue.session_state(btc), SessionState::Subscribed) << "state must be unchanged";
}

TEST_F(MockMarketDataTest, LegalPathToReady) {
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Syncing).is_ok());
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Ready).is_ok());
    EXPECT_TRUE(is_quotable(venue.session_state(btc)));
}

TEST_F(MockMarketDataTest, ResyncMustPassThroughSyncingAgain) {
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Syncing).is_ok());
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Ready).is_ok());
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::ResyncRequired).is_ok());

    EXPECT_TRUE(venue.set_session_state(btc, SessionState::Ready).is_error());
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Syncing).is_ok());
    EXPECT_TRUE(venue.set_session_state(btc, SessionState::Ready).is_ok());
}

TEST_F(MockMarketDataTest, SessionEventCarriesBothEndpoints) {
    ASSERT_TRUE(venue.set_session_state(btc, SessionState::Syncing, "snapshot applied").is_ok());
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::SessionState);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.session.from, SessionState::Subscribed);
    EXPECT_EQ(e->payload.session.to, SessionState::Syncing);
    EXPECT_EQ(e->payload.session.reason.view(), "snapshot applied");
}

// ---------------------------------------------------------------------------
// Chunking
// ---------------------------------------------------------------------------

TEST_F(MockMarketDataTest, SmallSnapshotIsASingleTerminatedChunk) {
    std::vector<PriceLevel> bids{level("60000.00", "1"), level("59999.99", "2")};
    std::vector<PriceLevel> asks{level("60000.01", "1")};
    ASSERT_TRUE(venue.publish_snapshot(btc, 100, bids, asks).is_ok());

    ASSERT_EQ(sink.count_of(MarketDataEventType::BookSnapshot), 1U);
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::BookSnapshot);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->payload.book_snapshot.is_first);
    EXPECT_TRUE(e->payload.book_snapshot.is_last);
    EXPECT_EQ(e->payload.book_snapshot.levels.bid_count, 2);
    EXPECT_EQ(e->payload.book_snapshot.levels.ask_count, 1);
    EXPECT_EQ(e->payload.book_snapshot.last_update_id, 100U);
}

TEST_F(MockMarketDataTest, LargeSnapshotIsChunkedWithoutLosingALevel) {
    // 40 levels a side against a 16-level event: three chunks, no truncation.
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    for (int i = 0; i < 40; ++i) {
        bids.push_back(level(("60000." + std::to_string(99 - i)).c_str(), "1"));
        asks.push_back(level(("60100." + std::to_string(i)).c_str(), "1"));
    }
    ASSERT_TRUE(venue.publish_snapshot(btc, 500, bids, asks).is_ok());

    std::size_t chunks = 0, total_bids = 0, total_asks = 0, firsts = 0, lasts = 0;
    for (const auto& e : sink.events) {
        if (e.type != MarketDataEventType::BookSnapshot) {
            continue;
        }
        const BookSnapshotEvent& s = e.payload.book_snapshot;
        ASSERT_TRUE(s.levels.counts_are_sane());
        ++chunks;
        total_bids += s.levels.bid_count;
        total_asks += s.levels.ask_count;
        firsts += s.is_first ? 1 : 0;
        lasts += s.is_last ? 1 : 0;
    }
    EXPECT_EQ(chunks, 3U);
    EXPECT_EQ(total_bids, 40U) << "chunking must not lose a level";
    EXPECT_EQ(total_asks, 40U);
    EXPECT_EQ(firsts, 1U);
    EXPECT_EQ(lasts, 1U) << "exactly one chunk terminates the run";
}

TEST_F(MockMarketDataTest, ChunkedUpdateCarriesTheSameSequenceRange) {
    std::vector<PriceLevel> bids;
    for (int i = 0; i < 20; ++i) {
        bids.push_back(level(("60000." + std::to_string(10 + i)).c_str(), "1"));
    }
    ASSERT_TRUE(venue.publish_update(btc, 101, 120, bids, {}).is_ok());

    std::size_t chunks = 0;
    for (const auto& e : sink.events) {
        if (e.type != MarketDataEventType::BookUpdate) {
            continue;
        }
        ++chunks;
        // Every chunk of one logical update reports the same range, so the
        // consumer can apply them all before publishing the book.
        EXPECT_EQ(e.payload.book_update.first_update_id, 101U);
        EXPECT_EQ(e.payload.book_update.final_update_id, 120U);
    }
    EXPECT_EQ(chunks, 2U);
}

TEST_F(MockMarketDataTest, UpdateReportsThePreviousFinalIdForGapDetection) {
    ASSERT_TRUE(venue.publish_snapshot(btc, 100, {level("60000.00", "1")}, {}).is_ok());
    ASSERT_TRUE(venue.publish_update(btc, 101, 105, {level("60000.00", "2")}, {}).is_ok());
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::BookUpdate);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.book_update.prev_final_update_id, 100U);
}

TEST_F(MockMarketDataTest, ZeroQuantityLevelIsADeletion) {
    const PriceLevel deletion = level("60000.00", "0");
    EXPECT_TRUE(deletion.is_deletion());
    ASSERT_TRUE(venue.publish_update(btc, 101, 101, {deletion}, {}).is_ok());
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::BookUpdate);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->payload.book_update.levels.bids[0].is_deletion());
}

// ---------------------------------------------------------------------------
// Trades, BBO, staleness
// ---------------------------------------------------------------------------

TEST_F(MockMarketDataTest, TradeCarriesTheAggressorSide) {
    Px px;
    Qty qty;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.5", qty));
    ASSERT_TRUE(venue.publish_trade(btc, px, qty, Side::Sell, TradeId("T1")).is_ok());

    const MarketDataEvent* e = sink.last_of(MarketDataEventType::Trade);
    ASSERT_NE(e, nullptr);
    // Getting this backwards inverts every order-flow signal built on it.
    EXPECT_EQ(e->payload.trade.aggressor, Side::Sell);
    EXPECT_EQ(e->payload.trade.trade_id, TradeId("T1"));
}

TEST_F(MockMarketDataTest, BboFlagsWhetherItIsNativeOrDerived) {
    BestBidAsk bbo;
    ASSERT_TRUE(Px::parse("60000.00", bbo.bid_px));
    ASSERT_TRUE(Qty::parse("1", bbo.bid_qty));
    ASSERT_TRUE(Px::parse("60000.01", bbo.ask_px));
    ASSERT_TRUE(Qty::parse("1", bbo.ask_qty));
    ASSERT_TRUE(bbo.is_sane());

    ASSERT_TRUE(venue.publish_bbo(btc, bbo, /*native=*/true).is_ok());
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::Bbo);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->payload.bbo.is_native);
    // A derived BBO is only as fresh as the last depth update, which is why the
    // distinction is carried rather than assumed.
    EXPECT_TRUE(e->payload.bbo.bbo.is_sane());
}

TEST_F(MockMarketDataTest, NativeBboIsRefusedWhereUnsupported) {
    ManualClock c{0, 0};
    MockExchangeMarketData v(c, minimal_mock_capabilities());
    RecordingMarketDataSink s;
    ASSERT_TRUE(v.start(s).is_ok());
    SubscriptionRequest req;
    req.symbol = btc;
    ASSERT_TRUE(v.subscribe(req).is_ok());
    EXPECT_TRUE(v.publish_bbo(btc, BestBidAsk{}, /*native=*/true).is_error());
}

TEST_F(MockMarketDataTest, ASymbolThatHasNeverProducedDataIsMaximallyStale) {
    // Returning zero here would let a silent symbol look perfectly healthy.
    EXPECT_EQ(venue.data_age_ns(btc), std::numeric_limits<Nanos>::max());
    EXPECT_EQ(venue.data_age_ns(Symbol("NOSUCH")), std::numeric_limits<Nanos>::max());
}

TEST_F(MockMarketDataTest, DataAgeGrowsWithTheSteadyClock) {
    ASSERT_TRUE(venue.publish_trade(btc, Px::from_units(1), Qty::from_units(1), Side::Buy,
                                    TradeId("T1")).is_ok());
    EXPECT_EQ(venue.data_age_ns(btc), 0);
    clock.advance(millis(750));
    EXPECT_EQ(venue.data_age_ns(btc), millis(750));
}

TEST_F(MockMarketDataTest, PublishingToAnUnsubscribedSymbolIsRefused) {
    EXPECT_TRUE(venue.publish_trade(Symbol("NOSUCH"), Px::from_units(1), Qty::from_units(1),
                                    Side::Buy, TradeId("T1")).is_error());
    EXPECT_TRUE(venue.publish_snapshot(Symbol("NOSUCH"), 1, {}, {}).is_error());
}

TEST_F(MockMarketDataTest, UnsubscribeEndsTheSession) {
    ASSERT_TRUE(venue.unsubscribe(btc).is_ok());
    EXPECT_FALSE(venue.is_subscribed(btc));
    EXPECT_EQ(venue.session_state(btc), SessionState::Disconnected);
    EXPECT_TRUE(venue.unsubscribe(btc).is_error());
}

TEST_F(MockMarketDataTest, InstrumentSpecIsPublished) {
    InstrumentSpec spec;
    spec.symbol = btc;
    ASSERT_TRUE(Px::parse("0.01", spec.tick_size));
    ASSERT_TRUE(Qty::parse("0.00001", spec.lot_size));
    spec.status = MarketStatus::Trading;
    venue.set_instrument(spec);

    const MarketDataEvent* e = sink.last_of(MarketDataEventType::InstrumentUpdate);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->payload.instrument.spec.symbol, btc);
    EXPECT_TRUE(e->payload.instrument.spec.is_valid());
}

TEST_F(MockMarketDataTest, SnapshotRequestsAreCounted) {
    EXPECT_EQ(venue.snapshot_requests(), 0U);
    ASSERT_TRUE(venue.request_snapshot(btc).is_ok());
    EXPECT_EQ(venue.snapshot_requests(), 1U);
    EXPECT_TRUE(venue.request_snapshot(Symbol("NOSUCH")).is_error());
}

TEST_F(MockMarketDataTest, EnvelopeAccessorsResolveSymbolAndSequence) {
    ASSERT_TRUE(venue.publish_snapshot(btc, 777, {level("60000.00", "1")}, {}).is_ok());
    const MarketDataEvent* e = sink.last_of(MarketDataEventType::BookSnapshot);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->symbol(), btc);
    EXPECT_EQ(e->sequence(), 777U);

    // Connection events are venue-wide and carry neither.
    venue.set_connection_state(ConnectionState::Reconnecting);
    const MarketDataEvent* c = sink.last_of(MarketDataEventType::Connection);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->symbol().empty());
    EXPECT_EQ(c->sequence(), kNoSeq);
}

TEST_F(MockMarketDataTest, IsDeterministicAcrossRuns) {
    const auto run = [] {
        ManualClock c{0, 0};
        MockExchangeMarketData v(c);
        RecordingMarketDataSink s;
        EXPECT_TRUE(v.start(s).is_ok());
        SubscriptionRequest req;
        req.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(v.subscribe(req).is_ok());
        std::vector<PriceLevel> bids;
        for (int i = 0; i < 25; ++i) {
            PriceLevel l;
            EXPECT_TRUE(Px::parse(("60000." + std::to_string(i)).c_str(), l.price));
            EXPECT_TRUE(Qty::parse("1", l.quantity));
            bids.push_back(l);
        }
        EXPECT_TRUE(v.publish_snapshot(Symbol("BTCUSDT"), 10, bids, {}).is_ok());
        std::string trace;
        for (const auto& e : s.events) {
            trace += to_string(e.type);
            trace += ':';
            trace += std::to_string(e.sequence());
            trace += ';';
        }
        return trace;
    };
    EXPECT_EQ(run(), run());
}

}  // namespace
}  // namespace mm::exchange::mock
