/// \file test_paper_pipeline.cpp
/// The whole engine, end to end, against a simulated venue:
///
///     Market Data -> Order Book -> Strategy -> Quote Manager -> Risk
///                 -> OMS -> Paper Execution -> execution events -> OMS
///
/// Everything below the strategy is the production path. Only the venue is
/// simulated, and it is reached through the same interface a live adapter will
/// implement, so what this proves about the wiring transfers unchanged.

#include <gtest/gtest.h>

#include <memory>

#include "mm/exchange/paper/PaperExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "mm/quote/QuoteManager.hpp"
#include "mm/risk/RiskEngine.hpp"
#include "mm/strategy/StrategyRegistry.hpp"
#include "mm/strategy/StrategyRuntime.hpp"
#include "support/PaperFixtures.hpp"

namespace mm {
namespace {

using exchange::paper::PaperExecution;
using exchange::paper::PaperExecutionConfig;
using oms::OrderManager;
using oms::OrderState;
using quote::OrderActionType;
using quote::QuoteManager;
using quote::QuoteManagerInput;
using quote::QuoteManagerResult;
using strategy::StrategyRuntime;

constexpr const char* kStrategy = "reference_mm_v1";

/// Forwards every event to the OMS and keeps a copy.
///
/// The engine will need exactly this shape later for journalling, so it is not
/// a test-only contrivance: the venue emits once, and more than one consumer
/// may listen. Crucially the OMS still receives events through the same
/// interface, so nothing about the boundary changes.
class TeeSink final : public exchange::IExecutionSink {
public:
    explicit TeeSink(exchange::IExecutionSink& downstream) : downstream_(downstream) {}

    void on_execution(const exchange::ExecutionEvent& event) override {
        recorded.push_back(event);
        downstream_.on_execution(event);
    }

    /// Snapshot rows collected since the last drain, in arrival order.
    [[nodiscard]] std::vector<exchange::OrderStatusReport> take_snapshot_rows() {
        std::vector<exchange::OrderStatusReport> rows;
        for (const auto& e : recorded) {
            if (e.type != exchange::ExecutionEventType::OpenOrdersSnapshot) {
                continue;
            }
            for (std::uint8_t i = 0; i < e.payload.open_orders.count; ++i) {
                rows.push_back(e.payload.open_orders.orders[i]);
            }
        }
        recorded.clear();
        return rows;
    }

    std::vector<exchange::ExecutionEvent> recorded;

private:
    exchange::IExecutionSink& downstream_;
};

class PaperPipelineTest : public ::testing::Test {
protected:
    PaperPipelineTest()
        : clock(millis(1'000), millis(1'000)),
          venue(test::paper_config(), clock),
          risk(build_limits(), clock),
          oms(build_oms_config(), journal, clock) {}

    void SetUp() override {
        spec = instrument();
        book = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "5"}}, {{"60000.10", "5"}});

        auto created = strategy::StrategyRegistry::instance().create(kStrategy);
        ASSERT_TRUE(created.is_ok()) << created.status().to_string();
        strategy::StrategyInit init;
        init.symbol = Symbol("BTCUSDT");
        init.instrument = spec;
        init.identity = created.value()->identity();
        init.params.set("half_spread_bps", "5.0");
        init.params.set("quote_size", "0.010");
        runtime = std::make_unique<StrategyRuntime>(strategy::StrategyRuntimeConfig{},
                                                    std::move(created.value()), clock);
        ASSERT_TRUE(runtime->initialize(init).is_ok());
        runtime->start();

        manager = std::make_unique<QuoteManager>(StrategyName(kStrategy),
                                                 quote::QuoteManagerConfig{}, clock);
        ASSERT_TRUE(risk.arm().is_ok());

