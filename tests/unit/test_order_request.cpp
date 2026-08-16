#include <gtest/gtest.h>

#include "mm/exchange/common/OrderRequest.hpp"
#include "mm/exchange/mock/MockExchangeExecution.hpp"

namespace mm::exchange {
namespace {

InstrumentSpec btc_spec() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    s.base = Asset("BTC");
    s.quote = Asset("USDT");
    EXPECT_TRUE(Px::parse("0.01", s.tick_size));
    EXPECT_TRUE(Qty::parse("0.00001", s.lot_size));
    EXPECT_TRUE(Qty::parse("0.00001", s.min_qty));
    EXPECT_TRUE(Qty::parse("100", s.max_qty));
    EXPECT_TRUE(Notional::parse("10", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

OrderRequest good_order() {
    OrderRequest r;
    r.client_order_id = ClientOrderId("mm-1-42");
    r.symbol = Symbol("BTCUSDT");
    r.side = Side::Buy;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::GTC;
    EXPECT_TRUE(Px::parse("60000.00", r.price));
    EXPECT_TRUE(Qty::parse("0.001", r.quantity));
    return r;
}

const ExchangeCapabilities& caps() {
    static const ExchangeCapabilities c = mock::permissive_mock_capabilities();
    return c;
}

TEST(ValidateOrder, AcceptsAWellFormedOrder) {
    const ExchangeError e = validate_order(good_order(), btc_spec(), caps());
    EXPECT_FALSE(e.is_error()) << e.to_string();
}

// Every validation failure must be NotSent: we refuse before a byte is written,
// so no order can exist and reconciliation must not be triggered.
TEST(ValidateOrder, EveryRejectionIsDefinitivelyNotSent) {
    const InstrumentSpec spec = btc_spec();
    std::vector<OrderRequest> bad;

    OrderRequest no_id = good_order();
    no_id.client_order_id.clear();
    bad.push_back(no_id);

    OrderRequest zero_qty = good_order();
    zero_qty.quantity = Qty::zero();
    bad.push_back(zero_qty);

    OrderRequest off_tick = good_order();
    ASSERT_TRUE(Px::parse("60000.005", off_tick.price));
    bad.push_back(off_tick);

    OrderRequest tiny = good_order();
    ASSERT_TRUE(Qty::parse("0.00001", tiny.quantity));
    bad.push_back(tiny);

    for (const auto& req : bad) {
        const ExchangeError e = validate_order(req, spec, caps());
        ASSERT_TRUE(e.is_error());
        EXPECT_EQ(e.outcome, RequestOutcome::NotSent) << e.to_string();
        EXPECT_FALSE(e.needs_reconciliation()) << e.to_string();
        EXPECT_TRUE(e.order_definitively_absent());
    }
}

TEST(ValidateOrder, RejectsOffTickPrice) {
    OrderRequest r = good_order();
    ASSERT_TRUE(Px::parse("60000.005", r.price));
    const ExchangeError e = validate_order(r, btc_spec(), caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::TickSizeViolation);
}

TEST(ValidateOrder, RejectsOffLotQuantity) {
    OrderRequest r = good_order();
    ASSERT_TRUE(Qty::parse("0.000015", r.quantity));
    const ExchangeError e = validate_order(r, btc_spec(), caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::LotSizeViolation);
}

TEST(ValidateOrder, RejectsNotionalBelowMinimum) {
    OrderRequest r = good_order();
    ASSERT_TRUE(Qty::parse("0.0001", r.quantity));  // 60000 * 0.0001 = 6 < 10
    const ExchangeError e = validate_order(r, btc_spec(), caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::NotionalTooSmall);
}

TEST(ValidateOrder, RejectsQuantityOutsideVenueBounds) {
    InstrumentSpec spec = btc_spec();
    OrderRequest r = good_order();
    ASSERT_TRUE(Qty::parse("200", r.quantity));
    const ExchangeError e = validate_order(r, spec, caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::QuantityOutOfBounds);
}

TEST(ValidateOrder, RejectsPriceOutsideVenueBand) {
    InstrumentSpec spec = btc_spec();
    ASSERT_TRUE(Px::parse("50000", spec.price_band_lo));
    ASSERT_TRUE(Px::parse("55000", spec.price_band_hi));
    const ExchangeError e = validate_order(good_order(), spec, caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::PriceOutOfBounds);
}

TEST(ValidateOrder, RejectsOrdersForANonTradingInstrument) {
    InstrumentSpec spec = btc_spec();
    spec.status = MarketStatus::Halted;
    const ExchangeError e = validate_order(good_order(), spec, caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::SymbolNotTrading);
}

TEST(ValidateOrder, RejectsWhenTheSpecIsNotLoaded) {
    // Orders cannot be validated before the venue's rules have arrived, and
    // "no rules" must not be read as "no restrictions".
    const InstrumentSpec empty;
    const ExchangeError e = validate_order(good_order(), empty, caps());
    ASSERT_TRUE(e.is_error());
}

TEST(ValidateOrder, RejectsSymbolMismatch) {
    OrderRequest r = good_order();
    r.symbol = Symbol("ETHUSDT");
    EXPECT_TRUE(validate_order(r, btc_spec(), caps()).is_error());
}

TEST(ValidateOrder, MarketOrderMustNotCarryAPrice) {
    OrderRequest r = good_order();
    r.type = OrderType::Market;
    r.tif = TimeInForce::IOC;
    // Silently dropping the price would hide a caller bug.
    const ExchangeError e = validate_order(r, btc_spec(), caps());
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.reason, RejectReason::PriceOutOfBounds);

    r.price = Px::zero();
    EXPECT_FALSE(validate_order(r, btc_spec(), caps()).is_error());
}

TEST(ValidateOrder, MarketOrderCannotBeGtc) {
    OrderRequest r = good_order();
    r.type = OrderType::Market;
    r.price = Px::zero();
    r.tif = TimeInForce::GTC;
    EXPECT_TRUE(validate_order(r, btc_spec(), caps()).is_error());
}

TEST(ValidateOrder, PostOnlyIsIncompatibleWithIocAndFok) {
    // "Rest or die" and "trade now or die" cannot both hold. The venue would
    // reject it, so refuse locally and save the round trip.
    for (const auto tif : {TimeInForce::IOC, TimeInForce::FOK}) {
        OrderRequest r = good_order();
        r.post_only = true;
        r.tif = tif;
        EXPECT_TRUE(validate_order(r, btc_spec(), caps()).is_error()) << to_string(tif);
    }
}

TEST(ValidateOrder, LimitMakerImpliesPostOnly) {
    OrderRequest r = good_order();
    r.type = OrderType::LimitMaker;
    EXPECT_TRUE(r.is_post_only());
    r.post_only = false;
    EXPECT_TRUE(r.is_post_only()) << "the order type alone must imply post-only";
}

TEST(ValidateOrder, UnsupportedFeaturesAreRefusedLocally) {
    const ExchangeCapabilities minimal = mock::minimal_mock_capabilities();

    OrderRequest post_only = good_order();
    post_only.post_only = true;
    ExchangeError e = validate_order(post_only, btc_spec(), minimal);
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.category, ExchangeErrorCategory::UnsupportedOperation);
    EXPECT_EQ(e.outcome, RequestOutcome::NotSent);

    OrderRequest reduce_only = good_order();
    reduce_only.reduce_only = true;
    e = validate_order(reduce_only, btc_spec(), minimal);
    ASSERT_TRUE(e.is_error());
    EXPECT_EQ(e.category, ExchangeErrorCategory::UnsupportedOperation);
}

TEST(ValidateOrder, RejectsClientOrderIdLongerThanTheVenueAccepts) {
    ExchangeCapabilities c = caps();
    c.max_client_order_id_len = 8;
    OrderRequest r = good_order();
    r.client_order_id = ClientOrderId("mm-session-000000042");
    // A truncated identifier is not an identifier; the order would become
    // unfindable at exactly the moment it matters.
    EXPECT_TRUE(validate_order(r, btc_spec(), c).is_error());
}

TEST(OrderRequest, NotionalUsesFixedPointThroughout) {
    const OrderRequest r = good_order();
    EXPECT_EQ(r.notional().to_string(), "60");
}

// ------------------------------------------------------------------- cancel

TEST(ValidateCancel, RequiresAnIdentifier) {
    CancelRequest c;
    c.symbol = Symbol("BTCUSDT");
    EXPECT_TRUE(validate_cancel(c, caps()).is_error());

    c.client_order_id = ClientOrderId("mm-1-42");
    EXPECT_FALSE(validate_cancel(c, caps()).is_error());

    CancelRequest by_exchange_id;
    by_exchange_id.symbol = Symbol("BTCUSDT");
    by_exchange_id.exchange_order_id = ExchangeOrderId("EX-1");
    EXPECT_FALSE(validate_cancel(by_exchange_id, caps()).is_error());
}

TEST(ValidateCancel, RequiresASymbol) {
    CancelRequest c;
    c.client_order_id = ClientOrderId("mm-1-42");
    EXPECT_TRUE(validate_cancel(c, caps()).is_error());
}

// ------------------------------------------------------------------ replace

TEST(ValidateReplace, RefusedOutrightWhereTheVenueLacksIt) {
    ReplaceRequest r;
    r.original_client_order_id = ClientOrderId("mm-1-42");
    r.new_client_order_id = ClientOrderId("mm-1-43");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60001.00", r.new_price));
    ASSERT_TRUE(Qty::parse("0.001", r.new_quantity));

    const ExchangeError e = validate_replace(r, btc_spec(), mock::minimal_mock_capabilities());
    ASSERT_TRUE(e.is_error());
    // Not silently decomposed into cancel-then-submit: the two have different
    // exposure profiles, and that choice belongs to the quote manager.
    EXPECT_EQ(e.category, ExchangeErrorCategory::UnsupportedOperation);
}

TEST(ValidateReplace, RequiresAFreshClientOrderId) {
    ReplaceRequest r;
    r.original_client_order_id = ClientOrderId("mm-1-42");
    r.new_client_order_id = ClientOrderId("mm-1-42");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60001.00", r.new_price));
    ASSERT_TRUE(Qty::parse("0.001", r.new_quantity));

    const ExchangeError e = validate_replace(r, btc_spec(), caps());
    ASSERT_TRUE(e.is_error());
    // Reusing the ID makes the two orders indistinguishable in out-of-order
    // reports -- the exact condition under which a replace goes wrong.
    EXPECT_EQ(e.reason, RejectReason::DuplicateClientOrderId);
}

TEST(ValidateReplace, AppliesTheSameGridRulesAsASubmit) {
    ReplaceRequest r;
    r.original_client_order_id = ClientOrderId("mm-1-42");
    r.new_client_order_id = ClientOrderId("mm-1-43");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60000.005", r.new_price));
    ASSERT_TRUE(Qty::parse("0.001", r.new_quantity));
    EXPECT_EQ(validate_replace(r, btc_spec(), caps()).reason, RejectReason::TickSizeViolation);
}

TEST(ValidateReplace, AcceptsAWellFormedReplace) {
    ReplaceRequest r;
    r.original_client_order_id = ClientOrderId("mm-1-42");
    r.new_client_order_id = ClientOrderId("mm-1-43");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60001.00", r.new_price));
    ASSERT_TRUE(Qty::parse("0.001", r.new_quantity));
    EXPECT_FALSE(validate_replace(r, btc_spec(), caps()).is_error());
}

}  // namespace
}  // namespace mm::exchange
