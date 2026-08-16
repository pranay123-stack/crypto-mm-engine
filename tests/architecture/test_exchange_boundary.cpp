/// \file test_exchange_boundary.cpp
/// The architectural acceptance tests for Phase 3.
///
/// These do not test behaviour; they test that the *shape* of the abstraction
/// holds. A behavioural bug costs a trade. A boundary that quietly erodes costs
/// the ability to add a second venue at all, and it erodes one reasonable-looking
/// include at a time -- which is why it is checked mechanically.

#include <gtest/gtest.h>

#include <type_traits>
#include <vector>

#include "mm/exchange/common/IExchangeExecution.hpp"
#include "mm/exchange/common/IExchangeMarketData.hpp"
#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/exchange/mock/MockExchangeMarketData.hpp"
#include "support/RecordingSinks.hpp"

namespace mm::exchange {
namespace {

// ===========================================================================
// Test A -- a concrete adapter satisfies both interfaces.
// ===========================================================================

TEST(ArchitectureA, MockSatisfiesBothInterfaces) {
    static_assert(std::is_base_of_v<IExchangeMarketData, mock::MockExchangeMarketData>);
    static_assert(std::is_base_of_v<IExchangeExecution, mock::MockExchangeExecution>);
    static_assert(!std::is_abstract_v<mock::MockExchangeMarketData>,
                  "the mock must implement every pure virtual, or the interface has a "
                  "member no adapter can realistically provide");
    static_assert(!std::is_abstract_v<mock::MockExchangeExecution>);

    ManualClock clock{0, 0};
    mock::MockExchangeMarketData md(clock);
    mock::MockExchangeExecution exec(clock);

    // Usable purely through base-class references: the engine never sees the
    // concrete type.
    IExchangeMarketData& md_iface = md;
    IExchangeExecution& exec_iface = exec;

    test::RecordingMarketDataSink md_sink;
    test::RecordingExecutionSink exec_sink;
    EXPECT_TRUE(md_iface.start(md_sink).is_ok());
    EXPECT_TRUE(exec_iface.start(exec_sink).is_ok());
    EXPECT_EQ(md_iface.venue(), VenueName("mock"));
    EXPECT_EQ(exec_iface.connection_state(), ConnectionState::Connected);
    md_iface.stop();
    exec_iface.stop();
}

// ===========================================================================
// Test C -- a *different* venue could implement the same interfaces.
//
// The mock alone proves little: an interface shaped around its only
// implementation will always fit that implementation. This second adapter is
// deliberately unlike the mock -- no atomic replace, a shorter id limit,
// derivatives-style positions, an opaque string id scheme -- and the engine
// still talks to it through the identical interface. That is the property a
// future Binance adapter depends on.
// ===========================================================================

class DifferentVenueExecution final : public IExchangeExecution {
public:
    DifferentVenueExecution() {
        caps_.supports_post_only = true;
        caps_.supports_replace = false;      // must be decomposed by the caller
        caps_.supports_reduce_only = true;   // derivatives venue
        caps_.has_positions = true;
        caps_.supports_position_stream = true;
        caps_.supports_mass_cancel = false;
        caps_.max_client_order_id_len = 20;  // shorter than the mock's 36
    }

    [[nodiscard]] Status start(IExecutionSink& sink) override {
        sink_ = &sink;
        return Status::ok();
    }
    void stop() override { sink_ = nullptr; }

    [[nodiscard]] Status submit(const OrderRequest& request) override {
        if (sink_ == nullptr) {
            return {ErrorCode::FailedPrecondition, "not started"};
        }
        // This venue answers with an opaque alphanumeric id, not a number.
        ExecutionEvent e;
        e.type = ExecutionEventType::OrderAck;
        e.payload.ack.client_order_id = request.client_order_id;
        e.payload.ack.exchange_order_id = ExchangeOrderId("ord_7f3a9c2e");
        e.payload.ack.symbol = request.symbol;
        e.payload.ack.original_qty = request.quantity;
        e.payload.ack.price = request.price;
        e.payload.ack.status = OrderStatus::New;
        sink_->on_execution(e);
        return Status::ok();
    }

