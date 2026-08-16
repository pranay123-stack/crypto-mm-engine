# Concurrency model

The engine is **single-writer for all trading state**. Exactly one thread mutates
order books, strategy instances, quote state, risk counters, OMS records, and
portfolio. Every other thread either feeds it through a bounded SPSC ring or
observes a snapshot it publishes.

---

## 1. Threads and ownership

| # | Thread          | Mutable state owned                                        | Blocking I/O | Steady-state allocation |
| - | --------------- | ---------------------------------------------------------- | ------------ | ----------------------- |
| 1 | `md-io`         | WS session, TLS ctx, read buffers, parse scratch            | epoll        | none (reused buffers)   |
| 2 | **`trading`**   | order books, strategies, quote mgr, risk, OMS, portfolio    | **never**    | **none**                |
| 3 | `exec-io`       | HTTP conns, listen key, HMAC scratch, in-flight map         | epoll        | none (pooled)           |
| 4 | `journal`       | file handles, write buffers                                 | write/fsync  | none (pooled)           |
| 5 | `monitor`       | HTTP server, snapshot copies                                | epoll        | unconstrained (off-path)|

A piece of state has exactly one owning thread. There is no shared mutable state
that two threads write. Where a value must cross a thread boundary it is either
*copied into a ring* or *published through a seqlock* — never aliased.

`md-io` and `exec-io` may share one `io_context` in low-symbol-count
deployments (config: `io.threads`), or run separate contexts pinned to separate
cores. The trading thread is never an Asio thread; it must not be at the mercy
of a handler that blocks.

### CPU pinning

Optional, config-driven (`io.cpu_affinity`). When enabled the trading thread is
pinned to an isolated core. Pinning is off by default because on a shared
machine it makes latency worse, not better. Benchmarks report both.

---

## 2. Message flow

```
   md-io ──MarketDataEvent──►┐
                             │
 exec-io ──ExecutionReport──►┤
                             ├──► trading ──OrderRequest───► exec-io
 monitor ──ControlCommand───►┤            ──JournalRecord──► journal
                             │            ──TelemetrySnapshot (seqlock) ──► monitor
                             ┘
```

Each arrow is one `SpscRing<T, N>` — a single producer thread and a single
consumer thread, statically paired at construction. There is no MPSC ring and no
work-stealing anywhere in the system; when N producers exist (multiple MD
connections), each gets its own ring and the trading thread polls them in a
fixed, documented order.

### Polling order and fairness

The trading thread's loop drains, in this order:

1. `ControlCommand` — operator commands (kill, pause) must never be starved.
2. `ExecutionReport` — order truth before decisions built on it.
3. `MarketDataEvent` — bounded batch per iteration (`kMdBatch`) so a data burst
   cannot starve steps 1–2.
4. Timers — strategy `on_timer`, staleness checks, reconciliation triggers.

Execution reports are drained before market data deliberately: quoting decisions
must be made against the freshest possible view of *our own* orders. Acting on a
new book with a stale order view is how a market maker doubles its size by
accident.

---

## 3. SpscRing

```cpp
template <class T, std::size_t N>  // N power of two
class SpscRing {
    alignas(64) std::atomic<std::size_t> head_;      // consumer writes
    alignas(64) std::atomic<std::size_t> tail_;      // producer writes
    alignas(64) std::size_t cached_head_;            // producer-private
                std::size_t cached_tail_;            // consumer-private
    alignas(64) std::array<T, N> buf_;
};
```

**Memory ordering.**

- Producer `push`: writes the slot, then `tail_.store(t + 1, memory_order_release)`.
- Consumer `pop`: `tail_.load(memory_order_acquire)`, then reads the slot.
  The release/acquire pair publishes the slot write. Symmetric for `head_`.
- Cached indices are plain loads; they are only refreshed (with an acquire load)
  when the cached value says the ring is full/empty. This removes a cache-line
  bounce per operation in the common case.

**Padding.** `head_` and `tail_` sit on separate 64-byte lines so producer and
consumer never contend on the same line. The cached copies live with their
owner.

**Blocking.** Never. `push` returns `false` when full; `pop` returns `false` when
empty. The caller decides — and the decision is always explicit:

