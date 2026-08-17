/// \file test_accounting_pipeline.cpp
/// The pipeline with accounting closed into it:
///
///   Strategy -> Quote Manager -> Risk -> OMS -> Paper Execution
///            -> fills -> OMS  and  -> Accounting -> Risk (next cycle)
///
/// The fills that move the ledger are produced by the real simulated venue
/// matching against the real book -- not synthesised by the test.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "mm/exchange/paper/PaperExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "mm/portfolio/PortfolioAccountant.hpp"
#include "support/AccountingOracle.hpp"
#include "support/PaperFixtures.hpp"

namespace mm {
namespace {

using exchange::paper::PaperExecution;
using portfolio::AccountingConfig;
using portfolio::PortfolioAccountant;

/// Routes execution events to the OMS **and** to accounting.
///
/// Two consumers of one event stream, answering two different questions: the
/// OMS asks "what happened to this order", accounting asks "what do we now
/// hold". Neither derives its answer from the other, which is exactly the
/// ownership separation Phase 10 requires.
class LedgerSink final : public exchange::IExecutionSink {
public:
    LedgerSink(exchange::IExecutionSink& oms, PortfolioAccountant& accounting)
        : oms_(oms), accounting_(accounting) {}

    void on_execution(const exchange::ExecutionEvent& event) override {
        oms_.on_execution(event);
        if (event.type == exchange::ExecutionEventType::Fill) {
            const portfolio::FillOutcome out = accounting_.on_fill(event.payload.fill);
            if (portfolio::corrupts_ledger(out.error)) {
                ++rejected;
            }
            ++seen_fills;
            fills.push_back(event);
        }
    }

    std::uint64_t seen_fills = 0;
    std::uint64_t rejected = 0;
    std::vector<exchange::ExecutionEvent> fills;

private:
    exchange::IExecutionSink& oms_;
    PortfolioAccountant& accounting_;
};

class AccountingPipelineTest : public ::testing::Test {
protected:
    AccountingPipelineTest()
        : clock(millis(1'000), millis(1'000)),
          venue(test::paper_config(), clock),
          journal(),
          oms(oms_config(), journal, clock),
          accounting(accounting_config(), clock) {}

    void SetUp() override {
        book = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "50"}}, {{"60000.10", "50"}});
        ASSERT_EQ(accounting.register_symbol(Symbol("BTCUSDT")), portfolio::AccountingError::None);
        sink = std::make_unique<LedgerSink>(oms, accounting);
        ASSERT_TRUE(venue.start(*sink).is_ok());
        oms.attach_execution(venue);
        venue.attach_book(Symbol("BTCUSDT"), *book);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
        static_cast<void>(oms.process_events());
    }

    static oms::OmsConfig oms_config() {
        oms::OmsConfig c;
        static_cast<void>(c.client_id_prefix.assign("mmp"));
        c.session_id = 11;
        c.max_orders = 64;
        c.new_request_timeout_ns = seconds(5);
        c.cancel_request_timeout_ns = seconds(5);
        c.replace_request_timeout_ns = seconds(5);
        return c;
    }
    static AccountingConfig accounting_config() {
        AccountingConfig c;
        c.max_symbols = 4;
        return c;
    }

    void settle(Nanos advance = millis(4)) {
        clock.advance(advance);
        static_cast<void>(venue.poll());
        static_cast<void>(oms.process_events());
    }

    void reprice(const std::vector<std::pair<const char*, const char*>>& bids,
                 const std::vector<std::pair<const char*, const char*>>& asks) {
        test::load_book(*book, bids, asks, ++book_seq);
        venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
        settle();
        portfolio::MarkPrice mark;
        mark.symbol = Symbol("BTCUSDT");
        mark.bid = book->best_bid_price();
        mark.ask = book->best_ask_price();
        mark.as_of_ns = clock.steady();
        static_cast<void>(accounting.on_mark(mark));
    }

    ManualClock clock;
    std::unique_ptr<book::OrderBook> book;
    Seq book_seq = 100;
    PaperExecution venue;
    oms::NullOrderJournal journal;
    oms::OrderManager oms;
    PortfolioAccountant accounting;
    std::unique_ptr<LedgerSink> sink;

    /// The fill events the venue actually produced, for oracle comparison.
    [[nodiscard]] const std::vector<exchange::ExecutionEvent>& recorded_fills() const {
        return sink->fills;
    }
};

TEST_F(AccountingPipelineTest, AVenueFillMovesTheLedger) {
    // A resting bid, filled by the market coming to it.
    exchange::OrderRequest request = test::paper_order("a1", Side::Buy, "59900.00", "2");
    ASSERT_TRUE(venue.submit(request).is_ok());
    settle();
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});

    EXPECT_GE(sink->seen_fills, 1U);
    EXPECT_EQ(sink->rejected, 0U);

    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->position().to_string(), "2");
    EXPECT_EQ(a->average_price().to_string(), "59900");
    EXPECT_TRUE(a->realized().is_zero()) << "opening realizes nothing";
}