    [[nodiscard]] Status cancel(const CancelRequest&) override { return Status::ok(); }

    [[nodiscard]] Status replace(const ReplaceRequest&) override {
        return {ErrorCode::InvalidArgument, "this venue has no atomic replace"};
    }
    [[nodiscard]] Status cancel_all(const Symbol&) override {
        return {ErrorCode::InvalidArgument, "this venue has no mass cancel"};
    }
    [[nodiscard]] Status query_open_orders(const Symbol&) override { return Status::ok(); }
    [[nodiscard]] Status query_order(const ClientOrderId&, const ExchangeOrderId&) override {
        return Status::ok();
    }
    [[nodiscard]] Status query_balances() override { return Status::ok(); }
    [[nodiscard]] Status query_positions() override { return Status::ok(); }

    [[nodiscard]] ConnectionState connection_state() const override {
        return ConnectionState::Connected;
    }
    [[nodiscard]] bool is_authenticated() const override { return true; }
    [[nodiscard]] std::size_t in_flight_requests() const override { return 0; }
    [[nodiscard]] const ExchangeCapabilities& capabilities() const override { return caps_; }
    [[nodiscard]] VenueName venue() const override { return VenueName("othervenue"); }

private:
    ExchangeCapabilities caps_{};
    IExecutionSink* sink_ = nullptr;
};

TEST(ArchitectureC, ADifferentVenueImplementsTheSameInterfaceUnchanged) {
    static_assert(!std::is_abstract_v<DifferentVenueExecution>);

    DifferentVenueExecution venue;
    test::RecordingExecutionSink sink;
    IExchangeExecution& iface = venue;
    ASSERT_TRUE(iface.start(sink).is_ok());

    OrderRequest r;
    r.client_order_id = ClientOrderId("mm-1-1");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60000.00", r.price));
    ASSERT_TRUE(Qty::parse("0.01", r.quantity));
    ASSERT_TRUE(iface.submit(r).is_ok());

    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].payload.ack.exchange_order_id, ExchangeOrderId("ord_7f3a9c2e"));
    EXPECT_TRUE(sink.invalid.empty());

    // Its differences are expressed through capabilities, not through a
    // different interface.
    EXPECT_FALSE(iface.capabilities().supports_replace);
    EXPECT_TRUE(iface.capabilities().has_positions);
    EXPECT_EQ(iface.capabilities().max_client_order_id_len, 20U);
    EXPECT_TRUE(iface.replace(ReplaceRequest{}).is_error());
}

TEST(ArchitectureC, TheEngineCanHoldEitherVenueBehindOnePointer) {
    ManualClock clock{0, 0};
    mock::MockExchangeExecution mock_venue(clock);
    DifferentVenueExecution other_venue;

    // This is the whole point: choosing a venue is choosing which object to
    // construct, not which code path to compile.
    std::vector<IExchangeExecution*> venues{&mock_venue, &other_venue};
    test::RecordingExecutionSink sink;
    for (IExchangeExecution* v : venues) {
        ASSERT_TRUE(v->start(sink).is_ok());
        EXPECT_FALSE(v->venue().empty());
        v->stop();
    }
}

// ===========================================================================
// Test D -- an unknown result stays UNKNOWN.
// ===========================================================================

TEST(ArchitectureD, UnknownIsNeverDowngradedToRejected) {
    ManualClock clock{0, 0};
    mock::MockExchangeExecution venue(clock);
    test::RecordingExecutionSink sink;
    ASSERT_TRUE(venue.start(sink).is_ok());
    sink.clear();

    venue.script_submit(mock::SubmitBehaviour::Timeout);
    OrderRequest r;
    r.client_order_id = ClientOrderId("mm-1-1");
    r.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Px::parse("60000.00", r.price));
    ASSERT_TRUE(Qty::parse("0.01", r.quantity));
    ASSERT_TRUE(venue.submit(r).is_ok());

    clock.advance(millis(10'000));
    venue.pump();

    ASSERT_EQ(sink.count_of(ExecutionEventType::OrderReject), 0U);
    const ExecutionEvent* failure = sink.first_of(ExecutionEventType::RequestFailure);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->outcome(), RequestOutcome::Unknown);
    EXPECT_TRUE(requires_reconciliation(failure->outcome()));
    EXPECT_FALSE(is_definitively_absent(failure->outcome()));
}

