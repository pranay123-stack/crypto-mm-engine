#pragma once

/// \file Fixed.hpp
/// Fixed-point scalars for prices, quantities and notionals.
///
/// Money is never represented as a floating-point number in this engine.
/// Binary floating point cannot represent 0.1 exactly, so repeated addition of
/// fills drifts, two nodes computing the same PnL disagree in the last digits,
/// and reconciliation against the exchange fails for reasons that have nothing
/// to do with trading. Everything monetary is a scaled 64-bit integer with a
/// fixed 1e-8 resolution -- enough for every crypto venue's tick and lot sizes,
/// and exactly representable end to end.

#include <cstdint>
#include <cstdlib>
#include <compare>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace mm {

#if defined(__SIZEOF_INT128__)
__extension__ using int128 = __int128;
#else
#  error "mm requires 128-bit integer support for overflow-free notional math"
#endif

namespace detail {

/// Exact decimal string -> raw fixed-point. Returns false on malformed input,
/// on overflow, or on more fractional digits than the scale can hold without
/// rounding away information the venue considered significant.
[[nodiscard]] bool parse_fixed_raw(std::string_view text, std::int64_t& out) noexcept;

/// Raw fixed-point -> shortest exact decimal string (trailing zeros trimmed).
[[nodiscard]] std::string format_fixed_raw(std::int64_t raw);

}  // namespace detail

/// Strongly-typed fixed-point value. `Tag` prevents a price being assigned to a
/// quantity, which is the single most common unit bug in trading code.
template <class Tag>
class Fixed {
public:
    using Raw = std::int64_t;

    static constexpr Raw kScale = 100'000'000;  ///< 1e8: 8 decimal places
    static constexpr int kDecimals = 8;

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed from_raw(Raw r) noexcept {
        Fixed f;
        f.raw_ = r;
        return f;
    }

    /// Whole units, e.g. `from_units(3)` == 3.00000000
    [[nodiscard]] static constexpr Fixed from_units(std::int64_t whole) noexcept {
        return from_raw(whole * kScale);
    }

    /// Lossy by construction. Permitted only where the source is already a
    /// double (strategy parameters, test fixtures) -- never on exchange data,
    /// which arrives as a decimal string and must go through `parse`.
    [[nodiscard]] static Fixed from_double(double d) noexcept {
        return from_raw(static_cast<Raw>(std::llround(d * static_cast<double>(kScale))));
    }

    /// Exact parse of a venue decimal string such as "60123.45000000".
    [[nodiscard]] static bool parse(std::string_view text, Fixed& out) noexcept {
        Raw r = 0;
        if (!detail::parse_fixed_raw(text, r)) {
            return false;
        }
        out = from_raw(r);
        return true;
    }

    [[nodiscard]] static constexpr Fixed zero() noexcept { return Fixed{}; }
    [[nodiscard]] static constexpr Fixed min() noexcept {
        return from_raw(std::numeric_limits<Raw>::min());
    }
    [[nodiscard]] static constexpr Fixed max() noexcept {
        return from_raw(std::numeric_limits<Raw>::max());
    }

