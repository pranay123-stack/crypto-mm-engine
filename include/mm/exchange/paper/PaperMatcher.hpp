#pragma once

/// \file PaperMatcher.hpp
/// The fill model: how a resting order becomes an execution.
///
/// Deliberately small and pure. It answers one question -- "given this book,
/// how much of this order executes and at what price?" -- and owns no state, so
/// the answer is a function of its inputs and nothing else. That is what makes
/// the determinism requirement (§31) provable rather than hoped for.

#include <cstdint>

#include "mm/exchange/paper/PaperOrder.hpp"
#include "mm/exchange/paper/PaperTypes.hpp"
#include "mm/orderbook/OrderBook.hpp"

namespace mm::exchange::paper {

/// One order's executable quantity against the current book.
struct MatchResult {
    /// How much executes now. Zero is the normal answer for a resting maker
    /// order in a book that has not come to it.
    Qty quantity{};
    /// Volume-weighted price of that quantity. Never worse than the order's
    /// limit -- a limit order that executed through its own limit would be a
    /// simulator bug, and the caller asserts it.
    Px price{};
    /// A resting order that the book came to is a maker; an order marketable on
    /// arrival is a taker.
    Liquidity liquidity = Liquidity::Maker;
    /// Levels consumed, so a test can tell a one-level fill from a sweep.
    std::uint32_t levels_consumed = 0;
};

/// How much of `available` is executable against `order`, **consuming what it
/// takes**.
///
/// The consumption is the point. A book is a market-data view: it does not
/// shrink because we filled against it. If each of our orders matched the
/// untouched book independently, two orders at the same price would each be
/// handed the same displayed size, and the simulator would print more volume
/// than the market showed. Callers build one working copy of the opposite side
/// per market update and pass every order through it in priority order.
///
/// `queue_share_bps` models the size queued ahead of us at each level: at
/// 5000, half of the displayed quantity is assumed to belong to somebody who
/// was there first. It is an approximation and is documented as one --
/// reproducing a real exchange's queue needs per-order arrival data that a
/// depth feed does not carry.
[[nodiscard]] MatchResult match_and_consume(const PaperOrder& order,
                                            std::vector<PriceLevel>& available, FillModel model,
                                            std::uint32_t queue_share_bps) noexcept;

/// The opposite side of `book` for `side`, as a working copy for
/// `match_and_consume`.
[[nodiscard]] std::vector<PriceLevel> executable_side(const book::OrderBook& book,
                                                      Side side);

/// Single-order convenience for arrival matching, where there is no queue of
/// our own orders to share liquidity with.
[[nodiscard]] MatchResult match_against_book(const PaperOrder& order,
                                             const book::OrderBook& book, FillModel model,
                                             std::uint32_t queue_share_bps);

/// Whether an order arriving now would immediately take liquidity (§12).
///
/// This is the question post-only exists to ask, and it is answered explicitly
/// rather than left to fall out of the matching path: a post-only order that
/// silently became a taker is a strategy trading a different instrument from
/// the one it thinks it is trading.
[[nodiscard]] bool is_marketable_on_arrival(const PaperOrder& order,
                                            const book::OrderBook& book) noexcept;

}  // namespace mm::exchange::paper
