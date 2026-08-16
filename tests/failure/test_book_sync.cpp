/// \file test_book_sync.cpp
/// Synchronization and failure-path tests for `BookSynchronizer`.
///
/// Every one of these is deterministic and offline: normalized events are fed
/// in directly and time advances only when a `ManualClock` is advanced. There
/// is no socket, no JSON, no sleep and no randomness, so a failure here is
/// always reproducible.

#include <gtest/gtest.h>

#include <vector>

#include "mm/orderbook/BookSynchronizer.hpp"
#include "support/BookFixtures.hpp"

namespace mm::book {
namespace {

using exchange::SessionState;
using test::snapshot;
using test::update;

constexpr const char* kSym = "BTCUSDT";

class SyncTest : public ::testing::Test {
protected:
    SyncTest() : clock(millis(1'000), 0), sync(Symbol(kSym), make_config(), clock) {
        sync.set_state_callback(
            [this](SessionState from, SessionState to, std::string_view reason) {
                transitions.push_back({from, to, std::string(reason)});
            });
    }

    static SyncConfig make_config() {
        SyncConfig c;
        c.max_data_age_ns = millis(500);
        c.snapshot_retry_delay_ns = millis(500);
        c.max_snapshot_retries = 3;
        c.max_buffered_updates = 8;
        c.max_protocol_errors = 3;
        c.max_depth = 20;
        return c;
    }

    /// Drives the session to Subscribed, where updates buffer.
    void bring_up() {
        sync.on_connected();
        sync.on_subscribed();
        ASSERT_EQ(sync.state(), SessionState::Subscribed);
    }

    /// The canonical happy path: buffer one update, snapshot, drain, Ready.
    void synchronize() {
        bring_up();
        ASSERT_TRUE(sync.on_update(update(kSym, 101, 105, {{"100", "5"}}, {{"101", "4"}})).is_ok());
        ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 100, {{"100", "1"}}, {{"101", "1"}})).is_ok());
        ASSERT_EQ(sync.state(), SessionState::Ready);
    }

    struct Transition {
        SessionState from;
        SessionState to;
        std::string reason;
    };

    [[nodiscard]] bool saw(SessionState to) const {
        for (const auto& t : transitions) {
            if (t.to == to) {
                return true;
            }
        }
        return false;
    }

    ManualClock clock;
    BookSynchronizer sync;
    std::vector<Transition> transitions;
};

// ===========================================================================
// The documented procedure
// ===========================================================================

TEST_F(SyncTest, StartsDisconnectedAndUnquotable) {
    EXPECT_EQ(sync.state(), SessionState::Disconnected);
    EXPECT_FALSE(sync.is_quotable());
    EXPECT_EQ(sync.data_age_ns(), std::numeric_limits<Nanos>::max())
        << "a symbol that has never produced data must not look fresh";
}

TEST_F(SyncTest, UpdatesBufferUntilASnapshotArrives) {
    bring_up();
    ASSERT_TRUE(sync.on_update(update(kSym, 101, 102, {{"100", "5"}}, {})).is_ok());
    ASSERT_TRUE(sync.on_update(update(kSym, 103, 104, {{"99", "3"}}, {})).is_ok());
    EXPECT_EQ(sync.buffered_updates(), 2U);
    // Nothing may touch the book before the snapshot establishes where the
    // stream starts.
    EXPECT_FALSE(sync.book().is_valid());
    EXPECT_FALSE(sync.is_quotable());
}

TEST_F(SyncTest, SnapshotDiscardsUpdatesItAlreadyContains) {
    bring_up();
    // These are older than the snapshot and must be dropped, not applied.
    ASSERT_TRUE(sync.on_update(update(kSym, 90, 95, {{"1", "1"}}, {})).is_ok());
    ASSERT_TRUE(sync.on_update(update(kSym, 96, 100, {{"2", "1"}}, {})).is_ok());
    ASSERT_TRUE(sync.on_update(update(kSym, 101, 105, {{"100", "9"}}, {})).is_ok());

    ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 100, {{"100", "5"}}, {{"101", "4"}})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.diagnostics().updates_discarded_stale, 2U);
    // Only the straddling update was applied.
    EXPECT_EQ(sync.book().quantity_at(Side::Buy, Px::from_units(100)).to_string(), "9");
    EXPECT_TRUE(sync.book().quantity_at(Side::Buy, Px::from_units(1)).is_zero());
}

