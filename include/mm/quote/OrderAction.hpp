#pragma once

/// \file OrderAction.hpp
/// What the quote manager asks for, in normalized terms.
///
/// An `OrderAction` is a *request for a state change*, not an exchange request.
/// It carries no venue parameter, no time-in-force, no signature and no
/// endpoint. Risk decides whether it is permitted; the OMS decides how it
/// becomes an order; the adapter decides how it reaches a venue. Nothing here
/// touches any of those.

#include <array>
#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"
#include "mm/quote/QuoteTypes.hpp"
#include "mm/strategy/StrategyTypes.hpp"

namespace mm::quote {

enum class OrderActionType : std::uint8_t {
    Noop = 0,
    New,
    Cancel,
    Replace,
};
[[nodiscard]] std::string_view to_string(OrderActionType t) noexcept;

/// Why this action exists. Diagnostic; no layer branches on it, but it is the
/// difference between "the maker replaced its bid" and knowing why.
enum class ActionReason : std::uint8_t {
    None = 0,
    NoQuoteResting,      ///< nothing there, and a quote is wanted
    PriceChanged,
    QuantityChanged,
    PartialFillReplenish,
    QuoteDisabled,       ///< the strategy asked to stand down on this side
    IntentExpired,
    MarketDataStale,
    StrategyNotRunning,
    OperatorDisabled,
    GenerationSuperseded,  ///< the resting order belongs to older intent
    ReplaceUnsupported,    ///< decomposed because the venue has no atomic replace
};
[[nodiscard]] std::string_view to_string(ActionReason r) noexcept;

/// One requested state change. Trivially copyable, so it can be journaled and
/// cross a ring without allocation.
struct OrderAction {
    OrderActionType type = OrderActionType::Noop;
    QuoteSlot slot = QuoteSlot::Bid;
    ActionReason reason = ActionReason::None;

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    Side side = Side::Buy;

    /// Meaningful for New and Replace.
    Px price{};
    Qty quantity{};

    /// Which order to act on, for Cancel and Replace. Carried opaquely: the
    /// manager addresses an order it was told about and never constructs an id.
    ClientOrderId target_client_order_id{};
    ExchangeOrderId target_exchange_order_id{};

    // ---- provenance, carried through from the intent ----
    strategy::StrategyIdentity identity{};
    std::uint64_t generation = 0;
    Seq market_sequence = kNoSeq;
    Nanos issued_ns = 0;
    TraceId trace = kNoTrace;

    [[nodiscard]] constexpr bool is_noop() const noexcept {
        return type == OrderActionType::Noop;
    }
    /// True when this action would create or move a resting order, i.e. the
    /// actions Risk must scrutinise most closely.
    [[nodiscard]] constexpr bool adds_exposure() const noexcept {
        return type == OrderActionType::New || type == OrderActionType::Replace;
    }
};

static_assert(std::is_trivially_copyable_v<OrderAction>);

/// A replace on a venue without atomic replace becomes cancel-then-new, so one
/// slot can produce two actions and two slots can produce four.
inline constexpr std::size_t kMaxActionsPerEvaluation = 4;

/// What the manager decided for one slot, and why.
struct SlotDecision {
    SlotOutcome outcome = SlotOutcome::Idle;
    ActionReason reason = ActionReason::None;
    /// Set when the outcome is `BlockedInvalid`, carrying the venue rule that
    /// the desired quote failed. Reuses the Phase 3 taxonomy rather than
    /// inventing a parallel one.
    RejectReason invalid_reason = RejectReason::None;
};

/// The complete, coherent result of one evaluation.
///
/// Both slots are decided from a single intent and a single snapshot of working
/// state, so the action set is internally consistent: it can never mix a bid
/// derived from one generation with an ask derived from another.
struct QuoteManagerResult {
    bool intent_accepted = false;
    QuoteRejection rejection = QuoteRejection::None;

    std::array<OrderAction, kMaxActionsPerEvaluation> actions{};
    std::uint8_t action_count = 0;

    SlotDecision bid{};
    SlotDecision ask{};

    /// The generation this result was produced from.
    std::uint64_t generation = 0;

    [[nodiscard]] const SlotDecision& decision(QuoteSlot slot) const noexcept {
        return slot == QuoteSlot::Bid ? bid : ask;
    }
    [[nodiscard]] bool has_actions() const noexcept { return action_count > 0; }

    [[nodiscard]] std::size_t count_of(OrderActionType type) const noexcept {
        std::size_t n = 0;
        for (std::uint8_t i = 0; i < action_count; ++i) {
            if (actions[i].type == type) {
                ++n;
            }
        }
        return n;
    }
};

static_assert(std::is_trivially_copyable_v<QuoteManagerResult>);

}  // namespace mm::quote