    [[nodiscard]] constexpr Raw raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(kScale);
    }
    [[nodiscard]] std::string to_string() const { return detail::format_fixed_raw(raw_); }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }

    [[nodiscard]] constexpr Fixed abs() const noexcept {
        return from_raw(raw_ < 0 ? -raw_ : raw_);
    }

    constexpr Fixed& operator+=(Fixed o) noexcept { raw_ += o.raw_; return *this; }
    constexpr Fixed& operator-=(Fixed o) noexcept { raw_ -= o.raw_; return *this; }
    constexpr Fixed& operator*=(std::int64_t k) noexcept { raw_ *= k; return *this; }

    [[nodiscard]] friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
        return from_raw(a.raw_ + b.raw_);
    }
    [[nodiscard]] friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
        return from_raw(a.raw_ - b.raw_);
    }
    [[nodiscard]] friend constexpr Fixed operator-(Fixed a) noexcept {
        return from_raw(-a.raw_);
    }
    [[nodiscard]] friend constexpr Fixed operator*(Fixed a, std::int64_t k) noexcept {
        return from_raw(a.raw_ * k);
    }
    [[nodiscard]] friend constexpr Fixed operator*(std::int64_t k, Fixed a) noexcept {
        return from_raw(a.raw_ * k);
    }
    [[nodiscard]] friend constexpr Fixed operator/(Fixed a, std::int64_t k) noexcept {
        return from_raw(a.raw_ / k);
    }

    /// Scale by a ratio without losing precision to an intermediate double.
    [[nodiscard]] constexpr Fixed scaled_by(std::int64_t num, std::int64_t den) const noexcept {
        const int128 wide = static_cast<int128>(raw_) * static_cast<int128>(num);
        return from_raw(static_cast<Raw>(wide / static_cast<int128>(den)));
    }

    [[nodiscard]] friend constexpr bool operator==(Fixed a, Fixed b) noexcept {
        return a.raw_ == b.raw_;
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Fixed a, Fixed b) noexcept {
        return a.raw_ <=> b.raw_;
    }

private:
    Raw raw_ = 0;
};

struct PxTag;
struct QtyTag;
struct NotionalTag;

using Px       = Fixed<PxTag>;        ///< price in quote currency
using Qty      = Fixed<QtyTag>;       ///< quantity in base currency
using Notional = Fixed<NotionalTag>;  ///< price x quantity, in quote currency

/// price x quantity, computed in 128 bits so a large notional cannot silently
/// wrap: 60000e8 * 1e8 already exceeds int64.
///
/// The result truncates toward zero at the 1e-8 scale. Truncation rather than
/// rounding keeps the operation odd -- notional_of(p, -q) == -notional_of(p, q)
/// -- so a position and its exact offset net to zero instead of leaving a
/// one-unit residue that would fail reconciliation.
[[nodiscard]] constexpr Notional notional_of(Px p, Qty q) noexcept {
    const int128 wide = static_cast<int128>(p.raw()) * static_cast<int128>(q.raw());
    return Notional::from_raw(static_cast<std::int64_t>(wide / Px::kScale));
}

/// Overflow-checked forms, for code that must *prove* an operation is safe
/// rather than assume it.
///
/// `notional_of` and the arithmetic operators compute correctly for every value
/// a real venue produces, but they narrow a 128-bit intermediate to int64
/// without checking, and `+`/`-` wrap on overflow like any integer. That is the
/// right trade on the hot path, where the inputs have already been validated.
///
/// It is the wrong trade in the risk engine, whose entire job is to refuse what
/// it cannot show to be safe: a wrapped notional would turn an absurd order
/// into a small one and approve it. These return false instead, and the caller
/// fails closed.

/// price x quantity with an explicit range check.
[[nodiscard]] constexpr bool checked_notional_of(Px p, Qty q, Notional& out) noexcept {
    const int128 wide = static_cast<int128>(p.raw()) * static_cast<int128>(q.raw());
    const int128 scaled = wide / static_cast<int128>(Px::kScale);
    constexpr int128 kMax = static_cast<int128>(std::numeric_limits<std::int64_t>::max());
    constexpr int128 kMin = static_cast<int128>(std::numeric_limits<std::int64_t>::min());
    if (scaled > kMax || scaled < kMin) {
        return false;
    }
    out = Notional::from_raw(static_cast<std::int64_t>(scaled));
    return true;
}

/// Addition that reports overflow instead of wrapping. Exposure is accumulated
/// across an unbounded number of working orders, which is exactly where a
/// wrapped sum would silently understate risk.
template <class Tag>
[[nodiscard]] constexpr bool checked_add(Fixed<Tag> a, Fixed<Tag> b, Fixed<Tag>& out) noexcept {
    const int128 wide = static_cast<int128>(a.raw()) + static_cast<int128>(b.raw());
    constexpr int128 kMax = static_cast<int128>(std::numeric_limits<std::int64_t>::max());
    constexpr int128 kMin = static_cast<int128>(std::numeric_limits<std::int64_t>::min());
    if (wide > kMax || wide < kMin) {
        return false;
    }
    out = Fixed<Tag>::from_raw(static_cast<std::int64_t>(wide));
    return true;
}

