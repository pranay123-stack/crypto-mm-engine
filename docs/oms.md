# Order management system

> **The OMS is the single authoritative owner of order lifecycle state.**
> Nothing else in the engine is allowed a second opinion about what is resting.

It answers one question:

> *What orders exist, in what state, and what do we actually know about them?*

It does not decide what to quote (the strategy), what state that implies (the
quote manager), or whether that state is permissible (risk). It receives an
approved action and owns everything that happens to it afterwards.

---

## 1. Position in the pipeline

```
 Strategy ─► Quote Manager ─► OrderAction ─► Risk ─► ApprovedAction ─► ┌─────┐ ─► adapter ─► venue
                                                                      │ OMS │
            working state ◄──────────────────────────────────────────  │     │ ◄── execution events
            exposure      ◄──────────────────────────────────────────  └─────┘
```

The loop closes here. What the OMS learns from the venue is what the quote
manager and risk see on the next cycle — not a parallel copy either of them
maintains.

---

## 2. Order identity

Three identities, never conflated, each absent at a different stage:

| Identity | Minted by | Exists from | Purpose |
| --- | --- | --- | --- |
| `LogicalOrderId` | the OMS | creation | the engine's own handle; survives a replace |
| `ClientOrderId` | the OMS | first request | what we send; what the venue echoes |
| `ExchangeOrderId` | the venue | acknowledgement | what the venue calls it |

A newly created order has a logical id and nothing else. After the request is
built it has a client id. Only after the venue answers does it have a venue id.
Code that assumes all three are present is code that breaks at the moment an
acknowledgement is late.

They are distinct types, so the compiler refuses the mix-up rather than leaving
it to review.

### Client order id format

```
<prefix>-<session-base36>-<sequence-base36>          e.g.  mmp-1x2-2f
```

- The **prefix** distinguishes paper from live (`mmp` / `mml` in the shipped
  configs) so a venue's order listing shows which run an order came from.
- The **session** distinguishes one run from the next, so a restart cannot mint
  an id that collides with an order the previous run left resting.
- The **sequence** is monotonic within a session.

**Generation fails rather than truncating.** Two orders sharing a client id is
not recoverable: every fill for either becomes ambiguous. `next()` returns
`false` and the order is refused, which is a bad outcome and a much smaller one.
`oms.max_client_id_length` is validated at startup against the prefix length, so
this fails when someone edits a config file rather than mid-session.

`ClientOrderIdGenerator::parse()` runs the format backwards. That is what lets
reconciliation tell *our* orphan from somebody else's order.

---

## 3. Lifecycle state machine

```
                              ┌─────────┐
                              │ CREATED │
                              └────┬────┘
                                   │ request sent
                              ┌────▼────────┐
                    ┌─────────┤ PENDING_NEW ├──────────┐
                    │         └────┬────────┘          │
              reject│              │ ack               │ timeout / transport loss
              ┌─────▼────┐    ┌────▼────┐         ┌────▼────┐
              │ REJECTED │    │ WORKING │◄────────┤ UNKNOWN │
              └──────────┘    └──┬───┬──┘  resolve└────▲────┘
                                 │   │                 │
                     partial fill│   │cancel/replace    │ timeout,
                          ┌──────▼─┐ │ requested        │ reconciliation
                          │PARTIAL │ │                  │
                          │ FILLED │ │            ┌─────┴──────┐
                          └───┬────┘ ├────────────► PENDING_*  │
                              │      │            │ CANCEL /   │
                         full │      │            │ REPLACE    │
                         fill │      │            └─────┬──────┘
                          ┌───▼──────▼───┐              │
                          │    FILLED    │              │ cancel confirmed
                          └──────────────┘         ┌────▼──────┐
                                                   │ CANCELLED │
                                                   └───────────┘

  ORPHANED: reachable from anywhere; an order at the venue we cannot attribute.
  EXPIRED:  the venue retired the order on its own terms.
```

Terminal states — `FILLED`, `CANCELLED`, `REJECTED`, `EXPIRED` — are absorbing.
Nothing leaves them. A late acknowledgement for a rejected order is *not news*,
and applying it would resurrect a dead order along with phantom exposure.

