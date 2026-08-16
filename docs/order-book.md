# Local order book

The book, its invariants, and the synchronization that decides whether it may be
quoted against.

Both components are **venue-agnostic**. They consume the normalized events from
[exchange-interface.md](exchange-interface.md) and know nothing about Binance,
which is why every synchronization test in this repository runs offline against
hand-written events rather than against a live exchange.

---

## 1. Data structure, and what the measurement actually said

Two sorted `std::vector<PriceLevel>` per book — bids descending, asks ascending
— so best bid and best ask are index 0 of their side.

The tempting rationale is that at a few dozen levels a contiguous vector must
beat a red-black tree. **The measurement disagrees.** Head to head at 50 levels
with identical pre-generated inputs and identical surrounding work:

| Structure | ns / level | ns / 16-level message |
| --- | --- | --- |
| sorted `std::vector` (this book) | ~54–65 | ~865–1050 |
| `std::map` baseline | ~18–33 | ~290–535 |

`std::map` is roughly two to three times faster on mean throughput, repeatably.
Hoisting the side comparison to a template parameter — removing an unpredictable
branch from every binary-search step — did not close the gap, so it is
structural: the vector pays a memmove per insert and erase where the tree
relinks pointers.

**The vector is kept anyway**, for reasons throughput does not capture:

- **No allocation.** `std::map` calls the allocator for every new price level,
  on the thread decoding market data. That is a `malloc` with an unbounded tail
  on a latency-sensitive path, and for a market maker the tail is what costs
  money — not the mean. The vectors are `reserve`d once at construction and
  never allocate again.
- **Bounded, predictable footprint**, fixed at startup rather than growing with
  venue behaviour.
- **Contiguous top-N access**, which is what consumers actually read.

The absolute difference is roughly 35 ns per depth message against a network
path measured in tens of microseconds. If profiling ever shows book updates
mattering, the candidates are a pooled-node map or a flat structure with a
separately maintained top-N — not a plain `std::map`, which would trade a
measured non-problem for an allocation problem.

This is recorded rather than quietly corrected because the first version of this
file asserted the opposite, and a design rationale that contradicts its own
benchmark is worse than no rationale.

### Truncation

The book tracks at most `max_depth` levels per side. An update for a price worse
than the worst tracked level, while that side is full, is dropped — it cannot
reach the top of book. The consequences are precise:

- **Best bid/ask and the top of book are always exact.**
- After deletions near the touch a side may hold fewer than `max_depth` levels,
  because the levels that would have been promoted were never tracked. Depth is
  a lower bound; it is never wrong at the top.
- A resync restores full depth from the snapshot.

---

## 2. Invariants

Checked on every mutation, and available as a full audit via
`check_invariants()` (~116 ns at 100 levels):

```
price > 0                       quantity >= 0
no zero-quantity level stored   no duplicate price levels
bids strictly descending        asks strictly ascending
best bid < best ask             depth <= max_depth
update ids never regress
```

A violation sets `BookViolation` and makes `is_valid()` false. There is no path
that continues with a "best effort" book: a crossed book promises a strategy
free money that does not exist, and a zero level left in place makes every depth
and cumulative-quantity read silently wrong.

Snapshot application is **atomic from a consumer's perspective**. Chunks
accumulate in a staging area and the visible book is swapped in only on the
final chunk, so no reader can observe bids from a new image against asks from
the old one.

---

## 3. Synchronization

`BookSynchronizer` owns the per-symbol `SessionState` from `Subscribed` onward
and is the only thing that can declare a book quotable.

### The procedure

Simplifying this to "fetch a snapshot, then start applying updates" produces a
book that is quietly wrong whenever the two do not abut — which is most of the
time on a busy symbol.

```
 1. Subscribe.                          state := Subscribed
 2. Buffer every update. Apply nothing.
 3. Fetch a snapshot with id S.         state := Syncing
 4. Discard buffered updates whose final id <= S.
 5. The first surviving update must span S+1:
        first_id <= S+1 <= final_id
    If it begins after S+1, the stream moved past the snapshot while it was in
    flight. Fetch a newer one -- do not paper over the hole.
 6. Apply the buffer in order, checking continuity from the second update on.
    The first is exempt: it legitimately straddles S.
 7. Audit invariants.
 8. Only now:                           state := Ready
```

### Continuity

Two forms, because venues differ:

- **Previous-final id published** (`pu` on some venues): it must equal the last
  applied id, **and** the message must agree with itself — its range must begin
  exactly one past that id.
- **Not published**: the range must begin exactly one past the last applied id.

The self-consistency half was added after a test caught the weaker rule
accepting a message that claimed to follow update 100 while its range began at
500. `prev_final` said "continuous", `first_update_id` said "there is a hole",
and trusting the former alone lost 399 updates with nothing downstream able to
notice. A message that contradicts itself is treated as a gap, because there is
no way to tell which of its two claims is the wrong one.

### Failure responses

| Condition | Response |
| --- | --- |
| Sequence gap | discard the book, `ResyncRequired` |
| Snapshot does not abut the stream | discard, request a newer snapshot |
| Duplicate / already-applied update | discard, counted — a diff is not idempotent |
| Stale update below the book | discard, counted |
| Update buffer full before the snapshot arrives | drop the buffer, re-request |
| Crossed or locked book | discard, `ResyncRequired` |
| Data older than `max_data_age_ms` | `Stale` — not quotable |
| Data stale ten times over | `ResyncRequired` — the book has drifted |
| Malformed messages past the budget | `Error` — operator action required |
| Snapshot retries past the budget | `Error` |
| Disconnect | **discard the book** |

That last one is deliberate. While the socket was down the venue kept trading,
so what we hold is not stale — it is wrong, and the difference decides whether
it is safe to resume from.

### Rate limiting

Snapshot requests are gated by `snapshot_retry_delay_ns` and bounded by
`max_snapshot_retries`. A resync loop must not become a REST request storm: that
is how a client gets rate-limited or banned, and it buries the original fault.
The engine may poll `wants_snapshot()` every tick without thinking about it.

---

## 4. Threading and publication

The book is built and mutated on the feed thread only — see
[binance-market-data.md](binance-market-data.md) §4. Consumers never touch it
directly.

Instead each symbol publishes a `BookView` (top 10 levels, BBO, session state,
update id) through a `Seqlock`. Readers get a torn-free snapshot and can never
observe bids from one generation against asks from another; the writer is never
blocked by a reader, which is the same property that keeps the dashboard out of
the hot path (docs/concurrency.md §4).

**Levels are published only when the session is `Ready`.** A view of a book
mid-rebuild would look usable and would not be.

---

## 5. Testing

All deterministic and offline — no sockets, no wall clock, no randomness that
is not seeded.

- **Exact state after update sequences**: insert, replace, delete-by-zero-
  quantity, both sides, touch improvement, sorted insertion position.
- **Rejections**: crossed and locked snapshots, duplicate levels, negative
  quantity, non-positive price, sequence regression, update before any snapshot.
- **Truncation**: depth capping, eviction of the worst level, dropping updates
  beyond tracked depth without disturbing the touch.
- **Property sweep**: 20,000 randomized updates from a fixed seed, asserting the
  full invariant set after every single one.
- **Synchronization**: the whole procedure above, plus every failure response,
  plus the §26 sequences end to end.
