# Risk engine

Risk is the safety boundary. Every order-changing action passes through it, and
nothing reaches the OMS without a decision.

The governing rule:

> **If risk cannot prove that an exposure-adding action is safe, the action is
> rejected — and cancellation remains available anyway.**

---

## 1. Responsibility

| Layer | Question |
| --- | --- |
| Strategy | What price and quantity should I quote? |
| Quote manager | What orders represent that quote? |
| **Risk** | **Is that order allowed?** |
| OMS | How do we manage that order's lifecycle? |
| Execution | How do we talk to the venue? |

Risk contains no strategy logic, no order management, no exchange code and no
PnL accounting. It is identical for paper and live and cannot tell which it is
serving — nothing in its inputs or outputs mentions an execution mode.

---

## 2. Decision model

```cpp
enum class RiskVerdict { Approve, Reduce, Reject };
```

Three outcomes, not five. "Cancel-only" is a property of the **engine**
(`Killed`, `Warning`), and "kill" is an **operator command** — neither is a
judgement about an individual order. Modelling them as per-action verdicts would
make every decision carry an opinion about the whole engine, which is confusing
to read and impossible to audit.

A `RiskDecision` carries the requested action, the approved action, the verdict,
a machine-readable `RiskReason`, the engine state, the limits it was measured
against, the timestamp and the strategy generation. It is trivially copyable and
allocation-free.

**A rejected decision carries a `Noop` as its approved action**, not a copy of
the request. A caller that forwards `approved` without inspecting the verdict
still sends nothing.

`RiskReason` is an enum, never a string. `indicates_blindness()` separates *"we
could not establish safety"* from *"we established it and the answer was no"* —
both reject, but only the first means the engine is operating blind, which is an
operational problem in its own right.

---

## 3. Position authority

There is exactly **one** authoritative position, and risk consumes it. It does
not maintain its own, because a strategy view, a quote-manager view and a risk
view would become three competing truths with no way to tell which was right.

`PositionSnapshot::valid` and its age are the engine's licence to reason at all:

- `valid == false` → `PositionUnavailable`, no new exposure.
- older than `max_position_age_ms` → `PositionStale`, no new exposure.

Cancellation stays available in both.

---

## 4. Exposure mathematics

A market maker rests on both sides at once, and the two cannot be assumed to
fill together. Position limits are therefore evaluated **worst case per side**:

```
  working_buy   = Σ remaining quantity of live or pending BUY orders
  working_sell  = Σ remaining quantity of live or pending SELL orders

  worst_case_long  = position + working_buy      // every bid fills, no ask does
  worst_case_short = position − working_sell     // every ask fills, no bid does

  limit holds  ⟺  |worst_case_long| ≤ max_position
              and  |worst_case_short| ≤ max_position
```

### Worked examples

**The brief's example.** Position +8, resting bid 1, limit 10:

```
  worst_case_long = 8 + 1     = 9    ✓
  a new bid of 1  → 8 + 1 + 1 = 10   ✓ exactly at the limit, permitted
  a further bid   → 11               ✗ rejected
```

**Why netting is wrong.** Position +9, resting ask 9, a new bid of 2:

```
  net view:        9 − 9 + 2 = 2     looks comfortable
  worst case long: 9 + 0 + 2 = 11    ✗ exceeds 10
```

Nothing guarantees the ask fills. Netting would approve an order that can take
the position past its limit.

**Sides are independent.** Long 9 with a resting bid of 1 puts `worst_case_long`
at 10, already at the limit. A *sell* touches only `worst_case_short`, so it is
permitted — which is exactly right, since selling reduces the risk that binds.

### How each order state contributes

| OMS state | Contributes | Why |
| --- | --- | --- |
| live / resting | remaining | it can fill |
| pending new | full size | it may already be working at the venue |
| pending replace | remaining | the old order is live until confirmed |
| pending cancel | remaining | **a cancel is a request, not a result** |
| unknown | **blocks entirely** | exposure cannot be established |
| terminal | nothing | it can no longer fill |

Releasing exposure on a cancel *request* rather than its confirmation is how a
maker ends up over its limit when a cancel is rejected or lost.

### Gross and side exposure

Separate notions, separately limited: `max_side_exposure` bounds one side's
working quantity, `max_working_exposure` bounds `working_buy + working_sell`.
Neither is a position limit; they bound how much is in the market at once.

### Replace

The arithmetic depends on whether the venue guarantees atomicity:

- **Atomic replace** — the old order is gone the instant the new one exists, so
  the old remaining is subtracted before the new quantity is added.
