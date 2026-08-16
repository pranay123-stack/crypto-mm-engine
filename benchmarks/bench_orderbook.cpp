/// Order-book and synchronization benchmarks.
///
/// The one that decides a design question is `BookUpdateVsStdMap`: the book
/// uses sorted vectors rather than `std::map`, and that choice should rest on a
/// measurement rather than on the assertion that vectors are cache-friendly.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken, not that the code is fast.

#include <benchmark/benchmark.h>

#include <map>
#include <random>

#include "mm/exchange/binance/BinanceCodec.hpp"
#include "mm/orderbook/BookSynchronizer.hpp"
#include "mm/orderbook/OrderBook.hpp"

namespace mm::book {
namespace {

/// `BookLevels` is a 520-byte struct (two 16-element arrays). Constructing one
/// per iteration zero-initialises all of it and swamps the operation under
/// test: the first draft of these benchmarks did exactly that and reported the
/// book as 4x slower than std::map, which was measuring memset, not the book.
/// Callers below build one outside the loop and mutate a single level in place,
/// which is also how the codec fills it in production.
void set_level(BookLevels& levels, Side side, Px price, Qty qty) {
    if (side == Side::Buy) {
        levels.bids[0] = PriceLevel{price, qty};
        levels.bid_count = 1;
        levels.ask_count = 0;
    } else {
        levels.asks[0] = PriceLevel{price, qty};
        levels.ask_count = 1;
        levels.bid_count = 0;
    }
}

OrderBook seeded_book(std::size_t depth) {
    OrderBook b(Symbol("BTCUSDT"), depth);
    b.begin_snapshot();
    BookLevels levels;
    // Seed in chunks of kMaxLevelsPerEvent, as a real snapshot arrives.
    for (std::size_t i = 0; i < depth; ++i) {
        levels.bids[levels.bid_count++] = PriceLevel{Px::from_units(
            static_cast<std::int64_t>(60'000 - i)), Qty::from_units(1)};
        levels.asks[levels.ask_count++] = PriceLevel{Px::from_units(
            static_cast<std::int64_t>(60'001 + i)), Qty::from_units(1)};
        if (levels.bid_count == exchange::kMaxLevelsPerEvent) {
            static_cast<void>(b.add_snapshot_levels(levels));
            levels = BookLevels{};
        }
    }
    static_cast<void>(b.add_snapshot_levels(levels));
    static_cast<void>(b.finish_snapshot(1));
    return b;
}

// ------------------------------------------------------- update throughput

/// Updates concentrate near the touch, which is where the memmove is shortest.
void BM_BookUpdateNearTouch(benchmark::State& state) {
    OrderBook b = seeded_book(50);
    std::mt19937_64 rng(11);
    Seq id = 1;
    BookLevels levels;
    for (auto _ : state) {
        const auto price = static_cast<std::int64_t>(59'995 + rng() % 5);
        const auto qty = static_cast<std::int64_t>(1 + rng() % 3);
        set_level(levels, Side::Buy, Px::from_units(price), Qty::from_units(qty));
        benchmark::DoNotOptimize(b);
        auto s = b.apply_update(levels, ++id);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["depth"] = static_cast<double>(b.depth(Side::Buy));
}
BENCHMARK(BM_BookUpdateNearTouch);

/// The adversarial case: updates spread across the whole tracked depth, so the
/// insert lands as far from index 0 as the structure allows.
void BM_BookUpdateAcrossFullDepth(benchmark::State& state) {
    OrderBook b = seeded_book(50);
    std::mt19937_64 rng(12);
    Seq id = 1;
    BookLevels levels;
    for (auto _ : state) {
        const auto price = static_cast<std::int64_t>(59'951 + rng() % 50);
        set_level(levels, Side::Buy, Px::from_units(price), Qty::from_units(2));
        benchmark::DoNotOptimize(b);
        auto s = b.apply_update(levels, ++id);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookUpdateAcrossFullDepth);

/// The comparison that justifies the data structure. Same access pattern, same
/// depth, against the container this project deliberately did not choose.
void BM_BookUpdateStdMapBaseline(benchmark::State& state) {
    std::map<std::int64_t, std::int64_t, std::greater<>> bids;
    for (int i = 0; i < 50; ++i) {
        bids.emplace(60'000 - i, 1);
    }
    std::mt19937_64 rng(11);
    for (auto _ : state) {
        const auto price = static_cast<std::int64_t>(59'995 + rng() % 5);
        const auto qty = static_cast<std::int64_t>(1 + rng() % 3);
        benchmark::DoNotOptimize(bids);
        if (qty == 0) {
            bids.erase(price);
        } else {
            bids.insert_or_assign(price, qty);
        }
        // Same trailing work the book does: read the touch after mutating.
        auto best = bids.empty() ? 0 : bids.begin()->first;
        benchmark::DoNotOptimize(best);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookUpdateStdMapBaseline);

/// The realistic unit of work: one depth message carrying many levels, which is
/// what the venue actually sends. The per-message validation (sequence check,
/// crossed/locked audit) amortises across the levels, so this is the number
/// that should drive the data-structure decision -- not a single-level update,
/// where fixed per-message cost dominates.
void BM_BookUpdateBatchOfLevels(benchmark::State& state) {
    const auto levels_per_update = static_cast<std::uint8_t>(state.range(0));
    OrderBook b = seeded_book(50);
    std::mt19937_64 rng(13);
    Seq id = 1;
    BookLevels levels;
    for (auto _ : state) {
        levels.bid_count = levels_per_update;
        levels.ask_count = 0;
        for (std::uint8_t i = 0; i < levels_per_update; ++i) {
            levels.bids[i] = PriceLevel{
                Px::from_units(static_cast<std::int64_t>(59'960 + rng() % 40)),
                Qty::from_units(static_cast<std::int64_t>(1 + rng() % 3))};
        }
        benchmark::DoNotOptimize(b);
        auto s = b.apply_update(levels, ++id);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BookUpdateBatchOfLevels)->Arg(1)->Arg(4)->Arg(16);

/// The same batch shape against std::map, so the comparison is like for like.
void BM_BookUpdateBatchStdMapBaseline(benchmark::State& state) {
    const auto levels_per_update = static_cast<int>(state.range(0));
    std::map<std::int64_t, std::int64_t, std::greater<>> bids;
    for (int i = 0; i < 50; ++i) {
        bids.emplace(60'000 - i, 1);
    }
    std::mt19937_64 rng(13);
    for (auto _ : state) {
        benchmark::DoNotOptimize(bids);
        for (int i = 0; i < levels_per_update; ++i) {
            const auto price = static_cast<std::int64_t>(59'960 + rng() % 40);
            const auto qty = static_cast<std::int64_t>(1 + rng() % 3);
            if (qty == 0) {
                bids.erase(price);
            } else {
                bids.insert_or_assign(price, qty);
            }
        }
        auto best = bids.empty() ? 0 : bids.begin()->first;
        benchmark::DoNotOptimize(best);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BookUpdateBatchStdMapBaseline)->Arg(1)->Arg(4)->Arg(16);

void BM_BestBidAskLookup(benchmark::State& state) {
    OrderBook b = seeded_book(50);
    for (auto _ : state) {
        benchmark::DoNotOptimize(b);
        auto q = b.bbo();
        benchmark::DoNotOptimize(q);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BestBidAskLookup);

void BM_SnapshotApplication(benchmark::State& state) {
    const auto depth = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        OrderBook b = seeded_book(depth);
        benchmark::DoNotOptimize(b);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SnapshotApplication)->Arg(50)->Arg(500);

/// Runs on every mutation in debug builds; measured so its release-build cost
/// is a known quantity rather than a guess.
void BM_InvariantAudit(benchmark::State& state) {
    OrderBook b = seeded_book(50);
    for (auto _ : state) {
        benchmark::DoNotOptimize(b);
        auto v = b.check_invariants();
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InvariantAudit);

// ------------------------------------------------- synchronization + codec

/// Sequence validation plus book application, i.e. the whole per-update cost
/// once a session is Ready.
void BM_SynchronizerUpdate(benchmark::State& state) {
    ManualClock clock(0, 0);
    SyncConfig config;
    config.max_depth = 50;
    BookSynchronizer sync(Symbol("BTCUSDT"), config, clock);
    sync.on_connected();
    sync.on_subscribed();

    exchange::BookSnapshotEvent snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.last_update_id = 100;
    snap.levels.bids[0] = PriceLevel{Px::from_units(60'000), Qty::from_units(1)};
    snap.levels.asks[0] = PriceLevel{Px::from_units(60'001), Qty::from_units(1)};
    snap.levels.bid_count = 1;
    snap.levels.ask_count = 1;
    static_cast<void>(sync.on_snapshot(snap));

    exchange::BookUpdateEvent u;
    u.symbol = Symbol("BTCUSDT");
    u.levels.bids[0] = PriceLevel{Px::from_units(60'000), Qty::from_units(2)};
    u.levels.bid_count = 1;
    Seq id = 100;

    for (auto _ : state) {
        u.first_update_id = id + 1;
        u.final_update_id = id + 1;
        ++id;
        benchmark::DoNotOptimize(u);
        auto s = sync.on_update(u);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["applied"] = static_cast<double>(sync.diagnostics().updates_applied);
}
BENCHMARK(BM_SynchronizerUpdate);

/// Normalization cost: JSON in, normalized events out. Runs on the md-io
/// thread, so it is bounded by network latency rather than competing with it —
/// measured to know the budget, not because it is on the trading thread.
void BM_CodecDepthUpdate(benchmark::State& state) {
    exchange::binance::BinanceCodec codec;
    std::vector<exchange::MarketDataEvent> out;
    out.reserve(8);
    const std::string frame =
        R"({"e":"depthUpdate","E":1786924800123,"s":"BTCUSDT","U":157,"u":160,)"
        R"("b":[["63120.73000000","0.28760000"],["63120.72000000","0.00115000"],)"
        R"(["63120.71000000","0.00016000"]],)"
        R"("a":[["63120.74000000","8.03602000"],["63120.75000000","0.00034000"]]})";

    for (auto _ : state) {
        out.clear();
        benchmark::DoNotOptimize(out);
        auto s = codec.decode_stream_frame(frame, 1'000, out);
        benchmark::DoNotOptimize(s);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}
BENCHMARK(BM_CodecDepthUpdate);

void BM_CodecTrade(benchmark::State& state) {
    exchange::binance::BinanceCodec codec;
    std::vector<exchange::MarketDataEvent> out;
    out.reserve(4);
    const std::string frame =
        R"({"e":"trade","E":1786924800123,"s":"BTCUSDT","t":12345,)"
        R"("p":"63120.50000000","q":"0.00500000","T":1786924800100,"m":true})";
    for (auto _ : state) {
        out.clear();
        benchmark::DoNotOptimize(out);
        auto s = codec.decode_stream_frame(frame, 1'000, out);
        benchmark::DoNotOptimize(s);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CodecTrade);

}  // namespace
}  // namespace mm::book
