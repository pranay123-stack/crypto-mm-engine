#pragma once

/// Builders for paper-execution tests. Every book here is constructed through
/// the real `book::OrderBook` snapshot path, so the paper venue is reading
/// exactly the representation the engine reads (§29).

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "mm/exchange/paper/PaperExecution.hpp"
#include "support/RecordingSinks.hpp"
#include "mm/orderbook/OrderBook.hpp"

namespace mm::test {

using exchange::paper::PaperExecution;
using exchange::paper::PaperExecutionConfig;

/// A comparable transcript of what a venue emitted — what the determinism and
/// replay tests assert on. Deliberately excludes wall-clock stamps: a
/// transcript that varied with the clock could never be compared across runs.
///
/// A free function over the shared `RecordingExecutionSink` rather than a
/// method on a second sink type. An earlier revision defined its own
/// `mm::test::RecordingExecutionSink` here, which collided with the one in
/// RecordingSinks.hpp: same name, same namespace, different layout. That is an
/// ODR violation, and the linker resolved it by running one class's destructor
/// over the other's object — a heap-use-after-free that only ASan caught.
[[nodiscard]] inline std::vector<std::string> transcript(const RecordingExecutionSink& sink) {
    std::vector<std::string> out;
    out.reserve(sink.events.size());
    for (const auto& e : sink.events) {
        std::string line(to_string(e.type));
        line += "|" + e.client_order_id().to_string();
        switch (e.type) {
            case exchange::ExecutionEventType::OrderAck:
                line += "|" + e.payload.ack.exchange_order_id.to_string() + "|" +
                        e.payload.ack.price.to_string() + "|" +
                        e.payload.ack.original_qty.to_string();
                break;
            case exchange::ExecutionEventType::Fill:
                line += "|" + e.payload.fill.trade_id.to_string() + "|" +
                        e.payload.fill.price.to_string() + "|" +
                        e.payload.fill.quantity.to_string() + "|" +
                        e.payload.fill.cumulative_qty.to_string();
                break;
            case exchange::ExecutionEventType::OrderCancel:
                line += std::string("|") +
                        std::string(to_string(e.payload.cancel.cancel_status)) + "|" +
                        std::string(to_string(e.payload.cancel.order_status));
                break;
            case exchange::ExecutionEventType::OrderReplace:
                line += std::string("|") + (e.payload.replace.accepted ? "OK" : "NO") + "|" +
                        e.payload.replace.new_price.to_string();
                break;
            case exchange::ExecutionEventType::OrderReject:
                line += std::string("|") +
                        std::string(to_string(e.payload.reject.error.reason));
                break;
            case exchange::ExecutionEventType::Connection:
                line += std::string("|") + std::string(to_string(e.payload.connection.to));
                break;
            default:
                break;
        }
        out.push_back(std::move(line));
    }
    return out;
}

/// Loads a two-sided image into an existing book through the real snapshot
/// path. Reused for repricing so the book object -- and therefore the pointer
/// the paper venue holds -- stays alive and stable.
inline void load_book(book::OrderBook& b,
                      const std::vector<std::pair<const char*, const char*>>& bids,
                      const std::vector<std::pair<const char*, const char*>>& asks,
                      Seq update_id) {
    exchange::BookLevels levels;
    for (const auto& [px, qty] : bids) {
        exchange::PriceLevel level;
        EXPECT_TRUE(Px::parse(px, level.price));
        EXPECT_TRUE(Qty::parse(qty, level.quantity));
        levels.bids[levels.bid_count++] = level;
    }
    for (const auto& [px, qty] : asks) {
        exchange::PriceLevel level;
        EXPECT_TRUE(Px::parse(px, level.price));
        EXPECT_TRUE(Qty::parse(qty, level.quantity));
        levels.asks[levels.ask_count++] = level;
    }
    b.begin_snapshot();
    EXPECT_TRUE(b.add_snapshot_levels(levels).is_ok());
    const Status fin = b.finish_snapshot(update_id);
    EXPECT_TRUE(fin.is_ok()) << fin.to_string() << " seq=" << update_id;
}

inline std::unique_ptr<book::OrderBook> make_book(
    const Symbol& symbol, const std::vector<std::pair<const char*, const char*>>& bids,
    const std::vector<std::pair<const char*, const char*>>& asks, Seq update_id = 100) {
    auto b = std::make_unique<book::OrderBook>(symbol, 32);
    load_book(*b, bids, asks, update_id);
    return b;
}

inline PaperExecutionConfig paper_config() {
    PaperExecutionConfig c;
    // Whole-microsecond latencies so a test can advance the clock by an exact
    // amount and know precisely which events are due.
    c.latency.request_ns = micros(500);
    c.latency.ack_ns = micros(500);
    c.latency.cancel_ns = micros(500);
    c.latency.replace_ns = micros(500);
    c.latency.fill_ns = micros(500);
    c.latency.query_ns = micros(500);
    // Full displayed size available, so fill quantities in tests are the ones
    // the test wrote rather than a queue approximation of them. Tests that care
    // about the queue model set it explicitly.
    c.queue_share_bps = 10'000;
    return c;
}

inline exchange::OrderRequest paper_order(const char* client_id, Side side, const char* price,
                                          const char* quantity, bool post_only = false) {
    exchange::OrderRequest r;
    EXPECT_TRUE(r.client_order_id.assign(client_id));
    r.symbol = Symbol("BTCUSDT");
    r.side = side;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    EXPECT_TRUE(Px::parse(price, r.price));
    EXPECT_TRUE(Qty::parse(quantity, r.quantity));
    r.post_only = post_only;
    return r;
}

}  // namespace mm::test
