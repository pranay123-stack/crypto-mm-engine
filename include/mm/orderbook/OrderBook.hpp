#pragma once

/// \file OrderBook.hpp
/// Local L2 order book.
///
/// ## Data structure, and what the measurement actually said
///
/// Two sorted `std::vector<PriceLevel>` — bids descending, asks ascending — so
/// the best bid and best ask are always index 0.
///
/// The tempting rationale is that a vector must beat a red-black tree at this
/// size. **It does not.** Measured head to head at 50 levels with identical
/// inputs (`bench_orderbook.cpp`, and see docs/benchmarks.md), `std::map` is
/// roughly two to three times faster per level on mean throughput. Hoisting the
/// side comparison to compile time did not close the gap, so it is structural:
/// the vector pays a memmove per insertion and erasure where the tree only
/// relinks pointers.
///
/// The vector is kept anyway, for reasons the throughput number does not
/// capture:
///
///  * **No allocation.** `std::map` calls the allocator for every new price
///    level, on the thread that decodes market data. That is a malloc with an
///    unbounded tail on a latency-sensitive path, and the tail is what hurts a
///    market maker, not the mean. The vectors are reserved once at construction
///    and never allocate again.
///  * **Bounded, predictable footprint**, decided at startup rather than
///    growing with venue behaviour.
///  * **Contiguous top-N access**, which is what consumers actually read.
///
/// The absolute difference is around 35 ns per depth message against a network
/// path measured in tens of microseconds, so it is not currently worth
/// optimising further. If profiling ever shows book updates mattering, the
/// candidates are a pooled-node map or a flat structure with a separately
/// maintained top-N — not a switch to a plain `std::map`, which would trade a
/// measured non-problem for an allocation problem.
///
/// ## Truncation
///
/// The book tracks at most `max_depth` levels per side. An update for a price
/// worse than the worst tracked level, while the side is full, is dropped: it
/// cannot affect the top of book. The consequences are precise and worth
/// stating, because "the book is approximate" would be too vague to act on:
///
///  * **Best bid/ask and the top of book are always exact.**
///  * After deletions near the touch a side may hold fewer than `max_depth`
///    levels, because the levels that would have been promoted were never
///    tracked. Depth is therefore a lower bound, never wrong at the top.
///  * A resync restores full depth from the snapshot.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"

namespace mm::book {

using exchange::BookLevels;
using exchange::PriceLevel;

/// Why a book was judged invalid. Each maps to a specific corruption mode; a
/// single "invalid" flag would not tell an operator which one happened.
enum class BookViolation : std::uint8_t {
    None = 0,
    CrossedBook,        ///< best bid > best ask
    LockedBook,         ///< best bid == best ask
    NegativeQuantity,
    ZeroQuantityLevel,  ///< a zero level survived instead of being erased
    NonPositivePrice,
    UnsortedLevels,
    DuplicateLevel,
    DepthExceeded,
    SequenceRegression, ///< an update id went backwards
};
[[nodiscard]] std::string_view to_string(BookViolation v) noexcept;

class OrderBook {
public:
    static constexpr std::size_t kDefaultMaxDepth = 50;

    OrderBook() = default;
    OrderBook(Symbol symbol, std::size_t max_depth);

    void reset(Symbol symbol, std::size_t max_depth);

    /// Drops every level and clears synchronization. Used when a resync begins:
    /// a book that might be wrong is worth less than no book at all.
    void clear() noexcept;

    // ---------------------------------------------------------- construction

    /// Begins a snapshot. Levels accumulate into a staging area and only become
    /// visible on `finish_snapshot`, so a consumer can never observe a
    /// half-applied image.
    void begin_snapshot() noexcept;
    [[nodiscard]] Status add_snapshot_levels(const BookLevels& levels);
    /// Publishes the staged image atomically from a consumer's perspective.
    [[nodiscard]] Status finish_snapshot(Seq last_update_id);

