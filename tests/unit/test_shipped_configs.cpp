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

TEST(ShippedConfig, LiveLoadsAndPassesItsOwnGates) {
    const auto r = load_config_file(config_path("live.yaml"));
    ASSERT_TRUE(r.is_ok()) << r.status().to_string();
    const EngineConfig& cfg = r.value();
    EXPECT_EQ(cfg.mode, TradingMode::Live);
    const Status s = cfg.validate_live_gates(true);
    EXPECT_TRUE(s.is_ok()) << s.to_string();
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
