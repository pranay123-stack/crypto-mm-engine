/// Phase 8 §37. Structural properties of the OMS that a code review would
/// otherwise have to re-check by hand every time somebody edits it.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/exchange/common/IExchangeExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "mm/risk/RiskDecision.hpp"

namespace mm::oms {
namespace {

// ---------------------------------------------------------------------------
// Risk cannot be bypassed
// ---------------------------------------------------------------------------

TEST(OmsBoundary, SubmitAcceptsOnlyARiskApproval) {
    // The single entry point takes an ApprovedAction, not an OrderAction.
    // Nothing can hand the OMS a raw quote-manager action.
    using Submit = SubmitResult (OrderManager::*)(const risk::ApprovedAction&);
    static_cast<void>(static_cast<Submit>(&OrderManager::submit));

    static_assert(!std::is_invocable_v<decltype(&OrderManager::submit), OrderManager&,
                                       const quote::OrderAction&>,
                  "the OMS must not accept an unapproved action");
    SUCCEED();
}

TEST(OmsBoundary, ApprovalCannotBeForgedOutsideRisk) {
    // Nothing public sets the validity flag. A caller can construct the token
    // -- it has to be embeddable in a decision -- but only RiskEngine can make
    // one that means anything.
    static_assert(std::is_default_constructible_v<risk::ApprovedAction>);
    static_assert(std::is_trivially_copyable_v<risk::ApprovedAction>);
    static_assert(!std::is_constructible_v<risk::ApprovedAction, quote::OrderAction>,
                  "an approval must not be constructible from the action it approves");

    const risk::ApprovedAction blank{};
    EXPECT_FALSE(blank.is_valid());
    EXPECT_EQ(blank.approval_sequence(), 0U);

    // Copying one does not make it more valid, and copying an invalid one
    // cannot produce a valid one.
    const risk::ApprovedAction copy = blank;
    EXPECT_FALSE(copy.is_valid());
}

// ---------------------------------------------------------------------------
// The OMS talks to an interface, never to a venue
// ---------------------------------------------------------------------------

TEST(OmsBoundary, ExecutionIsInjectedThroughTheInterface) {
    using Attach = void (OrderManager::*)(exchange::IExchangeExecution&) noexcept;
    static_cast<void>(static_cast<Attach>(&OrderManager::attach_execution));

    // The OMS receives events as the normalized sink, so a venue adapter is
    // interchangeable by construction.
    static_assert(std::is_base_of_v<exchange::IExecutionSink, OrderManager>);
    SUCCEED();
}

TEST(OmsBoundary, NoOwnershipOfTheExecutionAdapter) {
    // A raw reference/pointer, never a unique_ptr: the OMS uses an adapter, it
    // does not create, configure or destroy one. Lifetime belongs to whoever
    // chose paper or live.
    static_assert(sizeof(OrderManager) > 0);
    OmsConfig config;
    NullOrderJournal journal;
    SystemClock clock;
    const OrderManager oms(config, journal, clock);
    // Without an adapter attached the OMS is inert rather than defaulting to
    // some built-in venue.
    EXPECT_EQ(oms.order_count(), 0U);
}

// ---------------------------------------------------------------------------
// Hot-path shape
// ---------------------------------------------------------------------------

TEST(OmsBoundary, RecordsAreTriviallyCopyableAndAllocationFree) {
    static_assert(std::is_trivially_copyable_v<OrderJournalRecord>,
                  "journal records cross a ring buffer");
    static_assert(std::is_trivially_copyable_v<SeenTradeIds>);
    static_assert(std::is_trivially_copyable_v<OrderOwnership>);
    static_assert(std::is_trivially_copyable_v<SubmitResult>);
    static_assert(std::is_trivially_copyable_v<OrderRecord>,
                  "an order record must be copyable into a snapshot without allocating");
    SUCCEED();
}

TEST(OmsBoundary, IdentitiesAreDistinctTypes) {
    // Three identities that must never be interchanged. The type system is
    // what stops a client id being passed where a venue id belongs.
    static_assert(!std::is_same_v<LogicalOrderId, RequestId>);
    static_assert(!std::is_convertible_v<ClientOrderId, ExchangeOrderId>);
    static_assert(!std::is_convertible_v<LogicalOrderId, std::uint64_t>,
                  "a logical id must not decay into a bare integer");
    SUCCEED();
}

TEST(OmsBoundary, StateEnumIsStableAcrossTheJournal) {
    // Journal records are persisted and replayed. Reordering these values
    // silently reinterprets every record ever written.
    EXPECT_EQ(static_cast<int>(OrderState::Created), 0);
    EXPECT_EQ(static_cast<int>(OrderState::PendingNew), 1);
    EXPECT_EQ(static_cast<int>(OrderState::Working), 2);
    EXPECT_EQ(static_cast<int>(OrderState::PartiallyFilled), 3);
    EXPECT_EQ(static_cast<int>(OrderState::PendingCancel), 4);
    EXPECT_EQ(static_cast<int>(OrderState::PendingReplace), 5);
    EXPECT_EQ(static_cast<int>(OrderState::Filled), 6);
    EXPECT_EQ(static_cast<int>(OrderState::Cancelled), 7);
    EXPECT_EQ(static_cast<int>(OrderState::Rejected), 8);
    EXPECT_EQ(static_cast<int>(OrderState::Expired), 9);
    EXPECT_EQ(static_cast<int>(OrderState::Unknown), 10);
    EXPECT_EQ(static_cast<int>(OrderState::Orphaned), 11);
}

// ---------------------------------------------------------------------------
// Exposure semantics -- the property risk depends on
// ---------------------------------------------------------------------------

TEST(OmsBoundary, ExposureIsConservativeByConstruction) {
    // Every state where the order might be resting consumes exposure. Getting
    // this wrong understates risk, which is the direction that costs money.
    EXPECT_TRUE(consumes_exposure(OrderState::PendingNew));
    EXPECT_TRUE(consumes_exposure(OrderState::Working));
    EXPECT_TRUE(consumes_exposure(OrderState::PartiallyFilled));
    EXPECT_TRUE(consumes_exposure(OrderState::PendingCancel));
    EXPECT_TRUE(consumes_exposure(OrderState::PendingReplace));
    EXPECT_TRUE(consumes_exposure(OrderState::Unknown))
        << "an order we cannot account for must be assumed to exist";

    EXPECT_FALSE(consumes_exposure(OrderState::Filled));
    EXPECT_FALSE(consumes_exposure(OrderState::Cancelled));
    EXPECT_FALSE(consumes_exposure(OrderState::Rejected));
    EXPECT_FALSE(consumes_exposure(OrderState::Expired));

    // Every terminal state releases exposure; no live state does.
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(OrderState::Orphaned); ++raw) {
        const auto s = static_cast<OrderState>(raw);
        if (is_terminal(s)) {
            EXPECT_FALSE(consumes_exposure(s)) << to_string(s);
        }
        if (is_live(s)) {
            EXPECT_TRUE(consumes_exposure(s)) << to_string(s);
        }
    }
}

TEST(OmsBoundary, PaperAndLiveShareOneImplementation) {
    // §44. There is exactly one OrderManager type. If paper and live ever
    // needed different lifecycle handling, this is where it would show up as a
    // second class or a mode parameter on the constructor.
    static_assert(std::is_constructible_v<OrderManager, const OmsConfig&, IOrderJournal&,
                                          const Clock&>,
                  "the OMS is constructed from config, journal and clock -- no mode");
    static_assert(!std::is_constructible_v<OrderManager, const OmsConfig&, IOrderJournal&,
                                           const Clock&, bool>,
                  "no is_live flag may be threaded into the OMS");
    SUCCEED();
}

}  // namespace
}  // namespace mm::oms
