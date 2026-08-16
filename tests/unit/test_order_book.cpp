#include <gtest/gtest.h>

#include <random>

#include "mm/orderbook/OrderBook.hpp"
#include "support/BookFixtures.hpp"

namespace mm::book {
namespace {

using test::level;
using test::LevelSpec;
using test::levels;

OrderBook make_book(const LevelSpec& bids, const LevelSpec& asks, Seq id = 100,
                    std::size_t depth = 50) {
    OrderBook b(Symbol("BTCUSDT"), depth);
    b.begin_snapshot();
    EXPECT_TRUE(b.add_snapshot_levels(levels(bids, asks)).is_ok());
    EXPECT_TRUE(b.finish_snapshot(id).is_ok());
    return b;
}

// ---------------------------------------------------------------- snapshot

TEST(OrderBook, EmptyBookIsNotValidUntilASnapshotArrives) {
    const OrderBook b(Symbol("BTCUSDT"), 50);
    EXPECT_FALSE(b.is_valid());
    EXPECT_FALSE(b.has_bids());
    EXPECT_TRUE(b.best_bid_price().is_zero());
    EXPECT_TRUE(b.spread().is_zero());
    EXPECT_TRUE(b.mid().is_zero());
}

TEST(OrderBook, SnapshotEstablishesTheBook) {
    const OrderBook b = make_book({{"100", "5"}, {"99", "3"}}, {{"101", "4"}, {"102", "2"}});
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.best_bid_price().to_string(), "100");
    EXPECT_EQ(b.best_bid_qty().to_string(), "5");
    EXPECT_EQ(b.best_ask_price().to_string(), "101");
    EXPECT_EQ(b.spread().to_string(), "1");
    EXPECT_EQ(b.mid().to_string(), "100.5");
    EXPECT_EQ(b.depth(Side::Buy), 2U);
    EXPECT_EQ(b.depth(Side::Sell), 2U);
    EXPECT_EQ(b.last_update_id(), 100U);
}

TEST(OrderBook, SnapshotSortsUnorderedInput) {
    // Venues are not obliged to send levels in order, and chunked snapshots
    // arrive in pieces, so the book sorts rather than trusts.
    const OrderBook b = make_book({{"99", "3"}, {"100", "5"}, {"98", "1"}},
                                  {{"102", "2"}, {"101", "4"}});
    EXPECT_EQ(b.bids()[0].price.to_string(), "100");
    EXPECT_EQ(b.bids()[1].price.to_string(), "99");
    EXPECT_EQ(b.bids()[2].price.to_string(), "98");
    EXPECT_EQ(b.asks()[0].price.to_string(), "101");
    EXPECT_EQ(b.asks()[1].price.to_string(), "102");
    EXPECT_EQ(b.check_invariants(), BookViolation::None);
}

TEST(OrderBook, SnapshotIsAtomicAcrossChunks) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"100", "5"}}, {})).is_ok());
    // Mid-snapshot the book must still be invalid: a consumer must never see a
    // half-applied image.
    EXPECT_FALSE(b.is_valid());
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"99", "3"}}, {{"101", "4"}})).is_ok());
    EXPECT_FALSE(b.is_valid());
    ASSERT_TRUE(b.finish_snapshot(500).is_ok());
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.depth(Side::Buy), 2U);
    EXPECT_EQ(b.depth(Side::Sell), 1U);
}

TEST(OrderBook, SnapshotReplacementDoesNotMixGenerations) {
    OrderBook b = make_book({{"100", "5"}}, {{"101", "4"}});
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"200", "7"}}, {{"201", "8"}})).is_ok());
    // Old book still visible until the new one is finished.
    EXPECT_EQ(b.best_bid_price().to_string(), "100");
    ASSERT_TRUE(b.finish_snapshot(600).is_ok());
    EXPECT_EQ(b.best_bid_price().to_string(), "200");
    EXPECT_EQ(b.best_ask_price().to_string(), "201");
    EXPECT_EQ(b.depth(Side::Buy), 1U) << "levels from the previous image must not survive";
}

TEST(OrderBook, CrossedSnapshotIsRejected) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"102", "5"}}, {{"101", "4"}})).is_ok());
    EXPECT_TRUE(b.finish_snapshot(100).is_error());
    EXPECT_FALSE(b.is_valid());
    EXPECT_EQ(b.violation(), BookViolation::CrossedBook);
}

