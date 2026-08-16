#pragma once

/// Builders for quote-manager inputs, so tests read as scenarios rather than as
/// struct assembly.

#include "mm/quote/QuoteManager.hpp"

namespace mm::test {

using quote::PendingOperation;
using quote::QuoteManagerInput;
using quote::QuoteOwner;
using quote::QuoteSlot;
using quote::WorkingOrder;
using strategy::QuoteAction;
using strategy::QuoteIntent;

inline constexpr const char* kOwner = "reference_mm_v1";

inline InstrumentSpec btc_spec() {
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

inline exchange::ExchangeCapabilities caps_with_replace(bool supports_replace) {
    exchange::ExchangeCapabilities c;
    c.supports_client_order_id = true;
    c.supports_replace = supports_replace;
    return c;
}

/// A two-sided intent at the given prices.
inline QuoteIntent quoting_intent(std::uint64_t generation, const char* bid, const char* ask,
                                  const char* size = "0.001") {
    QuoteIntent intent;
    intent.action = QuoteAction::Quote;
    intent.quote_bid = true;
    intent.quote_ask = true;
    EXPECT_TRUE(Px::parse(bid, intent.bid_price));
    EXPECT_TRUE(Px::parse(ask, intent.ask_price));
    EXPECT_TRUE(Qty::parse(size, intent.bid_quantity));
    EXPECT_TRUE(Qty::parse(size, intent.ask_quantity));
    static_cast<void>(intent.identity.name.assign(kOwner));
    intent.identity.version = 1;
    intent.generation = generation;
    intent.market_sequence = 100 + generation;
    return intent;
}

inline QuoteIntent pull_intent(std::uint64_t generation) {
    QuoteIntent intent = QuoteIntent::pull(strategy::IntentReason::Normal);
    static_cast<void>(intent.identity.name.assign(kOwner));
    intent.identity.version = 1;
    intent.generation = generation;
    return intent;
}

/// A resting order owned by this manager.
inline WorkingOrder resting(QuoteSlot slot, const char* price, const char* quantity,
                            const char* filled = "0") {
    WorkingOrder order;
    order.present = true;
    static_cast<void>(order.owner.strategy.assign(kOwner));
    order.owner.slot = slot;
    order.side = quote::side_of(slot);
    EXPECT_TRUE(Px::parse(price, order.price));
    EXPECT_TRUE(Qty::parse(quantity, order.original_quantity));
    EXPECT_TRUE(Qty::parse(filled, order.cumulative_quantity));
    order.status = order.cumulative_quantity.is_positive() ? exchange::OrderStatus::PartiallyFilled
                                                           : exchange::OrderStatus::New;
    static_cast<void>(order.client_order_id.assign(
        slot == QuoteSlot::Bid ? "mm-bid-1" : "mm-ask-1"));
    static_cast<void>(order.exchange_order_id.assign("EX-1"));
    return order;
}

}  // namespace mm::test
