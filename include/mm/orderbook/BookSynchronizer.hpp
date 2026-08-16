#pragma once

/// \file BookSynchronizer.hpp
/// Snapshot/delta synchronization for one symbol.
///
/// This is the component that decides whether a book may be quoted against. It
/// owns the per-symbol `SessionState` from `Subscribed` onward and refuses to
/// reach `Ready` until a snapshot has been applied and every buffered update
/// has been proven continuous with it.
///
/// **It is venue-agnostic.** The normalized `BookUpdateEvent` already carries
/// `first_update_id`, `final_update_id` and `prev_final_update_id`, so the
/// continuity rules below are expressed over those fields and never over a
/// venue's own names for them. That is what makes every synchronization test in
/// this repository deterministic and offline: the tests drive normalized events
/// directly, with no socket and no JSON.
///
/// **No wall clock.** Staleness and retry backoff are measured against an
/// injected `Clock`, so tests advance a `ManualClock` instead of sleeping.

#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>

#include "mm/common/Time.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"
#include "mm/orderbook/OrderBook.hpp"

namespace mm::book {

using exchange::BookSnapshotEvent;
using exchange::BookUpdateEvent;
using exchange::SessionState;

/// Why a synchronization attempt failed, for diagnostics and for the journal.
enum class SyncFailure : std::uint8_t {
    None = 0,
    SequenceGap,          ///< an update id did not continue from the last applied
    StaleSnapshot,        ///< the stream had already moved past the snapshot
    BufferOverflow,       ///< too many updates arrived before the snapshot did
    CrossedBook,
    InvalidUpdate,        ///< malformed or structurally impossible
    DataStale,            ///< socket alive, data too old
    Disconnected,
    ProtocolErrorBudget,  ///< too many malformed messages
    SnapshotRetryBudget,  ///< snapshot kept failing
};
[[nodiscard]] std::string_view to_string(SyncFailure f) noexcept;

struct SyncConfig {
    /// Data older than this makes the session `Stale`.
    Nanos max_data_age_ns = millis(500);
    /// Minimum gap between snapshot requests, so a resync loop cannot become a
    /// REST request storm against the venue.
    Nanos snapshot_retry_delay_ns = millis(500);
    /// Consecutive snapshot attempts before giving up and entering `Error`.
    /// Retrying forever in an unrecoverable state is how a client gets banned.
    std::int32_t max_snapshot_retries = 5;
    /// Updates buffered while waiting for a snapshot. Bounded: an unbounded
    /// buffer converts a slow snapshot into an out-of-memory kill.
    std::size_t max_buffered_updates = 256;
    /// Malformed messages tolerated before the session is declared broken.
    std::int32_t max_protocol_errors = 20;
    std::size_t max_depth = OrderBook::kDefaultMaxDepth;
};

struct SyncDiagnostics {
    std::uint64_t snapshots_applied = 0;
    std::uint64_t updates_applied = 0;
    std::uint64_t updates_buffered = 0;
    std::uint64_t updates_discarded_stale = 0;   ///< older than the snapshot
    std::uint64_t updates_discarded_duplicate = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t stale_transitions = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t snapshot_requests = 0;
    std::uint64_t buffer_overflows = 0;
};

class BookSynchronizer {
public:
    /// Invoked on every accepted state change, on the caller's thread. Used by
    /// the adapter to emit `SessionStateEvent`. State changes are rare, so the
    /// indirection costs nothing that matters.
    using StateCallback =
        std::function<void(SessionState from, SessionState to, std::string_view reason)>;

    BookSynchronizer(Symbol symbol, SyncConfig config, const Clock& clock);

    void set_state_callback(StateCallback callback) { on_state_ = std::move(callback); }

    // ------------------------------------------------------- session lifecycle

    /// A connection attempt has begun. Separate from `on_connected` so a failed
    /// attempt can reach `Backoff` legally: `Disconnected -> Backoff` is illegal
    /// by design, because from there the next step is an attempt, not a wait.
    void on_connecting();
    /// Transport came up. Drives Disconnected/Backoff -> Connecting -> Connected.
    void on_connected();
    /// Venue confirmed the subscription. Updates now buffer until a snapshot.
    void on_subscribed();
    /// Transport went away. The book is discarded: a book that stopped being
    /// maintained is not a stale book, it is a wrong one.
    void on_disconnected(std::string_view reason);
    /// Waiting before the next connection attempt.
    void on_backoff(std::string_view reason);
    /// Unrecoverable. Only an operator clears it.
    void on_fatal(std::string_view reason);

    // ------------------------------------------------------------ data events

    [[nodiscard]] Status on_snapshot(const BookSnapshotEvent& snapshot);
    [[nodiscard]] Status on_update(const BookUpdateEvent& update);

    /// A message that could not be decoded. Counted; enough of them declare the
    /// session broken rather than letting corruption continue indefinitely.
    void on_protocol_error(std::string_view detail);

    /// Freshness check plus snapshot-retry timing. Call periodically.
    void on_timer();

    // ---------------------------------------------------------------- queries

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] bool is_quotable() const noexcept { return exchange::is_quotable(state_); }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] Symbol symbol() const noexcept { return symbol_; }

    /// True when the adapter should fetch a snapshot now. Already rate-limited
    /// by `snapshot_retry_delay_ns`, so the caller may poll it freely.
    [[nodiscard]] bool wants_snapshot() const noexcept;
    /// Records that a snapshot request was issued, starting the retry clock.
    void note_snapshot_requested();
    /// A snapshot request failed at the transport level.
    void note_snapshot_failed(std::string_view reason);

    /// Steady-clock age of the most recent data event, or `Nanos::max()` when
    /// nothing has ever arrived -- a silent symbol must never look healthy.
    [[nodiscard]] Nanos data_age_ns() const noexcept;

    [[nodiscard]] const SyncDiagnostics& diagnostics() const noexcept { return diag_; }
    [[nodiscard]] SyncFailure last_failure() const noexcept { return last_failure_; }
    [[nodiscard]] std::size_t buffered_updates() const noexcept { return buffer_.size(); }

private:
    void transition(SessionState to, std::string_view reason);
    void begin_resync(SyncFailure failure, std::string_view reason);
    /// Applies a buffered or live update; assumes continuity has been checked.
    [[nodiscard]] Status apply_update_to_book(const BookUpdateEvent& update);
    /// Continuity check for an update arriving in the Ready state.
    [[nodiscard]] bool continues_from(const BookUpdateEvent& update, Seq last_applied) const noexcept;
    void touch_data_clock();

    Symbol symbol_{};
    SyncConfig config_{};
    const Clock& clock_;

    SessionState state_ = SessionState::Disconnected;
    OrderBook book_;
    std::deque<BookUpdateEvent> buffer_;

    Nanos last_data_ns_ = 0;
    bool has_data_ = false;
    Nanos last_snapshot_request_ns_ = 0;
    bool snapshot_requested_ = false;
    std::int32_t snapshot_attempts_ = 0;
    std::int32_t protocol_errors_ = 0;

    SyncFailure last_failure_ = SyncFailure::None;
    SyncDiagnostics diag_{};
    StateCallback on_state_;
};

}  // namespace mm::book
