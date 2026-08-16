/// Benchmarks for the exchange-boundary operations that sit on the hot path.
///
/// Only four things here run per-event on the trading thread: request
/// validation, event validation, envelope transport through a ring, and the
/// session transition check. Everything else in the exchange layer runs on an
/// I/O thread or at startup and is not benchmarked, because a number nobody
/// acts on is noise.
///
/// Phase 2 discipline applies: any sub-nanosecond result for real work is
/// assumed to be the optimizer, not the code.

#include <benchmark/benchmark.h>

#include "mm/common/SpscRing.hpp"
#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"
#include "mm/exchange/common/OrderRequest.hpp"
#include "mm/exchange/mock/MockExchangeExecution.hpp"

namespace mm::exchange {
namespace {

InstrumentSpec spec() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("0.01", s.tick_size));
    static_cast<void>(Qty::parse("0.00001", s.lot_size));
    static_cast<void>(Qty::parse("0.00001", s.min_qty));
    static_cast<void>(Qty::parse("100", s.max_qty));
    static_cast<void>(Notional::parse("10", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

OrderRequest request() {
    OrderRequest r;
    r.client_order_id = ClientOrderId("mm-1-000000042");
    r.symbol = Symbol("BTCUSDT");
    r.side = Side::Buy;
    r.type = OrderType::Limit;
    static_cast<void>(Px::parse("60000.00", r.price));
    static_cast<void>(Qty::parse("0.001", r.quantity));
    return r;
}

// ------------------------------------------------------- request validation

/// Runs once per outbound order on the trading thread, so its cost is part of
/// the quote-to-wire budget.
void BM_ValidateOrder(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const ExchangeCapabilities caps = mock::permissive_mock_capabilities();
    OrderRequest r = request();
    std::int64_t nudge = 0;

    for (auto _ : state) {
        // Vary an input so the whole call cannot be hoisted out of the loop.
        r.price = Px::from_raw(6'000'000'000'000 + (nudge % 100) * 1'000'000);
        benchmark::DoNotOptimize(r);
        ExchangeError e = validate_order(r, s, caps);
        benchmark::DoNotOptimize(e);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ValidateOrder);

/// The rejection path matters too: it is what a misconfigured strategy hits
/// repeatedly, and it must not be dramatically more expensive than acceptance.
void BM_ValidateOrderRejectingPath(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const ExchangeCapabilities caps = mock::permissive_mock_capabilities();
    OrderRequest r = request();
    static_cast<void>(Px::parse("60000.005", r.price));  // off tick
    std::int64_t nudge = 0;

    for (auto _ : state) {
        r.quantity = Qty::from_raw(100'000 + (nudge % 100));
        benchmark::DoNotOptimize(r);
        ExchangeError e = validate_order(r, s, caps);
        benchmark::DoNotOptimize(e);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ValidateOrderRejectingPath);

// --------------------------------------------------------- event validation

ExecutionEvent fill_event() {
    ExecutionEvent e;
    e.type = ExecutionEventType::Fill;
    e.payload.fill.client_order_id = ClientOrderId("mm-1-000000042");
    e.payload.fill.trade_id = TradeId("T-00000001");
    e.payload.fill.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("60000.00", e.payload.fill.price));
    static_cast<void>(Qty::parse("0.0005", e.payload.fill.quantity));
    static_cast<void>(Qty::parse("0.0005", e.payload.fill.cumulative_qty));
    static_cast<void>(Qty::parse("0.0005", e.payload.fill.leaves_qty));
    return e;
}

/// Runs on every inbound execution event before the OMS acts on it.
void BM_ValidateExecutionEvent(benchmark::State& state) {
    ExecutionEvent e = fill_event();
    std::int64_t nudge = 0;
    for (auto _ : state) {
        e.payload.fill.seq = static_cast<Seq>(nudge);
        benchmark::DoNotOptimize(e);
        Status s = validate_execution_event(e);
        benchmark::DoNotOptimize(s);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ValidateExecutionEvent);

// -------------------------------------------------------- envelope transport

/// The real cost of the envelope decision: how much a 680-byte execution event
/// costs to move through a ring versus a 624-byte market data event. This is
/// what justified shrinking the open-orders chunk.
void BM_ExecutionEventThroughRing(benchmark::State& state) {
    SpscRing<ExecutionEvent, 256> ring;
    const ExecutionEvent in = fill_event();
    ExecutionEvent out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.try_push(in));
        benchmark::DoNotOptimize(ring.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(ExecutionEvent)));
}
BENCHMARK(BM_ExecutionEventThroughRing);

void BM_MarketDataEventThroughRing(benchmark::State& state) {
    SpscRing<MarketDataEvent, 256> ring;
    MarketDataEvent in;
    in.type = MarketDataEventType::BookUpdate;
    in.payload.book_update.symbol = Symbol("BTCUSDT");
    in.payload.book_update.levels.bid_count = 8;
    in.payload.book_update.levels.ask_count = 8;
    MarketDataEvent out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.try_push(in));
        benchmark::DoNotOptimize(ring.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(MarketDataEvent)));
}
BENCHMARK(BM_MarketDataEventThroughRing);

/// A trade is 60 bytes of content in a 624-byte slot. Measuring it separately
/// shows what the uniform envelope actually costs on the most common event.
void BM_TradeEventThroughRing(benchmark::State& state) {
    SpscRing<MarketDataEvent, 256> ring;
    MarketDataEvent in;
    in.type = MarketDataEventType::Trade;
    in.payload.trade.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("60000.00", in.payload.trade.price));
    static_cast<void>(Qty::parse("0.5", in.payload.trade.quantity));
    MarketDataEvent out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.try_push(in));
        benchmark::DoNotOptimize(ring.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TradeEventThroughRing);

// ------------------------------------------------------ session transitions

/// Checked on every session state change, which is rare -- included to confirm
/// it is not accidentally expensive rather than because it is on a hot path.
void BM_SessionTransitionCheck(benchmark::State& state) {
    int i = 0;
    for (auto _ : state) {
        auto from = static_cast<SessionState>(i % 11);
        auto to = static_cast<SessionState>((i + 3) % 11);
        benchmark::DoNotOptimize(from);
        benchmark::DoNotOptimize(to);
        bool legal = is_legal_transition(from, to);
        benchmark::DoNotOptimize(legal);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionTransitionCheck);

}  // namespace
}  // namespace mm::exchange
