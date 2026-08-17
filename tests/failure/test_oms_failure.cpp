/// Phase 8 §31/§41/§42. The twelve invariants, the failure paths, and
/// deterministic generated event sequences that try to break them.

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <set>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "support/OmsFixtures.hpp"

namespace mm::oms {
namespace {

using exchange::mock::CancelBehaviour;
using exchange::mock::MockExchangeExecution;
using exchange::mock::ReplaceBehaviour;
using exchange::mock::SubmitBehaviour;
using test::kOmsOwner;
using test::oms_action;
using test::oms_config;
using test::RiskApprover;

class OmsFailureTest : public ::testing::Test {
protected:
    OmsFailureTest()
        : clock(millis(10'000), 0),
          venue(clock),
          approver(clock),
          oms(oms_config(), journal, clock) {}

    void SetUp() override {
        ASSERT_TRUE(venue.start(oms).is_ok());
        oms.attach_execution(venue);
        static_cast<void>(oms.process_events());
    }

    std::size_t pump(Nanos advance = millis(1)) {
        clock.advance(advance);
        static_cast<void>(venue.pump());
        return oms.process_events();
    }

    [[nodiscard]] SubmitResult submit_new(const char* price = "60000.00",
                                          const char* quantity = "1",
                                          Side side = Side::Buy,
                                          std::uint64_t generation = 1) {
        return oms.submit(
            approver.approve(oms_action(quote::OrderActionType::New, side, price, quantity,
                                        generation)));
    }

    [[nodiscard]] SubmitResult cancel_of(LogicalOrderId id, std::uint64_t generation = 1) {
        const OrderRecord* order = oms.find(id);
        EXPECT_NE(order, nullptr);
        auto action = oms_action(quote::OrderActionType::Cancel, order->side, nullptr, nullptr,
                                 generation);
        action.target_client_order_id = order->client_order_id;
        return oms.submit(approver.approve(action));
    }

    /// Checks the invariants that must hold after *every* event, not just at
    /// the end of a scenario.
    void check_universal_invariants() {
        std::set<std::string> live_client_ids;
        std::set<std::string> exchange_ids;
        for (std::uint64_t raw = 1; raw <= oms.order_count() + 8; ++raw) {
            const OrderRecord* o = oms.find(LogicalOrderId{raw});
            if (o == nullptr) {
                continue;
            }
            // #3, #4: quantity arithmetic cannot go backwards or overshoot.
            EXPECT_LE(o->cumulative_quantity, o->original_quantity)
                << "cumulative fill exceeded the order";
            EXPECT_FALSE(o->remaining().is_negative()) << "negative remaining quantity";
            // #12: one live logical order per client identity.
            if (o->consumes_exposure()) {
                EXPECT_TRUE(live_client_ids.insert(o->client_order_id.to_string()).second)
                    << "client id " << o->client_order_id.to_string() << " maps to two live orders";
            }
            // #11: one local order per venue identity.
            if (!o->exchange_order_id.empty()) {
                EXPECT_TRUE(exchange_ids.insert(o->exchange_order_id.to_string()).second)
                    << "exchange id maps to two local orders";
            }
        }
        EXPECT_EQ(oms.metrics().illegal_transitions, illegal_baseline)
            << "an illegal transition was applied";
    }

    ManualClock clock;
    MockExchangeExecution venue;
    RiskApprover approver;
    InMemoryOrderJournal journal;
    OrderManager oms;
    std::uint64_t illegal_baseline = 0;
};

// ===========================================================================
// §41 invariant 1 -- terminal orders never return to Working
// ===========================================================================

TEST_F(OmsFailureTest, Invariant1_TerminalOrderRejectsLaterEvents) {
    venue.script_submit(SubmitBehaviour::Reject);
    const SubmitResult result = submit_new();
    pump();
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::Rejected);
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    // A late ack for an order the venue already rejected. Believing it would
    // resurrect a dead order and put phantom exposure on the books.
    ASSERT_TRUE(venue.deliver_late_ack(id).is_ok());
    static_cast<void>(oms.process_events());

    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Rejected);
    // Refused by the terminal guard, not by event validation: the event is
    // well-formed, it just arrives for an order that is already dead.
    EXPECT_EQ(oms.metrics().quarantined_events, 0U);
    EXPECT_EQ(oms.metrics().duplicate_events, 1U);
}

TEST_F(OmsFailureTest, Invariant1_FilledOrderCannotBeCancelled) {
    venue.script_submit(SubmitBehaviour::AckThenFullFill);
    const SubmitResult result = submit_new();
    pump(millis(5));
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::Filled);

    const SubmitResult cancel = cancel_of(result.logical_id);
    EXPECT_FALSE(cancel.accepted);
    EXPECT_EQ(cancel.reject, OmsReject::TerminalOrder);
    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Filled);
}