| Ring              | Full-ring policy                                                  |
| ----------------- | ----------------------------------------------------------------- |
| MarketDataEvent   | **SAFE_MODE.** The trading thread is behind; the book is suspect.  |
| ExecutionReport   | **SAFE_MODE.** Losing an execution report loses order truth.       |
| ControlCommand    | Retry with backoff on the monitor thread; commands are rare.       |
| OrderRequest      | Reject the quote action, count it, surface it. Never spin.         |
| JournalRecord     | Drop, increment `journal_dropped`, alert. Trading must not stall.  |

Journal is the only ring permitted to lose data, and only because the
alternative — blocking the trading thread on disk — is strictly worse. The drop
counter is a first-class dashboard metric, not a silent statistic.

---

## 4. Seqlock

Telemetry is published, not queried. The trading thread writes a
`TelemetrySnapshot` into a `Seqlock<T>` at a fixed cadence (default 100 ms):

```cpp
// writer (trading thread) — wait-free
seq_.store(s + 1, release);   // odd: write in progress
std::atomic_thread_fence(release);
value_ = snapshot;
std::atomic_thread_fence(release);
seq_.store(s + 2, release);   // even: consistent

// reader (monitor thread) — may retry, never blocks the writer
do { s1 = seq_.load(acquire); if (s1 & 1) continue;
     copy = value_;
     s2 = seq_.load(acquire);
} while (s1 != s2);
```

This is the mechanism behind the hard rule *the dashboard is never in the hot
path*. A monitor thread that is slow, wedged, or dead cannot apply backpressure
to the writer: the writer never inspects reader state. A reader that races the
writer retries and gets the next snapshot. The worst case for the operator is a
stale panel; the worst case for trading is nothing.

Snapshot writing costs one memcpy of a POD aggregate at 10 Hz — measured in the
benchmarks, and off the per-event path entirely.

---

## 5. What is forbidden on the trading thread

Enforced by review, by the target graph, and by an ASan/TSan build in CI:

- No mutex, condition variable, or `std::async`.
- No `new`/`delete`/`malloc` in steady state. Orders come from a pre-sized pool;
  events are POD in ring slots; strings are fixed-capacity inline buffers.
- No file, socket, or DNS I/O.
- No JSON parsing or serialization. Adapters do that on I/O threads.
- No exceptions across the per-event path. Strategy calls are the exception —
  they are wrapped, and a throwing strategy is disabled (§6).
- No unbounded loop over containers whose size an adversarial market can grow.

## 6. Strategy execution budget

`on_market_update` / `on_timer` run **inline on the trading thread**. That is the
only way to keep decisions coherent with the book that produced them, but it
means a misbehaving plug-in can stall the engine.

Containment:

- Each call is timed with the steady clock; the duration feeds a histogram.
- Exceeding `strategy.budget_ns` (default 50 µs) counts a violation; N
  consecutive violations disable the strategy and trip SAFE_MODE.
- Exceptions are caught at the call site. A throwing strategy is disabled
  immediately, its orders are cancelled, and the engine enters SAFE_MODE. It is
  never re-entered without an operator action.

The budget cannot preempt a strategy that hangs forever — no in-process
mechanism can, short of a thread per call, which would destroy the coherence the
inline call exists to provide. That residual risk is documented in
[risk.md](risk.md) and mitigated operationally by the systemd watchdog: the
trading thread pets a watchdog timer, and a stalled thread restarts the process
into its recovery path.

---

## 7. Startup and shutdown ordering

**Startup** brings threads up in dependency order, and the trading thread does
not leave `BOOTING` until the readiness gate passes:

```
journal → monitor → exec-io → (auth, account, open orders, positions, balances)
       → trading (BOOTING) → md-io → book sync → reconcile → readiness gate → ACTIVE
```

**Shutdown** is cooperative and ordered; each step waits with a bounded timeout
and escalates on expiry:

```
1. TradingState := DRAINING       (no new quote actions accepted)
2. cancel-all per symbol; await ack   [timeout → escalate, log, continue]
3. drain ExecutionReport ring; settle OMS
4. persist state; flush journal to disk (fsync)
5. stop io_contexts; join md-io, exec-io
6. join journal, monitor
7. exit(0), or exit(1) if step 2 or 3 timed out — an unclean exit is reported,
   never disguised
```

Signals (`SIGINT`, `SIGTERM`) set an atomic flag read by the trading loop. No
work happens in the handler itself.
