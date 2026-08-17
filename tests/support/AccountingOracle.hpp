#pragma once

/// \file AccountingOracle.hpp
/// An independent reimplementation of the accounting model, for tests only.
///
/// **This must not share a single line with `PositionAccount`.** A test that
/// compares the engine against itself proves the engine is self-consistent,
/// which is not the property anyone cares about. This is written from
/// docs/accounting.md -- the specification -- rather than from the code, in a
/// different style, using `long double` where the engine uses fixed point.
///
/// The floating-point choice is deliberate. Using the engine's own fixed-point
/// helpers would import the very arithmetic under test; a wholly different
/// numeric representation catches a rounding or sign error that a shared
/// implementation would reproduce identically in both places. Comparisons
/// therefore carry a small epsilon, which is the price of independence.

#include <cmath>
#include <string>
#include <vector>

#include "mm/common/Fixed.hpp"
#include "mm/common/Types.hpp"

namespace mm::test {

/// A fill, as the oracle sees it.
struct OracleFill {
    bool buy = true;
    long double price = 0.0L;
    long double quantity = 0.0L;
    long double fee = 0.0L;
};

/// Weighted-average cost accounting, written from the specification.
class AccountingOracle {
public:
    void apply(const OracleFill& f) {
        const long double signed_qty = f.buy ? f.quantity : -f.quantity;
        fees_ += f.fee;
        ++fills_;

        if (position_ == 0.0L) {
            // Opening.
            position_ = signed_qty;
            average_ = f.price;
            return;
        }

        const bool same_side = (position_ > 0.0L) == (signed_qty > 0.0L);
        if (same_side) {
            // Increasing: weighted average of old and new.
            const long double old_abs = std::fabs(position_);
            const long double add_abs = std::fabs(signed_qty);
            average_ = (old_abs * average_ + add_abs * f.price) / (old_abs + add_abs);
            position_ += signed_qty;
            return;
        }

        // Opposite side.
        const long double held = std::fabs(position_);
        const long double incoming = std::fabs(signed_qty);
        const long double closed = incoming < held ? incoming : held;
        const long double direction = position_ > 0.0L ? 1.0L : -1.0L;
        realized_ += (f.price - average_) * closed * direction;

        if (incoming > held) {
            // Flip: residual opens at this price.
            position_ += signed_qty;
            average_ = f.price;
        } else {
            position_ += signed_qty;
            if (position_ == 0.0L) {
                average_ = 0.0L;
            }
            // Partial reduction leaves the basis alone.
        }
    }

    /// Longs mark at the bid, shorts at the ask.
    [[nodiscard]] long double unrealized(long double bid, long double ask) const {
        if (position_ == 0.0L) {
            return 0.0L;
        }
        const long double mark = position_ < 0.0L ? ask : bid;
        const long double direction = position_ > 0.0L ? 1.0L : -1.0L;
        return (mark - average_) * std::fabs(position_) * direction;
    }

    [[nodiscard]] long double position() const { return position_; }
    [[nodiscard]] long double average() const { return average_; }
    [[nodiscard]] long double realized() const { return realized_; }
    [[nodiscard]] long double fees() const { return fees_; }
    [[nodiscard]] long double net(long double bid, long double ask) const {
        return realized_ + unrealized(bid, ask) - fees_;
    }
    [[nodiscard]] std::uint64_t fills() const { return fills_; }

private:
    long double position_ = 0.0L;
    long double average_ = 0.0L;
    long double realized_ = 0.0L;
    long double fees_ = 0.0L;
    std::uint64_t fills_ = 0;
};

/// Fixed-point value as a long double, for comparison against the oracle.
template <class Tag>
[[nodiscard]] inline long double as_real(Fixed<Tag> v) {
    return static_cast<long double>(v.raw()) / static_cast<long double>(Fixed<Tag>::kScale);
}

/// Tolerance for oracle comparisons.
///
/// The engine rounds to 1e-8; the oracle does not round at all. Over a
/// sequence of fills those roundings accumulate, so the bound scales with the
/// number of fills rather than being a fixed number pulled from the air.
[[nodiscard]] inline long double oracle_epsilon(std::size_t fills, long double scale = 1.0L) {
    return 1e-7L * static_cast<long double>(fills + 1) * (scale < 1.0L ? 1.0L : scale);
}

}  // namespace mm::test
