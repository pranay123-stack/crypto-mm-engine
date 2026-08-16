# Strategy runtime

> **The strategy runtime executes a finalized strategy; it does not perform
> research or establish profitability.**
>
> Research, alpha discovery, parameter search, backtesting, walk-forward
> testing and model training belong to the separate quant research platform.
> This runtime consumes what that platform produces: a strategy module and a
> validated parameter block. Nothing here optimizes anything.

---

## 1. The boundary

```
   Market Data ─► Order Book ─► StrategyContext ─► IStrategy
                                                      │
                                                      ▼
                                                 QuoteIntent
                                                      │
                                    ┌─────────────────┴─────────────────┐
                                    │        StrategyRuntime            │
                                    │  gate · time · catch · validate   │
                                    └─────────────────┬─────────────────┘
                                                      ▼
                          Risk ─► Quote Manager ─► OMS ─► Exchange
                                (phases 6-8, not yet implemented)
```

The direction is one-way and mandatory. There is no path from a strategy to the
exchange, and there is no path around the runtime.

### What a strategy cannot do, and why it cannot

Not by convention — by what it is handed and what it links against:

| Capability | Why it is impossible |
| --- | --- |
| Submit or cancel an order | No OMS or exchange reference exists in `StrategyContext` |
| Reach the network | `mm_strategy` links only `mm_orderbook` and `mm_common`; there is no Asio, Beast, OpenSSL or JSON in its dependency closure |
| Mutate the order book | It receives `BookDepthView` — non-owning, const, no setters |
| Read the clock | `context.now_ns` is supplied; nothing in its inputs exposes a clock |
| Choose venue parameters | `QuoteIntent` cannot express a time-in-force, a post-only flag or an order id |
| Name one of its own orders | `WorkingQuote` carries a price and a size, not an identifier |
| Bypass validation | Its only output is a value the runtime inspects before anyone else sees it |
| Claim another identity | The runtime stamps provenance; a strategy's own claim is overwritten |

`tools/check_exchange_boundary.py` enforces the include rules mechanically and
runs under `ctest`. It carries a self-test, because a boundary checker that
silently passes everything is worse than none.

---

## 2. Lifecycle

```
   Created ──initialize──► Initialized ──start──► Running ⇄ Paused
      │                         │                    │         │
      │                         └──────stop──────────┴─────────┘
      │                                    │
      │                                    ▼
      └────────────── (any state) ───► Faulted ──clear_fault──► Initialized
                                              │
                                          Stopped ──initialize──► Initialized
```

This is **not** a copy of the engine's `TradingState` or the per-symbol
`SessionState`. It describes only whether *this strategy instance* is willing
and able to produce intent. The two interact in one direction: the runtime asks
the session whether the book is quotable and refuses to evaluate if it is not.
A strategy can be `Running` while market data is stale — it simply is not asked.
Duplicating the session machine here would create a second opinion about whether
the book is safe, and two opinions is one too many.

`Faulted` never clears itself. A strategy that faulted once will fault again on
the same input; automatic recovery would produce a component that quotes
intermittently after being judged broken.

---

## 3. When the strategy is called

Evaluating on every market-data event is both wasteful and wrong: one venue
message can arrive as several chunks, so "every event" would evaluate a
partially applied book. Every mode fires only on a settled book.

| Mode | Fires on |
| --- | --- |
| `on_book_update` | every completed book update, and BBO changes |
| `on_bbo_change` | only when the top of book moved |
| `on_timer` | only the timer |
| `on_bbo_change_and_timer` | **default** — the touch, plus a periodic refresh |

The default suits a maker quoting at the touch: it reacts when the touch moves,
and re-evaluates periodically so a quiet market still refreshes quotes and
time-dependent state still decays. `TriggerReason` is passed in the context so a
strategy can tell a market move from a refresh without inferring it.

### Gates

Before the strategy is called at all:

```
runtime enabled        strategy Running (not Paused/Stopped/Faulted)
trigger wanted         session state == Ready
data age within limit  instrument spec loaded
two-sided, uncrossed market
```

A failed gate returns a `Pull` with a `SkipReason`, and the strategy is not
invoked. `gate()` is public so the engine can decide whether assembling a
context is even worth it (6 ns, versus 38 ns to build one).

---

## 4. Context and intent

