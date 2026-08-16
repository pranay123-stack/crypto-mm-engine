#include "mm/risk/RiskEngine.hpp"

#include "mm/exchange/common/OrderRequest.hpp"

namespace mm::risk {
namespace {

using quote::OrderActionType;

/// Absolute distance from a reference price, in basis points. Reporting and
/// band checks only; no order size is ever derived from a double.
[[nodiscard]] double distance_bps(Px price, Px reference) noexcept {
    if (!reference.is_positive()) {
        return 0.0;
    }
    const double diff = static_cast<double>(price.raw() - reference.raw());
    const double away = 10'000.0 * diff / static_cast<double>(reference.raw());
    return away < 0.0 ? -away : away;
}

/// A cancel or a noop never adds exposure and is therefore never blocked by an
/// exposure limit. This predicate is the difference between "risk stops you
/// trading" and "risk stops you stopping".
[[nodiscard]] constexpr bool adds_exposure(OrderActionType type) noexcept {
    return type == OrderActionType::New || type == OrderActionType::Replace;
}

/// The venue's own rules: tick grid, lot grid, minimum notional, bounds.
///
/// Risk is the last checkpoint before the OMS, so it re-applies these rather
/// than trusting that the quote manager did. An approved order that the venue
/// would reject is one the OMS will send, one round trip and one rate-limit
/// slot wasted, and a reject burst that the safety layer reads as an outage.
[[nodiscard]] bool passes_venue_rules(const OrderAction& action, Qty quantity,
                                      const InstrumentSpec& spec) noexcept {
    exchange::OrderRequest probe;
    static_cast<void>(probe.client_order_id.assign("risk-venue-probe"));
    probe.symbol = spec.symbol;
    probe.side = action.side;
    probe.type = OrderType::Limit;
    probe.price = action.price;
    probe.quantity = quantity;
    exchange::ExchangeCapabilities caps;
    caps.supports_client_order_id = true;
    return !exchange::validate_order(probe, spec, caps).is_error();
}

}  // namespace

RiskEngine::RiskEngine(RiskLimits limits, const Clock& clock)
    : limits_(limits),
      clock_(clock),
      new_orders_(limits.global.burst_capacity > 0 ? limits.global.burst_capacity
                                                   : limits.global.max_new_orders_per_second,
                  limits.global.max_new_orders_per_second),
      cancels_(limits.global.burst_capacity > 0 ? limits.global.burst_capacity
                                                : limits.global.max_cancels_per_second,
               limits.global.max_cancels_per_second),
      replaces_(limits.global.burst_capacity > 0 ? limits.global.burst_capacity
                                                 : limits.global.max_replaces_per_second,
                limits.global.max_replaces_per_second),
      all_actions_(limits.global.burst_capacity > 0 ? limits.global.burst_capacity
                                                    : limits.global.max_actions_per_second,
                   limits.global.max_actions_per_second) {}

void RiskEngine::transition(RiskState to, RiskReason reason) {
    if (!is_legal_transition(state_, to)) {
        return;
    }
    state_ = to;
    state_reason_ = reason;
}

Status RiskEngine::arm() {
    if (needs_operator_recovery(state_)) {
        return {ErrorCode::FailedPrecondition,
                "risk is killed or faulted; rearm explicitly before arming"};
    }
    MM_RETURN_IF_ERROR(limits_.validate_for_arming());
    transition(RiskState::Armed, RiskReason::None);
    return Status::ok();
}

void RiskEngine::warn(RiskReason reason) { transition(RiskState::Warning, reason); }

void RiskEngine::kill(RiskReason reason) {
    ++metrics_.kills;
    transition(RiskState::Killed, reason);
}

void RiskEngine::fault(RiskReason reason) { transition(RiskState::Faulted, reason); }

Status RiskEngine::rearm() {
    if (!needs_operator_recovery(state_)) {
        return {ErrorCode::FailedPrecondition, "risk is not killed or faulted"};
    }
    // Back to Disarmed, not straight to Armed. Arming re-validates the limits,
    // so a kill cannot be undone without someone passing that check again.
    state_ = RiskState::Disarmed;
    state_reason_ = RiskReason::None;
    return Status::ok();
}

void RiskEngine::record(const RiskDecision& decision) {
    ++metrics_.evaluations;
    switch (decision.verdict) {
        case RiskVerdict::Approve: ++metrics_.approvals; break;
        case RiskVerdict::Reduce:  ++metrics_.reductions; break;
        case RiskVerdict::Reject: {
            ++metrics_.rejections;
            const auto index = static_cast<std::size_t>(decision.reason);
            if (index < metrics_.rejections_by_reason.size()) {
                ++metrics_.rejections_by_reason[index];
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- screening

RiskReason RiskEngine::screen(const OrderAction& action, const RiskInput& input) const {
    // Ownership first: an action for another subsystem's order is not ours to
    // judge, and approving it would let one strategy act on another's orders.
    if (!input.expected_owner.empty() && !action.identity.name.empty() &&
        action.identity.name != input.expected_owner) {
        return RiskReason::OwnershipViolation;
    }

    switch (state_) {
        case RiskState::Disarmed: return RiskReason::RiskDisarmed;
        case RiskState::Killed:   return RiskReason::RiskKilled;
        case RiskState::Faulted:  return RiskReason::RiskFaulted;
        case RiskState::Warning:
        case RiskState::Armed:
            break;
    }
    if (!input.system_ready) {
        return RiskReason::SystemNotReady;
    }
    if (input.instrument == nullptr || !input.instrument->is_valid()) {
        return RiskReason::InstrumentInvalid;
    }

    // Stale market data blocks new exposure. It must never block a cancel --
    // the moment data goes stale is precisely when withdrawing matters most,
    // and that check happens before this function is reached.
    if (input.market_data_age_ns > limits_.global.max_market_data_age_ns) {
        return RiskReason::StaleMarket;
    }
    if (!input.bbo.is_sane()) {
        return RiskReason::ReferencePriceUnavailable;
    }

    // No authoritative position means no way to reason about a position limit.
    if (!input.position.valid) {
        return RiskReason::PositionUnavailable;
    }
    if (input.position.as_of_ns > 0) {
        const Nanos age = clock_.steady() - input.position.as_of_ns;
        if (age > limits_.global.max_position_age_ns) {
            return RiskReason::PositionStale;
        }
    }
    // A working order whose state we cannot determine might be filling right
    // now. Adding to it would be adding to an amount we cannot measure.
    if (!input.exposure.determinate) {
        return RiskReason::UnknownExposure;
    }
    return RiskReason::None;
}

// ------------------------------------------------------------ order bounds

RiskReason RiskEngine::check_order_bounds(const OrderAction& action, Qty quantity,
                                          const RiskInput& input, const SymbolLimits& limits,
                                          LimitContext& context) const {
    if (!quantity.is_positive()) {
        return RiskReason::InvalidQuantity;
    }
    if (!action.price.is_positive()) {
        return RiskReason::InvalidPrice;
    }
    if (limits.max_order_quantity.is_positive() && quantity > limits.max_order_quantity) {
        return RiskReason::MaxOrderQuantity;
    }

    Notional order_notional;
    if (!checked_notional_of(action.price, quantity, order_notional)) {
        // A wrapped notional would turn an enormous order into a small one and
        // approve it. Overflow is a rejection, never a wrap.
        return RiskReason::ArithmeticOverflow;
    }
    context.order_notional = order_notional;
    if (limits.max_order_notional.is_positive() && order_notional > limits.max_order_notional) {
        return RiskReason::MaxOrderNotional;
    }

    if (limits.price_band_bps > 0) {
        const Px reference = input.bbo.mid();
        if (!reference.is_positive()) {
            return RiskReason::ReferencePriceUnavailable;
        }
        if (distance_bps(action.price, reference) >
            static_cast<double>(limits.price_band_bps)) {
            return RiskReason::PriceOutsideBand;
        }
    }

    // The venue's own rules, applied on every path rather than only after a
    // reduction. Risk is the last gate before the OMS; approving something the
    // venue will refuse is a round trip and a rate-limit slot spent to learn
    // nothing, and a burst of them reads as an outage to the safety layer.
    if (!passes_venue_rules(action, quantity, *input.instrument)) {
        return RiskReason::ReductionInvalid;
    }
    return RiskReason::None;
}

// ---------------------------------------------------------------- exposure

RiskReason RiskEngine::check_exposure(const OrderAction& action, Qty quantity,
                                      const RiskInput& input, const SymbolLimits& limits,
                                      LimitContext& context) const {
    if (limits.max_open_orders > 0 &&
        input.exposure.open_order_count >= static_cast<std::uint32_t>(limits.max_open_orders)) {
        // A replace does not add an order, so it is exempt from the count.
        if (action.type == OrderActionType::New) {
            return RiskReason::MaxOpenOrders;
        }
    }

    // A replace on a venue with an atomic swap removes the old order at the
    // instant the new one exists. Without that guarantee both can be live at
    // once, so the full new quantity lands on top -- assuming otherwise would
    // understate exposure during exactly the window in which it is highest.
    ExposureSnapshot projected = input.exposure;
    if (action.type == OrderActionType::Replace && input.atomic_replace &&
        input.replaced_order_remaining.is_positive()) {
        Qty& side = (action.side == Side::Buy) ? projected.working_buy : projected.working_sell;
        Qty reduced;
        if (!checked_sub(side, input.replaced_order_remaining, reduced)) {
            return RiskReason::ArithmeticOverflow;
        }
        side = reduced.is_positive() ? reduced : Qty::zero();
    }

    WorstCaseExposure worst;
    if (!compute_worst_case_with(input.position, projected, action.side, quantity, worst)) {
        return RiskReason::ArithmeticOverflow;
    }
    context.peak_absolute_exposure = worst.peak_absolute();
    context.working_buy = projected.working_buy;
    context.working_sell = projected.working_sell;

    if (limits.max_position.is_positive() && worst.peak_absolute() > limits.max_position) {
        return RiskReason::MaxPosition;
    }

    if (limits.max_position_notional.is_positive()) {
        Notional peak_notional;
        if (!checked_notional_of(input.bbo.mid(), worst.peak_absolute(), peak_notional)) {
            return RiskReason::ArithmeticOverflow;
        }
        if (peak_notional > limits.max_position_notional) {
            return RiskReason::MaxPositionNotional;
        }
    }

    // Side and gross working exposure, with the prospective order included.
    Qty projected_side = (action.side == Side::Buy) ? projected.working_buy
                                                    : projected.working_sell;
    if (!checked_add(projected_side, quantity, projected_side)) {
        return RiskReason::ArithmeticOverflow;
    }
    if (limits.max_side_exposure.is_positive() && projected_side > limits.max_side_exposure) {
        return RiskReason::MaxSideExposure;
    }
    if (limits.max_working_exposure.is_positive()) {
        Qty gross;
        if (!checked_add(projected.working_buy, projected.working_sell, gross) ||
            !checked_add(gross, quantity, gross)) {
            return RiskReason::ArithmeticOverflow;
        }
        if (gross > limits.max_working_exposure) {
            return RiskReason::MaxWorkingExposure;
        }
    }

    // Reduce-only: an action that unwinds the position passes, one that adds to
    // it does not. This is what lets an operator de-risk without stranding a
    // position that still needs working out of.
    if (is_reduce_only(state_) &&
        increases_absolute_position(input.position, action.side, quantity)) {
        return RiskReason::ReduceOnly;
    }
    return RiskReason::None;
}

// --------------------------------------------------------------- reduction

Qty RiskEngine::largest_permitted_quantity(const OrderAction& action, const RiskInput& input,
                                           const SymbolLimits& limits) const {
    // Binary search over the lot grid for the largest quantity that satisfies
    // every bound. Searching rather than solving each limit algebraically keeps
    // one source of truth -- the checks themselves -- so a new limit is
    // automatically respected by the reduction path.
    const InstrumentSpec& spec = *input.instrument;
    const Qty step = spec.lot_size.is_positive() ? spec.lot_size : Qty::from_raw(1);

    std::int64_t low = 0;
    std::int64_t high = action.quantity.raw() / step.raw();
    if (high <= 0) {
        return Qty::zero();
    }

    LimitContext scratch;

    // Early-out: if a single lot does not fit, nothing does, and searching for
    // it costs a full binary search that ends in the same rejection. Measured:
    // without this, a rejection cost more than a reduction (1318 ns vs 1058),
    // because the rejecting case exhausted the search before giving up.
    const Qty smallest = Qty::from_raw(step.raw());
    if (check_order_bounds(action, smallest, input, limits, scratch) != RiskReason::None ||
        check_exposure(action, smallest, input, limits, scratch) != RiskReason::None) {
        return Qty::zero();
    }
    while (low < high) {
        const std::int64_t mid = low + (high - low + 1) / 2;
        const Qty candidate = Qty::from_raw(mid * step.raw());
        const bool ok =
            check_order_bounds(action, candidate, input, limits, scratch) == RiskReason::None &&
            check_exposure(action, candidate, input, limits, scratch) == RiskReason::None;
        if (ok) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return Qty::from_raw(low * step.raw());
}

// ---------------------------------------------------------------- evaluate

RiskDecision RiskEngine::approve_cancel(const OrderAction& cancel, const RiskInput& input) {
    const Nanos now = clock_.steady();
    RiskDecision decision;
    decision.verdict = RiskVerdict::Approve;
    decision.reason = RiskReason::None;
    decision.state = state_;
    decision.requested = cancel;
    decision.approved = cancel;
    decision.decided_ns = now;
    decision.generation = cancel.generation;
    decision.trace = cancel.trace;

    // Ownership is the one thing that can refuse a cancel: cancelling another
    // subsystem's order is not withdrawal, it is interference.
    if (!input.expected_owner.empty() && !cancel.identity.name.empty() &&
        cancel.identity.name != input.expected_owner) {
        decision = RiskDecision::rejected(cancel, RiskReason::OwnershipViolation, state_, now);
        record(decision);
        return decision;
    }

    // Cancels are counted against a bucket for observability, but never refused
    // on rate grounds. The moment the system is bursting is the moment
    // withdrawing matters most, and a rate limit that blocks an exit is a rate
    // limit that turns congestion into exposure.
    static_cast<void>(cancels_.try_acquire(now));
    if (!permits_new_exposure(state_)) {
        ++metrics_.cancels_permitted_while_blocked;
    }
    record(decision);
    return decision;
}

RiskDecision RiskEngine::evaluate(const OrderAction& action, const RiskInput& input) {
    const Nanos started = clock_.steady();

    // Nothing to judge.
    if (action.type == OrderActionType::Noop) {
        RiskDecision decision;
        decision.verdict = RiskVerdict::Approve;
        decision.state = state_;
        decision.requested = action;
        decision.approved = action;
        decision.decided_ns = started;
        decision.generation = action.generation;
        decision.trace = action.trace;
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    // Cancellation is available in every state, including killed and faulted.
    if (action.type == OrderActionType::Cancel) {
        const RiskDecision decision = approve_cancel(action, input);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    // From here on the action adds exposure, and every check below is a reason
    // to refuse it.
    const RiskReason screened = screen(action, input);
    if (screened != RiskReason::None) {
        const RiskDecision decision =
            RiskDecision::rejected(action, screened, state_, started);
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    const SymbolLimits* limits = limits_.find(input.symbol);
    if (limits == nullptr) {
        // An unconfigured symbol is not an unlimited one.
        const RiskDecision decision =
            RiskDecision::rejected(action, RiskReason::ConfigurationMissing, state_, started);
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    if (adds_exposure(action.type)) {
        RateLimiter& bucket =
            (action.type == OrderActionType::New) ? new_orders_ : replaces_;
        if (!bucket.try_acquire(started) || !all_actions_.try_acquire(started)) {
            const RiskDecision decision =
                RiskDecision::rejected(action, RiskReason::RateLimit, state_, started);
            record(decision);
            latency_.record(clock_.steady() - started);
            return decision;
        }
    }

    LimitContext context;
    context.max_position = limits->max_position;
    context.open_orders = input.exposure.open_order_count;

    RiskReason failure = check_order_bounds(action, action.quantity, input, *limits, context);
    if (failure == RiskReason::None) {
        failure = check_exposure(action, action.quantity, input, *limits, context);
    }

    if (failure == RiskReason::None) {
        RiskDecision decision;
        decision.verdict = RiskVerdict::Approve;
        decision.state = state_;
        decision.requested = action;
        decision.approved = action;
        decision.limits = context;
        decision.decided_ns = started;
        decision.generation = action.generation;
        decision.trace = action.trace;
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    // ---- reduction ------------------------------------------------------
    //
    // Some failures admit a smaller order; others do not. An invalid price, an
    // overflow or a state prohibition is not made acceptable by trading less.
    const bool reducible = (failure == RiskReason::MaxPosition ||
                            failure == RiskReason::MaxPositionNotional ||
                            failure == RiskReason::MaxOrderQuantity ||
                            failure == RiskReason::MaxOrderNotional ||
                            failure == RiskReason::MaxSideExposure ||
                            failure == RiskReason::MaxWorkingExposure);
    if (!reducible) {
        const RiskDecision decision = RiskDecision::rejected(action, failure, state_, started);
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    const Qty permitted = largest_permitted_quantity(action, input, *limits);
    if (!permitted.is_positive() || permitted >= action.quantity) {
        // Nothing smaller works, or the search produced something not smaller --
        // which would mean the transformation increased exposure.
        const RiskDecision decision = RiskDecision::rejected(action, failure, state_, started);
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    OrderAction reduced = action;
    reduced.quantity = permitted;

    // ---- revalidation ---------------------------------------------------
    //
    // The reduced action goes through the full pipeline again. Checking risk,
    // modifying the order and then sending it is how an order that nobody ever
    // approved reaches a venue.
    LimitContext revalidated;
    revalidated.max_position = limits->max_position;
    revalidated.open_orders = input.exposure.open_order_count;

    RiskReason recheck = check_order_bounds(reduced, reduced.quantity, input, *limits,
                                            revalidated);
    if (recheck == RiskReason::None) {
        recheck = check_exposure(reduced, reduced.quantity, input, *limits, revalidated);
    }
    // `check_order_bounds` already re-applies the venue rules, so a reduction
    // that lands off the lot grid or under the minimum notional fails here --
    // reducing is not always possible.
    if (recheck != RiskReason::None) {
        const RiskDecision decision = RiskDecision::rejected(action, recheck, state_, started);
        record(decision);
        latency_.record(clock_.steady() - started);
        return decision;
    }

    RiskDecision decision;
    decision.verdict = RiskVerdict::Reduce;
    // The reason records why the reduction was necessary, so the record shows
    // which limit bound the order rather than merely that one did.
    decision.reason = failure;
    decision.state = state_;
    decision.requested = action;
    decision.approved = reduced;
    decision.limits = revalidated;
    decision.decided_ns = started;
    decision.generation = action.generation;
    decision.trace = action.trace;
    record(decision);
    latency_.record(clock_.steady() - started);
    return decision;
}

}  // namespace mm::risk
