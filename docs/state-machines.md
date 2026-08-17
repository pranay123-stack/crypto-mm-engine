# State machines

Four state machines govern the system. Each has a single owning module, an
explicit transition table, and a test that asserts every illegal transition is
rejected rather than ignored.

---

## 1. Order lifecycle (owned by OMS)

> Implemented in Phase 8. The full treatment — identity, ownership, event
> ordering, reconciliation, recovery — is in [oms.md](oms.md); this section is
> the state machine itself. The names below are the implemented ones
> (`include/mm/oms/OrderTypes.hpp`, table in `src/oms/OrderTypes.cpp`).

```
                    ┌─────────┐
                    │ CREATED │  record allocated, nothing sent
                    └────┬────┘
                         │ request sent
                    ┌────▼────────┐
              ┌─────│ PENDING_NEW │────┐ timeout / transport loss
       reject │     └────┬────────┘    │
              │          │ ack         ▼
              ▼          │        ┌──────────┐
        ┌──────────┐     │        │ UNKNOWN  │
        │ REJECTED │     │        └────┬─────┘
        └──────────┘     │             │ evidence: event,
                         │             │ reconciliation, resolve()
                    ┌────▼────┐◄───────┘
                    │ WORKING │
                    └──┬───┬──┘
                       │   │ partial fill    ┌──────────────────┐
                       │   └────────────────►│ PARTIALLY_FILLED │
                       │                     └───┬────┬─────────┘
        cancel/replace │                  cancel │    │ fill
                 ┌─────▼──────────┐              │    │
                 │ PENDING_CANCEL │◄─────────────┘    │
                 │ PENDING_REPLACE│                   │
                 └──┬──┬──┬───┬───┘                   │
        confirmed   │  │  │   │ rejected              │
                    │  │  │   └──────► WORKING        │
                    │  │  │ filled (cancel lost race) │
                    │  │  └───────────────────────────┤
                    ▼  ▼                              ▼
             ┌───────────┐  ┌─────────┐  ┌─────────┐  ┌────────┐
             │ CANCELLED │  │ EXPIRED │  │REJECTED │  │ FILLED │
             └───────────┘  └─────────┘  └─────────┘  └────────┘
                    └────────────┴────────────┴────────────┘
                                terminal states

  ORPHANED: reachable from anywhere; an order at the venue we cannot attribute.
            Leaves only to CANCELLED or UNKNOWN, by operator or reconciliation.
```

### Rules

1. **Terminal is terminal.** `FILLED`, `CANCELLED`, `REJECTED`, `EXPIRED` accept
   no outbound transition. A late event for a terminal order is counted as a
   duplicate and discarded — it never resurrects the order.
2. **`UNKNOWN` is live for risk.** An order in `UNKNOWN` counts fully against
   position, notional and order-count limits, and makes
   `ExposureSnapshot::determinate` false. The assumption that costs money is "it
   probably didn't make it"; the safe assumption is "it might be working".
3. **Fills are idempotent.** Each fill carries a venue trade id. Every order
   keeps a 32-entry ring of seen trade ids; a repeat is counted as a duplicate
   and touches nothing. This is the single most important invariant in the
   accounting path — Binance *will* redeliver execution reports after a
   user-stream reconnect. The ring reports when it wraps, so "we may no longer
   detect a duplicate" is visible rather than assumed away.
4. **Monotonic filled quantity.** Cumulative quantity may only increase and may
   never exceed the original. A report violating either is **quarantined, not
   clamped** — clamping would leave us believing we hold less than we do.
5. **Out-of-order events.** Every report carries the venue's update sequence. A
   report older than the order's last applied sequence is discarded as stale. A
   second acknowledgement for an already-acknowledged order is counted as a
   duplicate, not as an illegal transition: nothing illegal happened, the event
   simply carried no news.
6. **Cancel is a request, not a result.** `PENDING_CANCEL` does not release
   exposure. Exposure drops on a confirmed `CANCELLED`. A cancel reject returns
   the order to `WORKING` — it is still resting.
