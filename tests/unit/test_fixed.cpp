#include <gtest/gtest.h>

#include "mm/common/Fixed.hpp"

namespace mm {
namespace {

TEST(Fixed, ConstructionAndAccess) {
    EXPECT_EQ(Px::from_units(3).raw(), 300'000'000);
    EXPECT_TRUE(Px::zero().is_zero());
    EXPECT_DOUBLE_EQ(Px::from_units(3).to_double(), 3.0);
}

TEST(Fixed, TypesDoNotMix) {
    // A price cannot be assigned to a quantity: distinct tags, no conversion.
    static_assert(!std::is_convertible_v<Px, Qty>);
    static_assert(!std::is_convertible_v<Qty, Notional>);
}

TEST(Fixed, ExactDecimalParse) {
    Px p;
    ASSERT_TRUE(Px::parse("60123.45", p));
    EXPECT_EQ(p.raw(), 6'012'345'000'000);

    ASSERT_TRUE(Px::parse("0.00000001", p));
    EXPECT_EQ(p.raw(), 1);

    ASSERT_TRUE(Px::parse("-12.5", p));
    EXPECT_EQ(p.raw(), -1'250'000'000);

    ASSERT_TRUE(Px::parse("100", p));
    EXPECT_EQ(p.raw(), 10'000'000'000);

    // Venue filters occasionally arrive in exponent form.
    ASSERT_TRUE(Px::parse("1E-8", p));
    EXPECT_EQ(p.raw(), 1);
    ASSERT_TRUE(Px::parse("1.5e2", p));
    EXPECT_EQ(p.raw(), 15'000'000'000);
}

TEST(Fixed, ParseIsExactNotApproximate) {
    // The canonical binary-floating-point failure: 0.1 + 0.2 != 0.3.
    Px a, b, c;
    ASSERT_TRUE(Px::parse("0.1", a));
    ASSERT_TRUE(Px::parse("0.2", b));
    ASSERT_TRUE(Px::parse("0.3", c));
    EXPECT_EQ((a + b).raw(), c.raw());
}

TEST(Fixed, ParseRejectsMalformedInput) {
    Px p;
    EXPECT_FALSE(Px::parse("", p));
    EXPECT_FALSE(Px::parse("abc", p));
    EXPECT_FALSE(Px::parse("1.2.3", p));
    EXPECT_FALSE(Px::parse("1.0x", p));
    EXPECT_FALSE(Px::parse(" 1.0", p));
    EXPECT_FALSE(Px::parse("--1", p));
    EXPECT_FALSE(Px::parse(".", p));
    EXPECT_FALSE(Px::parse("1e", p));
}

TEST(Fixed, ParseRefusesToSilentlyDiscardPrecision) {
    Px p;
    // A ninth significant decimal cannot be represented; losing it silently
    // would corrupt reconciliation, so the parse fails loudly instead.
    EXPECT_FALSE(Px::parse("1.000000001", p));
    // Trailing zeros beyond the scale carry no information and are accepted.
    ASSERT_TRUE(Px::parse("1.0000000000", p));
    EXPECT_EQ(p.raw(), 100'000'000);
}

TEST(Fixed, ParseRejectsOverflow) {
    Px p;
    EXPECT_FALSE(Px::parse("99999999999999999999999", p));
    EXPECT_FALSE(Px::parse("1e300", p));
}

TEST(Fixed, FormatRoundTrips) {
    for (const char* text : {"0", "1", "1.5", "-1.5", "60123.45", "0.00000001", "-0.00000001",
                             "123456789.12345678"}) {
        Px p;
        ASSERT_TRUE(Px::parse(text, p)) << text;
        EXPECT_EQ(p.to_string(), text);
    }
}

TEST(Fixed, FormatHandlesInt64Min) {
    // Negating INT64_MIN is undefined behaviour; the formatter must not do it.
    const std::string s = Px::min().to_string();
    EXPECT_EQ(s.front(), '-');
    Px back;
    ASSERT_TRUE(Px::parse(s, back));
    EXPECT_EQ(back.raw(), Px::min().raw());
}

TEST(Fixed, Arithmetic) {
    const Px a = Px::from_units(10);
    const Px b = Px::from_units(3);
    EXPECT_EQ((a + b).raw(), Px::from_units(13).raw());
    EXPECT_EQ((a - b).raw(), Px::from_units(7).raw());
    EXPECT_EQ((a * 2).raw(), Px::from_units(20).raw());
    EXPECT_EQ((a / 2).raw(), Px::from_units(5).raw());
    EXPECT_EQ((-a).raw(), -Px::from_units(10).raw());
    EXPECT_EQ(a.abs().raw(), (-a).abs().raw());
}

TEST(Fixed, NotionalUsesWideIntermediate) {
    // 60000 * 1.5 = 90000. Both operands scaled by 1e8 would overflow int64 in
    // a naive product (6e12 * 1.5e8 = 9e20), so the maths runs in 128 bits.
    Px price;
    Qty qty;
    ASSERT_TRUE(Px::parse("60000", price));
    ASSERT_TRUE(Qty::parse("1.5", qty));
    EXPECT_EQ(notional_of(price, qty).to_string(), "90000");
}

TEST(Fixed, NotionalPrecisionAtScale) {
    Px price;
    Qty qty;
    ASSERT_TRUE(Px::parse("12345.6789", price));
    ASSERT_TRUE(Qty::parse("0.00012345", qty));
    // 12345.6789 * 0.00012345 = 1.524074060205 exactly; the representable
    // scale is 1e-8, so the result truncates toward zero.
    EXPECT_EQ(notional_of(price, qty).to_string(), "1.52407406");
}

TEST(Fixed, NotionalTruncatesTowardZero) {
    // Truncation, not rounding: the engine never invents a fraction of a tick
    // that the venue did not trade. The direction is toward zero on both signs,
    // so a long and a short of the same size produce equal-magnitude notionals
    // and net to exactly zero rather than leaving a one-unit residue.
    Px price;
    Qty qty;
    // 1.11111111 * 0.33333333 = 0.3703703662962963 -> truncates to 0.37037036
    ASSERT_TRUE(Px::parse("1.11111111", price));
    ASSERT_TRUE(Qty::parse("0.33333333", qty));
    const Notional pos = notional_of(price, qty);
    const Notional neg = notional_of(price, Qty::from_raw(-qty.raw()));
    EXPECT_EQ(pos.to_string(), "0.37037036");
    EXPECT_EQ(neg.raw(), -pos.raw());
    EXPECT_TRUE((pos + neg).is_zero());
}

TEST(Fixed, QtyForNotionalInverts) {
    Px price;
    Notional target;
    ASSERT_TRUE(Px::parse("25000", price));
    ASSERT_TRUE(Notional::parse("500", target));
    const Qty q = qty_for_notional(target, price);
    EXPECT_EQ(q.to_string(), "0.02");
    EXPECT_EQ(notional_of(price, q).to_string(), "500");
}

TEST(Fixed, QtyForNotionalHandlesZeroPrice) {
    EXPECT_TRUE(qty_for_notional(Notional::from_units(100), Px::zero()).is_zero());
}

TEST(Fixed, RoundToStepDown) {
    Px tick;
    ASSERT_TRUE(Px::parse("0.01", tick));
    Px p;
    ASSERT_TRUE(Px::parse("60123.456", p));
    EXPECT_EQ(round_to_step(p, tick, Rounding::Down).to_string(), "60123.45");
    EXPECT_EQ(round_to_step(p, tick, Rounding::Up).to_string(), "60123.46");
    EXPECT_EQ(round_to_step(p, tick, Rounding::Nearest).to_string(), "60123.46");
}

TEST(Fixed, RoundToStepIsSymmetricForNegatives) {
    Px tick;
    ASSERT_TRUE(Px::parse("0.01", tick));
    Px p;
    ASSERT_TRUE(Px::parse("-60123.456", p));
    // Down means toward negative infinity, not toward zero.
    EXPECT_EQ(round_to_step(p, tick, Rounding::Down).to_string(), "-60123.46");
    EXPECT_EQ(round_to_step(p, tick, Rounding::Up).to_string(), "-60123.45");
    EXPECT_EQ(round_to_step(p, tick, Rounding::Nearest).to_string(), "-60123.46");
}

TEST(Fixed, RoundToStepNearestHalfAwayFromZero) {
    Px tick;
    ASSERT_TRUE(Px::parse("0.1", tick));
    Px p;
    ASSERT_TRUE(Px::parse("1.05", p));
    EXPECT_EQ(round_to_step(p, tick, Rounding::Nearest).to_string(), "1.1");
    ASSERT_TRUE(Px::parse("-1.05", p));
    EXPECT_EQ(round_to_step(p, tick, Rounding::Nearest).to_string(), "-1.1");
}

TEST(Fixed, RoundToStepIsIdentityForNonPositiveStep) {
    Px p = Px::from_units(5);
    EXPECT_EQ(round_to_step(p, Px::zero(), Rounding::Nearest).raw(), p.raw());
}

TEST(Fixed, IsOnStep) {
    Px tick;
    ASSERT_TRUE(Px::parse("0.01", tick));
    Px a, b;
    ASSERT_TRUE(Px::parse("60123.45", a));
    ASSERT_TRUE(Px::parse("60123.456", b));
    EXPECT_TRUE(is_on_step(a, tick));
    EXPECT_FALSE(is_on_step(b, tick));
}

TEST(Fixed, ScaledByAvoidsDoubleIntermediate) {
    Px p;
    ASSERT_TRUE(Px::parse("60000", p));
    // -3 basis points
    EXPECT_EQ(p.scaled_by(9997, 10000).to_string(), "59982");
}

TEST(Fixed, Ordering) {
    EXPECT_LT(Px::from_units(1), Px::from_units(2));
    EXPECT_GT(Px::from_units(2), Px::from_units(1));
    EXPECT_EQ(Px::from_units(2), Px::from_units(2));
    EXPECT_LT(Px::from_units(-2), Px::from_units(-1));
}

}  // namespace
}  // namespace mm