TEST_F(SyncTest, FirstAppliedUpdateMustStraddleTheSnapshot) {
    bring_up();
    // Snapshot is at 100; the stream begins at 110, so five updates are missing
    // and the snapshot is already stale. Applying it would leave a hole.
    ASSERT_TRUE(sync.on_update(update(kSym, 110, 115, {{"100", "9"}}, {})).is_ok());
    const Status s = sync.on_snapshot(snapshot(kSym, 100, {{"100", "5"}}, {{"101", "4"}}));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_EQ(sync.last_failure(), SyncFailure::StaleSnapshot);
    EXPECT_FALSE(sync.is_quotable());
}

TEST_F(SyncTest, SnapshotNewerThanTheWholeBufferSynchronizesCleanly) {
    bring_up();
    ASSERT_TRUE(sync.on_update(update(kSym, 10, 20, {{"1", "1"}}, {})).is_ok());
    // Every buffered update predates the snapshot; the book is simply the image.
    ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 100, {{"100", "5"}}, {{"101", "4"}})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().best_bid_price().to_string(), "100");
}

TEST_F(SyncTest, ReadyAppliesContiguousUpdates) {
    synchronize();
    ASSERT_TRUE(sync.on_update(update(kSym, 106, 110, {{"100", "7"}}, {})).is_ok());
    ASSERT_TRUE(sync.on_update(update(kSym, 111, 115, {{"100", "8"}}, {})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().quantity_at(Side::Buy, Px::from_units(100)).to_string(), "8");
    EXPECT_EQ(sync.book().last_update_id(), 115U);
}

TEST_F(SyncTest, ContinuityMayBeExpressedByPreviousFinalId) {
    // Some venues publish the previous message's final id. Where present it is
    // used, but the message must still be internally consistent: its range
    // begins exactly one past the previous final.
    synchronize();
    ASSERT_TRUE(sync.on_update(update(kSym, 106, 210, {{"100", "7"}}, {}, /*prev=*/105)).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().last_update_id(), 210U);
}

TEST_F(SyncTest, AMessageThatContradictsItselfIsTreatedAsAGap) {
    // prev_final says "I follow 105", but the range starts at 200 -- so updates
    // 106..199 are missing. Trusting prev_final alone would apply this happily
    // and leave a hole in the book that nothing later would detect.
    synchronize();
    const Status s = sync.on_update(update(kSym, 200, 210, {{"100", "7"}}, {}, /*prev=*/105));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_EQ(sync.last_failure(), SyncFailure::SequenceGap);
    EXPECT_FALSE(sync.book().is_valid());
}

TEST_F(SyncTest, PreviousFinalIdMismatchIsAGap) {
    synchronize();
    const Status s = sync.on_update(update(kSym, 200, 210, {{"100", "7"}}, {}, /*prev=*/999));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
}

// ===========================================================================
// Sequence failures
// ===========================================================================

TEST_F(SyncTest, SequenceGapStopsTheBookImmediately) {
    synchronize();
    ASSERT_TRUE(sync.book().is_valid());

    // 106 expected, 120 arrives.
    const Status s = sync.on_update(update(kSym, 120, 125, {{"100", "7"}}, {}));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_EQ(sync.last_failure(), SyncFailure::SequenceGap);
    EXPECT_EQ(sync.diagnostics().sequence_gaps, 1U);
    // The book is discarded rather than left in a plausible-looking state.
    EXPECT_FALSE(sync.book().is_valid());
    EXPECT_FALSE(sync.is_quotable());
}

TEST_F(SyncTest, DuplicateUpdateIsDiscardedNotReapplied) {
    synchronize();
    ASSERT_TRUE(sync.on_update(update(kSym, 106, 110, {{"100", "7"}}, {})).is_ok());
    // A diff is not idempotent, but a replay of an already-applied range is
    // safe to drop because the book already contains it.
    ASSERT_TRUE(sync.on_update(update(kSym, 106, 110, {{"100", "999"}}, {})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.diagnostics().updates_discarded_duplicate, 1U);
    EXPECT_EQ(sync.book().quantity_at(Side::Buy, Px::from_units(100)).to_string(), "7");
}

TEST_F(SyncTest, StaleUpdateBelowTheBookIsDiscarded) {
    synchronize();
    ASSERT_TRUE(sync.on_update(update(kSym, 106, 110, {{"100", "7"}}, {})).is_ok());
    ASSERT_TRUE(sync.on_update(update(kSym, 50, 60, {{"100", "999"}}, {})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().quantity_at(Side::Buy, Px::from_units(100)).to_string(), "7");
}

TEST_F(SyncTest, InvertedUpdateRangeIsAProtocolError) {
    synchronize();
    EXPECT_TRUE(sync.on_update(update(kSym, 200, 100, {{"100", "7"}}, {})).is_error());
    EXPECT_EQ(sync.diagnostics().protocol_errors, 1U);
}

TEST_F(SyncTest, BufferOverflowTriggersResyncRatherThanUnboundedGrowth) {
    bring_up();
    // An unbounded buffer turns a slow snapshot into an out-of-memory kill.
    for (int i = 0; i < 8; ++i) {
        const auto id = static_cast<Seq>(200 + i * 10);
        ASSERT_TRUE(sync.on_update(update(kSym, id, id + 9, {{"100", "1"}}, {})).is_ok());
    }
    EXPECT_EQ(sync.buffered_updates(), 8U);
    const Status s = sync.on_update(update(kSym, 400, 410, {{"100", "1"}}, {}));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.last_failure(), SyncFailure::BufferOverflow);
    EXPECT_EQ(sync.diagnostics().buffer_overflows, 1U);
    // Still Subscribed: there is no book to resync yet, so the correct response
    // is to drop the buffer and ask for a snapshot again, not to invent a
    // transition the session machine does not have.
    EXPECT_EQ(sync.state(), SessionState::Subscribed);
    EXPECT_EQ(sync.buffered_updates(), 0U);
    EXPECT_FALSE(sync.is_quotable());
    EXPECT_TRUE(sync.wants_snapshot());
}

TEST_F(SyncTest, CrossedSnapshotForcesResync) {
    bring_up();
    const Status s = sync.on_snapshot(snapshot(kSym, 100, {{"102", "5"}}, {{"101", "4"}}));
    EXPECT_TRUE(s.is_error());
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_FALSE(sync.is_quotable());
}

// ===========================================================================
// §26 — the required failure sequences, end to end
// ===========================================================================

TEST_F(SyncTest, SequenceGapToSnapshotToReady) {
    // GAP -> RESYNC -> SNAPSHOT -> VALIDATION -> READY
    synchronize();
    ASSERT_TRUE(sync.on_update(update(kSym, 500, 505, {{"100", "7"}}, {})).is_error());
    ASSERT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_TRUE(sync.wants_snapshot());

    sync.note_snapshot_requested();
    EXPECT_FALSE(sync.wants_snapshot()) << "a request in flight must not be re-issued";

    ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 600, {{"100", "5"}}, {{"101", "4"}})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_TRUE(sync.is_quotable());
    EXPECT_TRUE(sync.book().is_valid());
    EXPECT_EQ(sync.book().last_update_id(), 600U);
    EXPECT_EQ(sync.diagnostics().resyncs, 1U);
}

TEST_F(SyncTest, DisconnectToStaleToReconnectToResyncToReady) {
    // DISCONNECT -> RECONNECT -> RESYNC -> READY
    synchronize();
    ASSERT_TRUE(sync.book().is_valid());

    sync.on_disconnected("socket closed");
    EXPECT_EQ(sync.state(), SessionState::Disconnected);
    // While the socket was down the venue kept trading: what we hold is not
    // stale, it is wrong.
    EXPECT_FALSE(sync.book().is_valid());
    EXPECT_FALSE(sync.is_quotable());

    // A reconnect is an attempt first; only a failed attempt waits. Going
    // straight from Disconnected to Backoff is illegal by design.
    sync.on_connecting();
    EXPECT_EQ(sync.state(), SessionState::Connecting);
    sync.on_backoff("connect attempt failed");
    EXPECT_EQ(sync.state(), SessionState::Backoff);

    sync.on_connected();
    sync.on_subscribed();
    EXPECT_EQ(sync.state(), SessionState::Subscribed);
    EXPECT_FALSE(sync.is_quotable());

    ASSERT_TRUE(sync.on_update(update(kSym, 701, 705, {{"100", "5"}}, {{"101", "4"}})).is_ok());
    ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 700, {{"100", "1"}}, {{"101", "1"}})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_TRUE(sync.is_quotable());

    EXPECT_TRUE(saw(SessionState::Disconnected));
    EXPECT_TRUE(saw(SessionState::Backoff));
    EXPECT_TRUE(saw(SessionState::Ready));
}

