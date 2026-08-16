/// Benchmarks for the primitives every hot-path component is built from.
///
/// These exist so that latency claims in this repository are measured rather
/// than asserted. Run with:
///     ./build/mm_bench --benchmark_min_time=1s
/// and prefer a quiet machine; the numbers below are meaningful relative to
/// each other, not as absolute guarantees on unknown hardware.

#include <benchmark/benchmark.h>

#include <thread>

#include "mm/common/Fixed.hpp"
#include "mm/common/LatencyHistogram.hpp"
#include "mm/common/Metrics.hpp"
#include "mm/common/ObjectPool.hpp"
#include "mm/common/Seqlock.hpp"
#include "mm/common/SpscRing.hpp"
#include "mm/common/Types.hpp"

namespace mm {
namespace {

struct MdEvent {
    Symbol symbol{};
    EventStamps stamps{};
    Px px{};
    Qty qty{};
    Seq seq = 0;
    Side side = Side::Buy;
};

// --------------------------------------------------------------- fixed point

void BM_FixedParse(benchmark::State& state) {
    const std::string_view text = "60123.45000000";
    for (auto _ : state) {
        Px p;
        benchmark::DoNotOptimize(Px::parse(text, p));
        benchmark::DoNotOptimize(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixedParse);

void BM_FixedFormat(benchmark::State& state) {
    const Px p = Px::from_raw(6'012'345'000'000);
    for (auto _ : state) {
        benchmark::DoNotOptimize(p.to_string());
    }
}
BENCHMARK(BM_FixedFormat);

void BM_NotionalOf(benchmark::State& state) {
    std::int64_t px_raw = 6'012'345'000'000;
    std::int64_t qty_raw = 150'000'000;
    for (auto _ : state) {
        benchmark::DoNotOptimize(px_raw);
        benchmark::DoNotOptimize(qty_raw);
        auto n = notional_of(Px::from_raw(px_raw), Qty::from_raw(qty_raw));
        benchmark::DoNotOptimize(n);
        ++px_raw;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NotionalOf);

void BM_RoundToStep(benchmark::State& state) {
    std::int64_t px_raw = 6'012'345'678'901;
    const Px tick = Px::from_raw(1'000'000);
    for (auto _ : state) {
        benchmark::DoNotOptimize(px_raw);
        auto r = round_to_step(Px::from_raw(px_raw), tick, Rounding::Nearest);
        benchmark::DoNotOptimize(r);
        ++px_raw;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RoundToStep);

void BM_MicroPrice(benchmark::State& state) {
    BestBidAsk bbo;
    bbo.bid_px = Px::from_raw(6'012'300'000'000);
    bbo.ask_px = Px::from_raw(6'012'400'000'000);
    bbo.bid_qty = Qty::from_raw(150'000'000);
    bbo.ask_qty = Qty::from_raw(90'000'000);
    for (auto _ : state) {
        benchmark::DoNotOptimize(bbo);
        auto mp = bbo.micro_price();
        benchmark::DoNotOptimize(mp);
        bbo.bid_qty = Qty::from_raw(bbo.bid_qty.raw() + 1);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MicroPrice);

// --------------------------------------------------------------- spsc ring

/// Same-thread push/pop: isolates the ring's instruction cost from the cache
/// coherence traffic that dominates the cross-core case below.
void BM_SpscRingRoundTripSameThread(benchmark::State& state) {
    SpscRing<MdEvent, 1024> ring;
    MdEvent in;
    in.symbol = Symbol("BTCUSDT");
    in.px = Px::from_raw(6'012'345'000'000);
    MdEvent out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.try_push(in));
        benchmark::DoNotOptimize(ring.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRingRoundTripSameThread);

void BM_SpscRingEmplace(benchmark::State& state) {
    SpscRing<MdEvent, 1024> ring;
    MdEvent out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.emplace_with([](MdEvent& e) {
            e.px = Px::from_raw(6'012'345'000'000);
            e.seq = 1;
        }));
        benchmark::DoNotOptimize(ring.try_pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRingEmplace);

/// The number that actually matters: producer and consumer on different cores,
/// which is how the ring is used between the I/O and trading threads.
void BM_SpscRingCrossThreadThroughput(benchmark::State& state) {
    SpscRing<MdEvent, 4096> ring;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> consumed{0};

    std::thread consumer([&] {
        MdEvent out;
        while (!stop.load(std::memory_order_relaxed)) {
            if (ring.try_pop(out)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        while (ring.try_pop(out)) {
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    MdEvent in;
    in.symbol = Symbol("BTCUSDT");
    std::uint64_t produced = 0;
    for (auto _ : state) {
        while (!ring.try_push(in)) {
            // Producer-side backpressure is part of the measurement.
        }
        ++produced;
    }
    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(static_cast<std::int64_t>(produced));
}
BENCHMARK(BM_SpscRingCrossThreadThroughput)->UseRealTime();

// ----------------------------------------------------------------- seqlock

struct Snapshot {
    std::array<std::uint64_t, 128> data{};  // ~1 KB, telemetry-sized
};

void BM_SeqlockStore(benchmark::State& state) {
    Seqlock<Snapshot> lock;
    Snapshot s;
    for (auto _ : state) {
        lock.store(s);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(Snapshot)));
}
BENCHMARK(BM_SeqlockStore);

/// Proves the rule "the dashboard is never in the hot path": the writer's cost
/// must not change when readers are hammering it.
void BM_SeqlockStoreWithReaders(benchmark::State& state) {
    Seqlock<Snapshot> lock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            Snapshot out;
            while (!stop.load(std::memory_order_relaxed)) {
                bool got = lock.load(out);
                benchmark::DoNotOptimize(got);
            }
        });
    }

    Snapshot s;
    for (auto _ : state) {
        lock.store(s);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        r.join();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SeqlockStoreWithReaders)->UseRealTime();

/// The prescribed reader pattern: check the version first and copy only when it
/// changed. Readers polling a 10 Hz publisher then touch one cache line instead
/// of the whole payload, and the writer's cost stays close to the uncontended
/// case. Compare against BM_SeqlockStoreWithReaders.
void BM_SeqlockStoreWithVersionCheckingReaders(benchmark::State& state) {
    Seqlock<Snapshot> lock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            Snapshot out;
            std::uint64_t seen = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t v = lock.version();
                if (v != seen) {
                    seen = v;
                    bool got = lock.load(out);
                    benchmark::DoNotOptimize(got);
                }
            }
        });
    }

    Snapshot s;
    for (auto _ : state) {
        lock.store(s);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        r.join();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SeqlockStoreWithVersionCheckingReaders)->UseRealTime();

// ------------------------------------------------------------- object pool

void BM_ObjectPoolAcquireRelease(benchmark::State& state) {
    ObjectPool<MdEvent, 1024> pool;
    for (auto _ : state) {
        auto idx = pool.acquire();
        benchmark::DoNotOptimize(idx);
        auto released = pool.release(idx);
        benchmark::DoNotOptimize(released);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolAcquireRelease);

/// The baseline the pool exists to avoid.
void BM_HeapAllocateFree(benchmark::State& state) {
    for (auto _ : state) {
        auto* p = new MdEvent();
        benchmark::DoNotOptimize(p);
        delete p;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapAllocateFree);

// -------------------------------------------------------------- histogram

void BM_HistogramRecord(benchmark::State& state) {
    LatencyHistogram h;
    Nanos v = 1;
    for (auto _ : state) {
        // DoNotOptimize(h) forces the histogram to be treated as escaped.
        // ClobberMemory alone was not enough: GCC still sank the bucket update,
        // reporting 0.12 ns/op against a directly measured 2.9 ns/op.
        benchmark::DoNotOptimize(h);
        h.record(v);
        benchmark::DoNotOptimize(h);
        v = (v * 1103515245 + 12345) & 0xFFFFF;
    }
    state.SetItemsProcessed(state.iterations());
    // Consume the result so the whole loop cannot be proven dead.
    state.counters["p99_ns"] = static_cast<double>(h.p99());
}
BENCHMARK(BM_HistogramRecord);

void BM_HistogramPercentile(benchmark::State& state) {
    LatencyHistogram h;
    for (Nanos v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(h.p99());
    }
}
BENCHMARK(BM_HistogramPercentile);

// ---------------------------------------------------------------- metrics

void BM_CounterIncrement(benchmark::State& state) {
    Counter c;
    for (auto _ : state) {
        c.inc();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CounterIncrement);

// ------------------------------------------------------------------- clock

void BM_SteadyNow(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(steady_ns());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SteadyNow);

}  // namespace
}  // namespace mm

BENCHMARK_MAIN();
