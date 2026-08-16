#pragma once

/// \file StrategyTypes.hpp
/// The vocabulary of the strategy boundary.
///
/// This runtime **executes a finalized strategy**. It does not perform
/// research, does not search parameters, and establishes nothing about
/// profitability. Those belong to the separate quant research platform, which
/// hands this one a strategy module and a validated parameter block.

#include <cstdint>
#include <string_view>

#include "mm/common/Types.hpp"

namespace mm::strategy {

// ---------------------------------------------------------------- lifecycle

/// The strategy's own lifecycle. It is **not** a copy of the engine-wide
/// `TradingState` or of the per-symbol `SessionState`; it describes only
/// whether this strategy instance is willing and able to produce intent.
///
/// The two interact in one direction: the runtime refuses to evaluate unless
/// the session says the book is quotable, so a strategy can be `Running` while
/// the market data is stale — and it simply is not asked. Duplicating the
/// session machine here would create a second opinion about whether the book is
/// safe, and two opinions is one too many.
enum class StrategyState : std::uint8_t {
    Created = 0,   ///< constructed, parameters not yet accepted
    Initialized,   ///< parameters validated; not yet producing intent
    Running,       ///< may produce intent when the runtime asks
    Paused,        ///< operator-suspended; state retained, no intent produced
    Stopped,       ///< shut down cleanly; terminal without a fresh initialize
    Faulted,       ///< threw, overran its budget, or produced invalid output
};
[[nodiscard]] std::string_view to_string(StrategyState s) noexcept;

/// True when the runtime may call the strategy at all.
[[nodiscard]] constexpr bool is_evaluable(StrategyState s) noexcept {
    return s == StrategyState::Running;
}

/// `Faulted` and `Stopped` never resume on their own. A strategy that faulted
/// once will fault again on the same input, and a runtime that retried it
/// automatically would quote intermittently from a component it has already
/// judged broken.
[[nodiscard]] constexpr bool needs_operator_action(StrategyState s) noexcept {
    return s == StrategyState::Faulted;
}

[[nodiscard]] bool is_legal_transition(StrategyState from, StrategyState to) noexcept;

// ------------------------------------------------------------------ triggers

/// When the runtime asks the strategy for intent.
///
/// Evaluating on every market-data event is both wasteful and wrong: a single
/// venue message can arrive as several chunks, so "every event" would evaluate
/// a partially applied book. Every mode below fires only on a settled book.
enum class EvaluationMode : std::uint8_t {
    /// Every completed book update. Highest fidelity, highest cost.
    OnBookUpdate = 0,
    /// Only when the top of book actually moved. For a maker quoting at the
    /// touch this is the signal; deeper levels changing usually is not.
    OnBboChange,
    /// Only on the timer. For strategies whose quotes are time-driven.
    OnTimer,
    /// The default: react to the touch, and re-evaluate periodically so a quiet
    /// market still refreshes quotes and inventory skew still decays.
    OnBboChangeAndTimer,
};
[[nodiscard]] std::string_view to_string(EvaluationMode m) noexcept;
[[nodiscard]] bool parse_evaluation_mode(std::string_view text, EvaluationMode& out) noexcept;

/// Why this particular evaluation is happening. Passed to the strategy so it
/// can distinguish a market move from a periodic refresh without inferring it.
enum class TriggerReason : std::uint8_t {
    None = 0,
    BookUpdate,
    BboChange,
    Trade,
    Timer,
    InventoryChange,
    OrderStateChange,
};
[[nodiscard]] std::string_view to_string(TriggerReason r) noexcept;

/// True when `mode` wants an evaluation for `reason`.
[[nodiscard]] constexpr bool mode_accepts(EvaluationMode mode, TriggerReason reason) noexcept {
    switch (mode) {
        case EvaluationMode::OnBookUpdate:
            return reason == TriggerReason::BookUpdate || reason == TriggerReason::BboChange;
        case EvaluationMode::OnBboChange:
            return reason == TriggerReason::BboChange;
        case EvaluationMode::OnTimer:
            return reason == TriggerReason::Timer;
        case EvaluationMode::OnBboChangeAndTimer:
            return reason == TriggerReason::BboChange || reason == TriggerReason::Timer;
    }
    return false;
}

// ------------------------------------------------------------------ identity

/// Stamped onto every `QuoteIntent` so a fill months later can be attributed to
/// the exact strategy build and parameter set that produced it. This is for
/// order attribution, PnL analysis and incident investigation — it is not an
/// experiment-tracking system.
struct StrategyIdentity {
    StrategyName name{};
    std::int32_t version = 0;
    /// Bumped whenever the parameter set changes. Two intents with the same
    /// name and version but different generations came from different
    /// parameters, which is exactly the case that is otherwise invisible.
    std::uint64_t config_generation = 0;

    [[nodiscard]] bool is_valid() const noexcept { return !name.empty() && version > 0; }
};

static_assert(std::is_trivially_copyable_v<StrategyIdentity>);

}  // namespace mm::strategy