TEST_F(SyncTest, MalformedMessagesReachASafeStateOnceTheBudgetIsExhausted) {
    // MALFORMED -> DIAGNOSTIC -> SAFE STATE IF THRESHOLD EXCEEDED
    synchronize();
    sync.on_protocol_error("bad json");
    EXPECT_EQ(sync.state(), SessionState::Ready) << "isolated corruption is survivable";
    sync.on_protocol_error("bad json");
    EXPECT_EQ(sync.state(), SessionState::Ready);
    sync.on_protocol_error("bad json");
    // Sustained corruption means we are no longer parsing what the venue sends.
    EXPECT_EQ(sync.state(), SessionState::Error);
    EXPECT_EQ(sync.last_failure(), SyncFailure::ProtocolErrorBudget);
    EXPECT_FALSE(sync.is_quotable());
    EXPECT_EQ(sync.diagnostics().protocol_errors, 3U);
}

// ===========================================================================
// Staleness
// ===========================================================================

TEST_F(SyncTest, DataGoingSilentMakesTheSessionStale) {
    synchronize();
    EXPECT_TRUE(sync.is_quotable());

    clock.advance(millis(400));
    sync.on_timer();
    EXPECT_EQ(sync.state(), SessionState::Ready) << "still within tolerance";

    clock.advance(millis(200));  // 600ms total, limit is 500ms
    sync.on_timer();
    EXPECT_EQ(sync.state(), SessionState::Stale);
    EXPECT_FALSE(sync.is_quotable())
        << "a strategy must not be able to quote against stale data";
    EXPECT_EQ(sync.diagnostics().stale_transitions, 1U);
}

