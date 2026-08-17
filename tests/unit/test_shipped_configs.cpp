#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "mm/common/Config.hpp"

namespace mm {
namespace {

/// The configs that ship with the repository are executable documentation. If
/// one of them stops loading, the first person to notice should be CI, not an
/// operator at 3am.
std::string config_path(const char* name) {
    return std::string(MMX_CONFIG_DIR) + "/" + name;
}

TEST(ShippedConfig, PaperLoadsAndValidates) {
    const auto r = load_config_file(config_path("paper.yaml"));
    ASSERT_TRUE(r.is_ok()) << r.status().to_string();
    const EngineConfig& cfg = r.value();
    EXPECT_EQ(cfg.mode, TradingMode::Paper);
    EXPECT_FALSE(cfg.safety.live_trading_enabled);
    EXPECT_TRUE(cfg.validate().is_ok());
    // Paper needs no live gates, with or without operator confirmation.
    EXPECT_TRUE(cfg.validate_live_gates(false).is_ok());
    EXPECT_TRUE(cfg.validate_live_gates(true).is_ok());
}

TEST(ShippedConfig, LiveLoadsAndPassesItsOwnGatesButHasNoAdapterToRunOn) {
    const auto r = load_config_file(config_path("live.yaml"));
    ASSERT_TRUE(r.is_ok()) << r.status().to_string();
    const EngineConfig& cfg = r.value();
    EXPECT_EQ(cfg.mode, TradingMode::Live);
    // Structurally valid, and every live *safety* gate it can satisfy is
    // satisfied -- the file is a working template for when live exists.
    EXPECT_EQ(cfg.effective_execution_mode(), ExecutionMode::Live);

    // And it still cannot run, because no live execution adapter exists yet
    // (Phase 9 §39). Failing here rather than falling back to paper is the
    // point: a run somebody believed was live must never quietly not be.
    const Status s = cfg.validate_live_gates(true);
    ASSERT_TRUE(s.is_error()) << "live must not be runnable before the adapter exists";
    EXPECT_NE(s.message().find("no live execution adapter"), std::string::npos)
        << s.to_string();
}

TEST(ShippedConfig, PaperConfigCannotSelectLiveExecution) {
    const auto r = load_config_file(config_path("paper.yaml"));
    ASSERT_TRUE(r.is_ok()) << r.status().to_string();
    // The shipped paper config is paper on both switches, and the availability
    // gate agrees. There is no path from this file to a real venue.
    EXPECT_EQ(r.value().mode, TradingMode::Paper);
    EXPECT_EQ(r.value().effective_execution_mode(), ExecutionMode::Paper);
    EXPECT_TRUE(r.value().validate_execution_available().is_ok());
    EXPECT_TRUE(r.value().validate_live_gates(false).is_ok());
}

TEST(ShippedConfig, LiveConfigStillRefusesWithoutTheOperatorFlag) {
    const auto r = load_config_file(config_path("live.yaml"));
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().validate_live_gates(false).is_error());
}

TEST(ShippedConfig, NeitherConfigContainsCredentials) {
    for (const char* name : {"paper.yaml", "live.yaml"}) {
        std::ifstream in(config_path(name));
        ASSERT_TRUE(in.good()) << name;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        for (const char* forbidden : {"api_key", "api_secret", "apiKey", "secretKey"}) {
            EXPECT_EQ(text.find(forbidden), std::string::npos)
                << name << " appears to contain a credential field: " << forbidden;
        }
    }
}

}  // namespace
}  // namespace mm
