/// \file test_strategy_swap.cpp
/// Proof that replacing a strategy is a configuration change, not a code change.
///
/// The claim under test is architectural: the engine depends on `IStrategy` and
/// the registry, never on a concrete strategy, so swapping one for another
/// touches no infrastructure. A claim like that decays silently unless
/// something checks it, so this suite checks it mechanically.

#include <gtest/gtest.h>

#include <memory>

#include "mm/strategy/StrategyRegistry.hpp"
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

StrategyContext make_context(const InstrumentSpec& spec, Nanos now) {
    StrategyContext c;
    c.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse("60000.00", c.bbo.bid_px));
    EXPECT_TRUE(Qty::parse("1", c.bbo.bid_qty));
    EXPECT_TRUE(Px::parse("60000.10", c.bbo.ask_px));
    EXPECT_TRUE(Qty::parse("1", c.bbo.ask_qty));
    c.sequence = 500;
    c.now_ns = now;
    c.instrument = &spec;
    c.session_state = SessionState::Ready;
    c.trigger = TriggerReason::BboChange;
    EXPECT_TRUE(Qty::parse("0.5", c.inventory.position_limit));
    return c;
}

// ===========================================================================
// The registry
// ===========================================================================

TEST(StrategyRegistryTest, ThePluginIsActuallyRegistered) {
    // The failure this guards against is a link-time one: a self-registering
    // translation unit that nothing references is dead-stripped out of a static
    // archive and the strategy silently vanishes. `strategies/` is an OBJECT
    // library for exactly this reason, and this test is what would notice if
    // that changed.
    const StrategyRegistry& registry = StrategyRegistry::instance();
    EXPECT_TRUE(registry.contains("reference_mm_v1"))
        << "the strategies object library is not linked whole";
    EXPECT_GE(registry.size(), 1U);
}

TEST(StrategyRegistryTest, CreatesByName) {
    const auto result = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(result.is_ok()) << result.status().to_string();
    ASSERT_NE(result.value(), nullptr);
    EXPECT_EQ(result.value()->identity().name, StrategyName("reference_mm_v1"));
    EXPECT_EQ(result.value()->identity().version, 1);
}

TEST(StrategyRegistryTest, UnknownNameListsWhatIsAvailable) {
    const auto result = StrategyRegistry::instance().create("does_not_exist");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.status().code(), ErrorCode::NotFound);
    // A config typo should be a one-line fix, not a hunt through the tree.
    EXPECT_NE(result.status().message().find("reference_mm_v1"), std::string_view::npos);
}

TEST(StrategyRegistryTest, VersionMismatchIsCaughtAtStartup) {
    const StrategyRegistry& registry = StrategyRegistry::instance();
    EXPECT_TRUE(registry.check_version("reference_mm_v1", 1).is_ok());
    // A config asking for v2 against a binary containing v1 is a deployment
    // mistake; seconds to catch here, an investigation to catch in the journal.
    const Status s = registry.check_version("reference_mm_v1", 2);
    ASSERT_TRUE(s.is_error());
    EXPECT_NE(s.message().find("version"), std::string_view::npos);
}

TEST(StrategyRegistryTest, EachCreateYieldsAnIndependentInstance) {
    const auto a = StrategyRegistry::instance().create("reference_mm_v1");
    const auto b = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_NE(a.value().get(), b.value().get());
}

TEST(StrategyRegistryTest, ListingIsOrderIndependent) {
    const auto listed = StrategyRegistry::instance().list();
    ASSERT_FALSE(listed.empty());
    // Stable regardless of link order, so startup logs are comparable.
    for (std::size_t i = 1; i < listed.size(); ++i) {
        EXPECT_LT(listed[i - 1].name, listed[i].name);
    }
    for (const auto& d : listed) {
        EXPECT_FALSE(d.description.empty()) << d.name << " has no description";
        EXPECT_GT(d.version, 0);
    }
}

