#pragma once

/// \file Types.hpp
/// The shared vocabulary of the engine: identifiers, enumerations and the
/// timing envelope every event carries. Nothing here knows about a venue.

#include <cstdint>
#include <string_view>

#include "mm/common/Fixed.hpp"
#include "mm/common/InlineString.hpp"
#include "mm/common/Time.hpp"

namespace mm {

// ---------------------------------------------------------------- identifiers

using Symbol = InlineString<24>;           ///< normalized, e.g. "BTCUSDT"
using VenueName = InlineString<16>;        ///< "binance", "paper"
using ClientOrderId = InlineString<40>;    ///< ours; Binance caps at 36 chars
using ExchangeOrderId = InlineString<32>;  ///< theirs; numeric or string
using TradeId = InlineString<32>;          ///< venue fill identifier
using Asset = InlineString<12>;            ///< "BTC", "USDT"
using StrategyName = InlineString<32>;

/// Dense per-process handle for a symbol. Hot-path containers key on this
/// rather than on the string, so lookups are array indexing.
enum class SymbolId : std::uint16_t { kInvalid = 0xFFFF };

/// Dense handle for an order inside the OMS store.
using OrderHandle = std::uint32_t;
constexpr OrderHandle kInvalidOrderHandle = 0xFFFFFFFFU;

/// Correlates one strategy decision with every artifact it produces, from the
/// quote action through to the fill and the PnL delta. See architecture.md §8.
using TraceId = std::uint64_t;
constexpr TraceId kNoTrace = 0;

/// Venue update sequence. Monotonic within a stream; gaps are errors.
using Seq = std::uint64_t;
constexpr Seq kNoSeq = 0;

// ----------------------------------------------------------------- enumerations

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}
/// +1 for a buy, -1 for a sell. Used to fold side into signed position math.
[[nodiscard]] constexpr std::int64_t sign_of(Side s) noexcept {
    return s == Side::Buy ? 1 : -1;
}
[[nodiscard]] std::string_view to_string(Side s) noexcept;

enum class OrderType : std::uint8_t {
    Limit = 0,
    Market,
    LimitMaker,  ///< post-only: rejected rather than crossing. The market
                 ///< maker's default -- taking is a strategy decision, never
                 ///< an accident of a moving book.
};
[[nodiscard]] std::string_view to_string(OrderType t) noexcept;

enum class TimeInForce : std::uint8_t {
    GTC = 0,
    IOC,
    FOK,
    GTX,  ///< good-till-crossing (post-only)
};
[[nodiscard]] std::string_view to_string(TimeInForce t) noexcept;

/// Whether a fill added or removed liquidity. Drives the fee schedule, and the
/// maker ratio is a primary health metric for a market maker.
enum class Liquidity : std::uint8_t { Unknown = 0, Maker, Taker };
[[nodiscard]] std::string_view to_string(Liquidity l) noexcept;

/// Normalized rejection taxonomy. Adapters map venue errors onto this so the
/// core can react without knowing that Binance says -2010.
enum class RejectReason : std::uint8_t {
    None = 0,
    Unknown,
    InsufficientBalance,
    PriceOutOfBounds,       ///< outside the venue's percent-price band
    QuantityOutOfBounds,    ///< below min qty / above max qty
    NotionalTooSmall,
    TickSizeViolation,
    LotSizeViolation,
    PostOnlyWouldCross,     ///< LimitMaker that would have taken
    DuplicateClientOrderId,
    UnknownOrder,           ///< cancel/replace for an order the venue lost
    RateLimited,
    MarketClosed,
    SymbolNotTrading,
    ReduceOnlyViolation,
    RiskLimitExceeded,      ///< ours, not theirs
    Connectivity,
    InternalError,
};
[[nodiscard]] std::string_view to_string(RejectReason r) noexcept;

/// Instrument tradability as reported by the venue.
enum class MarketStatus : std::uint8_t {
    Unknown = 0,
    Trading,
    Halted,
    AuctionOnly,
    Delisted,
};
[[nodiscard]] std::string_view to_string(MarketStatus s) noexcept;

