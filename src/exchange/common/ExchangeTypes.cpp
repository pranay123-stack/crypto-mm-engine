#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::exchange {

std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Unknown:         return "UNKNOWN";
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Canceled:        return "CANCELED";
        case OrderStatus::Rejected:        return "REJECTED";
        case OrderStatus::Expired:         return "EXPIRED";
        case OrderStatus::PendingCancel:   return "PENDING_CANCEL";
        case OrderStatus::PendingReplace:  return "PENDING_REPLACE";
    }
    return "UNKNOWN";
}

std::string_view to_string(CancelStatus s) noexcept {
    switch (s) {
        case CancelStatus::Unknown:       return "UNKNOWN";
        case CancelStatus::Accepted:      return "ACCEPTED";
        case CancelStatus::Rejected:      return "REJECTED";
        case CancelStatus::OrderNotFound: return "ORDER_NOT_FOUND";
        case CancelStatus::TooLate:       return "TOO_LATE";
    }
    return "UNKNOWN";
}

std::string_view to_string(ExecutionType t) noexcept {
    switch (t) {
        case ExecutionType::Unknown:        return "UNKNOWN";
        case ExecutionType::New:            return "NEW";
        case ExecutionType::Trade:          return "TRADE";
        case ExecutionType::Canceled:       return "CANCELED";
        case ExecutionType::Replaced:       return "REPLACED";
        case ExecutionType::Rejected:       return "REJECTED";
        case ExecutionType::Expired:        return "EXPIRED";
        case ExecutionType::PendingCancel:  return "PENDING_CANCEL";
        case ExecutionType::PendingReplace: return "PENDING_REPLACE";
        case ExecutionType::Restated:       return "RESTATED";
    }
    return "UNKNOWN";
}

std::string_view to_string(ConnectionState s) noexcept {
    switch (s) {
        case ConnectionState::Disconnected: return "DISCONNECTED";
        case ConnectionState::Connecting:   return "CONNECTING";
        case ConnectionState::Connected:    return "CONNECTED";
        case ConnectionState::Reconnecting: return "RECONNECTING";
        case ConnectionState::Failed:       return "FAILED";
    }
    return "UNKNOWN";
}

std::string_view to_string(SessionState s) noexcept {
    switch (s) {
        case SessionState::Disconnected:   return "DISCONNECTED";
        case SessionState::Connecting:     return "CONNECTING";
        case SessionState::Connected:      return "CONNECTED";
        case SessionState::Authenticated:  return "AUTHENTICATED";
        case SessionState::Subscribed:     return "SUBSCRIBED";
        case SessionState::Syncing:        return "SYNCING";
        case SessionState::Ready:          return "READY";
        case SessionState::Stale:          return "STALE";
        case SessionState::ResyncRequired: return "RESYNC_REQUIRED";
        case SessionState::Backoff:        return "BACKOFF";
        case SessionState::Error:          return "ERROR";
    }
    return "UNKNOWN";
}

std::string_view to_string(RequestOutcome o) noexcept {
    switch (o) {
        case RequestOutcome::Acknowledged: return "ACKNOWLEDGED";
        case RequestOutcome::Rejected:     return "REJECTED";
        case RequestOutcome::Unknown:      return "UNKNOWN";
        case RequestOutcome::NotSent:      return "NOT_SENT";
    }
    return "UNKNOWN";
}

bool is_legal_transition(SessionState from, SessionState to) noexcept {
    // Re-reporting the current state is a no-op, not a violation: adapters
    // legitimately re-announce state after a heartbeat or a duplicate event.
    if (from == to) {
        return true;
    }
    // A socket can die at any moment, and an unrecoverable fault can be
    // detected from any state. These two are universally reachable.
    if (to == SessionState::Disconnected || to == SessionState::Error) {
        return true;
    }
    // Backoff always follows a failure, so it is reachable from anywhere except
    // two states. From Disconnected the next step is an attempt, not a wait.
    // From Error it must not be reachable at all: Error -> Backoff -> Connecting
    // would let an unrecoverable session quietly retry its way back to Ready,
    // which is exactly the self-healing this state exists to prevent.
    if (to == SessionState::Backoff) {
        return from != SessionState::Disconnected && from != SessionState::Error;
    }

    switch (from) {
        case SessionState::Disconnected:
            return to == SessionState::Connecting;

        case SessionState::Connecting:
            return to == SessionState::Connected;

        case SessionState::Connected:
            // Public market data skips authentication entirely; private streams
            // must pass through it.
            return to == SessionState::Authenticated || to == SessionState::Subscribed;

        case SessionState::Authenticated:
            return to == SessionState::Subscribed;

        case SessionState::Subscribed:
            // A subscription alone never makes a book quotable: the snapshot
            // has to be applied first. Subscribed -> Ready is illegal by design.
            return to == SessionState::Syncing;

        case SessionState::Syncing:
            return to == SessionState::Ready || to == SessionState::ResyncRequired;

        case SessionState::Ready:
            return to == SessionState::Stale || to == SessionState::ResyncRequired;

        case SessionState::Stale:
            // Data resumed within tolerance, or the gap proved unrecoverable.
            return to == SessionState::Ready || to == SessionState::ResyncRequired;

        case SessionState::ResyncRequired:
            return to == SessionState::Syncing;

        case SessionState::Backoff:
            return to == SessionState::Connecting;

        case SessionState::Error:
            // Only an operator clears an error, and only by tearing the session
            // down. It never self-heals into a quotable state.
            return false;
    }
    return false;
}

}  // namespace mm::exchange