TEST_F(OmsFailureTest, Invariant1_EveryTerminalStateIsAbsorbing) {
    for (const OrderState terminal : {OrderState::Filled, OrderState::Cancelled,
                                      OrderState::Rejected, OrderState::Expired}) {
        for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(OrderState::Orphaned); ++raw) {
            const auto to = static_cast<OrderState>(raw);
            if (to == terminal) {
                continue;
            }
            EXPECT_FALSE(is_legal_transition(terminal, to))
                << to_string(terminal) << " -> " << to_string(to);
        }
    }
}

// ===========================================================================
// §41 invariants 2-4 -- fill accounting
// ===========================================================================

TEST_F(OmsFailureTest, Invariant1_FillForATerminalOrderIsNotApplied) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;
    ASSERT_TRUE(cancel_of(result.logical_id).accepted);
    pump();
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::Cancelled);

    Px px;
    Qty qty;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.5", qty));
    // Two streams racing: the cancel confirmation arrived before a fill that
    // happened earlier.
    ASSERT_TRUE(venue.deliver_fill_unchecked(id, px, qty, TradeId("LATE1")).is_ok());
    static_cast<void>(oms.process_events());

    const OrderRecord* order = oms.find(result.logical_id);
    // Applying it would leave a CANCELLED order carrying fill quantity, and a
    // cancelled order has already released its exposure -- so the position
    // would be wrong in a record that looks entirely ordinary.
    EXPECT_EQ(order->state, OrderState::Cancelled);
    EXPECT_TRUE(order->cumulative_quantity.is_zero());
    EXPECT_GE(oms.metrics().quarantined_events, 1U);
    EXPECT_EQ(oms.metrics().count(OmsReject::TerminalOrder), 1U);
    check_universal_invariants();
}

TEST_F(OmsFailureTest, Invariant2_RepeatedDuplicateFillsNeverAccumulate) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    Px px;
    Qty qty;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.3", qty));
    ASSERT_TRUE(venue.deliver_fill(id, px, qty, Liquidity::Maker, TradeId("T1")).is_ok());
    static_cast<void>(oms.process_events());

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(venue.redeliver_last_event().is_ok());
        static_cast<void>(oms.process_events());
        EXPECT_EQ(oms.find(result.logical_id)->cumulative_quantity.to_string(), "0.3");
    }
    EXPECT_EQ(oms.metrics().duplicate_fills, 20U);
    check_universal_invariants();
}

TEST_F(OmsFailureTest, Invariant3_OverfillIsRefusedNotAbsorbed) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    Px px;
    Qty too_much;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("1.5", too_much));
    ASSERT_TRUE(venue.deliver_fill_unchecked(id, px, too_much, TradeId("T1")).is_ok());
    static_cast<void>(oms.process_events());

    const OrderRecord* order = oms.find(result.logical_id);
    // Clamping silently would make our position wrong in the direction we can
    // least afford: we would think we hold less than we do.
    EXPECT_LE(order->cumulative_quantity, order->original_quantity);
    EXPECT_GE(oms.metrics().quarantined_events, 1U);
    EXPECT_FALSE(order->remaining().is_negative());
}

TEST_F(OmsFailureTest, Invariant4_ManySmallFillsNeverGoNegative) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;
    Px px;
    Qty slice;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.05", slice));

    for (int i = 0; i < 20; ++i) {
        char trade[16];
        std::snprintf(trade, sizeof(trade), "T%d", i);
        static_cast<void>(venue.deliver_fill(id, px, slice, Liquidity::Maker, TradeId(trade)));
        static_cast<void>(oms.process_events());
        check_universal_invariants();
    }
    const OrderRecord* order = oms.find(result.logical_id);
    EXPECT_EQ(order->cumulative_quantity, order->original_quantity);
    EXPECT_EQ(order->state, OrderState::Filled);
}

// ===========================================================================
// §41 invariant 5 -- Unknown never silently becomes Cancelled
// ===========================================================================

TEST_F(OmsFailureTest, Invariant5_TimeoutProducesUnknownNotCancelled) {
    venue.script_submit(SubmitBehaviour::Timeout);
    const SubmitResult result = submit_new();
    clock.advance(seconds(30));
    oms.on_timer();

    const OrderRecord* order = oms.find(result.logical_id);
    EXPECT_EQ(order->state, OrderState::Unknown);
    EXPECT_NE(order->state, OrderState::Cancelled);
    // Unknown still consumes exposure: the order may well be resting.
    EXPECT_TRUE(order->consumes_exposure());
    EXPECT_FALSE(oms.exposure(Symbol("BTCUSDT")).determinate);
}

