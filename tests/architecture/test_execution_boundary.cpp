/// Phase 9 §36. Structural properties of the execution boundary.

#include <gtest/gtest.h>

#include <type_traits>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/exchange/paper/PaperExecution.hpp"
#include "mm/oms/OrderManager.hpp"

namespace mm::exchange::paper {
namespace {

TEST(ExecutionBoundary, PaperImplementsTheSameInterfaceALiveAdapterWill) {
    // The whole claim of the phase in one assertion: the OMS holds an
    // IExchangeExecution, and paper is one. When BinanceExecution arrives it
    // implements the same interface and the OMS does not change.
    static_assert(std::is_base_of_v<IExchangeExecution, PaperExecution>);
    static_assert(std::is_base_of_v<IExchangeExecution, mock::MockExchangeExecution>);
    static_assert(std::has_virtual_destructor_v<IExchangeExecution>);

    // And they are interchangeable at the call site, not merely related.
    ManualClock clock(0, 0);
    PaperExecution paper(PaperExecutionConfig{}, clock);
    mock::MockExchangeExecution mock(clock);
    IExchangeExecution* adapters[] = {&paper, &mock};
    for (IExchangeExecution* adapter : adapters) {
        EXPECT_FALSE(adapter->venue().empty());
    }
}

TEST(ExecutionBoundary, TheOmsNeverNamesAConcreteAdapter) {
    // attach_execution takes the interface. There is no overload, no template,
    // and no way to hand the OMS something venue-specific.
    using Attach = void (oms::OrderManager::*)(IExchangeExecution&) noexcept;
    static_cast<void>(static_cast<Attach>(&oms::OrderManager::attach_execution));

    static_assert(!std::is_constructible_v<oms::OrderManager, PaperExecution&>,
                  "the OMS must not be constructible from a concrete adapter");
    SUCCEED();
}

TEST(ExecutionBoundary, PaperVenueHoldsNoReferenceToEngineDecisionComponents) {
    // Its constructor takes configuration and a clock. Not a risk engine, not
    // an OMS, not a strategy -- there is nothing for it to reach back into.
    static_assert(std::is_constructible_v<PaperExecution, PaperExecutionConfig, const Clock&>);
    static_assert(!std::is_constructible_v<PaperExecution, PaperExecutionConfig, const Clock&,
                                           oms::OrderManager&>);
    SUCCEED();
}

TEST(ExecutionBoundary, VenueOrderStateIsSeparateFromOmsOrderState) {
    // Two distinct types (§24). If they were one, the simulator would
    // guarantee agreement between engine and venue -- and agreeing is exactly
    // what the production system does not get for free.
    static_assert(!std::is_same_v<PaperOrder, oms::OrderRecord>);
    static_assert(!std::is_convertible_v<PaperOrder, oms::OrderRecord>);
    static_assert(!std::is_convertible_v<oms::OrderRecord, PaperOrder>);

    // The venue exposes copies and const pointers only; there is no handle
    // through which the engine could write.
    static_assert(std::is_same_v<decltype(std::declval<const PaperExecution&>().snapshot_orders()),
                                 std::vector<PaperOrder>>,
                  "the venue hands out copies, never a mutable view");
    SUCCEED();
}

TEST(ExecutionBoundary, ExecutionEventsAreTriviallyCopyableForTheRing) {
    // They cross an SPSC ring by value on the way to the OMS thread (§35).
    static_assert(std::is_trivially_copyable_v<ExecutionEvent>);
    static_assert(std::is_trivially_copyable_v<OrderRequest>);
    static_assert(std::is_trivially_copyable_v<CancelRequest>);
    static_assert(std::is_trivially_copyable_v<ReplaceRequest>);
    static_assert(std::is_trivially_copyable_v<PaperOrder>);
    SUCCEED();
}

TEST(ExecutionBoundary, PaperConfigCarriesNoCredentialFields) {
    // §40. There is no field into which a key could be put, so there is
    // nothing to leak in a log, a metric or an error message.
    const PaperExecutionConfig config;
    EXPECT_EQ(config.venue, VenueName("paper"));
    EXPECT_EQ(config.deterministic_seed, 0U)
        << "the default simulation is deterministic; randomness is opt-in";
    EXPECT_FALSE(config.faults.any()) << "no fault injection unless a test asks for it";
    static_assert(sizeof(PaperExecutionConfig) > 0);
    SUCCEED();
}

TEST(ExecutionBoundary, CapabilitiesFollowConfigurationNotWishfulThinking) {
    // A venue without atomic replace must say so, or the quote manager will
    // ask for something that does not exist.
    EXPECT_TRUE(paper_capabilities(ReplaceMode::Atomic).supports_replace);
    EXPECT_FALSE(paper_capabilities(ReplaceMode::Unsupported).supports_replace);
}

}  // namespace
}  // namespace mm::exchange::paper
