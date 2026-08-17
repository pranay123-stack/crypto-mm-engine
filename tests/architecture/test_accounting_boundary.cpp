/// Phase 10 architecture tests: ownership and dependency structure.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/oms/OrderManager.hpp"
#include "mm/portfolio/PortfolioAccountant.hpp"

namespace mm::portfolio {
namespace {

TEST(AccountingBoundary, AccountingIsNotASecondOms) {
    // There is no way to give accounting an order, ask it about one, or set a
    // position. Its only mutating inputs are a fill, a mark and a balance.
    static_assert(std::is_invocable_v<decltype(&PortfolioAccountant::on_fill),
                                      PortfolioAccountant&, const exchange::FillEvent&>);
    static_assert(!std::is_invocable_v<decltype(&PortfolioAccountant::on_fill),
                                       PortfolioAccountant&, const exchange::OrderAckEvent&>,
                  "accounting must not consume order lifecycle events");
    // Position can only move through a fill. An API that could set it directly
    // is an API through which §23's rule -- positions update from actual fills,
    // never from submitted orders -- gets broken.
    static_assert(!std::is_convertible_v<PositionAccount, oms::OrderRecord>);
    static_assert(!std::is_convertible_v<oms::OrderRecord, PositionAccount>);
    SUCCEED();
}

TEST(AccountingBoundary, TheLedgerIsReachableOnlyAsConstOrByValue) {
    // No mutable handle escapes. A caller that could reach in and adjust a
    // position would be a second, competing authority for it.
    static_assert(std::is_same_v<decltype(std::declval<const PortfolioAccountant&>().find(
                                     std::declval<const Symbol&>())),
                                 const PositionAccount*>);
    static_assert(std::is_same_v<decltype(std::declval<const PortfolioAccountant&>()
                                              .portfolio_snapshot()),
                                 PortfolioSnapshot>);
    SUCCEED();
}

TEST(AccountingBoundary, SnapshotsAreTriviallyCopyableForPublication) {
    // Published to non-trading-thread readers through a seqlock, so they must
    // copy without allocating (docs/concurrency.md).
    static_assert(std::is_trivially_copyable_v<PortfolioSnapshot>);
    static_assert(std::is_trivially_copyable_v<risk::PortfolioState>);
    static_assert(std::is_trivially_copyable_v<risk::PositionSnapshot>);
    static_assert(std::is_trivially_copyable_v<PositionAccount::State>);
    static_assert(std::is_trivially_copyable_v<SeenTrades>);
    static_assert(std::is_trivially_copyable_v<FillOutcome>);
    SUCCEED();
}

TEST(AccountingBoundary, RiskRemainsTheDecisionAuthority) {
    // Accounting produces a fact; it never produces a verdict. There is no
    // approve/reject anywhere in its surface.
    static_assert(std::is_same_v<decltype(std::declval<const PortfolioAccountant&>().risk_view()),
                                 risk::PortfolioState>);
    // And the type it hands over carries no decision, only evidence.
    const risk::PortfolioState view;
    EXPECT_FALSE(view.valid) << "a default portfolio is unusable, never a permissive zero";
    EXPECT_FALSE(view.usable_for_notional_limits());
    EXPECT_FALSE(view.usable_for_loss_limits());
}

TEST(AccountingBoundary, DeterminacyIsSeparateFromValidity) {
    // Two different failures with two different consequences: an unusable
    // ledger blocks everything; an unmarkable symbol blocks only loss limits.
    risk::PortfolioState view;
    view.valid = true;
    view.pnl_determinate = false;
    EXPECT_TRUE(view.usable_for_notional_limits());
    EXPECT_FALSE(view.usable_for_loss_limits());
}

TEST(AccountingBoundary, AccountingOwnsNoMarketData) {
    // It is handed a normalized mark; it never reads a book. `MarkPrice` is
    // the entire market-data surface of this module.
    static_assert(std::is_trivially_copyable_v<MarkPrice>);
    MarkPrice mark;
    EXPECT_FALSE(mark.usable()) << "a default mark is unusable, not zero-valued";

    // The mark rule is a property of the type, not of a caller's convention.
    EXPECT_TRUE(Px::parse("100", mark.bid));
    EXPECT_TRUE(Px::parse("101", mark.ask));
    Qty long_position;
    Qty short_position;
    EXPECT_TRUE(Qty::parse("1", long_position));
    EXPECT_TRUE(Qty::parse("-1", short_position));
    EXPECT_EQ(mark.for_position(long_position), mark.bid);
    EXPECT_EQ(mark.for_position(short_position), mark.ask);
}

TEST(AccountingBoundary, EveryErrorHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(AccountingError::Faulted); ++i) {
        EXPECT_NE(to_string(static_cast<AccountingError>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::portfolio