TEST_F(OmsFailureTest, Invariant5_UnknownIsOnlyResolvedByEvidence) {
    venue.script_submit(SubmitBehaviour::Timeout);
    const SubmitResult result = submit_new();
    clock.advance(seconds(30));
    oms.on_timer();
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::Unknown);

    // Repeated timer ticks are not evidence, however many of them there are.
    for (int i = 0; i < 10; ++i) {
        clock.advance(seconds(30));
        oms.on_timer();
    }
    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Unknown);

    // An explicit operator/reconciliation resolution is.
    EXPECT_TRUE(oms.resolve(result.logical_id, OrderState::Cancelled).is_ok());
    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Cancelled);
    EXPECT_TRUE(oms.exposure(Symbol("BTCUSDT")).determinate);
}

TEST_F(OmsFailureTest, Invariant5_TransportLossLeavesTheOutcomeUnknown) {
    venue.script_submit(SubmitBehaviour::TransportLoss);
    const SubmitResult result = submit_new();
    ASSERT_TRUE(result.accepted);
    pump();

    // A socket that died mid-write tells us nothing about whether the venue
    // read the request.
    const OrderRecord* order = oms.find(result.logical_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Unknown);
    EXPECT_TRUE(order->consumes_exposure());
}

// ===========================================================================
// §41 invariant 6 -- no order without risk approval
// ===========================================================================

TEST_F(OmsFailureTest, Invariant6_UnapprovedActionIsRefused) {
    // A default-constructed token is the only one a caller can build: the flag
    // that makes it valid is settable by RiskEngine alone.
    const risk::ApprovedAction forged{};
    ASSERT_FALSE(forged.is_valid());

    const SubmitResult result = oms.submit(forged);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reject, OmsReject::NotRiskApproved);
    EXPECT_EQ(oms.order_count(), 0U);
    EXPECT_EQ(oms.metrics().not_risk_approved, 1U);
}

TEST_F(OmsFailureTest, Invariant6_CopiedApprovalIsSingleUse) {
    const auto action = oms_action(quote::OrderActionType::New, Side::Buy, "60000.00", "1");
    const risk::ApprovedAction approval = approver.approve(action);
    const SubmitResult first = oms.submit(approval);
    ASSERT_TRUE(first.accepted);

    // Replaying the same approval must not place a second order. The pending
    // slot refuses it; the approval sequence records which one it was.
    const SubmitResult replay = oms.submit(approval);
    EXPECT_FALSE(replay.accepted);
    EXPECT_EQ(oms.order_count(), 1U);
}

TEST_F(OmsFailureTest, Invariant6_ApprovalSequenceIsMonotonic) {
    std::uint64_t previous = 0;
    for (int i = 0; i < 5; ++i) {
        const auto action = oms_action(quote::OrderActionType::New, Side::Buy, "60000.00", "1");
        const risk::ApprovedAction approval = approver.approve(action);
        EXPECT_GT(approval.approval_sequence(), previous);
        previous = approval.approval_sequence();
    }
}

// ===========================================================================
// §41 invariant 7 -- non-owned orders cannot be modified
// ===========================================================================

TEST_F(OmsFailureTest, Invariant7_ForeignOrderCannotBeCancelled) {
    const SubmitResult result = submit_new();
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    auto foreign = oms_action(quote::OrderActionType::Cancel, Side::Buy, nullptr, nullptr);
    foreign.target_client_order_id = id;
    static_cast<void>(foreign.identity.name.assign("some_other_strategy_v1"));

    // Risk approves it on its own terms -- a cancel reduces exposure, and the
    // owner matches the one risk was told to expect. Whether that strategy may
    // touch *this* order is the OMS's question, and the OMS refuses.
    const SubmitResult refused = oms.submit(approver.approve_as_owner(foreign));
    EXPECT_FALSE(refused.accepted);
    EXPECT_EQ(refused.reject, OmsReject::OwnershipViolation);
    EXPECT_EQ(oms.find(result.logical_id)->state, OrderState::Working);
    EXPECT_EQ(oms.metrics().ownership_violations, 1U);
}

// ===========================================================================
// §41 invariant 8 -- pending operations cannot duplicate requests
// ===========================================================================

TEST_F(OmsFailureTest, Invariant8_RepeatedCancelsSendOneRequest) {
    const SubmitResult result = submit_new();
    pump();
    ASSERT_TRUE(cancel_of(result.logical_id).accepted);

    for (int i = 0; i < 10; ++i) {
        const SubmitResult repeat = cancel_of(result.logical_id);
        EXPECT_FALSE(repeat.accepted);
        EXPECT_EQ(repeat.reject, OmsReject::DuplicateRequest);
    }
    EXPECT_EQ(oms.metrics().cancel_requests, 1U);
    EXPECT_EQ(oms.metrics().duplicate_requests, 10U);
}

