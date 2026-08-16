/// \file test_feed_to_book.cpp
/// End-to-end market data: adapter -> sink -> synchronizer -> book.
///
/// Uses the mock adapter rather than Binance so the whole path is deterministic
/// and offline. What it proves is that the pieces compose through the real
/// normalized interfaces — the same plumbing the Binance adapter uses — rather
/// than only working when driven directly in a unit test.

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "mm/exchange/binance/BinanceCodec.hpp"
#include "mm/exchange/mock/MockExchangeMarketData.hpp"
#include "mm/orderbook/BookSynchronizer.hpp"
#include "support/BookFixtures.hpp"

namespace mm {
namespace {

using exchange::MarketDataEventType;
using exchange::SessionState;

/// Routes normalized events into a per-symbol synchronizer, which is exactly
/// what the engine's sink does once the events have crossed the ring.
class SynchronizingSink final : public exchange::IMarketDataSink {
public:
    SynchronizingSink(const Clock& clock, book::SyncConfig config)
        : clock_(clock), config_(config) {}

    book::BookSynchronizer& track(const Symbol& symbol) {
        auto [it, inserted] = syncs_.try_emplace(
            symbol, std::make_unique<book::BookSynchronizer>(symbol, config_, clock_));
        return *it->second;
    }

    void on_market_data(const exchange::MarketDataEvent& event) override {
        ++events;
        const auto it = syncs_.find(event.symbol());
        if (it == syncs_.end()) {
            return;
        }
        book::BookSynchronizer& sync = *it->second;
        switch (event.type) {
            case MarketDataEventType::BookSnapshot:
                static_cast<void>(sync.on_snapshot(event.payload.book_snapshot));
                break;
            case MarketDataEventType::BookUpdate:
                static_cast<void>(sync.on_update(event.payload.book_update));
                break;
            case MarketDataEventType::Trade:
                ++trades;
                break;
            default:
                break;
        }
    }

    [[nodiscard]] book::BookSynchronizer& sync_for(const Symbol& s) { return *syncs_.at(s); }

