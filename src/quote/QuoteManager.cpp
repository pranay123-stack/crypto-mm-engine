#include "mm/quote/QuoteManager.hpp"

#include <cmath>

#include "mm/exchange/common/OrderRequest.hpp"

namespace mm::quote {
namespace {

using strategy::QuoteAction;

/// Absolute price difference in ticks. Fixed point throughout: comparing
/// prices through a double would make a threshold of "one tick" depend on the
/// magnitude of the price.
[[nodiscard]] std::int64_t ticks_between(Px a, Px b, Px tick) noexcept {
    if (!tick.is_positive()) {
        return a == b ? 0 : 1;
    }
    const std::int64_t diff = a.raw() - b.raw();
    const std::int64_t magnitude = diff < 0 ? -diff : diff;
    return magnitude / tick.raw();
}

}  // namespace

QuoteManager::QuoteManager(StrategyName owner, QuoteManagerConfig config, const Clock& clock)
    : owner_(owner), config_(config), clock_(clock) {}

void QuoteManager::reset() {
    last_generation_ = 0;
    memory_ = {};
}

// --------------------------------------------------------------- screening

QuoteRejection QuoteManager::screen(const QuoteIntent& intent,
                                    const QuoteManagerInput& input) const {
    // Generation first. An intent delayed behind a newer one must never
    // resurrect a quote the newer one already superseded, so ordering is
    // decided before anything else is even looked at.
    if (intent.generation < last_generation_) {
        return QuoteRejection::GenerationRegression;
    }
    if (intent.identity.name != owner_) {
        return QuoteRejection::IdentityMismatch;
    }
    if (!input.system_ready) {
        return QuoteRejection::NotRunning;
    }
    if (input.instrument == nullptr || !input.instrument->is_valid()) {
        return QuoteRejection::InstrumentNotLoaded;
    }
    if (input.market_data_age_ns > config_.max_market_data_age_ns) {
        return QuoteRejection::MarketDataStale;
    }
    // Intent describes a market that existed at a moment. Acting on an old one
    // quotes a market that has since moved.
    if (config_.max_intent_age_ns > 0 && intent.computed_ns > 0) {
        const Nanos age = clock_.steady() - intent.computed_ns;
        if (age > config_.max_intent_age_ns) {
            return QuoteRejection::IntentExpired;
        }
    }
    return QuoteRejection::None;
}

// -------------------------------------------------------------- validation

RejectReason QuoteManager::validate(QuoteSlot slot, const DesiredQuote& desired,
                                    const QuoteManagerInput& input) const {
    if (!desired.wanted) {
        return RejectReason::None;
    }
    // Phase 3's rules, reused rather than reimplemented: tick, lot, min/max
    // quantity, min notional, price band, and the venue's own capabilities. A
    // second copy of these would drift from the first.
    exchange::OrderRequest request;
    static_cast<void>(request.client_order_id.assign("quote-manager-probe"));
    request.symbol = input.symbol;
    request.symbol_id = input.symbol_id;
    request.side = side_of(slot);
    request.type = OrderType::Limit;
    request.tif = TimeInForce::GTC;
    request.price = desired.price;
    request.quantity = desired.quantity;
    request.created_ns = clock_.steady();

    static const ExchangeCapabilities kPermissive = [] {
        ExchangeCapabilities c;
        c.supports_client_order_id = true;
        return c;
    }();
    const ExchangeCapabilities& caps =
        (input.capabilities != nullptr) ? *input.capabilities : kPermissive;

    const exchange::ExchangeError error = exchange::validate_order(request, *input.instrument, caps);
    if (!error.is_error()) {
        return RejectReason::None;
    }
    // Surface the specific venue rule, not a generic failure: "off the lot
    // grid" and "below the minimum notional" call for different fixes.
    return error.reason != RejectReason::None ? error.reason : RejectReason::Unknown;
}

// ------------------------------------------------------------- comparison

bool QuoteManager::represents(const WorkingOrder& order, const DesiredQuote& desired,
                              const InstrumentSpec& spec) const {
    if (!order.is_live() || !desired.wanted) {
        return false;
    }
    if (ticks_between(order.price, desired.price, spec.tick_size) >=
        static_cast<std::int64_t>(config_.min_price_move_ticks)) {
        return false;
    }

    // Compare against what is actually RESTING, not what was submitted.
    // Assuming the original quantity survives a partial fill would leave the
    // book quoting less than the strategy asked for with nobody noticing.
    const Qty resting = order.remaining();
    if (!config_.replenish_partial_fills && order.is_partially_filled()) {
        // Configured to leave partials alone; only price matters.
        return true;
    }
    const Qty difference = (resting > desired.quantity) ? (resting - desired.quantity)
                                                        : (desired.quantity - resting);
    if (difference.is_zero()) {
        return true;
    }
    if (config_.min_quantity_move_fraction <= 0.0) {
        return false;
    }
    const double fraction = ratio(difference, desired.quantity);
    return fraction < config_.min_quantity_move_fraction;
}

// ----------------------------------------------------------------- actions

OrderAction QuoteManager::make_action(OrderActionType type, QuoteSlot slot, ActionReason reason,
                                      const QuoteIntent& intent,
                                      const QuoteManagerInput& input) const {
    OrderAction action;
    action.type = type;
    action.slot = slot;
    action.reason = reason;
    action.symbol = input.symbol;
    action.symbol_id = input.symbol_id;
    action.side = side_of(slot);
    action.identity = intent.identity;
    action.generation = intent.generation;
    action.market_sequence = intent.market_sequence;
    action.trace = intent.trace;
    action.issued_ns = clock_.steady();
    return action;
}

void QuoteManager::push(QuoteManagerResult& result, const OrderAction& action) {
    if (result.action_count >= kMaxActionsPerEvaluation) {
        return;  // structurally unreachable: two slots, at most two actions each
    }
    result.actions[result.action_count++] = action;
    switch (action.type) {
        case OrderActionType::New:     ++metrics_.new_actions; break;
        case OrderActionType::Cancel:  ++metrics_.cancel_actions; break;
        case OrderActionType::Replace: ++metrics_.replace_actions; break;
        case OrderActionType::Noop:    break;
    }
}

// ------------------------------------------------------------ slot resolve

SlotDecision QuoteManager::resolve_slot(QuoteSlot slot, const DesiredQuote& desired,
                                        const QuoteIntent& intent,
                                        const QuoteManagerInput& input,
                                        QuoteManagerResult& result) {
    SlotDecision decision;
    const WorkingOrder& order = input.working.slot(slot);
    SlotMemory& memory = memory_[static_cast<std::size_t>(slot)];
    const InstrumentSpec& spec = *input.instrument;
    const Nanos now = clock_.steady();

    // ---- ownership ------------------------------------------------------
    //
    // An order in our slot that is not ours means something upstream is
    // confused. Acting on it either way -- cancelling somebody else's order or
    // quoting alongside it -- is worse than standing still, so the slot freezes
    // and the inconsistency is counted where an operator will see it.
    if (order.present && !order.owner.matches(owner_, slot)) {
        ++metrics_.ownership_violations;
        decision.outcome = SlotOutcome::FrozenUnknown;
        decision.reason = ActionReason::None;
        return decision;
    }

    // ---- unknown state --------------------------------------------------
    //
    // We do not know whether this order exists. It must not be assumed filled,
    // cancelled or rejected, and no competing order may be created against one
    // that might still be working.
    if (order.is_uncertain()) {
        ++metrics_.unknown_order_states;
        decision.outcome = SlotOutcome::FrozenUnknown;
        // The documented safe exception: when no quote is wanted, one cancel is
        // permitted. Cancelling an order that may not exist is harmless -- the
        // venue answers "unknown order" -- whereas creating one alongside it is
        // not. The pending guard keeps it to one attempt rather than every cycle.
        if (!desired.wanted && !order.has_pending_request()) {
            OrderAction action = make_action(OrderActionType::Cancel, slot,
                                             ActionReason::QuoteDisabled, intent, input);
            action.target_client_order_id = order.client_order_id;
            action.target_exchange_order_id = order.exchange_order_id;
            push(result, action);
            memory.last_action = OrderActionType::Cancel;
            memory.last_action_generation = intent.generation;
            memory.last_action_ns = now;
            memory.last_cancel_ns = now;
            decision.outcome = SlotOutcome::Cancel;
            decision.reason = ActionReason::QuoteDisabled;
        }
        return decision;
    }

    // ---- pending request ------------------------------------------------
    //
    // Something we asked for is unresolved. Re-issuing it every evaluation is
    // how a client floods a venue and gets rate-limited.
    if (order.has_pending_request()) {
        ++metrics_.pending_waits;
        decision.outcome = SlotOutcome::AwaitingPending;
        return decision;
    }

    // The same guard against a request we issued that the working state has not
    // caught up with yet. This is deliberately time-based rather than keyed on
    // the generation: generations advance on every evaluation, so a working
    // state lagging by a single cycle would defeat a generation check and the
    // manager would create one order per cycle.
    //
    // Bounded, so a request that genuinely never reached anyone does not wedge
    // the slot forever.
    const bool awaiting_own_request =
        (memory.last_action == OrderActionType::New ||
         memory.last_action == OrderActionType::Replace) &&
        !order.is_live();
    if (awaiting_own_request && config_.assume_request_lost_after_ns > 0 &&
        (now - memory.last_action_ns) < config_.assume_request_lost_after_ns) {
        ++metrics_.pending_waits;
        decision.outcome = SlotOutcome::AwaitingPending;
        return decision;
    }

    // ---- nothing wanted -------------------------------------------------
    if (!desired.wanted) {
        if (!order.is_live()) {
            decision.outcome = SlotOutcome::Idle;
            return decision;
        }
        OrderAction action = make_action(OrderActionType::Cancel, slot,
                                         ActionReason::QuoteDisabled, intent, input);
        action.target_client_order_id = order.client_order_id;
        action.target_exchange_order_id = order.exchange_order_id;
        push(result, action);
        memory.last_action = OrderActionType::Cancel;
        memory.last_action_generation = intent.generation;
        memory.last_action_ns = now;
        memory.last_cancel_ns = now;
        decision.outcome = SlotOutcome::Cancel;
        decision.reason = ActionReason::QuoteDisabled;
        return decision;
    }

    // ---- validation -----------------------------------------------------
    //
    // No invalid order action may be generated, and the failure must be
    // observable rather than silent.
    const RejectReason invalid = validate(slot, desired, input);
    if (invalid != RejectReason::None) {
        ++metrics_.invalid_quotes_blocked;
        decision.outcome = SlotOutcome::BlockedInvalid;
        decision.invalid_reason = invalid;
        return decision;
    }

    // ---- nothing resting ------------------------------------------------
    if (!order.is_live()) {
        if (config_.cooldown_after_cancel_ns > 0 && memory.last_cancel_ns > 0 &&
            (now - memory.last_cancel_ns) < config_.cooldown_after_cancel_ns) {
            ++metrics_.churn_suppressions;
            decision.outcome = SlotOutcome::SuppressedByChurnControl;
            return decision;
        }
        OrderAction action =
            make_action(OrderActionType::New, slot, ActionReason::NoQuoteResting, intent, input);
        action.price = desired.price;
        action.quantity = desired.quantity;
        push(result, action);
        memory.last_action = OrderActionType::New;
        memory.last_action_generation = intent.generation;
        memory.last_action_ns = now;
        decision.outcome = SlotOutcome::New;
        decision.reason = ActionReason::NoQuoteResting;
        return decision;
    }

    // ---- something rests: does it already say what we mean? --------------
    if (represents(order, desired, spec)) {
        ++metrics_.keeps;
        decision.outcome = SlotOutcome::Keep;
        return decision;
    }

    // It differs. Decide why, for the journal.
    ActionReason reason = ActionReason::PriceChanged;
    if (ticks_between(order.price, desired.price, spec.tick_size) <
        static_cast<std::int64_t>(config_.min_price_move_ticks)) {
        reason = order.is_partially_filled() ? ActionReason::PartialFillReplenish
                                             : ActionReason::QuantityChanged;
    }

    // Churn control: a replacement costs a round trip and a rate-limit slot.
    if (config_.min_replace_interval_ns > 0 && memory.last_action_ns > 0 &&
        (now - memory.last_action_ns) < config_.min_replace_interval_ns) {
        ++metrics_.churn_suppressions;
        decision.outcome = SlotOutcome::SuppressedByChurnControl;
        decision.reason = reason;
        return decision;
    }

    const bool atomic_replace =
        (input.capabilities != nullptr) && input.capabilities->supports_replace;

    if (atomic_replace) {
        OrderAction action = make_action(OrderActionType::Replace, slot, reason, intent, input);
        action.price = desired.price;
        action.quantity = desired.quantity;
        action.target_client_order_id = order.client_order_id;
        action.target_exchange_order_id = order.exchange_order_id;
        push(result, action);
        memory.last_action = OrderActionType::Replace;
        memory.last_action_generation = intent.generation;
        memory.last_action_ns = now;
        decision.outcome = SlotOutcome::Replace;
        decision.reason = reason;
        return decision;
    }

    // No atomic replace on this venue. Phase 3 deliberately refuses to
    // decompose silently, so the decomposition happens here, consciously, and
    // is reported as such: exposure is the union of both orders during the
    // window rather than a clean swap.
    OrderAction cancel = make_action(OrderActionType::Cancel, slot,
                                     ActionReason::ReplaceUnsupported, intent, input);
    cancel.target_client_order_id = order.client_order_id;
    cancel.target_exchange_order_id = order.exchange_order_id;
    push(result, cancel);

    OrderAction create =
        make_action(OrderActionType::New, slot, ActionReason::ReplaceUnsupported, intent, input);
    create.price = desired.price;
    create.quantity = desired.quantity;
    push(result, create);

    memory.last_action = OrderActionType::New;
    memory.last_action_generation = intent.generation;
    memory.last_action_ns = now;
    memory.last_cancel_ns = now;
    decision.outcome = SlotOutcome::CancelThenNew;
    decision.reason = reason;
    return decision;
}

// -------------------------------------------------------------- evaluate

QuoteManagerResult QuoteManager::evaluate(const QuoteIntent& intent,
                                          const QuoteManagerInput& input) {
    const Nanos started = clock_.steady();
    ++metrics_.intents_received;
    metrics_.foreign_orders_seen += input.working.foreign_order_count;

    QuoteManagerResult result;
    result.generation = intent.generation;

    if (intent.generation == last_generation_ && last_generation_ != 0) {
        // Not an error: re-evaluating the same intent must be idempotent, and
        // it is, because the decision is a function of the inputs. Counted so
        // an unexpectedly repetitive caller is visible.
        ++metrics_.duplicate_intents;
    }

    const QuoteRejection rejection = screen(intent, input);
    if (rejection != QuoteRejection::None) {
        ++metrics_.intents_rejected;
        result.rejection = rejection;
        if (rejection == QuoteRejection::GenerationRegression) {
            ++metrics_.generation_regressions;
        }
        if (rejection == QuoteRejection::IntentExpired ||
            rejection == QuoteRejection::MarketDataStale) {
            ++metrics_.stale_intents;
        }

        // A superseded or misaddressed intent changes nothing. An expired or
        // stale one means whatever is resting no longer reflects a market that
        // exists, so it is withdrawn -- as cancel actions, never by calling a
        // venue.
        if (implies_withdraw(rejection)) {
            const ActionReason reason = (rejection == QuoteRejection::IntentExpired)
                                            ? ActionReason::IntentExpired
                                        : (rejection == QuoteRejection::MarketDataStale)
                                            ? ActionReason::MarketDataStale
                                            : ActionReason::StrategyNotRunning;
            QuoteManagerResult withdraw = disable_all_quotes(input, reason);
            withdraw.rejection = rejection;
            withdraw.generation = intent.generation;
            // The withdrawal is now the newest statement about this symbol.
            if (intent.generation > last_generation_) {
                last_generation_ = intent.generation;
            }
            latency_.record(clock_.steady() - started);
            return withdraw;
        }
        latency_.record(clock_.steady() - started);
        return result;
    }

    // Both slots are derived from one intent and one snapshot of working state,
    // so the resulting action set cannot mix generations.
    DesiredQuote bid;
    DesiredQuote ask;
    if (intent.action == QuoteAction::Quote) {
        bid.wanted = intent.quote_bid;
        bid.price = intent.bid_price;
        bid.quantity = intent.bid_quantity;
        ask.wanted = intent.quote_ask;
        ask.price = intent.ask_price;
        ask.quantity = intent.ask_quantity;
    } else if (intent.action == QuoteAction::NoChange) {
        // "Leave my quotes alone." Whatever rests continues to represent the
        // strategy's intent, so the desired state is what is already there.
        const WorkingOrder& resting_bid = input.working.slot(QuoteSlot::Bid);
        const WorkingOrder& resting_ask = input.working.slot(QuoteSlot::Ask);
        bid.wanted = resting_bid.is_live();
        bid.price = resting_bid.price;
        bid.quantity = resting_bid.remaining();
        ask.wanted = resting_ask.is_live();
        ask.price = resting_ask.price;
        ask.quantity = resting_ask.remaining();
    }
    // QuoteAction::Pull leaves both `wanted` false, which cancels below.

    result.intent_accepted = true;
    result.bid = resolve_slot(QuoteSlot::Bid, bid, intent, input, result);
    result.ask = resolve_slot(QuoteSlot::Ask, ask, intent, input, result);

    if (intent.generation > last_generation_) {
        last_generation_ = intent.generation;
    }
    if (!result.has_actions()) {
        ++metrics_.noop_evaluations;
    }
    latency_.record(clock_.steady() - started);
    return result;
}

QuoteManagerResult QuoteManager::disable_all_quotes(const QuoteManagerInput& input,
                                                    ActionReason reason) {
    QuoteManagerResult result;
    result.intent_accepted = true;
    result.generation = last_generation_;

    QuoteIntent synthetic;
    static_cast<void>(synthetic.identity.name.assign(owner_.view()));
    synthetic.identity.version = 1;
    synthetic.generation = last_generation_;

    const Nanos now = clock_.steady();
    for (const QuoteSlot slot : {QuoteSlot::Bid, QuoteSlot::Ask}) {
        const WorkingOrder& order = input.working.slot(slot);
        SlotDecision decision;

        // Never reach outside what this manager owns, even when killing.
        if (order.present && !order.owner.matches(owner_, slot)) {
            ++metrics_.ownership_violations;
            decision.outcome = SlotOutcome::FrozenUnknown;
        } else if (!order.present || !order.is_live()) {
            decision.outcome = SlotOutcome::Idle;
        } else if (order.pending == PendingOperation::Cancel) {
            // A cancel is already in flight; re-sending it every cycle is churn.
            ++metrics_.pending_waits;
            decision.outcome = SlotOutcome::AwaitingPending;
        } else {
            OrderAction action = make_action(OrderActionType::Cancel, slot, reason, synthetic,
                                             input);
            action.target_client_order_id = order.client_order_id;
            action.target_exchange_order_id = order.exchange_order_id;
            push(result, action);
            SlotMemory& memory = memory_[static_cast<std::size_t>(slot)];
            memory.last_action = OrderActionType::Cancel;
            memory.last_action_ns = now;
            memory.last_cancel_ns = now;
            decision.outcome = SlotOutcome::Cancel;
            decision.reason = reason;
        }

        (slot == QuoteSlot::Bid ? result.bid : result.ask) = decision;
    }
    return result;
}

}  // namespace mm::quote
