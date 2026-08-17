# Paper execution

> **Paper execution is an execution simulator for validating the live engine
> architecture. It is NOT a historical backtesting engine.**

It consumes the *current* normalized book, holds its own order state, and
answers asynchronously — the same shape a real venue has. It loads no history,
sweeps no parameters, computes no Sharpe ratio and makes no claim about whether
a strategy is any good. Those belong to a separate research platform and are
explicitly out of scope for this repository.

What it is for: proving that

```
Strategy → Quote Manager → Risk → OMS → Execution → venue → events → OMS
```

works, end to end, before there is real money on the other side.

---

## 1. Architecture

```
        engine's normalized book  ──read──►  ┌─────────────────┐
                                             │ PaperExecution  │
   OMS ──requests──► IExchangeExecution ──►  │  own order state│
                                             │  own event queue│
   OMS ◄──events──── IExecutionSink ◄──────  └─────────────────┘
```

Three properties are load-bearing, and each has a test:

- **Asynchronous.** `submit()` hands off and returns; the sink is never called
  from inside it.
- **Separate state.** `PaperOrder` is a different type from `oms::OrderRecord`
  and the two are never shared. A simulator that let both sides point at one
  object would *guarantee* they agree — and agreeing is precisely what the
  production system does not get for free. Every synchronization bug worth
  catching lives in that gap, so the simulator has to have it.
- **Deterministic.** Same config, requests, market events and latency give a
  byte-identical event stream.

---

## 2. Market data

The paper venue is **another consumer of the engine's normalized market state**.
It is handed a `const book::OrderBook&` — the same book the strategy reads — and
told when it changed. It parses no feed, holds no second copy, and cannot tell
which venue the data came from.

This matters more than it looks. A simulator with its own market-data path
would be testing a pipeline that does not exist in production.

---

## 3. Fill model

Two models. The default is the conservative one.

### `displayed_liquidity` (default)

A resting order fills only against liquidity that is **displayed** on the
opposite side and priced through it, capped by that displayed size, and reduced
by an assumed queue share.

The alternative — *"the price touched my level, so I am filled"* — is the single
assumption that most flatters a paper run, because it grants a fill for every
trade that reaches the price regardless of how much size was there or who was
ahead of us.

Executions happen at the **book's** price, not the order's limit. A buy limit at
60000 meeting an ask of 59900 pays 59900; crediting the limit price would invent
price improvement that never happened.

### `full_on_cross`

Fills everything the moment the book crosses. Deliberately optimistic, never the
default, and present mainly so the difference between the two is visible rather
than theoretical.

### Liquidity is shared, not duplicated

Each market update builds **one** working copy of each side, and every order
draws from it in priority order. Without this, five orders at the same price
would each be handed the same displayed size and the simulator would print more
volume than the market ever showed. (This was a real defect, caught by the
price-time priority test.)

---

## 4. Queue model — and what it is not

Priority is **price, then arrival sequence**. Deterministic and simple.

`queue_share_bps` models the size queued ahead of us: at 5000, half the
displayed quantity at each level is assumed to belong to somebody who was there
first. 10000 means "we are always first", which is the assumption that makes a
paper run look better than reality.

> **This is an approximation, and it is not a reproduction of any exchange's
> queue.** Real queue position depends on per-order arrival data that a depth
> feed does not carry: we can see that a level holds 5 BTC, never how that 5 is
> divided or where our order sits within it. A model claiming true queue
> position would be claiming information the input does not contain.

A repriced replace goes to the back of the queue, because moving the price
surrenders priority at any real venue. Keeping it would model a free improvement
nobody offers.

---

## 5. Order types and time in force

**LIMIT only.** MARKET is not used by the quote manager, and inventing support
for it would be untested surface pretending to be a feature. A market order is
refused, not approximated.

TIF is carried and echoed but not yet differentiated: GTC is the only behaviour
implemented. IOC/FOK semantics arrive when something needs them.

**Post-only is explicit.** An order that would cross on arrival is refused
(`PostOnlyWouldCross`) or expired, per `post_only_policy`. It is **never**
silently converted into a taker — a strategy that asked not to pay the spread
and paid it anyway is trading a different instrument from the one it thinks it
is, and no downstream record would show why.

