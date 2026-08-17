/// Phase 10 risk integration: the limits Phase 7 configured but could not
/// enforce (docs/risk-engine.md §16).
///
/// The engine remains the decision authority. Accounting supplies the numbers;
/// nothing here bypasses `RiskEngine::evaluate`.

#include <gtest/gtest.h>

#include "mm/portfolio/PortfolioAccountant.hpp"
#include "mm/risk/RiskEngine.hpp"
#include "support/RiskFixtures.hpp"

namespace mm::risk {
namespace {

using portfolio::AccountingConfig;
using portfolio::PortfolioAccountant;

Px px(const char* v) { Px p; EXPECT_TRUE(Px::parse(v, p)); return p; }
Notional notional(const char* v) { Notional n; EXPECT_TRUE(Notional::parse(v, n)); return n; }

class AccountingRiskTest : public ::testing::Test {
protected:
    AccountingRiskTest() : clock(millis(10'000), millis(10'000)) {}

    /// Limits with the Phase 10 additions switched on.
    static RiskLimits limits_with(const char* portfolio_cap, const char* daily_loss,
                                  const char* emergency) {
        SymbolLimits btc;
        btc.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Qty::parse("100", btc.max_position));
        EXPECT_TRUE(Notional::parse("10000000", btc.max_position_notional));
        EXPECT_TRUE(Qty::parse("50", btc.max_order_quantity));
        EXPECT_TRUE(Notional::parse("5000000", btc.max_order_notional));
        EXPECT_TRUE(Qty::parse("200", btc.max_working_exposure));
        EXPECT_TRUE(Qty::parse("100", btc.max_side_exposure));
        btc.max_open_orders = 64;
        btc.price_band_bps = 5'000;

        RiskLimits l;
        l.global.max_market_data_age_ns = seconds(60);
        l.global.max_position_age_ns = seconds(60);
        if (portfolio_cap != nullptr) {
            EXPECT_TRUE(Notional::parse(portfolio_cap, l.global.max_portfolio_notional));
        }
        if (daily_loss != nullptr) {
            EXPECT_TRUE(Notional::parse(daily_loss, l.global.max_daily_loss));
        }
        if (emergency != nullptr) {
            EXPECT_TRUE(Notional::parse(emergency, l.global.emergency_loss));
        }
        EXPECT_TRUE(l.add(btc));
        return l;
    }

    RiskInput input_with(const PortfolioState& portfolio) {
        RiskInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.position.valid = true;
        in.position.symbol = Symbol("BTCUSDT");
        in.position.sequence = 1;
        in.position.as_of_ns = clock.steady();
        in.exposure.determinate = true;
        in.bbo.bid_px = px("60000.00");
        in.bbo.ask_px = px("60000.10");
        EXPECT_TRUE(Qty::parse("5", in.bbo.bid_qty));
        EXPECT_TRUE(Qty::parse("5", in.bbo.ask_qty));
        in.system_ready = true;
        EXPECT_TRUE(in.expected_owner.assign("reference_mm_v1"));
        in.portfolio = portfolio;
        return in;
    }

    static quote::OrderAction new_order() {
        quote::OrderAction a;
        a.type = quote::OrderActionType::New;
        a.slot = quote::QuoteSlot::Bid;
        a.side = Side::Buy;
        a.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Px::parse("60000.00", a.price));
        EXPECT_TRUE(Qty::parse("1", a.quantity));
        EXPECT_TRUE(a.identity.name.assign("reference_mm_v1"));
        a.identity.version = 1;
        a.generation = 1;
        return a;
    }

    PortfolioState healthy(const char* gross, const char* net_pnl) {
        PortfolioState p;
        p.valid = true;
        p.pnl_determinate = true;
        p.gross_notional = notional(gross);
        p.net_pnl = notional(net_pnl);
        p.as_of_ns = clock.steady();
        p.sequence = 1;
        return p;
    }

