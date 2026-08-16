# Architecture

> **Scope.** This repository is a *market-making execution platform*. It runs an
> already-finalized strategy in paper and live environments. It contains no
> research, no historical backtesting, no optimization, and no alpha discovery.
> Those belong to the separate quant research platform, which hands this
> repository a finalized strategy module plus its parameters.

---

## 1. Layer separation

```
┌───────────────────────────────────────────────────────────┐
│  QUANT RESEARCH PLATFORM            (separate repository)  │
│  research · historical data · backtesting · optimization   │
│  parameter selection · strategy validation                 │
└───────────────────────────┬───────────────────────────────┘
                            │  finalized strategy module
                            │  + validated parameters (YAML)
                            ▼
┌───────────────────────────────────────────────────────────┐
│  EXECUTION PLATFORM                    (this repository)   │
│  market data · order book · strategy runtime · quoting     │
│  risk · OMS · execution · portfolio · PnL · reconciliation │
│  monitoring · recovery · operations                        │
└───────────────────────────────────────────────────────────┘
```

The only artifacts that cross the boundary are (a) a C++ translation unit
implementing `IMarketMakingStrategy` and (b) a parameter block in YAML. Nothing
in this repository reads historical files, replays tapes, or fits parameters.

---

## 2. Module dependency graph

Dependencies point strictly downward. A module may only include headers from
modules at or below its own level. This is enforced at review time and by the
CMake target graph — each layer is a separate target with explicit
`target_link_libraries`.

```
 L8   apps/            mm_engine · mm_ctl · mm_probe
        │
 L7   engine/          Engine · SymbolContext · thread launch · wiring
        │
      ┌─┴──────────────┬────────────┬──────────────┬─────────────┐
      ▼                ▼            ▼              ▼             ▼
 L6 reconcile/     monitoring/  persistence/   execution/     (control)
      │                │            │              │
      ├────────────────┴────────────┴──────────────┤
      ▼                                            ▼
 L5 portfolio/        position · balance · pnl · fees
      │
 L4  oms/             order state machine · order store · client-id allocator
      │
 L3  risk/            authoritative pre-trade + continuous risk
      │
 L2  quote/           quote manager · quote lifecycle · replace policy
      │
 L1  strategy/        IMarketMakingStrategy · registry · normalized state
      │                        ▲
      │                        └── strategies/*    (plug-ins, no reverse dep)
      ▼
 L1  market_data/     session · sequencing · staleness · book sync
      │
 L0b orderbook/       local L2 book · invariants · BBO · depth
      │
 L0a exchange/common/ IExchangeMarketData · IExchangeExecution · normalized types
      │                        ▲
      │                        ├── exchange/binance/   (adapter)
      │                        └── exchange/paper/     (adapter)
      ▼
 L0  common/          types · fixed-point · time · ids · status · config
                      logging · metrics · spsc ring · seqlock · histogram
```

Two rules make the graph load-bearing rather than decorative:

1. **No module above L0a includes an exchange-specific header.** `binance/` and
   `paper/` are leaves. Nothing links against them except `engine/` (which
   constructs them from config) and their own tests.
2. **`strategies/` is a leaf that depends only on `strategy/` and `common/`.**
   No core module links against `strategies/`; the registry inverts the
   dependency.

---

## 3. The strategy boundary

The strategy is a pure decision function over normalized state. It is handed
what it needs to quote and nothing else.

```
  MarketDataEvent ──► OrderBook ──► StrategyMarketState (read-only view)
                                              │
                                              ▼
                                    IMarketMakingStrategy
                                              │
                                              ▼
                                       QuoteIntent               ← "what I want"
                                              │
                                              ▼
                                        QuoteManager             ← "what changes"
                                              │
                                              ▼
                                         RiskEngine              ← "what's allowed"
                                              │
                                              ▼
                                            OMS                  ← "what is real"
                                              │
                                              ▼
                                    IExchangeExecution
                                              │
                                  ┌───────────┴───────────┐
                                  ▼                       ▼
                          PaperExecution          BinanceExecution
```

What a strategy **can** do: read book/BBO/trades/position/balance/its own live
orders, and return a desired two-sided quote (or an instruction to pull).

What a strategy **cannot** do, structurally — not by convention:

| Capability                 | Why it is impossible                                       |
| -------------------------- | ---------------------------------------------------------- |
| Submit or cancel an order  | Has no reference to OMS or any exchange interface           |
| Reach the network          | `strategies/` does not link Asio, Beast, OpenSSL, or curl   |
| Mutate risk limits         | Receives `RiskView` by const ref; no setters exist          |
| Mutate position/balance    | Receives `Position`/`Balance` by value in a const state     |
| Bypass the quote pipeline  | Its only output is a value type; the engine calls it        |
| Block the trading thread   | Budgeted and watchdogged (§7); a slow strategy is disabled  |

