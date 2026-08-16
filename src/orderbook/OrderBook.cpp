#include "mm/orderbook/OrderBook.hpp"

#include <algorithm>

namespace mm::book {
namespace {

/// Bids sort descending, asks ascending, so index 0 is always the best level of
/// either side and "better" is a single comparison the caller never repeats.
[[nodiscard]] constexpr bool is_better(Side side, Px a, Px b) noexcept {
    return side == Side::Buy ? a > b : a < b;
}

/// Locates `price`, returning the insertion point when absent.
///
/// The side is a template parameter rather than a runtime argument on purpose.
/// A binary search over 50 levels is about six comparisons, and with a runtime
/// side each one carries an extra unpredictable branch inside the innermost
/// loop; hoisting it to compile time lets the comparison fold to a single
/// instruction. Measured effect on this workload: within noise. Kept because it
/// is strictly cleaner, not because it bought anything.
template <Side WhichSide, class Container>
[[nodiscard]] auto locate_impl(Container& side, Px price) noexcept {
    return std::lower_bound(side.begin(), side.end(), price,
                            [](const PriceLevel& level, Px target) {
                                if constexpr (WhichSide == Side::Buy) {
                                    return level.price > target;
                                } else {
                                    return level.price < target;
                                }
                            });
}

[[nodiscard]] std::vector<PriceLevel>::iterator locate(std::vector<PriceLevel>& side, Side which,
                                                       Px price) noexcept {
    return which == Side::Buy ? locate_impl<Side::Buy>(side, price)
                              : locate_impl<Side::Sell>(side, price);
}

[[nodiscard]] std::vector<PriceLevel>::const_iterator locate(const std::vector<PriceLevel>& side,
                                                             Side which, Px price) noexcept {
    return which == Side::Buy ? locate_impl<Side::Buy>(side, price)
                              : locate_impl<Side::Sell>(side, price);
}

}  // namespace

std::string_view to_string(BookViolation v) noexcept {
    switch (v) {
        case BookViolation::None:               return "NONE";
        case BookViolation::CrossedBook:        return "CROSSED_BOOK";
        case BookViolation::LockedBook:         return "LOCKED_BOOK";
        case BookViolation::NegativeQuantity:   return "NEGATIVE_QUANTITY";
        case BookViolation::ZeroQuantityLevel:  return "ZERO_QUANTITY_LEVEL";
        case BookViolation::NonPositivePrice:   return "NON_POSITIVE_PRICE";
        case BookViolation::UnsortedLevels:     return "UNSORTED_LEVELS";
        case BookViolation::DuplicateLevel:     return "DUPLICATE_LEVEL";
        case BookViolation::DepthExceeded:      return "DEPTH_EXCEEDED";
        case BookViolation::SequenceRegression: return "SEQUENCE_REGRESSION";
    }
    return "UNKNOWN";
}

OrderBook::OrderBook(Symbol symbol, std::size_t max_depth) { reset(symbol, max_depth); }

void OrderBook::reset(Symbol symbol, std::size_t max_depth) {
    symbol_ = symbol;
    max_depth_ = max_depth == 0 ? kDefaultMaxDepth : max_depth;
    // Reserve once so no steady-state update can allocate. One extra slot
    // absorbs the transient insert that precedes a trim.
    bids_.reserve(max_depth_ + 1);
    asks_.reserve(max_depth_ + 1);
    staging_bids_.reserve(max_depth_ + 1);
    staging_asks_.reserve(max_depth_ + 1);
    clear();
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    staging_bids_.clear();
    staging_asks_.clear();
    building_snapshot_ = false;
    last_update_id_ = kNoSeq;
    snapshot_applied_ = false;
    violated_ = false;
    violation_ = BookViolation::None;
}

void OrderBook::mark_invalid(BookViolation v) noexcept {
    violated_ = true;
    violation_ = v;
}

void OrderBook::begin_snapshot() noexcept {
    staging_bids_.clear();
    staging_asks_.clear();
    building_snapshot_ = true;
}

Status OrderBook::add_snapshot_levels(const BookLevels& levels) {
    if (!building_snapshot_) {
        return {ErrorCode::FailedPrecondition, "add_snapshot_levels without begin_snapshot"};
    }
    if (!levels.counts_are_sane()) {
        return {ErrorCode::OutOfRange, "snapshot chunk level count exceeds capacity"};
    }

    for (std::uint8_t i = 0; i < levels.bid_count; ++i) {
        const PriceLevel& l = levels.bids[i];
        // A snapshot describes what exists; a zero level is not a deletion here
        // but an absence, so it is simply not recorded.
        if (l.quantity.is_zero()) {
            continue;
        }
        if (!l.price.is_positive() || l.quantity.is_negative()) {
            return {ErrorCode::InvalidArgument, "snapshot contains an invalid bid level"};
        }
        staging_bids_.push_back(l);
    }
    for (std::uint8_t i = 0; i < levels.ask_count; ++i) {
        const PriceLevel& l = levels.asks[i];
        if (l.quantity.is_zero()) {
            continue;
        }
        if (!l.price.is_positive() || l.quantity.is_negative()) {
            return {ErrorCode::InvalidArgument, "snapshot contains an invalid ask level"};
        }
        staging_asks_.push_back(l);
    }
    return Status::ok();
}

Status OrderBook::finish_snapshot(Seq last_update_id) {
    if (!building_snapshot_) {
        return {ErrorCode::FailedPrecondition, "finish_snapshot without begin_snapshot"};
    }
    building_snapshot_ = false;

    // The venue is not required to send levels in order, and a chunked snapshot
    // arrives in pieces, so sort rather than trust.
    std::sort(staging_bids_.begin(), staging_bids_.end(),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    std::sort(staging_asks_.begin(), staging_asks_.end(),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

    const auto has_duplicates = [](const std::vector<PriceLevel>& side) {
        return std::adjacent_find(side.begin(), side.end(),
                                  [](const PriceLevel& a, const PriceLevel& b) {
                                      return a.price == b.price;
                                  }) != side.end();
    };
    if (has_duplicates(staging_bids_) || has_duplicates(staging_asks_)) {
        mark_invalid(BookViolation::DuplicateLevel);
        return {ErrorCode::InvalidArgument, "snapshot contains duplicate price levels"};
    }

    if (staging_bids_.size() > max_depth_) {
        staging_bids_.resize(max_depth_);
    }
    if (staging_asks_.size() > max_depth_) {
        staging_asks_.resize(max_depth_);
    }

    // A crossed snapshot means the venue's image is unusable; accepting it
    // would hand a strategy a book promising free money.
    if (!staging_bids_.empty() && !staging_asks_.empty() &&
        staging_bids_.front().price >= staging_asks_.front().price) {
        mark_invalid(staging_bids_.front().price == staging_asks_.front().price
                         ? BookViolation::LockedBook
                         : BookViolation::CrossedBook);
        return {ErrorCode::InvalidArgument, "snapshot is crossed or locked"};
    }

    // Swap rather than copy: the visible book flips in one step, so no consumer
    // can observe bids from the new image against asks from the old.
    bids_.swap(staging_bids_);
    asks_.swap(staging_asks_);
    staging_bids_.clear();
    staging_asks_.clear();

    last_update_id_ = last_update_id;
    snapshot_applied_ = true;
    violated_ = false;
    violation_ = BookViolation::None;
    return Status::ok();
}

Status OrderBook::apply_level(std::vector<PriceLevel>& side, Side which, const PriceLevel& level) {
    if (!level.price.is_positive()) {
        mark_invalid(BookViolation::NonPositivePrice);
        return {ErrorCode::InvalidArgument, "update contains a non-positive price"};
    }
    if (level.quantity.is_negative()) {
        mark_invalid(BookViolation::NegativeQuantity);
        return {ErrorCode::InvalidArgument, "update contains a negative quantity"};
    }

    const auto it = locate(side, which, level.price);
    const bool found = (it != side.end() && it->price == level.price);

    if (level.quantity.is_zero()) {
        // Zero means delete. A delete for a level we do not hold is normal --
        // it may have been trimmed beyond our depth -- and is not an error.
        if (found) {
            side.erase(it);
        }
        return Status::ok();
    }

    if (found) {
        it->quantity = level.quantity;
        return Status::ok();
    }

    // Beyond the tracked depth and the side is full: this level cannot reach
    // the top of book, so dropping it keeps the structure small without
    // affecting anything a quoting strategy can see.
    if (side.size() >= max_depth_ && it == side.end()) {
        ++levels_dropped_;
        return Status::ok();
    }

    side.insert(it, level);
    trim(side);
    return Status::ok();
}

void OrderBook::trim(std::vector<PriceLevel>& side) noexcept {
    if (side.size() > max_depth_) {
        side.resize(max_depth_);
        ++levels_dropped_;
    }
}

Status OrderBook::apply_update(const BookLevels& levels, Seq final_update_id) {
    if (!snapshot_applied_) {
        return {ErrorCode::FailedPrecondition, "update applied before any snapshot"};
    }
    if (!levels.counts_are_sane()) {
        mark_invalid(BookViolation::DepthExceeded);
        return {ErrorCode::OutOfRange, "update level count exceeds capacity"};
    }
    // Update ids must advance. A regression means we are replaying stale data
    // onto a newer book, which silently corrupts it.
    if (final_update_id != kNoSeq && final_update_id < last_update_id_) {
        mark_invalid(BookViolation::SequenceRegression);
        return {ErrorCode::FailedPrecondition, "update id regressed"};
    }

    for (std::uint8_t i = 0; i < levels.bid_count; ++i) {
        MM_RETURN_IF_ERROR(apply_level(bids_, Side::Buy, levels.bids[i]));
    }
    for (std::uint8_t i = 0; i < levels.ask_count; ++i) {
        MM_RETURN_IF_ERROR(apply_level(asks_, Side::Sell, levels.asks[i]));
    }

    // A diff can legitimately cross the book only transiently within a single
    // message; once the whole message is applied, a crossed book means either
    // the venue or our application of it is wrong.
    if (!bids_.empty() && !asks_.empty()) {
        if (bids_.front().price > asks_.front().price) {
            mark_invalid(BookViolation::CrossedBook);
            return {ErrorCode::FailedPrecondition, "update produced a crossed book"};
        }
        if (bids_.front().price == asks_.front().price) {
            mark_invalid(BookViolation::LockedBook);
            return {ErrorCode::FailedPrecondition, "update produced a locked book"};
        }
    }

    if (final_update_id != kNoSeq) {
        last_update_id_ = final_update_id;
    }
    ++updates_applied_;
    return Status::ok();
}

BestBidAsk OrderBook::bbo() const noexcept {
    BestBidAsk b;
    b.bid_px = best_bid_price();
    b.bid_qty = best_bid_qty();
    b.ask_px = best_ask_price();
    b.ask_qty = best_ask_qty();
    b.seq = last_update_id_;
    return b;
}

Px OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) {
        return Px::zero();
    }
    return asks_.front().price - bids_.front().price;
}

Px OrderBook::mid() const noexcept {
    if (bids_.empty() || asks_.empty()) {
        return Px::zero();
    }
    return Px::from_raw((bids_.front().price.raw() + asks_.front().price.raw()) / 2);
}

std::size_t OrderBook::depth(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

Qty OrderBook::quantity_at(Side side, Px price) const noexcept {
    const std::vector<PriceLevel>& levels = (side == Side::Buy) ? bids_ : asks_;
    const auto it = locate(levels, side, price);
    if (it == levels.end() || it->price != price) {
        return Qty::zero();
    }
    return it->quantity;
}

Qty OrderBook::cumulative_qty(Side side, std::size_t levels) const noexcept {
    const std::vector<PriceLevel>& v = (side == Side::Buy) ? bids_ : asks_;
    Qty total;
    const std::size_t n = std::min(levels, v.size());
    for (std::size_t i = 0; i < n; ++i) {
        total += v[i].quantity;
    }
    return total;
}

BookViolation OrderBook::check_invariants() const noexcept {
    const auto audit_side = [this](const std::vector<PriceLevel>& side,
                                   Side which) -> BookViolation {
        if (side.size() > max_depth_) {
            return BookViolation::DepthExceeded;
        }
        for (std::size_t i = 0; i < side.size(); ++i) {
            if (!side[i].price.is_positive()) {
                return BookViolation::NonPositivePrice;
            }
            if (side[i].quantity.is_negative()) {
                return BookViolation::NegativeQuantity;
            }
            // A zero level must have been erased, not stored: leaving it would
            // make depth counts and cumulative quantities silently wrong.
            if (side[i].quantity.is_zero()) {
                return BookViolation::ZeroQuantityLevel;
            }
            if (i > 0) {
                if (side[i].price == side[i - 1].price) {
                    return BookViolation::DuplicateLevel;
                }
                if (!is_better(which, side[i - 1].price, side[i].price)) {
                    return BookViolation::UnsortedLevels;
                }
            }
        }
        return BookViolation::None;
    };

    const BookViolation bid_violation = audit_side(bids_, Side::Buy);
    if (bid_violation != BookViolation::None) {
        return bid_violation;
    }
    const BookViolation ask_violation = audit_side(asks_, Side::Sell);
    if (ask_violation != BookViolation::None) {
        return ask_violation;
    }

    if (!bids_.empty() && !asks_.empty()) {
        if (bids_.front().price > asks_.front().price) {
            return BookViolation::CrossedBook;
        }
        if (bids_.front().price == asks_.front().price) {
            return BookViolation::LockedBook;
        }
    }
    return BookViolation::None;
}

}  // namespace mm::book
