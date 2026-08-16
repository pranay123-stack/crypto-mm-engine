/// \file test_strategy_failure.cpp
/// What happens when the plug-in misbehaves.
///
/// The governing rule: a strategy failure must never take down the process, and
/// must never leave the engine quoting on the strength of a decision it no
/// longer trusts. Every path below ends in "stop quoting", not "carry on with
/// the last quote".

#include <gtest/gtest.h>

#include <memory>

#include "mm/strategy/StrategyRuntime.hpp"
#include "support/TestStrategies.hpp"

namespace mm::strategy {
namespace {

InstrumentSpec btc_spec() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse("0.01", s.tick_size));
    EXPECT_TRUE(Qty::parse("0.00001", s.lot_size));
    EXPECT_TRUE(Qty::parse("1000", s.max_qty));
    EXPECT_TRUE(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

class StrategyFailureTest : public ::testing::Test {
protected:
    StrategyFailureTest() : clock(millis(1'000), 0), spec(btc_spec()) {}

    static StrategyRuntimeConfig make_config() {
        StrategyRuntimeConfig c;
        c.evaluation_mode = EvaluationMode::OnBboChangeAndTimer;
        c.max_evaluation_latency_ns = micros(50);
        c.max_consecutive_budget_violations = 3;
        c.max_consecutive_invalid_outputs = 3;
        return c;
    }

    template <class T>
    T* build(StrategyRuntimeConfig config = make_config()) {
        auto owned = std::make_unique<T>();
        T* raw = owned.get();
        runtime = std::make_unique<StrategyRuntime>(config, std::move(owned), clock);
        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = raw->identity();
        EXPECT_TRUE(runtime->initialize(init).is_ok());
        runtime->start();
        return raw;
    }

    StrategyContext make_context() {
        StrategyContext c;
        c.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Px::parse("60000.00", c.bbo.bid_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.bid_qty));
        EXPECT_TRUE(Px::parse("60000.10", c.bbo.ask_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.ask_qty));
        c.sequence = 500;
        c.now_ns = clock.steady();
        c.instrument = &spec;
        c.session_state = SessionState::Ready;
        c.trigger = TriggerReason::BboChange;
        EXPECT_TRUE(Qty::parse("0.5", c.inventory.position_limit));
        return c;
    }

    ManualClock clock;
    InstrumentSpec spec;
    std::unique_ptr<StrategyRuntime> runtime;
};

// ===========================================================================
// Exceptions
// ===========================================================================

TEST_F(StrategyFailureTest, AThrowingStrategyFaultsInsteadOfCrashing) {
    build<test::ThrowingStrategy>();

    // Must not propagate: an exception escaping into the trading thread would
    // take down a process holding live orders.
    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(runtime->state(), StrategyState::Faulted);
    EXPECT_EQ(r.skip, SkipReason::Faulted);
    EXPECT_EQ(r.intent.action, QuoteAction::Pull) << "a faulted strategy must stop quoting";
    EXPECT_EQ(runtime->metrics().exceptions, 1U);
    EXPECT_NE(runtime->fault_reason().find("deliberate strategy failure"),
              std::string_view::npos);
}

TEST_F(StrategyFailureTest, ANonStandardExceptionIsAlsoContained) {
    build<test::ThrowingNonStandardStrategy>();
    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(runtime->state(), StrategyState::Faulted);
    EXPECT_EQ(runtime->metrics().exceptions, 1U);
}

TEST_F(StrategyFailureTest, AFaultedStrategyStaysFaultedAndKeepsNotQuoting) {
    build<test::ThrowingStrategy>();
    static_cast<void>(runtime->evaluate(make_context()));
    ASSERT_EQ(runtime->state(), StrategyState::Faulted);

    // A strategy that faulted once will fault again on the same input; retrying
    // it automatically would produce a component that quotes intermittently
    // after being judged broken.
    for (int i = 0; i < 5; ++i) {
        const EvaluationResult r = runtime->evaluate(make_context());
        EXPECT_FALSE(r.accepted);
        EXPECT_EQ(r.skip, SkipReason::Faulted);
        EXPECT_EQ(r.intent.action, QuoteAction::Pull);
    }
    runtime->start();
    EXPECT_EQ(runtime->state(), StrategyState::Faulted) << "start() must not clear a fault";
    EXPECT_TRUE(needs_operator_action(runtime->state()));
}

TEST_F(StrategyFailureTest, ClearingAFaultRequiresAnExplicitOperatorAction) {
    auto* strategy = build<test::ScriptedStrategy>();
    // Drive it to faulted through invalid output.
    QuoteIntent bad;
    bad.action = QuoteAction::Quote;
    bad.quote_bid = true;
    bad.bid_price = Px::zero();
    strategy->next = bad;
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(runtime->evaluate(make_context()));
    }
    ASSERT_EQ(runtime->state(), StrategyState::Faulted);

    ASSERT_TRUE(runtime->clear_fault().is_ok());
    EXPECT_EQ(runtime->state(), StrategyState::Initialized);
    EXPECT_TRUE(runtime->fault_reason().empty());

    // And it works again once the strategy behaves.
    strategy->next = QuoteIntent::pull(IntentReason::Normal);
    runtime->start();
    EXPECT_TRUE(runtime->evaluate(make_context()).accepted);
}

TEST_F(StrategyFailureTest, ClearingAFaultOnAHealthyStrategyIsRefused) {
    build<test::WellBehavedStrategy>();
    EXPECT_TRUE(runtime->clear_fault().is_error());
}

TEST_F(StrategyFailureTest, AThrowingNotificationFaultsRatherThanEscaping) {
    class ThrowOnFill final : public test::TestStrategyBase {
    public:
        ThrowOnFill() : TestStrategyBase("test_throw_fill_v1", 1) {}
        [[nodiscard]] QuoteIntent on_market_update(const StrategyContext& c) override {
            return good_quote(c);
        }
        void on_fill(const StrategyFill&) override { throw std::runtime_error("fill blew up"); }
    };
    build<ThrowOnFill>();
    StrategyFill fill;
    fill.symbol = Symbol("BTCUSDT");
    runtime->on_fill(fill);
    EXPECT_EQ(runtime->state(), StrategyState::Faulted);
    EXPECT_EQ(runtime->evaluate(make_context()).skip, SkipReason::Faulted);
}

// ===========================================================================
// Latency budget
// ===========================================================================

TEST_F(StrategyFailureTest, ASingleSlowEvaluationIsCountedButTolerated) {
    auto* strategy = build<test::SlowStrategy>();
    strategy->clock = &clock;
    strategy->delay = micros(200);  // budget is 50us

    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_TRUE(r.accepted) << "one slow evaluation is a hiccup, not a fault";
    EXPECT_EQ(runtime->metrics().budget_violations, 1U);
    EXPECT_EQ(runtime->state(), StrategyState::Running);
    EXPECT_GE(r.evaluation_ns, micros(200));
}

TEST_F(StrategyFailureTest, PersistentSlownessFaultsTheStrategy) {
    auto* strategy = build<test::SlowStrategy>();
    strategy->clock = &clock;
    strategy->delay = micros(200);

    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));
    EXPECT_EQ(runtime->state(), StrategyState::Running);

