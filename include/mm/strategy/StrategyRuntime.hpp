#pragma once

/// \file StrategyRuntime.hpp
/// The container a finalized strategy runs inside.
///
/// The runtime owns everything the strategy is not trusted with: deciding
/// whether it may be called at all, timing it, catching it when it throws,
/// checking what it returns, and refusing anything that does not survive that
/// check. A strategy is a plug-in, and a plug-in is assumed to be wrong.
///
/// **This runtime executes a finalized strategy. It performs no research and
/// establishes nothing about profitability.**

#include <cstdint>
#include <memory>
#include <string_view>

#include "mm/common/LatencyHistogram.hpp"
#include "mm/common/Time.hpp"
#include "mm/strategy/IStrategy.hpp"
#include "mm/strategy/QuoteIntent.hpp"
#include "mm/strategy/StrategyContext.hpp"

namespace mm::strategy {

/// Why an evaluation did not happen, or happened but produced nothing usable.
enum class SkipReason : std::uint8_t {
    None = 0,
    NotInitialized,
    Paused,
    Stopped,
    Faulted,
    BookNotQuotable,     ///< session is not Ready
    MarketDataStale,
    NoTwoSidedMarket,    ///< one side missing, or crossed/locked
    InstrumentNotLoaded, ///< no tick/lot size yet; a strategy must not guess
    TriggerNotWanted,    ///< this trigger is not in the evaluation mode
    QuotingDisabled,     ///< operator switch
};
[[nodiscard]] std::string_view to_string(SkipReason r) noexcept;

/// Runtime parameters, independent of any particular strategy.
struct StrategyRuntimeConfig {
    bool enabled = true;
    /// Operator switch. When false the strategy is still evaluated, so its
    /// internal state stays warm, but no quoting intent is accepted.
    bool quoting_enabled = true;
    EvaluationMode evaluation_mode = EvaluationMode::OnBboChangeAndTimer;

    /// One evaluation's budget. Exceeding it is counted; exceeding it
    /// repeatedly faults the strategy.
    Nanos max_evaluation_latency_ns = micros(50);
    std::int32_t max_consecutive_budget_violations = 5;
    /// Invalid outputs tolerated before the strategy is declared broken. One is
    /// a bug in an edge case; a stream of them means it cannot be trusted to
    /// produce a quote at all.
    std::int32_t max_consecutive_invalid_outputs = 3;

    /// Data older than this is not offered to the strategy at all.
    Nanos max_data_age_ns = millis(500);

    IntentLimits limits{};
};

/// What the runtime does after an evaluation. Nothing is exposed that would let
/// a caller act on an unaccepted intent by mistake.
struct EvaluationResult {
    /// True only when the strategy ran, returned something, and that something
    /// passed validation.
    bool accepted = false;
    /// Safe to act on. When `accepted` is false this is always a `Pull`, never
    /// a stale copy of an earlier quote.
    QuoteIntent intent{};
    SkipReason skip = SkipReason::None;
    IntentRejection rejection = IntentRejection::None;
    /// Measured duration of the strategy call itself, zero when it was skipped.
    Nanos evaluation_ns = 0;
};

/// Watchdog counters (§12), for the future dashboard. No dashboard is built here.
struct StrategyMetrics {
    std::uint64_t evaluations = 0;
    std::uint64_t skipped = 0;
    std::uint64_t intents_accepted = 0;
    std::uint64_t intents_rejected = 0;
    std::uint64_t quote_intents = 0;   ///< accepted intents actually asking to quote
    std::uint64_t pull_intents = 0;
    std::uint64_t exceptions = 0;
    std::uint64_t budget_violations = 0;
    Nanos max_evaluation_ns = 0;
    /// Per-cause skip and rejection tallies, indexed by enum value, so a
    /// strategy that never quotes can be diagnosed from counters alone.
    std::array<std::uint64_t, 16> skips_by_reason{};
    std::array<std::uint64_t, 16> rejections_by_cause{};
};

class StrategyRuntime {
public:
    StrategyRuntime(StrategyRuntimeConfig config, StrategyPtr strategy, const Clock& clock);

    /// Validates identity against configuration and initializes the strategy.
    /// A strategy whose self-reported identity disagrees with the config is a
    /// wiring error, and it is refused here rather than allowed to run under
    /// the wrong name in the journal.
    [[nodiscard]] Status initialize(const StrategyInit& init);

    void start();
    void pause();
    void resume();
    void stop();

    /// Clears strategy state without destroying it. Called when market data
    /// resynchronizes: whatever the strategy inferred from the old book is no
    /// longer about the book that now exists.
    void reset_strategy();

    /// Clears a fault. Deliberately explicit: a faulted strategy will fault
    /// again on the same input, so automatic recovery would produce a component
    /// that quotes intermittently after being judged broken.
    [[nodiscard]] Status clear_fault();

    /// The single entry point. Applies every gate, invokes the strategy under a
    /// timer and an exception guard, then validates what comes back.
    [[nodiscard]] EvaluationResult evaluate(const StrategyContext& context);

    /// Forwarded to the strategy. Never allowed to fault the process: a
    /// throwing notification faults the strategy exactly as an evaluation does.
    void on_fill(const StrategyFill& fill);
    void on_order_event(const StrategyOrderEvent& event);

    [[nodiscard]] StrategyState state() const noexcept { return state_; }
    [[nodiscard]] const StrategyIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const StrategyMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const LatencyHistogram& latency() const noexcept { return latency_; }
    [[nodiscard]] LatencySummary latency_summary() const { return LatencySummary::from(latency_); }
    [[nodiscard]] const StrategyRuntimeConfig& config() const noexcept { return config_; }

    /// Last fault message, for the operator. Empty when not faulted.
    [[nodiscard]] std::string_view fault_reason() const noexcept { return fault_reason_.view(); }

    /// The generation stamped on the most recent intent. Monotonic across the
    /// runtime's lifetime, including across faults and pauses: reusing a
    /// generation would let a stale intent masquerade as a current one.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Applies the gates without calling the strategy. Exposed so the engine
    /// can decide whether an evaluation is worth assembling a context for.
    [[nodiscard]] SkipReason gate(const StrategyContext& context) const;

private:
    void transition(StrategyState to);
    void fault(std::string_view reason);
    void record_skip(SkipReason reason);
    void record_rejection(IntentRejection cause);
    [[nodiscard]] EvaluationResult skipped(SkipReason reason);

    StrategyRuntimeConfig config_;
    StrategyPtr strategy_;
    const Clock& clock_;

    StrategyState state_ = StrategyState::Created;
    StrategyIdentity identity_{};
    InstrumentSpec instrument_{};
    bool instrument_loaded_ = false;

    std::uint64_t generation_ = 0;
    std::int32_t consecutive_budget_violations_ = 0;
    std::int32_t consecutive_invalid_outputs_ = 0;
    InlineString<160> fault_reason_{};

    StrategyMetrics metrics_{};
    LatencyHistogram latency_{"strategy_evaluation"};
};

/// Validates an intent against the context that produced it and the venue's
/// rules. Free-standing so it can be tested, benchmarked and reused without a
/// runtime.
///
/// Returns `IntentRejection::None` when the intent may proceed to risk.
[[nodiscard]] IntentRejection validate_intent(const QuoteIntent& intent,
                                              const StrategyContext& context,
                                              const StrategyIdentity& expected,
                                              const IntentLimits& limits) noexcept;

}  // namespace mm::strategy