7. **Replace is atomic-or-nothing.** Where a venue has no atomic replace, the
   *quote manager* decomposes it consciously into `CancelThenNew`
   (quote-manager.md §4) rather than the adapter doing it silently, and the OMS
   enforces the boundary: the new order is created only after the cancel is
   confirmed. During any such window exposure is the *union* of both orders,
   never the difference.

### Transition table (full table in `src/oms/OrderTypes.cpp`)

| From | Event | To | Side effect |
| --- | --- | --- | --- |
| CREATED | RequestSent | PENDING_NEW | mint client id, journal |
| PENDING_NEW | Acknowledged | WORKING | bind exchange id |
| PENDING_NEW | Rejected | REJECTED | release exposure |
| PENDING_NEW | RequestFailed | UNKNOWN | flag indeterminate exposure |
| PENDING_NEW | RequestTimedOut | UNKNOWN | flag indeterminate exposure |
| WORKING | PartiallyFilled | PARTIALLY_FILLED | apply fill, update VWAP |
| WORKING | Filled | FILLED | apply fill, release exposure |
| WORKING | CancelRequested | PENDING_CANCEL | exposure unchanged |
| WORKING | ReplaceRequested | PENDING_REPLACE | live params unchanged |
| PENDING_CANCEL | Cancelled | CANCELLED | release exposure; release deferred new |
| PENDING_CANCEL | CancelRejected | WORKING | discard any deferred new |
| PENDING_CANCEL | Filled | FILLED | apply fill (cancel lost the race) |
| PENDING_REPLACE | Replaced | WORKING | swap price, qty, both ids |
| PENDING_REPLACE | ReplaceRejected | WORKING | live params untouched |
| PARTIALLY_FILLED | Cancelled | CANCELLED | release remaining |
| UNKNOWN | *any execution event* | mapped state | resync |
| UNKNOWN | ReconciliationAbsent | **UNKNOWN** | reported; **never** fabricated terminal |
| *any live state* | ReconciliationAbsent | UNKNOWN | complete listing contradicts us |
| *any terminal* | *any* | *no change* | count duplicate/illegal |

> An earlier draft of this table mapped "reconciliation could not find the
> order" to `REJECTED`. That is exactly the fabrication invariant 5 forbids: a
> venue's silence about an order is not a report that it was refused. The
> implemented behaviour is `UNKNOWN`, which keeps the exposure and demands
> evidence.

---

## 2. Session state (owned by `exchange/common/`)

Implemented as `mm::exchange::SessionState`, validated by `is_legal_transition`,
and shared by market-data and execution sessions. This is the **only** session
machine in the platform.

```
   Disconnected ──► Connecting ──► Connected ──┬──► Authenticated ──┐
        ▲                │                     │                    │
        │                │                     └────────────────────┤
        │                ▼                                          ▼
        │            Backoff ◄───────────────────────────────── Subscribed
        │                │                                          │
        │                └──► Connecting                            ▼
        │                                                        Syncing
        │                                                       ▲     │
        │                                    ResyncRequired ────┘     ▼
        │                                          ▲            ┌── Ready ──┐
        │                                          └────────────┤           │
        │                                                       └── Stale ◄─┘
        └───────────────────────── (from any state) ──────────────────┘
                        Error ◄── (from any state, operator-clear only)
```

Only `Ready` permits quoting, expressed as the single predicate
`is_quotable(state)`. Every other state suspends quoting for the affected
symbol.

| State | Meaning |
| --- | --- |
| `Disconnected` | No transport. |
| `Connecting` | Dialling / TLS handshake. |
| `Connected` | Transport up, nothing subscribed. |
| `Authenticated` | Credentials accepted — private streams only; public market data skips it. |
| `Subscribed` | Venue confirmed the subscription; updates are being buffered. |
| `Syncing` | Applying a snapshot and reconciling buffered updates. |
| `Ready` | Gap-free, fresh, internally consistent. **Quotable.** |
| `Stale` | Socket alive but data too old to trust. |
| `ResyncRequired` | Sequence gap or invariant violation; book discarded. |
| `Backoff` | Waiting before a reconnect attempt. |
| `Error` | Unrecoverable without operator action. |