TEST(OrderBook, LockedSnapshotIsRejected) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"101", "5"}}, {{"101", "4"}})).is_ok());
    EXPECT_TRUE(b.finish_snapshot(100).is_error());
    EXPECT_EQ(b.violation(), BookViolation::LockedBook);
}

TEST(OrderBook, SnapshotWithDuplicateLevelsIsRejected) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"100", "5"}, {"100", "3"}}, {})).is_ok());
    EXPECT_TRUE(b.finish_snapshot(100).is_error());
    EXPECT_EQ(b.violation(), BookViolation::DuplicateLevel);
}

TEST(OrderBook, SnapshotRejectsInvalidLevels) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    b.begin_snapshot();
    EXPECT_TRUE(b.add_snapshot_levels(levels({{"0", "5"}}, {})).is_error()) << "zero price";
}

// ------------------------------------------------------------- incremental
// The exact scenario from the brief.

TEST(OrderBook, UpdateReplacesQuantityAtAnExistingLevel) {
    OrderBook b = make_book({{"100", "5"}, {"101", "3"}}, {{"110", "1"}});
    ASSERT_TRUE(b.apply_update(levels({{"101", "7"}}, {}), 101).is_ok());
    EXPECT_EQ(b.quantity_at(Side::Buy, Px::from_units(101)).to_string(), "7");
    EXPECT_EQ(b.depth(Side::Buy), 2U);
    EXPECT_EQ(b.last_update_id(), 101U);
}

TEST(OrderBook, ZeroQuantityRemovesTheLevel) {
    OrderBook b = make_book({{"100", "5"}, {"101", "3"}}, {{"110", "1"}});
    ASSERT_TRUE(b.apply_update(levels({{"101", "7"}}, {}), 101).is_ok());
    ASSERT_TRUE(b.apply_update(levels({{"101", "0"}}, {}), 102).is_ok());
    EXPECT_TRUE(b.quantity_at(Side::Buy, Px::from_units(101)).is_zero());
    EXPECT_EQ(b.depth(Side::Buy), 1U);
    EXPECT_EQ(b.best_bid_price().to_string(), "100");
    EXPECT_EQ(b.check_invariants(), BookViolation::None)
        << "a zero level must be erased, not stored";
}

TEST(OrderBook, UpdateInsertsANewLevelInSortedPosition) {
    OrderBook b = make_book({{"100", "5"}, {"98", "3"}}, {{"110", "1"}});
    ASSERT_TRUE(b.apply_update(levels({{"99", "4"}}, {}), 101).is_ok());
    ASSERT_EQ(b.depth(Side::Buy), 3U);
    EXPECT_EQ(b.bids()[0].price.to_string(), "100");
    EXPECT_EQ(b.bids()[1].price.to_string(), "99");
    EXPECT_EQ(b.bids()[2].price.to_string(), "98");
}

TEST(OrderBook, UpdateCanImproveTheTouch) {
    OrderBook b = make_book({{"100", "5"}}, {{"110", "1"}});
    ASSERT_TRUE(b.apply_update(levels({{"105", "2"}}, {{"106", "3"}}), 101).is_ok());
    EXPECT_EQ(b.best_bid_price().to_string(), "105");
    EXPECT_EQ(b.best_ask_price().to_string(), "106");
    EXPECT_EQ(b.spread().to_string(), "1");
}

TEST(OrderBook, BothSidesUpdateIndependently) {
    OrderBook b = make_book({{"100", "5"}}, {{"110", "1"}, {"111", "2"}});
    ASSERT_TRUE(b.apply_update(levels({{"100", "9"}}, {{"111", "0"}}), 101).is_ok());
    EXPECT_EQ(b.best_bid_qty().to_string(), "9");
    EXPECT_EQ(b.depth(Side::Sell), 1U);
}

TEST(OrderBook, DeletingAnUntrackedLevelIsNotAnError) {
    // Beyond our depth the level may never have been tracked; a delete for it
    // is normal traffic, not corruption.
    OrderBook b = make_book({{"100", "5"}}, {{"110", "1"}});
    ASSERT_TRUE(b.apply_update(levels({{"50", "0"}}, {}), 101).is_ok());
    EXPECT_EQ(b.depth(Side::Buy), 1U);
    EXPECT_TRUE(b.is_valid());
}

