#include "mm/orderbook/BookSynchronizer.hpp"

#include <limits>

namespace mm::book {

std::string_view to_string(SyncFailure f) noexcept {
    switch (f) {
        case SyncFailure::None:                return "NONE";
        case SyncFailure::SequenceGap:         return "SEQUENCE_GAP";
        case SyncFailure::StaleSnapshot:       return "STALE_SNAPSHOT";
        case SyncFailure::BufferOverflow:      return "BUFFER_OVERFLOW";
        case SyncFailure::CrossedBook:         return "CROSSED_BOOK";
        case SyncFailure::InvalidUpdate:       return "INVALID_UPDATE";
        case SyncFailure::DataStale:           return "DATA_STALE";
        case SyncFailure::Disconnected:        return "DISCONNECTED";
        case SyncFailure::ProtocolErrorBudget: return "PROTOCOL_ERROR_BUDGET";
        case SyncFailure::SnapshotRetryBudget: return "SNAPSHOT_RETRY_BUDGET";
    }
    return "UNKNOWN";
}

BookSynchronizer::BookSynchronizer(Symbol symbol, SyncConfig config, const Clock& clock)
    : symbol_(symbol), config_(config), clock_(clock), book_(symbol, config.max_depth) {}

void BookSynchronizer::transition(SessionState to, std::string_view reason) {
    const SessionState from = state_;
    if (from == to) {
        return;
    }
    // The single session machine from docs/state-machines.md. An adapter bug
    // that tries to skip Syncing would otherwise present an unsynchronized book
    // as quotable.
    if (!exchange::is_legal_transition(from, to)) {
        state_ = SessionState::Error;
        if (on_state_) {
            on_state_(from, SessionState::Error, "illegal session transition attempted");
        }
        return;
    }
    state_ = to;
    if (on_state_) {
        on_state_(from, to, reason);
    }
}

void BookSynchronizer::touch_data_clock() {
    last_data_ns_ = clock_.steady();
    has_data_ = true;
}

Nanos BookSynchronizer::data_age_ns() const noexcept {
    if (!has_data_) {
        return std::numeric_limits<Nanos>::max();
    }
    return clock_.steady() - last_data_ns_;
}

// ---------------------------------------------------------------- lifecycle

void BookSynchronizer::on_connecting() {
    transition(SessionState::Connecting, "dialling");
}

void BookSynchronizer::on_connected() {
    if (state_ == SessionState::Disconnected || state_ == SessionState::Backoff) {
        transition(SessionState::Connecting, "dialling");
    }
    transition(SessionState::Connected, "transport established");
}

void BookSynchronizer::on_subscribed() {
    transition(SessionState::Subscribed, "subscription confirmed");
    // Buffering begins now. Nothing may be applied to the book until a snapshot
    // establishes where the stream starts.
    buffer_.clear();
    snapshot_requested_ = false;
    snapshot_attempts_ = 0;
}

void BookSynchronizer::on_disconnected(std::string_view reason) {
    last_failure_ = SyncFailure::Disconnected;
    // Discard the book. While the socket was down the venue kept trading, so
    // what we hold is not stale -- it is wrong, and the difference matters.
    book_.clear();
    buffer_.clear();
    has_data_ = false;
    snapshot_requested_ = false;
    transition(SessionState::Disconnected, reason);
}

void BookSynchronizer::on_backoff(std::string_view reason) {
    transition(SessionState::Backoff, reason);
}

void BookSynchronizer::on_fatal(std::string_view reason) {
    book_.clear();
    buffer_.clear();
    transition(SessionState::Error, reason);
}

void BookSynchronizer::begin_resync(SyncFailure failure, std::string_view reason) {
    last_failure_ = failure;
    // Stop trusting the book immediately: this is the moment at which
    // continuing to apply updates would corrupt it silently.
    book_.clear();
    buffer_.clear();
    snapshot_requested_ = false;
    snapshot_attempts_ = 0;

    // From Subscribed there is no book yet and a snapshot is already pending,
    // so ResyncRequired would state nothing new -- and it is not reachable from
    // Subscribed in the documented machine. Clearing the buffer and re-arming
    // the snapshot request is the whole of the correct response.
    if (state_ == SessionState::Subscribed) {
        return;
    }
    ++diag_.resyncs;
    transition(SessionState::ResyncRequired, reason);
}

// ------------------------------------------------------------- data events

bool BookSynchronizer::continues_from(const BookUpdateEvent& update,
                                      Seq last_applied) const noexcept {
    // Venues express continuity in one of two ways. Where a previous-final id
    // is published it is authoritative -- but it is not taken on its own.
    //
    // A message carrying both fields must agree with itself: an update whose
    // range begins at 500 while claiming to follow update 100 has a hole in it,
    // whatever its previous-final id says. Trusting `prev_final` alone made
    // exactly that message look continuous, and the book silently lost every
    // update in between. A message that contradicts itself is treated as a gap,
    // because we cannot tell which of its two claims is the wrong one.
    if (update.prev_final_update_id != kNoSeq) {
        const bool follows_last = (update.prev_final_update_id == last_applied);
        const bool internally_consistent =
            (update.first_update_id == kNoSeq) ||
            (update.first_update_id == update.prev_final_update_id + 1);
        return follows_last && internally_consistent;
    }
    return update.first_update_id == last_applied + 1;
}

Status BookSynchronizer::apply_update_to_book(const BookUpdateEvent& update) {
    // Only the terminating chunk advances the book's update id, so a logical
    // update split across chunks is applied whole or not at all.
    const Seq advance_to = update.is_last ? update.final_update_id : kNoSeq;
    const Status s = book_.apply_update(update.levels, advance_to);
    if (s.is_error()) {
        return s;
    }
    if (update.is_last) {
        ++diag_.updates_applied;
    }
    return Status::ok();
}

Status BookSynchronizer::on_snapshot(const BookSnapshotEvent& snapshot) {
    if (snapshot.symbol != symbol_) {
        return {ErrorCode::InvalidArgument, "snapshot for the wrong symbol"};
    }
    if (state_ != SessionState::Subscribed && state_ != SessionState::Syncing &&
        state_ != SessionState::ResyncRequired) {
        // A snapshot arriving while Ready is a late reply to a superseded
        // request; ignoring it is correct, applying it would rewind the book.
        return {ErrorCode::FailedPrecondition, "snapshot arrived outside a sync window"};
    }

    touch_data_clock();

    if (snapshot.is_first) {
        if (state_ != SessionState::Syncing) {
            transition(SessionState::Syncing, "applying snapshot");
        }
        book_.begin_snapshot();
    }
    MM_RETURN_IF_ERROR(book_.add_snapshot_levels(snapshot.levels));
    if (!snapshot.is_last) {
        // A partially delivered snapshot is not a snapshot. Wait for the rest.
        return Status::ok();
    }

    const Status finished = book_.finish_snapshot(snapshot.last_update_id);
    if (finished.is_error()) {
        begin_resync(SyncFailure::CrossedBook, "snapshot failed validation");
        return finished;
    }
    ++diag_.snapshots_applied;
    snapshot_requested_ = false;

    // ---- reconcile the buffered stream against the image -----------------
    //
    // The documented procedure, in full. Simplifying it to "take the snapshot
    // then start applying" produces a book that is quietly wrong whenever the
    // snapshot and the stream do not abut.
    const Seq snapshot_id = snapshot.last_update_id;

    // 1. Discard everything the snapshot already contains.
    while (!buffer_.empty() && buffer_.front().final_update_id <= snapshot_id) {
        buffer_.pop_front();
        ++diag_.updates_discarded_stale;
    }

    // 2. The first surviving update must span snapshot_id + 1. If it begins
    //    after that, the stream moved past the snapshot while it was in flight
    //    and a newer snapshot is required.
    if (!buffer_.empty()) {
        const BookUpdateEvent& first = buffer_.front();
        const bool spans_snapshot = first.first_update_id <= snapshot_id + 1 &&
                                    snapshot_id + 1 <= first.final_update_id;
        if (!spans_snapshot) {
            ++diag_.sequence_gaps;
            begin_resync(SyncFailure::StaleSnapshot,
                         "buffered stream does not abut the snapshot");
            return {ErrorCode::FailedPrecondition, "snapshot and stream do not abut"};
        }
    }

    // 3. Apply the buffer in order, checking continuity from the second update
    //    onward. The first is exempt: it legitimately straddles the snapshot id.
    bool first_applied = false;
    while (!buffer_.empty()) {
        const BookUpdateEvent update = buffer_.front();
        buffer_.pop_front();

        if (first_applied && !continues_from(update, book_.last_update_id())) {
            ++diag_.sequence_gaps;
            begin_resync(SyncFailure::SequenceGap, "gap while draining the buffer");
            return {ErrorCode::FailedPrecondition, "sequence gap in buffered updates"};
        }
        const Status applied = apply_update_to_book(update);
        if (applied.is_error()) {
            begin_resync(SyncFailure::InvalidUpdate, "buffered update failed to apply");
            return applied;
        }
        if (update.is_last) {
            first_applied = true;
        }
    }

    // 4. Only now is the book trustworthy.
    const BookViolation violation = book_.check_invariants();
    if (violation != BookViolation::None) {
        begin_resync(SyncFailure::CrossedBook, to_string(violation));
        return {ErrorCode::Internal, "book failed invariants after synchronization"};
    }

    last_failure_ = SyncFailure::None;
    snapshot_attempts_ = 0;
    transition(SessionState::Ready, "synchronized");
    return Status::ok();
}

Status BookSynchronizer::on_update(const BookUpdateEvent& update) {
    if (update.symbol != symbol_) {
        return {ErrorCode::InvalidArgument, "update for the wrong symbol"};
    }
    if (!update.levels.counts_are_sane()) {
        on_protocol_error("update level count exceeds capacity");
        return {ErrorCode::OutOfRange, "malformed update"};
    }
    if (update.final_update_id != kNoSeq && update.first_update_id > update.final_update_id) {
        on_protocol_error("update id range is inverted");
        return {ErrorCode::InvalidArgument, "malformed update id range"};
    }

    touch_data_clock();

    switch (state_) {
        case SessionState::Subscribed:
        case SessionState::Syncing: {
            // Buffer until the snapshot arrives.
            if (buffer_.size() >= config_.max_buffered_updates) {
                ++diag_.buffer_overflows;
                begin_resync(SyncFailure::BufferOverflow,
                             "snapshot did not arrive before the buffer filled");
                return {ErrorCode::Overflow, "update buffer overflow"};
            }
            buffer_.push_back(update);
            ++diag_.updates_buffered;
            return Status::ok();
        }

        case SessionState::Ready:
        case SessionState::Stale: {
            const Seq last_applied = book_.last_update_id();

            // Already contained in the book: a duplicate or a replay. Discard
            // rather than reapply -- reapplying is not idempotent for a diff.
            if (update.final_update_id != kNoSeq && update.final_update_id <= last_applied) {
                ++diag_.updates_discarded_duplicate;
                return Status::ok();
            }

            if (!continues_from(update, last_applied)) {
                ++diag_.sequence_gaps;
                begin_resync(SyncFailure::SequenceGap, "sequence gap in the live stream");
                return {ErrorCode::FailedPrecondition, "sequence gap"};
            }

            const Status applied = apply_update_to_book(update);
            if (applied.is_error()) {
                begin_resync(SyncFailure::InvalidUpdate, "update failed to apply");
                return applied;
            }
            // Data resumed, so a stale session becomes quotable again.
            if (state_ == SessionState::Stale) {
                transition(SessionState::Ready, "data resumed");
            }
            return Status::ok();
        }

        case SessionState::ResyncRequired:
        case SessionState::Disconnected:
        case SessionState::Connecting:
        case SessionState::Connected:
        case SessionState::Authenticated:
        case SessionState::Backoff:
        case SessionState::Error:
            // Not synchronized: the update is noted for freshness but must not
            // touch a book that is being rebuilt or does not exist.
            return Status::ok();
    }
    return Status::ok();
}

void BookSynchronizer::on_protocol_error(std::string_view detail) {
    ++diag_.protocol_errors;
    ++protocol_errors_;
    // Isolated corruption happens. Sustained corruption means we are no longer
    // parsing what the venue is sending, and continuing would be guesswork.
    if (protocol_errors_ >= config_.max_protocol_errors) {
        last_failure_ = SyncFailure::ProtocolErrorBudget;
        on_fatal(detail.empty() ? "protocol error budget exhausted" : detail);
    }
}

void BookSynchronizer::on_timer() {
    if (state_ == SessionState::Ready) {
        if (data_age_ns() > config_.max_data_age_ns) {
            // The socket is open and the data has stopped. This is the failure
            // that loses money quietly, because the book still looks fine.
            ++diag_.stale_transitions;
            last_failure_ = SyncFailure::DataStale;
            transition(SessionState::Stale, "market data age exceeded the limit");
        }
        return;
    }

    if (state_ == SessionState::Stale) {
        // Data that has stopped for long enough is not merely stale; the book
        // has almost certainly drifted and must be rebuilt.
        if (data_age_ns() > config_.max_data_age_ns * 10) {
            begin_resync(SyncFailure::DataStale, "data stale beyond recovery");
        }
    }
}

// ------------------------------------------------------------- snapshots

bool BookSynchronizer::wants_snapshot() const noexcept {
    if (state_ != SessionState::Subscribed && state_ != SessionState::ResyncRequired) {
        return false;
    }
    if (snapshot_requested_) {
        return false;
    }
    if (snapshot_attempts_ == 0) {
        return true;
    }
    // Rate-limited so a persistent failure cannot become a REST request storm.
    return (clock_.steady() - last_snapshot_request_ns_) >= config_.snapshot_retry_delay_ns;
}

void BookSynchronizer::note_snapshot_requested() {
    snapshot_requested_ = true;
    last_snapshot_request_ns_ = clock_.steady();
    ++snapshot_attempts_;
    ++diag_.snapshot_requests;
}

void BookSynchronizer::note_snapshot_failed(std::string_view reason) {
    snapshot_requested_ = false;
    if (snapshot_attempts_ >= config_.max_snapshot_retries) {
        // Never retry forever in an unrecoverable state: that is how a client
        // gets rate-limited or banned, and it hides the real fault.
        last_failure_ = SyncFailure::SnapshotRetryBudget;
        on_fatal(reason.empty() ? "snapshot retry budget exhausted" : reason);
    }
}

}  // namespace mm::book
