# Portfolio accounting

> **Phase 10 is implemented. That does not mean the system is production ready,
> it does not prove any strategy is profitable, and it does not prove live
> exchange execution is safe.** It means position, cost basis, PnL, fees and
> balances are computed deterministically from confirmed fills.

Master specification: §23 Position Engine, §24 Balance Engine, §25 PnL Engine.
Reconciliation against venue state is §26 and belongs to Phase 11.

---

## 1. Ownership

There is exactly one authoritative source for each piece of state.

| State | Owner |
| --- | --- |
| order lifecycle, all three identities | `oms::OrderManager` |
| the book, BBO | `book::OrderBook` |
| working exposure (live orders) | `oms::OrderManager` |
| **position, cost basis, realized PnL, fees, balances** | **`portfolio::PortfolioAccountant`** |
| whether an action is permitted | `risk::RiskEngine` |

**Accounting is not a second OMS.** It cannot tell you whether an order is
working, and there is no API through which to ask. Its only mutating inputs are
a confirmed fill, a mark and a balance.

The OMS already computes a per-order cumulative quantity and VWAP. That is
*order* state — what happened to one order. Position is the net of every fill
across every order on a symbol. Two consumers of the same event stream
answering two different questions; neither derives its answer from the other,
and an architecture test asserts they are not interconvertible.

A consequence worth stating: a fill for an order the OMS never issued — a
manual operator trade, another process on the same account — **still moves the
position**. Accounting records it; the OMS quarantines it. That disagreement is
correct, and surfacing it to an operator is Phase 11's job.

---

## 2. The accounting model

**Weighted-average cost basis.** Not FIFO, not LIFO, not specific-lot.

§23 requires an *average entry price*, and a market maker's inventory is a
continuously churning net position rather than a set of identifiable lots. FIFO
would carry every open lot — unbounded state on the hot path — to answer a tax
question this system does not ask. Weighted average answers the question that
drives decisions, in constant space.

Given position `q` (signed, negative short) at average `a`, and a fill of signed
quantity `f` at price `p`:

| Case | Position | Average | Realized |
| --- | --- | --- | --- |
| **Open** (from flat) | `f` | `p` | 0 |
| **Increase** (same sign) | `q + f` | `(|q|·a + |f|·p) / (|q| + |f|)` | 0 |
| **Reduce** (`|f| ≤ |q|`) | `q + f` | `a` — unchanged | `(p − a)·|f|·sign(q)` |
| **Close** (`|f| = |q|`) | 0 | 0 | `(p − a)·|q|·sign(q)` |
| **Flip** (`|f| > |q|`) | `q + f` | `p` | `(p − a)·|q|·sign(q)` |

Two points that are easy to get wrong and are tested explicitly:

- **A partial reduction does not move the cost basis.** What remains is the
  same inventory bought at the same average price. Re-deriving `a` from the
  residual would make the average entry price of an untouched holding change
  because something *else* was sold.
- **A flip is two events in one message.** The whole old position closes at the
  fill price, and the residual opens at that same price. Accounting them as one
  blended transition produces a basis that is neither.

**Fees never enter the cost basis.** They accumulate separately. Folding them
in would make `average_price` stop being a price — it would be a price plus an
amortised cost, and every comparison against a market price would be wrong.

**Realized PnL is gross.**

```
net = realized + unrealized + funding − fees
```

`funding` exists because §25 lists it. Spot venues have none, and no event
source in this repository produces it; carrying it as an always-zero component
is more honest than pretending the concept does not exist.

---

## 3. Marks and unrealized PnL

Accounting **never reads a book**. It is handed a normalized `MarkPrice` with a
bid and an ask.

**Longs mark at the bid; shorts mark at the ask** — the side you would actually
have to trade against to get out. Marking both at the mid is the common
alternative and is optimistic by half the spread in both directions. For a
market maker holding inventory it wants to unwind, that half-spread is precisely
the cost being ignored, and the error always flatters.

