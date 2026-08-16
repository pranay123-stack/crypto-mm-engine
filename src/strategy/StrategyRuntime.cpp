#include "mm/strategy/StrategyRuntime.hpp"

#include <limits>

namespace mm::strategy {
namespace {

/// Distance from a reference price, in basis points. Reporting and bounds only;
/// order sizing never leaves fixed point.
[[nodiscard]] double distance_bps(Px price, Px reference) noexcept {
    if (!reference.is_positive()) {
        return 0.0;
    }
    const double diff = static_cast<double>(price.raw() - reference.raw());
    return 10'000.0 * diff / static_cast<double>(reference.raw());
}

template <class Enum, std::size_t N>
void tally(std::array<std::uint64_t, N>& counters, Enum value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    if (index < N) {
        ++counters[index];
    }
}

/// Checks one side of a quote against the venue's rules.
[[nodiscard]] IntentRejection validate_side(Px price, Qty quantity, const InstrumentSpec& spec,
                                            Px touch, const IntentLimits& limits) noexcept {
    if (!price.is_positive()) {
        return IntentRejection::NonPositivePrice;
    }
    if (quantity.is_negative()) {
        return IntentRejection::NegativeQuantity;
    }
    if (!quantity.is_positive()) {
        // A quoting intent with zero size is not "no quote" -- that is what
        // Pull is for. Accepting it would send an order the venue rejects.
        return IntentRejection::NonPositiveQuantity;
    }
    if (!is_on_step(price, spec.tick_size)) {
        return IntentRejection::TickMisaligned;
    }
    if (!is_on_step(quantity, spec.lot_size)) {
        return IntentRejection::LotMisaligned;
    }
    if (spec.max_qty.is_positive() && quantity > spec.max_qty) {
        return IntentRejection::QuantityAboveVenueMax;
    }
    if (spec.min_notional.is_positive() && notional_of(price, quantity) < spec.min_notional) {
        return IntentRejection::NotionalTooSmall;
    }
    if (limits.max_distance_from_touch_bps > 0 && touch.is_positive()) {
        const double away = distance_bps(price, touch);
        const double magnitude = away < 0.0 ? -away : away;
        if (magnitude > static_cast<double>(limits.max_distance_from_touch_bps)) {
            // A quote far from the touch is almost always arithmetic gone
            // wrong, and sending it buys a venue rejection at the cost of a
            // rate-limit slot.
            return IntentRejection::QuoteTooFarFromTouch;
        }
    }
    return IntentRejection::None;
}

}  // namespace

std::string_view to_string(SkipReason r) noexcept {
    switch (r) {
        case SkipReason::None:                return "NONE";
        case SkipReason::NotInitialized:      return "NOT_INITIALIZED";
        case SkipReason::Paused:              return "PAUSED";
        case SkipReason::Stopped:             return "STOPPED";
        case SkipReason::Faulted:             return "FAULTED";
        case SkipReason::BookNotQuotable:     return "BOOK_NOT_QUOTABLE";
        case SkipReason::MarketDataStale:     return "MARKET_DATA_STALE";
        case SkipReason::NoTwoSidedMarket:    return "NO_TWO_SIDED_MARKET";
        case SkipReason::InstrumentNotLoaded: return "INSTRUMENT_NOT_LOADED";
        case SkipReason::TriggerNotWanted:    return "TRIGGER_NOT_WANTED";
        case SkipReason::QuotingDisabled:     return "QUOTING_DISABLED";
    }
    return "UNKNOWN";
}

// --------------------------------------------------------------- validation

IntentRejection validate_intent(const QuoteIntent& intent, const StrategyContext& context,
                                const StrategyIdentity& expected,
                                const IntentLimits& limits) noexcept {
    // Pull and NoChange carry no prices, so nothing below applies to them.
    if (intent.action == QuoteAction::Pull || intent.action == QuoteAction::NoChange) {
        return IntentRejection::None;
    }
    if (intent.action != QuoteAction::Quote) {
        return IntentRejection::UnknownAction;
    }

    // ---- provenance ----
    if (!intent.identity.is_valid()) {
        return IntentRejection::IdentityMissing;
    }
    if (intent.identity.name != expected.name || intent.identity.version != expected.version) {
        // A strategy stamping another's identity would misattribute every fill
        // it produced, which is exactly what §17 exists to prevent.
        return IntentRejection::IdentityMismatch;
    }
    if (limits.require_matching_sequence && intent.market_sequence != context.sequence) {
        // Catches a strategy that cached a decision and replayed it against a
        // book that has since moved.
        return IntentRejection::StaleSequence;
    }

    if (context.instrument == nullptr || !context.instrument->is_valid()) {
        return IntentRejection::InstrumentNotLoaded;
    }
    const InstrumentSpec& spec = *context.instrument;

    if (!intent.quote_bid && !intent.quote_ask) {
        // Quote-with-nothing-quoted is contradictory; Pull is how you say that.
        return IntentRejection::UnknownAction;
    }

    if (intent.quote_bid) {
        const IntentRejection r = validate_side(intent.bid_price, intent.bid_quantity, spec,
                                                context.bbo.bid_px, limits);
        if (r != IntentRejection::None) {
            return r;
        }
    }
    if (intent.quote_ask) {
        const IntentRejection r = validate_side(intent.ask_price, intent.ask_quantity, spec,
                                                context.bbo.ask_px, limits);
        if (r != IntentRejection::None) {
            return r;
        }
    }
    if (intent.quote_bid && intent.quote_ask && intent.bid_price >= intent.ask_price) {
        // Self-crossing: the strategy would be trading with itself.
        return IntentRejection::CrossedQuote;
    }
    return IntentRejection::None;
}

// ------------------------------------------------------------------ runtime

StrategyRuntime::StrategyRuntime(StrategyRuntimeConfig config, StrategyPtr strategy,
                                 const Clock& clock)
    : config_(config), strategy_(std::move(strategy)), clock_(clock) {}

void StrategyRuntime::transition(StrategyState to) {
    if (is_legal_transition(state_, to)) {
        state_ = to;
    }
}

void StrategyRuntime::fault(std::string_view reason) {
    state_ = StrategyState::Faulted;
    static_cast<void>(fault_reason_.assign(reason));
}

void StrategyRuntime::record_skip(SkipReason reason) {
    ++metrics_.skipped;
    tally(metrics_.skips_by_reason, reason);
}

void StrategyRuntime::record_rejection(IntentRejection cause) {
    ++metrics_.intents_rejected;
    tally(metrics_.rejections_by_cause, cause);
}

EvaluationResult StrategyRuntime::skipped(SkipReason reason) {
    record_skip(reason);
    EvaluationResult result;
    result.skip = reason;
    // Always a Pull, never a stale copy of a previous quote: if the runtime
    // cannot evaluate, the safe expression of that is "stand down".
    result.intent = QuoteIntent::pull(IntentReason::Normal);
    result.intent.identity = identity_;
    return result;
}

Status StrategyRuntime::initialize(const StrategyInit& init) {
    if (!strategy_) {
        return {ErrorCode::FailedPrecondition, "no strategy instance"};
    }
    if (!init.identity.is_valid()) {
        return {ErrorCode::InvalidArgument, "strategy identity is incomplete"};
    }

    const StrategyIdentity self = strategy_->identity();
    if (self.name != init.identity.name || self.version != init.identity.version) {
        // Refused rather than tolerated: a strategy running under another's name
        // makes every downstream attribution wrong.
        std::string msg = "strategy identity mismatch: binary reports '";
        msg.append(self.name.view())
            .append(" v")
            .append(std::to_string(self.version))
            .append("', configuration selected '")
            .append(init.identity.name.view())
            .append(" v")
            .append(std::to_string(init.identity.version))
            .append("'");
        return {ErrorCode::FailedPrecondition, msg};
    }
    if (!init.instrument.is_valid()) {
        // Without a tick and lot size nothing can be validated, and a strategy
        // must never guess them.
        return {ErrorCode::FailedPrecondition,
                "instrument specification is not loaded; cannot initialize a strategy"};
    }

    Status status = Status::ok();
    try {
        status = strategy_->initialize(init);
    } catch (const std::exception& e) {
        fault(std::string("initialize threw: ") + e.what());
        ++metrics_.exceptions;
        return {ErrorCode::Internal, fault_reason_.view()};
    } catch (...) {
        fault("initialize threw a non-standard exception");
        ++metrics_.exceptions;
        return {ErrorCode::Internal, fault_reason_.view()};
    }
    if (status.is_error()) {
        return status;
    }

    identity_ = init.identity;
    instrument_ = init.instrument;
    instrument_loaded_ = true;
    consecutive_budget_violations_ = 0;
    consecutive_invalid_outputs_ = 0;
    fault_reason_.clear();
    state_ = StrategyState::Initialized;
    return Status::ok();
}

void StrategyRuntime::start() { transition(StrategyState::Running); }
void StrategyRuntime::pause() { transition(StrategyState::Paused); }
void StrategyRuntime::resume() { transition(StrategyState::Running); }
void StrategyRuntime::stop() { transition(StrategyState::Stopped); }

void StrategyRuntime::reset_strategy() {
    if (!strategy_) {
        return;
    }
    try {
        strategy_->reset();
    } catch (...) {
        ++metrics_.exceptions;
        fault("reset threw");
    }
    consecutive_invalid_outputs_ = 0;
}

Status StrategyRuntime::clear_fault() {
    if (state_ != StrategyState::Faulted) {
        return {ErrorCode::FailedPrecondition, "strategy is not faulted"};
    }
    // Clear the fault BEFORE resetting, so that a fault raised by reset() is
    // distinguishable from the one being cleared. Checking `state_ == Faulted`
    // afterwards without doing this always tripped, and a faulted strategy
    // could never be recovered at all.
    state_ = StrategyState::Initialized;
    fault_reason_.clear();
    reset_strategy();
    if (state_ == StrategyState::Faulted) {
        // reset() threw; the strategy is not recoverable in place.
        return {ErrorCode::Internal, "strategy faulted again while clearing the fault"};
    }
    consecutive_budget_violations_ = 0;
    consecutive_invalid_outputs_ = 0;
    return Status::ok();
}

SkipReason StrategyRuntime::gate(const StrategyContext& context) const {
    if (!config_.enabled) {
        return SkipReason::QuotingDisabled;
    }
    switch (state_) {
        case StrategyState::Created:     return SkipReason::NotInitialized;
        case StrategyState::Initialized: return SkipReason::NotInitialized;
        case StrategyState::Paused:      return SkipReason::Paused;
        case StrategyState::Stopped:     return SkipReason::Stopped;
        case StrategyState::Faulted:     return SkipReason::Faulted;
        case StrategyState::Running:     break;
    }
    if (!mode_accepts(config_.evaluation_mode, context.trigger)) {
        return SkipReason::TriggerNotWanted;
    }
    // The session machine owns this judgement; the runtime asks it rather than
    // forming a second opinion.
    if (!context.is_book_quotable()) {
        return SkipReason::BookNotQuotable;
    }
    if (context.data_age_ns > config_.max_data_age_ns) {
        return SkipReason::MarketDataStale;
    }
    if (context.instrument == nullptr || !context.instrument->is_valid()) {
        return SkipReason::InstrumentNotLoaded;
    }
    if (!context.has_two_sided_market()) {
        // A one-sided or crossed book gives a maker nothing to quote around.
        return SkipReason::NoTwoSidedMarket;
    }
    return SkipReason::None;
}

EvaluationResult StrategyRuntime::evaluate(const StrategyContext& context) {
    const SkipReason skip = gate(context);
    if (skip != SkipReason::None) {
        return skipped(skip);
    }

    EvaluationResult result;
    QuoteIntent intent;
    const Nanos started = clock_.steady();

    // A strategy is a plug-in and is assumed to be able to throw. An exception
    // escaping here would take down a process holding live orders.
    try {
        intent = (context.trigger == TriggerReason::Timer) ? strategy_->on_timer(context)
                                                           : strategy_->on_market_update(context);
    } catch (const std::exception& e) {
        ++metrics_.exceptions;
        fault(std::string("evaluation threw: ") + e.what());
        return skipped(SkipReason::Faulted);
    } catch (...) {
        ++metrics_.exceptions;
        fault("evaluation threw a non-standard exception");
        return skipped(SkipReason::Faulted);
    }

    const Nanos elapsed = clock_.steady() - started;
    result.evaluation_ns = elapsed;
    latency_.record(elapsed);
    ++metrics_.evaluations;
    if (elapsed > metrics_.max_evaluation_ns) {
        metrics_.max_evaluation_ns = elapsed;
    }

    if (elapsed > config_.max_evaluation_latency_ns) {
        ++metrics_.budget_violations;
        ++consecutive_budget_violations_;
        if (consecutive_budget_violations_ >= config_.max_consecutive_budget_violations) {
            // Persistently slow is a different failure from a single hiccup,
            // and it is one the trading thread cannot absorb.
            fault("evaluation exceeded its latency budget repeatedly");
            return skipped(SkipReason::Faulted);
        }
    } else {
        consecutive_budget_violations_ = 0;
    }

    // The runtime stamps provenance rather than trusting the strategy to: a
    // strategy that got its own version wrong would corrupt attribution.
    intent.identity = identity_;
    intent.identity.config_generation = context.config_generation;
    intent.computed_ns = clock_.steady();
    if (intent.market_sequence == kNoSeq) {
        intent.market_sequence = context.sequence;
    }

    const IntentRejection rejection =
        validate_intent(intent, context, identity_, config_.limits);
    if (rejection != IntentRejection::None) {
        record_rejection(rejection);
        ++consecutive_invalid_outputs_;
        if (consecutive_invalid_outputs_ >= config_.max_consecutive_invalid_outputs) {
            // One bad quote is an edge case; a stream of them means the
            // strategy cannot be trusted to produce a quote at all.
            fault("strategy produced invalid output repeatedly");
        }
        result.rejection = rejection;
        result.accepted = false;
        // Never fall back to the previous quote: a rejected intent means we do
        // not know what the strategy wants, and standing on an old quote is a
        // position taken by nobody.
        result.intent = QuoteIntent::pull(IntentReason::Normal);
        result.intent.identity = identity_;
        return result;
    }
    consecutive_invalid_outputs_ = 0;

    // The operator switch is applied after validation so metrics still reflect
    // what the strategy would have done.
    if (!config_.quoting_enabled || !context.quoting_enabled) {
        if (intent.action == QuoteAction::Quote) {
            intent = QuoteIntent::pull(IntentReason::StrategyDisabled);
            intent.identity = identity_;
            intent.market_sequence = context.sequence;
        }
    }

    result.accepted = true;
    result.intent = intent;
    ++metrics_.intents_accepted;
    if (intent.wants_quotes()) {
        ++metrics_.quote_intents;
    } else {
        ++metrics_.pull_intents;
    }
    return result;
}

void StrategyRuntime::on_fill(const StrategyFill& fill) {
    if (!strategy_ || state_ == StrategyState::Faulted || state_ == StrategyState::Created) {
        return;
    }
    try {
        strategy_->on_fill(fill);
    } catch (...) {
        ++metrics_.exceptions;
        fault("on_fill threw");
    }
}

void StrategyRuntime::on_order_event(const StrategyOrderEvent& event) {
    if (!strategy_ || state_ == StrategyState::Faulted || state_ == StrategyState::Created) {
        return;
    }
    try {
        strategy_->on_order_event(event);
    } catch (...) {
        ++metrics_.exceptions;
        fault("on_order_event threw");
    }
}

}  // namespace mm::strategy
