# Quote manager

> **The quote manager does not decide whether an order is financially safe.
> Risk owns that decision.**

It answers exactly one question:

> *What orders should currently exist to represent the strategy's intent?*

It does not answer *is this permitted?* (Risk), *how does this become an order?*
(OMS), or *how does this reach a venue?* (the adapter).

---

## 1. Position in the pipeline

```
   Strategy ─► QuoteIntent ─► ┌─────────────────┐ ─► OrderAction ─► Risk ─► OMS ─► Exchange
                              │  Quote Manager  │
   Working order state ─────► │  desired state  │        (phases 7-9, not implemented)
   Venue rules + caps  ─────► │  + diff         │
   Market/session state ────► └─────────────────┘
```

**Inputs** — `QuoteIntent`, `WorkingQuoteState`, `InstrumentSpec`,
`ExchangeCapabilities`, market-data age, and a system-ready flag. All normalized;
nothing venue-specific reaches this layer.

**Output** — `QuoteManagerResult`: up to four `OrderAction`s plus a
`SlotDecision` per side explaining what happened and why.

---

## 2. State model

The manager owns exactly **two logical quotes** per symbol: `QuoteSlot::Bid` and
`QuoteSlot::Ask`. Logical, not physical — a slot is "our bid", independent of
which order id currently represents it.

It **does not track order state**. `WorkingQuoteState` is an input supplied by
the OMS. Duplicating order tracking here would create a second opinion about
what is resting, and two opinions is one too many.

What it *does* remember, per slot, is what it has already asked for: the last
action, when, and when the last cancel was. That is the minimum needed for
idempotency and churn control, and nothing more.

---

## 3. Order ownership

The OMS will carry orders this manager did not create — manual operator orders,
emergency inventory-reducing orders, orders from another strategy. **Cancelling
one of those would be the quote manager reaching outside its mandate.**

Ownership is an explicit tag, not an assumption that everything on the symbol is
ours:

```cpp
struct QuoteOwner {
    StrategyName strategy;   // empty == unmanaged, untouchable
    QuoteSlot slot;
};
```

An order in one of our slots that does not match `{our strategy, this slot}` —
including the right strategy on the *wrong* slot — makes the slot **freeze**: no
cancel, and no new order alongside it. Cancelling somebody else's order and
quoting on top of one are both worse than standing still, and the inconsistency
is counted as `ownership_violations` where an operator will see it.

This holds even for `disable_all_quotes()`. A kill switch does not reach outside
what this manager owns.

---

## 4. The diff

```
   current working state  +  desired quote state  ─►  required actions
```

| Current | Desired | Result |
| --- | --- | --- |
| nothing | quote | **New** |
| resting, materially same | quote | **Keep** — no action |
| resting, price differs | quote | **Replace** |
| resting, resting size differs | quote | **Replace** |
| resting | none | **Cancel** |
| nothing | none | **Idle** |
| resting, venue lacks atomic replace | different quote | **Cancel + New** |

Per-slot, so a bid can be replaced while an ask is kept. Both slots are derived
from **one intent and one snapshot** of working state, so an action set can never
mix a bid from one generation with an ask from another.

### Replacement semantics

An order "represents" the desired quote when **both** hold:

- price differs by fewer than `min_price_move_ticks`, and
- the **resting** quantity differs by less than `min_quantity_move_fraction` of
  the desired quantity.

Quantity is compared against `remaining()`, never against what was originally
submitted. Assuming the original quantity survives a partial fill would leave the
book quoting less than the strategy asked for, with nobody noticing.

### Where the venue has no atomic replace

Phase 3 deliberately refuses to decompose a replace silently, because the two
have different exposure profiles. The decomposition therefore happens **here,
consciously**, and is reported as `CancelThenNew`: during the window exposure is
the *union* of both orders, not a clean swap. The cancel is ordered first.

---

## 5. Generation semantics

Every `QuoteIntent` carries a monotonic `generation`, stamped by the strategy
runtime and advancing on **every** evaluation — including gated ones that produce
a `Pull`, and including across faults and pauses. Reusing a generation would let
a stale intent masquerade as a current one.

> Phase 5 originally had only `config_generation` (which changes when parameters
> change) and `market_sequence` (identical for two timer evaluations of the same
> book). Neither can order two intents, so the monotonic counter was added in
> Phase 6 as an extension to Phase 5.

The manager keeps a watermark:

- `generation > watermark` — normal; the watermark advances.
- `generation == watermark` — a duplicate. Counted, and still evaluated, because
  re-evaluation must be idempotent (§6 below).
- `generation < watermark` — **`GenerationRegression`**. No actions, and the
  watermark does not move backwards.

A superseded intent **changes nothing**: it neither creates nor withdraws. A
generation-9 `Pull` arriving after generation 10 placed quotes must not cancel
them, and a generation-8 quote arriving after a generation-10 withdrawal must not
resurrect them.

---

## 6. Idempotency

Evaluating the same intent twice must not produce duplicate actions. Three
mechanisms, because one is not enough:

1. **The diff is a function of its inputs.** Same working state plus same intent
   gives the same decision — `Keep`, not `Cancel + New`.
2. **Pending state.** An outstanding request means the slot waits rather than
   re-issuing (§7).
3. **A time-bounded guard on our own requests.** After issuing a New or Replace,
   the manager waits `assume_request_lost_after_ns` for the working state to
   reflect it.

