#pragma once

/// \file RiskEngine.hpp
/// The safety boundary.
///
/// **Every order-changing action passes through here.** The quote manager says
/// what the strategy wants; this decides whether it is permitted. There is no
/// path from the quote manager to the OMS that bypasses it.
///
/// ## The governing rule
///
/// > If risk cannot **prove** that an exposure-adding action is safe, the
/// > action is rejected.
///
/// Missing position, stale market, unknown order state, arithmetic overflow,
/// absent configuration, an unarmed engine — every one of them refuses new
/// exposure. Cancellation stays available in all of them, because the moment
/// you most need to withdraw is the moment things are least certain.
///
/// ## Concurrency
///
/// Owned by the trading thread, like every other component that mutates trading
/// state (docs/concurrency.md §1). One authoritative mutation path, no locks,
/// no I/O. Position and working-order state arrive as inputs; the engine never
/// maintains a competing copy.
///
/// ## Not here
///
/// No exchange code, no strategy logic, no order management, no PnL accounting.
/// The engine is identical for paper and live, and cannot tell which it is
/// serving.

#include <cstdint>

#include "mm/common/LatencyHistogram.hpp"
#include "mm/common/Time.hpp"
#include "mm/risk/RateLimiter.hpp"
#include "mm/risk/RiskDecision.hpp"
#include "mm/risk/RiskLimits.hpp"

namespace mm::risk {

/// Everything the engine needs besides the action itself.
struct RiskInput {
    Symbol symbol{};
    /// Venue trading rules; needed to revalidate a reduced quantity.
    const InstrumentSpec* instrument = nullptr;

    /// The authoritative position. `valid == false` refuses new exposure.
    PositionSnapshot position{};
    /// Working-order exposure as reported by the OMS.
    ExposureSnapshot exposure{};

    /// Reference market. `bbo.is_sane()` gates price-band checks.
    BestBidAsk bbo{};
    Nanos market_data_age_ns = 0;

    /// False when the engine as a whole is not ready to trade.
    bool system_ready = true;

    /// Whether the venue guarantees an atomic replace.
    ///
    /// This changes the exposure arithmetic materially. With an atomic swap the
    /// old order is gone the instant the new one exists, so the delta is
    /// `new - old`. Without one -- a cancel followed by a new order -- both can
    /// be live at once, and the full new quantity lands on top of the old.
    /// Assuming atomicity that the venue does not provide would understate
    /// exposure during exactly the window in which it is highest.
    bool atomic_replace = false;
    /// Remaining size of the order being replaced. Supplied by the caller,
    /// which holds the working state; only meaningful for a Replace.
    Qty replaced_order_remaining{};
    /// The strategy this action claims to come from, for ownership checks.
    StrategyName expected_owner{};
};

struct RiskMetrics {
    std::uint64_t evaluations = 0;
    std::uint64_t approvals = 0;
    std::uint64_t reductions = 0;
    std::uint64_t rejections = 0;
    std::uint64_t cancels_permitted_while_blocked = 0;
    std::uint64_t kills = 0;

    /// Per-cause rejection tallies, indexed by `RiskReason`. A risk engine that
    /// refuses everything must be diagnosable from counters alone.
    std::array<std::uint64_t, 32> rejections_by_reason{};

    [[nodiscard]] std::uint64_t count(RiskReason reason) const noexcept {
        const auto index = static_cast<std::size_t>(reason);
        return index < rejections_by_reason.size() ? rejections_by_reason[index] : 0;
    }
};

class RiskEngine {
public:
    RiskEngine(RiskLimits limits, const Clock& clock);

    /// Moves Disarmed -> Armed. Refuses when essential limits are unset: a zero
    /// limit is silently unlimited, and arming into that state would defeat the
    /// engine entirely.
    [[nodiscard]] Status arm();

    /// Enters reduce-only. Position-reducing actions still pass.
    void warn(RiskReason reason);
    /// Hard kill. No new exposure; cancellation stays available; no automatic
    /// recovery.
    void kill(RiskReason reason);
    /// An internal invariant failed. Same prohibition as a kill, distinct cause.
    void fault(RiskReason reason);

    /// Operator recovery. Deliberately explicit: the condition that tripped a
    /// kill does not clear itself, so neither does the kill.
    [[nodiscard]] Status rearm();

    /// The single entry point. Every order-changing action passes through it.
    [[nodiscard]] RiskDecision evaluate(const OrderAction& action, const RiskInput& input);

    /// Emergency withdrawal as a *decision*, not an execution: the OMS performs
    /// the cancellations. Cancels are approved even when killed or faulted.
    [[nodiscard]] RiskDecision approve_cancel(const OrderAction& cancel, const RiskInput& input);

    [[nodiscard]] RiskState state() const noexcept { return state_; }
    [[nodiscard]] RiskReason last_state_reason() const noexcept { return state_reason_; }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const RiskMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const LatencyHistogram& latency() const noexcept { return latency_; }

    /// Rate-limit budget remaining, for observability.
    [[nodiscard]] double new_order_budget() const noexcept {
        return new_orders_.available(clock_.steady());
    }

private:
    void transition(RiskState to, RiskReason reason);
    void record(const RiskDecision& decision);

    /// Checks that do not depend on the action's exposure: state, readiness,
    /// freshness, instrument, ownership.
    [[nodiscard]] RiskReason screen(const OrderAction& action, const RiskInput& input) const;

    /// Per-order bounds and the price band.
    [[nodiscard]] RiskReason check_order_bounds(const OrderAction& action, Qty quantity,
                                                const RiskInput& input,
                                                const SymbolLimits& limits,
                                                LimitContext& context) const;

    /// Exposure arithmetic. Returns the reason on failure, `None` on success.
    [[nodiscard]] RiskReason check_exposure(const OrderAction& action, Qty quantity,
                                            const RiskInput& input, const SymbolLimits& limits,
                                            LimitContext& context) const;

    /// Largest quantity that satisfies every bound, or zero when none does.
    [[nodiscard]] Qty largest_permitted_quantity(const OrderAction& action,
                                                 const RiskInput& input,
                                                 const SymbolLimits& limits) const;

    RiskLimits limits_;
    const Clock& clock_;

    RiskState state_ = RiskState::Disarmed;
    RiskReason state_reason_ = RiskReason::None;

    RateLimiter new_orders_;
    RateLimiter cancels_;
    RateLimiter replaces_;
    RateLimiter all_actions_;

    RiskMetrics metrics_{};
    LatencyHistogram latency_{"risk_evaluation"};
};

}  // namespace mm::risk