TEST_F(OmsFailureTest, Invariant8_ReplaceWhilePendingReplaceCoalesces) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    auto first = oms_action(quote::OrderActionType::Replace, Side::Buy, "60001.00", "1", 2);
    first.target_client_order_id = id;
    ASSERT_TRUE(oms.submit(approver.approve(first)).accepted);

    auto second = oms_action(quote::OrderActionType::Replace, Side::Buy, "60002.00", "1", 3);
    second.target_client_order_id = id;
    const SubmitResult coalesced = oms.submit(approver.approve(second));

    // Two in-flight amendments of one order is how a venue ends up with two
    // orders. The newest target wins and no second request goes out.
    // Not "accepted" -- no request was issued -- but not dropped either.
    EXPECT_FALSE(coalesced.accepted);
    EXPECT_TRUE(coalesced.coalesced);
    EXPECT_EQ(oms.find(result.logical_id)->pending_price.to_string(), "60002")
        << "the newest desired state must be what the ack lands on";
    EXPECT_EQ(oms.metrics().replace_requests, 1U);
}

// ===========================================================================
// §41 invariant 9 -- the cancel-then-new boundary
// ===========================================================================

TEST_F(OmsFailureTest, Invariant9_NewIsNotCreatedWhileCancelIsUnresolved) {
    const SubmitResult first = submit_new("60000.00", "1");
    pump();
    venue.script_cancel(CancelBehaviour::Timeout);
    ASSERT_TRUE(cancel_of(first.logical_id).accepted);

    const SubmitResult replacement = submit_new("60001.00", "1");
    EXPECT_TRUE(replacement.deferred);

    // The cancel never resolves. The replacement must stay unborn rather than
    // join an order that might still be working.
    for (int i = 0; i < 5; ++i) {
        pump(seconds(1));
        EXPECT_EQ(oms.order_count(), 1U);
    }

    // Even after the request times out into Unknown, the boundary has not been
    // crossed -- Unknown is not a cancellation.
    clock.advance(seconds(30));
    oms.on_timer();
    EXPECT_EQ(oms.find(first.logical_id)->state, OrderState::Unknown);
    EXPECT_EQ(oms.order_count(), 1U) << "an unresolved cancel is not a cancellation boundary";
}

TEST_F(OmsFailureTest, Invariant9_RejectedCancelDiscardsTheDeferredNew) {
    const SubmitResult first = submit_new("60000.00", "1");
    pump();
    venue.script_cancel(CancelBehaviour::Reject);
    ASSERT_TRUE(cancel_of(first.logical_id).accepted);
    ASSERT_TRUE(submit_new("60001.00", "1").deferred);

    pump();
    // The cancel failed, so the original is still working. Creating the
    // replacement now would double the exposure.
    EXPECT_EQ(oms.find(first.logical_id)->state, OrderState::Working);
    EXPECT_EQ(oms.order_count(), 1U);
    EXPECT_EQ(oms.exposure(Symbol("BTCUSDT")).working_buy.to_string(), "1");
}

// ===========================================================================
// §41 invariant 10 -- determinism
// ===========================================================================

