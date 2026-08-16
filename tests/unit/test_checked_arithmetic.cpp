#include <gtest/gtest.h>

#include "mm/common/Fixed.hpp"

namespace mm {
namespace {

TEST(CheckedArithmetic, NotionalMatchesTheUncheckedFormWhenInRange) {
    Px price;
    Qty quantity;
    ASSERT_TRUE(Px::parse("60000", price));
    ASSERT_TRUE(Qty::parse("1.5", quantity));

    Notional checked;
    ASSERT_TRUE(checked_notional_of(price, quantity, checked));
    EXPECT_EQ(checked.raw(), notional_of(price, quantity).raw());
    EXPECT_EQ(checked.to_string(), "90000");
}

TEST(CheckedArithmetic, NotionalReportsOverflowInsteadOfWrapping) {
    // 60,000 x 1e8 units is absurd, and the product exceeds int64 after
    // scaling. The unchecked form wraps, turning an enormous order into a
    // small one -- which a risk engine would then happily approve.
    Px price;
    Qty quantity;
    ASSERT_TRUE(Px::parse("60000", price));
    ASSERT_TRUE(Qty::parse("100000000", quantity));

    Notional out;
    EXPECT_FALSE(checked_notional_of(price, quantity, out))
        << "an out-of-range notional must be reported, not silently truncated";
}

TEST(CheckedArithmetic, NotionalHandlesTheExtremes) {
    Notional out;
    // Largest representable price against a unit quantity.
    EXPECT_TRUE(checked_notional_of(Px::max(), Qty::from_units(1), out));
    // Two maxima cannot possibly fit.
    EXPECT_FALSE(checked_notional_of(Px::max(), Qty::max(), out));
    // Zero on either side is always fine.
    EXPECT_TRUE(checked_notional_of(Px::zero(), Qty::max(), out));
    EXPECT_TRUE(out.is_zero());
}

TEST(CheckedArithmetic, NotionalIsSignedCorrectly) {
    Px price;
    ASSERT_TRUE(Px::parse("100", price));
    Notional out;
    ASSERT_TRUE(checked_notional_of(price, Qty::from_units(-3), out));
    EXPECT_EQ(out.to_string(), "-300") << "a short position has negative notional";
}

TEST(CheckedArithmetic, AddReportsOverflow) {
    Qty out;
    EXPECT_TRUE(checked_add(Qty::from_units(3), Qty::from_units(4), out));
    EXPECT_EQ(out.to_string(), "7");

    // Exposure accumulates across an unbounded number of working orders, which
    // is exactly where a wrapped sum would understate risk.
    EXPECT_FALSE(checked_add(Qty::max(), Qty::from_raw(1), out));
    EXPECT_FALSE(checked_add(Qty::min(), Qty::from_raw(-1), out));
}

TEST(CheckedArithmetic, SubReportsOverflow) {
    Qty out;
    EXPECT_TRUE(checked_sub(Qty::from_units(10), Qty::from_units(4), out));
    EXPECT_EQ(out.to_string(), "6");
    EXPECT_FALSE(checked_sub(Qty::min(), Qty::from_raw(1), out));
    EXPECT_FALSE(checked_sub(Qty::max(), Qty::from_raw(-1), out));
}

TEST(CheckedArithmetic, SaturatingAbsCannotTrap) {
    EXPECT_EQ(saturating_abs(Qty::from_units(-5)).to_string(), "5");
    EXPECT_EQ(saturating_abs(Qty::from_units(5)).to_string(), "5");
    EXPECT_TRUE(saturating_abs(Qty::zero()).is_zero());

    // Negating the minimum is undefined behaviour. Saturating means a caller
    // comparing against a limit gets a rejection rather than a crash.
    EXPECT_EQ(saturating_abs(Qty::min()).raw(), Qty::max().raw());
    EXPECT_TRUE(saturating_abs(Qty::min()).is_positive());
}

TEST(CheckedArithmetic, IsUsableInConstantExpressions) {
    constexpr bool ok = [] {
        Qty out;
        return checked_add(Qty::from_units(1), Qty::from_units(2), out) &&
               out.raw() == Qty::from_units(3).raw();
    }();
    static_assert(ok);
    SUCCEED();
}

}  // namespace
}  // namespace mm
