#pragma once

/// \file ExchangeError.hpp
/// Normalized failure model for the exchange boundary.
///
/// The distinction this file exists to preserve:
///
///     "the venue rejected the order"          -> the order does not exist
///     "we do not know what the venue did"     -> the order might be working
///
/// Collapsing those two into a single "error" is how a market maker ends up
/// with an unhedged position it does not know about. Every failure therefore
/// carries an explicit `RequestOutcome` alongside its category, and impossible
/// combinations of the two are rejected by `is_consistent` rather than trusted.

#include <cstdint>
#include <string_view>

#include "mm/common/InlineString.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::exchange {

/// Why a request failed. Deliberately not collapsed into one generic error:
/// each category implies a different response from the OMS and the safety layer.
enum class ExchangeErrorCategory : std::uint8_t {
    None = 0,

    /// I/O error while the request was in flight. We may have written some or
    /// all of it before the socket died.
    Transport,

    /// Credentials missing, expired, or refused.
    Authentication,

    /// The request was structurally wrong -- bad tick size, unknown symbol,
    /// quantity below the minimum. Caught either by our own pre-validation or
    /// by the venue.
    InvalidRequest,

    /// Our limiter or the venue's said no.
    RateLimit,

    /// The venue understood the request and refused it on business grounds:
    /// insufficient balance, post-only would cross, reduce-only violated.
    ExchangeRejection,

    /// No response within the deadline. The request WAS sent.
    Timeout,

    /// The transport was down when we tried, or dropped before the request
    /// could be written.
    ConnectionFailure,

    /// The venue reports an order we cannot map to anything we believe exists,
    /// or refuses to tell us the state of one we do.
    UnknownOrderState,

    /// The abstraction cannot express this on this venue -- e.g. an atomic
    /// replace on a venue without one. Always refused locally.
    UnsupportedOperation,

    /// A response arrived but could not be parsed or contained impossible
    /// values. We cannot conclude anything about the order from it.
    MalformedResponse,
};
[[nodiscard]] std::string_view to_string(ExchangeErrorCategory c) noexcept;

/// The conservative outcome for a category when the adapter has no better
/// information. Conservative means "assume the order might exist" -- the
/// direction whose worst case is an unnecessary reconciliation rather than an
/// untracked live order.
[[nodiscard]] RequestOutcome default_outcome_for(ExchangeErrorCategory c) noexcept;

/// Rejects combinations that cannot physically occur.
///
/// The safety-critical rows: `Timeout`, `Transport`, `ConnectionFailure`,
/// `UnknownOrderState` and `MalformedResponse` can NEVER yield `Rejected`,
/// because in every one of those cases the venue never told us it refused the
/// order. An adapter that tried to report otherwise is buggy, and this function
/// is what catches it.
///
/// `Acknowledged` is never valid on an error: success is not an error.
[[nodiscard]] bool is_consistent(ExchangeErrorCategory category, RequestOutcome outcome) noexcept;

/// A normalized failure. Trivially copyable so it can ride an SPSC ring inside
/// an execution event.
struct ExchangeError {
    ExchangeErrorCategory category = ExchangeErrorCategory::None;

    /// What this failure implies about the order's existence. This, not the
    /// category, is what the OMS branches on.
    RequestOutcome outcome = RequestOutcome::Unknown;

    /// Normalized business reason; meaningful when category is
    /// ExchangeRejection or InvalidRequest.
    RejectReason reason = RejectReason::None;

    /// The venue's own numeric code, carried verbatim for logs and incident
    /// analysis ONLY. Nothing above the adapter may branch on it -- that would
    /// be a venue detail leaking into the core.
    std::int32_t venue_code = 0;

    /// True when retrying the identical request is safe and sensible. False for
    /// anything whose retry could duplicate an order.
    bool retryable = false;

    InlineString<128> detail{};

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return category != ExchangeErrorCategory::None;
    }

    /// Convenience mirrors of the RequestOutcome predicates, so call sites read
    /// as intent rather than as enum comparisons.
    [[nodiscard]] constexpr bool needs_reconciliation() const noexcept {
        return requires_reconciliation(outcome);
    }
    [[nodiscard]] constexpr bool order_definitively_absent() const noexcept {
        return is_definitively_absent(outcome);
    }

    /// Builds an error and forces the category/outcome pair to be legal. An
    /// inconsistent pair is coerced to the category's conservative default
    /// rather than propagated, because the alternative is an adapter bug
    /// silently telling the OMS an order is dead when it may not be.
    [[nodiscard]] static ExchangeError make(ExchangeErrorCategory category, RequestOutcome outcome,
                                            std::string_view detail = {},
                                            RejectReason reason = RejectReason::None,
                                            std::int32_t venue_code = 0,
                                            bool retryable = false) noexcept;

    /// Category-only construction, taking the conservative outcome.
    [[nodiscard]] static ExchangeError from_category(ExchangeErrorCategory category,
                                                     std::string_view detail = {}) noexcept;

    [[nodiscard]] static ExchangeError none() noexcept { return ExchangeError{}; }

    [[nodiscard]] std::string to_string() const;
};

static_assert(std::is_trivially_copyable_v<ExchangeError>);

}  // namespace mm::exchange