`QuoteIntent` is deliberately *not* named `QuoteDecision` at the strategy
boundary in the implementation: the strategy expresses **intent**, and the
decision of what actually reaches the exchange is the infrastructure's. The
public type alias `QuoteDecision` is kept for the interface as specified.

### Strategy selection and versioning

```yaml
strategy:
  name: finalized_mm_v2      # registry key
  version: 2                 # asserted against the module's reported version
  params:                    # opaque to the engine; parsed by the strategy
    base_half_spread_bps: 4.0
```

Strategies self-register into a process-wide registry at static-init time via
`MM_REGISTER_STRATEGY(...)`. The strategies target is a CMake **OBJECT library**,
so every translation unit is linked in whole and no self-registration is
dead-stripped — the classic failure mode of static-registry plug-ins.

Adding a strategy means: add a directory under `strategies/`, implement the
interface, add one `MM_REGISTER_STRATEGY` line, change the YAML. No file under
`src/` or `include/mm/` is touched. `tests/strategy_swap/` asserts this
mechanically.

---

## 4. The exchange boundary

```
                             CORE ENGINE
                                  │
                        ┌─────────┴─────────┐
                        ▼                   ▼
              IExchangeMarketData    IExchangeExecution
                        │                   │
       ┌────────────────┼───────────┐       ├──────────────┐
       ▼                ▼           ▼       ▼              ▼
   Binance MD       (OKX MD)   (Bybit MD)  Binance Exec  Paper Exec
```

The core never sees a Binance field name, endpoint, signature scheme, error
code, or ID format. Adapters translate in both directions:

- **Inbound**: exchange wire format → `MarketDataEvent` / `ExecutionReport`,
  with exchange, receive, and parse timestamps stamped at the boundary.
- **Outbound**: `OrderRequest` / `CancelRequest` → exchange wire format.

Exchange-specific concerns owned entirely by the adapter: authentication and
signing, REST paths, WebSocket framing and subscription grammar, rate-limit
weights and headers, listen-key lifecycle, symbol filters (tick size, lot size,
min notional), ID formats, error taxonomy, and reconnect/backoff policy.

Normalized rejection reasons are a closed enum, so the core reacts to
`RejectReason::InsufficientBalance` without knowing Binance's `-2010`.

Paper is an adapter, not a mode switch scattered through the code. Every module
above the adapter is byte-identical between paper and live; only the object
constructed at wiring time differs. This is what makes paper/live parity a
structural property rather than a promise.

---

## 5. Event model

Every event carries a uniform timing envelope so latency is attributable
end-to-end rather than estimated:

```cpp
struct EventStamps {
    Nanos exchange_ns;   // venue-reported, wall clock, untrusted
    Nanos recv_ns;       // steady clock, stamped in the socket read handler
    Nanos parse_ns;      // steady clock, stamped after decode
    Nanos process_ns;    // steady clock, stamped when the trading thread pops it
};
```

`recv_ns`/`parse_ns`/`process_ns` are **monotonic** (`CLOCK_MONOTONIC`) and are
the only clocks used for internal latency arithmetic. `exchange_ns` is wall
clock from a foreign machine; it is used for staleness heuristics and clock-skew
monitoring, never for latency percentiles.

Event families:

| Family              | Producer            | Consumer         | Transport         |
| ------------------- | ------------------- | ---------------- | ----------------- |
| `MarketDataEvent`   | MD I/O thread       | Trading thread   | SPSC ring         |
| `ExecutionReport`   | Exec I/O thread     | Trading thread   | SPSC ring         |
| `ControlCommand`    | Monitoring thread   | Trading thread   | SPSC ring         |
| `OrderRequest`      | Trading thread      | Exec I/O thread  | SPSC ring         |
| `JournalRecord`     | Trading thread      | Journal thread   | SPSC ring         |
| `TelemetrySnapshot` | Trading thread      | Monitoring       | seqlock (no ring) |

Rings are bounded and **never block a producer**. Overflow policy is per-ring and
explicit: market data overflow is a fatal safety condition (it means the trading
thread is not keeping up, so the book is untrustworthy) and trips SAFE MODE;
journal overflow increments a dropped counter and is reported.

---

## 6. Concurrency model

The design is *single-writer for all trading state*. There is exactly one thread
that may mutate order books, strategy state, quote state, risk counters, OMS
state, and portfolio. Everything else feeds it or observes it.

