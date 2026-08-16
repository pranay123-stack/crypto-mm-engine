# Binance market data

The first real venue adapter. **Public market data only** — no credentials, no
signing, no account or order endpoint. Order entry is Phase 12 and none of its
code exists yet.

> **Status:** implemented and verified against live public data. See §8 for the
> smoke-test result. This does not make the platform production ready; see the
> Phase 4 report.

---

## 1. Streams and endpoints

| Purpose | Endpoint |
| --- | --- |
| Depth diffs | `wss://stream.binance.com:9443/stream?streams=<sym>@depth@100ms` |
| Trades | `…/<sym>@trade` |
| Book snapshot | `GET https://api.binance.com/api/v3/depth?symbol=…&limit=1000` |
| Instrument rules | `GET …/api/v3/exchangeInfo?symbol=…` |

Combined streams are used so one socket carries every symbol. Adding a symbol
sends a runtime `SUBSCRIBE` frame rather than reconnecting — a reconnect would
resynchronize every *other* symbol on the socket for the sake of the new one.

`@depth@100ms` rather than the 1000 ms default: a market maker quoting at the
touch cannot wait a second to learn the touch moved. `@bookTicker` is declared
in capabilities but not subscribed by default — BBO is derived from the book we
already maintain, and a second source of truth for the touch invites the two to
disagree.

**`exchangeInfo` is always symbol-filtered.** The unfiltered response is
**17.5 MB**; the filtered form is about 5 KB. The live smoke test caught this:
the unfiltered request exceeded the decode limit, failed silently, and the
engine ran with no instrument specs at all — which would have left order
validation in later phases with nothing to validate against. The failure is now
counted (`instrument_failures`) rather than swallowed.

---

## 2. Normalization

`BinanceCodec` is the only place in the repository that knows Binance field
names. It is a pure function of its input — no sockets, no clock, no state
beyond a reusable output buffer — which is what lets every parsing test run
offline against recorded payloads.

| Binance | Normalized |
| --- | --- |
| `"e":"depthUpdate"` | `BookUpdateEvent` |
| `U` / `u` / `pu` | `first_update_id` / `final_update_id` / `prev_final_update_id` |
| `b` / `a` arrays | `BookLevels.bids` / `.asks`, chunked at 16 per event |
| quantity `"0.00000000"` | `PriceLevel::is_deletion()` |
| `"e":"trade"`, `m` | `TradeEvent`, `aggressor` |
| `lastUpdateId` | `BookSnapshotEvent::last_update_id` |
| `PRICE_FILTER` / `LOT_SIZE` / `NOTIONAL` | `InstrumentSpec` tick, lot, min/max qty, min notional |
| `BTCUSDT` / `btcusdt` | one normalized `Symbol`; both spellings stay in the adapter |

Two details that are easy to get backwards and expensive when you do:

- **`m` is "was the *buyer* the maker?"** When true the seller lifted, so the
  aggressor is the **sell** side. Inverting this inverts every order-flow signal
  built on it.
- **Prices and quantities are parsed exactly from their decimal strings.** That
  is why the wire format uses strings at all; routing them through a `double`
  would reintroduce the rounding the fixed-point types exist to eliminate.

### Malformed input

Nothing throws. `nlohmann::json` runs in non-throwing mode and every field is
checked, because a market-data process that dies on an unexpected payload dies
during exactly the venue incident you needed it for.

Failures are classified — `NotJson`, `MissingField`, `InvalidNumber`,
`InvalidSequence`, `LevelParseFailed`, `TooLarge`, `UnknownMessageType` — so
sustained corruption of one kind is visible rather than averaged into "parse
error". Frames above 4 MB are refused without parsing.

A chunked update **emits all its chunks or none**. A partially decoded depth
message would leave a hole in the book that nothing downstream could detect.

Subscription acknowledgements (`{"result":null,"id":1}`) are expected traffic,
not corruption, and decode to zero events.

---

## 3. Synchronization, reconnect, staleness

The algorithm, the continuity rules, and every failure response live in
[order-book.md §3](order-book.md) — they are venue-agnostic and shared with the
mock adapter, which is why they can be tested without a network.

What is Binance-specific here: `U`/`u` carry the range, spot publishes no `pu`
(futures does, and the codec forwards it when present), and the snapshot comes
from REST rather than the stream.

**Reconnect** uses `ReconnectBackoff`: exponential from 200 ms to a 30 s cap,
with deterministic jitter derived from the attempt number and a per-process
seed. Jitter is not decoration — after a shared venue outage every client
reconnects at once, and without it they retry in lockstep and keep the venue
down. Deriving it from a seed rather than a random engine decorrelates clients
while keeping the schedule exactly reproducible in tests.

The backoff resets only on a **completed WebSocket handshake**, so a venue that
accepts TCP and then rejects the upgrade still backs off. Once the attempt
budget is spent the session goes to `Failed`/`Error` rather than retrying
forever, which hides the real fault and risks a ban.

**Staleness** is per symbol, measured on the steady clock against
`max_market_data_age_ms`. A symbol that has never produced data reports
`Nanos::max()`, not zero: a silent symbol must never look healthy.

---

## 4. Threading

