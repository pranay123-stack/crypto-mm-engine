#pragma once

/// \file QuoteIntent.hpp
/// What a strategy is allowed to say, and how the infrastructure checks it.
///
/// A `QuoteIntent` is a *wish*, not an instruction. It carries no order id, no
/// venue parameter, no time-in-force and no post-only flag, because none of
/// those are the strategy's to choose. Whether anything is sent, whether an
/// existing order is amended or cancelled, and what that becomes on the wire
/// are decided downstream by the quote manager, risk and the OMS.
///
/// The type is deliberately unable to express an order.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"
#include "mm/strategy/StrategyTypes.hpp"

namespace mm::strategy {

/// What the strategy wants to happen to its quotes.
enum class QuoteAction : std::uint8_t {
    /// Maintain the quotes described by the price/quantity fields.
    Quote = 0,
    /// Stand down: withdraw from the market. Distinct from `Quote` with zero
    /// size, which would be an invalid quote rather than a deliberate exit.
    Pull,
    /// Nothing has changed. Lets a strategy say "leave my quotes alone" without
    /// restating them, so the quote manager does not churn orders needlessly.
    NoChange,
};
[[nodiscard]] std::string_view to_string(QuoteAction a) noexcept;

/// Why the strategy decided what it decided. Diagnostic only — no layer
/// branches on it — but it is the difference between "the maker stopped
/// quoting" and knowing why at 3am.
enum class IntentReason : std::uint8_t {
    None = 0,
    Normal,
    InventoryLimit,     ///< at or beyond its own inventory comfort
    SpreadTooTight,     ///< market spread does not cover the strategy's edge
    SpreadTooWide,      ///< abnormal book; standing down
    InsufficientDepth,
    Warmup,             ///< not enough state yet to quote
    StrategyDisabled,
    Custom,
};
[[nodiscard]] std::string_view to_string(IntentReason r) noexcept;

/// The strategy's output. Trivially copyable so it can be journaled and cross a
/// ring without allocation.
struct QuoteIntent {
    QuoteAction action = QuoteAction::Pull;

    bool quote_bid = false;
    bool quote_ask = false;
    Px bid_price{};
    Px ask_price{};
    Qty bid_quantity{};
    Qty ask_quantity{};

    IntentReason reason = IntentReason::None;

    // ---- traceability (§17) ----
    StrategyIdentity identity{};
    /// The book sequence this intent was computed from. The runtime rejects an
    /// intent whose sequence does not match the context it was given, which
    /// catches a strategy that cached and replayed an old decision.
    Seq market_sequence = kNoSeq;
    /// Steady clock at the moment the runtime received the intent.
    Nanos computed_ns = 0;
    /// Correlates this decision with everything it later produces.
    TraceId trace = kNoTrace;

    [[nodiscard]] static QuoteIntent pull(IntentReason reason) noexcept {
        QuoteIntent intent;
        intent.action = QuoteAction::Pull;
        intent.reason = reason;
        return intent;
    }
    [[nodiscard]] static QuoteIntent no_change() noexcept {
        QuoteIntent intent;
        intent.action = QuoteAction::NoChange;
        intent.reason = IntentReason::Normal;
        return intent;
    }

    /// True when this intent asks for at least one live quote.
    [[nodiscard]] constexpr bool wants_quotes() const noexcept {
        return action == QuoteAction::Quote && (quote_bid || quote_ask);
    }
};

static_assert(std::is_trivially_copyable_v<QuoteIntent>);

/// Why an intent was refused. A buggy strategy must not be able to reach risk,
/// let alone the venue, and each cause is distinguished so a misbehaving
/// strategy is diagnosable rather than merely blocked.
enum class IntentRejection : std::uint8_t {
    None = 0,
    UnknownAction,
    NonPositivePrice,
    NonPositiveQuantity,
    NegativeQuantity,
    TickMisaligned,
    LotMisaligned,
    NotionalTooSmall,
    QuantityAboveVenueMax,
    CrossedQuote,          ///< the strategy's own bid at or above its own ask
    QuoteTooFarFromTouch,  ///< sanity bound; a mispriced quote is usually a bug
    StaleSequence,         ///< computed from a book we are no longer on
    IdentityMissing,
    IdentityMismatch,      ///< not the strategy the runtime invoked
    SymbolMismatch,
    InstrumentNotLoaded,
};
[[nodiscard]] std::string_view to_string(IntentRejection r) noexcept;

/// Bounds applied to every intent, independent of any particular strategy.
struct IntentLimits {
    /// Refuse a quote further than this from the touch. A strategy that
    /// computes a price two per cent away has almost certainly miscalculated,
    /// and letting it through burns a rate-limit slot to earn a venue rejection.
    std::int32_t max_distance_from_touch_bps = 500;
    /// Refuse an intent computed from a book older than the current one.
    bool require_matching_sequence = true;
};

}  // namespace mm::strategy