    // Persistently slow is a different failure from a hiccup, and one the
    // trading thread cannot absorb.
    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_EQ(runtime->state(), StrategyState::Faulted);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.intent.action, QuoteAction::Pull);
    EXPECT_EQ(runtime->metrics().budget_violations, 3U);
}

TEST_F(StrategyFailureTest, AFastEvaluationResetsTheViolationStreak) {
    auto* strategy = build<test::SlowStrategy>();
    strategy->clock = &clock;
    strategy->delay = micros(200);
    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));

    strategy->delay = 0;  // back inside budget
    static_cast<void>(runtime->evaluate(make_context()));
    strategy->delay = micros(200);
    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));
    // Streak restarted, so it takes three fresh violations to fault.
    EXPECT_EQ(runtime->state(), StrategyState::Running);
}

TEST_F(StrategyFailureTest, LatencyIsRecordedForTheWatchdog) {
    auto* strategy = build<test::SlowStrategy>();
    strategy->clock = &clock;
    strategy->delay = micros(10);
    for (int i = 0; i < 5; ++i) {
        static_cast<void>(runtime->evaluate(make_context()));
    }
    const LatencySummary s = runtime->latency_summary();
    EXPECT_EQ(s.count, 5U);
    EXPECT_GE(s.max_ns, micros(10));
    EXPECT_GE(runtime->metrics().max_evaluation_ns, micros(10));
}

