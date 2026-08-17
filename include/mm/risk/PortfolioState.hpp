#pragma once

/// \file PortfolioState.hpp
/// The portfolio view the risk engine consumes.
///
/// Declared here, in `mm_risk`, for the same reason `PositionSnapshot` is:
/// **risk defines what it needs to be told, and the accounting layer supplies
/// it.** The dependency points downward -- `mm_portfolio` links `mm_risk` and
/// produces this type -- so risk never learns that an accounting layer exists,
/// and there is no cycle.
///
/// Phase 7 configured `max_portfolio_notional` and the loss limits but could
/// not enforce them, because cross-symbol state and PnL did not exist until the
/// portfolio layer (docs/risk-engine.md §16). This is that missing input.

#include <cstdint>

#include "mm/common/Fixed.hpp"
#include "mm/common/Time.hpp"

namespace mm::risk {

struct PortfolioState {
    /// False when accounting cannot vouch for these numbers -- a faulted
    /// account, an arithmetic failure, an unregistered symbol. Risk must treat
    /// it exactly as it treats an invalid position: refuse to add exposure.
    /// It must never be read as "the portfolio is flat".
    bool valid = false;

    /// False when any symbol's unrealized PnL is indeterminate, typically a
    /// stale or missing mark. Notional limits can still be enforced (exposure
    /// is known even when its mark is not); **loss limits cannot**, and
    /// evaluating them against a partial total would understate a loss by
    /// exactly the symbols that could not be marked.
    bool pnl_determinate = false;

    /// Sum of |position| * mark across symbols. Absolute: a long and a short
    /// are two exposures, not none.
    Notional gross_notional{};
    Notional net_notional{};

    Notional realized{};
    Notional unrealized{};
    Notional fees{};
    /// realized + unrealized + funding - fees.
    Notional net_pnl{};

    Nanos as_of_ns = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool usable_for_notional_limits() const noexcept { return valid; }
    [[nodiscard]] bool usable_for_loss_limits() const noexcept { return valid && pnl_determinate; }
};

static_assert(std::is_trivially_copyable_v<PortfolioState>);

}  // namespace mm::risk
