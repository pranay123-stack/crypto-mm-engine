#pragma once

/// \file MarketDataEvents.hpp
/// Normalized inbound market-data events.
///
/// Every event is trivially copyable and fixed size so it can cross the
/// io-thread -> trading-thread boundary through an `SpscRing` without
/// allocating and without a pointer whose lifetime spans two threads.
///
/// **Why one envelope rather than one ring per event kind.** A trade and the
/// depth update it caused must be applied in the order the venue produced them.
/// Splitting them across rings would leave the trading thread to re-interleave
/// two streams by sequence number -- more machinery, and a new way to get the
/// book wrong. A single ordered ring makes ordering a property of the transport
/// instead of a problem to solve.

#include <array>
#include <cstdint>

#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeError.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::exchange {

/// Levels carried by one event. Depth diffs are small in practice; anything
/// larger -- and every snapshot -- is delivered as an ordered chunk sequence
/// rather than truncated. A book that silently loses levels is worse than no
/// book, so there is no path here that drops one.
inline constexpr std::size_t kMaxLevelsPerEvent = 16;

struct PriceLevel {
    Px price{};
    /// Zero means "this level is gone". Venues express deletion as a
    /// zero-quantity update, and the local book erases rather than storing it.
    Qty quantity{};

    [[nodiscard]] constexpr bool is_deletion() const noexcept { return quantity.is_zero(); }
};

static_assert(std::is_trivially_copyable_v<PriceLevel>);

/// One side-pair of levels. `bid_count`/`ask_count` bound the valid prefix.
struct BookLevels {
    std::array<PriceLevel, kMaxLevelsPerEvent> bids{};
    std::array<PriceLevel, kMaxLevelsPerEvent> asks{};
    std::uint8_t bid_count = 0;
    std::uint8_t ask_count = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return bid_count == 0 && ask_count == 0;
    }
    /// Guards against a malformed adapter writing a count past the array.
    [[nodiscard]] constexpr bool counts_are_sane() const noexcept {
        return bid_count <= kMaxLevelsPerEvent && ask_count <= kMaxLevelsPerEvent;
    }
};

static_assert(std::is_trivially_copyable_v<BookLevels>);

/// A full book image, used to seed or rebuild the local book.
struct BookSnapshotEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// The venue sequence this image is current as of. Buffered updates at or
    /// below this are discarded; the first applied update must continue from it.
    Seq last_update_id = kNoSeq;

    BookLevels levels{};

    /// Snapshots exceeding `kMaxLevelsPerEvent` arrive as an ordered chunk run.
    /// The book is only replaced once `is_last` arrives, so a partially
    /// delivered snapshot can never be mistaken for a complete one.
    bool is_first = true;
    bool is_last = true;
};

/// An incremental depth diff.
struct BookUpdateEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// Venue update range for this message: [first_update_id, final_update_id].
    Seq first_update_id = kNoSeq;
    Seq final_update_id = kNoSeq;
    /// The previous message's final id where the venue publishes it. Lets the
    /// consumer detect a gap directly instead of inferring one.
    Seq prev_final_update_id = kNoSeq;

    BookLevels levels{};

    /// False on all but the last chunk of an oversized diff.
    bool is_last = true;
};

struct TradeEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    Px price{};
    Qty quantity{};
    /// The side of the *aggressor*. This is the sign of order flow, and getting
    /// it backwards inverts every flow-based signal built on it.
    Side aggressor = Side::Buy;

    TradeId trade_id{};
    Seq seq = kNoSeq;
};

/// Best bid/offer. Reuses the existing `BestBidAsk`, including its `is_sane()`
/// predicate -- the market-data layer refuses to publish a crossed or locked
/// top of book rather than passing it to a strategy.
struct BboEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    BestBidAsk bbo{};
    /// True when the venue publishes BBO natively; false when it was derived
    /// from depth. The distinction matters because a derived BBO is only as
    /// fresh as the last depth update.
    bool is_native = false;
};

/// A session state change for one symbol's data stream. Carries both endpoints
/// so an illegal transition is visible in the journal rather than inferred.
struct SessionStateEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    SessionState from = SessionState::Disconnected;
    SessionState to = SessionState::Disconnected;
    ExchangeError error{};
    InlineString<64> reason{};
};

/// A transport-level change, not tied to a symbol.
struct ConnectionEvent {
    VenueName venue{};
    ConnectionState from = ConnectionState::Disconnected;
    ConnectionState to = ConnectionState::Disconnected;
    ExchangeError error{};
    /// Reconnect attempts since the last successful connect; feeds the safety
    /// layer's "venue is flapping" detector.
    std::uint32_t reconnect_count = 0;
};

/// Venue trading rules for an instrument, loaded at startup and re-published
/// when the venue changes them. Tick and lot sizes do change, and an order
/// validated against a stale spec is an order the venue will reject.
struct InstrumentUpdateEvent {
    InstrumentSpec spec{};
};

enum class MarketDataEventType : std::uint8_t {
    None = 0,
    BookSnapshot,
    BookUpdate,
    Trade,
    Bbo,
    SessionState,
    Connection,
    InstrumentUpdate,
};
[[nodiscard]] std::string_view to_string(MarketDataEventType t) noexcept;

/// The ring-transported envelope.
///
/// The union is safe here because every member is trivially copyable and the
/// tag decides which member is read; a byte-wise copy of a trivially copyable
/// type reproduces its value by definition.
struct MarketDataEvent {
    MarketDataEventType type = MarketDataEventType::None;
    EventStamps stamps{};

    union Payload {
        Payload() noexcept : book_snapshot{} {}
        BookSnapshotEvent book_snapshot;
        BookUpdateEvent book_update;
        TradeEvent trade;
        BboEvent bbo;
        SessionStateEvent session;
        ConnectionEvent connection;
        InstrumentUpdateEvent instrument;
    } payload;

    /// The symbol this event concerns, or an empty symbol for venue-wide events.
    [[nodiscard]] Symbol symbol() const noexcept;
    /// The venue sequence, or `kNoSeq` where the event carries none.
    [[nodiscard]] Seq sequence() const noexcept;
};

static_assert(std::is_trivially_copyable_v<MarketDataEvent>,
              "market data events cross a ring by value and must copy trivially");

}  // namespace mm::exchange
