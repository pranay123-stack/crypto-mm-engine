/// \file test_strategy_to_actions.cpp
/// The pipeline as far as it currently goes:
///
///     Strategy -> QuoteIntent -> Quote Manager -> OrderAction
///
/// and stops there. Nothing is executed; the mock exchange is used only to
/// supply venue capabilities and instrument rules, which is the boundary at
/// which this phase ends.

#include <gtest/gtest.h>

#include <memory>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/quote/QuoteManager.hpp"
#include "mm/strategy/StrategyRegistry.hpp"
#include "mm/strategy/StrategyRuntime.hpp"
#include "support/QuoteFixtures.hpp"

namespace mm {
namespace {

using quote::OrderActionType;
using quote::QuoteManager;
using quote::QuoteManagerInput;
using quote::QuoteManagerResult;
using quote::QuoteSlot;
using strategy::StrategyRuntime;

class PipelineTest : public ::testing::Test {
protected:
    PipelineTest() : clock(millis(1'000), 0), spec(test::btc_spec()) {
        // Capabilities come from a real adapter implementation rather than a
        // hand-written struct, so the manager is driven by what a venue
        // actually declares.
        caps = exchange::mock::permissive_mock_capabilities();
    }

    void SetUp() override {
        auto created = strategy::StrategyRegistry::instance().create("reference_mm_v1");
        ASSERT_TRUE(created.is_ok()) << created.status().to_string();

        strategy::StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "5.0");
        init.params.set("quote_size", "0.001");

        strategy::StrategyRuntimeConfig config;
        runtime = std::make_unique<StrategyRuntime>(config, std::move(created.value()), clock);
        ASSERT_TRUE(runtime->initialize(init).is_ok());
        runtime->start();

        manager = std::make_unique<QuoteManager>(StrategyName("reference_mm_v1"),
                                                 quote::QuoteManagerConfig{}, clock);
    }

    strategy::StrategyContext market_context() {
        strategy::StrategyContext c;
        c.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Px::parse("60000.00", c.bbo.bid_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.bid_qty));
        EXPECT_TRUE(Px::parse("60000.10", c.bbo.ask_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.ask_qty));
        c.sequence = 500;
        c.now_ns = clock.steady();
        c.instrument = &spec;
        c.session_state = exchange::SessionState::Ready;
        c.trigger = strategy::TriggerReason::BboChange;
        EXPECT_TRUE(Qty::parse("0.5", c.inventory.position_limit));
        return c;
    }

    QuoteManagerInput manager_input() {
        QuoteManagerInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.capabilities = &caps;
        in.system_ready = true;
        return in;
    }

