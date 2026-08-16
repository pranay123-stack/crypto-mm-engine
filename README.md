# Crypto Market-Making Execution Platform

A production execution platform in C++20 for running an **already-finalized**
market-making strategy against crypto venues, in paper and live modes.

This repository is not a research platform. It contains no backtester, no
parameter optimization, and no alpha research. The separate quant research
platform owns those and hands this one a finalized strategy module plus its
validated parameters.

> **Status: PHASE 7 OF 16 COMPLETE — NOT PRODUCTION READY.**
> The pipeline runs from live market data to risk-approved order actions: the
> Binance adapter synchronizes a real book, a strategy runs inside a runtime that
> contains it, the quote manager turns intent into desired order state, and the
> risk engine decides what is permitted. **Nothing can place an order** — no OMS
> and no execution path exists yet. See [Status](#status).

---

## What it is

```
Market Data → Order Book → Strategy → Quote Manager → Risk → OMS → Exchange
                                                                      │
                                                          ┌───────────┴───────────┐
                                                          ▼                       ▼
                                                   Paper Execution        Live Execution
```

Four properties the architecture is built to guarantee:

1. **The strategy is replaceable.** It is selected by name in YAML. Adding one
   touches `strategies/` and nothing else — not the engine, not risk, not the
   OMS, not the dashboard.
2. **The exchange is an adapter.** Binance is the reference implementation, not
   a dependency. No core module knows a Binance field name, endpoint, or error
   code.
3. **Paper and live share one engine.** Identical strategy, market state, quote
   manager, risk, OMS, and portfolio. Only the execution adapter differs, which
   makes paper/live parity structural rather than aspirational.
4. **When uncertain, it stops.** Stale data, a sequence gap, an invalid book, an
   unknown order, a failed reconciliation — every one has an explicit detector
   and an explicit response, and the response is never "carry on and hope".

## Build

Requires Linux, GCC 13+ or Clang 17+, CMake 3.24+.

System packages: Boost ≥ 1.74 (headers), OpenSSL 3, nlohmann/json, Google
Benchmark.

```bash
sudo apt install libboost-dev libssl-dev nlohmann-json3-dev libbenchmark-dev
```

fmt, spdlog, yaml-cpp and GoogleTest are pinned git submodules, so every build
resolves the same commits rather than whatever the distribution happens to ship.

```bash
git clone --recursive https://github.com/pranay123-stack/crypto-mm-engine.git
cd crypto-mm-engine

# already cloned without --recursive?
git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Sanitizers:

```bash
# Address + UB
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMMX_ASAN=ON && cmake --build build-asan -j

# Thread — requires Clang; GCC's TSan cannot model atomic_thread_fence
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMMX_TSAN=ON \
      -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-tsan -j
```

Benchmarks:

```bash
./build/mm_bench --benchmark_min_time=0.5s
```

### Build options

| Option             | Default | Purpose                                    |
| ------------------ | ------- | ------------------------------------------ |
| `MMX_WERROR`       | ON      | warnings are errors                        |
| `MMX_NATIVE_ARCH`  | OFF     | `-march=native` (off: portable binaries)    |
| `MMX_LTO`          | OFF     | link-time optimization in Release          |
| `MMX_ASAN`         | OFF     | address + UB sanitizers                    |
| `MMX_TSAN`         | OFF     | thread sanitizer (Clang only)              |
| `MMX_BUILD_TESTS`  | ON      | test suites                                |
| `MMX_BUILD_BENCH`  | ON      | Google Benchmark targets                   |

## Live-trading safety

Live trading requires **six independent conditions**, and no single file can
supply them all:

1. `mode: live` in the config
2. `safety.live_trading_enabled: true` — a separate opt-in from `mode`
3. every risk limit set to a positive value (an unset limit is a refusal, never
   "unlimited")
4. `risk.emergency_loss >= risk.max_daily_loss`
5. `exchange.credentials_env_prefix` naming an environment variable pair that
   exists — credentials are never stored in config
6. `--live` passed explicitly on the command line

Copying `config/live.yaml` to another machine cannot cause it to trade.
`tests/unit/test_config.cpp` removes each gate in turn and asserts the engine
refuses to start.

## Configuration

`config/paper.yaml` is the default. Unknown keys are a hard error — a typo like
`max_postion` would otherwise parse cleanly, apply nothing, and leave the engine
running with an unbounded position limit.

Selecting a strategy:

```yaml
strategy:
  name: finalized_mm_v2
  version: 2
  params:
    base_half_spread_bps: 4.0
```

## Documentation

| Document                                        | Covers                                    |
| ----------------------------------------------- | ----------------------------------------- |
| [architecture.md](docs/architecture.md)         | layers, module graph, boundaries, events  |
| [concurrency.md](docs/concurrency.md)           | threads, ownership, memory ordering       |
| [state-machines.md](docs/state-machines.md)     | order, session, book, and system states   |
| [exchange-interface.md](docs/exchange-interface.md) | adapter contract, unknown-state semantics |
| [binance-market-data.md](docs/binance-market-data.md) | streams, normalization, reconnect, threading |
| [order-book.md](docs/order-book.md)             | book, invariants, synchronization algorithm |
| [strategy-runtime.md](docs/strategy-runtime.md) | plug-in contract, lifecycle, replacement, failure behaviour |
| [quote-manager.md](docs/quote-manager.md)       | diff algorithm, ownership, generations, churn controls |
| [risk-engine.md](docs/risk-engine.md)           | exposure mathematics, fail-closed rules, kill switch |
| [benchmarks.md](docs/benchmarks.md)             | measured numbers and what they imply      |

## Status

Implemented and verified:

- **Phase 1 — Architecture & build.** Module graph, strategy/exchange/paper-live
  boundaries, concurrency model, event model, state machines. CMake with three
  sanitizer configurations.
- **Phase 2 — Core infrastructure.** Fixed-point money types, exact decimal
  parse/format, allocation-free identifiers, monotonic/wall clock separation,
  `Status`/`Result`, bounded SPSC ring, seqlock, object pool, log-linear latency
  histogram, structured async logging, metrics with Prometheus exposition, and
  strict YAML configuration with live-mode gates.

  136 tests, clean under `-Werror`, ASan+UBSan, and TSan. 17 benchmarks.

- **Phase 3 — Exchange abstraction.** `IExchangeMarketData` / `IExchangeExecution`
  and their sinks, normalized events, order requests with venue-rule validation,
  a capability model, and an explicit failure model whose central property is
  that *a request that failed is never confused with a request the venue
  refused*. A deterministic mock adapter implements both interfaces.

  279 tests, 24 benchmarks. The venue boundary is enforced mechanically by
  `tools/check_exchange_boundary.py`, wired into `ctest`: no core file may name
  a venue or include an adapter header.

- **Phase 4 — Binance market data and the local order book.** TLS WebSocket with
  bounded jittered reconnect, REST snapshots, exact-decimal decoding, the full
  documented snapshot/delta synchronization procedure with gap detection and
  resync, a sorted-vector L2 book with continuously enforced invariants, and
  per-symbol staleness. Multi-symbol, with isolation asserted by test.

  373 tests, 33 benchmarks. Verified against **live public Binance data** with
  `mm_md_probe`: book synchronized, 0 decode errors, book uncrossed at the
  venue's 0.01 tick. Public data only — the adapter contains no order-entry code.

- **Phase 5 — Strategy runtime.** `IStrategy`, an immutable `StrategyContext`,
  `QuoteIntent`, a lifecycle, a compile-time registry, and a runtime that gates
  on market-data health, times every call, catches exceptions, validates output
  against venue rules, and stops quoting on failure rather than standing on a
  stale quote. Strategy selection is a config change; the engine never names a
  concrete strategy.

  440 tests, 38 benchmarks. Includes one **TEST/REFERENCE ONLY** strategy that
  exists to validate the runtime — it is not a trading strategy and makes no
  profitability claim.

- **Phase 6 — Quote manager.** Deterministic diffing of desired quote state
  against working orders into New/Cancel/Replace/Keep actions, with monotonic
  generation ordering, idempotency, explicit order ownership, conservative
  handling of unknown order state, partial-fill replenishment, and configurable
  churn controls that decide *whether* to rewrite an order but never *what* to
  write.

  505 tests, 43 benchmarks. It contains no risk logic, no order management and
  no exchange code, enforced by the boundary checker.

- **Phase 7 — Risk engine.** The safety boundary. Worst-case-per-side exposure
  arithmetic, position/notional/order/working/rate limits, price bands, a
  five-state machine with reduce-only and operator-only recovery from a kill,
  explicit and revalidated reduction, and overflow-checked fixed-point
  throughout. Every fail-closed path refuses new exposure while leaving
  cancellation available.

  580 tests, 49 benchmarks. Contains no exchange code, no strategy logic and no
  order management, enforced by the boundary checker.

Not yet implemented — every one of these is currently absent, not partial:

Phase 8 OMS · Phase 9
paper execution · Phase 10 portfolio/PnL · Phase 11 reconciliation & recovery ·
Phase 12 Binance live execution · Phase 13 operations dashboard · Phase 14
failure hardening · Phase 15 performance hardening · Phase 16 deployment.

No claim of production readiness is made, and none will be until the failure
tests of Phase 14 pass.