```
   ┌────────────────┐   SPSC    ┌──────────────────────────┐   SPSC   ┌───────────────┐
   │  MD I/O thread │ ────────► │                          │ ───────► │ Exec I/O      │
   │  (Asio+Beast)  │           │     TRADING THREAD       │          │ thread        │
   └────────────────┘           │                          │ ◄─────── │ (REST + user  │
                                │  owns: books, strategy,  │   SPSC   │  data stream) │
   ┌────────────────┐   SPSC    │  quote mgr, risk, OMS,   │          └───────────────┘
   │  Monitoring    │ ────────► │  portfolio, PnL          │
   │  (HTTP)        │ ◄──────── │                          │   SPSC   ┌───────────────┐
   └────────────────┘  seqlock  │                          │ ───────► │ Journal thread│
                                └──────────────────────────┘          └───────────────┘
```

| Thread      | Owns (mutable)                              | Blocking I/O | Allocates in steady state |
| ----------- | ------------------------------------------- | ------------ | ------------------------- |
| MD I/O      | WS session, parse arenas                    | yes (epoll)  | no (reused buffers)       |
| **Trading** | books, strategy, quotes, risk, OMS, portfolio | **never**  | **no**                    |
| Exec I/O    | HTTP conns, listen key, signing scratch     | yes (epoll)  | no (pooled)               |
| Monitoring  | HTTP server, snapshot copy                  | yes          | yes (irrelevant)          |
| Journal     | file handles, write buffers                 | yes (fsync)  | no (pooled)               |

**Why single-writer.** A market maker's correctness hinges on the joint
consistency of *book × position × live orders × risk counters*. Locking those
four independently invites torn reads and lock-ordering bugs; locking them
together serializes everything anyway. Making one thread the sole writer gets
the same serialization with zero lock cost on the hot path and makes every
invariant checkable at a single point.

**Synchronization primitives.** Only two, both documented in
[docs/concurrency.md](concurrency.md):

- `SpscRing<T, N>` — bounded, power-of-two, cache-line-padded head/tail.
  Producer: `release` store of tail. Consumer: `acquire` load of tail. Cached
  opposite index avoids a coherence miss per operation.
- `Seqlock<T>` — single-writer snapshot publication for telemetry. Writer bumps
  an odd sequence, writes, bumps even. Readers retry on odd or changed sequence.
  Readers never block the writer, which is the entire point: **the dashboard
  cannot stall trading, even under load, even if it crashes.**

No mutex is taken on the trading thread in steady state. Mutexes exist only in
startup, shutdown, and the monitoring HTTP server.

**Shutdown.** Cooperative, ordered, and bounded: stop accepting new intents →
cancel-all and await confirmation (bounded wait) → drain execution reports →
flush journal → stop I/O contexts → join. Detailed in
[docs/recovery.md](recovery.md).

---

## 7. Safety architecture

Safety is not a feature bolted onto the engine; it is the engine's default
posture. The system has a global `TradingState` that gates order emission:

```
        ┌──────────┐  startup complete + reconciled + data synced
        │  BOOTING │ ─────────────────────────────────────────────┐
        └──────────┘                                              ▼
                                                            ┌──────────┐
        ┌───────────┐   operator resume + all-clear         │  ACTIVE  │
        │ SAFE_MODE │ ◄────────────────────────────────────►└──────────┘
        └───────────┘   any safety trigger (below)                │
              │                                                   │
              │  kill switch / fatal                              │
              ▼                                                   ▼
        ┌──────────┐                                        ┌──────────┐
        │  HALTED  │ ◄──────────────────────────────────────│ DRAINING │
        └──────────┘        shutdown requested              └──────────┘
```

Triggers that force SAFE_MODE, each with an explicit detector:

market data stale · sequence gap unrecoverable · book invariant violated ·
MD socket down · user stream down · MD ring overflow · reconciliation mismatch ·
position over hard limit · loss limit breached · reject burst · latency
anomaly · clock skew anomaly · OMS in unknown state · strategy exception or
budget overrun · risk state uncertain.

SAFE_MODE means: **stop quoting, cancel everything, keep observing.** It does not
mean exit — the process stays up so it can reconcile, report, and resume under
operator control. Resumption from SAFE_MODE always re-runs the readiness gate.

The governing rule, applied everywhere: *when uncertain, do not trade.* An
unknown order state is treated as live for risk purposes and blocking for
quoting purposes, because the expensive error is assuming an order is gone when
it is not.

---

## 8. Order traceability

Every fill is walkable back to the market state that caused it. Each stage
stamps the same `TraceId` (64-bit, allocated by the trading thread when a
strategy decision is produced):

