#include <gtest/gtest.h>

#include "mm/portfolio/PortfolioAccountant.hpp"
#include "support/AccountingOracle.hpp"

namespace mm::portfolio {
namespace {

Px px(const char* v) {
    Px p;
    EXPECT_TRUE(Px::parse(v, p));
    return p;
}
Qty qty(const char* v) {
    Qty q;
    EXPECT_TRUE(Qty::parse(v, q));
    return q;
}
Notional fee(const char* v) {
    Notional n;
    EXPECT_TRUE(Notional::parse(v, n));
    return n;
}

class AccountTest : public ::testing::Test {
protected:
    AccountTest() : account(Symbol("BTCUSDT")) {}

    /// Applies a fill with an automatically distinct trade id.
    FillOutcome fill(Side side, const char* price, const char* quantity,
                     const char* fee_amount = "0") {
        const std::string id = "T" + std::to_string(++trade_seq);
        return account.apply_fill(TradeId(id.c_str()), side, px(price), qty(quantity),
                                  fee(fee_amount));
    }

    PositionAccount account;
    int trade_seq = 0;
};

// ===========================================================================
// The four canonical realized-PnL cases
// ===========================================================================

TEST_F(AccountTest, LongProfit) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    const FillOutcome out = fill(Side::Sell, "110", "1");
    ASSERT_TRUE(out.ok()) << to_string(out.error);
    EXPECT_EQ(out.realized_delta.to_string(), "10");
    EXPECT_EQ(account.realized().to_string(), "10");
    EXPECT_TRUE(account.is_flat());
    EXPECT_TRUE(out.closed);
}

TEST_F(AccountTest, LongLoss) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    EXPECT_EQ(fill(Side::Sell, "90", "1").realized_delta.to_string(), "-10");
    EXPECT_EQ(account.realized().to_string(), "-10");
}

TEST_F(AccountTest, ShortProfit) {
    ASSERT_TRUE(fill(Side::Sell, "100", "1").ok());
    EXPECT_EQ(account.position().to_string(), "-1");
    EXPECT_EQ(fill(Side::Buy, "90", "1").realized_delta.to_string(), "10");
    EXPECT_EQ(account.realized().to_string(), "10");
}

TEST_F(AccountTest, ShortLoss) {
    ASSERT_TRUE(fill(Side::Sell, "100", "1").ok());
    EXPECT_EQ(fill(Side::Buy, "110", "1").realized_delta.to_string(), "-10");
}

// ===========================================================================
// Position transitions
// ===========================================================================

TEST_F(AccountTest, OpeningSetsTheCostBasisToTheFillPrice) {
    ASSERT_TRUE(fill(Side::Buy, "100", "2").ok());
    EXPECT_EQ(account.position().to_string(), "2");
    EXPECT_EQ(account.average_price().to_string(), "100");
    EXPECT_TRUE(account.realized().is_zero());
}

TEST_F(AccountTest, IncreasingWeightsTheAverage) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    ASSERT_TRUE(fill(Side::Buy, "120", "3").ok());
    // (1*100 + 3*120) / 4 = 115
    EXPECT_EQ(account.position().to_string(), "4");
    EXPECT_EQ(account.average_price().to_string(), "115");
    EXPECT_TRUE(account.realized().is_zero()) << "increasing realizes nothing";
}

TEST_F(AccountTest, PartialReductionLeavesTheCostBasisAlone) {
    ASSERT_TRUE(fill(Side::Buy, "100", "10").ok());
    const FillOutcome out = fill(Side::Sell, "110", "4");
    ASSERT_TRUE(out.ok());

    EXPECT_EQ(out.realized_delta.to_string(), "40");
    EXPECT_EQ(account.position().to_string(), "6");
    // The 6 still held is the same inventory bought at the same price.
    // Re-deriving the basis from the residual would make it move because
    // something else was sold.
    EXPECT_EQ(account.average_price().to_string(), "100");
}

TEST_F(AccountTest, ClosingExactlyLeavesNoBasis) {
    ASSERT_TRUE(fill(Side::Buy, "100", "5").ok());
    ASSERT_TRUE(fill(Side::Sell, "105", "5").ok());
    EXPECT_TRUE(account.is_flat());
    EXPECT_TRUE(account.average_price().is_zero()) << "flat carries no cost basis";
    EXPECT_EQ(account.realized().to_string(), "25");
}

TEST_F(AccountTest, FlipLongToShortClosesThenOpens) {
    ASSERT_TRUE(fill(Side::Buy, "100", "2").ok());
    const FillOutcome out = fill(Side::Sell, "110", "5");
    ASSERT_TRUE(out.ok());

    EXPECT_TRUE(out.flipped);
    // The whole 2-lot long closed at 110: +20. The residual 3 opened short.
    EXPECT_EQ(out.realized_delta.to_string(), "20");
    EXPECT_EQ(account.position().to_string(), "-3");
    EXPECT_EQ(account.average_price().to_string(), "110")
        << "the new position opened at this fill's price";
}

TEST_F(AccountTest, FlipShortToLongClosesThenOpens) {
    ASSERT_TRUE(fill(Side::Sell, "100", "2").ok());
    const FillOutcome out = fill(Side::Buy, "90", "5");
    ASSERT_TRUE(out.ok());

    EXPECT_TRUE(out.flipped);
    EXPECT_EQ(out.realized_delta.to_string(), "20");
    EXPECT_EQ(account.position().to_string(), "3");
    EXPECT_EQ(account.average_price().to_string(), "90");
}