TEST(StrategyRegistryTest, DuplicateRegistrationIsRefused) {
    // Two strategies sharing a name would make the config ambiguous, and which
    // one won would depend on link order.
    StrategyRegistry local;
    EXPECT_TRUE(local.register_strategy("dup", 1, "first",
                                        [] { return StrategyPtr(std::make_unique<
                                                 test::WellBehavedStrategy>()); }));
    EXPECT_FALSE(local.register_strategy("dup", 2, "second",
                                         [] { return StrategyPtr(std::make_unique<
                                                  test::WellBehavedStrategy>()); }));
    EXPECT_EQ(local.size(), 1U);
}

TEST(StrategyRegistryTest, RejectsMalformedRegistrations) {
    StrategyRegistry local;
    const auto factory = [] {
        return StrategyPtr(std::make_unique<test::WellBehavedStrategy>());
    };
    EXPECT_FALSE(local.register_strategy("", 1, "no name", factory));
    EXPECT_FALSE(local.register_strategy("x", 0, "no version", factory));
    EXPECT_FALSE(local.register_strategy("x", 1, "no factory", nullptr));
    EXPECT_EQ(local.size(), 0U);
}

// ===========================================================================
// The swap
// ===========================================================================

/// Runs a strategy through the identical runtime, wired identically. The only
/// thing that varies between calls is the name.
EvaluationResult run_through_runtime(StrategyPtr strategy, const InstrumentSpec& spec,
                                     ManualClock& clock) {
    StrategyRuntimeConfig config;
    config.evaluation_mode = EvaluationMode::OnBboChangeAndTimer;

    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = spec;
    init.identity = strategy->identity();
    init.params.set("half_spread_bps", "5.0");
    init.params.set("quote_size", "0.001");

    StrategyRuntime runtime(config, std::move(strategy), clock);
    EXPECT_TRUE(runtime.initialize(init).is_ok());
    runtime.start();
    return runtime.evaluate(make_context(spec, clock.steady()));
}

TEST(StrategySwapTest, TwoStrategiesRunThroughTheSameRuntimeUnchanged) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();

    // A: the registered reference strategy, resolved by name.
    auto from_registry = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(from_registry.is_ok());
    const EvaluationResult a = run_through_runtime(std::move(from_registry.value()), spec, clock);

    // B: a completely unrelated implementation.
    const EvaluationResult b =
        run_through_runtime(std::make_unique<test::WellBehavedStrategy>(), spec, clock);

    // Both produced usable intent through byte-identical infrastructure. No
    // branch anywhere in the runtime names either of them.
    ASSERT_TRUE(a.accepted) << to_string(a.rejection);
    ASSERT_TRUE(b.accepted) << to_string(b.rejection);
    EXPECT_EQ(a.intent.action, QuoteAction::Quote);
    EXPECT_EQ(b.intent.action, QuoteAction::Quote);

    // And their identities are distinct, so fills are attributable.
    EXPECT_NE(a.intent.identity.name, b.intent.identity.name);
    EXPECT_EQ(a.intent.identity.name, StrategyName("reference_mm_v1"));
    EXPECT_EQ(b.intent.identity.name, StrategyName("test_good_v1"));
}

TEST(StrategySwapTest, SwappingChangesTheQuotesAndNothingElse) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();

    auto reference = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(reference.is_ok());
    const EvaluationResult a = run_through_runtime(std::move(reference.value()), spec, clock);
    const EvaluationResult b =
        run_through_runtime(std::make_unique<test::WellBehavedStrategy>(), spec, clock);

    // The reference strategy quotes a 5bp half-spread around the mid; the test
    // strategy quotes at the touch. Different numbers, same pipeline.
    ASSERT_TRUE(a.accepted);
    ASSERT_TRUE(b.accepted);
    EXPECT_NE(a.intent.bid_price.raw(), b.intent.bid_price.raw());
    EXPECT_LT(a.intent.bid_price, b.intent.bid_price) << "5bp inside is worse than at the touch";
}

