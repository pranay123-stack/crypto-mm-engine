/// \file test_risk_boundary.cpp
/// Architectural acceptance tests for the risk boundary (§37, §41, §42).
///
/// Risk is the safety boundary. It must depend only on normalized abstractions,
/// must be identical for paper and live, and must be impossible to bypass.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/risk/RiskEngine.hpp"

namespace mm::risk {
namespace {

TEST(RiskBoundary, DecisionsAreAllocationFreeAndJournalable) {
    // Decisions are produced on the trading thread and journalled; anything
    // owning heap memory would allocate on the hot path.
    static_assert(std::is_trivially_copyable_v<RiskDecision>);
    static_assert(std::is_trivially_copyable_v<PositionSnapshot>);
    static_assert(std::is_trivially_copyable_v<ExposureSnapshot>);
    SUCCEED();
}

TEST(RiskBoundary, TheReasonIsMachineReadable) {
    // The primary field an operator, a dashboard or an incident review branches
    // on is an enum, never a formatted string.
    static_assert(std::is_enum_v<RiskReason>);
    static_assert(std::is_enum_v<RiskVerdict>);
    static_assert(std::is_enum_v<RiskState>);
    const RiskDecision d;
    EXPECT_EQ(d.verdict, RiskVerdict::Reject) << "the safe default is refusal";
    EXPECT_EQ(d.reason, RiskReason::None);
}

TEST(RiskBoundary, MoneyNeverBecomesFloatingPoint) {
    static_assert(std::is_same_v<decltype(PositionSnapshot{}.quantity), Qty>);
    static_assert(std::is_same_v<decltype(SymbolLimits{}.max_position), Qty>);
    static_assert(std::is_same_v<decltype(SymbolLimits{}.max_order_notional), Notional>);
    static_assert(std::is_same_v<decltype(ExposureSnapshot{}.working_buy), Qty>);
    static_assert(std::is_integral_v<Qty::Raw>);
    SUCCEED();
}

TEST(RiskBoundary, RiskIsIdenticalForPaperAndLive) {
    // Nothing in the engine's inputs or outputs mentions an execution mode, a
    // venue, or an adapter. It cannot tell which it is serving, which is what
    // makes paper and live risk-identical by construction rather than by
    // discipline.
    static_assert(std::is_same_v<decltype(RiskInput{}.instrument), const InstrumentSpec*>);
    ManualClock clock(0, 0);
    RiskEngine engine(RiskLimits{}, clock);
    EXPECT_EQ(engine.state(), RiskState::Disarmed);
    SUCCEED();
}

TEST(RiskBoundary, TheDefaultStateRefusesEverything) {
    // A risk engine that has not been armed must not be indistinguishable from
    // one that has. Construction alone permits nothing.
    ManualClock clock(0, 0);
    RiskEngine engine(RiskLimits{}, clock);
    EXPECT_FALSE(permits_new_exposure(engine.state()));
    EXPECT_FALSE(permits_new_exposure(RiskState::Disarmed));
    EXPECT_FALSE(permits_new_exposure(RiskState::Killed));
    EXPECT_FALSE(permits_new_exposure(RiskState::Faulted));
    EXPECT_FALSE(permits_new_exposure(RiskState::Warning));
    EXPECT_TRUE(permits_new_exposure(RiskState::Armed));
}

TEST(RiskBoundary, KillAndFaultRequireOperatorRecovery) {
    EXPECT_TRUE(needs_operator_recovery(RiskState::Killed));
    EXPECT_TRUE(needs_operator_recovery(RiskState::Faulted));
    EXPECT_FALSE(needs_operator_recovery(RiskState::Warning))
        << "reduce-only is a degraded mode, not a latched failure";
    EXPECT_FALSE(needs_operator_recovery(RiskState::Armed));
}

TEST(RiskBoundary, ARejectedDecisionCannotBeMistakenForAnApprovedOne) {
    // A caller that forwards `approved` without inspecting the verdict must
    // still send nothing.
    quote::OrderAction action;
    action.type = quote::OrderActionType::New;
    action.symbol = Symbol("BTCUSDT");
    ASSERT_TRUE(Qty::parse("1", action.quantity));

    const RiskDecision d =
        RiskDecision::rejected(action, RiskReason::RiskKilled, RiskState::Killed, 1'000);
    EXPECT_FALSE(d.is_approved());
    EXPECT_FALSE(d.permits_new_exposure());
    EXPECT_EQ(d.approved.type, quote::OrderActionType::Noop);
    EXPECT_TRUE(d.approved.quantity.is_zero());
}

TEST(RiskBoundary, BlindnessIsDistinguishedFromRefusal) {
    // "We could not establish safety" and "we established it and the answer was
    // no" both reject, but only the first means the engine is operating blind,
    // which is an operational problem in its own right.
    EXPECT_TRUE(indicates_blindness(RiskReason::PositionUnavailable));
    EXPECT_TRUE(indicates_blindness(RiskReason::UnknownExposure));
    EXPECT_TRUE(indicates_blindness(RiskReason::StaleMarket));
    EXPECT_TRUE(indicates_blindness(RiskReason::ArithmeticOverflow));

    EXPECT_FALSE(indicates_blindness(RiskReason::MaxPosition));
    EXPECT_FALSE(indicates_blindness(RiskReason::RiskKilled));
    EXPECT_FALSE(indicates_blindness(RiskReason::RateLimit));
}

TEST(RiskBoundary, ExposureMathIsCheckedNotAssumed) {
    // Every arithmetic entry point reports failure rather than wrapping, which
    // is what lets the engine refuse what it cannot compute.
    static_assert(std::is_same_v<decltype(compute_worst_case(std::declval<PositionSnapshot>(),
                                                             std::declval<ExposureSnapshot>(),
                                                             std::declval<WorstCaseExposure&>())),
                                 bool>);
    SUCCEED();
}

}  // namespace
}  // namespace mm::risk