        // The engine wires the OMS to the venue through the interface. Neither
        // side names the other's type.
        tee = std::make_unique<TeeSink>(oms);
        ASSERT_TRUE(venue.start(*tee).is_ok());
        oms.attach_execution(venue);
        venue.attach_book(Symbol("BTCUSDT"), *book);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
        static_cast<void>(oms.process_events());
    }

    static oms::OmsConfig build_oms_config() {
        oms::OmsConfig c;
        static_cast<void>(c.client_id_prefix.assign("mmp"));
        c.session_id = 9;
        c.max_orders = 64;
        c.new_request_timeout_ns = millis(50);
        c.cancel_request_timeout_ns = millis(50);
        c.replace_request_timeout_ns = millis(50);
        return c;
    }

    static InstrumentSpec instrument() {
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

    static risk::RiskLimits build_limits() {
        risk::SymbolLimits btc;
        btc.symbol = Symbol("BTCUSDT");
        EXPECT_TRUE(Qty::parse("10", btc.max_position));
        EXPECT_TRUE(Notional::parse("1000000", btc.max_position_notional));
        EXPECT_TRUE(Qty::parse("5", btc.max_order_quantity));
        EXPECT_TRUE(Notional::parse("500000", btc.max_order_notional));
        EXPECT_TRUE(Qty::parse("20", btc.max_working_exposure));
        EXPECT_TRUE(Qty::parse("10", btc.max_side_exposure));
        btc.max_open_orders = 16;
        btc.price_band_bps = 5'000;
        risk::RiskLimits l;
        l.global.max_market_data_age_ns = seconds(60);
        l.global.max_position_age_ns = seconds(60);
        EXPECT_TRUE(l.add(btc));
        return l;
    }

    strategy::StrategyContext market_context() {
        strategy::StrategyContext c;
        c.symbol = Symbol("BTCUSDT");
        c.bbo = book->bbo();
        c.sequence = ++sequence;
        c.now_ns = clock.steady();
        c.instrument = &spec;
        c.session_state = exchange::SessionState::Ready;
        c.trigger = strategy::TriggerReason::BboChange;
        EXPECT_TRUE(Qty::parse("5", c.inventory.position_limit));
        return c;
    }

    /// One engine cycle: evaluate, quote, risk-check, submit. Working state and
    /// exposure come from the OMS, which learned them from the venue.
    std::size_t cycle() {
        const strategy::EvaluationResult evaluated = runtime->evaluate(market_context());
        EXPECT_TRUE(evaluated.accepted) << to_string(evaluated.rejection);

        QuoteManagerInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.capabilities = &venue.capabilities();
        in.system_ready = true;
        in.working = oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));

        const QuoteManagerResult quoted = manager->evaluate(evaluated.intent, in);
        std::size_t submitted = 0;
        for (std::uint8_t i = 0; i < quoted.action_count; ++i) {
            risk::RiskInput risk_in;
            risk_in.symbol = Symbol("BTCUSDT");
            risk_in.instrument = &spec;
            risk_in.position.valid = true;
            risk_in.position.symbol = Symbol("BTCUSDT");
            risk_in.position.sequence = sequence;
            risk_in.exposure = oms.exposure(Symbol("BTCUSDT"));
            risk_in.bbo = book->bbo();
            risk_in.system_ready = true;
            static_cast<void>(risk_in.expected_owner.assign(kStrategy));

            const risk::RiskDecision decision = risk.evaluate(quoted.actions[i], risk_in);
            if (decision.is_approved() && oms.submit(decision.approval).accepted) {
                ++submitted;
            }
        }
        settle();
        return submitted;
    }

    /// Advances time far enough for the venue to answer, and drains both
    /// queues. Two hops: venue -> sink -> OMS ring -> OMS state.
    void settle(Nanos advance = millis(3)) {
        clock.advance(advance);
        static_cast<void>(venue.poll());
        static_cast<void>(oms.process_events());
    }

    void reprice(const std::vector<std::pair<const char*, const char*>>& bids,
                 const std::vector<std::pair<const char*, const char*>>& asks) {
        test::load_book(*book, bids, asks, ++book_seq);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
        settle();
    }

    ManualClock clock;
    InstrumentSpec spec{};
    std::unique_ptr<book::OrderBook> book;
    Seq book_seq = 100;
    PaperExecution venue;
    risk::RiskEngine risk;
    oms::InMemoryOrderJournal journal;
    OrderManager oms;
    std::unique_ptr<StrategyRuntime> runtime;
    std::unique_ptr<QuoteManager> manager;
    std::unique_ptr<TeeSink> tee;
    std::uint64_t sequence = 500;

    /// Runs the venue's real open-orders query and collects the answer into the
    /// shape reconciliation consumes.
    oms::VenueSnapshot venue_snapshot() {
        static_cast<void>(tee->take_snapshot_rows());
        EXPECT_TRUE(venue.query_open_orders(Symbol("BTCUSDT")).is_ok());
        settle();
        oms::VenueSnapshot snapshot;
        snapshot.is_complete = true;
        snapshot.symbol_filter = Symbol("BTCUSDT");
        snapshot.received_ns = clock.steady();
        snapshot.orders = tee->take_snapshot_rows();
        return snapshot;
    }
};