    std::size_t events = 0;
    std::size_t trades = 0;

private:
    const Clock& clock_;
    book::SyncConfig config_;
    std::map<Symbol, std::unique_ptr<book::BookSynchronizer>> syncs_;
};

class FeedToBookTest : public ::testing::Test {
protected:
    FeedToBookTest() : clock(millis(1'000), 0), venue(clock), sink(clock, make_config()) {}

    static book::SyncConfig make_config() {
        book::SyncConfig c;
        c.max_depth = 20;
        c.max_data_age_ns = millis(500);
        return c;
    }

    void subscribe(const char* symbol) {
        exchange::SubscriptionRequest request;
        request.symbol = Symbol(symbol);
        ASSERT_TRUE(venue.subscribe(request).is_ok());
        book::BookSynchronizer& sync = sink.track(Symbol(symbol));
        sync.on_connected();
        sync.on_subscribed();
    }

    ManualClock clock;
    exchange::mock::MockExchangeMarketData venue;
    SynchronizingSink sink;
};

TEST_F(FeedToBookTest, FeedDrivesABookToReady) {
    ASSERT_TRUE(venue.start(sink).is_ok());
    subscribe("BTCUSDT");
    const Symbol btc("BTCUSDT");

    // Updates arrive before the snapshot, as they do in production.
    ASSERT_TRUE(venue.publish_update(btc, 101, 105, {test::level("100", "5")},
                                     {test::level("101", "4")}).is_ok());
    EXPECT_FALSE(sink.sync_for(btc).is_quotable());

    ASSERT_TRUE(venue.publish_snapshot(btc, 100, {test::level("100", "1")},
                                       {test::level("101", "1")}).is_ok());
    EXPECT_TRUE(sink.sync_for(btc).is_quotable());

    const book::OrderBook& b = sink.sync_for(btc).book();
    EXPECT_EQ(b.best_bid_price().to_string(), "100");
    EXPECT_EQ(b.best_bid_qty().to_string(), "5") << "the buffered update must have been applied";
    EXPECT_EQ(b.check_invariants(), book::BookViolation::None);
}

TEST_F(FeedToBookTest, TradesFlowAlongsideDepthWithoutDisturbingTheBook) {
    ASSERT_TRUE(venue.start(sink).is_ok());
    subscribe("BTCUSDT");
    const Symbol btc("BTCUSDT");
    ASSERT_TRUE(venue.publish_snapshot(btc, 100, {test::level("100", "1")},
                                       {test::level("101", "1")}).is_ok());
    ASSERT_TRUE(sink.sync_for(btc).is_quotable());

    ASSERT_TRUE(venue.publish_trade(btc, Px::from_units(100), Qty::from_units(1), Side::Sell,
                                    TradeId("T1")).is_ok());
    EXPECT_EQ(sink.trades, 1U);
    EXPECT_TRUE(sink.sync_for(btc).is_quotable());
    EXPECT_EQ(sink.sync_for(btc).book().best_bid_price().to_string(), "100");
}

// §15: one symbol losing synchronization must not disturb another.
TEST_F(FeedToBookTest, SymbolsAreIsolated) {
    ASSERT_TRUE(venue.start(sink).is_ok());
    subscribe("BTCUSDT");
    subscribe("ETHUSDT");
    const Symbol btc("BTCUSDT");
    const Symbol eth("ETHUSDT");

    ASSERT_TRUE(venue.publish_snapshot(btc, 100, {test::level("100", "1")},
                                       {test::level("101", "1")}).is_ok());
    ASSERT_TRUE(venue.publish_snapshot(eth, 200, {test::level("50", "9")},
                                       {test::level("51", "9")}).is_ok());
    ASSERT_TRUE(sink.sync_for(btc).is_quotable());
    ASSERT_TRUE(sink.sync_for(eth).is_quotable());

    // A gap on BTC only.
    ASSERT_TRUE(venue.publish_update(btc, 500, 505, {test::level("100", "2")}, {}).is_ok());

    EXPECT_EQ(sink.sync_for(btc).state(), SessionState::ResyncRequired);
    EXPECT_FALSE(sink.sync_for(btc).book().is_valid());

    EXPECT_TRUE(sink.sync_for(eth).is_quotable()) << "ETH must be untouched by BTC's gap";
    EXPECT_EQ(sink.sync_for(eth).book().best_bid_price().to_string(), "50");

    // And ETH keeps working.
    ASSERT_TRUE(venue.publish_update(eth, 201, 205, {test::level("50", "7")}, {}).is_ok());
    EXPECT_TRUE(sink.sync_for(eth).is_quotable());
    EXPECT_EQ(sink.sync_for(eth).book().best_bid_qty().to_string(), "7");
}

TEST_F(FeedToBookTest, StalenessIsPerSymbol) {
    ASSERT_TRUE(venue.start(sink).is_ok());
    subscribe("BTCUSDT");
    subscribe("ETHUSDT");
    const Symbol btc("BTCUSDT");
    const Symbol eth("ETHUSDT");
    ASSERT_TRUE(venue.publish_snapshot(btc, 100, {test::level("100", "1")},
                                       {test::level("101", "1")}).is_ok());
    ASSERT_TRUE(venue.publish_snapshot(eth, 200, {test::level("50", "9")},
                                       {test::level("51", "9")}).is_ok());

    clock.advance(millis(600));
    // ETH keeps producing data; BTC goes quiet.
    ASSERT_TRUE(venue.publish_update(eth, 201, 205, {test::level("50", "8")}, {}).is_ok());
    sink.sync_for(btc).on_timer();
    sink.sync_for(eth).on_timer();

    EXPECT_EQ(sink.sync_for(btc).state(), SessionState::Stale);
    EXPECT_TRUE(sink.sync_for(eth).is_quotable());
}

/// The Binance codec's output feeds the same synchronizer, which is what makes
/// the offline synchronization tests meaningful for the real adapter.
TEST_F(FeedToBookTest, BinanceCodecOutputDrivesTheSameSynchronizer) {
    exchange::binance::BinanceCodec codec;
    std::vector<exchange::MarketDataEvent> events;

    book::SyncConfig config = make_config();
    book::BookSynchronizer sync(Symbol("BTCUSDT"), config, clock);
    sync.on_connected();
    sync.on_subscribed();

    // A real depth diff, then a real REST snapshot, both as the venue sends them.
    ASSERT_TRUE(codec
                    .decode_stream_frame(
                        R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":101,"u":105,)"
                        R"("b":[["63120.73000000","0.50000000"]],)"
                        R"("a":[["63120.74000000","1.00000000"]]})",
                        1'000, events)
                    .is_ok());
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(sync.on_update(events[0].payload.book_update).is_ok());
    EXPECT_FALSE(sync.is_quotable());

    events.clear();
    ASSERT_TRUE(codec
                    .decode_depth_snapshot(
                        R"({"lastUpdateId":100,)"
                        R"("bids":[["63120.70000000","0.10000000"]],)"
                        R"("asks":[["63120.80000000","0.10000000"]]})",
                        Symbol("BTCUSDT"), 2'000, events)
                    .is_ok());
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(sync.on_snapshot(events[0].payload.book_snapshot).is_ok());

    ASSERT_TRUE(sync.is_quotable());
    const book::OrderBook& b = sync.book();
    // Snapshot seeded 63120.70/63120.80; the buffered diff then tightened both.
    EXPECT_EQ(b.best_bid_price().to_string(), "63120.73");
    EXPECT_EQ(b.best_ask_price().to_string(), "63120.74");
    EXPECT_EQ(b.spread().to_string(), "0.01");
    EXPECT_EQ(b.check_invariants(), book::BookViolation::None);
}

}  // namespace
}  // namespace mm
