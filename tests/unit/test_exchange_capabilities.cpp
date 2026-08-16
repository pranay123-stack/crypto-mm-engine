#include <gtest/gtest.h>

#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/exchange/mock/MockExchangeExecution.hpp"

namespace mm::exchange {
namespace {

constexpr Feature kAllFeatures[] = {
    Feature::PostOnly,      Feature::Replace,          Feature::ReduceOnly,
    Feature::ClientOrderId, Feature::MassCancel,       Feature::OrderQuery,
    Feature::NativeBbo,     Feature::OrderbookSnapshot, Feature::IncrementalDepth,
    Feature::TradeStream,   Feature::BalanceStream,    Feature::PositionStream,
};

TEST(Capabilities, DefaultsAreConservative) {
    // An undeclared capability must read as absent. Defaulting to "supported"
    // would let an adapter author silently inherit a feature it cannot honour.
    const ExchangeCapabilities c;
    EXPECT_FALSE(c.supports_post_only);
    EXPECT_FALSE(c.supports_replace);
    EXPECT_FALSE(c.supports_reduce_only);
    EXPECT_FALSE(c.supports_mass_cancel);
    EXPECT_FALSE(c.supports_native_bbo);
    EXPECT_FALSE(c.has_positions);
}

TEST(Capabilities, QueryMatchesTheUnderlyingFlags) {
    const ExchangeCapabilities permissive = mock::permissive_mock_capabilities();
    for (const auto f : kAllFeatures) {
        EXPECT_TRUE(supports(permissive, f)) << to_string(f);
    }

    const ExchangeCapabilities minimal = mock::minimal_mock_capabilities();
    EXPECT_FALSE(supports(minimal, Feature::PostOnly));
    EXPECT_FALSE(supports(minimal, Feature::Replace));
    EXPECT_FALSE(supports(minimal, Feature::PositionStream));
    EXPECT_TRUE(supports(minimal, Feature::ClientOrderId));
    EXPECT_TRUE(supports(minimal, Feature::IncrementalDepth));
}

TEST(Capabilities, NoneIsAlwaysSupported) {
    EXPECT_TRUE(supports(ExchangeCapabilities{}, Feature::None));
}

TEST(Capabilities, EveryFeatureHasAName) {
    for (const auto f : kAllFeatures) {
        EXPECT_NE(to_string(f), "UNKNOWN");
    }
}

TEST(Capabilities, RenderIsHumanReadableAndComplete) {
    const std::string s = mock::minimal_mock_capabilities().to_string();
    EXPECT_NE(s.find("post_only=0"), std::string::npos);
    EXPECT_NE(s.find("client_order_id=1"), std::string::npos);
    EXPECT_NE(s.find("max_coid_len=36"), std::string::npos);
}

TEST(Capabilities, ClientOrderIdLimitIsExpressedNotAssumed) {
    // The OMS sizes its ID scheme to the smallest configured venue; a hardcoded
    // 36 would silently truncate on a venue that allows fewer.
    ExchangeCapabilities c;
    c.max_client_order_id_len = 20;
    EXPECT_EQ(c.max_client_order_id_len, 20U);
}

}  // namespace
}  // namespace mm::exchange
