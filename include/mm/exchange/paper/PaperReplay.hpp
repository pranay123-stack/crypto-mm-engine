#pragma once

/// \file PaperReplay.hpp
/// Record an execution session and replay it exactly (§43).
///
/// A session is three things and nothing else: the configuration, the requests
/// the engine made, and the market updates that arrived. Everything the venue
/// did follows from those deterministically, so replaying them must reproduce
/// the event stream byte for byte.
///
/// The value of this is not the test. It is that when something goes wrong
/// against a live venue, the same three inputs recorded from production can be
/// replayed here until the behaviour is understood -- which is a very different
/// position from reading logs and guessing.

#include <cstdint>
#include <string>
#include <vector>

#include "mm/exchange/common/MarketDataEvents.hpp"
#include "mm/exchange/paper/PaperExecution.hpp"

namespace mm::exchange::paper {

/// One recorded input, stamped with when it happened relative to session start.
struct RecordedAction {
    enum class Kind : std::uint8_t { Submit, Cancel, Replace, MarketUpdate, Poll };

    Kind kind = Kind::Poll;
    /// Nanoseconds since the session began. Relative, so a replay does not
    /// depend on the wall clock of the machine that recorded it.
    Nanos offset_ns = 0;

    OrderRequest submit{};
    CancelRequest cancel{};
    ReplaceRequest replace{};

    /// The book image as of a market update. Carried by value: a replay must
    /// not depend on a book object that no longer exists.
    Symbol symbol{};
    BookLevels levels{};
    Seq update_id = kNoSeq;
};

/// A complete, self-contained session.
struct RecordedSession {
    PaperExecutionConfig config{};
    Symbol symbol{};
    /// The book the session started from.
    BookLevels initial_levels{};
    Seq initial_update_id = kNoSeq;
    std::vector<RecordedAction> actions;

    [[nodiscard]] std::size_t size() const noexcept { return actions.size(); }
};

/// What a replay produced, in a form two runs can be compared by.
struct ReplayResult {
    /// One line per emitted event. Deliberately excludes wall-clock stamps: a
    /// transcript that varied with the clock could never be compared.
    std::vector<std::string> transcript;
    /// The venue's final order state, in the venue's own terms.
    std::vector<PaperOrder> final_orders;
    PaperMetrics metrics{};

    [[nodiscard]] bool matches(const ReplayResult& other) const;
    /// The first line where two results diverge, for a readable failure.
    [[nodiscard]] std::string first_difference(const ReplayResult& other) const;
};

/// Replays a session against a fresh venue and returns what happened.
///
/// Takes no clock: it makes its own, starting from the same instant every time.
/// Sharing a clock with the caller would make the result depend on how much
/// time had passed before the replay started, which is exactly the kind of
/// hidden input that makes a "deterministic" system reproducible only by luck.
[[nodiscard]] ReplayResult replay(const RecordedSession& session);

/// Records a session as it is driven, so the same script can be replayed.
class SessionRecorder {
public:
    SessionRecorder(PaperExecutionConfig config, const Symbol& symbol, BookLevels initial,
                    Seq initial_update_id);

    void record_submit(Nanos offset_ns, const OrderRequest& request);
    void record_cancel(Nanos offset_ns, const CancelRequest& request);
    void record_replace(Nanos offset_ns, const ReplaceRequest& request);
    void record_market(Nanos offset_ns, const Symbol& symbol, const BookLevels& levels,
                       Seq update_id);
    void record_poll(Nanos offset_ns);

    [[nodiscard]] const RecordedSession& session() const noexcept { return session_; }

private:
    RecordedSession session_;
};

}  // namespace mm::exchange::paper