- **No atomic replace** — both can be live at once, so the full new quantity
  lands on top of the old.

Assuming atomicity the venue does not provide would understate exposure during
exactly the window in which it is highest. The caller supplies
`atomic_replace` and `replaced_order_remaining`; risk never guesses.

---

## 5. Risk state machine

```
   Disarmed ──arm──► Armed ⇄ Warning
       ▲               │        │
       │               └────────┴──► Killed / Faulted
       │                                   │
       └──────────── rearm ────────────────┘
```

| State | New exposure | Cancel | Notes |
| --- | --- | --- | --- |
| `Disarmed` | ✗ | ✓ | the default; a config that never armed must not look armed |
| `Armed` | ✓ | ✓ | normal |
| `Warning` | reduce-only | ✓ | position-reducing actions pass |
| `Killed` | ✗ | ✓ | operator recovery required |
| `Faulted` | ✗ | ✓ | an internal invariant failed |

**`Warning` is reduce-only, not decoration.** An action that would increase the
absolute position is refused; one that unwinds it passes. Without such a state
the only choices are full quoting and nothing, and the second strands whatever
position is already open. From flat, *any* fill opens a position, so nothing
reduces and both sides are refused.

**Recovery is a deliberate two-step.** `rearm()` returns a killed engine to
`Disarmed`, and `arm()` then re-validates the limits. Going straight back to
`Armed` would let a kill be undone without anyone re-checking why it happened.
`Killed → Armed` is an illegal transition.

---

## 6. Arming refuses incomplete configuration

A limit left at zero is **unset**, and an unset limit is not enforced. Arming
therefore requires `max_position`, `max_order_quantity`, `max_order_notional`
and `max_open_orders` for every configured symbol.

That is the single most dangerous configuration a risk system can have: present,
apparently running, and enforcing nothing. `validate_for_arming()` refuses it.

An **unconfigured symbol is not an unlimited one** — an action for a symbol with
no limits is rejected with `ConfigurationMissing`.

---

## 7. Fail-closed rules

Every one of these refuses new exposure while leaving cancellation available:

| Condition | Reason |
| --- | --- |
| engine disarmed / killed / faulted | `RiskDisarmed` / `RiskKilled` / `RiskFaulted` |
| system not ready | `SystemNotReady` |
| market data older than the limit | `StaleMarket` |
| no sane BBO (missing or crossed) | `ReferencePriceUnavailable` |
| position missing or stale | `PositionUnavailable` / `PositionStale` |
| any working order in an unknown state | `UnknownExposure` |
| instrument rules absent | `InstrumentInvalid` |
| symbol not configured | `ConfigurationMissing` |
| notional or exposure would overflow | `ArithmeticOverflow` |
| action belongs to another subsystem | `OwnershipViolation` |

**Unknown is never read as filled, cancelled or rejected.** A working order whose
state cannot be determined might be filling right now; adding to it would be
adding to an amount that cannot be measured.

---

## 8. Cancellation is always available

Cancellation is approved in every state above, including `Killed` and
`Faulted`. The moment you most need to withdraw is the moment things are least
certain, and a risk engine that blocks an exit turns a problem into a position.

The single exception is **ownership**: cancelling another subsystem's order is
not withdrawal, it is interference.

Cancels are counted against a rate bucket for observability but **never refused
on rate grounds**. A rate limit that blocks an exit converts congestion into
exposure.

---

## 9. Price bands

`price_band_bps` bounds how far an order may sit from the mid. Outside it,
`PriceOutsideBand` — and **not reducible**: a mispriced quote is almost always
arithmetic gone wrong, and trading less of it does not make it correct.

Non-positive prices and quantities are refused outright.

Risk also re-applies the **venue's own rules** — tick grid, lot grid, minimum
notional, quantity bounds — on every path, not only after a reduction. Risk is
the last gate before the OMS, and "the quote manager already checked" is exactly
the assumption a safety boundary should not make. This was added after a
property test caught the engine approving an off-grid quantity.

---

## 10. Reduction

When a quote exceeds a limit that a smaller order would satisfy, risk reduces
rather than refuses — **explicitly**. The decision reports `Reduce`, names the
limit that bound it, and carries both the requested and approved actions. Risk
never quietly shrinks an order.

```
   requested → evaluate → transformation → REVALIDATE → approved
```

The reduced action goes through the full pipeline again: bounds, exposure, and
the venue's rules. Checking risk, modifying the order and then sending it is how
an order nobody approved reaches a venue.

Reduction is attempted only for limits a smaller order can satisfy — position,
notional, order size, side and gross exposure. An invalid price, an overflow or
a state prohibition is not made acceptable by trading less.