    /// Applies an incremental diff. `final_update_id` must not regress.
    [[nodiscard]] Status apply_update(const BookLevels& levels, Seq final_update_id);

    // ---------------------------------------------------------------- access

    [[nodiscard]] bool has_bids() const noexcept { return !bids_.empty(); }
    [[nodiscard]] bool has_asks() const noexcept { return !asks_.empty(); }

    [[nodiscard]] Px best_bid_price() const noexcept {
        return bids_.empty() ? Px::zero() : bids_.front().price;
    }
    [[nodiscard]] Qty best_bid_qty() const noexcept {
        return bids_.empty() ? Qty::zero() : bids_.front().quantity;
    }
    [[nodiscard]] Px best_ask_price() const noexcept {
        return asks_.empty() ? Px::zero() : asks_.front().price;
    }
    [[nodiscard]] Qty best_ask_qty() const noexcept {
        return asks_.empty() ? Qty::zero() : asks_.front().quantity;
    }

    [[nodiscard]] BestBidAsk bbo() const noexcept;

    /// Zero when either side is missing: a one-sided book has no spread, and
    /// returning the price itself would be a number that looks like one.
    [[nodiscard]] Px spread() const noexcept;
    [[nodiscard]] Px mid() const noexcept;

    [[nodiscard]] const std::vector<PriceLevel>& bids() const noexcept { return bids_; }
    [[nodiscard]] const std::vector<PriceLevel>& asks() const noexcept { return asks_; }
    [[nodiscard]] std::size_t depth(Side side) const noexcept;

    /// Quantity resting at an exact price, or zero if the level is absent.
    [[nodiscard]] Qty quantity_at(Side side, Px price) const noexcept;

    /// Sum of quantity across the best `levels` levels of a side.
    [[nodiscard]] Qty cumulative_qty(Side side, std::size_t levels) const noexcept;

    [[nodiscard]] Symbol symbol() const noexcept { return symbol_; }
    [[nodiscard]] Seq last_update_id() const noexcept { return last_update_id_; }
    [[nodiscard]] std::size_t max_depth() const noexcept { return max_depth_; }

    /// True once a snapshot has been applied and no invariant has been violated.
    [[nodiscard]] bool is_valid() const noexcept { return snapshot_applied_ && !violated_; }
    [[nodiscard]] BookViolation violation() const noexcept { return violation_; }

    /// Full structural audit. Runs after every mutation in debug builds and on
    /// demand in release; `bench_orderbook.cpp` measures the cost.
    [[nodiscard]] BookViolation check_invariants() const noexcept;

    // ----------------------------------------------------------- diagnostics

    [[nodiscard]] std::uint64_t updates_applied() const noexcept { return updates_applied_; }
    [[nodiscard]] std::uint64_t levels_dropped_beyond_depth() const noexcept {
        return levels_dropped_;
    }

private:
    /// Applies one level to one side. Zero quantity erases.
    [[nodiscard]] Status apply_level(std::vector<PriceLevel>& side, Side which,
                                     const PriceLevel& level);
    void trim(std::vector<PriceLevel>& side) noexcept;
    void mark_invalid(BookViolation v) noexcept;

    Symbol symbol_{};
    std::size_t max_depth_ = kDefaultMaxDepth;

    std::vector<PriceLevel> bids_;  ///< descending price
    std::vector<PriceLevel> asks_;  ///< ascending price

    // Staging for a chunked snapshot; swapped in on finish_snapshot.
    std::vector<PriceLevel> staging_bids_;
    std::vector<PriceLevel> staging_asks_;
    bool building_snapshot_ = false;

    Seq last_update_id_ = kNoSeq;
    bool snapshot_applied_ = false;
    bool violated_ = false;
    BookViolation violation_ = BookViolation::None;

    std::uint64_t updates_applied_ = 0;
    std::uint64_t levels_dropped_ = 0;
};

}  // namespace mm::book
