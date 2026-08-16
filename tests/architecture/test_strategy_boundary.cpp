/// \file test_strategy_boundary.cpp
/// Architectural acceptance tests for the strategy boundary (§21, §24).
///
/// The rule these defend is directional:
///
///     Strategy -> QuoteIntent -> (Risk / Quote Manager / OMS) -> Exchange
///
/// and never Strategy -> Exchange. A rule stated in a document decays; these
/// express it as facts a compiler and a test runner can check.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/strategy/IStrategy.hpp"
#include "mm/strategy/StrategyRegistry.hpp"
#include "mm/strategy/StrategyRuntime.hpp"
#include "support/TestStrategies.hpp"

namespace mm::strategy {
namespace {

// ===========================================================================
// A strategy is given nothing it could reach the exchange with
// ===========================================================================

TEST(StrategyBoundary, ContextCarriesNoInfrastructure) {
    // The context is the strategy's entire window onto the world. Everything in
    // it is a value, a read-only view, or a pointer to venue *rules* -- never a
    // socket, a client, a queue or a lock.
    static_assert(std::is_trivially_destructible_v<BookDepthView>,
                  "the depth view must be a plain non-owning window");
    static_assert(std::is_trivially_copyable_v<WorkingQuote>);
    static_assert(std::is_trivially_copyable_v<InventoryView>);

    // The only pointer is to an InstrumentSpec: tick, lot, min notional. There
    // is nothing to call through.
    static_assert(std::is_same_v<decltype(StrategyContext{}.instrument), const InstrumentSpec*>);
    SUCCEED();
}

TEST(StrategyBoundary, WorkingQuotesCarryNoOrderIdentity) {
    // A strategy that knew an order id would be one step from referencing it,
    // and referencing orders belongs to the OMS. What it gets is "there is a
    // quote of this size at this price", and nothing that names an order.
    //
    // No compile-time check can enumerate a struct's members, so this is a
    // tripwire rather than a proof: the struct is currently a flag, two prices,
    // a quantity and a flag. Adding a ClientOrderId (42 bytes) or an
    // ExchangeOrderId (34) would push it well past this bound and fail loudly,
    // which is the point at which someone should be asked why a strategy needs
    // to name an order.
    static_assert(sizeof(WorkingQuote) <= 48,
                  "WorkingQuote grew; check that it has not gained order identity");
    static_assert(std::is_trivially_copyable_v<WorkingQuote>,
                  "an owning member would mean the strategy holds infrastructure state");
    SUCCEED();
}

TEST(StrategyBoundary, TheStrategyIsNeverGivenTheClock) {
    // Time arrives as a value in the context. A strategy taking the time itself
    // would be non-deterministic and untestable, which is why nothing in its
    // inputs exposes a clock.
    static_assert(std::is_same_v<decltype(StrategyContext{}.now_ns), Nanos>);
    SUCCEED();
}

// ===========================================================================
// QuoteIntent cannot express an order
// ===========================================================================

TEST(StrategyBoundary, QuoteIntentCannotExpressAnOrder) {
    // The type is deliberately unable to say the things that are not the
    // strategy's to say: no order id, no time-in-force, no post-only flag, no
    // venue parameter. Downstream layers decide all of that.
    static_assert(std::is_trivially_copyable_v<QuoteIntent>);

    // What it *can* say is prices, sizes and an action.
    const QuoteIntent intent;
    EXPECT_EQ(intent.action, QuoteAction::Pull) << "the safe default is to stand down";
    EXPECT_FALSE(intent.quote_bid);
    EXPECT_FALSE(intent.quote_ask);
    EXPECT_TRUE(intent.bid_price.is_zero());
    SUCCEED();
}

TEST(StrategyBoundary, MoneyNeverBecomesFloatingPoint) {
    static_assert(std::is_same_v<decltype(QuoteIntent{}.bid_price), Px>);
    static_assert(std::is_same_v<decltype(QuoteIntent{}.bid_quantity), Qty>);
    static_assert(std::is_same_v<decltype(InventoryView{}.position), Qty>);
    static_assert(std::is_integral_v<Px::Raw>);
    SUCCEED();
}

// ===========================================================================
// The engine depends on the interface, never on a concrete strategy
// ===========================================================================

TEST(StrategyBoundary, RuntimeIsWrittenAgainstTheInterfaceOnly) {
    static_assert(std::is_same_v<StrategyPtr, std::unique_ptr<IStrategy>>);
    static_assert(std::is_abstract_v<IStrategy>,
                  "IStrategy must stay abstract or the engine could depend on a default");
    static_assert(std::has_virtual_destructor_v<IStrategy>);
    static_assert(!std::is_copy_constructible_v<IStrategy>,
                  "copying a strategy would duplicate its internal state silently");
    SUCCEED();
}

TEST(StrategyBoundary, AnyImplementationIsAcceptedByTheRuntime) {
    // Three unrelated implementations, one runtime, no branch that names any of
    // them.
    ManualClock clock(millis(1'000), 0);
    StrategyRuntimeConfig config;

    StrategyRuntime a(config, std::make_unique<test::WellBehavedStrategy>(), clock);
    StrategyRuntime b(config, std::make_unique<test::ThrowingStrategy>(), clock);
    StrategyRuntime c(config, std::make_unique<test::ScriptedStrategy>(), clock);

    EXPECT_EQ(a.state(), StrategyState::Created);
    EXPECT_EQ(b.state(), StrategyState::Created);
    EXPECT_EQ(c.state(), StrategyState::Created);
}

TEST(StrategyBoundary, StrategySelectionIsDataNotCode) {
    // Choosing a strategy is a string lookup. There is no switch statement
    // anywhere that enumerates the available strategies.
    const auto listed = StrategyRegistry::instance().list();
    ASSERT_FALSE(listed.empty());
    for (const auto& descriptor : listed) {
        const auto created = StrategyRegistry::instance().create(descriptor.name);
        EXPECT_TRUE(created.is_ok()) << descriptor.name;
    }
}

// ===========================================================================
// The direction of the pipeline
// ===========================================================================

TEST(StrategyBoundary, TheOnlyOutputIsAValueTheRuntimeInspects) {
    // `evaluate` returns a result the caller must look at; there is no path by
    // which a strategy's output reaches anything without passing validation
    // first, because the strategy hands its intent to the runtime and to
    // nothing else.
    static_assert(std::is_same_v<decltype(std::declval<StrategyRuntime&>().evaluate(
                                     std::declval<const StrategyContext&>())),
                                 EvaluationResult>);
    // And an unaccepted result is always a Pull, never a stale quote.
    ManualClock clock(millis(1'000), 0);
    StrategyRuntimeConfig config;
    StrategyRuntime runtime(config, std::make_unique<test::WellBehavedStrategy>(), clock);
    StrategyContext context;
    const EvaluationResult r = runtime.evaluate(context);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.intent.action, QuoteAction::Pull);
}

TEST(StrategyBoundary, EveryEnumHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(StrategyState::Faulted); ++i) {
        EXPECT_NE(to_string(static_cast<StrategyState>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(IntentRejection::InstrumentNotLoaded);
         ++i) {
        EXPECT_NE(to_string(static_cast<IntentRejection>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(SkipReason::QuotingDisabled); ++i) {
        EXPECT_NE(to_string(static_cast<SkipReason>(i)), "UNKNOWN") << int{i};
    }
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(IntentReason::Custom); ++i) {
        EXPECT_NE(to_string(static_cast<IntentReason>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::strategy