    ManualClock clock;
    InstrumentSpec spec;
    exchange::ExchangeCapabilities caps;
    std::unique_ptr<StrategyRuntime> runtime;
    std::unique_ptr<QuoteManager> manager;
};

TEST_F(PipelineTest, StrategyIntentBecomesOrderActionsAndStops) {
    const strategy::EvaluationResult evaluated = runtime->evaluate(market_context());
    ASSERT_TRUE(evaluated.accepted) << to_string(evaluated.rejection);
    ASSERT_EQ(evaluated.intent.action, strategy::QuoteAction::Quote);

    const QuoteManagerResult r = manager->evaluate(evaluated.intent, manager_input());
    ASSERT_TRUE(r.intent_accepted) << to_string(r.rejection);
    EXPECT_EQ(r.count_of(OrderActionType::New), 2U);

    // The actions carry the strategy's identity end to end, which is what makes
    // a fill attributable later.
    for (std::uint8_t i = 0; i < r.action_count; ++i) {
        EXPECT_EQ(r.actions[i].identity.name, StrategyName("reference_mm_v1"));
        EXPECT_EQ(r.actions[i].generation, evaluated.intent.generation);
        EXPECT_EQ(r.actions[i].market_sequence, 500U);
        // And nothing beyond this point exists yet: an action is a request for
        // a state change, not an order.
        EXPECT_TRUE(r.actions[i].target_client_order_id.empty())
            << "the quote manager does not mint order ids";
    }
}

TEST_F(PipelineTest, GenerationsAdvanceThroughTheWholePipeline) {
    QuoteManagerInput in = manager_input();
    std::uint64_t previous = 0;
    for (int i = 0; i < 5; ++i) {
        const strategy::EvaluationResult evaluated = runtime->evaluate(market_context());
        ASSERT_TRUE(evaluated.accepted);
        EXPECT_GT(evaluated.intent.generation, previous);
        previous = evaluated.intent.generation;

        const QuoteManagerResult r = manager->evaluate(evaluated.intent, in);
        EXPECT_EQ(r.generation, evaluated.intent.generation);
        // The working state deliberately never catches up here. The manager
        // must still not create one order per cycle: it waits for the request
        // it already issued, even though every cycle carries a newer generation.
        if (i > 0) {
            EXPECT_EQ(r.count_of(OrderActionType::New), 0U) << "cycle " << i;
            EXPECT_EQ(r.bid.outcome, quote::SlotOutcome::AwaitingPending) << "cycle " << i;
        }
    }
    EXPECT_EQ(manager->last_generation(), previous);
}

TEST_F(PipelineTest, AGatedStrategyWithdrawsQuotesThroughTheManager) {
    // Place quotes first.
    const strategy::EvaluationResult first = runtime->evaluate(market_context());
    ASSERT_TRUE(first.accepted);
    QuoteManagerInput in = manager_input();
    static_cast<void>(manager->evaluate(first.intent, in));

    // Now they exist.
    in.working.slot(QuoteSlot::Bid) =
        test::resting(QuoteSlot::Bid, first.intent.bid_price.to_string().c_str(), "0.001");
    in.working.slot(QuoteSlot::Ask) =
        test::resting(QuoteSlot::Ask, first.intent.ask_price.to_string().c_str(), "0.001");

    // The market goes stale, so the strategy is not even consulted.
    strategy::StrategyContext stale = market_context();
    stale.data_age_ns = millis(600);
    const strategy::EvaluationResult gated = runtime->evaluate(stale);
    EXPECT_FALSE(gated.accepted);
    EXPECT_EQ(gated.intent.action, strategy::QuoteAction::Pull);

    // And the Pull becomes cancels, not silence.
    const QuoteManagerResult r = manager->evaluate(gated.intent, in);
    EXPECT_EQ(r.count_of(OrderActionType::Cancel), 2U);
}

TEST_F(PipelineTest, AFaultedStrategyResultsInCancelsNotStaleQuotes) {
    const strategy::EvaluationResult first = runtime->evaluate(market_context());
    ASSERT_TRUE(first.accepted);
    QuoteManagerInput in = manager_input();
    static_cast<void>(manager->evaluate(first.intent, in));
    in.working.slot(QuoteSlot::Bid) =
        test::resting(QuoteSlot::Bid, first.intent.bid_price.to_string().c_str(), "0.001");

    // The operator kills quoting; the manager withdraws what it owns.
    const QuoteManagerResult killed =
        manager->disable_all_quotes(in, quote::ActionReason::OperatorDisabled);
    EXPECT_EQ(killed.count_of(OrderActionType::Cancel), 1U);
    EXPECT_EQ(killed.bid.reason, quote::ActionReason::OperatorDisabled);
}

TEST_F(PipelineTest, TheReferenceStrategysQuotesSurviveVenueValidation) {
    // The strategy rounds outward and the manager re-validates against the same
    // Phase 3 rules the venue would apply, so a well-behaved strategy should
    // never see BlockedInvalid.
    for (int i = 0; i < 20; ++i) {
        strategy::StrategyContext c = market_context();
        c.inventory.position = Qty::from_raw(i * 1'000'000);
        const strategy::EvaluationResult evaluated = runtime->evaluate(c);
        ASSERT_TRUE(evaluated.accepted);

        QuoteManager fresh(StrategyName("reference_mm_v1"), quote::QuoteManagerConfig{}, clock);
        const QuoteManagerResult r = fresh.evaluate(evaluated.intent, manager_input());
        EXPECT_NE(r.bid.outcome, quote::SlotOutcome::BlockedInvalid)
            << "iteration " << i << " bid " << to_string(r.bid.invalid_reason);
        EXPECT_NE(r.ask.outcome, quote::SlotOutcome::BlockedInvalid)
            << "iteration " << i << " ask " << to_string(r.ask.invalid_reason);
    }
}

}  // namespace
}  // namespace mm
