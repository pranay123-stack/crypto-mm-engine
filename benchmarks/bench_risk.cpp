/// Risk-engine benchmarks.
///
/// Risk sits on the path between the quote manager and the OMS, so every order
/// pays it. Only that path is measured; arming and configuration happen once.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken.

#include <benchmark/benchmark.h>

#include "mm/risk/RiskEngine.hpp"

namespace mm::risk {
namespace {

InstrumentSpec instrument() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("0.01", s.tick_size));
    static_cast<void>(Qty::parse("0.001", s.lot_size));
    static_cast<void>(Qty::parse("0.001", s.min_qty));
    static_cast<void>(Qty::parse("1000", s.max_qty));
    static_cast<void>(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

RiskLimits limits() {
    SymbolLimits btc;
    btc.symbol = Symbol("BTCUSDT");
    static_cast<void>(Qty::parse("10", btc.max_position));
    static_cast<void>(Notional::parse("700000", btc.max_position_notional));
    static_cast<void>(Qty::parse("2", btc.max_order_quantity));
    static_cast<void>(Notional::parse("150000", btc.max_order_notional));
    static_cast<void>(Qty::parse("20", btc.max_working_exposure));
    static_cast<void>(Qty::parse("12", btc.max_side_exposure));
    btc.max_open_orders = 8;
    btc.price_band_bps = 500;

    RiskLimits l;
    l.global.max_market_data_age_ns = millis(500);
    l.global.max_position_age_ns = seconds(5);
    static_cast<void>(l.add(btc));
    return l;
}

RiskInput make_input(const InstrumentSpec& spec, Nanos now) {
    RiskInput in;
    in.symbol = Symbol("BTCUSDT");
    in.instrument = &spec;
    in.position.valid = true;
    in.position.symbol = Symbol("BTCUSDT");
    static_cast<void>(Qty::parse("1", in.position.quantity));
    in.position.as_of_ns = now;
    in.position.sequence = 1;
    in.exposure.determinate = true;
    static_cast<void>(Qty::parse("1", in.exposure.working_buy));
    static_cast<void>(Qty::parse("1", in.exposure.working_sell));
    in.exposure.open_order_count = 2;
    static_cast<void>(Px::parse("60000.00", in.bbo.bid_px));
    static_cast<void>(Qty::parse("5", in.bbo.bid_qty));
    static_cast<void>(Px::parse("60000.10", in.bbo.ask_px));
    static_cast<void>(Qty::parse("5", in.bbo.ask_qty));
    in.system_ready = true;
    static_cast<void>(in.expected_owner.assign("bench_v1"));
    return in;
}

quote::OrderAction order(quote::OrderActionType type, Side side, std::int64_t price_raw,
                         std::int64_t quantity_raw) {
    quote::OrderAction a;
    a.type = type;
    a.slot = quote::slot_of(side);
    a.side = side;
    a.symbol = Symbol("BTCUSDT");
    a.price = Px::from_raw(price_raw);
    a.quantity = Qty::from_raw(quantity_raw);
    static_cast<void>(a.identity.name.assign("bench_v1"));
    a.identity.version = 1;
    a.generation = 1;
    return a;
}

/// The common case: an order comfortably inside every limit. This is what the
/// trading thread pays on the large majority of actions.
void BM_RiskApprove(benchmark::State& state) {
    ManualClock clock(seconds(10), 0);
    const InstrumentSpec spec = instrument();
    RiskEngine engine(limits(), clock);
    if (engine.arm().is_error()) {
        state.SkipWithError("arm failed");
        return;
    }
    RiskInput in = make_input(spec, clock.steady());

    std::int64_t nudge = 0;
    for (auto _ : state) {
        // Vary the price so nothing folds out of the loop.
        auto action = order(quote::OrderActionType::New, Side::Buy,
                            6'000'000'000'000 + (nudge % 20) * 1'000'000, 1'000'000);
        benchmark::DoNotOptimize(in);
        RiskDecision d = engine.evaluate(action, in);
        benchmark::DoNotOptimize(d);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["approvals"] = static_cast<double>(engine.metrics().approvals);
}
BENCHMARK(BM_RiskApprove);

/// Rejection by a hard limit: the full exposure arithmetic still runs.
void BM_RiskReject(benchmark::State& state) {
    ManualClock clock(seconds(10), 0);
    const InstrumentSpec spec = instrument();
    RiskEngine engine(limits(), clock);
    if (engine.arm().is_error()) {
        state.SkipWithError("arm failed");
        return;
    }
    RiskInput in = make_input(spec, clock.steady());
    static_cast<void>(Qty::parse("9.5", in.position.quantity));

    std::int64_t nudge = 0;
    for (auto _ : state) {
        auto action = order(quote::OrderActionType::New, Side::Buy,
                            6'000'000'000'000 + (nudge % 20) * 1'000'000, 200'000'000);
        benchmark::DoNotOptimize(in);
        RiskDecision d = engine.evaluate(action, in);
        benchmark::DoNotOptimize(d);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["rejections"] = static_cast<double>(engine.metrics().rejections);
}
BENCHMARK(BM_RiskReject);

/// Reduction: a binary search over the lot grid, each step re-running the full
/// bounds and exposure checks. The most expensive path by construction.
void BM_RiskReduce(benchmark::State& state) {
    ManualClock clock(seconds(10), 0);
    const InstrumentSpec spec = instrument();
    RiskEngine engine(limits(), clock);
    if (engine.arm().is_error()) {
        state.SkipWithError("arm failed");
        return;
    }
    RiskInput in = make_input(spec, clock.steady());
    static_cast<void>(Qty::parse("5", in.position.quantity));

    std::int64_t nudge = 0;
    for (auto _ : state) {
        // Well over the 2 BTC order limit, so a reduction is always needed.
        auto action = order(quote::OrderActionType::New, Side::Buy,
                            6'000'000'000'000 + (nudge % 20) * 1'000'000, 900'000'000);
        benchmark::DoNotOptimize(in);
        RiskDecision d = engine.evaluate(action, in);
        benchmark::DoNotOptimize(d);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["reductions"] = static_cast<double>(engine.metrics().reductions);
}
BENCHMARK(BM_RiskReduce);

/// Cancellation: the path that must stay available in every state.
void BM_RiskCancel(benchmark::State& state) {
    ManualClock clock(seconds(10), 0);
    const InstrumentSpec spec = instrument();
    RiskEngine engine(limits(), clock);
    if (engine.arm().is_error()) {
        state.SkipWithError("arm failed");
        return;
    }
    RiskInput in = make_input(spec, clock.steady());
    auto action = order(quote::OrderActionType::Cancel, Side::Buy, 6'000'000'000'000, 0);

    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        RiskDecision d = engine.evaluate(action, in);
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskCancel);

/// The exposure arithmetic in isolation, so its share of the total is known
/// rather than guessed.
void BM_WorstCaseExposure(benchmark::State& state) {
    PositionSnapshot position;
    position.valid = true;
    static_cast<void>(Qty::parse("3", position.quantity));
    ExposureSnapshot exposure;
    exposure.determinate = true;
    static_cast<void>(Qty::parse("2", exposure.working_buy));
    static_cast<void>(Qty::parse("2", exposure.working_sell));

    std::int64_t nudge = 0;
    for (auto _ : state) {
        position.quantity = Qty::from_raw(300'000'000 + (nudge % 100));
        benchmark::DoNotOptimize(position);
        WorstCaseExposure out;
        bool ok = compute_worst_case_with(position, exposure, Side::Buy,
                                          Qty::from_raw(1'000'000), out);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(out);
        ++nudge;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WorstCaseExposure);

/// The state gate alone, which is what a killed engine pays.
void BM_RiskKilledRejection(benchmark::State& state) {
    ManualClock clock(seconds(10), 0);
    const InstrumentSpec spec = instrument();
    RiskEngine engine(limits(), clock);
    if (engine.arm().is_error()) {
        state.SkipWithError("arm failed");
        return;
    }
    engine.kill(RiskReason::RiskKilled);
    RiskInput in = make_input(spec, clock.steady());
    auto action = order(quote::OrderActionType::New, Side::Buy, 6'000'000'000'000, 1'000'000);

    for (auto _ : state) {
        benchmark::DoNotOptimize(in);
        RiskDecision d = engine.evaluate(action, in);
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskKilledRejection);

}  // namespace
}  // namespace mm::risk