// ===========================================================================
// Invalid output
// ===========================================================================

TEST_F(StrategyFailureTest, PersistentInvalidOutputFaultsTheStrategy) {
    auto* strategy = build<test::ScriptedStrategy>();
    QuoteIntent bad;
    bad.action = QuoteAction::Quote;
    bad.quote_bid = true;
    bad.bid_price = Px::zero();
    ASSERT_TRUE(Qty::parse("0.001", bad.bid_quantity));
    strategy->next = bad;

    for (int i = 0; i < 2; ++i) {
        const EvaluationResult r = runtime->evaluate(make_context());
        EXPECT_FALSE(r.accepted);
        EXPECT_EQ(runtime->state(), StrategyState::Running) << "one bad quote is an edge case";
    }
    static_cast<void>(runtime->evaluate(make_context()));
    // A stream of them means it cannot be trusted to produce a quote at all.
    EXPECT_EQ(runtime->state(), StrategyState::Faulted);
    EXPECT_EQ(runtime->metrics().intents_rejected, 3U);
}

TEST_F(StrategyFailureTest, AValidQuoteResetsTheInvalidStreak) {
    auto* strategy = build<test::ScriptedStrategy>();
    QuoteIntent bad;
    bad.action = QuoteAction::Quote;
    bad.quote_bid = true;
    bad.bid_price = Px::zero();
    ASSERT_TRUE(Qty::parse("0.001", bad.bid_quantity));

    strategy->next = bad;
    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));

    strategy->next = QuoteIntent::pull(IntentReason::Normal);
    ASSERT_TRUE(runtime->evaluate(make_context()).accepted);

    strategy->next = bad;
    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));
    EXPECT_EQ(runtime->state(), StrategyState::Running);
}

TEST_F(StrategyFailureTest, AnInvalidIntentNeverFallsBackToTheLastGoodQuote) {
    auto* strategy = build<test::ScriptedStrategy>();

    QuoteIntent good;
    good.action = QuoteAction::Quote;
    good.quote_bid = true;
    ASSERT_TRUE(Px::parse("60000.00", good.bid_price));
    ASSERT_TRUE(Qty::parse("0.001", good.bid_quantity));
    strategy->next = good;
    const EvaluationResult first = runtime->evaluate(make_context());
    ASSERT_TRUE(first.accepted);

    QuoteIntent bad = good;
    bad.bid_price = Px::zero();
    strategy->next = bad;
    const EvaluationResult second = runtime->evaluate(make_context());

    // Standing on the old quote would be holding a position nobody decided to
    // hold: the strategy's current opinion is unknown, not "the same as before".
    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(second.intent.action, QuoteAction::Pull);
    EXPECT_FALSE(second.intent.quote_bid);
    EXPECT_TRUE(second.intent.bid_price.is_zero());
}

// ===========================================================================
// Reset
// ===========================================================================

TEST_F(StrategyFailureTest, ResetClearsStrategyStateWithoutDestroyingIt) {
    auto* strategy = build<test::WellBehavedStrategy>();
    StrategyFill fill;
    fill.symbol = Symbol("BTCUSDT");
    runtime->on_fill(fill);
    ASSERT_EQ(strategy->fills, 1U);

    runtime->reset_strategy();
    EXPECT_EQ(strategy->resets, 1U);
    EXPECT_EQ(runtime->state(), StrategyState::Running) << "reset is not a fault";
    EXPECT_TRUE(runtime->evaluate(make_context()).accepted);
}

}  // namespace
}  // namespace mm::strategy