```
unrealized = (mark − a) · |q| · sign(q)
```

A mark older than `max_mark_age_ns`, a non-positive price, or a crossed book
leaves unrealized PnL **indeterminate** rather than stale. An old number is not
a small error in PnL; it is an unknown, and a loss limit evaluated against it
would pass on evidence that no longer exists.

A fill also invalidates the previous mark: the position changed, so the number
computed against the old one describes a position that no longer exists.

---

## 4. Fees

The fee comes from the execution event and is **never recomputed**. Recomputing
it would require this layer to know the venue's schedule — the venue coupling
the boundary exists to prevent — and would produce a second, competing answer
to a question the venue has already answered.

The Phase 9 defect where a venue reported the entire trade value as the fee is
covered here permanently: `FeesAccumulateAndAreNotConfusedWithNotional` asserts
the fee is strictly less than the notional, and the pipeline test asserts the
same end to end.

---

## 5. Idempotency

Every fill carries a trade id. Each account keeps a 64-entry ring of ids it has
applied; a repeat changes **nothing** — not position, not realized PnL, not
fees, not the trade count — and is counted separately so redelivery is visible
without being alarming. The ring reports when it wraps.

This is a *separate* instance from the OMS's per-order ring, deliberately: the
OMS asks "has this order seen this trade", accounting asks "has the ledger seen
this trade", and one is not derivable from the other.

A fill with **no** trade id is refused. Without an identity it cannot be
deduplicated, so accepting it would make the ledger unverifiable.

---

## 6. Failing closed

Every failure below leaves state **completely unchanged** and is reported.
Nothing is half-applied: a fill that overflows partway through would otherwise
leave a state that looks valid and is not.

The governing question for each is: *can this make the system believe it has
LESS exposure than it actually has?*

| Failure | Behaviour | Understates exposure? |
| --- | --- | --- |
| duplicate fill | ignored, counted | no — nothing changed |
| zero / negative quantity | refused | no |
| non-positive price | refused | no |
| missing trade id | refused | no |
| negative fee (rebate) | refused | no — not modelled |
| unregistered symbol | refused, never auto-created | no |
| arithmetic overflow | refused **and the account faults** | no |
| unrepresentable cost basis | refused at the fill | no |
| stale / invalid / crossed mark | unrealized becomes indeterminate | no |
| no mark at all | gross exposure falls back to **cost**, never zero | no |
| corrupt recovery snapshot | refused entirely, nothing partially restored | no |

Two of these deserve emphasis:

- **A faulted account never reads as flat.** `position_snapshot()` returns
  `valid = false`, and risk refuses to add exposure against a position it
  cannot vouch for. An invalid position must be invalid, not a valid zero.
- **Exposure that cannot be computed never aggregates as zero.** If any
  symbol's gross exposure fails to compute, the portfolio snapshot is marked
  degraded and invalid rather than summing the rest.

---

## 7. Risk integration

Phase 7 configured `max_portfolio_notional` and the loss limits and could not
enforce them: cross-symbol state and PnL did not exist (docs/risk-engine.md
§16). They are enforced now.

`risk::PortfolioState` is declared in `mm_risk` — **risk defines what it needs
to be told, and accounting supplies it**. The dependency points downward
(`mm_portfolio` links `mm_risk`), so risk never learns an accounting layer
exists and there is no cycle. `RiskEngine` remains the decision authority;
accounting produces facts, never verdicts.

Enforcement, in the existing screening path so it fails closed exactly as the
position checks already do:

| Condition | Reason |
| --- | --- |
| limits configured, no portfolio supplied | `PortfolioUnavailable` |
| portfolio older than `max_position_age_ns` | `PortfolioStale` |
| ledger degraded | `PortfolioUnavailable` |
| gross notional over cap | `MaxPortfolioNotional` |
| loss limits configured, PnL indeterminate | `PnlIndeterminate` |
| net loss ≥ `emergency_loss` | `EmergencyLoss` |
| net loss ≥ `max_daily_loss` | `MaxDailyLoss` |
| net loss ≥ `max_session_loss` | `MaxSessionLoss` |

