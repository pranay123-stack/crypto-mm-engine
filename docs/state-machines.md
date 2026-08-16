# State machines

Four state machines govern the system. Each has a single owning module, an
explicit transition table, and a test that asserts every illegal transition is
rejected rather than ignored.

---

## 1. Order lifecycle (owned by OMS)

```
                    ┌─────────┐
                    │ CREATED │  allocated, client_id assigned, not sent
                    └────┬────┘
                         │ submit
                    ┌────▼────────┐
              ┌─────│ SUBMITTING  │────┐ reject / send failure
              │     └────┬────────┘    │
         ack  │          │ timeout     ▼
              │          │        ┌──────────┐        ┌──────────┐
              │          └───────►│ UNKNOWN  │───────►│ REJECTED │
              │                   └────┬─────┘  recon └──────────┘
        ┌─────▼──────┐                 │ recon: found
        │ACKNOWLEDGED│◄────────────────┘
        └─────┬──────┘
              │ resting on book
        ┌─────▼──────┐   partial fill    ┌──────────────────┐
        │   ACTIVE   │◄─────────────────►│ PARTIALLY_FILLED │
        └──┬───┬───┬─┘                   └───┬────┬─────────┘
           │   │   │                         │    │
    cancel │   │   │ replace          cancel │    │ fill
           │   │   │                         │    │
   ┌───────▼─┐ │ ┌─▼──────────────┐          │    │
   │ CANCEL_ │ │ │ REPLACE_PENDING│◄─────────┘    │
   │ PENDING │ │ └─┬──────────┬───┘               │
   └──┬───┬──┘ │   │ ack      │ reject            │
      │   │    │   ▼          ▼                   │
      │   │    │ (new order) (stay ACTIVE)        │
 ack  │   │ reject                                │
      │   └──────────────► ACTIVE ◄───────────────┘
      ▼                                            │ fully filled
 ┌───────────┐   ┌─────────┐   ┌─────────┐   ┌────▼───┐
 │ CANCELLED │   │ EXPIRED │   │REJECTED │   │ FILLED │
 └───────────┘   └─────────┘   └─────────┘   └────────┘
      └──────────────┴─────────────┴─────────────┘
                    terminal states
```

### Rules

1. **Terminal is terminal.** `FILLED`, `CANCELLED`, `REJECTED`, `EXPIRED` accept
   no outbound transition. A late event for a terminal order is recorded as a
   duplicate/late event and discarded — it never resurrects the order.
2. **`UNKNOWN` is live for risk.** An order in `UNKNOWN` counts fully against
   position, notional, and order-count limits. The assumption that costs money
   is "it probably didn't make it"; the assumption that is safe is "it might be
   working".
3. **Fills are idempotent.** Each fill carries an exchange trade id. The OMS
   keeps a per-order set of seen trade ids; a repeat is counted as a duplicate
   and does not touch position or PnL. This is the single most important
   invariant in the accounting path — Binance *will* redeliver execution reports
   after a user-stream reconnect.
4. **Monotonic filled quantity.** `cum_qty` may only increase, and may never
   exceed `orig_qty`. A report violating either is rejected and trips
   reconciliation.
5. **Out-of-order events.** Every report carries the exchange's update sequence
   (`E`/`u` on Binance). A report older than the order's last applied sequence is
   discarded as stale.
6. **Cancel is a request, not a result.** `CANCEL_PENDING` does not decrement
   exposure. Exposure drops on a confirmed `CANCELLED`. A cancel reject returns
   the order to `ACTIVE` — it is still working.
7. **Replace is modeled as atomic-or-nothing.** On venues where replace is not
   atomic, the adapter decomposes it into cancel→submit and the OMS tracks two
   orders with a linkage id; exposure is the *union* during the window, never
   the difference.

### Transition table (excerpt — full table in `oms/OrderStateMachine.cpp`)

| From               | Event              | To                 | Side effect                    |
| ------------------ | ------------------ | ------------------ | ------------------------------ |
| CREATED            | Submit             | SUBMITTING         | reserve risk, journal          |
| SUBMITTING         | Ack                | ACKNOWLEDGED       | bind exchange id               |
| SUBMITTING         | Reject             | REJECTED           | release risk                   |
| SUBMITTING         | SendFailure        | UNKNOWN            | schedule reconcile             |
| SUBMITTING         | AckTimeout         | UNKNOWN            | schedule reconcile             |
| ACKNOWLEDGED       | New                | ACTIVE             | —                              |
| ACTIVE             | PartialFill        | PARTIALLY_FILLED   | apply fill, position, PnL      |
| ACTIVE             | Fill               | FILLED             | apply fill, release risk       |
| ACTIVE             | CancelRequest      | CANCEL_PENDING     | —                              |
| CANCEL_PENDING     | Cancelled          | CANCELLED          | release risk                   |
| CANCEL_PENDING     | CancelReject       | ACTIVE             | count, maybe retry             |
| CANCEL_PENDING     | Fill               | FILLED             | apply fill (cancel lost race)  |
| PARTIALLY_FILLED   | Cancelled          | CANCELLED          | release remaining              |
| UNKNOWN            | ReconFound(state)  | mapped state       | resync                         |
| UNKNOWN            | ReconAbsent        | REJECTED           | release risk                   |
| *any terminal*     | *any*              | *no change*        | count `late_event`             |

---

## 2. Market-data session (owned by `market_data/`)

```
   ┌──────────────┐  connect  ┌────────────┐  ws upgrade  ┌────────────┐
   │ DISCONNECTED │──────────►│ CONNECTING │─────────────►│ SUBSCRIBED │
   └──────▲───────┘           └─────┬──────┘              └─────┬──────┘
          │                         │ fail                      │ first buffered
          │                         ▼                           │ update
          │                  ┌────────────┐              ┌──────▼─────┐
          │                  │ BACKOFF    │              │ SYNCING    │
          │                  └─────┬──────┘              └──────┬─────┘
          │  socket error / no     │ expire                     │ snapshot applied
          │  heartbeat / stale ◄───┘                            │ + gap-free
          │                                                ┌────▼────┐
          └────────────────────────────────────────────────│ SYNCED  │
                                                           └────┬────┘
                                                                │ gap / invalid
                                                          ┌─────▼────┐
                                                          │ RESYNC   │──► SYNCING
                                                          └──────────┘
```

Only `SYNCED` permits quoting. Every other state forces SAFE_MODE for that
symbol. Reconnect uses exponential backoff with jitter (200 ms → 30 s cap) to
avoid synchronized reconnect storms against the venue after a shared outage.

Heartbeat: Binance sends a ping every ~3 min; the client must pong. Independently
the session tracks *data* liveness — a socket that is open but silent for
`max_market_data_age_ms` is treated as dead, because a TCP connection that
survives while data stops is the failure mode that actually loses money.

---

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
