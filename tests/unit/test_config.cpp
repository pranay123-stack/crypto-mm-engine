#include <gtest/gtest.h>

#include "mm/common/Config.hpp"

namespace mm {
namespace {

// A configuration that satisfies every live gate. Individual tests break one
// field at a time so each gate is proven to be load-bearing on its own.
constexpr const char* kLiveReadyYaml = R"YAML(
mode: live
session_name: test
exchange:
  name: binance
  credentials_env_prefix: BINANCE_LIVE
symbols:
  - symbol: BTCUSDT
strategy:
  name: finalized_mm_v1
  version: 1
risk:
  max_position: 0.5
  max_notional: 25000
  max_order_qty: 0.1
  max_order_notional: 6000
  max_daily_loss: 500
  emergency_loss: 1000
safety:
  live_trading_enabled: true
)YAML";

EngineConfig load_ok(const char* yaml) {
    auto r = load_config_string(yaml);
    EXPECT_TRUE(r.is_ok()) << r.status().to_string();
    return r.is_ok() ? r.value() : EngineConfig{};
}

TEST(Config, DefaultsToPaper) {
    const auto cfg = load_ok(R"YAML(
symbols: [BTCUSDT]
strategy: { name: test_v1 }
)YAML");
    EXPECT_EQ(cfg.mode, TradingMode::Paper);
    EXPECT_FALSE(cfg.safety.live_trading_enabled)
        << "live trading must be off unless explicitly enabled";
}

TEST(Config, ParsesFullDocument) {
    const auto cfg = load_ok(R"YAML(
mode: paper
session_name: alpha
exchange:
  name: binance
  book_depth: 20
  recv_window_ms: 3000
symbols:
  - symbol: BTCUSDT
    quote_size: 0.001
  - symbol: ETHUSDT
    enabled: false
strategy:
  name: finalized_mm_v2
  version: 2
  budget_ns: 40000
  params:
    base_half_spread_bps: 4.5
    inventory:
      skew_coefficient: 0.8
    levels: [1, 2, 3]
risk:
  max_position: 0.5
  max_notional: 25000
execution:
  max_orders_per_second: 15
safety:
  max_market_data_age_ms: 250
monitoring:
  http_port: 9090
)YAML");

    EXPECT_EQ(cfg.session_name, "alpha");
    EXPECT_EQ(cfg.exchange.book_depth, 20);
    ASSERT_EQ(cfg.symbols.size(), 2U);
    EXPECT_EQ(cfg.symbols[0].symbol, "BTCUSDT");
    EXPECT_EQ(cfg.symbols[0].quote_size.to_string(), "0.001");
    EXPECT_FALSE(cfg.symbols[1].enabled);
    EXPECT_EQ(cfg.strategy.name, "finalized_mm_v2");
    EXPECT_EQ(cfg.strategy.version, 2);
    EXPECT_EQ(cfg.risk.max_position.to_string(), "0.5");
    EXPECT_EQ(cfg.execution.max_orders_per_second, 15);
    EXPECT_EQ(cfg.safety.max_market_data_age_ms, 250);
    EXPECT_EQ(cfg.monitoring.http_port, 9090);
}

TEST(Config, FixedPointFieldsAreParsedExactly) {
    // Read as text and parsed exactly. Going through a double would turn 0.1
    // into 0.09999999999999999 and make every position limit slightly wrong.
    const auto cfg = load_ok(R"YAML(
symbols: [BTCUSDT]
strategy: { name: t }
risk:
  max_position: 0.1
)YAML");
    EXPECT_EQ(cfg.risk.max_position.raw(), 10'000'000);
    EXPECT_EQ(cfg.risk.max_position.to_string(), "0.1");
}

TEST(Config, StrategyParamsAreFlattened) {
    const auto cfg = load_ok(R"YAML(
symbols: [BTCUSDT]
strategy:
  name: t
  params:
    base_half_spread_bps: 4.5
    inventory:
      skew_coefficient: 0.8
      max_skew_bps: 12
    enabled: true
    levels: [10, 20]
)YAML");
    const Params& p = cfg.strategy.params;
    EXPECT_DOUBLE_EQ(p.double_or("base_half_spread_bps", 0.0), 4.5);
    EXPECT_DOUBLE_EQ(p.double_or("inventory.skew_coefficient", 0.0), 0.8);
    EXPECT_EQ(p.int_or("inventory.max_skew_bps", 0), 12);
    EXPECT_TRUE(p.bool_or("enabled", false));
    EXPECT_EQ(p.int_or("levels.0", 0), 10);
    EXPECT_EQ(p.int_or("levels.1", 0), 20);
    EXPECT_EQ(p.int_or("levels.count", 0), 2);
}

// A typo in a risk limit must stop the engine. Silently ignoring it would leave
// the operator believing a limit is enforced when it is not -- the exact
// failure mode a risk system exists to prevent.
TEST(Config, UnknownKeyIsRejected) {
    const auto r = load_config_string(R"YAML(
symbols: [BTCUSDT]
strategy: { name: t }
risk:
  max_postion: 0.5
)YAML");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.status().message().find("max_postion"), std::string_view::npos);
}

TEST(Config, UnknownTopLevelKeyIsRejected) {
    const auto r = load_config_string("symbols: [BTCUSDT]\nstrategy: {name: t}\nbacktest: {}\n");
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(r.status().message().find("backtest"), std::string_view::npos);
}

TEST(Config, RejectsMalformedYaml) {
    const auto r = load_config_string("mode: [unclosed\n");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::ParseError);
}