### Illegal transitions that matter

These are rejected, not merely discouraged, and each has a test:

- **`Subscribed → Ready`.** A subscription says nothing about whether a snapshot
  has been applied. Allowing it would let a strategy quote against an empty book.
- **`ResyncRequired → Ready`.** Recovery must pass back through `Syncing`.
- **`Error → anything but Disconnected`,** including `Backoff`. Otherwise
  `Error → Backoff → Connecting → … → Ready` would let an unrecoverable session
  quietly retry its way back to trading.
- **`Disconnected → Backoff`.** From there the next step is an attempt, not a wait.

Self-transitions are legal and idempotent: adapters legitimately re-announce
state after a heartbeat, and treating that as a violation would produce noise
rather than safety.

### Staleness

`Stale` earns its place because a socket that stays open while data stops is the
failure mode that loses money quietly — the book looks fine and is minutes old.
`IExchangeMarketData::data_age_ns()` is the detector, compared against
`safety.max_market_data_age_ms`. A symbol that has never produced data reports
`Nanos::max()`, not zero: a silent symbol must never look healthy.

Reconnect uses exponential backoff with jitter (200 ms → 30 s cap) to avoid
synchronized reconnect storms against the venue after a shared outage.

## 3. Book synchronization (owned by `orderbook/` + `market_data/`)

Binance's documented depth-sync procedure, encoded as a state machine so it is
testable rather than a comment:

```
1. open the diff-depth stream, buffer events
2. GET /api/v3/depth?limit=5000 → snapshot with lastUpdateId = U0
3. drop buffered events where u <= U0
4. the first applied event must satisfy  U <= U0+1 <= u   ── else restart from 1
5. thereafter each event must satisfy    U == prev_u + 1  ── else RESYNC
6. apply: qty == 0 → erase level; else set level
```

Invariants checked on every mutation (`orderbook/BookInvariants.hpp`):

```
best_bid < best_ask          (crossed book → RESYNC)
all quantities  > 0          (a zero level must have been erased)
bids strictly descending, asks strictly ascending
level count <= configured max depth
last_update_id monotonically increasing
```

A violated invariant never produces a "best effort" book. It sets `RESYNC`, drops
the book, cancels the symbol's quotes, and rebuilds. There is no path in the code
where an invalid book reaches a strategy.

---

## 4. System trading state (owned by `engine/`)

```
   BOOTING ──readiness gate──► ACTIVE ◄──operator resume + all-clear──┐
      │                          │                                    │
      │                          │ any safety trigger                 │
      │                          ▼                                    │
      │                      SAFE_MODE ─────────────────────────────┘
      │                          │
      │                          │ kill switch / fatal
      ▼                          ▼
   HALTED ◄──────────────────────┘
      ▲
      │  shutdown complete
   DRAINING ◄── shutdown requested (from any state)
```

**Readiness gate** (all must hold to enter `ACTIVE`):

```
✓ exchange authenticated            ✓ open orders fetched and reconciled
✓ positions fetched and reconciled  ✓ balances fetched and reconciled
✓ every symbol's book SYNCED        ✓ user data stream connected
✓ market data age < threshold       ✓ risk configuration valid and loaded
✓ no kill switch engaged            ✓ clock skew within tolerance
✓ journal writable                  ✓ live mode: all live gates satisfied
```

The gate is re-evaluated on every transition into `ACTIVE`, including resumption
from `SAFE_MODE`. A restart never begins quoting on the strength of "it worked
before the restart".

**Kill-switch scopes** are nested: `GLOBAL ⊃ EXCHANGE ⊃ SYMBOL ⊃ STRATEGY`.
Engaging any scope runs the same sequence within it — *stop new orders → cancel
all → verify cancellation → hold*. Disengaging requires an explicit operator
action and re-runs the readiness gate; a kill switch never clears itself on a
timer, because the condition that tripped it does not clear itself either.