`StrategyContext` is immutable, non-owning, and valid only for the call. It
carries market state, depth, timestamps, venue rules, inventory, our own working
quotes, and why this evaluation is happening. It carries no socket, no client,
no queue, no lock and no venue object.

`QuoteIntent` is a *wish*, not an instruction:

```cpp
QuoteAction action;           // Quote | Pull | NoChange
bool quote_bid, quote_ask;
Px bid_price, ask_price;
Qty bid_quantity, ask_quantity;
IntentReason reason;          // diagnostic
StrategyIdentity identity;    // stamped by the runtime
Seq market_sequence;          // the book it was computed from
```

`Pull` and `NoChange` are distinct on purpose. `Pull` means "withdraw"; zero
size would be an invalid quote rather than a deliberate exit. `NoChange` lets a
strategy say "leave my quotes alone" without restating them, so the quote
manager does not churn orders needlessly.

---

## 5. Validation

Every intent is checked before it reaches risk. A buggy strategy must not be
able to reach the venue.

| Check | Rejection |
| --- | --- |
| price > 0, quantity > 0, quantity not negative | `NonPositivePrice`, `NonPositiveQuantity`, `NegativeQuantity` |
| tick and lot alignment | `TickMisaligned`, `LotMisaligned` |
| venue min notional / max quantity | `NotionalTooSmall`, `QuantityAboveVenueMax` |
| its own bid below its own ask | `CrossedQuote` |
| within `max_quote_distance_bps` of the touch | `QuoteTooFarFromTouch` |
| computed from the current book | `StaleSequence` |
| identity present and matching | `IdentityMissing`, `IdentityMismatch` |
| instrument rules loaded | `InstrumentNotLoaded` |

`StaleSequence` catches a strategy that cached a decision and replayed it against
a book that has since moved. `QuoteTooFarFromTouch` catches arithmetic gone
wrong: sending such a quote buys a venue rejection at the cost of a rate-limit
slot.

---

## 6. Failure behaviour

**A strategy failure never crashes the process, and never leaves the engine
quoting on a decision it no longer trusts.**

| Failure | Response |
| --- | --- |
| Throws (`std::exception` or otherwise) | caught, `Faulted` immediately, quoting stops |
| One slow evaluation | counted; tolerated — a hiccup is not a fault |
| `max_consecutive_budget_violations` in a row | `Faulted`; persistent slowness is not something the trading thread can absorb |
| One invalid output | rejected, counted; tolerated |
| `max_consecutive_invalid_outputs` in a row | `Faulted`; it cannot be trusted to produce a quote at all |
| Throws in `on_fill` / `on_order_event` | caught, `Faulted` |

**On any failure the result is a `Pull`, never the previous quote.** Standing on
an old quote would be holding a position nobody decided to hold: a rejected or
failed evaluation means the strategy's current opinion is *unknown*, not "the
same as last time". A single successful evaluation resets both streaks.

The budget cannot preempt a strategy that hangs forever — no in-process
mechanism can, short of a thread per call, which would destroy the coherence the
inline call exists to provide. That residual risk is covered operationally by
the systemd watchdog (docs/concurrency.md §6).

---

## 7. Replacing a strategy

**Files that must change: two, plus one line of configuration.**

1. Add `strategies/<name>/YourStrategy.{hpp,cpp}` implementing `IStrategy`.
2. Add one `MM_REGISTER_STRATEGY(...)` line at the bottom of that `.cpp`.
3. Change `strategy.name` in the YAML and add a `strategies.<name>` block.

**Files that must NOT change:** the Binance adapter, the order book, the
synchronizer, the exchange interfaces, the strategy runtime, risk, the OMS, the
execution layer, the dashboard, CMake (the glob picks it up), or any test
outside the new strategy's own.

The engine depends on `IStrategy` and the registry, never on a concrete
strategy. There is no switch statement anywhere that enumerates them.
`tests/strategy_swap/` runs two unrelated implementations through byte-identical
infrastructure and asserts both work and that their identities differ.

### The linker caveat

A self-registering translation unit that nothing references is **dead-stripped
out of a static archive**, and the strategy silently vanishes from the registry.
`strategies/` is therefore a CMake **OBJECT library**, whose objects are linked
whole. `StrategyRegistryTest.ThePluginIsActuallyRegistered` is what would notice
if that ever changed.

---

## 8. Configuration

