/// Accounting benchmarks (Phase 10).
///
/// These are engineering measurements of the accounting layer's own cost.
/// **They say nothing about whether any strategy makes money**, and nothing
/// about exchange latency. Environment caveats in docs/benchmarks.md apply:
/// this is a shared desktop and the absolute values move with load.
///
/// Phase 2 discipline: a sub-nanosecond result for real work means the
/// benchmark is broken, not that the code is fast.

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

#include "mm/portfolio/PortfolioAccountant.hpp"

namespace mm::portfolio {
namespace {

AccountingConfig bench_config(std::size_t symbols) {
    AccountingConfig c;
    c.max_symbols = symbols;
    return c;
}

exchange::FillEvent bench_fill(std::size_t i, const Symbol& symbol, bool buy) {
    exchange::FillEvent f;
    static_cast<void>(f.trade_id.assign(("BT" + std::to_string(i)).c_str()));
    f.symbol = symbol;
    f.side = buy ? Side::Buy : Side::Sell;
    // Varying price and quantity, so nothing folds to a constant and the
    // weighted-average path is genuinely exercised.
    static_cast<void>(
        Px::parse((std::to_string(59'000 + (i % 2'000)) + ".25").c_str(), f.price));
    static_cast<void>(Qty::parse((i % 2 == 0 ? "0.25" : "0.75"), f.quantity));
    static_cast<void>(Notional::parse("0.06", f.fee));
    return f;
}

/// One fill applied end to end: validation, dedup, cost basis, PnL, fees.
void BM_AccountingApplyFill(benchmark::State& state) {
    constexpr std::size_t kBatch = 4'096;
    ManualClock clock(0, 0);
    auto accountant = std::make_unique<PortfolioAccountant>(bench_config(4), clock);
    static_cast<void>(accountant->register_symbol(Symbol("BTCUSDT")));

    std::vector<exchange::FillEvent> fills;
    fills.reserve(kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        // Alternating sides so reductions, closes and flips all occur rather
        // than only the cheap "increase a long" path.
        fills.push_back(bench_fill(i, Symbol("BTCUSDT"), (i % 3) != 0));
    }

    std::size_t i = 0;
    for (auto _ : state) {
        FillOutcome out = accountant->on_fill(fills[i % kBatch]);
        benchmark::DoNotOptimize(out);
        ++i;
        if (i % kBatch == 0) {
            // Rebuilt outside the timed region: the dedup ring would otherwise
            // start reporting every fill as a duplicate and this would become
            // a benchmark of the rejection path.
            state.PauseTiming();
            accountant = std::make_unique<PortfolioAccountant>(bench_config(4), clock);
            static_cast<void>(accountant->register_symbol(Symbol("BTCUSDT")));
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingApplyFill);

/// The duplicate path, which a redelivering venue makes common.
void BM_AccountingDuplicateDetection(benchmark::State& state) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(4), clock);
    static_cast<void>(accountant.register_symbol(Symbol("BTCUSDT")));
    for (std::size_t i = 0; i < 32; ++i) {
        static_cast<void>(accountant.on_fill(bench_fill(i, Symbol("BTCUSDT"), true)));
    }

    std::size_t i = 0;
    for (auto _ : state) {
        FillOutcome out = accountant.on_fill(bench_fill(i % 32, Symbol("BTCUSDT"), true));
        benchmark::DoNotOptimize(out);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingDuplicateDetection);

/// A mark applied to an open position: the unrealized-PnL recomputation.
void BM_AccountingApplyMark(benchmark::State& state) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(4), clock);
    static_cast<void>(accountant.register_symbol(Symbol("BTCUSDT")));
    static_cast<void>(accountant.on_fill(bench_fill(1, Symbol("BTCUSDT"), true)));

    std::vector<MarkPrice> marks;
    marks.reserve(256);
    for (std::size_t i = 0; i < 256; ++i) {
        MarkPrice m;
        m.symbol = Symbol("BTCUSDT");
        static_cast<void>(
            Px::parse((std::to_string(59'000 + (i % 500)) + ".00").c_str(), m.bid));
        static_cast<void>(
            Px::parse((std::to_string(59'001 + (i % 500)) + ".00").c_str(), m.ask));
        marks.push_back(m);
    }

    std::size_t i = 0;
    for (auto _ : state) {
        AccountingError e = accountant.on_mark(marks[i % marks.size()]);
        benchmark::DoNotOptimize(e);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingApplyMark);

/// The per-symbol snapshot risk reads every cycle.
void BM_AccountingPositionSnapshot(benchmark::State& state) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(4), clock);
    static_cast<void>(accountant.register_symbol(Symbol("BTCUSDT")));
    static_cast<void>(accountant.on_fill(bench_fill(1, Symbol("BTCUSDT"), true)));

    for (auto _ : state) {
        risk::PositionSnapshot s = accountant.position_snapshot(Symbol("BTCUSDT"));
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingPositionSnapshot);

/// Portfolio aggregation across symbols -- the full pass risk reads for its
/// portfolio and loss limits.
void BM_AccountingPortfolioAggregate(benchmark::State& state) {
    const auto symbols = static_cast<std::size_t>(state.range(0));
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(symbols + 2), clock);
    for (std::size_t s = 0; s < symbols; ++s) {
        const Symbol sym(("SYM" + std::to_string(s)).c_str());
        if (accountant.register_symbol(sym) != AccountingError::None) {
            state.SkipWithError("fixture could not register the requested symbols");
            return;
        }
        static_cast<void>(accountant.on_fill(bench_fill(s, sym, s % 2 == 0)));
        MarkPrice m;
        m.symbol = sym;
        static_cast<void>(Px::parse("59000.00", m.bid));
        static_cast<void>(Px::parse("59001.00", m.ask));
        if (accountant.on_mark(m) != AccountingError::None) {
            state.SkipWithError("fixture could not mark the requested symbols");
            return;
        }
    }
    if (accountant.symbol_count() != symbols) {
        state.SkipWithError("fixture did not produce the requested symbol count");
        return;
    }

    for (auto _ : state) {
        PortfolioSnapshot s = accountant.portfolio_snapshot();
        benchmark::DoNotOptimize(s);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(symbols));
}
BENCHMARK(BM_AccountingPortfolioAggregate)->Arg(1)->Arg(8)->Arg(32);

/// The risk-facing view, which is the aggregate plus a translation.
void BM_AccountingRiskView(benchmark::State& state) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(8), clock);
    for (std::size_t s = 0; s < 4; ++s) {
        const Symbol sym(("SYM" + std::to_string(s)).c_str());
        static_cast<void>(accountant.register_symbol(sym));
        static_cast<void>(accountant.on_fill(bench_fill(s, sym, true)));
    }

    for (auto _ : state) {
        risk::PortfolioState v = accountant.risk_view();
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingRiskView);

/// Snapshot publication for recovery. Allocates by design -- it is not a hot
/// path -- and is measured so that fact is visible rather than assumed.
void BM_AccountingSnapshotForRecovery(benchmark::State& state) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(bench_config(16), clock);
    for (std::size_t s = 0; s < 8; ++s) {
        const Symbol sym(("SYM" + std::to_string(s)).c_str());
        static_cast<void>(accountant.register_symbol(sym));
        static_cast<void>(accountant.on_fill(bench_fill(s, sym, true)));
    }

    for (auto _ : state) {
        PortfolioAccountant::RecoveryState s = accountant.save();
        benchmark::DoNotOptimize(s.accounts.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountingSnapshotForRecovery);

}  // namespace
}  // namespace mm::portfolio