TEST_F(SyncTest, ResumedDataRecoversFromStale) {
    synchronize();
    clock.advance(millis(600));
    sync.on_timer();
    ASSERT_EQ(sync.state(), SessionState::Stale);

    ASSERT_TRUE(sync.on_update(update(kSym, 106, 110, {{"100", "7"}}, {})).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_TRUE(sync.is_quotable());
}

TEST_F(SyncTest, DataStaleBeyondRecoveryForcesResync) {
    synchronize();
    clock.advance(millis(600));
    sync.on_timer();
    ASSERT_EQ(sync.state(), SessionState::Stale);

    // Long enough that the book has certainly drifted; rebuilding beats
    // resuming from an image of unknown age.
    clock.advance(seconds(10));
    sync.on_timer();
    EXPECT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_FALSE(sync.book().is_valid());
}

TEST_F(SyncTest, DataAgeTracksTheSteadyClock) {
    synchronize();
    EXPECT_EQ(sync.data_age_ns(), 0);
    clock.advance(millis(250));
    EXPECT_EQ(sync.data_age_ns(), millis(250));
}

// ===========================================================================
// Snapshot rate limiting
// ===========================================================================

TEST_F(SyncTest, SnapshotRequestsAreRateLimited) {
    bring_up();
    ASSERT_TRUE(sync.wants_snapshot());
    sync.note_snapshot_requested();
    sync.note_snapshot_failed("transport error");

    // Immediately afterwards a retry must be refused: repeated failures must
    // not become a REST request storm against the venue.
    EXPECT_FALSE(sync.wants_snapshot());
    clock.advance(millis(499));
    EXPECT_FALSE(sync.wants_snapshot());
    clock.advance(millis(2));
    EXPECT_TRUE(sync.wants_snapshot());
}

TEST_F(SyncTest, SnapshotRetriesAreBounded) {
    bring_up();
    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_TRUE(sync.wants_snapshot()) << "attempt " << attempt;
        sync.note_snapshot_requested();
        sync.note_snapshot_failed("transport error");
        clock.advance(millis(600));
    }
    // Retrying forever in an unrecoverable state gets a client banned and hides
    // the real fault.
    EXPECT_EQ(sync.state(), SessionState::Error);
    EXPECT_EQ(sync.last_failure(), SyncFailure::SnapshotRetryBudget);
    EXPECT_FALSE(sync.wants_snapshot());
}

