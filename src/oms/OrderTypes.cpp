#include "mm/oms/OrderTypes.hpp"

namespace mm::oms {

std::string_view to_string(OrderState s) noexcept {
    switch (s) {
        case OrderState::Created:         return "CREATED";
        case OrderState::PendingNew:      return "PENDING_NEW";
        case OrderState::Working:         return "WORKING";
        case OrderState::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderState::PendingCancel:   return "PENDING_CANCEL";
        case OrderState::PendingReplace:  return "PENDING_REPLACE";
        case OrderState::Filled:          return "FILLED";
        case OrderState::Cancelled:       return "CANCELLED";
        case OrderState::Rejected:        return "REJECTED";
        case OrderState::Expired:         return "EXPIRED";
        case OrderState::Unknown:         return "UNKNOWN";
        case OrderState::Orphaned:        return "ORPHANED";
    }
    return "UNKNOWN_STATE";
}

std::string_view to_string(OrderEventType t) noexcept {
    switch (t) {
        case OrderEventType::None:                   return "NONE";
        case OrderEventType::Created:                return "ORDER_CREATED";
        case OrderEventType::RequestSent:            return "ORDER_REQUEST_SENT";
        case OrderEventType::Acknowledged:           return "ORDER_ACKNOWLEDGED";
        case OrderEventType::Rejected:               return "ORDER_REJECTED";
        case OrderEventType::PartiallyFilled:        return "ORDER_PARTIALLY_FILLED";
        case OrderEventType::Filled:                 return "ORDER_FILLED";
        case OrderEventType::CancelRequested:        return "CANCEL_REQUESTED";
        case OrderEventType::CancelAcknowledged:     return "CANCEL_ACKNOWLEDGED";
        case OrderEventType::CancelRejected:         return "CANCEL_REJECTED";
        case OrderEventType::ReplaceRequested:       return "REPLACE_REQUESTED";
        case OrderEventType::ReplaceAcknowledged:    return "REPLACE_ACKNOWLEDGED";
        case OrderEventType::ReplaceRejected:        return "REPLACE_REJECTED";
        case OrderEventType::Expired:                return "ORDER_EXPIRED";
        case OrderEventType::RequestTimedOut:        return "REQUEST_TIMED_OUT";
        case OrderEventType::OutcomeUnknown:         return "OUTCOME_UNKNOWN";
        case OrderEventType::ReconciliationDetected: return "RECONCILIATION_DETECTED";
        case OrderEventType::ReconciliationResolved: return "RECONCILIATION_RESOLVED";
        case OrderEventType::EventQuarantined:       return "EVENT_QUARANTINED";
    }
    return "UNKNOWN";
}

std::string_view to_string(OmsReject r) noexcept {
    switch (r) {
        case OmsReject::None:                     return "NONE";
        case OmsReject::NotRiskApproved:          return "NOT_RISK_APPROVED";
        case OmsReject::IllegalTransition:        return "ILLEGAL_TRANSITION";
        case OmsReject::TerminalOrder:            return "TERMINAL_ORDER";
        case OmsReject::UnknownOrder:             return "UNKNOWN_ORDER";
        case OmsReject::DuplicateRequest:         return "DUPLICATE_REQUEST";
        case OmsReject::DuplicateEvent:           return "DUPLICATE_EVENT";
        case OmsReject::PendingOperation:         return "PENDING_OPERATION";
        case OmsReject::OwnershipViolation:       return "OWNERSHIP_VIOLATION";
        case OmsReject::StaleGeneration:          return "STALE_GENERATION";
        case OmsReject::CapacityExhausted:        return "CAPACITY_EXHAUSTED";
        case OmsReject::IdentityGenerationFailed: return "IDENTITY_GENERATION_FAILED";
        case OmsReject::ImpossibleFill:           return "IMPOSSIBLE_FILL";
        case OmsReject::ArithmeticOverflow:       return "ARITHMETIC_OVERFLOW";
        case OmsReject::ExecutionUnavailable:     return "EXECUTION_UNAVAILABLE";
        case OmsReject::IdentityCollision:        return "IDENTITY_COLLISION";
    }
    return "UNKNOWN";
}

bool is_legal_transition(OrderState from, OrderState to) noexcept {
    if (from == to) {
        return true;  // idempotent re-report
    }
    // Terminal is terminal. A late event for a finished order is recorded and
    // discarded; it never resurrects the order.
    if (is_terminal(from)) {
        return false;
    }
    // Uncertainty can strike at any moment: a timeout, a transport failure, a
    // response we cannot parse.
    if (to == OrderState::Unknown) {
        return from != OrderState::Created;
    }
    // Quarantine is likewise reachable whenever the venue and our record cannot
    // be reconciled.
    if (to == OrderState::Orphaned) {
        return true;
    }

    switch (from) {
        case OrderState::Created:
            return to == OrderState::PendingNew || to == OrderState::Rejected;

        case OrderState::PendingNew:
            // A venue may coalesce the ack with an immediate fill, so both the
            // resting and the filled outcomes are reachable directly.
            return to == OrderState::Working || to == OrderState::PartiallyFilled ||
                   to == OrderState::Filled || to == OrderState::Rejected ||
                   to == OrderState::Expired;

        case OrderState::Working:
            return to == OrderState::PartiallyFilled || to == OrderState::Filled ||
                   to == OrderState::PendingCancel || to == OrderState::PendingReplace ||
                   to == OrderState::Cancelled || to == OrderState::Expired ||
                   to == OrderState::Rejected;

        case OrderState::PartiallyFilled:
            return to == OrderState::Filled || to == OrderState::PendingCancel ||
                   to == OrderState::PendingReplace || to == OrderState::Cancelled ||
                   to == OrderState::Expired;

        case OrderState::PendingCancel:
            // A cancel does not necessarily win. The order can fill while the
            // request is in flight, and a rejected cancel leaves it working.
            return to == OrderState::Cancelled || to == OrderState::Filled ||
                   to == OrderState::PartiallyFilled || to == OrderState::Working ||
                   to == OrderState::Expired;

        case OrderState::PendingReplace:
            return to == OrderState::Working || to == OrderState::PartiallyFilled ||
                   to == OrderState::Filled || to == OrderState::Cancelled ||
                   to == OrderState::Rejected || to == OrderState::Expired;

        case OrderState::Unknown:
            // Only reconciliation moves an order out of Unknown, and it can
            // land anywhere the evidence supports.
            return true;

        case OrderState::Orphaned:
            // Quarantined until an operator or reconciliation resolves it.
            return to == OrderState::Cancelled || to == OrderState::Unknown;

        case OrderState::Filled:
        case OrderState::Cancelled:
        case OrderState::Rejected:
        case OrderState::Expired:
            break;
    }
    return false;
}

OrderState from_venue_status(exchange::OrderStatus status) noexcept {
    switch (status) {
        case exchange::OrderStatus::New:             return OrderState::Working;
        case exchange::OrderStatus::PartiallyFilled: return OrderState::PartiallyFilled;
        case exchange::OrderStatus::Filled:          return OrderState::Filled;
        case exchange::OrderStatus::Canceled:        return OrderState::Cancelled;
        case exchange::OrderStatus::Rejected:        return OrderState::Rejected;
        case exchange::OrderStatus::Expired:         return OrderState::Expired;
        case exchange::OrderStatus::PendingCancel:   return OrderState::PendingCancel;
        case exchange::OrderStatus::PendingReplace:  return OrderState::PendingReplace;
        case exchange::OrderStatus::Unknown:
            break;
    }
    // The venue saying "I don't know" is not the venue saying "it's gone".
    return OrderState::Unknown;
}

}  // namespace mm::oms
