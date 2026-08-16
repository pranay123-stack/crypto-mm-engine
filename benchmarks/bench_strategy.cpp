/// Strategy-runtime benchmarks.
///
/// These measure the cost the trading thread pays to consult a strategy: the
/// context, the call, the validation and the whole path together. The reference
/// strategy is used as a stand-in — it is deliberately trivial, so these are a
/// floor for the runtime's overhead, not a prediction of what a real strategy
/// will cost.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken.

#include <benchmark/benchmark.h>

#include <memory>

#include "mm/orderbook/OrderBook.hpp"
#include "mm/strategy/StrategyRegistry.hpp"
#include "mm/strategy/StrategyRuntime.hpp"

namespace mm::strategy {
namespace {

InstrumentSpec spec() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("0.01", s.tick_size));
    static_cast<void>(Qty::parse("0.00001", s.lot_size));
    static_cast<void>(Qty::parse("1000", s.max_qty));
    static_cast<void>(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

book::OrderBook seeded_book() {
    book::OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    exchange::BookLevels levels;
    for (int i = 0; i < 16; ++i) {
        levels.bids[levels.bid_count++] =
            exchange::PriceLevel{Px::from_units(60'000 - i), Qty::from_units(1)};
        levels.asks[levels.ask_count++] =
            exchange::PriceLevel{Px::from_units(60'001 + i), Qty::from_units(1)};
    }
    static_cast<void>(b.add_snapshot_levels(levels));
    static_cast<void>(b.finish_snapshot(1));
    return b;
}

StrategyContext make_context(const InstrumentSpec& s, const book::OrderBook& b, Nanos now) {
    StrategyContext c;
    c.symbol = Symbol("BTCUSDT");
    c.bbo = b.bbo();
    c.depth = BookDepthView(b.bids().data(), b.bids().size(), b.asks().data(), b.asks().size());
    c.sequence = b.last_update_id();
    c.now_ns = now;
    c.recv_ns = now;
    c.instrument = &s;
    c.session_state = SessionState::Ready;
    c.trigger = TriggerReason::BboChange;
    static_cast<void>(Qty::parse("0.5", c.inventory.position_limit));
    return c;
}

std::unique_ptr<StrategyRuntime> make_runtime(const InstrumentSpec& s, ManualClock& clock) {
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    if (created.is_error()) {
        return nullptr;
    }
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = s;
    init.identity = created.value()->identity();
    init.params.set("half_spread_bps", "5.0");
    init.params.set("quote_size", "0.001");
    init.params.set("inventory_skew_bps", "10.0");

    StrategyRuntimeConfig config;
    // A ManualClock that never advances would make every evaluation appear to
    // take zero time; the budget check must not fire spuriously either way.
    config.max_evaluation_latency_ns = seconds(1);
    auto runtime = std::make_unique<StrategyRuntime>(config, std::move(created.value()), clock);
    if (runtime->initialize(init).is_error()) {
        return nullptr;
    }
    runtime->start();
    return runtime;
}

/// Assembling the immutable view the strategy is handed. Runs once per
/// evaluation on the trading thread.
void BM_ContextConstruction(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const book::OrderBook b = seeded_book();
    Nanos now = 1'000;
    for (auto _ : state) {
        benchmark::DoNotOptimize(now);
        StrategyContext c = make_context(s, b, now);
        benchmark::DoNotOptimize(c);
        ++now;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ContextConstruction);

/// The strategy call alone, without the runtime around it.
void BM_StrategyInvocation(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const book::OrderBook b = seeded_book();
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    if (created.is_error()) {
        state.SkipWithError("reference strategy is not registered");
        return;
    }
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = s;
    init.identity = created.value()->identity();
    init.params.set("half_spread_bps", "5.0");
    init.params.set("quote_size", "0.001");
    init.params.set("inventory_skew_bps", "10.0");
    StrategyPtr strategy = std::move(created.value());
    if (strategy->initialize(init).is_error()) {
        state.SkipWithError("initialize failed");
        return;
    }

    StrategyContext c = make_context(s, b, 1'000);
    std::int64_t nudge = 0;
    for (auto _ : state) {
        // Vary inventory so the skew branch cannot be folded away.
        c.inventory.position = Qty::from_raw(nudge % 1'000);
        benchmark::DoNotOptimize(c);
        QuoteIntent intent = strategy->on_market_update(c);
        benchmark::DoNotOptimize(intent);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StrategyInvocation);

/// Output validation: every tick, lot, notional and provenance check.
void BM_IntentValidation(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const book::OrderBook b = seeded_book();
    StrategyContext c = make_context(s, b, 1'000);

    StrategyIdentity identity;
    static_cast<void>(identity.name.assign("reference_mm_v1"));
    identity.version = 1;

    QuoteIntent intent;
    intent.action = QuoteAction::Quote;
    intent.quote_bid = true;
    intent.quote_ask = true;
    intent.bid_price = c.bbo.bid_px;
    intent.ask_price = c.bbo.ask_px;
    static_cast<void>(Qty::parse("0.001", intent.bid_quantity));
    static_cast<void>(Qty::parse("0.001", intent.ask_quantity));
    intent.identity = identity;
    intent.market_sequence = c.sequence;

    const IntentLimits limits;
    std::int64_t nudge = 0;
    for (auto _ : state) {
        intent.bid_price = Px::from_raw(c.bbo.bid_px.raw() - (nudge % 5) * 1'000'000);
        benchmark::DoNotOptimize(intent);
        IntentRejection r = validate_intent(intent, c, identity, limits);
        benchmark::DoNotOptimize(r);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IntentValidation);

/// The number that matters: gate, invoke, time, validate, stamp. This is what
/// the trading thread actually pays per evaluation.
void BM_FullRuntimePath(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const book::OrderBook b = seeded_book();
    ManualClock clock(1'000, 0);
    auto runtime = make_runtime(s, clock);
    if (!runtime) {
        state.SkipWithError("could not build the runtime");
        return;
    }

    StrategyContext c = make_context(s, b, clock.steady());
    std::int64_t nudge = 0;
    for (auto _ : state) {
        c.inventory.position = Qty::from_raw(nudge % 1'000);
        benchmark::DoNotOptimize(c);
        EvaluationResult r = runtime->evaluate(c);
        benchmark::DoNotOptimize(r);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["accepted"] = static_cast<double>(runtime->metrics().intents_accepted);
}
BENCHMARK(BM_FullRuntimePath);

/// The gate alone, for the case where the engine wants to know whether
/// assembling a context is worth it.
void BM_RuntimeGate(benchmark::State& state) {
    const InstrumentSpec s = spec();
    const book::OrderBook b = seeded_book();
    ManualClock clock(1'000, 0);
    auto runtime = make_runtime(s, clock);
    if (!runtime) {
        state.SkipWithError("could not build the runtime");
        return;
    }
    StrategyContext c = make_context(s, b, clock.steady());
    std::int64_t nudge = 0;
    for (auto _ : state) {
        c.data_age_ns = nudge % 100;
        benchmark::DoNotOptimize(c);
        SkipReason r = runtime->gate(c);
        benchmark::DoNotOptimize(r);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RuntimeGate);

}  // namespace
}  // namespace mm::strategy
