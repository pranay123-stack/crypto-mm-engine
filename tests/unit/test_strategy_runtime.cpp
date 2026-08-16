#include <gtest/gtest.h>

#include <memory>

#include "mm/strategy/StrategyRuntime.hpp"
#include "support/TestStrategies.hpp"

namespace mm::strategy {
namespace {

using test::ScriptedStrategy;
using test::WellBehavedStrategy;

InstrumentSpec btc_spec() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse("0.01", s.tick_size));
    EXPECT_TRUE(Qty::parse("0.00001", s.lot_size));
    EXPECT_TRUE(Qty::parse("0.00001", s.min_qty));
    EXPECT_TRUE(Qty::parse("1000", s.max_qty));
    EXPECT_TRUE(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

class RuntimeTest : public ::testing::Test {
protected:
    RuntimeTest() : clock(millis(1'000), 0), spec(btc_spec()) {}

    static StrategyRuntimeConfig make_config() {
        StrategyRuntimeConfig c;
        c.evaluation_mode = EvaluationMode::OnBboChangeAndTimer;
        c.max_evaluation_latency_ns = micros(50);
        c.max_consecutive_budget_violations = 3;
        c.max_consecutive_invalid_outputs = 3;
        c.max_data_age_ns = millis(500);
        return c;
    }

    /// Builds a running runtime around `strategy`, returning the raw pointer so
    /// a test can still script it.
    template <class T>
    T* build(StrategyRuntimeConfig config = make_config()) {
        auto owned = std::make_unique<T>();
        T* raw = owned.get();
        runtime = std::make_unique<StrategyRuntime>(config, std::move(owned), clock);

        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = raw->identity();
        init.identity.config_generation = 7;
        EXPECT_TRUE(runtime->initialize(init).is_ok());
        runtime->start();
        return raw;
    }

    StrategyContext make_context(TriggerReason trigger = TriggerReason::BboChange) {
        StrategyContext c;
        c.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Px::parse("60000.00", c.bbo.bid_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.bid_qty));
        EXPECT_TRUE(Px::parse("60000.10", c.bbo.ask_px));
        EXPECT_TRUE(Qty::parse("1", c.bbo.ask_qty));
        c.sequence = 500;
        c.now_ns = clock.steady();
        c.recv_ns = clock.steady();
        c.data_age_ns = 0;
        c.instrument = &spec;
        c.session_state = SessionState::Ready;
        c.trigger = trigger;
        c.quoting_enabled = true;
        c.config_generation = 7;
        EXPECT_TRUE(Qty::parse("0.5", c.inventory.position_limit));
        return c;
    }

    ManualClock clock;
    InstrumentSpec spec;
    std::unique_ptr<StrategyRuntime> runtime;
};

// ===========================================================================
// Lifecycle and initialization
// ===========================================================================

TEST_F(RuntimeTest, StartsInCreatedAndCannotEvaluate) {
    auto strategy = std::make_unique<WellBehavedStrategy>();
    StrategyRuntime rt(make_config(), std::move(strategy), clock);
    EXPECT_EQ(rt.state(), StrategyState::Created);
    const EvaluationResult r = rt.evaluate(make_context());
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.skip, SkipReason::NotInitialized);
}

TEST_F(RuntimeTest, InitializeThenStart) {
    build<WellBehavedStrategy>();
    EXPECT_EQ(runtime->state(), StrategyState::Running);
    EXPECT_EQ(runtime->identity().name, StrategyName("test_good_v1"));
    EXPECT_EQ(runtime->identity().version, 1);
}

TEST_F(RuntimeTest, IdentityMismatchIsRefusedAtInitialization) {
    auto strategy = std::make_unique<test::MisidentifiedStrategy>();
    StrategyRuntime rt(make_config(), std::move(strategy), clock);

    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = spec;
    static_cast<void>(init.identity.name.assign("something_else_v1"));
    init.identity.version = 1;

    // A strategy running under another's name misattributes every fill it
    // produces; caught at startup rather than discovered in the journal.
    const Status s = rt.initialize(init);
    ASSERT_TRUE(s.is_error());
    EXPECT_NE(s.message().find("identity mismatch"), std::string_view::npos);
    EXPECT_EQ(rt.state(), StrategyState::Created) << "a refused strategy must not be initialized";
}

TEST_F(RuntimeTest, InitializationRequiresAnInstrumentSpec) {
    auto strategy = std::make_unique<WellBehavedStrategy>();
    StrategyRuntime rt(make_config(), std::move(strategy), clock);
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.identity.name = StrategyName("test_good_v1");
    init.identity.version = 1;
    // No instrument: without a tick and lot size nothing can be validated, and
    // a strategy must never guess them.
    EXPECT_TRUE(rt.initialize(init).is_error());
}

TEST_F(RuntimeTest, PauseAndResume) {
    build<WellBehavedStrategy>();
    runtime->pause();
    EXPECT_EQ(runtime->state(), StrategyState::Paused);
    EXPECT_EQ(runtime->evaluate(make_context()).skip, SkipReason::Paused);

    runtime->resume();
    EXPECT_EQ(runtime->state(), StrategyState::Running);
    EXPECT_TRUE(runtime->evaluate(make_context()).accepted);
}

TEST_F(RuntimeTest, StopIsTerminalWithoutReinitialization) {
    build<WellBehavedStrategy>();
    runtime->stop();
    EXPECT_EQ(runtime->evaluate(make_context()).skip, SkipReason::Stopped);
    runtime->start();
    EXPECT_EQ(runtime->state(), StrategyState::Stopped) << "Stopped -> Running must be illegal";
}

TEST_F(RuntimeTest, LifecycleTransitionTable) {
    EXPECT_TRUE(is_legal_transition(StrategyState::Created, StrategyState::Initialized));
    EXPECT_TRUE(is_legal_transition(StrategyState::Initialized, StrategyState::Running));
    EXPECT_TRUE(is_legal_transition(StrategyState::Running, StrategyState::Paused));
    EXPECT_FALSE(is_legal_transition(StrategyState::Created, StrategyState::Running))
        << "a strategy must not run before its parameters were accepted";
    EXPECT_FALSE(is_legal_transition(StrategyState::Stopped, StrategyState::Running));
    // Faulted is reachable from anywhere and leaves only via re-initialization.
    for (const auto from : {StrategyState::Created, StrategyState::Initialized,
                            StrategyState::Running, StrategyState::Paused}) {
        EXPECT_TRUE(is_legal_transition(from, StrategyState::Faulted));
    }
    EXPECT_FALSE(is_legal_transition(StrategyState::Faulted, StrategyState::Running));
}

// ===========================================================================
// Gating (§6) — the strategy must not quote when conditions are wrong
// ===========================================================================

TEST_F(RuntimeTest, QuotesOnAHealthyMarket) {
    auto* strategy = build<WellBehavedStrategy>();
    const EvaluationResult r = runtime->evaluate(make_context());
    ASSERT_TRUE(r.accepted) << to_string(r.rejection);
    EXPECT_EQ(r.intent.action, QuoteAction::Quote);
    EXPECT_TRUE(r.intent.quote_bid);
    EXPECT_EQ(strategy->evaluations, 1U);
}

TEST_F(RuntimeTest, StaleMarketDataIsNotOfferedToTheStrategy) {
    auto* strategy = build<WellBehavedStrategy>();
    StrategyContext c = make_context();
    c.data_age_ns = millis(600);
    const EvaluationResult r = runtime->evaluate(c);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.skip, SkipReason::MarketDataStale);
    EXPECT_EQ(strategy->evaluations, 0U) << "the strategy must not even be called";
    EXPECT_EQ(r.intent.action, QuoteAction::Pull);
}

TEST_F(RuntimeTest, UnsynchronizedBookIsNotOfferedToTheStrategy) {
    auto* strategy = build<WellBehavedStrategy>();
    for (const auto state : {SessionState::Syncing, SessionState::ResyncRequired,
                             SessionState::Stale, SessionState::Disconnected,
                             SessionState::Subscribed, SessionState::Error}) {
        StrategyContext c = make_context();
        c.session_state = state;
        const EvaluationResult r = runtime->evaluate(c);
        EXPECT_FALSE(r.accepted) << exchange::to_string(state);
        EXPECT_EQ(r.skip, SkipReason::BookNotQuotable);
    }
    EXPECT_EQ(strategy->evaluations, 0U);
}

TEST_F(RuntimeTest, OneSidedOrCrossedMarketIsRefused) {
    build<WellBehavedStrategy>();
    StrategyContext one_sided = make_context();
    one_sided.bbo.ask_px = Px::zero();
    EXPECT_EQ(runtime->evaluate(one_sided).skip, SkipReason::NoTwoSidedMarket);

    StrategyContext crossed = make_context();
    ASSERT_TRUE(Px::parse("60000.20", crossed.bbo.bid_px));
    EXPECT_EQ(runtime->evaluate(crossed).skip, SkipReason::NoTwoSidedMarket);
}

TEST_F(RuntimeTest, MissingInstrumentSpecIsRefused) {
    build<WellBehavedStrategy>();
    StrategyContext c = make_context();
    c.instrument = nullptr;
    EXPECT_EQ(runtime->evaluate(c).skip, SkipReason::InstrumentNotLoaded);
}

TEST_F(RuntimeTest, DisabledRuntimeNeverEvaluates) {
    StrategyRuntimeConfig config = make_config();
    config.enabled = false;
    auto* strategy = build<WellBehavedStrategy>(config);
    EXPECT_EQ(runtime->evaluate(make_context()).skip, SkipReason::QuotingDisabled);
    EXPECT_EQ(strategy->evaluations, 0U);
}

TEST_F(RuntimeTest, QuotingSwitchKeepsTheStrategyWarmButPullsQuotes) {
    StrategyRuntimeConfig config = make_config();
    config.quoting_enabled = false;
    auto* strategy = build<WellBehavedStrategy>(config);

    const EvaluationResult r = runtime->evaluate(make_context());
    // Still evaluated, so internal state stays warm; but nothing is quoted.
    EXPECT_EQ(strategy->evaluations, 1U);
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.intent.action, QuoteAction::Pull);
    EXPECT_EQ(r.intent.reason, IntentReason::StrategyDisabled);
}