```
      ┌──────────────────────────────────────────────┐
      │  md-io thread (one io_context)               │
      │                                              │
      │  WS read ─► codec ─► synchronizer ─► book    │
      │     │                     │                  │
      │     │                     └─► Seqlock<BookView>
      │     └─► sink.on_market_data(event) ──────────┼──► SPSC ring ─► trading
      │  REST snapshot ─► codec ─► synchronizer      │
      │  timer (50 ms): staleness + pending snapshots│
      └──────────────────────────────────────────────┘
```

Everything above happens on one thread, so none of it needs a lock. Public
methods called from elsewhere (`subscribe`, `request_snapshot`, `stop`) post
work onto it rather than touching state.

Cross-thread **reads** — `session_state`, `data_age_ns`, `connection_state`,
`book_view` — are served from per-symbol atomics and a `Seqlock`, never a mutex.
They are polled by the trading thread and by monitoring, and a mutex there would
let an observer stall the feed.

The one lock in the adapter guards the symbol registry, which changes on
subscribe and unsubscribe and never on the per-event path. Symbol slots live in
a fixed array and are published only once fully constructed, so a concurrent
reader cannot see a half-built symbol.

### Why the adapter owns the book

This refines architecture.md's earlier "the trading thread owns order books".
It follows from the Phase 3 interface: `session_state()`, `request_snapshot()`
and `data_age_ns()` are adapter methods, and they only mean anything if the
adapter knows whether the book is synchronized. Building the book beside the
data that produces it also keeps sequence validation, gap detection and resync
in one place. The trading thread receives a *validated* view and never a book
mid-rebuild.

### Timestamps

Every event carries the venue's own time (`E`/`T`, milliseconds, converted to
nanoseconds and **never overwritten with ours**), plus `recv_ns` stamped in the
socket read handler and `parse_ns` stamped after decode. The latter two are
`CLOCK_MONOTONIC` and are the only clocks used for latency arithmetic.

Stamping `recv_ns` at the socket rather than later matters: anything further
downstream folds our own queuing into what is supposed to measure the venue-to-
process path.

---

## 5. Multi-symbol isolation

Each symbol has its own synchronizer, book, sequence state, session state,
staleness clock and published view. A gap on one symbol resyncs that symbol
only; the others keep quoting. `tests/integration/test_feed_to_book.cpp` asserts
this directly, including that staleness is tracked per symbol.

The shared resource is the socket. A disconnect necessarily affects every symbol
on it — which is correct, since none of their books can be maintained while it
is down.

---

## 6. Rate-limit safety

- Snapshot requests are gated by `snapshot_retry_delay_ns` and bounded by
  `max_snapshot_retries`, then `Error`.
- Reconnects are bounded by the backoff budget, then `Failed`.
- `exchangeInfo` is symbol-filtered (§1), cutting request weight and 17.5 MB of
  transfer per call.
- HTTP 429 and 418 are classified as `Unavailable` rather than `Rejected`, so
  the retry budget backs off instead of treating the request as permanently
  refused.
- Every REST request opens a fresh connection with a hard deadline. That costs a
  handshake and buys the guarantee that a half-closed socket cannot silently
  serve a stale snapshot.

---

## 7. Diagnostics

`BinanceMarketData::diagnostics()` exposes frames received, decode errors,
events published, snapshot requests and failures, reconnects, instruments
loaded and instrument failures. Per symbol, `BookSynchronizer::diagnostics()`
adds snapshots applied, updates applied/buffered/discarded (stale vs duplicate,
counted separately), sequence gaps, resyncs, stale transitions, protocol errors
and buffer overflows.

---

## 8. Live public-data smoke test

`./build/mm_md_probe BTCUSDT 15` — connects, synchronizes, prints the book each
second, exits non-zero if it never reached a clean synchronized state. **It
places no orders and sends no credentials**; the adapter it drives contains no
order-entry code.

Result, run against live Binance on 2026-08-17:

```
  session DISCONNECTED   -> CONNECTING      dialling
  session CONNECTING     -> CONNECTED       transport established
  session CONNECTED      -> SUBSCRIBED      subscription confirmed
  session SUBSCRIBED     -> SYNCING         applying snapshot
  session SYNCING        -> READY           synchronized
[ 1s] READY   bid 63061.31 x 6.2803 | ask 63061.32 x 0.9747 | spread 0.01 | age 43ms
...
  frames received  : 777      decode errors : 0
  snapshot reqs    : 1 (0 failed)           reconnects : 0
  snapshots/updates/trades : 63 / 164 / 634
  instruments      : 1 loaded, 0 failed
PASS: book synchronized and stayed clean
```

Verified: WebSocket connects, subscription succeeds, messages arrive, the REST
snapshot succeeds, the documented session path is walked in order without
skipping `Syncing`, the book stays consistent and uncrossed at the venue's 0.01
tick, sequence ids advance monotonically, and nothing fails to decode.

**Not verified against the live venue:** reconnect and resync under a real
disconnect, and behaviour during an actual venue incident. Both are covered by
deterministic tests, and neither can be triggered on demand against production
Binance. Treated as unverified rather than assumed.

---

## 9. Not implemented

No authenticated REST, no signing, no user data stream, no order or account
endpoint, no futures adapter. `@bookTicker` and partial-depth streams are not
subscribed. Historical endpoints are out of scope permanently — this is an
execution platform.
