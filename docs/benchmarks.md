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

## Phase 4 — order book, synchronization, Binance codec

Same machine; note it is a shared laptop and was measurably noisier during this
run than during Phase 2. Absolute numbers moved by up to 40% between repeats, so
treat ratios as the signal and absolutes as indicative.

| Benchmark | Time | Notes |
| --- | --- | --- |
| `BestBidAskLookup` | 1.3 ns | index 0 of a vector |
| `BookUpdateNearTouch` (1 level) | 77 ns | includes per-message validation |
| `BookUpdateAcrossFullDepth` (1 level) | 90 ns | insert far from the touch |
| `BookUpdateBatchOfLevels/16` | 1101 ns | ~69 ns/level, realistic message shape |
| `BookUpdateBatchStdMapBaseline/16` | 597 ns | ~37 ns/level — **faster** |
| `SnapshotApplication/50` | 907 ns | sort, dedupe, validate, swap in |
| `SnapshotApplication/500` | 6808 ns | ~14 ns/level |
| `InvariantAudit` (100 levels) | 141 ns | ~1.4 ns/level |
| `SynchronizerUpdate` | 92 ns | continuity check + book application |
| `CodecDepthUpdate` (5 levels, ~300 B) | 5.9 µs | JSON → normalized, io thread |
| `CodecTrade` | 3.4 µs | JSON → normalized, io thread |

### The data-structure result, which went against the design

`std::map` is repeatably **2–3× faster** than this project's sorted-vector book
on mean update throughput, at every batch size measured. That is the opposite of
the rationale originally written into `OrderBook.hpp`.

The vector is kept, on grounds the throughput number does not capture — no
allocation on the feed path, bounded footprint, contiguous top-N reads — and the
reasoning, including what would replace it if profiling ever justified the work,
is in [order-book.md §1](order-book.md). The point here is that the number is
recorded rather than the claim.

### JSON decode is the largest single cost in the ingress path

At ~5.9 µs, decoding one depth message costs more than everything else in this
table combined. That is acceptable *today* because it happens on the md-io
thread, where it is dwarfed by network latency and cannot delay the trading
thread. It is worth stating plainly rather than burying:

- It bounds single-threaded feed throughput to roughly 170k depth messages/sec.
  Binance's `@depth@100ms` on a handful of symbols is nowhere near that.
- If more symbols or a faster stream ever approach it, the fix is local to the
  adapter — `nlohmann::json` builds a DOM and allocates; a streaming or
  SAX-style parse would remove most of the cost — and no interface changes.

### Benchmark hygiene, again

The first version of the book benchmarks constructed a `BookLevels` — a 520-byte
struct with two 16-element arrays — **inside** the timed loop. Zero-initialising
it dominated the measurement, reporting the book at 161 ns/update when the
operation itself was around 62 ns, and the `std::map` baseline paid no
equivalent cost, so the comparison was invalid in the direction that flattered
neither structure honestly.

Two further corrections in the same pass: the RNG was hoisted out of the timed
region, and the map baseline was made to do the same trailing work as the book
(read the touch after mutating, enforce the crossed-book check) so the two sides
compare like for like.

Same lesson as Phase 2: a benchmark that measures the wrong thing is worse than
no benchmark, because it produces a number people design against.

## Phase 5 — strategy runtime

Measured with the reference strategy, which is deliberately trivial. These are
therefore a **floor for the runtime's overhead**, not a prediction of what a
real strategy will cost.

| Benchmark | Time | Notes |
| --- | --- | --- |
| `RuntimeGate` | 6.1 ns | all gates, without building a context |
| `ContextConstruction` | 38.4 ns | the immutable view handed to the strategy |
| `StrategyInvocation` | 52.4 ns | the strategy call alone |
| `IntentValidation` | 18.1 ns | tick, lot, notional, provenance, distance |
| **`FullRuntimePath`** | **132 ns** | gate, invoke, time, validate, stamp |

The parts sum to about 109 ns against a 132 ns whole. The difference is the
runtime's own bookkeeping — recording the latency histogram, stamping identity
and sequence, updating metrics — which is worth stating because it is the cost
of the watchdog rather than of the strategy.

`gate()` at 6.1 ns versus 38.4 ns to build a context is why the gate is public:
the engine can ask whether an evaluation is worth assembling a context for
before assembling one.

## Phase 6 — quote manager

| Benchmark | Time | Notes |
| --- | --- | --- |
| `EvaluateRejected` | 85.6 ns | screening only; no slot considered |
| `DisableAllQuotes` | 89.2 ns | the withdrawal path behind every kill switch |
| `EvaluateNew` | 143 ns | validation plus two New actions |
| **`EvaluateKeep`** | **152 ns** | **the steady state — quotes rest, nothing material moved** |
| `EvaluateReplace` | 325 ns | full validation on both sides plus two actions |

`EvaluateKeep` is the number that matters: it is what the trading thread pays on
the large majority of evaluations, when the market has not moved enough to
justify rewriting anything. Replace costs roughly twice that because it re-runs
Phase 3 validation on both sides and constructs two actions.

