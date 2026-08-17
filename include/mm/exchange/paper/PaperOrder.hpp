#pragma once

/// \file PaperOrder.hpp
/// The venue's own record of an order.
///
/// **This is deliberately a separate type from `oms::OrderRecord`, and the two
/// are never shared (§24).** A simulator that let the OMS and the venue point
/// at one object would silently guarantee they agree -- and agreeing is exactly
/// the property the production system does *not* get for free. Every
/// synchronization bug worth catching lives in the gap between what we believe
/// and what the venue believes, so the simulator has to have that gap.

#include <cstdint>
#include <string_view>

#include "mm/common/Fixed.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::exchange::paper {

/// An order as the simulated venue sees it. No logical id, no ownership, no
/// generation: a venue knows none of those things.
struct PaperOrder {
    /// Minted by the venue. Never derived from the client id, so a test that
    /// accidentally relies on them matching will fail here rather than in
    /// production (§21).
    ExchangeOrderId exchange_order_id{};
    ClientOrderId client_order_id{};

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    bool post_only = false;

    Px price{};
    Qty original_qty{};
    Qty cumulative_qty{};

    OrderStatus status = OrderStatus::New;

    /// Arrival order at this price, assigned when the order becomes resting.
    /// Together with price this gives the deterministic priority of §10.
    std::uint64_t arrival_sequence = 0;

    /// Venue-side clock readings, for latency accounting and snapshots.
    Nanos accepted_ns = 0;
    Nanos last_event_ns = 0;

    /// True once a cancel has been accepted but the confirming event has not
    /// yet been delivered. The order can still fill in this window, which is
    /// the cancel/fill race the OMS must survive.
    bool cancel_pending = false;

    [[nodiscard]] Qty remaining() const noexcept {
        Qty out;
        if (!checked_sub(original_qty, cumulative_qty, out)) {
            return Qty{};
        }
        return out.is_negative() ? Qty{} : out;
    }

    [[nodiscard]] bool is_resting() const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }

    [[nodiscard]] bool is_terminal() const noexcept {
        return status == OrderStatus::Filled || status == OrderStatus::Canceled ||
               status == OrderStatus::Rejected || status == OrderStatus::Expired;
    }

    /// Would this order cross the given opposite-side price on arrival?
    [[nodiscard]] bool would_cross(Px opposite) const noexcept {
        if (!opposite.is_positive()) {
            return false;
        }
        return side == Side::Buy ? price >= opposite : price <= opposite;
    }
};

static_assert(std::is_trivially_copyable_v<PaperOrder>,
              "the venue's order record is copied into snapshots and replay logs");

}  // namespace mm::exchange::paper
