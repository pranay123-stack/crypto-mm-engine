#include "mm/exchange/paper/PaperMatcher.hpp"

#include <algorithm>

namespace mm::exchange::paper {
namespace {

/// `qty * bps / 10000`, saturating rather than wrapping.
[[nodiscard]] Qty apply_share(Qty qty, std::uint32_t bps) noexcept {
    if (bps >= 10'000) {
        return qty;
    }
    const auto scaled =
        (static_cast<int128>(qty.raw()) * static_cast<int128>(bps)) / static_cast<int128>(10'000);
    return Qty::from_raw(static_cast<std::int64_t>(scaled));
}

}  // namespace

bool is_marketable_on_arrival(const PaperOrder& order, const book::OrderBook& book) noexcept {
    if (!book.is_valid()) {
        return false;
    }
    const Px opposite =
        order.side == Side::Buy ? book.best_ask_price() : book.best_bid_price();
    return order.would_cross(opposite);
}

std::vector<PriceLevel> executable_side(const book::OrderBook& book, Side side) {
    // A buy consumes asks; a sell consumes bids.
    return side == Side::Buy ? book.asks() : book.bids();
}

MatchResult match_against_book(const PaperOrder& order, const book::OrderBook& book,
                               FillModel model, std::uint32_t queue_share_bps) {
    if (!book.is_valid()) {
        return MatchResult{};
    }
    std::vector<PriceLevel> available = executable_side(book, order.side);
    return match_and_consume(order, available, model, queue_share_bps);
}

MatchResult match_and_consume(const PaperOrder& order, std::vector<PriceLevel>& levels,
                              FillModel model, std::uint32_t queue_share_bps) noexcept {
    MatchResult result;
    if (!order.is_resting()) {
        return result;
    }
    const Qty remaining = order.remaining();
    if (!remaining.is_positive()) {
        return result;
    }
    if (levels.empty()) {
        return result;
    }

    if (model == FillModel::FullOnCross) {
        // Deliberately optimistic: crossing at all fills everything at the
        // order's own limit. Never the default; see PaperTypes.hpp.
        const Px touch = levels.front().price;
        const bool crosses = order.side == Side::Buy ? order.price >= touch : order.price <= touch;
        if (!crosses) {
            return result;
        }
        result.quantity = remaining;
        result.price = order.price;
        result.levels_consumed = 1;
        // Optimistic on size, but still consuming: even this model must not
        // hand the same displayed quantity to two orders.
        Qty left;
        if (checked_sub(levels.front().quantity, remaining, left) && !left.is_negative()) {
            levels.front().quantity = left;
        } else {
            levels.front().quantity = Qty{};
        }
        return result;
    }

    // DisplayedLiquidity: walk the opposite side, taking only what is shown and
    // only at prices this order's limit permits.
    Qty filled{};
    int128 value = 0;
    for (PriceLevel& level : levels) {
        const bool executable =
            order.side == Side::Buy ? order.price >= level.price : order.price <= level.price;
        if (!executable) {
            break;
        }
        const Qty available = apply_share(level.quantity, queue_share_bps);
        if (!available.is_positive()) {
            // Everything displayed here is assumed to be ahead of us. Not a
            // reason to stop: a deeper level may still be executable, but the
            // level itself yields nothing.
            continue;
        }
        Qty room;
        if (!checked_sub(remaining, filled, room) || !room.is_positive()) {
            break;
        }
        const Qty take = std::min(available, room);
        Qty next;
        if (!checked_add(filled, take, next)) {
            break;
        }
        // Executions happen at the *book's* price, not the order's: a buy
        // limit at 100 hitting an ask of 99 pays 99. Crediting the limit price
        // would silently invent price improvement.
        value += static_cast<int128>(take.raw()) * static_cast<int128>(level.price.raw());
        filled = next;
        ++result.levels_consumed;
        // Taken off the book for this update, so the next order in priority
        // order cannot be handed the same size.
        Qty left;
        if (checked_sub(level.quantity, take, left) && !left.is_negative()) {
            level.quantity = left;
        } else {
            level.quantity = Qty{};
        }
        if (filled >= remaining) {
            break;
        }
    }

    if (!filled.is_positive()) {
        return result;
    }
    result.quantity = filled;
    result.price = Px::from_raw(static_cast<std::int64_t>(value / static_cast<int128>(filled.raw())));
    return result;
}

}  // namespace mm::exchange::paper
