# The execution boundary

> The OMS must be able to execute an order without knowing whether it is going
> to a simulator or to a real venue.

That is the whole purpose of this layer. Everything below is in service of it.

```
   OMS  ──►  IExchangeExecution  ──►  PaperExecution      (Phase 9, implemented)
                                 ──►  BinanceExecution    (Phase 12, not written)
                                 ──►  MockExchangeExecution (test double)
```

The OMS contains no `if (paper)`, no `if (live)`, no `if (binance)`. The
implementation decides the environment, and a test asserts the OMS is not even
*constructible* with a mode flag. `tools/check_exchange_boundary.py` enforces
the same rule mechanically.

---

## 1. The interface

`exchange::IExchangeExecution` (Phase 3, unchanged by Phase 9 — which is the
point) offers:

| Group | Operations |
| --- | --- |
| lifecycle | `start(sink)`, `stop()` |
| order entry | `submit`, `cancel`, `replace`, `cancel_all` |
| queries | `query_open_orders`, `query_order`, `query_balances`, `query_positions` |
| status | `connection_state`, `is_authenticated`, `in_flight_requests`, `capabilities`, `venue` |

Every type crossing it is normalized. **No venue type appears anywhere in the
interface or in anything it references** — no JSON, no REST, no WebSocket, no
venue error codes. That is enforced, not merely intended.

### What a return value means

`submit()` returning **ok** means only *handed off*. It does not mean accepted,
and it certainly does not mean resting. `submit()` returning an **error** means
the request was **not sent** and no order can exist.

Everything else arrives asynchronously.

---

## 2. The request model

| Field | Owner | Notes |
| --- | --- | --- |
| `client_order_id` | OMS | our identity; the venue echoes it |
| `symbol`, `symbol_id` | engine | normalized |
| `side`, `type`, `tif` | quote manager | `type` is LIMIT in this phase (§13) |
| `price`, `quantity` | strategy, via the quote manager | already validated |
| `post_only` | quote manager | honoured explicitly, never silently dropped |
| `reduce_only` | quote manager | only where capabilities allow |
| `trace` | engine | correlates a request with its answers |
| `created_ns` | engine | monotonic, for latency accounting |

`CancelRequest` carries both identities where known, because a venue may accept
either and the one we have depends on how far the order got. `ReplaceRequest`
carries the original identity plus the new price, quantity and client id.

No field exists that the current architecture cannot justify. Deferred
deliberately: iceberg quantity, self-trade-prevention mode, order groups —
none are used by anything, and unused fields in a request struct get filled in
wrongly by whoever needs them first.

---

## 3. The event model

Reused from Phase 3 unchanged. Nothing new was invented, because a second event
vocabulary that means almost the same thing is how two parts of a system come
to disagree.

| Concept (§4) | Type |
| --- | --- |
| ConnectionState | `ConnectionEvent` |
| OrderAccepted | `OrderAckEvent` |
| OrderRejected | `OrderRejectEvent` |
| OrderCancelled / OrderCancelRejected | `OrderCancelEvent` + `CancelStatus` |
| OrderReplaced / OrderReplaceRejected | `OrderReplaceEvent` + `accepted` |
| OrderPartiallyFilled / OrderFilled | `FillEvent` + `OrderStatus` |
| OrderExpired | `OrderCancelEvent` with `OrderStatus::Expired` |
| ExecutionError | `RequestFailureEvent` |
| OpenOrdersSnapshot / ReconciliationEvent | `OpenOrdersSnapshotEvent` |

Every event carries an engine sequence, a venue transaction time, receive and
process stamps, and the identities it refers to. **Ordering is by sequence, not
by wall clock** — see docs/concurrency.md.

---

## 4. Asynchrony is not optional

```
   OMS ──submit()──► adapter        (returns immediately)
                        │
                        │ simulated or real latency
                        ▼
   OMS ◄──on_execution()── adapter  (later, on the I/O thread)
```

The adapter never calls the sink from inside `submit()`. A test asserts this
directly, because an adapter that answered synchronously would let the engine
pass tests it would fail against any real venue — every race the OMS exists to
survive would be invisible.

---

## 5. Concurrency

```
  execution thread            SPSC ring            OMS thread
  ────────────────            ─────────            ──────────
  adapter emits event  ──►  OMS enqueues  ──►  process_events()  ──►  state
```

Unchanged from Phase 8. The adapter's callback runs wherever the adapter's I/O
runs and does exactly one thing: enqueue. All order-state mutation happens on
the OMS thread. Nothing is shared, so nothing is locked.

The paper adapter's `poll()` is called by whoever owns the execution thread —
in tests, by the test, which is what makes time controllable and the whole
thing deterministic.

---

## 6. Selecting an environment

```yaml
execution_env:
  mode: paper     # the only implemented environment
```

`mode: live` at the top level implies live execution; the two can never
disagree in the dangerous direction. Selecting live **fails at startup** with a
message naming the missing adapter (`EngineConfig::validate_execution_available`).

It does not fall back to paper. A run somebody believed was live must never
quietly not be, and the reverse mistake — a paper config reaching a real venue —
is the one that loses money.

---

## 7. What is not here

No live venue adapter, no credentials, no network. `tools/check_exchange_boundary.py`
rejects any include of a socket, TLS, HTTP, WebSocket or JSON header from the
OMS or from paper execution, and the rules are verified by injecting violations
and confirming the checker exits non-zero.