Three properties worth stating explicitly, because each is a mistake that is
easy to make and expensive to find:

- **`PENDING_CANCEL` does not imply cancellation.** It can end in `CANCELLED`,
  `FILLED`, `PARTIALLY_FILLED`, `EXPIRED` — or back at `WORKING` if the venue
  refuses. The request is not the result.
- **`UNKNOWN` is reachable from everything except `CREATED`**, and leaves only
  on evidence. A timer tick is not evidence, however many of them there are.
- **`UNKNOWN` still consumes exposure.** An order we cannot account for must be
  assumed to exist.

The enum values are fixed and asserted in a test: journal records are persisted
and replayed, and reordering the values would silently reinterpret every record
ever written.

---

## 4. Ownership

Every order carries the strategy identity, quote slot and generation that
created it. Ownership is checked on **every** modifying request.

The OMS will hold orders one strategy did not create — another strategy's,
an operator's, an emergency inventory-reducing order. A cancel from the wrong
owner is refused (`OmsReject::OwnershipViolation`) and counted, not honoured.

This is deliberately a *separate* check from risk's. Risk asks whether the owner
is the one it was configured to expect. The OMS asks whether that owner may
touch *this particular order*. Both must pass.

`working_state(symbol, owner)` reflects this: an order belonging to someone else
is reported in `foreign_order_count` and never presented as one of the caller's
slots. That is what makes one OMS safe to share.

---

## 5. Request flow

```
ApprovedAction ─► submit() ─► ownership + state checks ─► mint client id
                                                       ─► journal RequestSent
                                                       ─► adapter.submit()
```

`submit()` takes a `risk::ApprovedAction` and nothing else. The token is
default-constructible (it has to be embeddable in a `RiskDecision` and stay
trivially copyable) but its validity flag is settable only by `RiskEngine`
through friendship. A caller cannot hand-build one, and an invalid one is
refused with `NotRiskApproved`.

There is no other entry point. `QuoteManager → OMS` does not exist as a callable
path.

---

## 6. Pending operations

An order has at most **one** outstanding request. While it does:

| Request | Behaviour |
| --- | --- |
| second `New` on the same slot | refused — `PendingOperation` |
| second `Cancel` | refused — `DuplicateRequest`; the first is still in flight |
| `Replace` during `PendingReplace` | **coalesced** — the newest target wins, no second request |
| `Replace` with an older generation | refused — `StaleGeneration` |

Coalescing is reported as `SubmitResult::coalesced`, distinct from a plain
rejection: the caller's intent *was* recorded, it simply did not produce a
second request. Two in-flight amendments of one order is how a venue ends up
with two orders.

---

## 7. Event ordering

```
  exec io thread          SPSC ring            OMS thread
  ──────────────          ─────────            ──────────
  on_execution(e)  ──►  enqueue only  ──►  process_events()  ──►  apply
```

The adapter's callback runs on the I/O thread and does exactly one thing:
enqueue. All state mutation happens on the OMS thread when `process_events()`
drains the ring. Nothing else touches order state, so nothing needs a lock.

Ordering defences, applied in this order:

1. **Structural validation.** A self-contradictory event is quarantined before
   anything looks at it (`quarantined_events`).
2. **Attribution.** An event naming an order we have no record of never creates
   one. It is counted as an orphan and quarantined.
3. **Terminal check.** Events for terminal orders are duplicates by definition.
4. **Sequence check.** An event older than the last applied one is stale
   (`out_of_order_events`).
5. **Already-acknowledged check.** A second ack — a redelivery, or two venue
   streams racing so the ack lands after its own fill — is counted as a
   duplicate. Not as an illegal transition: nothing illegal happened, the event
   simply carried no news.

---

## 8. Fills

Fills are idempotent by trade id. Each order keeps a 32-entry ring of trade ids
it has already applied; a repeat increments `duplicate_fills` and changes
nothing. Venues redeliver execution reports after a private-stream reconnect,
and double-counting a fill corrupts position and PnL at the same instant with
nothing downstream able to detect it afterwards.

The ring reports `overflowed()` when it wraps, so "we may no longer be able to
detect a duplicate" is visible rather than assumed away.

Every arithmetic step is checked:

- `cumulative_quantity` may never exceed `original_quantity`. An over-fill is
  **quarantined, not clamped** — clamping would leave us believing we hold less
  than we do, which is the direction that costs money.
- `remaining()` is computed with `checked_sub` and can never go negative.
- The average fill price is a `__int128` volume-weighted accumulation, not a
  running float.

Quantity comparisons downstream use `remaining()`, never the original
quantity — a partially filled order does not represent the size it was
submitted with.

---

## 9. Cancellation

**A cancel request never releases exposure.** The order stays in
`PENDING_CANCEL`, which still consumes exposure, until the venue confirms.
Releasing early would let the engine quote against capacity it does not have,
in the window where the order may still be filling.

A rejected cancel returns the order to `WORKING`. A cancel that loses a race
with a fill ends in `FILLED`. Both are ordinary outcomes, not errors.

---

## 10. Replacement

An accepted replace atomically swaps price, quantity and both ids. While
`PENDING_REPLACE`, the **live** parameters are untouched — the new values sit in
separate `pending_*` fields — so a refused amendment leaves the order exactly as
the venue still has it, including its place in the queue.

Where the venue has no atomic replace, the quote manager decomposes it into
`CancelThenNew` (docs/quote-manager.md §4). The OMS enforces the boundary:

> **The new order is created only after the cancel is confirmed.**

A `New` arriving on a slot whose occupant is `PENDING_CANCEL` is *deferred*, not
refused (`SubmitResult::deferred`). The deferral table is sized to `max_orders`
so a deferral never allocates, but deferrals are rare — they happen only during
a cancel-then-new window — so an occupancy counter short-circuits the scan. That
matters: without it, every order resolution would walk 221 KB at the shipped
`max_orders: 1024` to almost always find nothing. If the cancel is rejected, the deferred
order is discarded — the original is still working, and creating the replacement
would double the exposure. If the cancel never resolves, the deferred order is
never born: `UNKNOWN` is not a cancellation boundary.

---

## 11. Reconciliation

`reconcile(VenueSnapshot)` compares the venue's open-order listing against local
state and produces a report. Six discrepancy kinds: `OrphanAtVenue`,
`MissingAtVenue`, `QuantityMismatch`, `PriceMismatch`, `StatusMismatch`,
`IdentityCollision`.

**An incomplete snapshot is refused outright.** Concluding "the venue does not
have this order" from a truncated or paginated listing would report every real
order as missing. Refusals are counted separately (`reconciliations_refused`) so
a query that never returns complete results is visible rather than looking like
a run of clean reconciliations.

Two consequences of taking the evidence seriously:

- **An orphan is not assumed to be ours.** `ClientOrderIdGenerator::parse()`
  decides, and the answer is carried on the discrepancy as `identity_is_ours`.
  An order that is not ours is reported and left alone.
- **A missing order becomes `UNKNOWN`, never `CANCELLED`.** A complete listing
  that omits an order we believe is working contradicts our own state;
  continuing to assert `WORKING` would keep risk counting exposure the venue
  says is not there. But "not in the listing" is not proof of cancellation, and
  inventing one would be fabricating an outcome nobody reported.

The snapshot race is handled directly rather than heuristically: an order
acknowledged *after* the snapshot was received is skipped, because the venue
built that listing before it knew about the order.

Reconciliation **reports**; it does not silently rewrite fill history. A
`QuantityMismatch` is surfaced for an operator, not applied.

---

## 12. Timeout semantics

On expiry of `new_request_timeout_ms` / `cancel_request_timeout_ms` /
`replace_request_timeout_ms`, the order becomes **`UNKNOWN`**.

Not cancelled. Not rejected. A timeout tells us only that we do not know, and
marking the order anything else would be fabricating an outcome nobody observed.
The consequences are deliberate:

- exposure remains consumed, and `ExposureSnapshot::determinate` becomes false,
  so risk stops *adding* rather than continuing on a number it cannot trust;
- the quote manager freezes the slot (docs/quote-manager.md §8);
- only an execution event, a reconciliation, or an explicit `resolve()` moves
  the order out.

A zero timeout is refused at config load: it would mark every request unknown
the instant it was sent, which is indistinguishable from a permanently broken
venue.