Three properties hold and are tested:

- **Notional limits survive an unmarkable symbol; loss limits do not.**
  Exposure is known even when its mark is not. Totalling only the markable
  symbols and calling it the portfolio PnL would understate the loss by exactly
  the ones that could not be marked.
- **Unconfigured limits demand nothing.** A zero limit is "not configured", so
  Phases 1–9 behave exactly as before.
- **Cancellation survives every portfolio failure.** Withdrawing must stay
  possible when every limit is breached and the ledger is unusable — that is
  the moment it matters most.

Losses are held as **positive magnitudes**: `max_daily_loss = 500` means "stop
at −500". Storing them signed invites a sign error in the one comparison that
must never be wrong, and a test asserts a profit is never mistaken for a loss.

---

## 8. Portfolio aggregation

`gross_notional` is the sum of `|position| · mark` — **absolute**, so a long and
a short are two exposures, not none. `net_notional` is the signed sum and is a
different question, kept separately.

Symbols are held in a contiguous vector, scanned linearly. Deliberately not an
`unordered_map`: a market maker runs tens of symbols, not thousands, and at that
size a contiguous scan of 24-byte keys beats a hash lookup that chases a pointer
into a bucket. It also allocates nothing and keeps the accounts adjacent for the
aggregation pass, which touches all of them anyway. Capacity is fixed at
construction, so no fill allocates.

---

## 9. Snapshot and recovery

`save()` returns everything needed to reconstruct the ledger: position, cost
basis, realized PnL, fees, fill count, deduplication state and the accounting
sequence, per symbol, plus balances.

**Durable storage is deliberately absent.** The master spec places persistence
and crash recovery in Phase 11 (§27, §28). What exists here is the state
representation and the recovery contract — the same boundary Phase 8 drew for
the OMS, for the same reason.

Two rules on restore:

- **A snapshot that is not self-consistent is refused**, and nothing is
  partially restored. A position with no cost basis, or a flat account carrying
  one, is rejected. A half-restored ledger is worse than none: it looks
  complete.
- **Unrealized PnL is never restored.** It is a function of a market price, and
  the market has moved. It stays indeterminate until a fresh mark arrives.

Deduplication state survives, so a fill redelivered across a restart is still
recognised.

---

## 10. Concurrency

Accounting state is owned entirely by the **trading thread**, like every other
component that mutates trading state (docs/concurrency.md §1).

| Thing | Owner |
| --- | --- |
| fills entering accounting | trading thread, drained from the execution ring |
| marks entering accounting | trading thread |
| the ledger | trading thread, exclusively |
| snapshots read elsewhere | copied by value through a seqlock |

**Phase 10 adds no new cross-thread boundary.** The one thing that crosses a
thread is a `PortfolioSnapshot`, which is trivially copyable precisely so a
reader never touches accounting state. There is no lock on the hot path because
nothing is shared.

---

## 11. Measured cost

See docs/benchmarks.md. Those are engineering measurements of this layer's own
cost. They are not exchange latency and they say nothing about profitability.

---

## 12. Limitations

- **Weighted-average only.** No FIFO, no specific-lot, no tax-lot reporting.
- **No funding.** The component exists; nothing produces it.
- **No fee tiers, rebates or maker/taker schedules** — the venue's reported fee
  is taken as given.
- **Balances are recorded, not reconciled.** §24's "reconcile against exchange
  state" is Phase 11.
- **No persistence.** State representation and recovery contract only.
- **No cross-instrument correlation, no margin model, no borrow cost.**
- **The deduplication ring is 64 entries per symbol.** A redelivery older than
  that could slip through; the account reports when the ring has wrapped.
- **It cannot tell you whether a strategy is profitable**, and no part of this
  repository will.