TEST(OrderBook, UpdateBeforeAnySnapshotIsRefused) {
    OrderBook b(Symbol("BTCUSDT"), 50);
    EXPECT_TRUE(b.apply_update(levels({{"100", "5"}}, {}), 101).is_error());
}

TEST(OrderBook, UpdateProducingACrossedBookIsRejected) {
    OrderBook b = make_book({{"100", "5"}}, {{"101", "4"}});
    // A bid above the best ask: either the venue or our application is wrong,
    // and a strategy handed this book would see free money that is not there.
    EXPECT_TRUE(b.apply_update(levels({{"102", "1"}}, {}), 101).is_error());
    EXPECT_FALSE(b.is_valid());
    EXPECT_EQ(b.violation(), BookViolation::CrossedBook);
}

TEST(OrderBook, UpdateProducingALockedBookIsRejected) {
    OrderBook b = make_book({{"100", "5"}}, {{"101", "4"}});
    EXPECT_TRUE(b.apply_update(levels({{"101", "1"}}, {}), 101).is_error());
    EXPECT_EQ(b.violation(), BookViolation::LockedBook);
}

TEST(OrderBook, NegativeQuantityIsRejected) {
    OrderBook b = make_book({{"100", "5"}}, {{"110", "1"}});
    BookLevels bad;
    bad.bids[0].price = Px::from_units(99);
    bad.bids[0].quantity = Qty::from_raw(-1);
    bad.bid_count = 1;
    EXPECT_TRUE(b.apply_update(bad, 101).is_error());
    EXPECT_EQ(b.violation(), BookViolation::NegativeQuantity);
}

TEST(OrderBook, SequenceRegressionIsRejected) {
    OrderBook b = make_book({{"100", "5"}}, {{"110", "1"}}, 500);
    // Replaying stale data onto a newer book corrupts it silently.
    EXPECT_TRUE(b.apply_update(levels({{"100", "9"}}, {}), 400).is_error());
    EXPECT_EQ(b.violation(), BookViolation::SequenceRegression);
}

// ------------------------------------------------------------------ depth

TEST(OrderBook, TruncatesToConfiguredDepth) {
    LevelSpec bids;
    std::vector<std::string> storage;
    storage.reserve(20);
    for (int i = 0; i < 10; ++i) {
        storage.push_back(std::to_string(100 - i));
    }
    for (const auto& s : storage) {
        bids.emplace_back(s.c_str(), "1");
    }
    OrderBook b(Symbol("BTCUSDT"), 3);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels(bids, {})).is_ok());
    ASSERT_TRUE(b.finish_snapshot(100).is_ok());

    EXPECT_EQ(b.depth(Side::Buy), 3U);
    EXPECT_EQ(b.bids()[0].price.to_string(), "100") << "the top of book must be exact";
    EXPECT_EQ(b.bids()[2].price.to_string(), "98");
}

TEST(OrderBook, DropsUpdatesBeyondTrackedDepthWithoutCorruptingTheTouch) {
    OrderBook b(Symbol("BTCUSDT"), 2);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"100", "1"}, {"99", "1"}}, {})).is_ok());
    ASSERT_TRUE(b.finish_snapshot(100).is_ok());

    // Worse than everything tracked, and the side is full: it cannot reach the
    // top of book, so it is dropped rather than grown into.
    ASSERT_TRUE(b.apply_update(levels({{"50", "5"}}, {}), 101).is_ok());
    EXPECT_EQ(b.depth(Side::Buy), 2U);
    EXPECT_EQ(b.best_bid_price().to_string(), "100");
    EXPECT_GT(b.levels_dropped_beyond_depth(), 0U);
    EXPECT_EQ(b.check_invariants(), BookViolation::None);
}

TEST(OrderBook, ABetterLevelEvictsTheWorstWhenFull) {
    OrderBook b(Symbol("BTCUSDT"), 2);
    b.begin_snapshot();
    ASSERT_TRUE(b.add_snapshot_levels(levels({{"100", "1"}, {"99", "1"}}, {})).is_ok());
    ASSERT_TRUE(b.finish_snapshot(100).is_ok());

    ASSERT_TRUE(b.apply_update(levels({{"101", "5"}}, {}), 101).is_ok());
    EXPECT_EQ(b.depth(Side::Buy), 2U);
    EXPECT_EQ(b.bids()[0].price.to_string(), "101");
    EXPECT_EQ(b.bids()[1].price.to_string(), "100");
    EXPECT_TRUE(b.quantity_at(Side::Buy, Px::from_units(99)).is_zero());
}