template <class Tag>
[[nodiscard]] constexpr bool checked_sub(Fixed<Tag> a, Fixed<Tag> b, Fixed<Tag>& out) noexcept {
    const int128 wide = static_cast<int128>(a.raw()) - static_cast<int128>(b.raw());
    constexpr int128 kMax = static_cast<int128>(std::numeric_limits<std::int64_t>::max());
    constexpr int128 kMin = static_cast<int128>(std::numeric_limits<std::int64_t>::min());
    if (wide > kMax || wide < kMin) {
        return false;
    }
    out = Fixed<Tag>::from_raw(static_cast<std::int64_t>(wide));
    return true;
}

/// Absolute value that cannot trap. `abs()` on the minimum representable value
/// is undefined for a plain negation; this saturates instead, so a caller
/// comparing it against a limit gets a rejection rather than a crash.
template <class Tag>
[[nodiscard]] constexpr Fixed<Tag> saturating_abs(Fixed<Tag> v) noexcept {
    if (v.raw() == std::numeric_limits<std::int64_t>::min()) {
        return Fixed<Tag>::from_raw(std::numeric_limits<std::int64_t>::max());
    }
    return v.raw() < 0 ? Fixed<Tag>::from_raw(-v.raw()) : v;
}

/// notional / price -> quantity. Returns zero for a zero price rather than
/// trapping; callers on the quoting path must reject a zero price upstream.
[[nodiscard]] constexpr Qty qty_for_notional(Notional n, Px p) noexcept {
    if (p.raw() == 0) {
        return Qty::zero();
    }
    const int128 wide = static_cast<int128>(n.raw()) * static_cast<int128>(Px::kScale);
    return Qty::from_raw(static_cast<std::int64_t>(wide / static_cast<int128>(p.raw())));
}

/// Ratio of two like-typed values as a plain double. Only for reporting and
/// limit-utilisation display -- never for order sizing.
template <class Tag>
[[nodiscard]] inline double ratio(Fixed<Tag> a, Fixed<Tag> b) noexcept {
    if (b.raw() == 0) {
        return 0.0;
    }
    return static_cast<double>(a.raw()) / static_cast<double>(b.raw());
}

enum class Rounding : std::uint8_t {
    Down,     ///< toward negative infinity
    Up,       ///< toward positive infinity
    Nearest,  ///< half away from zero
};

/// Snap a value onto a venue tick/lot grid. Exchanges reject anything off-grid,
/// so this is applied to every price and quantity before it leaves the engine.
template <class Tag>
[[nodiscard]] constexpr Fixed<Tag> round_to_step(Fixed<Tag> v, Fixed<Tag> step,
                                                 Rounding mode) noexcept {
    if (step.raw() <= 0) {
        return v;
    }
    const std::int64_t s = step.raw();
    const std::int64_t r = v.raw();
    std::int64_t q = r / s;
    const std::int64_t rem = r - q * s;
    if (rem != 0) {
        switch (mode) {
            case Rounding::Down:
                if (rem < 0) { --q; }
                break;
            case Rounding::Up:
                if (rem > 0) { ++q; }
                break;
            case Rounding::Nearest: {
                const std::int64_t twice = (rem < 0 ? -rem : rem) * 2;
                if (twice >= s) { q += (rem > 0 ? 1 : -1); }
                break;
            }
        }
    }
    return Fixed<Tag>::from_raw(q * s);
}

/// True when `v` sits exactly on the grid defined by `step`.
template <class Tag>
[[nodiscard]] constexpr bool is_on_step(Fixed<Tag> v, Fixed<Tag> step) noexcept {
    return step.raw() <= 0 || (v.raw() % step.raw()) == 0;
}

}  // namespace mm