TEST(Config, RejectsUnknownMode) {
    const auto r = load_config_string("mode: production\nsymbols: [BTCUSDT]\nstrategy: {name: t}");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::InvalidArgument);
}

TEST(Config, RejectsEmptySymbolList) {
    const auto r = load_config_string("strategy: {name: t}\n");
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(r.status().message().find("no symbols"), std::string_view::npos);
}

TEST(Config, RejectsDuplicateSymbols) {
    const auto r = load_config_string("symbols: [BTCUSDT, BTCUSDT]\nstrategy: {name: t}");
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(r.status().message().find("duplicate"), std::string_view::npos);
}

TEST(Config, RejectsMissingStrategyName) {
    const auto r = load_config_string("symbols: [BTCUSDT]\n");
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(r.status().message().find("strategy.name"), std::string_view::npos);
}

TEST(Config, RejectsNegativeRiskLimits) {
    const auto r = load_config_string(R"YAML(
symbols: [BTCUSDT]
strategy: { name: t }
risk:
  max_position: -1
)YAML");
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(r.status().message().find("must not be negative"), std::string_view::npos);
}

TEST(Config, RejectsInconsistentOrderCountLimits) {
    const auto r = load_config_string(R"YAML(
symbols: [BTCUSDT]
strategy: { name: t }
risk:
  max_active_orders_per_symbol: 50
  max_active_orders_total: 10
)YAML");
    ASSERT_TRUE(r.is_error());
}

TEST(Config, RejectsOutOfRangePaperProbabilities) {
    const auto r = load_config_string(R"YAML(
symbols: [BTCUSDT]
strategy: { name: t }
paper:
  queue_fill_ratio: 1.5
)YAML");
    ASSERT_TRUE(r.is_error());
}

// ---------------------------------------------------------------------------
// Live gates. Each test removes exactly one requirement.
// ---------------------------------------------------------------------------

TEST(ConfigLiveGates, PaperModeNeedsNoGates) {
    const auto cfg = load_ok("symbols: [BTCUSDT]\nstrategy: {name: t}");
    EXPECT_TRUE(cfg.validate_live_gates(false).is_ok());
}

TEST(ConfigLiveGates, FullyConfiguredLivePasses) {
    const auto cfg = load_ok(kLiveReadyYaml);
    ASSERT_EQ(cfg.mode, TradingMode::Live);
    const Status s = cfg.validate_live_gates(true);
    EXPECT_TRUE(s.is_ok()) << s.to_string();
}

