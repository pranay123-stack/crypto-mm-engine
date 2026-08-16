#pragma once

/// \file QuoteTypes.hpp
/// The vocabulary of the quote manager.
///
/// **The quote manager does not decide whether an order is financially safe.
/// Risk owns that decision.** This layer answers one question: *what orders
/// should currently exist to represent the strategy's intent?* Whether those
/// orders are permitted, and how they reach a venue, belong to Risk and the OMS
/// respectively.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"

namespace mm::quote {

/// The two logical quotes this manager owns per symbol.
///
/// Logical, not physical: a slot is "our bid", independent of which order id
/// currently represents it. Order ids belong to the OMS, and a strategy never
/// sees either.
enum class QuoteSlot : std::uint8_t { Bid = 0, Ask = 1 };
[[nodiscard]] std::string_view to_string(QuoteSlot s) noexcept;

[[nodiscard]] constexpr Side side_of(QuoteSlot slot) noexcept {
    return slot == QuoteSlot::Bid ? Side::Buy : Side::Sell;
}
[[nodiscard]] constexpr QuoteSlot slot_of(Side side) noexcept {
    return side == Side::Buy ? QuoteSlot::Bid : QuoteSlot::Ask;
}

inline constexpr std::size_t kSlotCount = 2;

/// Identifies which logical quote an order represents, and whose it is.
///
/// The OMS will carry orders this manager did not create: manual operator
/// orders, emergency inventory-reducing orders, orders from another strategy.
/// Cancelling one of those would be the quote manager reaching outside its own
/// mandate, so ownership is an explicit tag rather than an assumption that
/// every order on the symbol is ours.
struct QuoteOwner {
    /// The strategy whose intent this order represents. Empty means the order
    /// is not managed by any quote manager -- it is foreign, and untouchable.
    StrategyName strategy{};
    QuoteSlot slot = QuoteSlot::Bid;

    [[nodiscard]] bool is_managed() const noexcept { return !strategy.empty(); }
    [[nodiscard]] bool matches(const StrategyName& owner, QuoteSlot which) const noexcept {
        return is_managed() && strategy == owner && slot == which;
    }
};

static_assert(std::is_trivially_copyable_v<QuoteOwner>);

/// A request this manager has already issued that the venue has not yet
/// resolved. While one is outstanding the slot is not re-driven, which is what
/// prevents a cancel being re-sent on every evaluation cycle.
enum class PendingOperation : std::uint8_t {
    None = 0,
    New,
    Cancel,
    Replace,
};
[[nodiscard]] std::string_view to_string(PendingOperation p) noexcept;

/// What the manager decided for one slot. Reported for every evaluation so a
/// quote that never appears is diagnosable from the decision, not inferred.
enum class SlotOutcome : std::uint8_t {
    /// No order, and none wanted.
    Idle = 0,
    /// The resting order already represents the desired quote.
    Keep,
    New,
    Cancel,
    Replace,
    /// Replace decomposed into cancel-then-new because the venue has no atomic
    /// replace. Exposure is the union of both orders during the window.
    CancelThenNew,
    /// A request is outstanding; the slot waits rather than re-issuing.
    AwaitingPending,
    /// The order's state is unknown. The slot is frozen: no new order may be
    /// created against an order that might still be working.
    FrozenUnknown,
    /// A churn control suppressed a replacement the difference did not justify.
    SuppressedByChurnControl,
    /// The desired quote failed validation, so no action may be generated.
    BlockedInvalid,
};
[[nodiscard]] std::string_view to_string(SlotOutcome o) noexcept;

/// Why an intent was refused wholesale, before any slot was considered.
enum class QuoteRejection : std::uint8_t {
    None = 0,
    /// An intent older than one already processed. It must never resurrect a
    /// quote a newer intent superseded.
    GenerationRegression,
    /// The intent is for a different strategy than this manager serves.
    IdentityMismatch,
    SymbolMismatch,
    /// The intent aged out before it was acted on.
    IntentExpired,
    /// Market data is not fresh enough to act on.
    MarketDataStale,
    /// The venue's trading rules have not been loaded.
    InstrumentNotLoaded,
    NotRunning,
};
[[nodiscard]] std::string_view to_string(QuoteRejection r) noexcept;

/// True when the manager should withdraw its quotes rather than simply ignore
/// the intent. A superseded or misaddressed intent changes nothing; an expired
/// or stale one means the market has moved out from under whatever is resting.
[[nodiscard]] constexpr bool implies_withdraw(QuoteRejection r) noexcept {
    return r == QuoteRejection::IntentExpired || r == QuoteRejection::MarketDataStale ||
           r == QuoteRejection::NotRunning;
}

}  // namespace mm::quote
