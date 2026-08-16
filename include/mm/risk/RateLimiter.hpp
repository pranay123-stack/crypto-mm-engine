#pragma once

/// \file RateLimiter.hpp
/// Deterministic token bucket.
///
/// A market-data burst must not become an unbounded stream of order actions.
/// The bucket refills at a fixed rate and holds a fixed capacity, so a burst
/// draws down the reserve and the sustained rate is bounded regardless.
///
/// Time is injected. A rate limiter tested against the wall clock is a rate
/// limiter tested by sleeping, and such tests are slow, flaky, and silent about
/// the boundary cases that matter.

#include <cstdint>

#include "mm/common/Time.hpp"

namespace mm::risk {

class RateLimiter {
public:
    RateLimiter() = default;

    /// `capacity` is the burst allowance; `per_second` the sustained rate.
    /// A zero rate disables the limiter entirely -- an unset limit means
    /// "unlimited" only here, where the alternative would be to block all
    /// trading on a missing config value.
    RateLimiter(std::int32_t capacity, std::int32_t per_second) noexcept
        : capacity_(capacity), per_second_(per_second) {
        tokens_ = static_cast<double>(capacity);
    }

    /// Consumes one token if available. Returns false when the budget is spent.
    [[nodiscard]] bool try_acquire(Nanos now) noexcept {
        if (per_second_ <= 0) {
            return true;
        }
        refill(now);
        if (tokens_ < 1.0) {
            ++denials_;
            return false;
        }
        tokens_ -= 1.0;
        ++grants_;
        return true;
    }

    /// Reports whether a token is available without consuming one.
    [[nodiscard]] bool would_allow(Nanos now) const noexcept {
        if (per_second_ <= 0) {
            return true;
        }
        return projected_tokens(now) >= 1.0;
    }

    void reset(Nanos now) noexcept {
        tokens_ = static_cast<double>(capacity_);
        last_refill_ns_ = now;
    }

    [[nodiscard]] std::uint64_t grants() const noexcept { return grants_; }
    [[nodiscard]] std::uint64_t denials() const noexcept { return denials_; }
    [[nodiscard]] bool enabled() const noexcept { return per_second_ > 0; }
    [[nodiscard]] double available(Nanos now) const noexcept { return projected_tokens(now); }

private:
    [[nodiscard]] double projected_tokens(Nanos now) const noexcept {
        if (last_refill_ns_ == 0) {
            return static_cast<double>(capacity_);
        }
        const Nanos elapsed = now - last_refill_ns_;
        if (elapsed <= 0) {
            return tokens_;
        }
        const double refilled = tokens_ + (static_cast<double>(elapsed) /
                                           static_cast<double>(kNanosPerSecond)) *
                                              static_cast<double>(per_second_);
        const double cap = static_cast<double>(capacity_);
        return refilled > cap ? cap : refilled;
    }

    void refill(Nanos now) noexcept {
        if (last_refill_ns_ == 0) {
            last_refill_ns_ = now;
            tokens_ = static_cast<double>(capacity_);
            return;
        }
        tokens_ = projected_tokens(now);
        last_refill_ns_ = now;
    }

    std::int32_t capacity_ = 0;
    std::int32_t per_second_ = 0;
    double tokens_ = 0.0;
    Nanos last_refill_ns_ = 0;
    std::uint64_t grants_ = 0;
    std::uint64_t denials_ = 0;
};

}  // namespace mm::risk
