# Exchange interface

The boundary between the engine and any trading venue. Everything above it is
venue-agnostic; everything venue-specific lives below it, inside an adapter.

> **Status:** interfaces, normalized types, and a deterministic mock adapter are
> implemented and tested. **No real venue adapter exists yet.** Binance arrives
> in Phases 4 and 12.

---

## 1. Shape

```
                         CORE ENGINE
                              │
              ┌───────────────┴───────────────┐
              ▼                               ▼
     IExchangeMarketData              IExchangeExecution
     IMarketDataSink                  IExecutionSink
              │                               │
      ┌───────┼───────┐               ┌───────┼────────┐
      ▼       ▼       ▼               ▼       ▼        ▼
   Mock   (Binance) (OKX)          Mock  (Paper)  (Binance)
```

Four types make up the contract:

| Type | Direction | Implemented by |
| --- | --- | --- |
| `IExchangeMarketData` | engine → venue | the adapter |
| `IMarketDataSink` | venue → engine | the engine |
| `IExchangeExecution` | engine → venue | the adapter |
| `IExecutionSink` | venue → engine | the engine |

Splitting each direction into its own interface is what lets the engine own the
threading policy. The adapter calls the sink on its own I/O thread; the sink's
only job is to stamp the event and push it into an `SpscRing`. It never touches
the book, strategy, risk, OMS or portfolio — those belong to the trading thread
([concurrency.md §1](concurrency.md)).

---

## 2. Types reused, not redefined

Phase 3 adds nothing that Phase 2 already provides. The vocabulary an adapter
speaks is mostly `common/`:

| Concept | Type | Defined in |
| --- | --- | --- |
| Symbol, order ids, trade ids, assets | `Symbol`, `ClientOrderId`, `ExchangeOrderId`, `TradeId`, `Asset` | `common/Types.hpp` |
| Price, quantity, notional | `Px`, `Qty`, `Notional` (tagged fixed-point, 1e-8) | `common/Fixed.hpp` |
| Side, order type, TIF, liquidity | `Side`, `OrderType`, `TimeInForce`, `Liquidity` | `common/Types.hpp` |
| Rejection reason, market status | `RejectReason`, `MarketStatus` | `common/Types.hpp` |
| Timing envelope, BBO, instrument rules | `EventStamps`, `BestBidAsk`, `InstrumentSpec` | `common/Types.hpp` |
| Error propagation | `Status`, `Result<T>` | `common/Status.hpp` |

No monetary or quantity value is ever a floating-point number. `Px`, `Qty` and
`Notional` do not implicitly convert to one another, so a price cannot be
assigned to a quantity — the most common unit bug in trading code, made
impossible rather than discouraged.

### Added by Phase 3

`OrderStatus`, `CancelStatus`, `ExecutionType`, `ConnectionState`,
`SessionState`, `RequestOutcome`, `ExchangeErrorCategory`, `ExchangeError`,
`ExchangeCapabilities`, `Feature`, `OrderRequest`/`CancelRequest`/
`ReplaceRequest`, and the event structs in `MarketDataEvents.hpp` /
`ExecutionEvents.hpp`.

### `OrderStatus` is not the OMS lifecycle

`OrderStatus` is **what the venue says**: `New`, `PartiallyFilled`, `Filled`,
`Canceled`, `Rejected`, `Expired`, `PendingCancel`, `PendingReplace`, `Unknown`.

The OMS lifecycle in [state-machines.md §1](state-machines.md) — `CREATED`,
`SUBMITTING`, `CANCEL_PENDING`, `UNKNOWN` and the rest — is **what we know**. No
venue can report `SUBMITTING`, because that describes our side of the wire.

Keeping them separate matters: merging them would put OMS state under the
adapter's control, and the OMS must own order truth. The adapter reports facts;
the OMS decides what they mean.

---

## 3. Unknown-state semantics

The single most consequential design decision in this layer.

> A request that fails is not the same as a request the venue refused.

```
    submit(order)
         │
         ├── venue said yes ────────────► OrderAckEvent        → Acknowledged
         │
         ├── venue said no ─────────────► OrderRejectEvent     → Rejected
         │                                                       order does NOT exist
         │
         ├── venue said nothing ────────► RequestFailureEvent  → Unknown
         │   (timeout, socket died,                              order MIGHT exist
         │    unparseable response)                              → must reconcile
         │
         └── never left this process ───► Status error         → NotSent
             (validation, not connected)                         order does NOT exist
```