// ===========================================================================
// Trigger model (§7)
// ===========================================================================

TEST_F(RuntimeTest, EvaluationModeSelectsTriggers) {
    struct Case {
        EvaluationMode mode;
        TriggerReason trigger;
        bool wanted;
    };
    const Case cases[] = {
        {EvaluationMode::OnBboChange, TriggerReason::BboChange, true},
        {EvaluationMode::OnBboChange, TriggerReason::Timer, false},
        {EvaluationMode::OnBboChange, TriggerReason::BookUpdate, false},
        {EvaluationMode::OnTimer, TriggerReason::Timer, true},
        {EvaluationMode::OnTimer, TriggerReason::BboChange, false},
        {EvaluationMode::OnBookUpdate, TriggerReason::BookUpdate, true},
        {EvaluationMode::OnBookUpdate, TriggerReason::BboChange, true},
        {EvaluationMode::OnBboChangeAndTimer, TriggerReason::BboChange, true},
        {EvaluationMode::OnBboChangeAndTimer, TriggerReason::Timer, true},
        {EvaluationMode::OnBboChangeAndTimer, TriggerReason::BookUpdate, false},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(mode_accepts(c.mode, c.trigger), c.wanted)
            << to_string(c.mode) << " / " << to_string(c.trigger);
    }
}

