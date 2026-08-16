#pragma once

/// \file PositionState.hpp
/// Position and exposure, as risk is allowed to see them.
///
/// ## One authoritative position
///
/// There is exactly one source of truth for position, and risk *consumes* it.
/// It does not maintain its own, because a strategy's view, a quote manager's
/// view and a risk view would become three competing truths and the system
/// would have no way to tell which was right.
///
/// Until the portfolio layer exists (Phase 10) that source is the OMS. Whoever
/// it is, `PositionSnapshot::valid` and its freshness are the engine's licence
/// to reason at all: without them it refuses to add exposure.
///
/// ## Exposure mathematics
///
/// A market maker rests on both sides at once, and the two cannot be assumed to
/// fill together. Position limits are therefore evaluated **worst case per
/// side**:
///
/// ```
///   working_buy   = Σ remaining quantity of live or pending BUY orders
///   working_sell  = Σ remaining quantity of live or pending SELL orders
///
///   worst_case_long  = position + working_buy      // every bid fills, no ask does
///   worst_case_short = position − working_sell     // every ask fills, no bid does
///
///   position limit holds  ⟺  |worst_case_long| ≤ max_position
///                        and  |worst_case_short| ≤ max_position
/// ```
///
/// Worked example. Position +8, a resting bid for 1, limit 10:
///
/// ```
///   worst_case_long  = 8 + 1 = 9      ✓ within 10
///   a new bid for 1  → 8 + 1 + 1 = 10 ✓ exactly at the limit, permitted
///   a further bid    → 11             ✗ rejected
/// ```
///
/// Netting the two sides — treating a resting bid and ask as cancelling — would
/// approve that fourth order, and it is wrong: nothing guarantees the ask fills.
///
/// **Gross working exposure** is a separate notion, limited separately:
/// `working_buy + working_sell`. It bounds how much is in the market at once,
/// regardless of direction.

#include <cstdint>

#include "mm/common/Time.hpp"
#include "mm/common/Types.hpp"

namespace mm::risk {

/// The authoritative position, as reported by its owner.
struct PositionSnapshot {
    /// False when no authoritative position is available. The engine then
    /// refuses to add exposure: it cannot reason about a limit on a quantity it
    /// does not know.
    bool valid = false;

    Symbol symbol{};
    /// Signed: negative is short.
    Qty quantity{};
    /// Signed notional at the position's own average price, where the owner
    /// supplies it. Zero when unknown; the engine then falls back to marking
    /// against the reference price rather than assuming zero exposure.
    Notional notional{};
    Px average_price{};

    /// Steady clock, when this snapshot was produced. Compared against the
    /// configured maximum age; a position from a minute ago describes a
    /// position that may no longer exist.
    Nanos as_of_ns = 0;
    /// Monotonic. An older snapshot must never overwrite a newer one.
    std::uint64_t sequence = 0;

    [[nodiscard]] bool is_long() const noexcept { return quantity.is_positive(); }
    [[nodiscard]] bool is_short() const noexcept { return quantity.is_negative(); }
    [[nodiscard]] bool is_flat() const noexcept { return quantity.is_zero(); }
};

static_assert(std::is_trivially_copyable_v<PositionSnapshot>);

/// Working-order exposure, derived from OMS-reported state.
///
/// Risk does not run a second order state machine. It consumes what the OMS
/// says and converts it into the two side totals above.
///
/// How each order state contributes:
///
/// | OMS state        | Contributes | Why |
/// | ---------------- | ----------- | --- |
/// | live / resting   | remaining   | it can fill |
/// | pending new      | full size   | it may already be working at the venue |
/// | pending replace  | remaining   | the old order is still live until confirmed |
/// | pending cancel   | remaining   | a cancel is a request, not a result |
/// | unknown          | **blocks**  | exposure cannot be established at all |
/// | terminal         | nothing     | it can no longer fill |
///
/// A pending cancel still counts. Releasing exposure on the *request* rather
/// than the confirmation is how a maker ends up over its limit when a cancel is
/// rejected or lost.
struct ExposureSnapshot {
    /// False when any relevant order's state could not be determined. The
    /// engine then refuses new exposure: an unknown order might be working.
    bool determinate = true;

    Qty working_buy{};   ///< always non-negative
    Qty working_sell{};  ///< always non-negative
    Notional working_buy_notional{};
    Notional working_sell_notional{};

    std::uint32_t open_order_count = 0;
    /// Orders whose state prevented a determinate answer, for diagnostics.
    std::uint32_t indeterminate_order_count = 0;

    [[nodiscard]] Qty gross() const noexcept { return working_buy + working_sell; }
};

static_assert(std::is_trivially_copyable_v<ExposureSnapshot>);

/// The two worst-case positions, and whether they could be computed at all.
struct WorstCaseExposure {
    bool valid = false;
    Qty worst_case_long{};   ///< position + working_buy
    Qty worst_case_short{};  ///< position - working_sell

    /// The larger of the two in absolute terms: the number a single position
    /// limit is compared against.
    [[nodiscard]] Qty peak_absolute() const noexcept {
        const Qty a = saturating_abs(worst_case_long);
        const Qty b = saturating_abs(worst_case_short);
        return a > b ? a : b;
    }
};

/// Computes worst-case exposure, reporting failure rather than wrapping.
/// Returns false on arithmetic overflow, which the caller treats as "cannot
/// prove safety" and therefore as a rejection.
[[nodiscard]] bool compute_worst_case(const PositionSnapshot& position,
                                      const ExposureSnapshot& exposure,
                                      WorstCaseExposure& out) noexcept;

/// Worst case after adding `quantity` on `side`, as if the new order were
/// already working. This is what a NEW or REPLACE is evaluated against.
[[nodiscard]] bool compute_worst_case_with(const PositionSnapshot& position,
                                           const ExposureSnapshot& exposure, Side side,
                                           Qty quantity, WorstCaseExposure& out) noexcept;

/// True when adding `quantity` on `side` would increase the absolute position
/// in the worst case. Used by the reduce-only state, where an action that
/// unwinds risk is permitted and one that adds to it is not.
[[nodiscard]] bool increases_absolute_position(const PositionSnapshot& position, Side side,
                                               Qty quantity) noexcept;

}  // namespace mm::risk