TEST_F(OmsFailureTest, Invariant10_SameEventSequenceGivesSameFinalState) {
    struct Outcome {
        OrderState state = OrderState::Created;
        std::string cumulative;
        std::string average;
        std::uint64_t journal_entries = 0;
    };

    const auto run = []() {
        ManualClock run_clock(millis(10'000), 0);
        MockExchangeExecution run_venue(run_clock);
        RiskApprover run_approver(run_clock);
        InMemoryOrderJournal run_journal;
        OrderManager run_oms(oms_config(), run_journal, run_clock);
        EXPECT_TRUE(run_venue.start(run_oms).is_ok());
        run_oms.attach_execution(run_venue);
        static_cast<void>(run_oms.process_events());

        const SubmitResult r = run_oms.submit(run_approver.approve(
            oms_action(quote::OrderActionType::New, Side::Buy, "60000.00", "1")));
        run_clock.advance(millis(1));
        static_cast<void>(run_venue.pump());
        static_cast<void>(run_oms.process_events());

        const ClientOrderId id = run_oms.find(r.logical_id)->client_order_id;
        Px px;
        Qty slice;
        EXPECT_TRUE(Px::parse("60000.00", px));
        EXPECT_TRUE(Qty::parse("0.25", slice));
        for (int i = 0; i < 4; ++i) {
            char trade[16];
            std::snprintf(trade, sizeof(trade), "T%d", i);
            static_cast<void>(
                run_venue.deliver_fill(id, px, slice, Liquidity::Maker, TradeId(trade)));
            static_cast<void>(run_venue.redeliver_last_event());
            static_cast<void>(run_oms.process_events());
        }
        const OrderRecord* o = run_oms.find(r.logical_id);
        return Outcome{o->state, o->cumulative_quantity.to_string(),
                       o->average_fill_price.to_string(),
                       static_cast<std::uint64_t>(run_journal.for_order(r.logical_id).size())};
    };

    const Outcome a = run();
    const Outcome b = run();
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(a.cumulative, b.cumulative);
    EXPECT_EQ(a.average, b.average);
    EXPECT_EQ(a.journal_entries, b.journal_entries);
    EXPECT_EQ(a.state, OrderState::Filled);
}

// ===========================================================================
// §41 invariants 11-12 -- identity uniqueness
// ===========================================================================

TEST_F(OmsFailureTest, Invariant11_EventForAnUnknownOrderChangesNothing) {
    static_cast<void>(submit_new());
    pump();
    const std::size_t before = oms.order_count();

    static_cast<void>(venue.deliver_event_for_unknown_order(ClientOrderId("someone-elses-1")));
    static_cast<void>(oms.process_events());

    // An event we cannot attribute is quarantined, never used to invent an
    // order record.
    EXPECT_EQ(oms.order_count(), before);
    EXPECT_GE(oms.metrics().quarantined_events, 1U);
    EXPECT_EQ(oms.find(ClientOrderId("someone-elses-1")), nullptr);
    check_universal_invariants();
}

TEST_F(OmsFailureTest, Invariant12_ClientIdIsNeverReusedWhileLive) {
    std::set<std::string> seen;
    for (int i = 0; i < 40; ++i) {
        const SubmitResult r = submit_new("60000.00", "1", i % 2 == 0 ? Side::Buy : Side::Sell);
        if (!r.accepted) {
            pump();
            continue;
        }
        pump();
        const OrderRecord* o = oms.find(r.logical_id);
        ASSERT_NE(o, nullptr);
        EXPECT_TRUE(seen.insert(o->client_order_id.to_string()).second);
        static_cast<void>(cancel_of(r.logical_id));
        pump();
        check_universal_invariants();
    }
}

// ===========================================================================
// §31/§40 -- out-of-order events, replace rejection, reconciliation
// ===========================================================================

TEST_F(OmsFailureTest, OutOfOrderAckAfterFillDoesNotUndoTheFill) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;

    Px px;
    Qty half;
    ASSERT_TRUE(Px::parse("60000.00", px));
    ASSERT_TRUE(Qty::parse("0.5", half));
    ASSERT_TRUE(venue.deliver_fill(id, px, half, Liquidity::Maker, TradeId("T1")).is_ok());
    static_cast<void>(oms.process_events());
    ASSERT_EQ(oms.find(result.logical_id)->state, OrderState::PartiallyFilled);

    // The ack for this order arrives after its own fill -- two venue streams
    // racing. Applying it as a fresh acknowledgement would erase the fill.
    ASSERT_TRUE(venue.deliver_late_ack(id).is_ok());
    static_cast<void>(oms.process_events());

    const OrderRecord* order = oms.find(result.logical_id);
    EXPECT_EQ(order->state, OrderState::PartiallyFilled);
    EXPECT_EQ(order->cumulative_quantity.to_string(), "0.5");
    EXPECT_EQ(oms.metrics().duplicate_events, 1U);
    // Classified as a duplicate, not as an illegal transition: nothing illegal
    // happened, the event simply carried no news.
    EXPECT_EQ(oms.metrics().illegal_transitions, 0U);
}

TEST_F(OmsFailureTest, RejectedReplaceLeavesTheOriginalUntouched) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const OrderRecord* before = oms.find(result.logical_id);
    const ClientOrderId original_id = before->client_order_id;
    const ExchangeOrderId original_venue_id = before->exchange_order_id;

    venue.script_replace(ReplaceBehaviour::Reject);
    auto replace = oms_action(quote::OrderActionType::Replace, Side::Buy, "60005.00", "2", 2);
    replace.target_client_order_id = original_id;
    ASSERT_TRUE(oms.submit(approver.approve(replace)).accepted);
    pump();

    const OrderRecord* after = oms.find(result.logical_id);
    EXPECT_EQ(after->state, OrderState::Working);
    EXPECT_EQ(after->price.to_string(), "60000") << "a refused amendment must not be applied";
    EXPECT_EQ(after->original_quantity.to_string(), "1");
    EXPECT_EQ(after->client_order_id, original_id);
    EXPECT_EQ(after->exchange_order_id, original_venue_id);
}

