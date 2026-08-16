#pragma once

/// \file WorkingQuoteState.hpp
/// What the manager is told currently exists at the venue.
///
/// This is an *input*. The quote manager does not track order state itself --
/// that is the OMS's job, and duplicating it here would create a second opinion
/// about what is resting. The OMS will supply this view; until Phase 8 exists,
/// tests supply it directly.

#include <array>
#include <cstdint>

#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/quote/QuoteTypes.hpp"

namespace mm::quote {

using exchange::OrderStatus;

/// One order as the manager sees it.
struct WorkingOrder {
    bool present = false;

    QuoteOwner owner{};
    Side side = Side::Buy;
    Px price{};

    /// As submitted, and as filled so far. `remaining()` is what is actually
    /// resting, which is what a desired quantity must be compared against --
    /// assuming the original quantity still rests after a partial fill would
    /// leave the book quoting less than intended without anyone noticing.
    Qty original_quantity{};
    Qty cumulative_quantity{};

    OrderStatus status = OrderStatus::Unknown;
    PendingOperation pending = PendingOperation::None;

    /// Identifiers, carried opaquely. The manager reads them only to address a
    /// cancel or replace at the right order; it never parses or constructs one.
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};

    /// The intent generation this order was created for. Lets the manager tell
    /// an order representing current intent from one left over.
    std::uint64_t generation = 0;
    /// Steady clock, when this order was last acted on. Drives replace-interval
    /// and cooldown controls.
    Nanos last_action_ns = 0;

    [[nodiscard]] Qty remaining() const noexcept {
        const Qty left = original_quantity - cumulative_quantity;
        return left.is_positive() ? left : Qty::zero();
    }
    [[nodiscard]] bool is_partially_filled() const noexcept {
        return cumulative_quantity.is_positive() && remaining().is_positive();
    }
    /// True when the venue can still say something further about this order.
    [[nodiscard]] bool is_live() const noexcept {
        return present && !exchange::is_terminal(status);
    }
    /// True when we do not know whether this order exists. The slot must not be
    /// re-driven while this holds.
    [[nodiscard]] bool is_uncertain() const noexcept {
        return present && status == OrderStatus::Unknown;
    }
    [[nodiscard]] bool has_pending_request() const noexcept {
        return pending != PendingOperation::None;
    }
};

static_assert(std::is_trivially_copyable_v<WorkingOrder>);

/// Everything currently resting for one symbol, from the manager's point of
/// view: its own two slots, plus a count of orders it does not own.
struct WorkingQuoteState {
    std::array<WorkingOrder, kSlotCount> slots{};

    /// Orders on this symbol belonging to somebody else -- an operator, an
    /// emergency flattening order, another strategy. Recorded so the manager
    /// can report that it saw them; it may never act on them.
    std::uint32_t foreign_order_count = 0;

    [[nodiscard]] WorkingOrder& slot(QuoteSlot which) noexcept {
        return slots[static_cast<std::size_t>(which)];
    }
    [[nodiscard]] const WorkingOrder& slot(QuoteSlot which) const noexcept {
        return slots[static_cast<std::size_t>(which)];
    }
    [[nodiscard]] bool any_live() const noexcept {
        return slots[0].is_live() || slots[1].is_live();
    }
};

static_assert(std::is_trivially_copyable_v<WorkingQuoteState>);

}  // namespace mm::quote