TEST(StrategySwapTest, TheRuntimeNeverNamesAConcreteStrategy) {
    // Expressed as a type-level fact: the runtime is constructible from any
    // IStrategy, and holds it only as the interface.
    static_assert(std::is_constructible_v<StrategyRuntime, StrategyRuntimeConfig, StrategyPtr,
                                          const Clock&>);
    static_assert(std::is_same_v<StrategyPtr, std::unique_ptr<IStrategy>>);
    SUCCEED();
}

// ===========================================================================
// The reference strategy itself
// ===========================================================================

TEST(ReferenceStrategyTest, RefusesIncompleteParameters) {
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(created.is_ok());
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = btc_spec();
    init.identity = created.value()->identity();
    // A mistyped parameter name must stop the engine from starting, not let it
    // quote a spread nobody chose.
    EXPECT_TRUE(created.value()->initialize(init).is_error());

    init.params.set("half_spread_bps", "5.0");
    EXPECT_TRUE(created.value()->initialize(init).is_error()) << "quote_size still missing";

    init.params.set("quote_size", "0.001");
    EXPECT_TRUE(created.value()->initialize(init).is_ok());
}

TEST(ReferenceStrategyTest, RefusesNonsensicalParameters) {
    const auto attempt = [](const char* key, const char* value) {
        auto created = StrategyRegistry::instance().create("reference_mm_v1");
        EXPECT_TRUE(created.is_ok());
        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = btc_spec();
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "5.0");
        init.params.set("quote_size", "0.001");
        init.params.set(key, value);
        return created.value()->initialize(init);
    };
    EXPECT_TRUE(attempt("half_spread_bps", "0").is_error());
    EXPECT_TRUE(attempt("half_spread_bps", "-1").is_error());
    EXPECT_TRUE(attempt("quote_size", "0").is_error());
    EXPECT_TRUE(attempt("inventory_skew_bps", "-5").is_error());
    EXPECT_TRUE(attempt("max_inventory_utilisation", "0").is_error());
    EXPECT_TRUE(attempt("max_inventory_utilisation", "1.5").is_error());
    // An off-lot quote size would otherwise be rejected on every evaluation.
    EXPECT_TRUE(attempt("quote_size", "0.000015").is_error());
}

TEST(ReferenceStrategyTest, InventorySkewMovesQuotesAwayFromTheHeavySide) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();

    const auto quote_at = [&](const char* position) {
        auto created = StrategyRegistry::instance().create("reference_mm_v1");
        EXPECT_TRUE(created.is_ok());
        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "10.0");
        init.params.set("quote_size", "0.001");
        init.params.set("inventory_skew_bps", "20.0");

        StrategyRuntimeConfig config;
        StrategyRuntime runtime(config, std::move(created.value()), clock);
        EXPECT_TRUE(runtime.initialize(init).is_ok());
        runtime.start();

        StrategyContext c = make_context(spec, clock.steady());
        EXPECT_TRUE(Qty::parse(position, c.inventory.position));
        return runtime.evaluate(c);
    };

    const EvaluationResult flat = quote_at("0");
    const EvaluationResult long_pos = quote_at("0.25");
    ASSERT_TRUE(flat.accepted);
    ASSERT_TRUE(long_pos.accepted);

    // Long inventory pushes both quotes down: less eager to buy, keener to sell.
    EXPECT_LT(long_pos.intent.bid_price, flat.intent.bid_price);
    EXPECT_LT(long_pos.intent.ask_price, flat.intent.ask_price);
}

TEST(ReferenceStrategyTest, StopsQuotingASideAtTheInventoryLimit) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(created.is_ok());

    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = spec;
    init.identity = created.value()->identity();
    init.params.set("half_spread_bps", "5.0");
    init.params.set("quote_size", "0.001");
    init.params.set("max_inventory_utilisation", "0.8");

    StrategyRuntimeConfig config;
    StrategyRuntime runtime(config, std::move(created.value()), clock);
    ASSERT_TRUE(runtime.initialize(init).is_ok());
    runtime.start();

    StrategyContext c = make_context(spec, clock.steady());
    ASSERT_TRUE(Qty::parse("0.45", c.inventory.position));  // 90% of the 0.5 limit
    const EvaluationResult r = runtime.evaluate(c);
    ASSERT_TRUE(r.accepted) << to_string(r.rejection);
    EXPECT_FALSE(r.intent.quote_bid) << "must stop adding to a side it is already heavy on";
    EXPECT_TRUE(r.intent.quote_ask) << "the reducing side keeps quoting";
    EXPECT_EQ(r.intent.reason, IntentReason::InventoryLimit);
}