// --------------------------------------------------------------- accessors

TEST(OrderBook, CumulativeQuantity) {
    const OrderBook b = make_book({{"100", "5"}, {"99", "3"}, {"98", "2"}}, {{"101", "1"}});
    EXPECT_EQ(b.cumulative_qty(Side::Buy, 2).to_string(), "8");
    EXPECT_EQ(b.cumulative_qty(Side::Buy, 10).to_string(), "10") << "clamps to available depth";
    EXPECT_EQ(b.cumulative_qty(Side::Sell, 1).to_string(), "1");
}

TEST(OrderBook, BboMatchesTheTopOfBook) {
    const OrderBook b = make_book({{"100", "5"}}, {{"101", "4"}}, 777);
    const BestBidAsk q = b.bbo();
    EXPECT_TRUE(q.is_sane());
    EXPECT_EQ(q.bid_px.to_string(), "100");
    EXPECT_EQ(q.ask_qty.to_string(), "4");
    EXPECT_EQ(q.seq, 777U);
}

TEST(OrderBook, OneSidedBookHasNoSpreadOrMid) {
    const OrderBook b = make_book({{"100", "5"}}, {});
    EXPECT_TRUE(b.spread().is_zero());
    EXPECT_TRUE(b.mid().is_zero()) << "a number that looks like a mid would be worse than zero";
    EXPECT_FALSE(b.bbo().is_sane());
}

TEST(OrderBook, ClearDiscardsEverything) {
    OrderBook b = make_book({{"100", "5"}}, {{"101", "4"}});
    b.clear();
    EXPECT_FALSE(b.is_valid());
    EXPECT_EQ(b.depth(Side::Buy), 0U);
    EXPECT_EQ(b.last_update_id(), kNoSeq);
}

// ------------------------------------------------------ property/invariant

TEST(OrderBook, InvariantsHoldAcrossARandomisedUpdateStream) {
    // Deterministically seeded: a failure here must be reproducible.
    std::mt19937_64 rng(20260817);
    OrderBook b(Symbol("BTCUSDT"), 20);
    b.begin_snapshot();
    LevelSpec bids, asks;
    std::vector<std::string> storage;
    storage.reserve(40);
    for (int i = 0; i < 10; ++i) {
        storage.push_back(std::to_string(1000 - i));
        storage.push_back(std::to_string(1010 + i));
    }
    for (std::size_t i = 0; i < storage.size(); i += 2) {
        bids.emplace_back(storage[i].c_str(), "1");
        asks.emplace_back(storage[i + 1].c_str(), "1");
    }
    ASSERT_TRUE(b.add_snapshot_levels(levels(bids, asks)).is_ok());
    ASSERT_TRUE(b.finish_snapshot(1).is_ok());

    Seq id = 1;
    for (int round = 0; round < 20'000; ++round) {
        // Stay strictly inside the spread's boundaries so a valid stream never
        // legitimately crosses the book.
        const bool is_bid = (rng() % 2) == 0;
        const std::int64_t price = is_bid ? static_cast<std::int64_t>(991 + rng() % 10)
                                          : static_cast<std::int64_t>(1010 + rng() % 10);
        const std::int64_t qty = static_cast<std::int64_t>(rng() % 4);  // 0 deletes

        BookLevels l;
        PriceLevel pl;
        pl.price = Px::from_units(price);
        pl.quantity = Qty::from_units(qty);
        if (is_bid) {
            l.bids[0] = pl;
            l.bid_count = 1;
        } else {
            l.asks[0] = pl;
            l.ask_count = 1;
        }

        ASSERT_TRUE(b.apply_update(l, ++id).is_ok()) << "round " << round;
        ASSERT_EQ(b.check_invariants(), BookViolation::None)
            << "round " << round << " violation " << to_string(b.check_invariants());
        ASSERT_LE(b.depth(Side::Buy), 20U);
        ASSERT_LE(b.depth(Side::Sell), 20U);
    }
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.last_update_id(), id);
}

TEST(BookViolation, EveryValueHasAName) {
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(BookViolation::SequenceRegression);
         ++i) {
        EXPECT_NE(to_string(static_cast<BookViolation>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::book