`RequestOutcome` carries this on every failure:

| Outcome | Meaning | OMS response |
| --- | --- | --- |
| `Acknowledged` | The venue confirmed it. | Order exists. |
| `Rejected` | The venue refused it. | Discard; no reconciliation. |
| `Unknown` | We cannot tell. | **Treat as possibly live. Reconcile.** |
| `NotSent` | Never left this process. | Discard; no reconciliation. |

`NotSent` versus `Unknown` matters as much as `Rejected` versus `Unknown`: the
first is safe to drop, the second is not.

### Enforcement, not convention

`is_consistent(category, outcome)` rejects pairings that cannot physically
occur. The safety-critical rows:

| Category | May be `Rejected`? | Legal outcomes |
| --- | --- | --- |
| `Timeout` | **No** | `Unknown` only — and never `NotSent`: a timeout means it *was* sent |
| `Transport` | **No** | `Unknown`, `NotSent` |
| `ConnectionFailure` | **No** | `Unknown`, `NotSent` |
| `UnknownOrderState` | **No** | `Unknown` |
| `MalformedResponse` | **No** | `Unknown` |
| `ExchangeRejection` | Yes | `Rejected` only |
| `UnsupportedOperation` | No | `NotSent` only |
| `Authentication`, `RateLimit`, `InvalidRequest` | Yes | any of the three — the venue may or may not have spoken |

`ExchangeError::make` *corrects* an impossible pairing rather than propagating
it. The correction target is the **safest** legal outcome, not the category's
typical one:

```cpp
// An adapter bug claiming a dropped connection means "rejected".
auto e = ExchangeError::make(ExchangeErrorCategory::ConnectionFailure,
                             RequestOutcome::Rejected, "adapter bug");
assert(e.outcome == RequestOutcome::Unknown);   // not NotSent, the typical default
```

`ConnectionFailure`'s typical outcome is `NotSent` ("forget the order"), but a
confused adapter earns `Unknown` ("go and check"). Forgetting a live order is
unrecoverable; a needless reconciliation is not. This asymmetry was found by a
test, not by design — see the Phase 3 report.

`validate_execution_event` closes the other side: an `OrderRejectEvent` that
does not carry `Rejected` is refused with a message telling the adapter author
to emit a `RequestFailureEvent` instead.

---

## 4. Error model

Ten categories, deliberately not collapsed into one:

`Transport` · `Authentication` · `InvalidRequest` · `RateLimit` ·
`ExchangeRejection` · `Timeout` · `ConnectionFailure` · `UnknownOrderState` ·
`UnsupportedOperation` · `MalformedResponse`

`ExchangeError` carries the category, the outcome, a normalized `RejectReason`,
a `retryable` flag, a short detail string, and `venue_code`.

**`venue_code` is for logs only.** It holds the venue's raw numeric code
verbatim, because an incident investigation needs it. Nothing above the adapter
may branch on it — that would be a venue detail leaking into the core, and
`RejectReason` exists precisely so the core can react to
`InsufficientBalance` without knowing that a particular venue calls it `-2010`.

---

## 5. Capabilities

Venues differ. The core is written against neither the union nor the
intersection of their features; a venue declares what it can do and the layers
above adapt.

```cpp
struct ExchangeCapabilities {
    bool supports_post_only, supports_replace, supports_reduce_only;
    bool supports_client_order_id, supports_mass_cancel, supports_order_query;
    bool supports_native_bbo, supports_orderbook_snapshot;
    bool supports_incremental_depth, supports_trade_stream;
    bool supports_balance_stream, supports_position_stream, has_positions;
    std::uint32_t max_client_order_id_len;
    std::uint32_t max_subscriptions_per_connection;
    std::uint32_t max_snapshot_depth;
};
```

Every flag defaults to **false**. An undeclared capability reads as absent, so
an adapter author cannot silently inherit a feature they have not implemented.

Two consequences worth stating:

- `validate_order` refuses an order carrying an unsupported flag **locally**,
  as `UnsupportedOperation` / `NotSent`. A rejected order costs a round trip and
  a rate-limit slot, and a burst of rejects is indistinguishable from a venue
  outage to the safety layer.
- Where `supports_replace` is false, `replace()` returns an error rather than
  silently decomposing into cancel-then-submit. The two have different exposure
  profiles — during a decomposed replace, exposure is the *union* of both orders
  — and that decision belongs to the quote manager, not to an adapter quietly
  doing something else than what was asked.

