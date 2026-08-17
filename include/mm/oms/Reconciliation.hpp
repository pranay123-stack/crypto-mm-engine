#pragma once

/// \file Reconciliation.hpp
/// Comparing what we believe against what the venue reports.
///
/// **Reconciliation detects; it does not silently correct.** A discrepancy can
/// mean the venue is right, or that a snapshot raced an event we have already
/// applied. Guessing wrong in either direction is worse than surfacing it, so
/// unresolved discrepancies become explicit state for an operator rather than
/// automatic transitions.

#include <cstdint>
#include <vector>

#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/oms/OrderRecord.hpp"

namespace mm::oms {

/// What kind of disagreement was found.
enum class DiscrepancyKind : std::uint8_t {
    None = 0,
    /// The venue has an order we have no record of. Never assumed to be ours.
    OrphanAtVenue,
    /// We believe an order is live; the venue's listing does not contain it.
    MissingAtVenue,
    /// Both know the order, but the executed quantities differ.
    QuantityMismatch,
    PriceMismatch,
    StatusMismatch,
    /// One exchange id maps to more than one local order, or vice versa.
    IdentityCollision,
};
[[nodiscard]] std::string_view to_string(DiscrepancyKind k) noexcept;

struct Discrepancy {
    DiscrepancyKind kind = DiscrepancyKind::None;
    LogicalOrderId logical_id = LogicalOrderId::kInvalid;
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};

    /// What we believed and what the venue said, so the record is meaningful
    /// after the fact.
    OrderState local_state = OrderState::Unknown;
    exchange::OrderStatus venue_status = exchange::OrderStatus::Unknown;
    Qty local_cumulative{};
    Qty venue_cumulative{};
    Px local_price{};
    Px venue_price{};

    /// True when the local record was produced by this session's id scheme, so
    /// an orphan can be told from another process's order.
    bool identity_is_ours = false;
};

/// A complete venue listing, assembled from `OpenOrdersSnapshotEvent` chunks.
///
/// Completeness matters more than it looks: concluding "the venue does not have
/// this order" from a partial listing would cancel-or-forget live orders. The
/// OMS only reconciles once `is_complete` is true.
struct VenueSnapshot {
    std::vector<exchange::OrderStatusReport> orders;
    Symbol symbol_filter{};
    bool is_complete = false;
    Nanos received_ns = 0;
};

struct ReconciliationReport {
    std::vector<Discrepancy> discrepancies;
    std::uint32_t orders_compared = 0;
    std::uint32_t orders_agreed = 0;
    Nanos performed_ns = 0;

    [[nodiscard]] bool clean() const noexcept { return discrepancies.empty(); }
    [[nodiscard]] std::size_t count_of(DiscrepancyKind kind) const {
        std::size_t n = 0;
        for (const auto& d : discrepancies) {
            if (d.kind == kind) {
                ++n;
            }
        }
        return n;
    }
};

}  // namespace mm::oms
