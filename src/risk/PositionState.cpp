#include "mm/risk/PositionState.hpp"

namespace mm::risk {

bool compute_worst_case(const PositionSnapshot& position, const ExposureSnapshot& exposure,
                        WorstCaseExposure& out) noexcept {
    out = WorstCaseExposure{};
    if (!position.valid || !exposure.determinate) {
        return false;
    }
    // Checked throughout: a wrapped sum would understate exposure, and the
    // whole purpose of this function is to establish an upper bound.
    if (!checked_add(position.quantity, exposure.working_buy, out.worst_case_long)) {
        return false;
    }
    if (!checked_sub(position.quantity, exposure.working_sell, out.worst_case_short)) {
        return false;
    }
    out.valid = true;
    return true;
}

bool compute_worst_case_with(const PositionSnapshot& position, const ExposureSnapshot& exposure,
                             Side side, Qty quantity, WorstCaseExposure& out) noexcept {
    ExposureSnapshot projected = exposure;
    // The prospective order is folded into the side it would rest on, and only
    // that side: a new bid cannot make us shorter.
    if (side == Side::Buy) {
        if (!checked_add(projected.working_buy, quantity, projected.working_buy)) {
            return false;
        }
    } else {
        if (!checked_add(projected.working_sell, quantity, projected.working_sell)) {
            return false;
        }
    }
    return compute_worst_case(position, projected, out);
}

bool increases_absolute_position(const PositionSnapshot& position, Side side,
                                 Qty quantity) noexcept {
    if (!quantity.is_positive()) {
        return false;
    }
    if (position.is_flat()) {
        // From flat, any fill opens a position in some direction.
        return true;
    }
    // Buying while long, or selling while short, adds to the exposure already
    // held. The opposite unwinds it -- which is what reduce-only permits.
    return (side == Side::Buy) == position.is_long();
}

}  // namespace mm::risk