---

## 6. Events

All events are trivially copyable, fixed-size POD so they cross the
io-thread → trading-thread boundary through an `SpscRing` with no allocation and
no pointer whose lifetime spans two threads.

| Market data | Execution |
| --- | --- |
| `BookSnapshotEvent` | `OrderAckEvent` |
| `BookUpdateEvent` | `OrderRejectEvent` |
| `TradeEvent` | `OrderCancelEvent` |
| `BboEvent` | `OrderReplaceEvent` |
| `SessionStateEvent` | `FillEvent` |
| `ConnectionEvent` | `BalanceUpdateEvent` |
| `InstrumentUpdateEvent` | `PositionUpdateEvent` |
| | `ConnectionEvent` |
| | **`RequestFailureEvent`** |
| | `OpenOrdersSnapshotEvent` |
| | `OrderStatusReport` |

Each family travels in one tagged envelope — `MarketDataEvent` and
`ExecutionEvent` — with a `type` discriminator and a union payload.

### Why one envelope per family

A trade and the depth update it caused must be applied in the order the venue
produced them. Splitting them across rings would leave the trading thread to
re-interleave two streams by sequence number: more machinery, and a new way to
get the book wrong. One ordered ring makes ordering a property of the transport.

The cost is that every event occupies the largest member's footprint. Measured:
a 60-byte trade and a 16-level depth update both cost ~22 ns through a ring
(`bench_exchange.cpp`), because both copy the full 624-byte envelope. That is
the price of ordering, paid deliberately.

### Chunking

`kMaxLevelsPerEvent = 16` and `kMaxOrdersPerSnapshotEvent = 4` bound the ring
slot. Larger payloads are **chunked**, never truncated:

- Book snapshots and oversized depth diffs carry `is_first` / `is_last`. The
  local book is only replaced once `is_last` arrives, so a partially delivered
  snapshot cannot be mistaken for a complete one.
- Open-order listings carry the same markers. Reconciliation may only conclude
  "the venue does not have this order" after a chunk marked `is_last` — acting
  on a partial listing would cancel-or-forget live orders.

`kMaxOrdersPerSnapshotEvent` was reduced from 8 to 4 after measurement: at 8 the
`ExecutionEvent` envelope was 1432 bytes, so every 224-byte fill paid a 6× copy
tax for a payload that only appears during reconciliation. Dropping the
redundant `error` field from `OpenOrdersSnapshotEvent` (failures already travel
as `RequestFailureEvent`) brought it to 680 bytes.

### Fills are idempotent by construction

`FillEvent::trade_id` is mandatory — `validate_execution_event` rejects a fill
without one. It is the idempotency key the OMS uses to discard redelivered
reports, and venues *do* redeliver after a private-stream reconnect. A
double-counted fill corrupts position and PnL simultaneously.

---

## 7. Session state

One state machine, shared by market data and execution sessions. It supersedes
the earlier sketch in [state-machines.md §2](state-machines.md), which that
document now reflects.

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

`is_quotable(state)` returns true for **`Ready` only**. It is the single
predicate that gates quoting, so no caller can invent its own notion of "close
enough".

Illegal transitions are rejected by `is_legal_transition`, not merely
discouraged. The ones that matter:

- **`Subscribed → Ready` is illegal.** A subscription says nothing about whether
  a snapshot has been applied; allowing it would let a strategy quote against an
  empty book.
- **`ResyncRequired → Ready` is illegal.** Recovery must pass through `Syncing`.
- **`Error → anything but Disconnected` is illegal**, including `Backoff`.
  Otherwise `Error → Backoff → Connecting → … → Ready` would let an
  unrecoverable session quietly retry its way back to trading.
- **`Disconnected → Backoff` is illegal.** From there the next step is an
  attempt, not a wait.

`Stale` is the state that earns its place: a socket that is open while data has
stopped is the failure mode that quietly loses money, because the book looks
fine and is minutes old. `data_age_ns()` is how the safety layer detects it, and
a symbol that has never produced data reports `Nanos::max()` rather than zero —
a silent symbol must not look healthy.

---

## 8. Dependency boundaries

```
   common/                    (no exchange knowledge)
      ▲
      │
   exchange/common/           interfaces + normalized types
      ▲                       links mm_common and nothing else
      │
      ├── exchange/mock/      deterministic test adapter
      └── exchange/binance/   (Phase 4/12 — does not exist yet)
```