**Marketable limits** (not post-only) take immediately, after their
acknowledgement, reported as `Liquidity::Taker`. The ack comes first, as a real
venue would send it.

---

## 6. Latency model

Five separately configurable stages:

| Stage | Default | Meaning |
| --- | --- | --- |
| `request_latency_us` | 500 | request in flight before the venue sees it |
| `ack_latency_us` | 1500 | venue decision time for a new order |
| `cancel_latency_us` | 1500 | " for a cancel |
| `replace_latency_us` | 2000 | " for a replace |
| `fill_latency_us` | 1000 | fill occurring → report reaching us |
| `query_latency_us` | 5000 | query answer |

> **These are simulation values chosen to be structurally realistic. They are
> not measurements of Binance or of any other venue and must not be quoted as
> such.** Their purpose is to force the engine to cope with answers that arrive
> after the request.

Requests are held for `request_latency_us` before the venue processes them, so
an order does not exist at the venue the instant `submit()` returns. Collapsing
the two would make `request_latency_us` meaningless and — worse — would let an
order fill in a window when the venue had not yet received it.

Answers are dated from when the request **arrived**, not from whenever `poll()`
happened to run, so a late poll cannot silently stretch every latency.

---

## 7. Cancellation

```
  OMS ──cancel──► venue ...(latency)... ──► CancelAck / CancelReject ──► OMS
```

A cancel is **accepted but not applied** until its confirming event is
delivered. The order keeps resting for the whole latency window, so a market
move in that window can still fill it.

That is the cancel/fill race, and the venue does not resolve it in the engine's
favour:

- filled in the window → `CancelStatus::TooLate`, `OrderStatus::Filled`;
- partially filled → the cancel still succeeds, and the confirming event carries
  the cumulative quantity the engine must reconcile against;
- refused → the order is still working, and says so.

**Cancellation survives degraded connectivity.** Only a fully disconnected
transport refuses a cancel. Gating withdrawal on healthy connectivity would trap
exposure exactly when releasing it matters most.

---

## 8. Replacement

`replace_mode: atomic` gives one request, one answer, and an atomic swap of
identity and parameters. Filled quantity carries forward — an amendment cannot
undo a fill — and reducing quantity below what is already filled is refused.

A **refused** replace leaves every parameter untouched, including the venue
identity and queue position. Moving the price before the answer arrived would
leave the engine and the venue describing different orders.

`replace_mode: unsupported` makes `replace()` return an error rather than
decomposing it. The decomposition into cancel-then-new belongs to the quote
manager, which does it consciously, because the two have different exposure
profiles. Claiming atomicity we do not have would hide that window entirely.

---

## 9. Stale market data

A book older than `max_book_age_ms` produces **no fills**. Inventing an
execution from data already known to be stale would put a position on the
engine's books that never existed anywhere else.

Cancellation and reconciliation are deliberately unaffected: they are how the
engine gets out of trouble, and gating them on fresh data would trap it.

---

## 10. Failure injection

Every fault is a **count** or a **switch**, never a probability:

`reject_next_new`, `reject_next_cancel`, `reject_next_replace`,
`drop_next_request`, `duplicate_next_event`, `reorder_next_event`,
`delay_next_request_ns`, `hide_next_snapshot_orders`, `error_next_request`.

A test that fails one run in twenty is worse than no test: it trains people to
re-run rather than to look. Faults are also **not configurable from a file** —
failure injection is a testing instrument, and a config that could switch it on
is a config that could make a paper session silently unrepresentative.

`drop_next_request` is the important one: it produces *no answer at all*, so the
engine must time out and conclude `Unknown` on its own. Silence is not a
cancellation, a rejection, or anything else.

---

## 11. Connectivity

Reuses the normalized `ConnectionState`: `Disconnected`, `Connecting`,
`Connected`, `Reconnecting`, `Failed`. Transitions are delivered as
`ConnectionEvent`s immediately rather than queued behind order latency —
they describe the transport, and telling the engine it is connected *after* it
had already timed out would be worse than useless.