```yaml
strategy:
  name: reference_mm_v1            # registry key; selects the implementation
  version: 1                       # checked against the binary at startup
  enabled: true
  quoting_enabled: true            # operator switch, separate from `enabled`
  evaluation_mode: on_bbo_change_and_timer
  budget_ns: 50000
  max_consecutive_budget_violations: 5
  max_consecutive_invalid_outputs: 3
  timer_interval_ms: 100
  max_quote_distance_bps: 500

strategies:                        # finalized parameters, one block per strategy
  reference_mm_v1:
    half_spread_bps: 4.0
    quote_size: 0.001
```

Core configuration contains no field belonging to any particular strategy;
strategy-specific parameters live only under `strategies.<name>`, and only the
selected strategy's block is merged into what the runtime passes along. Unknown
keys elsewhere remain hard errors, as in Phase 2.

`quoting_enabled` is separate from `enabled` on purpose: turning quoting off
must not require restarting a strategy that has spent minutes warming up. With
it off the strategy is still evaluated and its state stays warm, but no quoting
intent is accepted.

**`config/live.yaml` deliberately names a strategy that is not in the binary.**
The only registered strategy is a test fixture; a live config pointing at it
would be one copy-paste from trading real money with a placeholder. Starting
live with the shipped file fails at startup, which is the correct outcome until
a finalized strategy exists.

---

## 9. Versioning and attribution

Every `QuoteIntent` carries `StrategyIdentity`: name, version, and
`config_generation`. Two intents with the same name and version but different
generations came from different parameters — the case that is otherwise
invisible.

The runtime **stamps** this rather than trusting the strategy to. A strategy
that got its own version wrong, or claimed another's, would misattribute every
fill it produced. A strategy whose self-reported identity disagrees with the
configuration is refused at initialization rather than allowed to run under the
wrong name in the journal.

This exists for order attribution, PnL analysis, incident investigation and
deployment rollback. It is not an experiment-tracking platform.

---

## 10. Determinism

Given identical context, identical internal state and identical parameters, a
strategy must return an identical `QuoteIntent`. That means no wall clock, no
random engine seeded from entropy, no global mutable state, no I/O. A strategy
needing randomness must take its seed through parameters so a run is
reproducible.

The runtime supports this by supplying time in the context rather than letting
the strategy take it. Tests assert both that repeated evaluation of one context
agrees and that two independently constructed instances agree.

---

## 11. Watchdog and latency

`StrategyMetrics` tracks evaluations, skips (tallied per cause), accepted and
rejected intents (tallied per cause), quote versus pull intents, exceptions,
budget violations and max evaluation time. A `LatencyHistogram` gives p50
through p99.9. A strategy that never quotes is diagnosable from counters alone.

Measured on the reference strategy — a floor for the runtime's overhead, not a
prediction for a real strategy:

| Operation | Time |
| --- | --- |
| `gate()` alone | 6.1 ns |
| Context construction | 38.4 ns |
| Strategy invocation | 52.4 ns |
| Intent validation | 18.1 ns |
| **Full path** (gate, invoke, time, validate, stamp) | **132 ns** |

The parts sum to ~109 ns against a 132 ns whole; the difference is the runtime's
own bookkeeping — histogram, stamping, metrics.

No dashboard is built here. These counters exist for Phase 13 to consume.

---

## 12. Paper and live are invisible to the strategy

A strategy produces `QuoteIntent` and nothing else. What that becomes on the
wire is decided by risk, the quote manager, the OMS and whichever
`IExchangeExecution` was constructed — paper or live. Nothing in
`StrategyContext` or `QuoteIntent` mentions either, so paper/live parity is a
structural property of the strategy layer rather than something to maintain.

---

## 13. The reference strategy

`strategies/reference_mm_v1/` contains `ReferenceMarketMaker`.

> **TEST / REFERENCE ONLY.** It exists solely to validate the runtime. No claim
> is made that it is profitable — it almost certainly is not. It is **not
> suitable for live trading**. It contains no alpha, no research and no
> calibration: it quotes symmetrically around the mid at a fixed spread with a
> linear inventory skew, because that is the least interesting thing that still
> exercises every path in the runtime.

It does demonstrate the properties every strategy should have: deterministic,
allocation-free after construction, clock-free, parameter-validating at
initialization rather than defaulting, and rounding outward so a bid rounds down
and an ask rounds up.

---

## 14. Not implemented here

Risk, the quote manager, the OMS, paper execution, live execution and the
dashboard are phases 6 through 13. Nothing in this repository can place an
order.
