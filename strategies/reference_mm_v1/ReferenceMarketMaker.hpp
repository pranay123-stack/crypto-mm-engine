#pragma once

/// \file ReferenceMarketMaker.hpp
///
/// ============================ TEST / REFERENCE ONLY =========================
///
/// This strategy exists **solely to validate the strategy runtime**. It is not
/// a trading strategy.
///
///  * No claim is made that it is profitable. It almost certainly is not.
///  * It is **not suitable for live trading** and must not be used as such.
///  * It contains no alpha, no research, no calibration and no edge. It quotes
///    symmetrically around the mid at a fixed spread with a linear inventory
///    skew, because that is the least interesting thing that still exercises
///    every path in the runtime: quoting, pulling, rounding, inventory
///    response, and refusing to quote when conditions are wrong.
///
/// A finalized strategy arrives from the separate quant research platform. This
/// file shows what such a strategy plugs into; it does not show what one should
/// contain.
///
/// ============================================================================
///
/// It is also the reference for the properties every strategy must have:
/// deterministic, allocation-free after construction, clock-free (time comes
/// from the context), and incapable of reaching anything outside its inputs.

#include "mm/strategy/IStrategy.hpp"

namespace mm::strategies {

class ReferenceMarketMaker final : public strategy::IStrategy {
public:
    /// Finalized parameters, as they would arrive from research.
    struct Parameters {
        /// Half-spread around the reference price, in basis points.
        double half_spread_bps = 5.0;
        /// Size quoted on each side.
        Qty quote_size{};
        /// How far inventory pushes quotes away from the mid, in basis points
        /// at full position limit. Zero disables skew.
        double inventory_skew_bps = 0.0;
        /// Stop quoting a side once inventory reaches this fraction of the
        /// limit, in [0, 1].
        double max_inventory_utilisation = 0.9;
        /// Refuse to quote when the market spread is below this; a maker with
        /// no room to stand inside the spread has nothing to do.
        double min_market_spread_bps = 0.0;
        /// Use the size-weighted micro price rather than the mid as reference.
        bool use_micro_price = false;
    };

    ReferenceMarketMaker() = default;

    [[nodiscard]] Status initialize(const strategy::StrategyInit& init) override;
    [[nodiscard]] strategy::QuoteIntent on_market_update(
        const strategy::StrategyContext& context) override;
    [[nodiscard]] strategy::QuoteIntent on_timer(
        const strategy::StrategyContext& context) override;
    void on_fill(const strategy::StrategyFill& fill) override;
    void on_order_event(const strategy::StrategyOrderEvent& event) override;
    void reset() override;
    [[nodiscard]] strategy::StrategyIdentity identity() const override;

    /// Exposed for tests; not part of `IStrategy`.
    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }
    [[nodiscard]] std::uint64_t fills_seen() const noexcept { return fills_seen_; }

private:
    /// The single decision function. Both entry points delegate here, so a
    /// timer evaluation and a market evaluation cannot drift apart.
    [[nodiscard]] strategy::QuoteIntent decide(const strategy::StrategyContext& context) const;

    Parameters params_{};
    InstrumentSpec instrument_{};
    bool initialized_ = false;
    std::uint64_t fills_seen_ = 0;
};

}  // namespace mm::strategies