// ===========================================================================
// §25 — the full pipeline
// ===========================================================================

TEST_F(PaperPipelineTest, QuotesTravelTheWholeWayAndComeBackAsWorkingState) {
    EXPECT_EQ(cycle(), 2U) << "one order per side reaches the venue";

    // The OMS believes what the venue told it, and the venue independently
    // holds the same two orders. Two separate states that agree because the
    // events made them agree -- not because they share an object.
    EXPECT_EQ(oms.live_order_count(), 2U);
    EXPECT_EQ(venue.resting_order_count(), 2U);

    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    EXPECT_TRUE(state.slot(quote::QuoteSlot::Bid).present);
    EXPECT_TRUE(state.slot(quote::QuoteSlot::Ask).present);

    // And the venue's identities are the ones the OMS recorded.
    const ClientOrderId bid_id = state.slot(quote::QuoteSlot::Bid).client_order_id;
    const exchange::paper::PaperOrder* at_venue = venue.find_order(bid_id);
    ASSERT_NE(at_venue, nullptr);
    EXPECT_EQ(oms.find(bid_id)->exchange_order_id, at_venue->exchange_order_id);
}

TEST_F(PaperPipelineTest, TheOmsIsPendingUntilTheVenueAnswers) {
    const strategy::EvaluationResult evaluated = runtime->evaluate(market_context());
    ASSERT_TRUE(evaluated.accepted);
    QuoteManagerInput in;
    in.symbol = Symbol("BTCUSDT");
    in.instrument = &spec;
    in.capabilities = &venue.capabilities();
    in.system_ready = true;
    in.working = oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const QuoteManagerResult quoted = manager->evaluate(evaluated.intent, in);
    ASSERT_GT(quoted.action_count, 0U);

    risk::RiskInput risk_in;
    risk_in.symbol = Symbol("BTCUSDT");
    risk_in.instrument = &spec;
    risk_in.position.valid = true;
    risk_in.position.symbol = Symbol("BTCUSDT");
    risk_in.position.sequence = sequence;
    risk_in.exposure = oms.exposure(Symbol("BTCUSDT"));
    risk_in.bbo = book->bbo();
    risk_in.system_ready = true;
    static_cast<void>(risk_in.expected_owner.assign(kStrategy));
    const risk::RiskDecision decision = risk.evaluate(quoted.actions[0], risk_in);
    ASSERT_TRUE(decision.is_approved());

    const oms::SubmitResult r = oms.submit(decision.approval);
    ASSERT_TRUE(r.accepted);

    // Steps 4-6 of §25, observed separately: the OMS is PendingNew, the venue
    // has not answered, and nothing has been assumed.
    EXPECT_EQ(oms.find(r.logical_id)->state, OrderState::PendingNew);
    EXPECT_TRUE(oms.find(r.logical_id)->exchange_order_id.empty());
    EXPECT_EQ(venue.order_count(), 0U) << "the request has not been processed yet";

    settle();
    EXPECT_EQ(oms.find(r.logical_id)->state, OrderState::Working);
    EXPECT_FALSE(oms.find(r.logical_id)->exchange_order_id.empty());
    EXPECT_EQ(venue.resting_order_count(), 1U);
}