    ManualClock clock;
    InstrumentSpec spec = test::btc_instrument();
};

TEST_F(AccountingRiskTest, PortfolioNotionalLimitIsNowEnforced) {
    RiskEngine engine(limits_with("100000", nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());

    EXPECT_TRUE(engine.evaluate(new_order(), input_with(healthy("50000", "0"))).is_approved());

    const RiskDecision over = engine.evaluate(new_order(), input_with(healthy("150000", "0")));
    EXPECT_FALSE(over.is_approved());
    EXPECT_EQ(over.reason, RiskReason::MaxPortfolioNotional);
}

TEST_F(AccountingRiskTest, LossLimitsAreNowEnforced) {
    RiskEngine engine(limits_with(nullptr, "500", "2000"), clock);
    ASSERT_TRUE(engine.arm().is_ok());

    EXPECT_TRUE(engine.evaluate(new_order(), input_with(healthy("0", "-100"))).is_approved());

    const RiskDecision daily = engine.evaluate(new_order(), input_with(healthy("0", "-600")));
    EXPECT_FALSE(daily.is_approved());
    EXPECT_EQ(daily.reason, RiskReason::MaxDailyLoss);

    // The emergency threshold takes precedence: it is the more severe fact.
    const RiskDecision emergency = engine.evaluate(new_order(), input_with(healthy("0", "-2500")));
    EXPECT_EQ(emergency.reason, RiskReason::EmergencyLoss);
}

TEST_F(AccountingRiskTest, ProfitIsNeverMistakenForALoss) {
    RiskEngine engine(limits_with(nullptr, "500", nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());
    // A sign error here would halt a profitable session. net_pnl is signed;
    // the limits are positive magnitudes.
    EXPECT_TRUE(engine.evaluate(new_order(), input_with(healthy("0", "5000"))).is_approved());
}

// ===========================================================================
// Fail-closed behaviour — the whole point of the integration
// ===========================================================================

TEST_F(AccountingRiskTest, AnAbsentPortfolioRefusesRatherThanPasses) {
    RiskEngine engine(limits_with("100000", nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());

    // A caller that supplies no portfolio gets a default-constructed one,
    // which is invalid. Silently passing would make the limit decorative.
    RiskInput in = input_with(PortfolioState{});
    const RiskDecision d = engine.evaluate(new_order(), in);
    EXPECT_FALSE(d.is_approved());
    EXPECT_EQ(d.reason, RiskReason::PortfolioUnavailable);
    EXPECT_TRUE(indicates_blindness(d.reason))
        << "this is 'we could not establish safety', not 'we checked and the answer was no'";
}

TEST_F(AccountingRiskTest, ADegradedLedgerRefusesNewExposure) {
    RiskEngine engine(limits_with("100000", nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());

    // Exactly what a faulted account produces.
    PortfolioState degraded = healthy("10", "0");
    degraded.valid = false;
    const RiskDecision d = engine.evaluate(new_order(), input_with(degraded));
    EXPECT_FALSE(d.is_approved());
    EXPECT_EQ(d.reason, RiskReason::PortfolioUnavailable);
}

TEST_F(AccountingRiskTest, IndeterminatePnlBlocksLossLimitsButNotNotionalLimits) {
    // An unmarkable symbol leaves exposure knowable and PnL not. The two
    // limits must therefore behave differently, and do.
    PortfolioState unmarked = healthy("50000", "0");
    unmarked.pnl_determinate = false;

    RiskEngine notional_only(limits_with("100000", nullptr, nullptr), clock);
    ASSERT_TRUE(notional_only.arm().is_ok());
    EXPECT_TRUE(notional_only.evaluate(new_order(), input_with(unmarked)).is_approved())
        << "exposure is known even when its mark is not";

    RiskEngine with_loss(limits_with(nullptr, "500", nullptr), clock);
    ASSERT_TRUE(with_loss.arm().is_ok());
    const RiskDecision d = with_loss.evaluate(new_order(), input_with(unmarked));
    EXPECT_FALSE(d.is_approved());
    EXPECT_EQ(d.reason, RiskReason::PnlIndeterminate)
        << "totalling only the markable symbols would understate the loss by the rest";
}

TEST_F(AccountingRiskTest, AStalePortfolioRefuses) {
    RiskEngine engine(limits_with("100000", nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());
    PortfolioState old = healthy("10", "0");
    clock.advance(seconds(120));
    const RiskDecision d = engine.evaluate(new_order(), input_with(old));
    EXPECT_FALSE(d.is_approved());
    EXPECT_EQ(d.reason, RiskReason::PortfolioStale);
}

TEST_F(AccountingRiskTest, UnconfiguredLimitsDoNotDemandAPortfolio) {
    // Phases 1-9 supply no portfolio. With the limits unset the engine must
    // behave exactly as it did before, or Phase 10 would have broken paper
    // trading for everyone who had not opted in.
    RiskEngine engine(limits_with(nullptr, nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());
    EXPECT_TRUE(engine.evaluate(new_order(), input_with(PortfolioState{})).is_approved());
}

TEST_F(AccountingRiskTest, CancellationSurvivesEveryPortfolioFailure) {
    RiskEngine engine(limits_with("1", "1", "1"), clock);
    ASSERT_TRUE(engine.arm().is_ok());

    quote::OrderAction cancel = new_order();
    cancel.type = quote::OrderActionType::Cancel;
    EXPECT_TRUE(cancel.target_client_order_id.assign("mm-1-a"));

    // Every portfolio limit is breached and the ledger is unusable. Withdrawing
    // must still be permitted -- that is the moment it matters most.
    const RiskDecision d = engine.evaluate(cancel, input_with(PortfolioState{}));
    EXPECT_TRUE(d.is_approved()) << to_string(d.reason);
}

// ===========================================================================
// End to end: real accountant, real engine
// ===========================================================================

TEST_F(AccountingRiskTest, ARealLedgerDrivesTheRealEngine) {
    AccountingConfig config;
    config.max_symbols = 4;
    PortfolioAccountant accountant(config, clock);
    ASSERT_EQ(accountant.register_symbol(Symbol("BTCUSDT")), portfolio::AccountingError::None);

    exchange::FillEvent fill;
    ASSERT_TRUE(fill.trade_id.assign("T1"));
    fill.symbol = Symbol("BTCUSDT");
    fill.side = Side::Buy;
    fill.price = px("60000.00");
    ASSERT_TRUE(Qty::parse("1", fill.quantity));
    ASSERT_TRUE(accountant.on_fill(fill).ok());

    portfolio::MarkPrice mark;
    mark.symbol = Symbol("BTCUSDT");
    mark.bid = px("60000.00");
    mark.ask = px("60000.10");
    mark.as_of_ns = clock.steady();
    ASSERT_EQ(accountant.on_mark(mark), portfolio::AccountingError::None);

    // 60000 of gross exposure against a 50000 cap, computed by the real
    // accountant and enforced by the real engine.
    RiskEngine engine(limits_with("50000", nullptr, nullptr), clock);
    ASSERT_TRUE(engine.arm().is_ok());
    const RiskDecision d = engine.evaluate(new_order(), input_with(accountant.risk_view()));
    EXPECT_FALSE(d.is_approved());
    EXPECT_EQ(d.reason, RiskReason::MaxPortfolioNotional);

    // And a larger cap lets it through.
    RiskEngine roomy(limits_with("500000", nullptr, nullptr), clock);
    ASSERT_TRUE(roomy.arm().is_ok());
    EXPECT_TRUE(roomy.evaluate(new_order(), input_with(accountant.risk_view())).is_approved());
}

}  // namespace
}  // namespace mm::risk
