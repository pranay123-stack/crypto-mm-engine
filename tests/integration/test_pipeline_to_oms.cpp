/// \file test_pipeline_to_oms.cpp
/// The pipeline as far as Phase 8 takes it:
///
///     Strategy -> QuoteIntent -> Quote Manager -> OrderAction
///              -> Risk -> ApprovedAction -> OMS -> execution adapter
///              -> execution events -> OMS -> working state, exposure
///
/// The loop closes here: what the OMS learns from the venue is what the quote
/// manager and risk see on the next cycle. Driven through the real interfaces
/// with a mock execution adapter, so it is deterministic and offline.

#include <gtest/gtest.h>

#include <memory>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "mm/quote/QuoteManager.hpp"
#include "mm/risk/RiskEngine.hpp"
#include "mm/strategy/StrategyRegistry.hpp"
#include "mm/strategy/StrategyRuntime.hpp"
#include "support/QuoteFixtures.hpp"

namespace mm {
namespace {

using exchange::mock::MockExchangeExecution;
using oms::OrderManager;
using oms::OrderState;
using quote::OrderActionType;
using quote::QuoteManager;
using quote::QuoteManagerInput;
using quote::QuoteManagerResult;
using strategy::StrategyRuntime;

constexpr const char* kStrategy = "reference_mm_v1";

class FullPipelineTest : public ::testing::Test {
protected:
    FullPipelineTest()
        : clock(millis(1'000), 0),
          spec(test::btc_spec()),
          venue(clock),
          risk(build_limits(), clock),
          oms(build_oms_config(), journal, clock) {
        caps = exchange::mock::permissive_mock_capabilities();
    }

    void SetUp() override {
        auto created = strategy::StrategyRegistry::instance().create(kStrategy);
        ASSERT_TRUE(created.is_ok()) << created.status().to_string();

        strategy::StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "5.0");
        init.params.set("quote_size", "0.010");

        runtime = std::make_unique<StrategyRuntime>(strategy::StrategyRuntimeConfig{},
                                                   std::move(created.value()), clock);
        ASSERT_TRUE(runtime->initialize(init).is_ok());
        runtime->start();

        manager = std::make_unique<QuoteManager>(StrategyName(kStrategy),
                                                 quote::QuoteManagerConfig{}, clock);
        ASSERT_TRUE(risk.arm().is_ok());
        ASSERT_TRUE(venue.start(oms).is_ok());
        oms.attach_execution(venue);
        static_cast<void>(oms.process_events());
    }

    static oms::OmsConfig build_oms_config() {
        oms::OmsConfig c;
        static_cast<void>(c.client_id_prefix.assign("mm"));
        c.session_id = 7;
        c.max_orders = 32;
        c.new_request_timeout_ns = seconds(5);
        c.cancel_request_timeout_ns = seconds(5);
        c.replace_request_timeout_ns = seconds(5);
        return c;
    }

    static risk::RiskLimits build_limits() {
        risk::SymbolLimits btc;
        btc.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Qty::parse("10", btc.max_position));
        EXPECT_TRUE(Notional::parse("1000000", btc.max_position_notional));
        EXPECT_TRUE(Qty::parse("5", btc.max_order_quantity));
        EXPECT_TRUE(Notional::parse("500000", btc.max_order_notional));
        EXPECT_TRUE(Qty::parse("20", btc.max_working_exposure));
        EXPECT_TRUE(Qty::parse("10", btc.max_side_exposure));
        btc.max_open_orders = 16;
        btc.price_band_bps = 5'000;

        risk::RiskLimits l;
        l.global.max_market_data_age_ns = seconds(60);
        l.global.max_position_age_ns = seconds(60);
        EXPECT_TRUE(l.add(btc));
        return l;
    }

    strategy::StrategyContext market_context() {
        strategy::StrategyContext c;
        c.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Px::parse("60000.00", c.bbo.bid_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.bid_qty));
        EXPECT_TRUE(Px::parse("60000.10", c.bbo.ask_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.ask_qty));
        c.sequence = ++sequence;
        c.now_ns = clock.steady();
        c.instrument = &spec;
        c.session_state = exchange::SessionState::Ready;
        c.trigger = strategy::TriggerReason::BboChange;
        EXPECT_TRUE(Qty::parse("5", c.inventory.position_limit));
        return c;
    }

    /// One full cycle. The working state and exposure fed back in come from
    /// the OMS -- nothing in this loop keeps a second opinion about what is
    /// resting.
    std::size_t cycle() {
        const strategy::EvaluationResult evaluated = runtime->evaluate(market_context());
        EXPECT_TRUE(evaluated.accepted) << to_string(evaluated.rejection);

        QuoteManagerInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.capabilities = &caps;
        in.system_ready = true;
        in.working = oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));