When disconnected, new orders and replaces fail safely and synchronously.
Cancels remain possible while merely degraded (§7).

`stop()` does **not** cancel resting orders. Closing a socket is not a trading
decision, and a venue does not forget your orders because you disconnected.

---

## 11a. Threading

**`PaperExecution` is single-threaded and owns no thread.** Every entry point —
`submit`, `cancel`, `replace`, `poll`, `on_market_update` — is called from one
thread, and in paper mode that is the trading thread.

This is a contract, not an oversight. A real venue adapter has a socket whose
I/O genuinely overlaps the trading thread and therefore needs internal
synchronization. A simulator has no I/O to overlap: adding a lock would buy
nothing and would make the simulator's timing depend on contention that
production would not have.

The concurrency in the architecture is unchanged from Phase 8 and lives in the
OMS:

```
  whichever thread the adapter runs on          trading thread
  ────────────────────────────────────          ──────────────
  venue emits ─► OMS::on_execution ─► SPSC ring ─► OMS::process_events
                 (enqueue only)                     (all state mutation)
```

That boundary — not the venue — is what
`tests/integration/test_execution_concurrency.cpp` exercises across two real
threads under TSan.

`ManualClock`, used throughout the tests, is deliberately *not* thread-safe: it
is a test instrument driven by whoever owns the simulated timeline. The
concurrency test uses the real monotonic clock instead.

---

## 12. Identity

The venue mints its own `ExchangeOrderId` — `PX00000000042`, deliberately
unlike a client order id in both prefix and shape. Nothing is derived from the
client id.

This matters because the whole three-identity architecture has to be exercised
exactly as it will work against a real venue. If any code assumes the two are
interchangeable, it fails here rather than in production.

A replace mints a fresh venue identity, as an atomic replace does at a real
venue.

---

## 13. Reconciliation

`query_open_orders` answers with `OpenOrdersSnapshotEvent`s carrying everything
reconciliation needs: both identities, status, price, original and executed
quantity. Answers are **chunked** like a paged venue response, including the
empty case — reconciliation may only conclude an order is absent after
`is_last`, so a last chunk must exist even when there is nothing to report.

`hide_next_snapshot_orders` makes the venue omit resting orders, producing
exactly the divergence reconciliation exists to find.

---

## 14. Determinism and replay

Given the same initial state, requests, market events and configured latency,
the event sequence is identical. Two mechanisms make that true rather than
likely:

1. Events are ordered by **(due time, emission ordinal)** — nothing that could
   vary between runs.
2. There is no randomness anywhere on the default path. `deterministic_seed`
   exists for optional stochastic simulation later; it is zero, and no test
   uses it.

`PaperReplay` records a session — configuration, requests, market updates — and
replays it against a fresh venue with its own clock, comparing both the event
transcript and the final venue state.

The value of this is not the test. When something goes wrong against a live
venue, the same three inputs recorded from production can be replayed here until
the behaviour is understood, which is a very different position from reading
logs and guessing.

---

## 15. Measured cost

See docs/benchmarks.md. **Paper execution performance is not equivalent to real
exchange latency and nothing there should be read as a venue measurement** —
what those numbers bound is how much of a paper session is harness rather than
engine.

---

## 16. Limitations

Stated plainly, because a simulator whose limits are not written down gets
trusted for things it cannot do:

- **Queue position is an approximation** (§4), not a reproduction.
- **No market impact.** Our orders never move the book, and the book is not
  aware of them. At meaningful size this is optimistic.
- **No adverse selection modelling.** Fills happen when the displayed book
  reaches us; nothing models *why* it reached us.
- **No fee schedule tiers, no rebates, no funding.** A flat maker/taker rate.
- **LIMIT and GTC only.**
- **No balance or position custody.** `query_balances` and `query_positions`
  return "not modelled" rather than inventing a number.
- **No partial-book depth beyond what the feed carries.** The venue can only
  fill against what the normalized book shows.
- **It cannot tell you whether a strategy is profitable**, and no part of this
  repository will.