Two rules, both enforced mechanically by
`tools/check_exchange_boundary.py`, wired into `ctest` as `exchange_boundary`:

1. **No venue identifier in core code.** Venue names and wire-protocol tokens
   (`listenKey`, `newClientOrderId`, `recvWindow`, `X-MBX`, …) may appear only
   inside an adapter directory. Comments are stripped before scanning: a doc
   comment reading "this venue caps client order ids at 36 chars" is useful
   context, not a leak.
2. **No core file includes an adapter header**, so the core compiles with every
   adapter deleted from the tree.

This check already caught one real leak: `ExchangeConfig::name` defaulted to a
specific venue. The default is gone — a venue is now an explicit choice that
`validate()` requires, which is also the right behaviour independently.

---

## 9. Mock adapter

`MockExchangeMarketData` and `MockExchangeExecution` implement both interfaces
completely. **This is not the paper-trading engine** — it has no market model, no
queue simulation and no fee schedule. It exists to prove the abstraction is
implementable and to let later phases drive the OMS through venue behaviours
that a real venue produces only rarely and never on demand.

**Determinism is the point.** No threads, no wall clock, no randomness. Time
advances only when a test advances a `ManualClock`; events are delivered only
when the test calls `pump()`, ordered by due time and then by insertion. Open
orders are sorted before chunking, because `unordered_map` iteration order is
not stable across runs and a test that depends on it is a flaky test.

Scriptable behaviours:

| Submit | Cancel |
| --- | --- |
| `Ack` | `Accept` |
| `Reject` | `Reject` — *order stays live* |
| `Timeout` → `Unknown` | `NotFound` |
| `TransportLoss` → `Unknown` | `TooLate` — filled during the cancel |
| `AckThenFullFill` | `Timeout` → `Unknown` |
| `AckThenPartialFill` | |

Plus `disconnect()` / `reconnect()` (in-flight requests become `Unknown`, never
rejected), `redeliver_last_event()` for byte-identical replay, and
`deliver_fill()` which **refuses** a quantity exceeding what remains — a test
must not be able to script a sequence no venue could produce.

The mock also enforces the session machine on itself: driving it from
`Subscribed` straight to `Ready` fails, exactly as it would for a real adapter.

---

## 10. Designing the future Binance adapter

Nothing below is implemented. It is recorded so Phase 4 starts from a boundary
already thought through.

| Binance concept | Where it lives | Normalized as |
| --- | --- | --- |
| `wss://.../btcusdt@depth@100ms` | adapter subscription logic | `SubscriptionRequest` |
| `depthUpdate` `U`/`u`/`pu` fields | adapter decode | `BookUpdateEvent` sequence fields |
| `GET /api/v3/depth?limit=5000` | adapter REST | `BookSnapshotEvent` (chunked) |
| `executionReport` payload | adapter decode | `OrderAckEvent` / `FillEvent` / `OrderCancelEvent` |
| `listenKey` lifecycle + keepalive | adapter, entirely | `ConnectionEvent` |
| HMAC-SHA256 signing, `recvWindow` | adapter, entirely | invisible above |
| `-2010`, `-1021`, `-2011` … | adapter error map | `RejectReason` + `venue_code` |
| `LIMIT_MAKER`, `GTX` | adapter order encoding | `OrderType` / `TimeInForce` |
| `newClientOrderId` (36-char cap) | adapter | `capabilities().max_client_order_id_len` |
| No atomic replace on spot | adapter | `supports_replace = false` |

Two things Phase 4 must get right, both already expressible here:

- **A REST timeout on `POST /order` is `Timeout` / `Unknown`.** Never
  `ExchangeRejection`. The order may be resting on the venue.
- **The documented depth-sync procedure** ([state-machines.md §3](state-machines.md))
  maps onto `Subscribed → Syncing → Ready`, with a failed continuity check
  driving `ResyncRequired`.

---

## 11. What this layer deliberately does not do

- No venue is implemented. The seams exist; the code does not, because unused
  adapters rot.
- No paper-trading engine — the mock is not it. Paper execution is Phase 9.
- No OMS. This layer reports facts; deciding what they mean is Phase 8.
- No retry policy. Whether to resend is a risk and OMS decision; the adapter
  only reports `retryable` as a hint.