        const QuoteManagerResult quoted = manager->evaluate(evaluated.intent, in);
        std::size_t submitted = 0;
        for (std::uint8_t i = 0; i < quoted.action_count; ++i) {
            risk::RiskInput risk_in;
            risk_in.symbol = Symbol("BTCUSDT");
            risk_in.instrument = &spec;
            risk_in.position.valid = true;
            risk_in.position.symbol = Symbol("BTCUSDT");
            risk_in.position.sequence = sequence;
            risk_in.exposure = oms.exposure(Symbol("BTCUSDT"));
            risk_in.bbo = market_context().bbo;
            risk_in.system_ready = true;
            static_cast<void>(risk_in.expected_owner.assign(kStrategy));

            const risk::RiskDecision decision = risk.evaluate(quoted.actions[i], risk_in);
            if (!decision.is_approved()) {
                continue;
            }
            // The only way an action reaches the OMS.
            if (oms.submit(decision.approval).accepted) {
                ++submitted;
            }
        }
        clock.advance(millis(2));
        static_cast<void>(venue.pump());
        static_cast<void>(oms.process_events());
        return submitted;
    }

    ManualClock clock;
    InstrumentSpec spec;
    exchange::ExchangeCapabilities caps;
    MockExchangeExecution venue;
    risk::RiskEngine risk;
    oms::InMemoryOrderJournal journal;
    OrderManager oms;
    std::unique_ptr<StrategyRuntime> runtime;
    std::unique_ptr<QuoteManager> manager;
    std::uint64_t sequence = 500;
};

TEST_F(FullPipelineTest, QuotesReachTheVenueAndComeBackAsWorkingState) {
    EXPECT_EQ(cycle(), 2U) << "one order per side";

    // The OMS is now the single source of truth for what is resting, and the
    // quote manager reads it rather than remembering what it asked for.
    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    EXPECT_TRUE(state.slot(quote::QuoteSlot::Bid).present);
    EXPECT_TRUE(state.slot(quote::QuoteSlot::Ask).present);
    EXPECT_EQ(oms.live_order_count(), 2U);

    // Exposure is derived from the same records risk will consult next cycle.
    const risk::ExposureSnapshot exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_TRUE(exposure.determinate);
    EXPECT_EQ(exposure.working_buy.to_string(), "0.01");
    EXPECT_EQ(exposure.working_sell.to_string(), "0.01");
}

TEST_F(FullPipelineTest, SteadyStateStopsCreatingOrders) {
    EXPECT_EQ(cycle(), 2U);
    // With the book unchanged the resting orders already express the intent.
    // A pipeline that re-quoted every cycle would churn the venue for nothing.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(cycle(), 0U) << "cycle " << i;
    }
    EXPECT_EQ(oms.live_order_count(), 2U);
    EXPECT_EQ(oms.metrics().new_requests, 2U);
}

TEST_F(FullPipelineTest, AFillFlowsBackIntoExposure) {
    ASSERT_EQ(cycle(), 2U);
    const quote::WorkingQuoteState before =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const ClientOrderId bid_id = before.slot(quote::QuoteSlot::Bid).client_order_id;
    ASSERT_FALSE(bid_id.empty());

    Px px;
    Qty half;
    ASSERT_TRUE(Px::parse("59970.00", px));
    ASSERT_TRUE(Qty::parse("0.005", half));
    ASSERT_TRUE(venue.deliver_fill(bid_id, px, half, Liquidity::Maker, TradeId("F1")).is_ok());
    static_cast<void>(oms.process_events());

    // Working exposure falls by exactly what filled -- computed from the
    // remaining quantity, never from what was originally submitted.
    const risk::ExposureSnapshot exposure = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_EQ(exposure.working_buy.to_string(), "0.005");
    EXPECT_EQ(oms.metrics().partial_fills, 1U);
}

TEST_F(FullPipelineTest, RiskRefusalStopsAnActionBeforeItBecomesAnOrder) {
    // Disarm risk. Every exposure-adding action must now die at the boundary,
    // and the OMS must never see one.
    risk.kill(risk::RiskReason::RiskKilled);
    const std::size_t submitted = cycle();

    EXPECT_EQ(submitted, 0U);
    EXPECT_EQ(oms.order_count(), 0U);
    EXPECT_EQ(oms.metrics().new_requests, 0U);
}

TEST_F(FullPipelineTest, TheSameOmsServesAnySymbolAndStrategyIdentity) {
    ASSERT_EQ(cycle(), 2U);
    // A different strategy sees none of our slots as its own, which is what
    // makes one OMS safe to share.
    const quote::WorkingQuoteState other =
        oms.working_state(Symbol("BTCUSDT"), StrategyName("another_strategy_v1"));
    EXPECT_FALSE(other.slot(quote::QuoteSlot::Bid).present);
    EXPECT_EQ(other.foreign_order_count, 2U);

    // And a symbol we never traded reports nothing rather than everything.
    const risk::ExposureSnapshot none = oms.exposure(Symbol("ETHUSDT"));
    EXPECT_TRUE(none.working_buy.is_zero());
    EXPECT_TRUE(none.working_sell.is_zero());
}

}  // namespace
}  // namespace mm
