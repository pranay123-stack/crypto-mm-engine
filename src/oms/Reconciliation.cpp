#include "mm/oms/Reconciliation.hpp"

namespace mm::oms {

std::string_view to_string(DiscrepancyKind k) noexcept {
    switch (k) {
        case DiscrepancyKind::None:              return "NONE";
        case DiscrepancyKind::OrphanAtVenue:     return "ORPHAN_AT_VENUE";
        case DiscrepancyKind::MissingAtVenue:    return "MISSING_AT_VENUE";
        case DiscrepancyKind::QuantityMismatch:  return "QUANTITY_MISMATCH";
        case DiscrepancyKind::PriceMismatch:     return "PRICE_MISMATCH";
        case DiscrepancyKind::StatusMismatch:    return "STATUS_MISMATCH";
        case DiscrepancyKind::IdentityCollision: return "IDENTITY_COLLISION";
    }
    return "UNKNOWN";
}

}  // namespace mm::oms
