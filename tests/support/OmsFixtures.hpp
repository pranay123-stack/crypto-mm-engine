#pragma once

/// Builders for OMS tests, including the one thing a test cannot make for
/// itself: a valid risk approval.

#include "mm/oms/OrderManager.hpp"
#include "mm/risk/RiskEngine.hpp"

namespace mm::test {

using oms::OmsConfig;
using oms::OrderManager;
using oms::OrderState;
using quote::OrderAction;
using quote::OrderActionType;
using quote::QuoteSlot;

inline constexpr const char* kOmsOwner = "reference_mm_v1";

inline InstrumentSpec oms_instrument() {
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

inline OmsConfig oms_config() {
    OmsConfig c;
    static_cast<void>(c.client_id_prefix.assign("mm"));
    c.session_id = 42;
    c.max_client_id_length = 36;
    c.max_orders = 64;
    c.new_request_timeout_ns = seconds(5);
    c.cancel_request_timeout_ns = seconds(5);
    c.replace_request_timeout_ns = seconds(5);
    return c;
}

inline OrderAction oms_action(OrderActionType type, Side side, const char* price,
                              const char* quantity, std::uint64_t generation = 1) {
    OrderAction a;
    a.type = type;
    a.slot = quote::slot_of(side);
    a.side = side;
    a.symbol = Symbol("BTCUSDT");
    if (price != nullptr) {
        EXPECT_TRUE(Px::parse(price, a.price));
    }
    if (quantity != nullptr) {
        EXPECT_TRUE(Qty::parse(quantity, a.quantity));
    }
    static_cast<void>(a.identity.name.assign(kOmsOwner));
    a.identity.version = 1;
    a.generation = generation;
    return a;
}

/// The only way a test can obtain a valid approval: by running the real risk
/// engine. That is the point of the token -- a test cannot fabricate one any
/// more than production code can.
class RiskApprover {
public:
    explicit RiskApprover(const Clock& clock) : engine_(build_limits(), clock) {
        EXPECT_TRUE(engine_.arm().is_ok());
        instrument_ = oms_instrument();
    }

    /// Approves an action whose owner is not ours. Risk checks the owner it
    /// was told to expect; whether a *different* strategy may touch this
    /// particular order is the OMS's question, not risk's.
    [[nodiscard]] risk::ApprovedAction approve_as_owner(const OrderAction& action) {
        return approve_with_expected_owner(action, action.identity.name);
    }

    [[nodiscard]] risk::ApprovedAction approve(const OrderAction& action) {
        return approve_with_expected_owner(action, StrategyName(kOmsOwner));
    }

private:
    [[nodiscard]] risk::ApprovedAction approve_with_expected_owner(const OrderAction& action,
                                                                   const StrategyName& owner) {
        risk::RiskInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &instrument_;
        in.position.valid = true;
        in.position.symbol = Symbol("BTCUSDT");
        in.position.sequence = 1;
        in.exposure.determinate = true;
        EXPECT_TRUE(Px::parse("60000.00", in.bbo.bid_px));
        EXPECT_TRUE(Qty::parse("5", in.bbo.bid_qty));
        EXPECT_TRUE(Px::parse("60000.10", in.bbo.ask_px));
        EXPECT_TRUE(Qty::parse("5", in.bbo.ask_qty));
        in.system_ready = true;
        in.expected_owner = owner;

        const risk::RiskDecision decision = engine_.evaluate(action, in);
        EXPECT_TRUE(decision.is_approved())
            << "fixture produced an action risk refused: " << to_string(decision.reason);
        return decision.approval;
    }

    static risk::RiskLimits build_limits() {
        risk::SymbolLimits btc;
        btc.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Qty::parse("100", btc.max_position));
        EXPECT_TRUE(Notional::parse("10000000", btc.max_position_notional));
        EXPECT_TRUE(Qty::parse("50", btc.max_order_quantity));
        EXPECT_TRUE(Notional::parse("5000000", btc.max_order_notional));
        EXPECT_TRUE(Qty::parse("200", btc.max_working_exposure));
        EXPECT_TRUE(Qty::parse("100", btc.max_side_exposure));
        btc.max_open_orders = 64;
        btc.price_band_bps = 5'000;

        risk::RiskLimits l;
        l.global.max_market_data_age_ns = seconds(60);
        l.global.max_position_age_ns = seconds(60);
        EXPECT_TRUE(l.add(btc));
        return l;
    }

    risk::RiskEngine engine_;
    InstrumentSpec instrument_{};
};

}  // namespace mm::test
