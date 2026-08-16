/// Quote-manager benchmarks.
///
/// Only the paths the trading thread actually walks: the full evaluation, and
/// the three shapes it resolves to. Everything else here runs at configuration
/// time and is not measured.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken.

#include <benchmark/benchmark.h>

#include "mm/quote/QuoteManager.hpp"

namespace mm::quote {
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

exchange::ExchangeCapabilities caps() {
    exchange::ExchangeCapabilities c;
    c.supports_client_order_id = true;
    c.supports_replace = true;
    return c;
}

strategy::QuoteIntent intent(std::uint64_t generation, std::int64_t bid_raw) {
    strategy::QuoteIntent i;
    i.action = strategy::QuoteAction::Quote;
    i.quote_bid = true;
    i.quote_ask = true;
    i.bid_price = Px::from_raw(bid_raw);
    i.ask_price = Px::from_raw(bid_raw + 10'000'000);  // 0.10 wide
    static_cast<void>(Qty::parse("0.001", i.bid_quantity));
    static_cast<void>(Qty::parse("0.001", i.ask_quantity));
    static_cast<void>(i.identity.name.assign("bench_v1"));
    i.identity.version = 1;
    i.generation = generation;
    i.market_sequence = 100 + generation;
    return i;
}

WorkingOrder resting(QuoteSlot slot, std::int64_t price_raw) {
    WorkingOrder o;
    o.present = true;
    static_cast<void>(o.owner.strategy.assign("bench_v1"));
    o.owner.slot = slot;
    o.side = side_of(slot);
    o.price = Px::from_raw(price_raw);
    static_cast<void>(Qty::parse("0.001", o.original_quantity));
    o.status = exchange::OrderStatus::New;
    static_cast<void>(o.client_order_id.assign("mm-1"));
    static_cast<void>(o.exchange_order_id.assign("EX-1"));
    return o;
}

QuoteManagerInput base_input(const InstrumentSpec& s, const exchange::ExchangeCapabilities& c) {
    QuoteManagerInput in;
    in.symbol = Symbol("BTCUSDT");
    in.instrument = &s;
    in.capabilities = &c;
    in.system_ready = true;
    return in;
}

/// The common steady-state case: quotes already rest and the market has not
/// moved enough to matter. This is what the trading thread pays most of the
/// time, so it is the number that matters.
void BM_EvaluateKeep(benchmark::State& state) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec s = spec();
    const exchange::ExchangeCapabilities c = caps();
    QuoteManagerConfig config;
    config.assume_request_lost_after_ns = 0;
    QuoteManager manager(StrategyName("bench_v1"), config, clock);

    QuoteManagerInput in = base_input(s, c);
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, 6'000'000'000'000);
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, 6'000'010'000'000);

    std::uint64_t generation = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        QuoteManagerResult r = manager.evaluate(intent(++generation, 6'000'000'000'000), in);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["keeps"] = static_cast<double>(manager.metrics().keeps);
}
BENCHMARK(BM_EvaluateKeep);

/// A price move that warrants rewriting both quotes.
void BM_EvaluateReplace(benchmark::State& state) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec s = spec();
    const exchange::ExchangeCapabilities c = caps();
    QuoteManagerConfig config;
    config.assume_request_lost_after_ns = 0;
    QuoteManager manager(StrategyName("bench_v1"), config, clock);

    QuoteManagerInput in = base_input(s, c);
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, 6'000'000'000'000);
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, 6'000'010'000'000);

    std::uint64_t generation = 0;
    std::int64_t nudge = 0;
    for (auto _ : state) {
        // Move the desired price every iteration so the diff genuinely differs.
        benchmark::DoNotOptimize(in);
        QuoteManagerResult r = manager.evaluate(
            intent(++generation, 6'000'000'000'000 + (nudge % 20) * 1'000'000), in);
        benchmark::DoNotOptimize(r);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["replaces"] = static_cast<double>(manager.metrics().replace_actions);
}
BENCHMARK(BM_EvaluateReplace);

/// Nothing resting, both sides wanted: validation plus two New actions.
void BM_EvaluateNew(benchmark::State& state) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec s = spec();
    const exchange::ExchangeCapabilities c = caps();
    QuoteManagerConfig config;
    config.assume_request_lost_after_ns = 0;
    QuoteManager manager(StrategyName("bench_v1"), config, clock);
    QuoteManagerInput in = base_input(s, c);

    std::uint64_t generation = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        QuoteManagerResult r = manager.evaluate(++generation % 2 == 0
                                                     ? intent(generation, 6'000'000'000'000)
                                                     : intent(generation, 6'000'001'000'000),
                                                 in);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["news"] = static_cast<double>(manager.metrics().new_actions);
}
BENCHMARK(BM_EvaluateNew);

/// The withdrawal path, used by every kill switch and safety halt.
void BM_DisableAllQuotes(benchmark::State& state) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec s = spec();
    const exchange::ExchangeCapabilities c = caps();
    QuoteManager manager(StrategyName("bench_v1"), QuoteManagerConfig{}, clock);

    QuoteManagerInput in = base_input(s, c);
    in.working.slot(QuoteSlot::Bid) = resting(QuoteSlot::Bid, 6'000'000'000'000);
    in.working.slot(QuoteSlot::Ask) = resting(QuoteSlot::Ask, 6'000'010'000'000);

    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        QuoteManagerResult r = manager.disable_all_quotes(in, ActionReason::OperatorDisabled);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DisableAllQuotes);

/// The screening path alone: an intent rejected before any slot is considered.
void BM_EvaluateRejected(benchmark::State& state) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec s = spec();
    const exchange::ExchangeCapabilities c = caps();
    QuoteManager manager(StrategyName("bench_v1"), QuoteManagerConfig{}, clock);
    QuoteManagerInput in = base_input(s, c);
    in.market_data_age_ns = seconds(5);  // stale

    std::uint64_t generation = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        QuoteManagerResult r = manager.evaluate(intent(++generation, 6'000'000'000'000), in);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EvaluateRejected);

}  // namespace
}  // namespace mm::quote
