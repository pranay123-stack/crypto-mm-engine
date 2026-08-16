#pragma once

/// \file RiskLimits.hpp
/// The configured bounds, per symbol and globally.
///
/// A limit left at zero means **unset**, and an unset limit is not enforced.
/// That is deliberate but dangerous, so `validate_for_arming()` refuses to arm
/// an engine whose essential limits are missing: unset must never be a quiet
/// synonym for unlimited on a live venue.

#include <cstdint>

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"

namespace mm::risk {

/// Bounds for one instrument.
struct SymbolLimits {
    Symbol symbol{};

    /// Absolute worst-case position, either direction. Compared against
    /// `WorstCaseExposure::peak_absolute()`.
    Qty max_position{};
    /// Absolute worst-case position valued at the reference price.
    Notional max_position_notional{};

    /// Per-order bounds, applied before any exposure arithmetic.
    Qty max_order_quantity{};
    Notional max_order_notional{};

    /// Gross working exposure: working_buy + working_sell.
    Qty max_working_exposure{};
    /// Per-side working exposure, so one side cannot consume the whole budget.
    Qty max_side_exposure{};

    std::int32_t max_open_orders = 0;

    /// Price band around the reference price, in basis points. An order priced
    /// outside it is refused: a quote far from the market is almost always
    /// arithmetic gone wrong, and the venue would reject it anyway.
    std::int32_t price_band_bps = 0;

    [[nodiscard]] bool has_position_limit() const noexcept { return max_position.is_positive(); }
};

/// Bounds across every instrument. Portfolio state does not exist until Phase
/// 10, so these are configured and validated but only the parts derivable from
/// per-symbol state are enforced today -- see `docs/risk-engine.md`.
struct GlobalLimits {
    Notional max_portfolio_notional{};
    std::int32_t max_open_orders_total = 0;

    // ---- rate limits ----
    std::int32_t max_new_orders_per_second = 0;
    std::int32_t max_cancels_per_second = 0;
    std::int32_t max_replaces_per_second = 0;
    std::int32_t max_actions_per_second = 0;
    /// Burst allowance. A market-data burst legitimately produces a cluster of
    /// actions; the bucket absorbs it while bounding the sustained rate.
    std::int32_t burst_capacity = 0;

    // ---- freshness ----
    Nanos max_market_data_age_ns = millis(500);
    Nanos max_position_age_ns = seconds(5);
};

struct RiskLimits {
    GlobalLimits global{};
    /// Per-symbol bounds. Symbols are configured, never hard-coded.
    std::array<SymbolLimits, 32> symbols{};
    std::uint8_t symbol_count = 0;

    [[nodiscard]] const SymbolLimits* find(const Symbol& symbol) const noexcept {
        for (std::uint8_t i = 0; i < symbol_count; ++i) {
            if (symbols[i].symbol == symbol) {
                return &symbols[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool add(const SymbolLimits& limits) noexcept {
        if (symbol_count >= symbols.size() || find(limits.symbol) != nullptr) {
            return false;
        }
        symbols[symbol_count++] = limits;
        return true;
    }

    /// Refuses to arm when an essential bound is missing. A zero limit is
    /// silently unlimited, which is exactly the failure a risk engine exists to
    /// prevent, so arming requires them to be stated.
    [[nodiscard]] Status validate_for_arming() const;
};

}  // namespace mm::risk