TEST_F(RuntimeTest, UnwantedTriggerSkipsWithoutCallingTheStrategy) {
    StrategyRuntimeConfig config = make_config();
    config.evaluation_mode = EvaluationMode::OnTimer;
    auto* strategy = build<WellBehavedStrategy>(config);

    EXPECT_EQ(runtime->evaluate(make_context(TriggerReason::BboChange)).skip,
              SkipReason::TriggerNotWanted);
    EXPECT_EQ(strategy->evaluations, 0U);
    EXPECT_TRUE(runtime->evaluate(make_context(TriggerReason::Timer)).accepted);
    EXPECT_EQ(strategy->evaluations, 1U);
}

// ===========================================================================
// Output validation (§10)
// ===========================================================================

TEST_F(RuntimeTest, RejectsEveryMalformedQuote) {
    auto* strategy = build<ScriptedStrategy>();
    const StrategyContext context = make_context();

    const auto base = [&] {
        QuoteIntent i;
        i.action = QuoteAction::Quote;
        i.quote_bid = true;
        i.quote_ask = true;
        i.bid_price = context.bbo.bid_px;
        i.ask_price = context.bbo.ask_px;
        EXPECT_TRUE(Qty::parse("0.001", i.bid_quantity));
        EXPECT_TRUE(Qty::parse("0.001", i.ask_quantity));
        return i;
    };

    struct Case {
        const char* what;
        std::function<void(QuoteIntent&)> mutate;
        IntentRejection expected;
    };
    const Case cases[] = {
        {"zero bid price", [](QuoteIntent& i) { i.bid_price = Px::zero(); },
         IntentRejection::NonPositivePrice},
        {"zero quantity", [](QuoteIntent& i) { i.bid_quantity = Qty::zero(); },
         IntentRejection::NonPositiveQuantity},
        {"negative quantity", [](QuoteIntent& i) { i.bid_quantity = Qty::from_raw(-1); },
         IntentRejection::NegativeQuantity},
        {"off-tick price",
         [](QuoteIntent& i) { EXPECT_TRUE(Px::parse("60000.005", i.bid_price)); },
         IntentRejection::TickMisaligned},
        {"off-lot quantity",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("0.000015", i.bid_quantity)); },
         IntentRejection::LotMisaligned},
        {"below min notional",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("0.00001", i.bid_quantity)); },
         IntentRejection::NotionalTooSmall},
        {"above venue max quantity",
         [](QuoteIntent& i) { EXPECT_TRUE(Qty::parse("2000", i.bid_quantity)); },
         IntentRejection::QuantityAboveVenueMax},
        {"self-crossing quote",
         [](QuoteIntent& i) {
             EXPECT_TRUE(Px::parse("60000.50", i.bid_price));
             EXPECT_TRUE(Px::parse("60000.10", i.ask_price));
         },
         IntentRejection::CrossedQuote},
        {"quote miles from the touch",
         [](QuoteIntent& i) { EXPECT_TRUE(Px::parse("30000.00", i.bid_price)); },
         IntentRejection::QuoteTooFarFromTouch},
        {"quote with nothing quoted",
         [](QuoteIntent& i) {
             i.quote_bid = false;
             i.quote_ask = false;
         },
         IntentRejection::UnknownAction},
    };

    for (const auto& c : cases) {
        QuoteIntent intent = base();
        c.mutate(intent);
        strategy->next = intent;
        const EvaluationResult r = runtime->evaluate(context);
        EXPECT_FALSE(r.accepted) << c.what;
        EXPECT_EQ(r.rejection, c.expected) << c.what;
        // A rejected intent must never fall back to a previous quote.
        EXPECT_EQ(r.intent.action, QuoteAction::Pull) << c.what;
        // Enough consecutive rejections fault the strategy by design; clear it
        // between cases so each case exercises validation rather than the
        // fault path that follows it.
        if (runtime->state() == StrategyState::Faulted) {
            ASSERT_TRUE(runtime->clear_fault().is_ok()) << c.what;
        }
        runtime->start();
        ASSERT_EQ(runtime->state(), StrategyState::Running) << c.what;
    }
}

