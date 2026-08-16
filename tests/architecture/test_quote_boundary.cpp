/// \file test_quote_boundary.cpp
/// Architectural acceptance tests for the quote-manager boundary (Phase 6 §28).
///
/// The rule: the quote manager depends only on normalized engine abstractions.
/// It contains no exchange code, no risk logic and no order tracking, and it
/// cannot express an exchange request even if it wanted to.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/quote/QuoteManager.hpp"

// ===========================================================================
// Quote manager boundary (Phase 6 §28)
// ===========================================================================

namespace mm::quote {
namespace {

TEST(QuoteManagerBoundary, ActionsCannotExpressAnExchangeRequest) {
    // An OrderAction is a request for a state change, not an exchange request:
    // no time-in-force, no post-only, no signature, no endpoint. Risk decides
    // whether it is permitted; the OMS decides how it becomes an order.
    static_assert(std::is_trivially_copyable_v<OrderAction>);
    const OrderAction action;
    EXPECT_EQ(action.type, OrderActionType::Noop) << "the safe default does nothing";
    EXPECT_TRUE(action.target_client_order_id.empty())
        << "the manager addresses orders it was told about; it never mints an id";
    SUCCEED();
}

TEST(QuoteManagerBoundary, MoneyStaysFixedPoint) {
    static_assert(std::is_same_v<decltype(OrderAction{}.price), Px>);
    static_assert(std::is_same_v<decltype(OrderAction{}.quantity), Qty>);
    static_assert(std::is_same_v<decltype(WorkingOrder{}.original_quantity), Qty>);
    SUCCEED();
}

TEST(QuoteManagerBoundary, ResultIsCoherentAndRingTransportable) {
    // Both slots are decided from one intent and one snapshot, so an action set
    // can never mix generations.
    static_assert(std::is_trivially_copyable_v<QuoteManagerResult>);
    static_assert(std::is_trivially_copyable_v<WorkingQuoteState>);
    const QuoteManagerResult result;
    EXPECT_FALSE(result.intent_accepted);
    EXPECT_EQ(result.action_count, 0);
    SUCCEED();
}

TEST(QuoteManagerBoundary, OwnershipIsExplicitRatherThanAssumed) {
    // The OMS will carry orders this manager did not create. Ownership is a tag
    // it checks, not an assumption that everything on the symbol is its own.
    const QuoteOwner unmanaged;
    EXPECT_FALSE(unmanaged.is_managed());
    EXPECT_FALSE(unmanaged.matches(StrategyName("anything"), QuoteSlot::Bid));

    QuoteOwner owned;
    static_cast<void>(owned.strategy.assign("mine_v1"));
    owned.slot = QuoteSlot::Bid;
    EXPECT_TRUE(owned.matches(StrategyName("mine_v1"), QuoteSlot::Bid));
    EXPECT_FALSE(owned.matches(StrategyName("mine_v1"), QuoteSlot::Ask))
        << "right strategy, wrong slot, is still not ours";
    EXPECT_FALSE(owned.matches(StrategyName("other_v1"), QuoteSlot::Bid));
}

TEST(QuoteManagerBoundary, RemainingQuantityNeverGoesNegative) {
    WorkingOrder order;
    order.present = true;
    ASSERT_TRUE(Qty::parse("0.001", order.original_quantity));
    ASSERT_TRUE(Qty::parse("0.005", order.cumulative_quantity));  // impossible, but defended
    EXPECT_TRUE(order.remaining().is_zero());
}

}  // namespace
}  // namespace mm::quote