---

## 13. Restart recovery

`snapshot_for_recovery()` returns a serialisable `RecoveryState` — session id,
the next value of every id counter, and the order records. `restore()` rebuilds
the OMS from it.

The honest boundary: **this is a state snapshot, not a database.** Durability,
fsync policy and crash-consistent writing are not implemented in Phase 8. What
*is* implemented is the semantics of coming back:

- Acknowledged orders are restored with all three identities intact.
- **Every order with an outstanding request comes back as `UNKNOWN`.** We
  crashed without learning its outcome; recovering it as `PENDING_NEW` would
  assert a request is still in flight over a socket that no longer exists.
- Id counters resume past what the previous session used, so a restart cannot
  mint an id colliding with an order still resting at the venue.

The first act after a restart should be a reconciliation, which is what turns
those `UNKNOWN`s back into facts.

---

## 14. Journaling

Every state change is journalled: from-state, to-state, event type, all three
identities, quantities, and a monotonic engine sequence — enough to reconstruct
the transition, which is what makes replay possible. Sequencing uses the
engine's own counter, not a venue timestamp from a clock we do not control.

`IOrderJournal` has two implementations in this phase: `NullOrderJournal` and
`InMemoryOrderJournal` (bounded; drops oldest and **counts** what it dropped, so
a full journal is visible rather than silent). Durable journalling is a later
phase.

---

## 15. Concurrency model

| Thing | Owner |
| --- | --- |
| order records, indices, metrics | the OMS thread, exclusively |
| `on_execution()` | the exec I/O thread — enqueue only, no mutation |
| the event ring | SPSC: exec I/O writes, OMS reads |
| `submit()` | the trading thread (same thread as the OMS in this design) |

See docs/concurrency.md §1. No locks, because nothing is shared.

---

## 16. Paper and live

There is exactly one `OrderManager`. Paper and live differ **only** in which
adapter is attached below the execution interface. There is no mode flag on the
constructor, no `if (paper)` anywhere in the OMS, and a test asserts the OMS is
not constructible with one. `tools/check_exchange_boundary.py` enforces the same
rule mechanically, with a deliberately narrow token list so the OMS's own
`is_live()` — an order's liveness — never trips it.

---

## 17. Invariants

Enforced and tested (tests/failure/test_oms_failure.cpp):

1. Terminal orders never return to Working.
2. Duplicate fills never increase cumulative fill twice.
3. Cumulative fill never exceeds original quantity.
4. Remaining quantity never becomes negative.
5. Unknown never silently becomes Cancelled.
6. The OMS cannot create an order without risk approval.
7. Non-owned orders cannot be modified.
8. Pending operations cannot create duplicate requests.
9. CancelThenNew never creates the new order before the cancellation boundary.
10. The same event sequence produces the same final state.
11. An exchange order identity maps to at most one local order.
12. A client order identity maps to at most one live logical order.

Invariants 3, 4, 11 and 12 are additionally re-checked after **every** event in
a deterministic generated-sequence sweep (§42): 24 seeds × 60 steps drawn from
submits, cancels, replaces, fills, duplicate events, late acks, events for
unknown orders, timer ticks and reconciliations.

---

## 18. Metrics

`orders_created`, `new_requests`, `cancel_requests`, `replace_requests`,
`acknowledgements`, `rejects`, `fills`, `partial_fills`, `duplicate_events`,
`duplicate_fills`, `duplicate_requests`, `out_of_order_events`,
`quarantined_events`, `illegal_transitions`, `unknown_states`, `timeouts`,
`stale_operations`, `ownership_violations`, `deferred_news`, `reconciliations`,
`reconciliations_refused`, `orphan_orders`, `missing_orders`,
`not_risk_approved`, plus per-reject counts and two latency histograms.

No dashboard is built here. These exist for Phase 13 to consume.

---

## 19. Measured cost

See docs/benchmarks.md for the numbers and the conditions they were taken under.

---

## 20. Not implemented here

No venue execution adapter, no paper execution, no live execution, no position
or PnL accounting, no portfolio risk, no dashboard, no durable persistence, and
no strategy logic of any kind. The OMS owns order lifecycle state and stops.