TEST(ConfigLiveGates, ConfigAloneCannotGoLive) {
    // The single most important gate: a config file that says `mode: live` and
    // `live_trading_enabled: true` still cannot trade without the operator
    // passing --live at launch. Copying a live config to a test box is safe.
    const auto cfg = load_ok(kLiveReadyYaml);
    const Status s = cfg.validate_live_gates(/*operator_confirmed_live=*/false);
    ASSERT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::PermissionDenied);
    EXPECT_NE(s.message().find("--live"), std::string_view::npos);
}

TEST(ConfigLiveGates, ModeLiveWithoutSafetyFlagIsRefused) {
    std::string yaml(kLiveReadyYaml);
    const auto pos = yaml.find("live_trading_enabled: true");
    ASSERT_NE(pos, std::string::npos);
    yaml.replace(pos, std::string("live_trading_enabled: true").size(),
                 "live_trading_enabled: false");
    const auto cfg = load_ok(yaml.c_str());
    const Status s = cfg.validate_live_gates(true);
    ASSERT_TRUE(s.is_error());
    EXPECT_EQ(s.code(), ErrorCode::PermissionDenied);
}

TEST(ConfigLiveGates, UnsetLimitIsRefusalNotUnlimited) {
    for (const char* removed : {"max_position: 0.5", "max_notional: 25000", "max_order_qty: 0.1",
                                "max_order_notional: 6000", "max_daily_loss: 500",
                                "emergency_loss: 1000"}) {
        std::string yaml(kLiveReadyYaml);
        const auto pos = yaml.find(removed);
        ASSERT_NE(pos, std::string::npos) << removed;
        yaml.erase(pos, std::string(removed).size());

        const auto r = load_config_string(yaml);
        ASSERT_TRUE(r.is_ok()) << r.status().to_string();
        const Status s = r.value().validate_live_gates(true);
        EXPECT_TRUE(s.is_error()) << "live mode accepted a config missing '" << removed << "'";
    }
}

TEST(ConfigLiveGates, EmergencyLossMustExceedDailyLoss) {
    std::string yaml(kLiveReadyYaml);
    const auto pos = yaml.find("emergency_loss: 1000");
    yaml.replace(pos, std::string("emergency_loss: 1000").size(), "emergency_loss: 100");
    const auto cfg = load_ok(yaml.c_str());
    // Otherwise the ordinary daily halt could never fire before the emergency
    // one, making the graduated response meaningless.
    EXPECT_TRUE(cfg.validate_live_gates(true).is_error());
}

TEST(ConfigLiveGates, CredentialsPrefixIsRequired) {
    std::string yaml(kLiveReadyYaml);
    const auto pos = yaml.find("credentials_env_prefix: BINANCE_LIVE");
    yaml.erase(pos, std::string("credentials_env_prefix: BINANCE_LIVE").size());
    const auto cfg = load_ok(yaml.c_str());
    EXPECT_TRUE(cfg.validate_live_gates(true).is_error());
}

TEST(ConfigLiveGates, AtLeastOneEnabledSymbolIsRequired) {
    std::string yaml(kLiveReadyYaml);
    const auto pos = yaml.find("  - symbol: BTCUSDT");
    yaml.replace(pos, std::string("  - symbol: BTCUSDT").size(),
                 "  - symbol: BTCUSDT\n    enabled: false");
    const auto cfg = load_ok(yaml.c_str());
    EXPECT_TRUE(cfg.validate_live_gates(true).is_error());
}

TEST(Config, ConfigFileNotFound) {
    const auto r = load_config_file("/nonexistent/path/config.yaml");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::NotFound);
}

TEST(Config, FindSymbol) {
    const auto cfg = load_ok("symbols: [BTCUSDT, ETHUSDT]\nstrategy: {name: t}");
    ASSERT_NE(cfg.find_symbol("ETHUSDT"), nullptr);
    EXPECT_EQ(cfg.find_symbol("ETHUSDT")->symbol, "ETHUSDT");
    EXPECT_EQ(cfg.find_symbol("SOLUSDT"), nullptr);
}

}  // namespace
}  // namespace mm
