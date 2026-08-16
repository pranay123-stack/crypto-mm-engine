#pragma once

/// \file ExchangeTypes.hpp
/// Normalized, venue-independent vocabulary for the exchange boundary.
///
/// Everything here describes what *a* venue can tell us, never what a
/// particular venue calls it. Adapters translate their wire vocabulary into
/// these types at the boundary and nothing above the adapter ever sees a venue
/// field name, error code, or enum value.
///
/// Types that already exist in `common/` -- Symbol, ClientOrderId, Px, Qty,
/// Side, OrderType, TimeInForce, RejectReason, EventStamps -- are reused
/// directly and deliberately not redefined here.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"

namespace mm::exchange {

// ---------------------------------------------------------------------------
// Order status as reported BY THE VENUE
//
// This is not the OMS order lifecycle. The OMS owns states like SUBMITTING,
// CANCEL_PENDING and UNKNOWN, because those describe *our* knowledge of an
// order, which no venue can report. Conflating the two would put the OMS state
// machine under the adapter's control -- exactly backwards, since the OMS must
// own order truth. See docs/state-machines.md §1 for the OMS machine and
// docs/exchange-interface.md for the mapping between them.
// ---------------------------------------------------------------------------
enum class OrderStatus : std::uint8_t {
    /// The venue has not told us anything we can interpret. Never assume this
    /// means "no order" -- it means "no information".
    Unknown = 0,
    New,              ///< accepted and resting
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
    Expired,          ///< venue-side expiry (IOC/FOK remainder, session end)
    PendingCancel,    ///< venue acknowledged a cancel request, not yet done
    PendingReplace,
};
[[nodiscard]] std::string_view to_string(OrderStatus s) noexcept;

/// True when the venue can never say anything further about this order.
[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
    return s == OrderStatus::Filled || s == OrderStatus::Canceled ||
           s == OrderStatus::Rejected || s == OrderStatus::Expired;
}

/// True when the order may still consume risk or produce a fill. `Unknown` is
/// deliberately included: an order we know nothing about might be working, and
/// the expensive mistake is assuming it is gone when it is not.
[[nodiscard]] constexpr bool may_still_trade(OrderStatus s) noexcept {
    return !is_terminal(s);
}

/// Outcome of a cancel request, kept separate from OrderStatus because "the
/// cancel was rejected" and "the order was rejected" are different facts with
/// opposite implications for exposure.
enum class CancelStatus : std::uint8_t {
    Unknown = 0,   ///< request sent, outcome not established -- must reconcile
    Accepted,      ///< venue confirmed the order is cancelled
    Rejected,      ///< venue refused; the order is STILL WORKING
    OrderNotFound, ///< venue has no such order: already done, or never existed
    TooLate,       ///< the order filled while the cancel was in flight
};
[[nodiscard]] std::string_view to_string(CancelStatus s) noexcept;

/// What an execution report signifies. Separate from OrderStatus because one
/// report carries both "what happened" (this) and "where the order now stands"
/// (OrderStatus), and venues routinely send a Trade whose resulting status is
/// PartiallyFilled.
enum class ExecutionType : std::uint8_t {
    Unknown = 0,
    New,             ///< order accepted onto the book
    Trade,           ///< a fill occurred
    Canceled,
    Replaced,
    Rejected,
    Expired,
    PendingCancel,
    PendingReplace,
    /// The venue restated the order without a business event -- a resend after
    /// reconnect, or a venue-side correction. Must be treated as idempotent.
    Restated,
};
[[nodiscard]] std::string_view to_string(ExecutionType t) noexcept;

// ---------------------------------------------------------------------------
// Transport-level connection state
//
// Coarser than SessionState and applicable to any stream, market data or
// execution. A socket can be Connected while the session is Stale, which is
// precisely the failure mode that loses money quietly.
// ---------------------------------------------------------------------------
enum class ConnectionState : std::uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Reconnecting,
    /// Repeated failures; the adapter has stopped trying without operator
    /// intervention. Distinct from Disconnected, which is a normal resting state.
    Failed,
};
[[nodiscard]] std::string_view to_string(ConnectionState s) noexcept;

[[nodiscard]] constexpr bool is_usable(ConnectionState s) noexcept {
    return s == ConnectionState::Connected;
}

// ---------------------------------------------------------------------------
// Market-data session state
//
// The single session state machine for the platform; docs/state-machines.md §2
// documents it and there is no second, incompatible one. Only `Ready` permits
// quoting -- every other state suspends it for the affected symbol.
// ---------------------------------------------------------------------------
enum class SessionState : std::uint8_t {
    Disconnected = 0,  ///< no transport
    Connecting,        ///< dialling / TLS handshake
    Connected,         ///< transport up, nothing subscribed yet
    Authenticated,     ///< credentials accepted (private streams only)
    Subscribed,        ///< venue confirmed the subscription; buffering updates
    Syncing,           ///< applying a snapshot and reconciling buffered updates
    Ready,             ///< gap-free, fresh, and internally consistent -- quotable
    Stale,             ///< socket alive but data too old to trust
    ResyncRequired,    ///< sequence gap or invariant violation; book discarded
    Backoff,           ///< waiting before a reconnect attempt
    Error,             ///< unrecoverable without operator action
};
[[nodiscard]] std::string_view to_string(SessionState s) noexcept;

/// The only predicate that may gate quoting. Deliberately a single function so
/// no caller can invent its own notion of "close enough to ready".
[[nodiscard]] constexpr bool is_quotable(SessionState s) noexcept {
    return s == SessionState::Ready;
}

/// States from which the engine expects the session to recover on its own.
[[nodiscard]] constexpr bool is_recovering(SessionState s) noexcept {
    return s == SessionState::Connecting || s == SessionState::Backoff ||
           s == SessionState::Syncing || s == SessionState::ResyncRequired ||
           s == SessionState::Subscribed || s == SessionState::Connected ||
           s == SessionState::Authenticated;
}

/// Validates a transition against the documented machine. Illegal transitions
/// are rejected rather than silently applied, so an adapter bug surfaces as a
/// test failure instead of a session that claims to be Ready without ever
/// having synced.
[[nodiscard]] bool is_legal_transition(SessionState from, SessionState to) noexcept;

// ---------------------------------------------------------------------------
// What a failed request implies about the order's existence
//
// This is the most consequential enum in the exchange layer. See
// docs/exchange-interface.md, "Unknown-state semantics".
// ---------------------------------------------------------------------------
enum class RequestOutcome : std::uint8_t {
    /// The venue confirmed it. The order exists.
    Acknowledged = 0,
    /// The venue explicitly refused it. The order definitively does not exist,
    /// and no reconciliation is required.
    Rejected,
    /// We cannot tell. The request may or may not have reached the venue and
    /// may or may not have created an order. The OMS must treat the order as
    /// possibly-live and reconcile. Timeouts and mid-flight transport failures
    /// land here -- never in Rejected.
    Unknown,
    /// The request definitively never left this process (validation failure,
    /// not connected, local queue full). Safe to discard with no reconciliation.
    NotSent,
};
[[nodiscard]] std::string_view to_string(RequestOutcome o) noexcept;

/// True when the OMS must schedule reconciliation for the affected order.
[[nodiscard]] constexpr bool requires_reconciliation(RequestOutcome o) noexcept {
    return o == RequestOutcome::Unknown;
}

/// True when the order can be discarded without ever asking the venue about it.
[[nodiscard]] constexpr bool is_definitively_absent(RequestOutcome o) noexcept {
    return o == RequestOutcome::Rejected || o == RequestOutcome::NotSent;
}

}  // namespace mm::exchange
