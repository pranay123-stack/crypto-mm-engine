#pragma once

/// \file RiskDecision.hpp
/// The auditable record of one risk judgement.
///
/// Every decision carries what was asked for, what was permitted, why, and the
/// state the engine was in. That is what makes an incident review possible
/// months later, and it is why the reason is a machine-readable enum rather
/// than a formatted string.
///
/// Trivially copyable and allocation-free: decisions are produced on the
/// trading thread and journalled.

#include "mm/quote/OrderAction.hpp"
#include "mm/risk/PositionState.hpp"
#include "mm/risk/RiskTypes.hpp"

namespace mm::risk {

using quote::OrderAction;

/// The limit values a decision was measured against, captured so the record
/// remains meaningful after the configuration changes.
struct LimitContext {
    Qty max_position{};
    Qty peak_absolute_exposure{};   ///< what the action would have produced
    Qty working_buy{};
    Qty working_sell{};
    Notional order_notional{};
    std::uint32_t open_orders = 0;
};

struct RiskDecision {
    RiskVerdict verdict = RiskVerdict::Reject;
    RiskReason reason = RiskReason::None;
    RiskState state = RiskState::Disarmed;

    /// Exactly as the quote manager asked.
    OrderAction requested{};
    /// What may proceed. Identical to `requested` on Approve; carries the
    /// reduced quantity on Reduce; a Noop on Reject, so a caller that ignores
    /// the verdict and forwards `approved` blindly still sends nothing.
    OrderAction approved{};

    LimitContext limits{};

    /// Steady clock, when the decision was made.
    Nanos decided_ns = 0;
    /// Carried through from the action, so a fill is traceable to the strategy
    /// generation that asked for it and the risk decision that permitted it.
    std::uint64_t generation = 0;
    TraceId trace = kNoTrace;

    [[nodiscard]] bool is_approved() const noexcept {
        return verdict == RiskVerdict::Approve || verdict == RiskVerdict::Reduce;
    }
    /// True when something that adds exposure was permitted. The single
    /// predicate every invariant test checks.
    [[nodiscard]] bool permits_new_exposure() const noexcept {
        return is_approved() && approved.adds_exposure();
    }

    [[nodiscard]] static RiskDecision rejected(const OrderAction& requested, RiskReason reason,
                                               RiskState state, Nanos now) noexcept {
        RiskDecision d;
        d.verdict = RiskVerdict::Reject;
        d.reason = reason;
        d.state = state;
        d.requested = requested;
        // Deliberately a Noop rather than a copy: a caller that forwards the
        // approved action without checking the verdict must still send nothing.
        d.approved = OrderAction{};
        d.approved.slot = requested.slot;
        d.approved.symbol = requested.symbol;
        d.decided_ns = now;
        d.generation = requested.generation;
        d.trace = requested.trace;
        return d;
    }
};

static_assert(std::is_trivially_copyable_v<RiskDecision>);

}  // namespace mm::risk
