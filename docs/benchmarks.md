# Benchmarks

Every performance claim in this repository points here. Nothing in the codebase
asserts a latency number that is not measured, and no optimization is made
without a measurement that justified it.

## How to run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/mm_bench --benchmark_min_time=0.5s
```

For numbers you intend to quote, use a quiet machine, disable turbo/SMT drift as
far as the platform allows, and prefer `-DMMX_NATIVE_ARCH=ON`.

## Baseline — Phase 2 primitives

Intel Core Ultra 5 125U, 14 threads, GCC 13.3, `-O2 -g`, `RelWithDebInfo`,
shared desktop machine (i.e. noisy — treat these as relative, not as a contract).

| Benchmark                              | Time      | Notes                                        |
| -------------------------------------- | --------- | -------------------------------------------- |
| `FixedParse` ("60123.45000000")        | 25.2 ns   | exchange decimal string → int64, exact        |
| `FixedFormat`                          | 33.9 ns   | int64 → shortest exact decimal                |
| `NotionalOf` (128-bit product)         | 2.67 ns   | price × qty, overflow-free                    |
| `RoundToStep`                          | 1.05 ns   | tick/lot snapping                             |
| `MicroPrice`                           | 2.98 ns   | size-weighted fair value                      |
| `SpscRingRoundTrip` (same thread)      | 2.98 ns   | instruction cost, no coherence traffic        |
| `SpscRingEmplace` (same thread)        | 1.67 ns   | in-place construction, no temporary           |
| **`SpscRingCrossThread`**              | **39.5 ns** | **producer→consumer, different cores**      |
| `SeqlockStore` (1 KB, uncontended)     | 52.4 ns   | telemetry publication                         |
| `SeqlockStore` + 3 hot readers         | 107 ns    | see "seqlock contention" below                |
| `SeqlockStore` + 3 version-gated readers | 98.6 ns | prescribed reader pattern                     |
| `ObjectPoolAcquireRelease`             | 11.5 ns   | pooled                                        |
| `HeapAllocateFree` (baseline)          | 19.1 ns   | `new`/`delete` — the thing the pool avoids    |
| `HistogramRecord`                      | 2.10 ns   | one latency sample                            |
| `HistogramPercentile`                  | 224 ns    | off hot path; runs at snapshot cadence        |
| `CounterIncrement`                     | 4.40 ns   | relaxed atomic                                |
| `steady_ns()`                          | 14.6 ns   | `clock_gettime(CLOCK_MONOTONIC)` via vDSO     |

## What these numbers mean for the design

**`steady_ns()` costs 14.6 ns**, which is more than most of the operations being
timed. Four stamps per event (`recv`, `parse`, `process`, plus one for the
strategy call) costs ~60 ns — a real fraction of the ingress budget. That is an
accepted price: latency that is not measured is latency that is not managed. It
does mean instrumentation must be deliberate rather than sprinkled, and that
sub-microsecond components should be timed in aggregate rather than individually.

**The pool beats the heap by only ~1.7×** in a benchmark where the allocator's
free list is hot and uncontended. That ratio understates the real benefit: the
pool's value is not its mean but its *absence of a tail* — no lock, no page
fault, no arena refill, and a hard bound on memory. The mean is the least
interesting property of an allocator on a trading thread.

**Cross-thread ring transfer is 39.5 ns**, an order of magnitude above the
same-thread 2.98 ns. That gap is cache-line ownership transfer between cores and
is irreducible for any cross-thread queue; it is the floor for the
market-data→trading hop and is included in the ingress latency budget rather
than wished away.

### Seqlock contention — an honest correction

The uncontended seqlock write is 52 ns; with three readers spinning on it, the
writer's cost roughly doubles to 107 ns. The writer is never *blocked* — it
never inspects reader state, which is the property the design depends on — but
it does pay cache-coherence cost when readers pull the payload lines into a
shared state. Claiming "readers have zero effect on the writer" would have been
wrong, and the benchmark is here specifically to keep that claim honest.

Two things make it a non-issue in practice, and one rule follows from it:

- The snapshot is published at 10 Hz. Even the contended 107 ns costs ~1 µs per
  second of trading — six orders of magnitude below the budget.
- **Rule for readers:** poll `version()` and copy only when it changes. A
  version check touches one cache line instead of the whole payload. The
  monitoring thread follows this pattern; `BM_SeqlockStoreWithVersionCheckingReaders`
  measures it.

## Benchmark hygiene

Two benchmarks in the first draft of this file were measuring nothing:

- `NotionalOf`, `RoundToStep` and `MicroPrice` reported 0.125 ns — about half a
  cycle — because their inputs were loop-invariant and the computation was
  hoisted out of the loop entirely. Fixed by varying an input per iteration.
- `HistogramRecord` reported 0.123 ns (8.1 G samples/s, which is not physically
  possible on this machine). `benchmark::ClobberMemory()` was not sufficient;
  GCC still sank the bucket update because nothing ever read the histogram. A
  direct standalone measurement gave 2.86 ns/op, and after adding
  `DoNotOptimize(h)` around the call plus consuming `p99()` at the end, the
  benchmark reports 2.10 ns — consistent with ground truth.

Both are recorded here rather than quietly corrected, because a benchmark that
measures nothing is worse than no benchmark: it produces a number people trust.
Any new benchmark reporting a sub-nanosecond time for non-trivial work should be
assumed broken until proven otherwise.

## Phase 3 — exchange boundary

Same machine and build. Only the four operations that run per-event on the
trading thread are measured; the rest of the exchange layer runs on an I/O
thread or at startup, and a number nobody acts on is noise.

| Benchmark | Time | Notes |
| --- | --- | --- |
| `ValidateOrder` (accepting) | 29.4 ns | full tick/lot/notional/band/capability check |
| `ValidateOrder` (rejecting) | 44.3 ns | see below |
| `ValidateExecutionEvent` | 11.1 ns | self-contradiction check on every inbound event |
| `ExecutionEventThroughRing` | 15.6 ns | 680-byte envelope, same thread |
| `MarketDataEventThroughRing` | 21.7 ns | 624-byte envelope, 16-level depth update |
| `TradeEventThroughRing` | 22.8 ns | 60 bytes of payload in the same 624-byte slot |
| `SessionTransitionCheck` | 2.81 ns | not hot; measured to confirm it is not accidentally costly |

Two results worth reading carefully:

**The rejecting path is 50% slower than the accepting one (44.3 ns vs 29.4 ns),
which is the opposite of what short-circuiting suggests.** The cause is
`ExchangeError`'s detail string: acceptance returns `ExchangeError::none()` and
copies nothing, while every rejection copies a message into an
`InlineString<128>`. It is the right trade — a diagnosable rejection is worth
15 ns — but it means a *burst* of rejections costs more than a burst of
acceptances, which is relevant to the safety layer's reject-rate detector.

**A trade costs the same as a depth update (22.8 ns vs 21.7 ns)** because both
copy the full 624-byte envelope regardless of payload. That is the measured
price of the single-ordered-ring decision described in
[exchange-interface.md §6](exchange-interface.md): a trade and the depth update
it caused stay in venue order without the trading thread re-interleaving two
streams. Paid knowingly.

Envelope sizing was itself driven by measurement rather than taste. At
`kMaxOrdersPerSnapshotEvent = 8` the `ExecutionEvent` union was 1432 bytes, so
every 224-byte fill paid a 6× copy tax for a payload that only appears during
reconciliation. Reducing the chunk to 4 and removing a redundant `error` field
brought it to 680 bytes.

## Not yet benchmarked

These arrive with their phases and are listed so the gaps are explicit:

order-book apply/snapshot · market-state generation · strategy invocation ·
quote-decision diffing · risk validation · OMS state transition · order
serialization (venue encode/decode) · end-to-end tick-to-trade.
