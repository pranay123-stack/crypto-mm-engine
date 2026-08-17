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

class RiskEngine;

/// Proof that an action passed risk.
///
/// The OMS accepts only this type, never a bare `OrderAction`. That is what
/// makes the risk boundary impossible to route around: a caller cannot
/// construct an approved action, because only `RiskEngine` can set the flag
/// that makes one valid.
///
/// It is deliberately default-constructible -- it has to be, to stay trivially
/// copyable and to live inside a `RiskDecision` -- but a default-constructed
/// one is invalid, and the OMS refuses it. This is an architectural boundary,
/// not a cryptographic one: it stops a mistake, not an adversary with a
/// debugger, which is the threat that actually matters inside one process.
class ApprovedAction {
public:
    ApprovedAction() = default;

    [[nodiscard]] const OrderAction& action() const noexcept { return action_; }
    /// False for anything not produced by `RiskEngine`.
    [[nodiscard]] bool is_valid() const noexcept { return valid_; }
    /// The risk decision's sequence, so an approval can be tied to the record
    /// that granted it.
    [[nodiscard]] std::uint64_t approval_sequence() const noexcept { return sequence_; }

private:
    friend class RiskEngine;

    OrderAction action_{};
    std::uint64_t sequence_ = 0;
    bool valid_ = false;
};

static_assert(std::is_trivially_copyable_v<ApprovedAction>);

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

    /// The token the OMS requires. Valid only when this decision approved the
    /// action; a rejected decision carries an invalid one, so forwarding it
    /// achieves nothing.
    ApprovedAction approval{};

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