TEST_F(OmsFailureTest, UnsolicitedReplaceCannotDetachAnOrderFromItsIndex) {
    const SubmitResult result = submit_new("60000.00", "1");
    pump();
    const ClientOrderId id = oms.find(result.logical_id)->client_order_id;
    ASSERT_NE(oms.find(id), nullptr);

    // A replace-accepted for an amendment we never requested. There is no
    // pending identity to swap in, so nothing may be swapped out either.
    exchange::ExecutionEvent event{};
    event.type = exchange::ExecutionEventType::OrderReplace;
    event.payload.replace.original_client_order_id = id;
    static_cast<void>(event.payload.replace.new_client_order_id.assign("not-ours-1"));
    static_cast<void>(event.payload.replace.new_exchange_order_id.assign("VENUE-X"));
    event.payload.replace.symbol = Symbol("BTCUSDT");
    event.payload.replace.accepted = true;
    event.payload.replace.new_status = exchange::OrderStatus::New;
    ASSERT_TRUE(Px::parse("59000.00", event.payload.replace.new_price));
    ASSERT_TRUE(Qty::parse("1", event.payload.replace.new_qty));
    event.payload.replace.seq = 999;
    event.payload.replace.transact_ns = clock.wall();
    oms.on_execution(event);
    static_cast<void>(oms.process_events());

    // The order is still reachable by the identity we actually sent, so a
    // later fill for it can still be attributed.
    EXPECT_NE(oms.find(id), nullptr) << "the order was detached from its own index";
    EXPECT_EQ(oms.find(id)->logical_id, result.logical_id);
    check_universal_invariants();
}

TEST_F(OmsFailureTest, ReconciliationDetectsOrphansAndMissingOrders) {
    const SubmitResult mine = submit_new("60000.00", "1");
    pump();
    const OrderRecord* order = oms.find(mine.logical_id);

    VenueSnapshot snapshot;
    snapshot.is_complete = true;
    snapshot.symbol_filter = Symbol("BTCUSDT");
    clock.advance(millis(1));
    snapshot.received_ns = clock.steady();

    // The venue reports an order we do not have. It carries one of our client
    // ids, so it is ours and was lost, not somebody else's.
    exchange::OrderStatusReport orphan;
    static_cast<void>(orphan.client_order_id.assign("mm-16-zz"));
    static_cast<void>(orphan.exchange_order_id.assign("VENUE-ORPHAN"));
    orphan.symbol = Symbol("BTCUSDT");
    orphan.side = Side::Buy;
    orphan.status = exchange::OrderStatus::New;
    ASSERT_TRUE(Px::parse("59000.00", orphan.price));
    ASSERT_TRUE(Qty::parse("1", orphan.original_qty));
    snapshot.orders.push_back(orphan);

    // Our order is absent from the snapshot entirely.
    const ReconciliationReport report = oms.reconcile(snapshot);

    EXPECT_EQ(report.count_of(DiscrepancyKind::OrphanAtVenue), 1U);
    EXPECT_EQ(report.count_of(DiscrepancyKind::MissingAtVenue), 1U);
    EXPECT_FALSE(report.clean());
    EXPECT_EQ(oms.metrics().orphan_orders, 1U);
    EXPECT_EQ(oms.metrics().missing_orders, 1U);

    // A missing order is not proof it is gone; it becomes Unknown, not
    // Cancelled.
    EXPECT_EQ(oms.find(mine.logical_id)->state, OrderState::Unknown);
    static_cast<void>(order);
}

TEST_F(OmsFailureTest, IncompleteSnapshotIsRefused) {
    const SubmitResult mine = submit_new();
    pump();

    VenueSnapshot partial;
    partial.is_complete = false;  // a truncated or paginated query result
    partial.symbol_filter = Symbol("BTCUSDT");
    const ReconciliationReport report = oms.reconcile(partial);

    // Reconciling against a snapshot that may be missing rows would report
    // every real order as missing and cancel a working book.
    EXPECT_TRUE(report.clean());
    EXPECT_EQ(report.orders_compared, 0U);
    EXPECT_EQ(oms.find(mine.logical_id)->state, OrderState::Working);
    EXPECT_EQ(oms.metrics().reconciliations, 0U);
    EXPECT_EQ(oms.metrics().reconciliations_refused, 1U);
}

TEST_F(OmsFailureTest, ReconciliationDetectsQuantityDivergence) {
    const SubmitResult mine = submit_new("60000.00", "1");
    pump();
    const OrderRecord* order = oms.find(mine.logical_id);

    VenueSnapshot snapshot;
    snapshot.is_complete = true;
    snapshot.symbol_filter = Symbol("BTCUSDT");
    exchange::OrderStatusReport report_row;
    report_row.client_order_id = order->client_order_id;
    report_row.exchange_order_id = order->exchange_order_id;
    report_row.symbol = Symbol("BTCUSDT");
    report_row.side = Side::Buy;
    report_row.status = exchange::OrderStatus::PartiallyFilled;
    ASSERT_TRUE(Px::parse("60000.00", report_row.price));
    ASSERT_TRUE(Qty::parse("1", report_row.original_qty));
    // The venue has filled 0.4 that we never saw an event for.
    ASSERT_TRUE(Qty::parse("0.4", report_row.cumulative_qty));
    snapshot.orders.push_back(report_row);

    const ReconciliationReport result = oms.reconcile(snapshot);
    EXPECT_EQ(result.count_of(DiscrepancyKind::QuantityMismatch), 1U);
    // Reconciliation reports; it does not silently rewrite fill history.
    EXPECT_EQ(oms.find(mine.logical_id)->cumulative_quantity.to_string(), "0");
}