TEST_F(PaperPipelineTest, AMarketMoveDrivesAReplacementThroughEveryLayer) {
    ASSERT_EQ(cycle(), 2U);
    const quote::WorkingQuoteState before =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const Px original_bid = before.slot(quote::QuoteSlot::Bid).price;

    // Steps 8-14: the market moves, the strategy re-quotes, and the change is
    // carried all the way to the venue and back.
    reprice({{"60500.00", "5"}}, {{"60500.10", "5"}});
    const std::size_t actions = cycle();
    EXPECT_GT(actions, 0U) << "a 500-point move must produce new quotes";

    const quote::WorkingQuoteState after =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    ASSERT_TRUE(after.slot(quote::QuoteSlot::Bid).present);
    EXPECT_NE(after.slot(quote::QuoteSlot::Bid).price, original_bid);
    EXPECT_EQ(oms.live_order_count(), venue.resting_order_count())
        << "the two states must agree once the events have been applied";
}

// ===========================================================================
// §26 — the fill pipeline
// ===========================================================================

TEST_F(PaperPipelineTest, AFillPropagatesToWorkingStateAndRiskExposure) {
    ASSERT_EQ(cycle(), 2U);
    const risk::ExposureSnapshot before = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_EQ(before.working_buy.to_string(), "0.01");

    // The market trades down through our bid. The fill originates in the
    // venue's matching, not in a test calling the OMS.
    reprice({{"59900.00", "1"}}, {{"59940.00", "0.004"}});

    const risk::ExposureSnapshot after = oms.exposure(Symbol("BTCUSDT"));
    EXPECT_LT(after.working_buy, before.working_buy) << "working exposure falls by what filled";
    EXPECT_TRUE(after.determinate);

    // And the same quantity is visible from both sides of the boundary.
    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const ClientOrderId bid_id = state.slot(quote::QuoteSlot::Bid).client_order_id;
    ASSERT_FALSE(bid_id.empty());
    EXPECT_EQ(oms.find(bid_id)->cumulative_quantity,
              venue.find_order(bid_id)->cumulative_qty);
}

TEST_F(PaperPipelineTest, AFullFillRetiresTheOrderOnBothSides) {
    ASSERT_EQ(cycle(), 2U);
    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const ClientOrderId bid_id = state.slot(quote::QuoteSlot::Bid).client_order_id;

    reprice({{"59000.00", "1"}}, {{"59100.00", "50"}});

    EXPECT_EQ(oms.find(bid_id)->state, OrderState::Filled);
    EXPECT_EQ(venue.find_order(bid_id)->status, exchange::OrderStatus::Filled);
    EXPECT_TRUE(oms.exposure(Symbol("BTCUSDT")).working_buy.is_zero());
}

// ===========================================================================
// §27 — the cancel pipeline
// ===========================================================================

TEST_F(PaperPipelineTest, WithdrawalTravelsToTheVenueAndBack) {
    ASSERT_EQ(cycle(), 2U);

    QuoteManagerInput in;
    in.symbol = Symbol("BTCUSDT");
    in.instrument = &spec;
    in.capabilities = &venue.capabilities();
    in.system_ready = true;
    in.working = oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));

    // The kill path: the quote manager withdraws everything it owns.
    const QuoteManagerResult pulled = manager->disable_all_quotes(in, quote::ActionReason::OperatorDisabled);
    ASSERT_EQ(pulled.count_of(OrderActionType::Cancel), 2U);

    for (std::uint8_t i = 0; i < pulled.action_count; ++i) {
        risk::RiskInput risk_in;
        risk_in.symbol = Symbol("BTCUSDT");
        risk_in.instrument = &spec;
        risk_in.position.valid = true;
        risk_in.position.symbol = Symbol("BTCUSDT");
        risk_in.position.sequence = sequence;
        risk_in.exposure = oms.exposure(Symbol("BTCUSDT"));
        risk_in.bbo = book->bbo();
        risk_in.system_ready = true;
        static_cast<void>(risk_in.expected_owner.assign(kStrategy));
        const risk::RiskDecision d = risk.evaluate(pulled.actions[i], risk_in);
        ASSERT_TRUE(d.is_approved()) << to_string(d.reason);
        EXPECT_TRUE(oms.submit(d.approval).accepted);
    }

    // Requested but not confirmed: exposure is still held, because a cancel is
    // a request and not a result.
    EXPECT_EQ(oms.exposure(Symbol("BTCUSDT")).working_buy.to_string(), "0.01");

    settle();
    EXPECT_EQ(oms.live_order_count(), 0U);
    EXPECT_EQ(venue.resting_order_count(), 0U);
    EXPECT_TRUE(oms.exposure(Symbol("BTCUSDT")).working_buy.is_zero());
}

