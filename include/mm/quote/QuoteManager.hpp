#pragma once

/// \file QuoteManager.hpp
/// Translates strategy intent into a desired working-order state.
///
/// **The quote manager does not decide whether an order is financially safe.
/// Risk owns that decision.** It answers exactly one question: *what orders
/// should currently exist to represent this intent?* Whether they are permitted
/// is Risk's; how they become orders is the OMS's; how they reach a venue is the
/// adapter's.
///
/// ## Concurrency
///
/// Owned entirely by the trading thread, like every other component that
/// mutates trading state (docs/concurrency.md §1). It holds no lock, spawns no
/// thread and performs no I/O. Working-order state arrives as an input rather
/// than being tracked here, so there is no second opinion about what is resting.
///
/// ## What it is not
///
/// No exchange code, no risk logic, no order tracking, no profitability
/// reasoning. The churn controls below are infrastructure limits on *how often
/// an order may be rewritten*; they never alter the price or size the strategy
/// asked for, and must not be used to encode strategy behaviour.

#include <cstdint>

#include "mm/common/LatencyHistogram.hpp"
#include "mm/common/Time.hpp"
#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/quote/OrderAction.hpp"
#include "mm/quote/WorkingQuoteState.hpp"
#include "mm/strategy/QuoteIntent.hpp"

namespace mm::quote {

using exchange::ExchangeCapabilities;
using strategy::QuoteIntent;

/// Infrastructure controls. Deliberately separate from `strategies.<name>`:
/// these bound how the infrastructure behaves, not what the strategy wants.
struct QuoteManagerConfig {
    /// An intent older than this is not acted on. Intent describes a market
    /// that existed at a moment; acting on a stale one quotes a market that has
    /// moved on.
    Nanos max_intent_age_ns = millis(250);
    /// Market data older than this withdraws quotes regardless of intent.
    Nanos max_market_data_age_ns = millis(500);

    // ---- churn controls ----
    //
    // These suppress *rewrites of an existing order* when the difference does
    // not justify a round trip. They never change the requested price or size:
    // if the strategy wants a materially different quote it gets one, and if it
    // does not, the resting order already expresses its intent. Encoding
    // strategy behaviour here would make the strategy's stated quotes a fiction.

    /// Smallest price difference, in ticks, that justifies a replacement.
    /// One means any tick of movement is material.
    std::int32_t min_price_move_ticks = 1;
    /// Smallest quantity difference, as a fraction of the desired quantity,
    /// that justifies a replacement. Zero means any difference is material.
    double min_quantity_move_fraction = 0.0;
    /// Minimum interval between replacements of the same slot.
    Nanos min_replace_interval_ns = 0;
    /// Quiet period after a cancel before the same slot may be re-quoted.
    Nanos cooldown_after_cancel_ns = 0;

    /// How long the manager waits for the working state to reflect a request it
    /// already issued, before assuming that request never reached anyone.
    ///
    /// Without this the manager would re-issue a New on every evaluation until
    /// the OMS reported back, creating one order per cycle. Keying the guard on
    /// the intent generation alone is not enough: generations advance on every
    /// evaluation, so a working state lagging by a single cycle would defeat
    /// it. Set to zero to disable the guard entirely.
    Nanos assume_request_lost_after_ns = seconds(1);

    /// Replace a partially filled order to restore the intended resting size.
    /// When false a partial fill is left alone until the price moves, and the
    /// book carries less size than the strategy asked for.
    bool replenish_partial_fills = true;
};

/// Counters for the future dashboard (§30). No dashboard is built here.
struct QuoteManagerMetrics {
    std::uint64_t intents_received = 0;
    std::uint64_t intents_rejected = 0;
    std::uint64_t stale_intents = 0;
    std::uint64_t duplicate_intents = 0;
    std::uint64_t generation_regressions = 0;

    std::uint64_t new_actions = 0;
    std::uint64_t cancel_actions = 0;
    std::uint64_t replace_actions = 0;
    std::uint64_t noop_evaluations = 0;
    std::uint64_t keeps = 0;

    std::uint64_t ownership_violations = 0;
    std::uint64_t unknown_order_states = 0;
    std::uint64_t churn_suppressions = 0;
    std::uint64_t invalid_quotes_blocked = 0;
    std::uint64_t pending_waits = 0;
    std::uint64_t foreign_orders_seen = 0;