TEST_F(SyncTest, SuccessfulSnapshotResetsTheRetryBudget) {
    bring_up();
    sync.note_snapshot_requested();
    sync.note_snapshot_failed("transport error");
    clock.advance(millis(600));

    ASSERT_TRUE(sync.on_snapshot(snapshot(kSym, 100, {{"100", "5"}}, {{"101", "4"}})).is_ok());
    ASSERT_EQ(sync.state(), SessionState::Ready);

    // A later resync starts with a full budget rather than an exhausted one.
    ASSERT_TRUE(sync.on_update(update(kSym, 900, 905, {{"100", "1"}}, {})).is_error());
    ASSERT_EQ(sync.state(), SessionState::ResyncRequired);
    EXPECT_TRUE(sync.wants_snapshot());
}

TEST_F(SyncTest, NoSnapshotIsWantedWhileReady) {
    synchronize();
    EXPECT_FALSE(sync.wants_snapshot());
}

// ===========================================================================
// State machine integrity
// ===========================================================================

TEST_F(SyncTest, ReadyIsNeverReachedWithoutPassingThroughSyncing) {
    synchronize();
    bool syncing_before_ready = false;
    for (const auto& t : transitions) {
        if (t.to == SessionState::Ready) {
            EXPECT_EQ(t.from, SessionState::Syncing)
                << "Ready must only be entered from Syncing";
            syncing_before_ready = true;
        }
    }
    EXPECT_TRUE(syncing_before_ready);
}

TEST_F(SyncTest, ErrorIsTerminalUntilTheSessionIsTornDown) {
    synchronize();
    sync.on_fatal("unrecoverable");
    ASSERT_EQ(sync.state(), SessionState::Error);

    // No amount of good data recovers an errored session on its own.
    sync.on_connected();
    EXPECT_EQ(sync.state(), SessionState::Error);
    static_cast<void>(sync.on_update(update(kSym, 106, 110, {{"100", "7"}}, {})));
    EXPECT_EQ(sync.state(), SessionState::Error);
    EXPECT_FALSE(sync.is_quotable());

    sync.on_disconnected("operator teardown");
    EXPECT_EQ(sync.state(), SessionState::Disconnected);
}

TEST_F(SyncTest, UpdatesForTheWrongSymbolAreRejected) {
    synchronize();
    EXPECT_TRUE(sync.on_update(update("ETHUSDT", 106, 110, {{"100", "7"}}, {})).is_error());
    EXPECT_TRUE(sync.on_snapshot(snapshot("ETHUSDT", 200, {{"100", "1"}}, {})).is_error());
}

TEST_F(SyncTest, SnapshotArrivingWhileReadyIsIgnored) {
    synchronize();
    const Seq before = sync.book().last_update_id();
    // A late reply to a superseded request must not rewind the book.
    EXPECT_TRUE(sync.on_snapshot(snapshot(kSym, 50, {{"1", "1"}}, {})).is_error());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().last_update_id(), before);
}

TEST_F(SyncTest, ChunkedSnapshotOnlyBecomesReadyOnTheLastChunk) {
    bring_up();
    auto first = snapshot(kSym, 100, {{"100", "5"}}, {});
    first.is_last = false;
    ASSERT_TRUE(sync.on_snapshot(first).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Syncing);
    EXPECT_FALSE(sync.is_quotable());

    auto last = snapshot(kSym, 100, {{"99", "3"}}, {{"101", "4"}});
    last.is_first = false;
    last.is_last = true;
    ASSERT_TRUE(sync.on_snapshot(last).is_ok());
    EXPECT_EQ(sync.state(), SessionState::Ready);
    EXPECT_EQ(sync.book().depth(Side::Buy), 2U);
}

TEST(SyncFailureNames, EveryValueHasAName) {
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(SyncFailure::SnapshotRetryBudget);
         ++i) {
        EXPECT_NE(to_string(static_cast<SyncFailure>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::book
