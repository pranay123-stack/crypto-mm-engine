/// \file test_accounting_concurrency.cpp
/// Phase 10 concurrency: accounting state is owned by the trading thread, and
/// readers elsewhere see it only through a published snapshot.
///
/// There is deliberately **no new cross-thread boundary** in this phase. Fills
/// reach accounting on the trading thread, drained from the same execution ring
/// the OMS drains; marks reach it on the trading thread; and nothing else
/// mutates it. The one thing that does cross a thread is a `PortfolioSnapshot`
/// copied out through a seqlock, which is what this exercises under TSan.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "mm/common/Seqlock.hpp"
#include "mm/portfolio/PortfolioAccountant.hpp"

namespace mm::portfolio {
namespace {

exchange::FillEvent fill_of(std::size_t i, bool buy) {
    exchange::FillEvent f;
    EXPECT_TRUE(f.trade_id.assign(("C" + std::to_string(i)).c_str()));
    f.symbol = Symbol("BTCUSDT");
    f.side = buy ? Side::Buy : Side::Sell;
    EXPECT_TRUE(Px::parse("100.00", f.price));
    EXPECT_TRUE(Qty::parse("0.5", f.quantity));
    EXPECT_TRUE(Notional::parse("0.01", f.fee));
    return f;
}

/// The trading thread mutates and publishes; a monitoring thread reads. The
/// reader never touches accounting state -- only the copy.
TEST(AccountingConcurrency, SnapshotsCrossThreadsWithoutSharingState) {
    ManualClock clock(0, 0);
    PortfolioAccountant accountant(AccountingConfig{}, clock);
    ASSERT_EQ(accountant.register_symbol(Symbol("BTCUSDT")), AccountingError::None);

    Seqlock<PortfolioSnapshot> published;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> torn{0};

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            PortfolioSnapshot s;
            if (!published.load(s)) {
                continue;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
            // A torn read would show a symbol count that never existed, or a
            // fee total larger than the number of fills could produce.
            if (s.symbol_count > 1 || s.fees.is_negative()) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Trading thread: the only mutator.
    for (std::size_t i = 0; i < 2'000; ++i) {
        ASSERT_TRUE(accountant.on_fill(fill_of(i, (i % 3) != 0)).ok());
        published.store(accountant.portfolio_snapshot());
    }
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_GT(reads.load(), 0U) << "the reader must actually have observed something";
    EXPECT_EQ(torn.load(), 0U);

    // And the trading thread's own view is intact.
    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->fill_count(), 2'000U);
}

TEST(AccountingConcurrency, TheReaderCannotMutateWhatItReads) {
    // Structural, not behavioural: the reader gets a value, not a handle.
    // There is no path from a snapshot back to the ledger.
    static_assert(std::is_trivially_copyable_v<PortfolioSnapshot>);
    static_assert(!std::is_pointer_v<decltype(std::declval<PortfolioSnapshot>().gross_notional)>);
    static_assert(std::is_same_v<decltype(std::declval<const PortfolioAccountant&>()
                                              .portfolio_snapshot()),
                                 PortfolioSnapshot>,
                  "returned by value; a reference would let a reader outlive or alter it");
    SUCCEED();
}

}  // namespace
}  // namespace mm::portfolio
