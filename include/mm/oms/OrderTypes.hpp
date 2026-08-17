#pragma once

/// \file OrderTypes.hpp
/// The vocabulary of order lifecycle.
///
/// **The OMS is the single authoritative owner of order state.** The quote
/// manager consumes it; risk consumes the exposure derived from it; the
/// strategy never sees it. No other layer runs a competing order state machine.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::oms {

/// Engine-owned identity, assigned before anything is sent and stable for the
/// order's whole life.
///
/// Distinct from `ClientOrderId` (what we send the venue) and
/// `ExchangeOrderId` (what the venue calls it). Conflating them is the classic
/// OMS bug: each is absent at a different stage, and a lookup keyed on one that
/// does not exist yet silently finds nothing.
enum class LogicalOrderId : std::uint64_t { kInvalid = 0 };

[[nodiscard]] constexpr bool is_valid(LogicalOrderId id) noexcept {
    return id != LogicalOrderId::kInvalid;
}

/// Identity of one outbound request. Engine-owned and monotonic, never derived
/// from a venue identifier -- a venue that has not answered yet has given us
/// nothing to key on, which is exactly when duplicate suppression matters.
enum class RequestId : std::uint64_t { kInvalid = 0 };

/// The authoritative lifecycle state.
enum class OrderState : std::uint8_t {
    /// Record exists; nothing has been sent.
    Created = 0,
    /// A new-order request is outstanding. **Not working**: an order is not on
    /// the book because we asked for it to be.
    PendingNew,
    /// Acknowledged and resting.
    Working,
    /// Resting with a non-zero executed quantity.
    PartiallyFilled,
    /// A cancel request is outstanding. Exposure is NOT released here: a cancel
    /// is a request, not a result.
    PendingCancel,
    /// A replace request is outstanding.
    PendingReplace,

    // ---- terminal ----
    Filled,
    Cancelled,
    Rejected,
    Expired,

    /// We cannot prove what happened. Never read as filled, cancelled or
    /// rejected; counts fully against exposure until reconciliation resolves it.
    Unknown,
    /// The venue has an order we have no record of, or our record contradicts
    /// the venue irreconcilably. Quarantined for the operator.
    Orphaned,
};
[[nodiscard]] std::string_view to_string(OrderState s) noexcept;

/// Terminal states accept no outbound transition. A late event for a terminal
/// order is recorded and discarded; it never resurrects the order.
[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Cancelled ||
           s == OrderState::Rejected || s == OrderState::Expired;
}

/// True when the order may still trade, and therefore still consumes exposure.
///
/// `Unknown` is deliberately included. The assumption that costs money is "it
/// probably didn't make it"; the assumption that is safe is "it might be
/// working".
[[nodiscard]] constexpr bool consumes_exposure(OrderState s) noexcept {
    return !is_terminal(s) && s != OrderState::Created && s != OrderState::Orphaned;
}

/// True when the order is resting at the venue as far as we know.
[[nodiscard]] constexpr bool is_live(OrderState s) noexcept {
    return s == OrderState::Working || s == OrderState::PartiallyFilled ||
           s == OrderState::PendingCancel || s == OrderState::PendingReplace;
}

/// True when a request of ours is outstanding.
[[nodiscard]] constexpr bool has_pending_request(OrderState s) noexcept {
    return s == OrderState::PendingNew || s == OrderState::PendingCancel ||
           s == OrderState::PendingReplace;
}

/// What happened to an order, as the OMS understands it. Distinguished from the
/// venue's `ExecutionType` because the OMS also has events the venue does not:
/// a timeout, a reconciliation result, an operator action.
enum class OrderEventType : std::uint8_t {
    None = 0,
    Created,
    RequestSent,
    Acknowledged,
    Rejected,
    PartiallyFilled,
    Filled,
    CancelRequested,
    CancelAcknowledged,
    CancelRejected,
    ReplaceRequested,
    ReplaceAcknowledged,
    ReplaceRejected,
    Expired,
    /// A request went unanswered past its deadline. Produces `Unknown`, never
    /// a fabricated success or failure.
    RequestTimedOut,
    /// The transport told us the outcome is indeterminate.
    OutcomeUnknown,
    ReconciliationDetected,
    ReconciliationResolved,
    /// An event that could not be applied: a duplicate, a contradiction, a
    /// fill for an order we do not have.
    EventQuarantined,
};
[[nodiscard]] std::string_view to_string(OrderEventType t) noexcept;

/// Why a transition or an event was refused.
enum class OmsReject : std::uint8_t {
    None = 0,
    /// The action did not carry valid risk approval. The OMS accepts nothing
    /// else, which is what makes the risk boundary impossible to route around.
    NotRiskApproved,
    IllegalTransition,
    TerminalOrder,
    UnknownOrder,
    DuplicateRequest,
    DuplicateEvent,
    /// A request is already outstanding for this order.
    PendingOperation,
    /// The order belongs to another strategy or slot.
    OwnershipViolation,
    /// A generation older than one already applied.
    StaleGeneration,
    CapacityExhausted,
    /// Client order id could not be generated within the venue's length limit.
    IdentityGenerationFailed,
    /// A fill that cannot be true: negative, zero, or beyond the remaining size.
    ImpossibleFill,
    ArithmeticOverflow,
    ExecutionUnavailable,
    /// The exchange id in this event already belongs to a different order.
    IdentityCollision,
};
[[nodiscard]] std::string_view to_string(OmsReject r) noexcept;

/// Validates a lifecycle transition. Illegal transitions are refused, not
/// applied: a state machine that quietly accepts anything is not a state
/// machine.
[[nodiscard]] bool is_legal_transition(OrderState from, OrderState to) noexcept;

/// Maps the venue's reported status onto the OMS lifecycle.
///
/// Deliberately partial. `OrderStatus::Unknown` maps to `OrderState::Unknown`
/// and never to a terminal state, because the venue saying "I don't know" is
/// not the venue saying "it's gone".
[[nodiscard]] OrderState from_venue_status(exchange::OrderStatus status) noexcept;

}  // namespace mm::oms