// ===========================================================================
// §40 -- restart recovery
// ===========================================================================

TEST_F(OmsFailureTest, RestartRecoveryPreservesIdentityAndDistrustsPending) {
    const SubmitResult acked = submit_new("60000.00", "1", Side::Buy);
    pump();
    const SubmitResult pending = submit_new("60000.10", "1", Side::Sell);
    ASSERT_TRUE(pending.accepted);
    ASSERT_EQ(oms.find(pending.logical_id)->state, OrderState::PendingNew);

    const RecoveryState state = oms.snapshot_for_recovery();

    InMemoryOrderJournal fresh_journal;
    OrderManager restarted(oms_config(), fresh_journal, clock);
    ASSERT_TRUE(restarted.restore(state).is_ok());

    // The working order survives with all three identities intact.
    const OrderRecord* recovered = restarted.find(acked.logical_id);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->state, OrderState::Working);
    EXPECT_EQ(recovered->client_order_id, oms.find(acked.logical_id)->client_order_id);
    EXPECT_EQ(recovered->exchange_order_id, oms.find(acked.logical_id)->exchange_order_id);

    // The in-flight one does not: we crashed without learning its outcome.
    const OrderRecord* uncertain = restarted.find(pending.logical_id);
    ASSERT_NE(uncertain, nullptr);
    EXPECT_EQ(uncertain->state, OrderState::Unknown);
    EXPECT_FALSE(restarted.exposure(Symbol("BTCUSDT")).determinate);

    // The journal records what the order actually was before the restart, not
    // the state we replaced it with.
    const auto entries = fresh_journal.for_order(pending.logical_id);
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.back().from_state, OrderState::PendingNew);
    EXPECT_EQ(entries.back().to_state, OrderState::Unknown);

    // New client ids continue past the ones already used, so a restart cannot
    // collide with an order still resting from the previous session.
    restarted.attach_execution(venue);
    const SubmitResult after = restarted.submit(approver.approve(
        oms_action(quote::OrderActionType::New, Side::Buy, "59000.00", "1", 9)));
    if (after.accepted) {
        EXPECT_NE(restarted.find(after.logical_id)->client_order_id,
                  recovered->client_order_id);
    }
}

// ===========================================================================
// Capacity -- the reservation that makes the record pointers safe
// ===========================================================================

TEST_F(OmsFailureTest, CapacityIsRefusedRatherThanGrown) {
    // `orders_` is reserved to max_orders and submits are refused at that
    // bound, so the vector never reallocates. That is what makes it safe for
    // an event handler to hold an OrderRecord* across a call that may create
    // another order -- release_deferred() does exactly this. If the bound were
    // ever checked after insertion instead of before, the pointer would dangle
    // and ASan would report a use-after-free here.
    const std::size_t capacity = oms_config().max_orders;
    std::size_t placed = 0;
    for (std::size_t i = 0; i < capacity + 8; ++i) {
        auto action = oms_action(quote::OrderActionType::New, Side::Buy, "60000.00", "1",
                                 static_cast<std::uint64_t>(i) + 1);
        // A distinct owner per order, so each occupies its own slot.
        static_cast<void>(action.identity.name.assign(
            ("owner_" + std::to_string(i)).c_str()));
        const SubmitResult r = oms.submit(approver.approve_as_owner(action));
        if (r.accepted) {
            ++placed;
        } else {
            EXPECT_EQ(r.reject, OmsReject::CapacityExhausted);
        }
        pump();
    }
    EXPECT_EQ(placed, capacity);
    EXPECT_EQ(oms.order_count(), capacity);
    EXPECT_GE(oms.metrics().count(OmsReject::CapacityExhausted), 8U);
    check_universal_invariants();
}

// ===========================================================================
// §42 -- generated event sequences
// ===========================================================================

/// A tiny deterministic generator. Not a framework: a counter and a table.
class SequenceGenerator {
public:
    explicit SequenceGenerator(std::uint64_t seed) : state_(seed | 1U) {}
    std::uint64_t next(std::uint64_t bound) {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return bound == 0 ? 0 : state_ % bound;
    }

private:
    std::uint64_t state_;
};