TEST_F(PaperPipelineTest, ARefusedCancelLeavesTheOrderWorkingEverywhere) {
    ASSERT_EQ(cycle(), 2U);
    const quote::WorkingQuoteState state =
        oms.working_state(Symbol("BTCUSDT"), StrategyName(kStrategy));
    const ClientOrderId bid_id = state.slot(quote::QuoteSlot::Bid).client_order_id;

    venue.faults().reject_next_cancel = 1;
    auto action = quote::OrderAction{};
    action.type = OrderActionType::Cancel;
    action.slot = quote::QuoteSlot::Bid;
    action.side = Side::Buy;
    action.symbol = Symbol("BTCUSDT");
    action.target_client_order_id = bid_id;
    static_cast<void>(action.identity.name.assign(kStrategy));
    action.identity.version = 1;
    action.generation = 99;

    risk::RiskInput risk_in;
    risk_in.symbol = Symbol("BTCUSDT");
    risk_in.instrument = &spec;
    risk_in.position.valid = true;
    risk_in.position.symbol = Symbol("BTCUSDT");
    risk_in.position.sequence = sequence;
    risk_in.exposure = oms.exposure(Symbol("BTCUSDT"));
    risk_in.bbo = book->bbo();
    risk_in.system_ready = true;
    static_cast<void>(risk_in.expected_owner.assign(kStrategy));
    const risk::RiskDecision d = risk.evaluate(action, risk_in);
    ASSERT_TRUE(d.is_approved());
    ASSERT_TRUE(oms.submit(d.approval).accepted);
    settle();

    // Both sides agree it is still there. Assuming otherwise would silently
    // drop exposure that genuinely exists.
    EXPECT_EQ(oms.find(bid_id)->state, OrderState::Working);
    EXPECT_TRUE(venue.find_order(bid_id)->is_resting());
    EXPECT_EQ(oms.exposure(Symbol("BTCUSDT")).working_buy.to_string(), "0.01");
}

// ===========================================================================
// §20 — reconciliation across the boundary
// ===========================================================================

TEST_F(PaperPipelineTest, ReconciliationAgreesWhenBothSidesAgree) {
    ASSERT_EQ(cycle(), 2U);

    // Built from what the venue actually reports, through the real query path
    // -- not from the OMS's own beliefs, which would make agreement circular.
    const oms::VenueSnapshot snapshot = venue_snapshot();
    ASSERT_EQ(snapshot.orders.size(), 2U);

    const oms::ReconciliationReport report = oms.reconcile(snapshot);
    EXPECT_TRUE(report.clean()) << "two independent states, agreeing because the events made them";
    EXPECT_EQ(report.orders_agreed, 2U);
}

TEST_F(PaperPipelineTest, ReconciliationFindsAnOrderTheVenueHides) {
    ASSERT_EQ(cycle(), 2U);

    venue.faults().hide_next_snapshot_orders = 1;
    const oms::VenueSnapshot snapshot = venue_snapshot();
    ASSERT_EQ(snapshot.orders.size(), 1U);

    const oms::ReconciliationReport report = oms.reconcile(snapshot);
    EXPECT_EQ(report.count_of(oms::DiscrepancyKind::MissingAtVenue), 1U);
    // Not assumed cancelled: the OMS marks it Unknown and demands evidence.
    EXPECT_FALSE(oms.exposure(Symbol("BTCUSDT")).determinate);
}

}  // namespace
}  // namespace mm