The third exists because the first two are not sufficient. Keying the guard on
generation alone would fail: generations advance on every evaluation, so a
working state lagging by a *single cycle* would defeat it and the manager would
create one order per cycle. The guard is bounded so a request that genuinely
never reached anyone does not wedge the slot forever.

---

## 7. Pending operations

While a request is outstanding the slot reports `AwaitingPending` and generates
nothing. A cancel is not re-sent every evaluation cycle; that is how a client
floods a venue and gets rate-limited.

This holds for `New`, `Cancel` and `Replace` alike, and for
`disable_all_quotes()` when a cancel is already in flight.

---

## 8. Unknown order state

Phase 3 semantics apply: `OrderStatus::Unknown` means **we do not know whether
the order exists**. It must not be read as filled, cancelled or rejected.

The slot **freezes**. No New — creating a competing order against one that might
still be working is exactly the mistake this state exists to prevent. No Replace,
for the same reason.

**The one documented safe exception:** when no quote is wanted on that side, a
single cancel is permitted. Cancelling an order that may not exist is harmless —
the venue answers "unknown order" — whereas creating one alongside it is not. The
pending guard keeps it to one attempt rather than one per cycle.

The other side is unaffected. Uncertainty about the bid does not stop the ask.

---

## 9. Stale intent and stale market

Desired state becomes **no quotes** when any of these hold:

| Condition | Rejection |
| --- | --- |
| intent older than `max_intent_age_ms` | `IntentExpired` |
| market data older than `max_market_data_age_ms` | `MarketDataStale` |
| the engine is not ready | `NotRunning` |

These **withdraw** rather than merely ignore: whatever is resting no longer
reflects a market that exists. `GenerationRegression`, `IdentityMismatch` and
`SymbolMismatch` do the opposite — they say nothing about the present, so they
change nothing.

Withdrawal produces cancel *actions*. Nothing here calls an exchange.

---

## 10. Churn controls, and why they are not strategy logic

| Control | Effect |
| --- | --- |
| `min_price_move_ticks` | how far the price must move to justify a rewrite |
| `min_quantity_move_fraction` | likewise for size |
| `min_replace_interval_ms` | minimum gap between rewrites of one slot |
| `cooldown_after_cancel_ms` | quiet period before re-quoting a cancelled slot |

**These decide *whether* to rewrite an order. They never decide *what* to
write.** If the strategy wants a materially different quote, it gets exactly the
price and size it asked for. If it does not, the resting order already expresses
its intent and a round trip buys nothing.

That distinction is what keeps them infrastructure controls. Using them to shade
prices or sizes would make the strategy's stated quotes a fiction, and every
downstream analysis of what the strategy did would be wrong.

They live in the `quote:` config block, never under `strategies.<name>`.

---

## 11. Validation

Every desired quote is checked before any action is generated, reusing Phase 3's
`validate_order` rather than a second copy that would drift from it: tick size,
lot size, min/max quantity, min notional, price bands and venue capabilities.

A failure yields `SlotOutcome::BlockedInvalid` carrying the specific
`RejectReason` — "off the lot grid" and "below the minimum notional" call for
different fixes. **No invalid order action can be generated**, and the failure is
observable rather than silent.

Two consequences worth stating:

- One invalid side does not silence the other.
- An invalid quote does **not** cancel an existing order. Withdrawing from the
  market because of a strategy bug would be a worse outcome than leaving a
  previously valid quote resting.

---

## 12. Quote kill

`disable_all_quotes(input, reason)` produces cancel actions for every managed
quote. It is the mechanism behind an operator kill, a strategy fault, a safety
halt and — later — a risk kill.

It respects ownership and pending state exactly as a normal evaluation does, and
it does not call an exchange.

---

## 13. Concurrency

Owned entirely by the trading thread, like every other component that mutates
trading state (docs/concurrency.md §1). No lock, no thread, no I/O.

| Thing | Owner |
| --- | --- |
| `QuoteIntent` | produced by the strategy runtime on the trading thread |
| working-order state | the OMS; supplied to the manager as an input |
| desired state and slot memory | the quote manager, trading thread only |
| emitted actions | returned by value; the caller decides where they go |

Nothing is shared, so nothing needs guarding.

---

## 14. Metrics

`intents_received`, `intents_rejected`, `stale_intents`, `duplicate_intents`,
`generation_regressions`, `new_actions`, `cancel_actions`, `replace_actions`,
`noop_evaluations`, `keeps`, `ownership_violations`, `unknown_order_states`,
`churn_suppressions`, `invalid_quotes_blocked`, `pending_waits`,
`foreign_orders_seen`, plus `churn()` (replaces + cancels) as the single number
an operator watches.

No dashboard is built here. These exist for Phase 13 to consume.

---

## 15. Measured cost

| Path | Time |
| --- | --- |
| `evaluate` → Keep (the steady state) | 152 ns |
| `evaluate` → New | 143 ns |
| `evaluate` → Replace | 325 ns |
| `evaluate` → rejected by screening | 85.6 ns |
| `disable_all_quotes` | 89.2 ns |

Replace costs roughly twice Keep because it runs full Phase 3 validation on both
sides and builds two actions. Keep is the case that matters, since it is what the
trading thread pays most of the time.

---

## 16. Not implemented here

No risk logic, no OMS, no execution, no exchange code, no order tracking, no
profitability reasoning. The manager emits `OrderAction`s and stops.

An `OrderAction` carries no time-in-force, no post-only flag, no signature and no
endpoint — and it never mints an order id. It addresses orders it was told about
and describes state changes it would like. Everything after that belongs to Risk,
the OMS and the adapter.