    /// Replacements plus cancels, i.e. how hard this manager is working the
    /// venue. The single number an operator watches for pathological churn.
    [[nodiscard]] std::uint64_t churn() const noexcept {
        return replace_actions + cancel_actions;
    }
};

/// Everything the manager needs about the world, besides the intent itself.
struct QuoteManagerInput {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// Venue trading rules. Null means orders cannot be validated, and nothing
    /// may be generated.
    const InstrumentSpec* instrument = nullptr;
    /// What the venue can do. Decides whether a replacement is atomic or must
    /// be decomposed into cancel-then-new.
    const ExchangeCapabilities* capabilities = nullptr;

    /// As reported by the OMS.
    WorkingQuoteState working{};

    /// Steady-clock age of the most recent market data.
    Nanos market_data_age_ns = 0;
    /// False when the engine is not in a state where quoting is permitted.
    /// The manager withdraws rather than simply idling, so resting orders do
    /// not survive a condition that should have removed them.
    bool system_ready = true;
};

class QuoteManager {
public:
    QuoteManager(StrategyName owner, QuoteManagerConfig config, const Clock& clock);

    /// The single entry point. Pure with respect to its inputs apart from the
    /// generation and churn bookkeeping it deliberately carries.
    [[nodiscard]] QuoteManagerResult evaluate(const QuoteIntent& intent,
                                              const QuoteManagerInput& input);

    /// Produces cancels for every managed quote, without needing an intent.
    ///
    /// This is the mechanism behind an operator kill, a strategy fault, a
    /// safety halt and (later) a risk kill. It generates cancel *actions*; it
    /// does not call an exchange, and it does not reach outside the slots this
    /// manager owns.
    [[nodiscard]] QuoteManagerResult disable_all_quotes(const QuoteManagerInput& input,
                                                        ActionReason reason);

    /// Clears generation and churn bookkeeping. Used when the strategy is
    /// reset or resynchronized: what it decided about the old book is no longer
    /// about the book that now exists.
    void reset();

    [[nodiscard]] const StrategyName& owner() const noexcept { return owner_; }
    [[nodiscard]] std::uint64_t last_generation() const noexcept { return last_generation_; }
    [[nodiscard]] const QuoteManagerMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const QuoteManagerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const LatencyHistogram& latency() const noexcept { return latency_; }

private:
    /// Per-slot memory of what this manager has already asked for. Prevents a
    /// duplicate request when the same intent is evaluated twice before the
    /// working state has caught up with the first one.
    struct SlotMemory {
        OrderActionType last_action = OrderActionType::Noop;
        std::uint64_t last_action_generation = 0;
        Nanos last_action_ns = 0;
        Nanos last_cancel_ns = 0;
    };

    struct DesiredQuote {
        bool wanted = false;
        Px price{};
        Qty quantity{};
    };

    [[nodiscard]] QuoteRejection screen(const QuoteIntent& intent,
                                        const QuoteManagerInput& input) const;

    [[nodiscard]] SlotDecision resolve_slot(QuoteSlot slot, const DesiredQuote& desired,
                                            const QuoteIntent& intent,
                                            const QuoteManagerInput& input,
                                            QuoteManagerResult& result);

    /// True when the resting order already represents the desired quote, under
    /// the configured churn thresholds.
    [[nodiscard]] bool represents(const WorkingOrder& order, const DesiredQuote& desired,
                                  const InstrumentSpec& spec) const;

    /// Phase 3 validation, reused rather than reimplemented.
    [[nodiscard]] RejectReason validate(QuoteSlot slot, const DesiredQuote& desired,
                                        const QuoteManagerInput& input) const;

    void push(QuoteManagerResult& result, const OrderAction& action);
    [[nodiscard]] OrderAction make_action(OrderActionType type, QuoteSlot slot,
                                          ActionReason reason, const QuoteIntent& intent,
                                          const QuoteManagerInput& input) const;

    StrategyName owner_{};
    QuoteManagerConfig config_{};
    const Clock& clock_;

    std::uint64_t last_generation_ = 0;
    std::array<SlotMemory, kSlotCount> memory_{};

    QuoteManagerMetrics metrics_{};
    LatencyHistogram latency_{"quote_manager"};
};

}  // namespace mm::quote