```
MarketDataEvent(seq)  ─► StrategyDecision(trace) ─► QuoteAction(trace)
   ─► RiskVerdict(trace) ─► OmsOrder(client_id, trace) ─► ExchangeOrder(exch_id)
   ─► ExecutionReport(exch_id) ─► Fill ─► PositionDelta ─► PnLDelta
```

`ClientOrderId` is the join key across the boundary, and is generated to be
unique across process restarts (session epoch + monotonic counter), so a restart
can never collide with in-flight orders from the previous life — which is
exactly the window in which reconciliation matters most.

---

## 9. Project layout

```
crypto_market_making_system_c++_live/
├─ CMakeLists.txt              top-level: options, deps, target graph
├─ cmake/                      helper modules
├─ config/
│  ├─ paper.yaml               default; live_trading_enabled: false
│  └─ live.yaml                requires explicit gates (§ live safety)
├─ include/mm/                 public headers, one dir per module
│  ├─ common/  market_data/  orderbook/  strategy/  quote/  risk/
│  ├─ oms/  execution/  portfolio/  monitoring/  persistence/  engine/
│  └─ exchange/{common,binance,paper}/
├─ src/                        mirrors include/mm/
├─ strategies/                 plug-ins (OBJECT lib, self-registering)
├─ apps/                       mm_engine, mm_ctl, mm_probe
├─ tests/{unit,integration,failure,strategy_swap,support}/
├─ benchmarks/                 Google Benchmark
├─ tools/                      operational scripts
├─ deploy/systemd/             units, env files, log rotation
├─ dashboard/                  static operator UI (served by monitoring)
├─ third_party/                vendored: fmt, spdlog, yaml-cpp, googletest
└─ docs/
```

---

## 10. Technology and dependency justification

| Dependency        | Version | Where              | Why                                                     |
| ----------------- | ------- | ------------------ | ------------------------------------------------------- |
| C++20 / GCC 13    | —       | everywhere         | concepts, `<bit>`, designated init, `std::span`          |
| Boost.Asio        | 1.83    | I/O threads only   | proactor, timers, TLS integration                       |
| Boost.Beast       | 1.83    | adapters only      | WebSocket + HTTP/1.1 over Asio                          |
| OpenSSL           | 3.0     | adapters only      | TLS; HMAC-SHA256 request signing                        |
| nlohmann/json     | system  | adapters only      | JSON decode at the boundary — never on the trading thread |
| fmt               | 11.0.2  | logging, tools     | formatting; spdlog backend                              |
| spdlog            | 1.15.0  | logging            | async logger with a bounded queue                       |
| yaml-cpp          | 0.8.0   | config, startup    | config parsing (startup only)                           |
| GoogleTest        | 1.15.2  | tests              | unit/integration/failure tests                          |
| Google Benchmark  | system  | benchmarks         | evidence for any latency claim                          |

No other runtime dependency is accepted without justification here. **Python is
not a runtime dependency of the engine** — it appears only in optional developer
tooling under `tools/`.

JSON parsing deserves a note: nlohmann/json is convenient but allocates. It is
therefore confined to the I/O threads, where a few microseconds of decode are
irrelevant next to network latency, and never appears on the trading thread. If
benchmarks later show decode dominating the ingress path, the replacement is
local to the adapter.

---

## 11. What is deliberately not here

- No historical data loading, tape replay, or backtest harness.
- No parameter search, fitting, or optimization of any kind.
- No signal research, feature engineering, or model training.
- No exchange beyond Binance implemented in V1 — the seams exist, the code does
  not, because unused adapters rot.

---

## 12. Document map

| Document                                             | Covers                                   |
| ---------------------------------------------------- | ---------------------------------------- |
| [strategy-interface.md](strategy-interface.md)       | plug-in contract, registry, lifecycle    |
| [exchange-interface.md](exchange-interface.md)       | adapter contract, normalized types, unknown-state semantics |
| [market-data.md](market-data.md)                     | sessions, sequencing, resync, staleness  |
| [concurrency.md](concurrency.md)                     | threads, ownership, memory ordering      |
| [state-machines.md](state-machines.md)               | order, session, book, system states      |
| [benchmarks.md](benchmarks.md)                       | measured numbers and what they imply     |
| [risk.md](risk.md)                                   | limits, kill switches, safety halts      |
| [oms.md](oms.md)                                     | order lifecycle and failure handling     |
| [execution.md](execution.md)                         | paper and live execution paths           |
| [reconciliation.md](reconciliation.md)               | truth resolution against the exchange    |
| [recovery.md](recovery.md)                           | startup, crash, and shutdown sequences   |
| [monitoring.md](monitoring.md)                       | telemetry, metrics, dashboard            |
| [operations.md](operations.md)                       | deployment and runbook                   |