TEST(ReferenceStrategyTest, ProducesTickAlignedQuotesThePipelineAccepts) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(created.is_ok());
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = spec;
    init.identity = created.value()->identity();
    init.params.set("half_spread_bps", "3.7");  // deliberately not tick-friendly
    init.params.set("quote_size", "0.001");

    StrategyRuntimeConfig config;
    StrategyRuntime runtime(config, std::move(created.value()), clock);
    ASSERT_TRUE(runtime.initialize(init).is_ok());
    runtime.start();

    const EvaluationResult r = runtime.evaluate(make_context(spec, clock.steady()));
    ASSERT_TRUE(r.accepted) << to_string(r.rejection);
    EXPECT_TRUE(is_on_step(r.intent.bid_price, spec.tick_size));
    EXPECT_TRUE(is_on_step(r.intent.ask_price, spec.tick_size));
    EXPECT_LT(r.intent.bid_price, r.intent.ask_price);
}

TEST(ReferenceStrategyTest, IsDeterministic) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();
    const auto run = [&] {
        auto created = StrategyRegistry::instance().create("reference_mm_v1");
        EXPECT_TRUE(created.is_ok());
        StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "4.25");
        init.params.set("quote_size", "0.001");
        init.params.set("inventory_skew_bps", "12.5");
        StrategyRuntimeConfig config;
        StrategyRuntime runtime(config, std::move(created.value()), clock);
        EXPECT_TRUE(runtime.initialize(init).is_ok());
        runtime.start();
        StrategyContext c = make_context(spec, clock.steady());
        EXPECT_TRUE(Qty::parse("0.13", c.inventory.position));
        return runtime.evaluate(c).intent;
    };
    const QuoteIntent a = run();
    const QuoteIntent b = run();
    EXPECT_EQ(a.bid_price.raw(), b.bid_price.raw());
    EXPECT_EQ(a.ask_price.raw(), b.ask_price.raw());
    EXPECT_EQ(a.bid_quantity.raw(), b.bid_quantity.raw());
}

TEST(ReferenceStrategyTest, TimerAndMarketEvaluationAgree) {
    ManualClock clock(millis(1'000), 0);
    const InstrumentSpec spec = btc_spec();
    auto created = StrategyRegistry::instance().create("reference_mm_v1");
    ASSERT_TRUE(created.is_ok());
    StrategyInit init;
    init.symbol = Symbol("BTCUSDT");
    init.instrument = spec;
    init.identity = created.value()->identity();
    init.params.set("half_spread_bps", "5.0");
    init.params.set("quote_size", "0.001");

    StrategyRuntimeConfig config;
    StrategyRuntime runtime(config, std::move(created.value()), clock);
    ASSERT_TRUE(runtime.initialize(init).is_ok());
    runtime.start();

    StrategyContext market = make_context(spec, clock.steady());
    StrategyContext timer = market;
    timer.trigger = TriggerReason::Timer;

    const EvaluationResult a = runtime.evaluate(market);
    const EvaluationResult b = runtime.evaluate(timer);
    ASSERT_TRUE(a.accepted);
    ASSERT_TRUE(b.accepted);
    // The quotes depend on the market and inventory, never on how long since
    // the last evaluation, so the two paths must not drift apart.
    EXPECT_EQ(a.intent.bid_price.raw(), b.intent.bid_price.raw());
    EXPECT_EQ(a.intent.ask_price.raw(), b.intent.ask_price.raw());
}

}  // namespace
}  // namespace mm::strategy
