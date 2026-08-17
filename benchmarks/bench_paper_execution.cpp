/// Paper execution benchmarks (Phase 9 §34).
///
/// **Paper execution performance is not equivalent to real exchange latency,
/// and nothing here should be read as a venue measurement.** What these numbers
/// bound is the engine-side cost of running against the simulator: how much of
/// a test or a paper session is the harness rather than the code under test.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken, not that the code is fast.

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

#include "mm/exchange/paper/PaperExecution.hpp"

namespace mm::exchange::paper {
namespace {

/// Discards events. The cost of consuming them belongs to the OMS benchmarks.
class NullSink final : public IExecutionSink {
public:
    void on_execution(const ExecutionEvent& event) override {
        ++delivered;
        ExecutionEventType type = event.type;
        benchmark::DoNotOptimize(type);
    }
    std::uint64_t delivered = 0;
};

PaperExecutionConfig bench_config() {
    PaperExecutionConfig c;
    c.max_orders = 200'000;
    c.max_pending_events = 200'000;
    c.queue_share_bps = 10'000;
    return c;
}

/// A book with `depth` levels a side, built through the real snapshot path.
std::unique_ptr<book::OrderBook> bench_book(std::size_t depth) {
    auto b = std::make_unique<book::OrderBook>(Symbol("BTCUSDT"), 32);
    BookLevels levels;
    for (std::size_t i = 0; i < depth && i < kMaxLevelsPerEvent; ++i) {
        PriceLevel bid;
        PriceLevel ask;
        static_cast<void>(
            Px::parse((std::to_string(60'000 - static_cast<int>(i)) + ".00").c_str(), bid.price));
        static_cast<void>(Qty::parse("5", bid.quantity));
        static_cast<void>(
            Px::parse((std::to_string(60'001 + static_cast<int>(i)) + ".00").c_str(), ask.price));
        static_cast<void>(Qty::parse("5", ask.quantity));
        levels.bids[levels.bid_count++] = bid;
        levels.asks[levels.ask_count++] = ask;
    }
    b->begin_snapshot();
    static_cast<void>(b->add_snapshot_levels(levels));
    static_cast<void>(b->finish_snapshot(100));
    return b;
}

OrderRequest bench_order(std::size_t i, Side side) {
    OrderRequest r;
    static_cast<void>(r.client_order_id.assign(("b" + std::to_string(i)).c_str()));
    r.symbol = Symbol("BTCUSDT");
    r.side = side;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    // Away from the touch *on the correct side*: a sell priced below the bid
    // would cross on arrival and execute as a taker instead of resting, which
    // would leave every "resting orders" fixture below half empty.
    const int offset = static_cast<int>(i % 5'000);
    const std::string price = side == Side::Buy ? std::to_string(50'000 + offset) + ".00"
                                                : std::to_string(65'000 + offset) + ".00";
    static_cast<void>(Px::parse(price.c_str(), r.price));
    static_cast<void>(Qty::parse("1", r.quantity));
    return r;
}

/// Fills a venue with `count` acknowledged resting orders.
void populate(PaperExecution& venue, ManualClock& clock, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!venue.submit(bench_order(i, i % 2 == 0 ? Side::Buy : Side::Sell)).is_ok()) {
            continue;
        }
        // Past the full request-plus-ack round trip. Advancing less would leave
        // the orders in flight, and every benchmark below would silently
        // measure an empty venue -- which reports a very good number.
        clock.advance(venue.config().latency.new_total() + micros(1));
        static_cast<void>(venue.poll());
    }
}

// ---------------------------------------------------------------------------
// Request submission -- validation, identity, queueing. No event work.
// ---------------------------------------------------------------------------

void BM_PaperSubmit(benchmark::State& state) {
    constexpr std::size_t kBatch = 4'096;
    ManualClock clock(0, 0);
    NullSink sink;
    auto venue = std::make_unique<PaperExecution>(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue->start(sink));
    venue->attach_book(Symbol("BTCUSDT"), *book);

    std::vector<OrderRequest> requests;
    requests.reserve(kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        requests.push_back(bench_order(i, i % 2 == 0 ? Side::Buy : Side::Sell));
    }

    std::size_t i = 0;
    for (auto _ : state) {
        Status s = venue->submit(requests[i % kBatch]);
        benchmark::DoNotOptimize(s);
        ++i;
        if (i % kBatch == 0) {
            // Rebuilding is not part of the measurement; without it this would
            // become a benchmark of an ever-growing in-flight queue.
            state.PauseTiming();
            venue = std::make_unique<PaperExecution>(bench_config(), clock);
            static_cast<void>(venue->start(sink));
            venue->attach_book(Symbol("BTCUSDT"), *book);
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperSubmit);

// ---------------------------------------------------------------------------
// Event generation -- a request becoming an acknowledgement and being delivered
// ---------------------------------------------------------------------------

void BM_PaperRequestToEvent(benchmark::State& state) {
    constexpr std::size_t kBatch = 2'048;
    ManualClock clock(0, 0);
    NullSink sink;
    auto venue = std::make_unique<PaperExecution>(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue->start(sink));
    venue->attach_book(Symbol("BTCUSDT"), *book);

    std::size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (i % kBatch == 0 && i != 0) {
            venue = std::make_unique<PaperExecution>(bench_config(), clock);
            static_cast<void>(venue->start(sink));
            venue->attach_book(Symbol("BTCUSDT"), *book);
        }
        static_cast<void>(venue->submit(bench_order(i % kBatch, Side::Buy)));
        clock.advance(millis(1));
        ++i;
        state.ResumeTiming();

        // Processes the request and delivers the acknowledgement.
        std::size_t n = venue->poll();
        benchmark::DoNotOptimize(n);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperRequestToEvent);

// ---------------------------------------------------------------------------
// Fill evaluation -- the matching pass over resting orders on a book update
// ---------------------------------------------------------------------------

void BM_PaperMatchOnMarketUpdate(benchmark::State& state) {
    const auto resting = static_cast<std::size_t>(state.range(0));
    ManualClock clock(0, 0);
    NullSink sink;
    PaperExecution venue(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue.start(sink));
    venue.attach_book(Symbol("BTCUSDT"), *book);
    populate(venue, clock, resting);
    if (venue.resting_order_count() != resting) {
        state.SkipWithError("fixture did not produce the requested resting orders");
        return;
    }

    // The orders rest well away from the touch, so this measures the scan and
    // the priority sort -- the cost paid on every single book update, whether
    // or not anything fills.
    for (auto _ : state) {
        clock.advance(micros(1));
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(resting));
}
BENCHMARK(BM_PaperMatchOnMarketUpdate)->Arg(8)->Arg(64)->Arg(512);

/// The matcher alone, against a book that actually executes.
void BM_PaperMatchAgainstLevels(benchmark::State& state) {
    auto book = bench_book(16);
    PaperOrder order;
    order.side = Side::Buy;
    static_cast<void>(Px::parse("70000.00", order.price));
    static_cast<void>(Qty::parse("1000", order.original_qty));
    order.status = OrderStatus::New;

    for (auto _ : state) {
        state.PauseTiming();
        // A fresh working copy each iteration: the matcher consumes it, and
        // measuring the second pass over an exhausted book would measure
        // nothing.
        std::vector<PriceLevel> available = executable_side(*book, Side::Buy);
        state.ResumeTiming();

        MatchResult r = match_and_consume(order, available, FillModel::DisplayedLiquidity, 10'000);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperMatchAgainstLevels);

// ---------------------------------------------------------------------------
// Order lookup -- what cancel, replace and query all pay first
// ---------------------------------------------------------------------------

void BM_PaperLookupByClientId(benchmark::State& state) {
    const auto resting = static_cast<std::size_t>(state.range(0));
    ManualClock clock(0, 0);
    NullSink sink;
    PaperExecution venue(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue.start(sink));
    venue.attach_book(Symbol("BTCUSDT"), *book);
    populate(venue, clock, resting);
    if (venue.resting_order_count() != resting) {
        state.SkipWithError("fixture did not produce the requested resting orders");
        return;
    }

    std::vector<ClientOrderId> ids;
    for (const PaperOrder& o : venue.snapshot_orders()) {
        ids.push_back(o.client_order_id);
    }
    if (ids.empty()) {
        state.SkipWithError("fixture produced no orders to look up");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        const PaperOrder* found = venue.find_order(ids[i % ids.size()]);
        benchmark::DoNotOptimize(found);
        benchmark::ClobberMemory();
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperLookupByClientId)->Arg(16)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Cancel and replace
// ---------------------------------------------------------------------------

void BM_PaperCancel(benchmark::State& state) {
    constexpr std::size_t kBatch = 1'024;
    ManualClock clock(0, 0);
    NullSink sink;
    auto venue = std::make_unique<PaperExecution>(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue->start(sink));
    venue->attach_book(Symbol("BTCUSDT"), *book);
    populate(*venue, clock, kBatch);

    std::size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (i % kBatch == 0 && i != 0) {
            venue = std::make_unique<PaperExecution>(bench_config(), clock);
            static_cast<void>(venue->start(sink));
            venue->attach_book(Symbol("BTCUSDT"), *book);
            populate(*venue, clock, kBatch);
        }
        CancelRequest req;
        static_cast<void>(req.client_order_id.assign(("b" + std::to_string(i % kBatch)).c_str()));
        req.symbol = Symbol("BTCUSDT");
        ++i;
        state.ResumeTiming();

        Status s = venue->cancel(req);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperCancel);

void BM_PaperReplace(benchmark::State& state) {
    constexpr std::size_t kBatch = 512;
    ManualClock clock(0, 0);
    NullSink sink;
    auto venue = std::make_unique<PaperExecution>(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue->start(sink));
    venue->attach_book(Symbol("BTCUSDT"), *book);
    populate(*venue, clock, kBatch);

    std::size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (i % kBatch == 0 && i != 0) {
            venue = std::make_unique<PaperExecution>(bench_config(), clock);
            static_cast<void>(venue->start(sink));
            venue->attach_book(Symbol("BTCUSDT"), *book);
            populate(*venue, clock, kBatch);
        }
        ReplaceRequest req;
        static_cast<void>(
            req.original_client_order_id.assign(("b" + std::to_string(i % kBatch)).c_str()));
        static_cast<void>(req.new_client_order_id.assign(("n" + std::to_string(i)).c_str()));
        req.symbol = Symbol("BTCUSDT");
        static_cast<void>(Px::parse("52000.00", req.new_price));
        static_cast<void>(Qty::parse("1", req.new_quantity));
        ++i;
        state.ResumeTiming();

        Status s = venue->replace(req);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PaperReplace);

// ---------------------------------------------------------------------------
// Event publication -- draining a queue of due events to the sink
// ---------------------------------------------------------------------------

void BM_PaperEventPublication(benchmark::State& state) {
    const auto batch = static_cast<std::size_t>(state.range(0));
    ManualClock clock(0, 0);
    NullSink sink;
    auto venue = std::make_unique<PaperExecution>(bench_config(), clock);
    auto book = bench_book(8);
    static_cast<void>(venue->start(sink));
    venue->attach_book(Symbol("BTCUSDT"), *book);

    std::size_t generation = 0;
    for (auto _ : state) {
        state.PauseTiming();
        venue = std::make_unique<PaperExecution>(bench_config(), clock);
        static_cast<void>(venue->start(sink));
        venue->attach_book(Symbol("BTCUSDT"), *book);
        for (std::size_t i = 0; i < batch; ++i) {
            static_cast<void>(venue->submit(bench_order(generation * batch + i, Side::Buy)));
        }
        ++generation;
        clock.advance(millis(1));
        state.ResumeTiming();

        // One poll: processes every request and delivers every answer.
        std::size_t n = venue->poll();
        benchmark::DoNotOptimize(n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}
BENCHMARK(BM_PaperEventPublication)->Arg(1)->Arg(16)->Arg(256);

}  // namespace
}  // namespace mm::exchange::paper