TEST_F(AccountTest, MultipleEntriesAndExits) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    ASSERT_TRUE(fill(Side::Buy, "102", "1").ok());
    ASSERT_TRUE(fill(Side::Buy, "104", "2").ok());
    // (100 + 102 + 208) / 4 = 102.5
    EXPECT_EQ(account.average_price().to_string(), "102.5");

    ASSERT_TRUE(fill(Side::Sell, "110", "1").ok());   // +7.5
    ASSERT_TRUE(fill(Side::Sell, "105", "2").ok());   // +5
    EXPECT_EQ(account.realized().to_string(), "12.5");
    EXPECT_EQ(account.position().to_string(), "1");
    EXPECT_EQ(account.average_price().to_string(), "102.5");
}

TEST_F(AccountTest, ExactDecimalQuantitiesStayExact) {
    ASSERT_TRUE(fill(Side::Buy, "60000.12", "0.00100000").ok());
    ASSERT_TRUE(fill(Side::Buy, "60000.14", "0.00300000").ok());
    // (0.001*60000.12 + 0.003*60000.14) / 0.004 = 60000.135
    EXPECT_EQ(account.average_price().to_string(), "60000.135");
    EXPECT_EQ(account.position().to_string(), "0.004");
}

// ===========================================================================
// Fees
// ===========================================================================

TEST_F(AccountTest, FeesAccumulateAndAreNotConfusedWithNotional) {
    // Phase 9 regression: the paper venue once reported the whole executed
    // notional as the fee. Accounting must never blur the two -- a 1-lot fill
    // at 60000 with a 6.00 fee has a notional of 60000 and a fee of 6.
    ASSERT_TRUE(fill(Side::Buy, "60000", "1", "6").ok());
    EXPECT_EQ(account.fees().to_string(), "6");

    Notional cost{};
    ASSERT_TRUE(account.checked_cost_basis(cost));
    EXPECT_EQ(cost.to_string(), "60000");
    EXPECT_LT(account.fees(), cost) << "a fee is a fraction of notional, never the whole of it";
}

TEST_F(AccountTest, FeesDoNotEnterTheCostBasis) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1", "5").ok());
    // Folding the fee in would make average_price 105 and stop it being a
    // price -- every comparison against a market price would then be wrong.
    EXPECT_EQ(account.average_price().to_string(), "100");
    EXPECT_EQ(account.fees().to_string(), "5");
}

TEST_F(AccountTest, FeesReduceNetButNotGross) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1", "1").ok());
    ASSERT_TRUE(fill(Side::Sell, "110", "1", "1").ok());

    const PnlBreakdown p = account.pnl();
    EXPECT_EQ(p.realized.to_string(), "10") << "gross is trading PnL before costs";
    EXPECT_EQ(p.fees.to_string(), "2");
    Notional net;
    ASSERT_TRUE(p.checked_net(net));
    EXPECT_EQ(net.to_string(), "8");
}

// ===========================================================================
// Marks and unrealized PnL
// ===========================================================================

TEST_F(AccountTest, LongMarksAtTheBid) {
    ASSERT_TRUE(fill(Side::Buy, "100", "2").ok());
    MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("104");
    mark.ask = px("106");
    mark.as_of_ns = 1'000;

    EXPECT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
    // Marked at the bid: what the long could actually be sold into. Marking at
    // the mid (105) would flatter it by half the spread.
    EXPECT_EQ(account.unrealized().to_string(), "8");
    EXPECT_TRUE(account.unrealized_valid());
}

TEST_F(AccountTest, ShortMarksAtTheAsk) {
    ASSERT_TRUE(fill(Side::Sell, "100", "2").ok());
    MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("94");
    mark.ask = px("96");
    mark.as_of_ns = 1'000;

    EXPECT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
    // Marked at the ask: what it would cost to buy the short back. (100-96)*2.
    EXPECT_EQ(account.unrealized().to_string(), "8");
}

TEST_F(AccountTest, LosingMarks) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("90");
    mark.ask = px("91");
    mark.as_of_ns = 1'000;
    EXPECT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
    EXPECT_EQ(account.unrealized().to_string(), "-10");
}

TEST_F(AccountTest, FlatPositionHasZeroUnrealizedAndItIsDeterminate) {
    MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("100");
    mark.ask = px("101");
    mark.as_of_ns = 1'000;
    EXPECT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
    EXPECT_TRUE(account.unrealized().is_zero());
    EXPECT_TRUE(account.unrealized_valid());
}

TEST_F(AccountTest, AFillInvalidatesTheMarkUntilAFreshOneArrives) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("110");
    mark.ask = px("111");
    mark.as_of_ns = 1'000;
    ASSERT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
    ASSERT_EQ(account.unrealized().to_string(), "10");

    // The position changed, so the previous mark describes a position that no
    // longer exists. Carrying the number forward would misstate PnL.
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    EXPECT_FALSE(account.unrealized_valid());
    EXPECT_TRUE(account.unrealized().is_zero());
}

TEST_F(AccountTest, MarkUpdatesTrackTheMarket) {
    ASSERT_TRUE(fill(Side::Buy, "100", "1").ok());
    for (const auto& [bid, expected] : std::vector<std::pair<const char*, const char*>>{
             {"101", "1"}, {"105", "5"}, {"99", "-1"}, {"100", "0"}}) {
        MarkPrice mark;
        mark.symbol = Symbol("BTCUSDT");
        mark.bid = px(bid);
        mark.ask = px("200");
        mark.as_of_ns = 1'000;
        ASSERT_EQ(account.apply_mark(mark, 1'000, seconds(5)), AccountingError::None);
        EXPECT_EQ(account.unrealized().to_string(), expected);
    }
}

}  // namespace
}  // namespace mm::portfolio