TEST_F(RuntimeTest, AcceptsPullAndNoChangeWithoutPriceChecks) {
    auto* strategy = build<ScriptedStrategy>();
    strategy->next = QuoteIntent::pull(IntentReason::InventoryLimit);
    EXPECT_TRUE(runtime->evaluate(make_context()).accepted);

    strategy->next = QuoteIntent::no_change();
    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.intent.action, QuoteAction::NoChange);
}

TEST_F(RuntimeTest, RejectsAnIntentComputedFromAnOlderBook) {
    auto* strategy = build<ScriptedStrategy>();
    strategy->use_context_sequence = false;
    QuoteIntent intent;
    intent.action = QuoteAction::Quote;
    intent.quote_bid = true;
    ASSERT_TRUE(Px::parse("60000.00", intent.bid_price));
    ASSERT_TRUE(Qty::parse("0.001", intent.bid_quantity));
    intent.market_sequence = 499;  // the context is at 500
    strategy->next = intent;

    const EvaluationResult r = runtime->evaluate(make_context());
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.rejection, IntentRejection::StaleSequence);
}

// ===========================================================================
// Versioning (§17)
// ===========================================================================

TEST_F(RuntimeTest, IntentCarriesStrategyIdentityAndGeneration) {
    build<WellBehavedStrategy>();
    StrategyContext c = make_context();
    c.config_generation = 42;
    const EvaluationResult r = runtime->evaluate(c);
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.intent.identity.name, StrategyName("test_good_v1"));
    EXPECT_EQ(r.intent.identity.version, 1);
    EXPECT_EQ(r.intent.identity.config_generation, 42U)
        << "attribution must distinguish two runs of the same build with different parameters";
    EXPECT_EQ(r.intent.market_sequence, c.sequence);
    EXPECT_GT(r.intent.computed_ns, 0);
}