Together with Phase 5, one full strategy-to-action cycle in the steady state
costs about 280 ns (132 ns runtime + 152 ns quote manager) — against a
cross-thread ring hop of 40 ns and a network path measured in tens of
microseconds.

## Phase 7 — risk engine

Shared laptop, noticeably noisier during this run than earlier phases; treat
ratios as the signal and absolutes as indicative.

| Path | Time | Notes |
| --- | --- | --- |
| `WorstCaseExposure` | 10–14 ns | the exposure arithmetic alone |
| `RiskKilledRejection` | 74–110 ns | state gate only |
| `RiskCancel` | 79–112 ns | must stay available in every state |
| **`RiskApprove`** | **~271 ns** | **the common path: limits, bands, venue rules, exposure** |
| `RiskReject` | ~340 ns | rejection by a hard limit |
| `RiskReduce` | 630–1490 ns | binary search over the lot grid |

Reduction is the most expensive path by construction: each search step re-runs
the real bounds and exposure checks, which is what keeps one source of truth for
the limits rather than a second algebraic copy that could drift.

### A benchmark that pointed at a real inefficiency

The first measurement had **rejection costing more than reduction** — 1318 ns
against 1058 — which is backwards, since a rejection should do strictly less
work. The cause was real: a reducible failure ran the full binary search even
when there was no room for a single lot, then rejected anyway. Testing one lot
before searching cut rejection to ~340 ns.

The number was the only reason anyone looked.

### The full pipeline, steady state

Strategy runtime 132 ns + quote manager 152 ns + risk ~271 ns ≈ **555 ns** from
market event to an approved order action, against a cross-thread ring hop of
40 ns and a network path measured in tens of microseconds.


## Phase 8 — OMS

Same machine, `-DCMAKE_BUILD_TYPE=Release`, pinned with `taskset -c 2`,
`--benchmark_min_time=0.4s`, taken at a 1-minute load average of ~2.

**These are indicative, not a latency contract, and the margin is larger than
usual.** This is a shared desktop, and repeating the identical run later at a
load average of ~24 produced figures **6-8x higher across every single
benchmark** — `OmsSubmitNew/4` read 4903 ns against 602 ns, `OmsWorkingState/16`
1104 ns against 171 ns. Taking the minimum across seven repetitions did not
rescue it, which says the contention was continuous rather than intermittent.

So the honest statement is: the *ratios* below are meaningful (they were all
measured under the same conditions, and the scaling with order count is
reproducible), and the absolute values are an estimate of the uncontended cost
that a quiet machine would need to confirm. Nothing in the engine asserts a
latency number derived from these.

| Benchmark | Time | What it covers |
| --- | --- | --- |
| `OmsLegalTransitionCheck` | 5.27 ns | the transition-legality table |
| `OmsDuplicateTradeIdLookup` | 84.0 ns | a 32-entry trade-id ring, mixed hits and misses |
| `OmsSubmitNew` / 4 resident | 602 ns | approval → identity → journal → adapter call |
| `OmsSubmitNew` / 32 resident | 843 ns | " |
| `OmsSubmitNew` / 256 resident | 2777 ns | " |
| `OmsProcessAcknowledgement` | ~1010 ns | drain the ring, attribute, transition |
| `OmsProcessFill` | 854 ns | attribute, dedupe, VWAP, transition |
| `OmsLookupByClientId` / 16 · 256 · 1024 | 23.6 · 33.4 · 36.0 ns | reconciliation's per-row lookup |
| `OmsReconcile` / 16 · 256 · 1024 | 0.88 · 16.0 · 74.4 µs | full snapshot comparison |
| `OmsWorkingState` / 16 · 256 · 1024 | 171 ns · 1.98 · 7.57 µs | what the quote manager reads each cycle |
| `OmsExposure` / 16 · 256 · 1024 | 290 ns · 4.27 · 17.5 µs | what risk reads each cycle |

### The scan cost is a record-layout cost

`OmsWorkingState` and `OmsExposure` are linear in resident orders, and the slope
is steeper than the work justifies. The cause is measurable rather than
mysterious:

```
sizeof(OrderRecord)  = 1472 bytes
  of which SeenTradeIds = 1112 bytes   (32 × 34-byte trade ids)
```

Every scan walks 1472 bytes per order to read a symbol, an owner, a state and
two quantities — about 360 bytes of live data. 1024 orders × 1472 B ≈ 1.5 MB,
and 1.5 MB at this machine's L2/L3 bandwidth is almost exactly the 7.57 µs
measured. The number is not a mystery; it is the record size.

**Not changed in this phase, deliberately.** The duplicate-trade-id ring is cold
data — read only in `apply_fill` — sitting on three hot scan paths, and moving it
into a parallel array would cut the scanned footprint roughly 4×. But a market
maker's resident set is tens of orders, not thousands: at 16 orders the same
publication costs 171 ns, which is noise next to the 40 ns ring hop and the
tens of microseconds of network path. Phase 8's instruction was to avoid
restructuring data before measuring, and having now measured, the honest
conclusion is that the cost does not bind at realistic sizes. The split is
recorded here so that if a deployment ever holds hundreds of resident orders,
the fix is already identified rather than rediscovered.

