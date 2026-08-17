#pragma once

/// \file RiskTypes.hpp
/// The vocabulary of the risk boundary.
///
/// **Risk is the safety boundary.** The quote manager says what the strategy
/// wants; risk says whether it is permitted. Nothing may reach the OMS without
/// passing through here, and when risk cannot *prove* an exposure-adding action
/// is safe, it refuses.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"

namespace mm::risk {

/// The engine's operating state.
///
/// `Warning` is not decoration: it is **reduce-only**. An action that would
/// increase the absolute position is refused, while one that reduces it is
/// allowed, so the engine can be de-risked without being switched off. Without
/// such a state the only choices are "full quoting" and "nothing", and the
/// second strands whatever position is already open.
enum class RiskState : std::uint8_t {
    /// Constructed but not yet armed. No exposure may be added; a config that
    /// never armed the engine must not be indistinguishable from one that did.
    Disarmed = 0,
    Armed,
    /// Reduce-only. Position-reducing actions pass; position-increasing ones do
    /// not.
    Warning,
    /// Operator or automatic kill. No new exposure, cancellation still allowed,
    /// and no automatic return to Armed.
    Killed,
    /// An internal invariant failed -- an overflow, an impossible input. Same
    /// prohibition as `Killed`, kept distinct because the cause and the remedy
    /// differ: a kill is a decision, a fault is a bug.
    Faulted,
};
[[nodiscard]] std::string_view to_string(RiskState s) noexcept;

/// True when the engine may approve actions that add exposure.
[[nodiscard]] constexpr bool permits_new_exposure(RiskState s) noexcept {
    return s == RiskState::Armed;
}
/// True when only position-reducing exposure is permitted.
[[nodiscard]] constexpr bool is_reduce_only(RiskState s) noexcept {
    return s == RiskState::Warning;
}
/// Killed and Faulted never return to Armed on their own. A condition that
/// tripped a kill does not clear itself, and neither should the kill.
[[nodiscard]] constexpr bool needs_operator_recovery(RiskState s) noexcept {
    return s == RiskState::Killed || s == RiskState::Faulted;
}
[[nodiscard]] bool is_legal_transition(RiskState from, RiskState to) noexcept;

/// What risk decided about one action.
///
/// Deliberately three outcomes rather than five. "Cancel-only" is a property of
/// the *engine* (`Killed`, `Warning`), not a verdict on an individual action,
/// and "kill" is an operator command, not a judgement about an order. Modelling
/// either as a per-action verdict would make every decision carry an opinion
/// about the whole engine, which is both confusing and impossible to audit.
enum class RiskVerdict : std::uint8_t {
    /// May proceed exactly as requested.
    Approve = 0,
    /// May proceed only in the reduced form carried by the decision. The
    /// reduction is explicit; risk never quietly shrinks an order.
    Reduce,
    /// May not proceed at all.
    Reject,
};
[[nodiscard]] std::string_view to_string(RiskVerdict v) noexcept;

/// Machine-readable cause. The primary field an operator, a dashboard or an
/// incident review branches on -- never a free-form string.
enum class RiskReason : std::uint8_t {
    None = 0,

    // ---- limits ----
    MaxPosition,
    MaxPositionNotional,
    MaxOrderQuantity,
    MaxOrderNotional,
    MaxWorkingExposure,
    MaxSideExposure,
    MaxOpenOrders,
    MaxPortfolioNotional,

    // ---- market and instrument ----
    StaleMarket,
    InvalidPrice,
    InvalidQuantity,
    PriceOutsideBand,
    ReferencePriceUnavailable,
    InstrumentInvalid,

    // ---- state ----
    RiskDisarmed,
    RiskKilled,
    RiskFaulted,
    ReduceOnly,          ///< Warning state; the action would increase position
    SystemNotReady,

    // ---- knowledge ----
    PositionUnavailable, ///< no authoritative position to reason from
    PositionStale,
    PortfolioUnavailable, ///< accounting cannot vouch for the portfolio
    PortfolioStale,       ///< the portfolio view is older than tolerated
    PnlIndeterminate,     ///< a loss limit cannot be evaluated: PnL is unknown
    UnknownExposure,     ///< a working order whose state we cannot determine

    // ---- infrastructure ----
    RateLimit,
    OwnershipViolation,
    ArithmeticOverflow,
    ConfigurationMissing,

    /// A reduction produced something that was itself invalid -- below the
    /// venue's minimum, off the lot grid. Reducing is not always possible.
    ReductionInvalid,

    // ---- loss limits (Phase 10) ----
    MaxDailyLoss,
    MaxSessionLoss,
    EmergencyLoss,
};
[[nodiscard]] std::string_view to_string(RiskReason r) noexcept;

/// True for reasons that mean "we could not establish safety", as distinct from
/// "we established it and the answer was no". Both reject; only the first
/// indicates the engine is operating blind, which is an operational problem in
/// its own right.
[[nodiscard]] constexpr bool indicates_blindness(RiskReason r) noexcept {
    return r == RiskReason::PositionUnavailable || r == RiskReason::PositionStale ||
           r == RiskReason::UnknownExposure || r == RiskReason::ReferencePriceUnavailable ||
           r == RiskReason::StaleMarket || r == RiskReason::ArithmeticOverflow ||
           r == RiskReason::ConfigurationMissing || r == RiskReason::PortfolioUnavailable ||
           r == RiskReason::PortfolioStale || r == RiskReason::PnlIndeterminate;
}

}  // namespace mm::risk
