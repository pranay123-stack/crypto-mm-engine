#include <gtest/gtest.h>

#include "mm/common/Types.hpp"

namespace mm {
namespace {

TEST(Side, OppositeAndSign) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
    EXPECT_EQ(sign_of(Side::Buy), 1);
    EXPECT_EQ(sign_of(Side::Sell), -1);
    EXPECT_EQ(to_string(Side::Buy), "BUY");
}

TEST(Enums, EveryValueHasAName) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(RejectReason::InternalError); ++i) {
        const auto r = static_cast<RejectReason>(i);
        if (r == RejectReason::Unknown) {
            continue;
        }
        EXPECT_NE(to_string(r), "UNKNOWN") << "RejectReason " << int{i} << " has no string form";
    }
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(MarketStatus::Delisted); ++i) {
        const auto s = static_cast<MarketStatus>(i);
        if (s == MarketStatus::Unknown) {
            continue;
        }
        EXPECT_NE(to_string(s), "UNKNOWN");
    }
}

TEST(EventStamps, LatencyDecomposition) {
    EventStamps st;
    st.recv_ns = 1'000;
    st.parse_ns = 3'500;
    st.process_ns = 9'000;
    EXPECT_EQ(st.decode_latency(), 2'500);
    EXPECT_EQ(st.queue_latency(), 5'500);
    EXPECT_EQ(st.ingress_latency(), 8'000);
}

BestBidAsk make_bbo(const char* bid, const char* bid_qty, const char* ask, const char* ask_qty) {
    BestBidAsk b;
    EXPECT_TRUE(Px::parse(bid, b.bid_px));
    EXPECT_TRUE(Qty::parse(bid_qty, b.bid_qty));
    EXPECT_TRUE(Px::parse(ask, b.ask_px));
    EXPECT_TRUE(Qty::parse(ask_qty, b.ask_qty));
    return b;
}

TEST(BestBidAsk, SanityPredicate) {
    EXPECT_TRUE(make_bbo("100", "1", "100.01", "1").is_sane());
    EXPECT_FALSE(make_bbo("100.02", "1", "100.01", "1").is_sane()) << "crossed book";
    EXPECT_FALSE(make_bbo("100", "1", "100", "1").is_sane()) << "locked book";
    EXPECT_FALSE(make_bbo("0", "1", "100.01", "1").is_sane()) << "one-sided";
    EXPECT_FALSE(make_bbo("100", "0", "100.01", "1").is_sane()) << "zero size";
}

TEST(BestBidAsk, MidAndSpread) {
    const BestBidAsk b = make_bbo("100", "1", "100.02", "1");
    EXPECT_EQ(b.mid().to_string(), "100.01");
    EXPECT_EQ(b.spread().to_string(), "0.02");
}

TEST(BestBidAsk, MicroPriceLeansTowardTheThinSide) {
    // Heavy bid, light ask -> fair value sits closer to the ask.
    const BestBidAsk b = make_bbo("100", "9", "101", "1");
    EXPECT_GT(b.micro_price(), b.mid());
    EXPECT_LT(b.micro_price(), b.ask_px);

    const BestBidAsk c = make_bbo("100", "1", "101", "9");
    EXPECT_LT(c.micro_price(), c.mid());
    EXPECT_GT(c.micro_price(), c.bid_px);
}

TEST(BestBidAsk, MicroPriceFallsBackToMidWithoutSize) {
    BestBidAsk b = make_bbo("100", "1", "101", "1");
    b.bid_qty = Qty::zero();
    b.ask_qty = Qty::zero();
    EXPECT_EQ(b.micro_price().raw(), b.mid().raw());
}

TEST(InstrumentSpec, ValidityRequiresTickAndLot) {
    InstrumentSpec s;
    EXPECT_FALSE(s.is_valid());
    s.symbol = Symbol("BTCUSDT");
    EXPECT_FALSE(s.is_valid());
    ASSERT_TRUE(Px::parse("0.01", s.tick_size));
    EXPECT_FALSE(s.is_valid());
    ASSERT_TRUE(Qty::parse("0.00001", s.lot_size));
    EXPECT_TRUE(s.is_valid());
}

}  // namespace
}  // namespace mm