### One scan that was worth removing

Looking at the scan costs above prompted a read of the other full-table walks,
which turned up `release_deferred()` iterating all `max_orders` deferral slots on
**every** order resolution — 221 KB at the shipped `max_orders: 1024`, to almost
always find nothing, since deferrals exist only during a cancel-then-new window.
An occupancy counter short-circuits it. That one was found by reading rather than
by measuring; the benchmarks are what made anybody look.

### Two benchmarks that were measuring the wrong thing

The first run reported `OmsReconcile` at 11–12 ns regardless of size, and
`OmsWorkingState` getting *faster* as the order count grew. Both were false.
`populate()` advanced the clock by 1 µs against a mock acknowledgement latency of
500 µs, so no order in any fixture ever reached `WORKING`: reconciliation was
comparing against an empty snapshot, and the working-state scan was matching
nothing. The tell was `items_per_second=0`, which is not a plausible measurement.

The fixtures now assert they produced live orders and `SkipWithError` if they did
not, because a benchmark that silently measures an empty table reports a very
good number.

The first `OmsSubmitNew` also read 24 µs. Two causes, both benchmark bugs rather
than engine ones: the resident set grew by 4096 orders during the timed window
so the average was dominated by the tail, and the mock adapter's own map inserts
and event scheduling were inside the timed region. Submit is now measured at a
fixed resident size against a do-nothing adapter, which is what "the cost of a
submit" should mean.

### The full pipeline, steady state

Strategy runtime 132 ns + quote manager 152 ns + risk ~271 ns + OMS submit
~602 ns ≈ **1.16 µs** from market event to a request handed to an adapter,
against a cross-thread ring hop of 40 ns and a network path measured in tens of
microseconds.

The components were measured in different phases under different machine load,
so this sum is an order-of-magnitude statement rather than an addition of
comparable quantities. What it establishes is the thing that matters for design:
the engine's own cost is a small fraction of the network path, so correctness in
the lifecycle is worth more than nanoseconds in it. An end-to-end tick-to-trade
benchmark — one measurement, one set of conditions — is listed below as not yet
done, and is what would actually settle the number.


## Phase 9 — paper execution

Same machine, `-DCMAKE_BUILD_TYPE=Release`, pinned with `taskset -c 6`,
`--benchmark_min_time=0.2s`, on a shared desktop. The caveat from the Phase 8
section applies in full: **treat the ratios as meaningful and the absolute
values as an estimate a quiet machine would need to confirm.**

> **Paper execution performance is not equivalent to real exchange latency, and
> nothing here is a measurement of any venue.** What these numbers bound is the
> engine-side cost of running against the simulator — how much of a paper
> session is harness rather than code under test. A real venue's round trip is
> tens of microseconds to milliseconds of network, against which every figure
> below is noise.

| Benchmark | Time | What it covers |
| --- | --- | --- |
| `PaperSubmit` | 183 ns | validation, identity, queueing — no event work |
| `PaperCancel` | 361 ns | lookup plus request queueing |
| `PaperReplace` | 388 ns | " |
| `PaperMatchAgainstLevels` | 329 ns | the matcher alone, 16 levels, executing |
| `PaperLookupByClientId` / 16 · 256 · 1024 | 17.4 · 36.9 · 90.1 ns | what cancel, replace and query all pay first |
| `PaperMatchOnMarketUpdate` / 8 · 64 · 512 resting | 186 ns · 1.37 · 11.97 µs | the scan and priority sort paid on **every** book update |
| `PaperRequestToEvent` | 1371 ns | one request processed and its answer delivered |
| `PaperEventPublication` / 1 · 16 · 256 | 6.83 · 17.4 · 235 µs | draining a batch of due events |

### Reading these

**The matching pass is linear in resting orders and is paid on every update,
fill or no fill** — roughly 23 ns per resting order. At a market maker's real
working set (a handful of orders) that is a rounding error; the 512 figure is
there to make the slope visible rather than to describe a realistic load.

**`PaperEventPublication/1` at 6.8 µs is mostly harness.** Each iteration
rebuilds a venue inside `PauseTiming`, and the timer-toggle overhead dominates a
single event. The per-event cost is better read from the 256 case: 235 µs / 256
≈ 918 ns, which includes processing the request as well as delivering the
answer.

### Two fixtures that measured nothing

Both were caught by assertions that the fixture had produced what it claimed,
added after Phase 8's identical lesson:

- `populate()` advanced the clock 10 µs per order against a 2 ms request-plus-ack
  round trip, so no order ever reached the venue and every "N resting orders"
  benchmark ran against an empty table.
- `bench_order()` priced sells *below* the bid, so half of them crossed on
  arrival and executed as takers instead of resting.

The fixtures now `SkipWithError` unless they produced exactly the requested
number of resting orders. A benchmark that silently measures an empty container
reports a very good number.

## Not yet benchmarked

These arrive with their phases and are listed so the gaps are explicit:

order serialization · end-to-end tick-to-trade.