TEST_F(OmsFailureTest, PropertyGeneratedEventSequencesNeverBreakInvariants) {
    enum class Step : std::uint8_t {
        SubmitNew,
        SubmitCancel,
        SubmitReplace,
        Pump,
        SmallFill,
        DuplicateEvent,
        LateAck,
        UnknownOrderEvent,
        Timer,
        Reconcile,
    };
    constexpr std::array<Step, 10> kSteps{Step::SubmitNew,       Step::SubmitCancel,
                                          Step::SubmitReplace,   Step::Pump,
                                          Step::SmallFill,       Step::DuplicateEvent,
                                          Step::LateAck,         Step::UnknownOrderEvent,
                                          Step::Timer,           Step::Reconcile};

    std::uint64_t trade_counter = 0;
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        SequenceGenerator gen(seed * 2654435761U);
        for (int step = 0; step < 60; ++step) {
            const Step action = kSteps[gen.next(kSteps.size())];
            switch (action) {
                case Step::SubmitNew:
                    static_cast<void>(submit_new("60000.00", "0.5",
                                                 gen.next(2) == 0 ? Side::Buy : Side::Sell,
                                                 static_cast<std::uint64_t>(step) + 1));
                    break;
                case Step::SubmitCancel:
                case Step::SubmitReplace: {
                    for (std::uint64_t raw = 1; raw <= oms.order_count(); ++raw) {
                        const OrderRecord* o = oms.find(LogicalOrderId{raw});
                        if (o == nullptr || !o->is_live() || o->has_pending_request()) {
                            continue;
                        }
                        if (action == Step::SubmitCancel) {
                            static_cast<void>(cancel_of(o->logical_id,
                                                        static_cast<std::uint64_t>(step) + 1));
                        } else {
                            auto rep = oms_action(quote::OrderActionType::Replace, o->side,
                                                  "60001.00", "0.5",
                                                  static_cast<std::uint64_t>(step) + 1);
                            rep.target_client_order_id = o->client_order_id;
                            static_cast<void>(oms.submit(approver.approve(rep)));
                        }
                        break;
                    }
                    break;
                }
                case Step::Pump:
                    pump(millis(1 + static_cast<Nanos>(gen.next(3))));
                    break;
                case Step::SmallFill: {
                    for (std::uint64_t raw = 1; raw <= oms.order_count(); ++raw) {
                        const OrderRecord* o = oms.find(LogicalOrderId{raw});
                        if (o == nullptr || !o->is_live()) {
                            continue;
                        }
                        Px px;
                        Qty slice;
                        EXPECT_TRUE(Px::parse("60000.00", px));
                        EXPECT_TRUE(Qty::parse("0.1", slice));
                        char trade[24];
                        std::snprintf(trade, sizeof(trade), "G%llu",
                                      static_cast<unsigned long long>(++trade_counter));
                        static_cast<void>(venue.deliver_fill(o->client_order_id, px, slice,
                                                             Liquidity::Maker, TradeId(trade)));
                        static_cast<void>(oms.process_events());
                        break;
                    }
                    break;
                }
                case Step::DuplicateEvent:
                    static_cast<void>(venue.redeliver_last_event());
                    static_cast<void>(oms.process_events());
                    break;
                case Step::LateAck: {
                    for (std::uint64_t raw = 1; raw <= oms.order_count(); ++raw) {
                        const OrderRecord* o = oms.find(LogicalOrderId{raw});
                        if (o == nullptr) {
                            continue;
                        }
                        static_cast<void>(venue.deliver_late_ack(o->client_order_id));
                        static_cast<void>(oms.process_events());
                        break;
                    }
                    break;
                }
                case Step::UnknownOrderEvent:
                    static_cast<void>(
                        venue.deliver_event_for_unknown_order(ClientOrderId("ghost-order")));
                    static_cast<void>(oms.process_events());
                    break;
                case Step::Timer:
                    clock.advance(seconds(1 + static_cast<Nanos>(gen.next(8))));
                    oms.on_timer();
                    break;
                case Step::Reconcile: {
                    VenueSnapshot snapshot;
                    snapshot.is_complete = true;
                    snapshot.symbol_filter = Symbol("BTCUSDT");
                    static_cast<void>(oms.reconcile(snapshot));
                    break;
                }
            }
            // Illegal transitions are counted rather than applied, so the
            // baseline moves; what must never move is the arithmetic.
            illegal_baseline = oms.metrics().illegal_transitions;
            check_universal_invariants();
        }
    }
    // The sweep must actually have exercised the engine.
    EXPECT_GT(oms.metrics().orders_created, 20U);
    EXPECT_GT(oms.metrics().fills + oms.metrics().partial_fills, 5U);
}

}  // namespace
}  // namespace mm::oms
