#pragma once

/// \file PaperTypes.hpp
/// Configuration and metrics for the paper execution environment.
///
/// **Paper execution is an execution simulator for validating the live engine
/// architecture. It is not a backtesting engine.** It consumes the current
/// normalized book, never history, and it exists to prove that the
/// OMS -> IExchangeExecution boundary works before a real venue is on the
/// other side of it.

#include <cstdint>
#include <string_view>

#include "mm/common/Time.hpp"
#include "mm/common/Types.hpp"

namespace mm::exchange::paper {

/// How a resting order is filled when the book becomes executable against it.
enum class FillModel : std::uint8_t {
    /// A resting order fills only against liquidity that is *displayed* on the
    /// opposite side and priced through it, capped by that displayed size.
    ///
    /// This is the conservative default. The alternative -- "the price touched
    /// my level, so I am filled" -- is the single assumption that most flatters
    /// a paper run, because it grants a fill for every trade that reaches the
    /// price regardless of how much size was actually there or who was ahead.
    DisplayedLiquidity,

    /// A resting order fills in full the moment the opposite side crosses it.
    ///
    /// Deliberately optimistic and never the default. It exists so a test can
    /// drive a full-fill path in one step, and so the difference between the
    /// two models is visible rather than theoretical.
    FullOnCross,
};
[[nodiscard]] std::string_view to_string(FillModel m) noexcept;

/// What happens to a post-only order that would cross on arrival.
enum class PostOnlyPolicy : std::uint8_t {
    /// Refused with `RejectReason::PostOnlyWouldCross`. This is what Binance
    /// and most venues do, and it is the default.
    Reject,
    /// Refused, and the venue reports the order as expired rather than
    /// rejected. Some venues model it this way.
    Expire,
};
[[nodiscard]] std::string_view to_string(PostOnlyPolicy p) noexcept;

/// Whether the simulated venue offers atomic replace.
enum class ReplaceMode : std::uint8_t {
    /// One request, one answer, identity swapped atomically.
    Atomic,
    /// The venue has no replace at all: `replace()` returns an error and the
    /// quote manager must decompose it consciously into cancel-then-new.
    /// Claiming atomicity we do not have would hide the exposure window that
    /// decomposition actually creates.
    Unsupported,
};
[[nodiscard]] std::string_view to_string(ReplaceMode m) noexcept;

/// Latency the simulated venue applies to each stage.
///
/// **These are simulation values chosen to be structurally realistic. They are
/// not measurements of Binance or of any other venue, and nothing should quote
/// them as such.** Their purpose is to force the engine to cope with answers
/// that arrive later than the request, which is the property that matters.
struct PaperLatency {
    /// Request in flight before the venue looks at it.
    Nanos request_ns = micros(500);
    /// Venue decision time for a new order, added to `request_ns`.
    Nanos ack_ns = micros(1'500);
    Nanos cancel_ns = micros(1'500);
    Nanos replace_ns = micros(2'000);
    /// Delay between a fill occurring and its report reaching us.
    Nanos fill_ns = micros(1'000);
    /// Delay on a query answer (open orders, single order).
    Nanos query_ns = micros(5'000);

    [[nodiscard]] constexpr Nanos new_total() const noexcept { return request_ns + ack_ns; }
    [[nodiscard]] constexpr Nanos cancel_total() const noexcept { return request_ns + cancel_ns; }
    [[nodiscard]] constexpr Nanos replace_total() const noexcept { return request_ns + replace_ns; }
};

/// Deterministic failure injection (§18).
///
/// Every field here is a *count* or a *switch*, never a probability. A test
/// that fails one time in twenty is worse than no test: it trains people to
/// re-run rather than to look. Stochastic simulation, if it is ever wanted,
/// belongs behind `deterministic_seed` and stays out of the default path.
struct PaperFaults {
    /// Refuse the next N new orders.
    std::uint32_t reject_next_new = 0;
    /// Refuse the next N cancels; the order stays working.
    std::uint32_t reject_next_cancel = 0;
    /// Refuse the next N replaces; the original keeps its parameters.
    std::uint32_t reject_next_replace = 0;
    /// Never answer the next N requests at all. The engine must time out and
    /// conclude Unknown rather than inventing an outcome.
    std::uint32_t drop_next_request = 0;
    /// Emit the next N events twice.
    std::uint32_t duplicate_next_event = 0;
    /// Deliver the next N events *before* an event that was scheduled earlier,
    /// so the engine sees them out of the order the venue produced them.
    std::uint32_t reorder_next_event = 0;
    /// Add this much extra latency to the next request only.
    Nanos delay_next_request_ns = 0;
    /// Omit resting orders from the next open-orders snapshot, so
    /// reconciliation sees a discrepancy it must handle.
    std::uint32_t hide_next_snapshot_orders = 0;
    /// Report an execution error instead of a normal answer.
    std::uint32_t error_next_request = 0;

    [[nodiscard]] constexpr bool any() const noexcept {
        return reject_next_new != 0 || reject_next_cancel != 0 || reject_next_replace != 0 ||
               drop_next_request != 0 || duplicate_next_event != 0 || reorder_next_event != 0 ||
               delay_next_request_ns != 0 || hide_next_snapshot_orders != 0 ||
               error_next_request != 0;
    }
};

/// Everything the paper venue needs to behave deterministically.
struct PaperExecutionConfig {
    VenueName venue{"paper"};

    PaperLatency latency{};
    PaperFaults faults{};

    FillModel fill_model = FillModel::DisplayedLiquidity;
    PostOnlyPolicy post_only_policy = PostOnlyPolicy::Reject;
    ReplaceMode replace_mode = ReplaceMode::Atomic;

    /// Fraction of displayed liquidity a resting order may take, in basis
    /// points of that liquidity, modelling the orders queued ahead of ours.
    ///
    /// 10000 means "we are always first in the queue", which is the assumption
    /// that makes a paper run look better than reality. The default of 5000
    /// says half the displayed size is ahead of us. It is an approximation and
    /// docs/paper-execution.md says so plainly -- we do not model a real
    /// exchange queue and must not claim to.
    std::uint32_t queue_share_bps = 5'000;

    /// A book older than this stops producing fills (§30). Cancellation and
    /// reconciliation continue to work: refusing to cancel because the data is
    /// stale would trap exposure exactly when it most needs releasing.
    Nanos max_book_age_ns = millis(500);

    /// Capacity of the pending-event queue. Exceeding it is an error, never a
    /// silent drop.
    std::size_t max_pending_events = 4'096;
    std::size_t max_orders = 1'024;

    /// Reserved for optional stochastic simulation. Zero -- the default and the
    /// only value used by any test -- means the venue is fully deterministic.
    std::uint64_t deterministic_seed = 0;

    /// Fee rates in basis points of executed notional. Flat: no tiers, no
    /// rebates, no funding -- see docs/paper-execution.md §16.
    std::int32_t maker_fee_bps = 10;
    std::int32_t taker_fee_bps = 40;
};

/// §33. Counters an operator would watch. No dashboard is built here.
struct PaperMetrics {
    std::uint64_t new_requests = 0;
    std::uint64_t new_acks = 0;
    std::uint64_t new_rejects = 0;
    std::uint64_t cancel_requests = 0;
    std::uint64_t cancel_acks = 0;
    std::uint64_t cancel_rejects = 0;
    std::uint64_t replace_requests = 0;
    std::uint64_t replace_acks = 0;
    std::uint64_t replace_rejects = 0;
    std::uint64_t fills = 0;
    std::uint64_t partial_fills = 0;
    std::uint64_t expiries = 0;
    std::uint64_t execution_errors = 0;
    std::uint64_t connection_transitions = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t events_emitted = 0;
    std::uint64_t events_dropped = 0;
    std::uint64_t requests_dropped = 0;
    std::uint64_t duplicates_injected = 0;
    std::uint64_t reorders_injected = 0;
    std::uint64_t stale_book_skips = 0;
    std::uint64_t post_only_rejections = 0;
    /// Total simulated latency applied, so a test can assert the model ran.
    Nanos simulated_latency_ns = 0;
};

}  // namespace mm::exchange::paper