// -------------------------------------------------------------- timing envelope

/// Attached to every event so latency is measured rather than inferred.
///
/// The three internal stamps come from `steady_ns()` and are mutually
/// subtractable. `exchange_ns` is wall-clock from a foreign machine and is used
/// only for staleness heuristics and skew monitoring -- subtracting it from a
/// steady reading would be comparing two unrelated timelines.
struct EventStamps {
    Nanos exchange_ns = 0;  ///< venue-reported event time (wall clock, untrusted)
    Nanos recv_ns = 0;      ///< stamped in the socket read handler (steady)
    Nanos parse_ns = 0;     ///< stamped after decode (steady)
    Nanos process_ns = 0;   ///< stamped when the trading thread dequeues (steady)

    [[nodiscard]] constexpr Nanos decode_latency() const noexcept { return parse_ns - recv_ns; }
    [[nodiscard]] constexpr Nanos queue_latency() const noexcept { return process_ns - parse_ns; }
    [[nodiscard]] constexpr Nanos ingress_latency() const noexcept { return process_ns - recv_ns; }
};

static_assert(std::is_trivially_copyable_v<EventStamps>);

// ------------------------------------------------------------------ instrument

/// Venue trading rules for one instrument, normalized. Every outbound order is
/// validated against these before it is allowed near the network -- a rejected
/// order costs a round trip and a rate-limit slot, and a burst of them looks
/// like an outage to the safety layer.
struct InstrumentSpec {
    Symbol symbol{};
    SymbolId id = SymbolId::kInvalid;
    Asset base{};
    Asset quote{};

    Px tick_size{};        ///< price granularity
    Qty lot_size{};        ///< quantity granularity
    Qty min_qty{};
    Qty max_qty{};
    Notional min_notional{};
    Px price_band_lo{};    ///< zero when the venue publishes no band
    Px price_band_hi{};

    MarketStatus status = MarketStatus::Unknown;

    [[nodiscard]] bool is_valid() const noexcept {
        return !symbol.empty() && tick_size.is_positive() && lot_size.is_positive();
    }
};

// ------------------------------------------------------------------ quotes/BBO

struct BestBidAsk {
    Px bid_px{};
    Qty bid_qty{};
    Px ask_px{};
    Qty ask_qty{};
    Seq seq = kNoSeq;
    Nanos exchange_ns = 0;

    [[nodiscard]] constexpr bool is_two_sided() const noexcept {
        return bid_px.is_positive() && ask_px.is_positive();
    }
    /// A crossed or locked book is never handed to a strategy; this is the
    /// predicate the market-data layer checks before publishing.
    [[nodiscard]] constexpr bool is_sane() const noexcept {
        return is_two_sided() && bid_px < ask_px && bid_qty.is_positive() && ask_qty.is_positive();
    }
    [[nodiscard]] constexpr Px mid() const noexcept {
        return Px::from_raw((bid_px.raw() + ask_px.raw()) / 2);
    }
    [[nodiscard]] constexpr Px spread() const noexcept { return ask_px - bid_px; }

    /// Size-weighted mid. Leans toward the side with less size, which is the
    /// side more likely to move -- a standard fair-value reference.
    [[nodiscard]] Px micro_price() const noexcept {
        const std::int64_t bq = bid_qty.raw();
        const std::int64_t aq = ask_qty.raw();
        const std::int64_t total = bq + aq;
        if (total <= 0) {
            return mid();
        }
        const int128 weighted = static_cast<int128>(bid_px.raw()) * static_cast<int128>(aq) +
                                static_cast<int128>(ask_px.raw()) * static_cast<int128>(bq);
        return Px::from_raw(static_cast<std::int64_t>(weighted / static_cast<int128>(total)));
    }
};

static_assert(std::is_trivially_copyable_v<BestBidAsk>);

}  // namespace mm