TEST_F(AccountingPipelineTest, ARoundTripRealizesPnlThroughTheWholeStack) {
    ASSERT_TRUE(venue.submit(test::paper_order("a1", Side::Buy, "59900.00", "2")).is_ok());
    settle();
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});
    ASSERT_EQ(accounting.find(Symbol("BTCUSDT"))->position().to_string(), "2");

    // Now sell it back higher.
    ASSERT_TRUE(venue.submit(test::paper_order("a2", Side::Sell, "60100.00", "2")).is_ok());
    settle();
    reprice({{"60100.00", "2"}}, {{"60200.00", "5"}});

    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    EXPECT_TRUE(a->is_flat());
    // (60100 - 59900) * 2 = 400.
    EXPECT_EQ(a->realized().to_string(), "400");
    EXPECT_TRUE(a->fees().is_positive()) << "the venue charged a fee and accounting kept it";
}

TEST_F(AccountingPipelineTest, TheFeeIsAFeeNotTheNotional) {
    // The Phase 9 regression, now covered end to end: the venue reports a fee,
    // accounting stores it, and it must remain a small fraction of the trade.
    ASSERT_TRUE(venue.submit(test::paper_order("a1", Side::Buy, "59900.00", "2")).is_ok());
    settle();
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});

    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    Notional cost{};
    ASSERT_TRUE(a->checked_cost_basis(cost));
    ASSERT_TRUE(a->fees().is_positive());
    EXPECT_LT(a->fees(), cost)
        << "fee " << a->fees().to_string() << " vs notional " << cost.to_string();
    // The paper venue's maker rate is 10 bps by default: ~120 on 119800.
    EXPECT_LT(a->fees().to_string().size(), cost.to_string().size());
}

TEST_F(AccountingPipelineTest, AFillForAnOrderTheOmsNeverIssuedStillMovesThePosition) {
    // This order was sent straight to the venue, bypassing the OMS -- which is
    // what a manual operator trade or another process on the same account
    // looks like from here.
    ASSERT_TRUE(venue.submit(test::paper_order("a1", Side::Buy, "59900.00", "2")).is_ok());
    settle();
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});

    // The OMS refuses to invent an order it never issued.
    EXPECT_EQ(oms.find(ClientOrderId("a1")), nullptr);
    EXPECT_GT(oms.metrics().quarantined_events + oms.metrics().orphan_orders, 0U);

    // Accounting records the position anyway, and must. A fill on our account
    // is our position regardless of who originated it; ignoring it would make
    // the system believe it holds less than it does, which is the one direction
    // that must never happen.
    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->position().to_string(), "2");

    // The two layers disagree here, and that disagreement is correct: they
    // answer different questions and neither derives its answer from the other.
    // Reconciliation (Phase 11) is what surfaces it to an operator.
    EXPECT_EQ(venue.find_order(ClientOrderId("a1"))->cumulative_qty, a->position())
        << "the ledger matches the venue, which is the authority on what filled";
}

TEST_F(AccountingPipelineTest, AccountingMatchesAnIndependentOracleOverTheRealPipeline) {
    test::AccountingOracle oracle;
    std::size_t fills = 0;

    struct Leg { const char* id; Side side; const char* price; const char* bid; const char* ask; };
    const std::vector<Leg> legs = {
        {"l1", Side::Buy,  "59900.00", "59800.00", "59900.00"},
        {"l2", Side::Buy,  "59850.00", "59750.00", "59850.00"},
        {"l3", Side::Sell, "60050.00", "60050.00", "60150.00"},
        {"l4", Side::Sell, "60100.00", "60100.00", "60200.00"},
    };
    for (const Leg& leg : legs) {
        ASSERT_TRUE(venue.submit(test::paper_order(leg.id, leg.side, leg.price, "1")).is_ok());
        settle();
        reprice({{leg.bid, "5"}}, {{leg.ask, "5"}});
    }

    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    ASSERT_NE(a, nullptr);

    // Rebuild the oracle from the fills the venue actually produced, so the
    // comparison is against real execution rather than an assumed one.
    for (const exchange::ExecutionEvent& e : recorded_fills()) {
        test::OracleFill of;
        of.buy = e.payload.fill.side == Side::Buy;
        of.price = test::as_real(e.payload.fill.price);
        of.quantity = test::as_real(e.payload.fill.quantity);
        of.fee = test::as_real(e.payload.fill.fee);
        oracle.apply(of);
        ++fills;
    }
    ASSERT_GT(fills, 0U);

    const long double eps = test::oracle_epsilon(fills, 100'000.0L);
    EXPECT_NEAR(static_cast<double>(test::as_real(a->position())),
                static_cast<double>(oracle.position()), static_cast<double>(eps));
    EXPECT_NEAR(static_cast<double>(test::as_real(a->realized())),
                static_cast<double>(oracle.realized()), static_cast<double>(eps));
    EXPECT_NEAR(static_cast<double>(test::as_real(a->fees())),
                static_cast<double>(oracle.fees()), static_cast<double>(eps));
}

TEST_F(AccountingPipelineTest, DuplicateVenueFillsDoNotDoubleCount) {
    ASSERT_TRUE(venue.submit(test::paper_order("a1", Side::Buy, "59900.00", "2")).is_ok());
    settle();
    venue.faults().duplicate_next_event = 1;
    reprice({{"59800.00", "1"}}, {{"59900.00", "2"}});

    // The venue emitted the fill twice; the ledger counted it once.
    const portfolio::PositionAccount* a = accounting.find(Symbol("BTCUSDT"));
    EXPECT_EQ(a->position().to_string(), "2");
    EXPECT_GE(accounting.metrics().duplicate_fills, 1U);
    EXPECT_EQ(sink->rejected, 0U) << "a duplicate is benign, not a ledger corruption";
}

}  // namespace
}  // namespace mm