**Reduction can fail.** A quantity below the venue's minimum notional or off the
lot grid is not a smaller order, it is an invalid one; the result is a rejection.

The largest permitted quantity is found by binary search over the lot grid, with
each step re-running the real checks. Searching rather than solving each limit
algebraically keeps one source of truth — the checks themselves — so a new limit
is automatically respected by the reduction path.

**Risk never increases requested exposure.** Asserted by a 5,000-iteration
property sweep.

---

## 11. Rate limits and burst control

Token buckets with injected time: `capacity` is the burst allowance,
`per_second` the sustained rate. A market-data burst legitimately produces a
cluster of actions; the reserve absorbs it while bounding the sustained rate.

| Action | On exhaustion |
| --- | --- |
| new | rejected, `RateLimit` |
| replace | rejected, `RateLimit` |
| **cancel** | **always allowed** |

A zero rate disables a limiter. That is the one place where "unset means
unlimited" is correct, because the alternative — blocking all trading on a
missing config value — is worse.

Time is injected so tests advance a `ManualClock` rather than sleeping. A rate
limiter tested against the wall clock is slow, flaky, and silent about the
boundary cases that matter.

---

## 12. Integer safety

No floating point touches a price, quantity, notional, limit or exposure. The
only doubles are basis-point distances for the price band and a
quantity-difference fraction — thresholds, never sizes.

Phase 2's arithmetic computes correctly for every value a real venue produces
but narrows a 128-bit intermediate without checking, and `+`/`-` wrap like any
integer. That is right on the hot path, where inputs are pre-validated, and
wrong here: a wrapped notional would turn an absurd order into a small one and
approve it.

Phase 7 therefore adds `checked_notional_of`, `checked_add`, `checked_sub` and
`saturating_abs`. Every exposure computation uses them, and any overflow is
`ArithmeticOverflow` — a rejection, never a wrap. `saturating_abs` matters
because negating the minimum representable value is undefined; saturating means
a caller comparing against a limit gets a rejection rather than a crash.

---

## 13. Concurrency

Owned by the trading thread, like every other component that mutates trading
state (docs/concurrency.md §1). One authoritative mutation path, no locks, no
I/O. Position and working-order state arrive as inputs; the engine never keeps a
competing copy.

---

## 14. Observability

`approvals`, `reductions`, `rejections`, `kills`,
`cancels_permitted_while_blocked`, and a per-cause rejection tally indexed by
`RiskReason`. A risk engine that refuses everything must be diagnosable from
counters alone.

---

## 15. Measured cost

Shared laptop, noticeably noisy; treat these as indicative and ratios as the
signal.

| Path | Time |
| --- | --- |
| `WorstCaseExposure` alone | 10–14 ns |
| Killed rejection (state gate only) | 74–110 ns |
| Cancel | 79–112 ns |
| **Approve** | **~271 ns** |
| Reject by a hard limit | ~340 ns |
| Reduce (binary search) | 630–1490 ns |

Reduction is the most expensive path by construction: a binary search over the
lot grid, each step re-running the full bounds and exposure checks. It only
fires when a limit binds.

The first version had **rejection costing more than reduction** (1318 ns vs
1058), because a reducible failure exhausted the whole search before giving up
even when there was no room at all. Testing a single lot first cut it to ~340 ns.
The number pointed at a real inefficiency; that is what benchmarks are for.

---

## 16. Known limitations, and what is deferred

- **Portfolio limits are configured but not enforced.** `max_portfolio_notional`
  and `max_open_orders_total` need cross-symbol state that does not exist until
  the portfolio layer in Phase 10. The fields and the interface are present; the
  enforcement is deliberately absent rather than fabricated from per-symbol
  state that would not be a portfolio view.
- **Loss limits are not enforced.** `max_daily_loss`, `max_session_loss` and
  `emergency_loss` require realized and unrealized PnL, which is Phase 10. They
  remain in configuration and are unused here.
- **Position sequence ordering is recorded, not enforced.** `PositionSnapshot`
  carries a monotonic sequence, but ordering snapshots is the position owner's
  responsibility — risk evaluates what it is given. Enforcing it here would put
  risk in the business of tracking position history, which is exactly the
  competing-truth problem §19 exists to prevent.
- Positions are per symbol; cross-instrument correlation is out of scope.
- No venue-specific rate limits. Exchange capabilities will supply those later;
  nothing here hard-codes a particular venue's numbers.

---

## 17. Not implemented here

No OMS, no execution, no dashboard, no PnL accounting, no strategy logic, no
exchange code. Risk emits decisions and stops.
