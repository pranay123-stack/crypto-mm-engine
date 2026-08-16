#include "mm/exchange/common/ExchangeError.hpp"

namespace mm::exchange {

std::string_view to_string(ExchangeErrorCategory c) noexcept {
    switch (c) {
        case ExchangeErrorCategory::None:                 return "NONE";
        case ExchangeErrorCategory::Transport:            return "TRANSPORT";
        case ExchangeErrorCategory::Authentication:       return "AUTHENTICATION";
        case ExchangeErrorCategory::InvalidRequest:       return "INVALID_REQUEST";
        case ExchangeErrorCategory::RateLimit:            return "RATE_LIMIT";
        case ExchangeErrorCategory::ExchangeRejection:    return "EXCHANGE_REJECTION";
        case ExchangeErrorCategory::Timeout:              return "TIMEOUT";
        case ExchangeErrorCategory::ConnectionFailure:    return "CONNECTION_FAILURE";
        case ExchangeErrorCategory::UnknownOrderState:    return "UNKNOWN_ORDER_STATE";
        case ExchangeErrorCategory::UnsupportedOperation: return "UNSUPPORTED_OPERATION";
        case ExchangeErrorCategory::MalformedResponse:    return "MALFORMED_RESPONSE";
    }
    return "UNKNOWN";
}

RequestOutcome default_outcome_for(ExchangeErrorCategory c) noexcept {
    switch (c) {
        case ExchangeErrorCategory::None:
            return RequestOutcome::Acknowledged;

        // The venue actively refused. The order does not exist.
        case ExchangeErrorCategory::ExchangeRejection:
            return RequestOutcome::Rejected;

        // Refused locally before anything was written. Nothing to reconcile.
        case ExchangeErrorCategory::UnsupportedOperation:
        case ExchangeErrorCategory::ConnectionFailure:
            return RequestOutcome::NotSent;

        // We do not know. This is the conservative default and the one that
        // matters: a timeout is NOT a rejection.
        case ExchangeErrorCategory::Transport:
        case ExchangeErrorCategory::Timeout:
        case ExchangeErrorCategory::UnknownOrderState:
        case ExchangeErrorCategory::MalformedResponse:
            return RequestOutcome::Unknown;

        // Ambiguous without more context: an authentication or rate-limit
        // failure may be detected locally (never sent) or reported by the venue
        // (definitively refused). The adapter knows which; absent that
        // knowledge, assume the order might exist.
        case ExchangeErrorCategory::Authentication:
        case ExchangeErrorCategory::RateLimit:
        case ExchangeErrorCategory::InvalidRequest:
            return RequestOutcome::Unknown;
    }
    return RequestOutcome::Unknown;
}

bool is_consistent(ExchangeErrorCategory category, RequestOutcome outcome) noexcept {
    // Success is not an error.
    if (category == ExchangeErrorCategory::None) {
        return outcome == RequestOutcome::Acknowledged;
    }
    if (outcome == RequestOutcome::Acknowledged) {
        return false;
    }

    switch (category) {
        // ---- The venue never told us anything. `Rejected` is impossible. ----
        case ExchangeErrorCategory::Timeout:
            // A timeout means the request WAS sent, so NotSent is impossible too.
            return outcome == RequestOutcome::Unknown;

        case ExchangeErrorCategory::UnknownOrderState:
        case ExchangeErrorCategory::MalformedResponse:
            return outcome == RequestOutcome::Unknown;

        case ExchangeErrorCategory::Transport:
        case ExchangeErrorCategory::ConnectionFailure:
            // Either it died mid-flight (Unknown) or before a byte was written
            // (NotSent). It was never refused.
            return outcome == RequestOutcome::Unknown || outcome == RequestOutcome::NotSent;

        // ---- The venue may or may not have spoken. ----
        case ExchangeErrorCategory::Authentication:
        case ExchangeErrorCategory::RateLimit:
        case ExchangeErrorCategory::InvalidRequest:
            return outcome == RequestOutcome::Rejected || outcome == RequestOutcome::NotSent ||
                   outcome == RequestOutcome::Unknown;

        // ---- The venue definitively refused. ----
        case ExchangeErrorCategory::ExchangeRejection:
            return outcome == RequestOutcome::Rejected;

        // ---- We refused, locally and definitively. ----
        case ExchangeErrorCategory::UnsupportedOperation:
            return outcome == RequestOutcome::NotSent;

        case ExchangeErrorCategory::None:
            break;
    }
    return false;
}

namespace {

/// The safest legal outcome for a category, used when an adapter reports an
/// impossible pairing.
///
/// This is deliberately NOT `default_outcome_for`. The default is the *typical*
/// outcome, and for some categories it is `NotSent` -- which tells the OMS the
/// order is definitively absent and may be forgotten. Correcting a confused
/// adapter into a less safe verdict than the one it offered would defeat the
/// purpose of correcting it at all. `Unknown` costs a reconciliation; `NotSent`
/// can cost an untracked live order, so `Unknown` wins wherever it is legal.
RequestOutcome safest_legal_outcome(ExchangeErrorCategory category) noexcept {
    if (is_consistent(category, RequestOutcome::Unknown)) {
        return RequestOutcome::Unknown;
    }
    return default_outcome_for(category);
}

}  // namespace

ExchangeError ExchangeError::make(ExchangeErrorCategory category, RequestOutcome outcome,
                                  std::string_view detail, RejectReason reason,
                                  std::int32_t venue_code, bool retryable) noexcept {
    ExchangeError e;
    e.category = category;
    // An adapter reporting an impossible pair is buggy. Coerce to the safest
    // legal outcome rather than propagating a pairing that could tell the OMS
    // an order is dead when it is in fact working.
    e.outcome = is_consistent(category, outcome) ? outcome : safest_legal_outcome(category);
    e.reason = reason;
    e.venue_code = venue_code;
    e.retryable = retryable;
    static_cast<void>(e.detail.assign(detail));
    return e;
}

ExchangeError ExchangeError::from_category(ExchangeErrorCategory category,
                                           std::string_view detail) noexcept {
    return make(category, default_outcome_for(category), detail);
}

std::string ExchangeError::to_string() const {
    std::string out(::mm::exchange::to_string(category));
    out.push_back('/');
    out.append(::mm::exchange::to_string(outcome));
    if (reason != RejectReason::None) {
        out.push_back('/');
        out.append(::mm::to_string(reason));
    }
    if (venue_code != 0) {
        out.append(" venue_code=").append(std::to_string(venue_code));
    }
    if (!detail.empty()) {
        out.append(": ").append(detail.view());
    }
    return out;
}

}  // namespace mm::exchange