TEST_F(RuntimeTest, RuntimeStampsIdentityRatherThanTrustingTheStrategy) {
    auto* strategy = build<ScriptedStrategy>();
    QuoteIntent intent;
    intent.action = QuoteAction::Quote;
    intent.quote_bid = true;
    ASSERT_TRUE(Px::parse("60000.00", intent.bid_price));
    ASSERT_TRUE(Qty::parse("0.001", intent.bid_quantity));
    // The strategy tries to claim someone else's identity.
    static_cast<void>(intent.identity.name.assign("impersonated_v9"));
    intent.identity.version = 9;
    strategy->next = intent;

    const EvaluationResult r = runtime->evaluate(make_context());
    ASSERT_TRUE(r.accepted);
    EXPECT_EQ(r.intent.identity.name, StrategyName("test_scripted_v1"))
        << "provenance is the runtime's to stamp, not the strategy's to claim";
}

// ===========================================================================
// Determinism (§8)
// ===========================================================================

TEST_F(RuntimeTest, SameContextProducesSameIntent) {
    build<WellBehavedStrategy>();
    const StrategyContext c = make_context();

    const EvaluationResult a = runtime->evaluate(c);
    const EvaluationResult b = runtime->evaluate(c);
    ASSERT_TRUE(a.accepted);
    ASSERT_TRUE(b.accepted);
    EXPECT_EQ(a.intent.bid_price.raw(), b.intent.bid_price.raw());
    EXPECT_EQ(a.intent.ask_price.raw(), b.intent.ask_price.raw());
    EXPECT_EQ(a.intent.bid_quantity.raw(), b.intent.bid_quantity.raw());
    EXPECT_EQ(a.intent.action, b.intent.action);
}

TEST_F(RuntimeTest, TwoIndependentInstancesAgree) {
    // Same context, same parameters, separate objects: any hidden global or
    // clock dependency would show up here.
    const StrategyContext c = make_context();

    auto run_once = [&] {
        ManualClock local(millis(1'000), 0);
        auto strategy = std::make_unique<WellBehavedStrategy>();
        StrategyRuntime rt(make_config(), std::move(strategy), local);
        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        static_cast<void>(init.identity.name.assign("test_good_v1"));
        init.identity.version = 1;
        EXPECT_TRUE(rt.initialize(init).is_ok());
        rt.start();
        return rt.evaluate(c).intent;
    };

    const QuoteIntent first = run_once();
    const QuoteIntent second = run_once();
    EXPECT_EQ(first.bid_price.raw(), second.bid_price.raw());
    EXPECT_EQ(first.ask_price.raw(), second.ask_price.raw());
    EXPECT_EQ(first.action, second.action);
}

// ===========================================================================
// Notifications and metrics
// ===========================================================================

TEST_F(RuntimeTest, NotificationsReachTheStrategy) {
    auto* strategy = build<WellBehavedStrategy>();
    StrategyFill fill;
    fill.symbol = Symbol("BTCUSDT");
    fill.side = Side::Buy;
    runtime->on_fill(fill);
    EXPECT_EQ(strategy->fills, 1U);

    StrategyOrderEvent event;
    event.event = QuoteLifecycle::Placed;
    runtime->on_order_event(event);
    EXPECT_EQ(strategy->order_events, 1U);
}

TEST_F(RuntimeTest, MetricsCountEvaluationsSkipsAndIntents) {
    build<WellBehavedStrategy>();
    static_cast<void>(runtime->evaluate(make_context()));
    static_cast<void>(runtime->evaluate(make_context()));

    StrategyContext stale = make_context();
    stale.data_age_ns = millis(600);
    static_cast<void>(runtime->evaluate(stale));

    const StrategyMetrics& m = runtime->metrics();
    EXPECT_EQ(m.evaluations, 2U);
    EXPECT_EQ(m.skipped, 1U);
    EXPECT_EQ(m.intents_accepted, 2U);
    EXPECT_EQ(m.quote_intents, 2U);
    EXPECT_EQ(m.skips_by_reason[static_cast<std::size_t>(SkipReason::MarketDataStale)], 1U);
    EXPECT_EQ(runtime->latency().count(), 2U);
}

}  // namespace
}  // namespace mm::strategy
