#pragma once

/// Builders for risk-engine inputs, so tests read as scenarios.

#include "mm/risk/RiskEngine.hpp"

namespace mm::test {

using quote::OrderAction;
using quote::OrderActionType;
using quote::QuoteSlot;
using risk::ExposureSnapshot;
using risk::PositionSnapshot;
using risk::RiskInput;
using risk::RiskLimits;
using risk::SymbolLimits;

inline constexpr const char* kRiskOwner = "reference_mm_v1";

inline InstrumentSpec btc_instrument() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse("0.01", s.tick_size));
    EXPECT_TRUE(Qty::parse("0.001", s.lot_size));
    EXPECT_TRUE(Qty::parse("0.001", s.min_qty));
    EXPECT_TRUE(Qty::parse("1000", s.max_qty));
    EXPECT_TRUE(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

/// Limits sized so the interesting boundaries are reachable with round numbers:
/// max position 10 BTC, max order 2 BTC.
inline RiskLimits default_limits() {
    SymbolLimits btc;
    btc.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Qty::parse("10", btc.max_position));
    EXPECT_TRUE(Notional::parse("700000", btc.max_position_notional));
    EXPECT_TRUE(Qty::parse("2", btc.max_order_quantity));
    EXPECT_TRUE(Notional::parse("150000", btc.max_order_notional));
    EXPECT_TRUE(Qty::parse("20", btc.max_working_exposure));
    EXPECT_TRUE(Qty::parse("12", btc.max_side_exposure));
    btc.max_open_orders = 8;
    btc.price_band_bps = 500;

    RiskLimits limits;
    limits.global.max_market_data_age_ns = millis(500);
    limits.global.max_position_age_ns = seconds(5);
    limits.global.max_new_orders_per_second = 0;  // off unless a test enables it
    EXPECT_TRUE(limits.add(btc));
    return limits;
}

inline PositionSnapshot position_at(const char* quantity, Nanos as_of = 0) {
    PositionSnapshot p;
    p.valid = true;
    p.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Qty::parse(quantity, p.quantity));
    p.as_of_ns = as_of;
    p.sequence = 1;
    return p;
}

inline ExposureSnapshot exposure(const char* working_buy, const char* working_sell,
                                 std::uint32_t open_orders = 0) {
    ExposureSnapshot e;
    e.determinate = true;
    EXPECT_TRUE(Qty::parse(working_buy, e.working_buy));
    EXPECT_TRUE(Qty::parse(working_sell, e.working_sell));
    e.open_order_count = open_orders;
    return e;
}

inline BestBidAsk market(const char* bid = "60000.00", const char* ask = "60000.10") {
    BestBidAsk b;
    EXPECT_TRUE(Px::parse(bid, b.bid_px));
    EXPECT_TRUE(Qty::parse("5", b.bid_qty));
    EXPECT_TRUE(Px::parse(ask, b.ask_px));
    EXPECT_TRUE(Qty::parse("5", b.ask_qty));
    return b;
}

inline OrderAction new_order(Side side, const char* price, const char* quantity) {
    OrderAction a;
    a.type = OrderActionType::New;
    a.slot = quote::slot_of(side);
    a.side = side;
    a.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(Px::parse(price, a.price));
    EXPECT_TRUE(Qty::parse(quantity, a.quantity));
    static_cast<void>(a.identity.name.assign(kRiskOwner));
    a.identity.version = 1;
    a.generation = 1;
    return a;
}

inline OrderAction cancel_order(Side side) {
    OrderAction a;
    a.type = OrderActionType::Cancel;
    a.slot = quote::slot_of(side);
    a.side = side;
    a.symbol = Symbol("BTCUSDT");
    static_cast<void>(a.target_client_order_id.assign("mm-1"));
    static_cast<void>(a.identity.name.assign(kRiskOwner));
    a.identity.version = 1;
    a.generation = 1;
    return a;
}

inline OrderAction replace_order(Side side, const char* price, const char* quantity) {
    OrderAction a = new_order(side, price, quantity);
    a.type = OrderActionType::Replace;
    static_cast<void>(a.target_client_order_id.assign("mm-1"));
    return a;
}

}  // namespace mm::test