TEST(ArchitectureD, TheTypeSystemForbidsMappingSilenceToRejection) {
    // Not a convention: an adapter physically cannot construct a "timeout means
    // rejected" error, because `make` corrects it.
    for (const auto category : {ExchangeErrorCategory::Timeout, ExchangeErrorCategory::Transport,
                                ExchangeErrorCategory::MalformedResponse,
                                ExchangeErrorCategory::UnknownOrderState}) {
        const ExchangeError forged =
            ExchangeError::make(category, RequestOutcome::Rejected, "adapter bug");
        EXPECT_NE(forged.outcome, RequestOutcome::Rejected) << to_string(category);
    }
}

// ===========================================================================
// Test E -- normalized types carry no venue identity.
//
// The strategy layer arrives in Phase 5; what is checkable today is that the
// types it will consume are venue-free, which is the property that makes the
// Phase 5 guarantee possible.
// ===========================================================================

TEST(ArchitectureE, NormalizedTypesAreVenueAgnostic) {
    // Nothing a strategy will consume carries a venue name, a venue code, or a
    // venue-shaped identifier.
    OrderRequest r;
    r.symbol = Symbol("BTCUSDT");
    EXPECT_EQ(r.symbol.view(), "BTCUSDT") << "symbols are plain normalized strings";

    MarketDataEvent md;
    md.type = MarketDataEventType::Trade;
    md.payload.trade.symbol = Symbol("BTCUSDT");
    EXPECT_EQ(md.symbol(), Symbol("BTCUSDT"));

    // The raw venue code exists but is confined to the error, documented as
    // log-only, and never influences the normalized reason.
    const ExchangeError e =
        ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                            "", RejectReason::InsufficientBalance, -2010);
    EXPECT_EQ(e.reason, RejectReason::InsufficientBalance)
        << "callers branch on the normalized reason, never on the venue code";
}

TEST(ArchitectureE, StrategyFacingStateUsesFixedPointNotFloatingPoint) {
    // Money never becomes a double anywhere a strategy or the OMS can see it.
    static_assert(std::is_same_v<decltype(OrderRequest{}.price), Px>);
    static_assert(std::is_same_v<decltype(OrderRequest{}.quantity), Qty>);
    static_assert(std::is_same_v<decltype(FillEvent{}.fee), Notional>);
    static_assert(std::is_integral_v<Px::Raw>);
    SUCCEED();
}

// ===========================================================================
// Interface hygiene
// ===========================================================================

TEST(ArchitectureInterfaces, AreNonCopyableAndVirtuallyDestructible) {
    // An adapter owns threads and sockets; copying one would duplicate both.
    static_assert(!std::is_copy_constructible_v<IExchangeExecution>);
    static_assert(!std::is_copy_assignable_v<IExchangeExecution>);
    static_assert(!std::is_copy_constructible_v<IExchangeMarketData>);
    static_assert(std::has_virtual_destructor_v<IExchangeExecution>);
    static_assert(std::has_virtual_destructor_v<IExchangeMarketData>);
    static_assert(std::has_virtual_destructor_v<IExecutionSink>);
    static_assert(std::has_virtual_destructor_v<IMarketDataSink>);
    SUCCEED();
}

TEST(ArchitectureInterfaces, EveryFallibleOperationReturnsAStatus) {
    // Nothing on these interfaces throws; the trading thread does not unwind.
    static_assert(std::is_same_v<decltype(std::declval<IExchangeExecution&>().submit(
                                     std::declval<const OrderRequest&>())),
                                 Status>);
    static_assert(std::is_same_v<decltype(std::declval<IExchangeExecution&>().cancel(
                                     std::declval<const CancelRequest&>())),
                                 Status>);
    static_assert(std::is_same_v<decltype(std::declval<IExchangeMarketData&>().subscribe(
                                     std::declval<const SubscriptionRequest&>())),
                                 Status>);
    SUCCEED();
}

}  // namespace
}  // namespace mm::exchange
