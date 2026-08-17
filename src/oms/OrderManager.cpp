#include "mm/oms/OrderManager.hpp"

#include <algorithm>

namespace mm::oms {
namespace {

using quote::OrderAction;
using quote::OrderActionType;
using quote::QuoteSlot;

}  // namespace

OrderManager::OrderManager(OmsConfig config, IOrderJournal& journal, const Clock& clock)
    : config_(config),
      id_generator_(config.client_id_prefix.view(), config.session_id,
                    config.max_client_id_length),
      journal_(journal),
      clock_(clock) {
    // Reserved once: an order record must never allocate on the submit path,
    // and exhaustion must be a bounded, observable condition.
    orders_.reserve(config_.max_orders);
    deferred_.resize(config_.max_orders);
}

// ------------------------------------------------------------------ journal

void OrderManager::journal(OrderEventType type, const OrderRecord& order, OrderState from,
                           OrderState to, OmsReject reject) {
    OrderJournalRecord record;
    record.sequence = ++journal_sequence_;
    record.type = type;
    record.logical_id = order.logical_id;
    record.client_order_id = order.client_order_id;
    record.exchange_order_id = order.exchange_order_id;
    record.symbol = order.symbol;
    record.side = order.side;
    record.from_state = from;
    record.to_state = to;
    record.price = order.price;
    record.quantity = order.original_quantity;
    record.cumulative_quantity = order.cumulative_quantity;
    record.reject = reject;
    record.request_id = order.pending_request;
    record.recorded_ns = clock_.steady();
    record.venue_ns = order.venue_ns;
    record.generation = order.ownership.generation;
    record.trace = order.trace;
    journal_.append(record);
}

void OrderManager::journal_rejection(OrderEventType type, const OrderAction& action,
                                     OmsReject reject) {
    OrderJournalRecord record;
    record.sequence = ++journal_sequence_;
    record.type = type;
    record.symbol = action.symbol;
    record.side = action.side;
    record.price = action.price;
    record.quantity = action.quantity;
    record.reject = reject;
    record.recorded_ns = clock_.steady();
    record.generation = action.generation;
    record.trace = action.trace;
    journal_.append(record);
}

void OrderManager::record_reject(OmsReject reject) {
    const auto index = static_cast<std::size_t>(reject);
    if (index < metrics_.rejects_by_reason.size()) {
        ++metrics_.rejects_by_reason[index];
    }
}

bool OrderManager::transition(OrderRecord& order, OrderState to, OrderEventType event,
                              const OrderJournalRecord& detail) {
    const OrderState from = order.state;
    if (!is_legal_transition(from, to)) {
        // A state machine that quietly accepts anything is not a state machine.
        ++metrics_.illegal_transitions;
        record_reject(OmsReject::IllegalTransition);
        OrderJournalRecord quarantine = detail;
        quarantine.sequence = ++journal_sequence_;
        quarantine.type = OrderEventType::EventQuarantined;
        quarantine.from_state = from;
        quarantine.to_state = to;
        quarantine.reject = OmsReject::IllegalTransition;
        quarantine.recorded_ns = clock_.steady();
        journal_.append(quarantine);
        return false;
    }
    order.state = to;
    if (is_terminal(to)) {
        order.terminal_ns = clock_.steady();
        order.pending_request = RequestId::kInvalid;
        order.pending_since_ns = 0;
    }
    if (to == OrderState::Unknown) {
        ++metrics_.unknown_states;
    }
    journal(event, order, from, to);
    return true;
}

// ------------------------------------------------------------------ lookup

OrderRecord* OrderManager::mutable_find(LogicalOrderId id) {
    const auto it = by_logical_id_.find(static_cast<std::uint64_t>(id));
    return it == by_logical_id_.end() ? nullptr : &orders_[it->second];
}

OrderRecord* OrderManager::mutable_find(const ClientOrderId& id) {
    const auto it = by_client_id_.find(id);
    return it == by_client_id_.end() ? nullptr : &orders_[it->second];
}

OrderRecord* OrderManager::mutable_find_by_exchange_id(const ExchangeOrderId& id) {
    const auto it = by_exchange_id_.find(id);
    return it == by_exchange_id_.end() ? nullptr : &orders_[it->second];
}

const OrderRecord* OrderManager::find(LogicalOrderId id) const {
    const auto it = by_logical_id_.find(static_cast<std::uint64_t>(id));
    return it == by_logical_id_.end() ? nullptr : &orders_[it->second];
}

const OrderRecord* OrderManager::find(const ClientOrderId& id) const {
    const auto it = by_client_id_.find(id);
    return it == by_client_id_.end() ? nullptr : &orders_[it->second];
}

const OrderRecord* OrderManager::find_by_exchange_id(const ExchangeOrderId& id) const {
    const auto it = by_exchange_id_.find(id);
    return it == by_exchange_id_.end() ? nullptr : &orders_[it->second];
}

OrderRecord* OrderManager::find_slot_order(const StrategyName& owner, const Symbol& symbol,
                                           QuoteSlot slot) {
    for (OrderRecord& order : orders_) {
        if (order.symbol == symbol && order.ownership.matches(owner, slot) &&
            order.consumes_exposure()) {
            return &order;
        }
    }
    return nullptr;
}

std::size_t OrderManager::live_order_count() const {
    return static_cast<std::size_t>(
        std::count_if(orders_.begin(), orders_.end(),
                      [](const OrderRecord& o) { return o.is_live(); }));
}

// ------------------------------------------------------------------ submit

SubmitResult OrderManager::submit(const risk::ApprovedAction& approved) {
    const Nanos started = clock_.steady();
    SubmitResult result;

    // The single door. A caller cannot construct a valid ApprovedAction, so
    // there is no way to hand the OMS an order risk has not seen.
    if (!approved.is_valid()) {
        ++metrics_.not_risk_approved;
        record_reject(OmsReject::NotRiskApproved);
        result.reject = OmsReject::NotRiskApproved;
        journal_rejection(OrderEventType::EventQuarantined, approved.action(),
                          OmsReject::NotRiskApproved);
        submit_latency_.record(clock_.steady() - started);
        return result;
    }

    const OrderAction& action = approved.action();
    switch (action.type) {
        case OrderActionType::New:     result = handle_new(action); break;
        case OrderActionType::Cancel:  result = handle_cancel(action); break;
        case OrderActionType::Replace: result = handle_replace(action); break;
        case OrderActionType::Noop:
            result.accepted = true;
            break;
    }
    submit_latency_.record(clock_.steady() - started);
    return result;
}

SubmitResult OrderManager::handle_new(const OrderAction& action) {
    SubmitResult result;

    if (orders_.size() >= config_.max_orders) {
        record_reject(OmsReject::CapacityExhausted);
        result.reject = OmsReject::CapacityExhausted;
        return result;
    }

    // Cancel-then-new: the replacement must not exist before the cancellation
    // boundary. If the slot still holds an order that is being cancelled, the
    // New is held rather than sent -- two live orders on one slot is exactly the
    // exposure the sequencing exists to avoid.
    OrderRecord* occupant = find_slot_order(action.identity.name, action.symbol, action.slot);
    if (occupant != nullptr) {
        if (occupant->state == OrderState::PendingCancel) {
            for (DeferredNew& slot : deferred_) {
                if (!slot.present) {
                    slot.present = true;
                    slot.action = action;
                    slot.waiting_on = occupant->logical_id;
                    ++deferred_count_;
                    ++metrics_.deferred_news;
                    result.accepted = true;
                    result.deferred = true;
                    return result;
                }
            }
            record_reject(OmsReject::CapacityExhausted);
            result.reject = OmsReject::CapacityExhausted;
            return result;
        }
        if (occupant->has_pending_request()) {
            // A request is already outstanding for this slot. Issuing another
            // because an acknowledgement is slow is how duplicate orders happen.
            ++metrics_.duplicate_requests;
            record_reject(OmsReject::PendingOperation);
            result.reject = OmsReject::PendingOperation;
            return result;
        }
    }

    ClientOrderId client_id;
    if (!id_generator_.next(client_id)) {
        // Truncation is failure: a shortened id can collide, and the order it
        // names becomes unfindable exactly when reconciliation needs it.
        record_reject(OmsReject::IdentityGenerationFailed);
        result.reject = OmsReject::IdentityGenerationFailed;
        return result;
    }
    if (by_client_id_.find(client_id) != by_client_id_.end()) {
        record_reject(OmsReject::IdentityCollision);
        result.reject = OmsReject::IdentityCollision;
        return result;
    }

    OrderRecord order;
    order.logical_id = static_cast<LogicalOrderId>(++next_logical_id_);
    order.client_order_id = client_id;
    order.ownership.strategy = action.identity.name;
    order.ownership.slot = action.slot;
    order.ownership.generation = action.generation;
    order.ownership.creation_sequence = ++creation_sequence_;
    order.symbol = action.symbol;
    order.symbol_id = action.symbol_id;
    order.side = action.side;
    order.type = OrderType::Limit;
    order.tif = TimeInForce::GTC;
    order.price = action.price;
    order.original_quantity = action.quantity;
    order.state = OrderState::Created;
    order.created_ns = clock_.steady();
    order.trace = action.trace;

    const std::size_t index = orders_.size();
    orders_.push_back(order);
    by_client_id_.emplace(client_id, index);
    by_logical_id_.emplace(static_cast<std::uint64_t>(order.logical_id), index);
    ++metrics_.orders_created;
    journal(OrderEventType::Created, orders_[index], OrderState::Created, OrderState::Created);

    OrderRecord& stored = orders_[index];
    if (execution_ == nullptr) {
        record_reject(OmsReject::ExecutionUnavailable);
        result.reject = OmsReject::ExecutionUnavailable;
        result.logical_id = stored.logical_id;
        return result;
    }

    exchange::OrderRequest request;
    request.client_order_id = stored.client_order_id;
    request.symbol = stored.symbol;
    request.symbol_id = stored.symbol_id;
    request.side = stored.side;
    request.type = stored.type;
    request.tif = stored.tif;
    request.price = stored.price;
    request.quantity = stored.original_quantity;
    request.trace = stored.trace;
    request.created_ns = clock_.steady();

    stored.pending_request = next_request_id();
    stored.pending_since_ns = clock_.steady();
    stored.sent_ns = stored.pending_since_ns;

    // PendingNew, not Working. An order is not on the book because we asked for
    // it to be; only an acknowledgement establishes that.
    OrderJournalRecord detail;
    detail.logical_id = stored.logical_id;
    static_cast<void>(transition(stored, OrderState::PendingNew, OrderEventType::RequestSent,
                                 detail));
    ++metrics_.new_requests;

    const Status sent = execution_->submit(request);
    if (sent.is_error()) {
        // The adapter refused before anything left the process, so the order
        // definitively does not exist.
        stored.pending_request = RequestId::kInvalid;
        static_cast<void>(transition(stored, OrderState::Rejected, OrderEventType::Rejected,
                                     detail));
        ++metrics_.rejects;
        result.reject = OmsReject::ExecutionUnavailable;
        result.logical_id = stored.logical_id;
        return result;
    }

    result.accepted = true;
    result.logical_id = stored.logical_id;
    result.request_id = stored.pending_request;
    return result;
}

SubmitResult OrderManager::handle_cancel(const OrderAction& action) {
    SubmitResult result;

    OrderRecord* order = nullptr;
    if (!action.target_client_order_id.empty()) {
        order = mutable_find(action.target_client_order_id);
    }
    if (order == nullptr && !action.target_exchange_order_id.empty()) {
        order = mutable_find_by_exchange_id(action.target_exchange_order_id);
    }
    if (order == nullptr) {
        order = find_slot_order(action.identity.name, action.symbol, action.slot);
    }
    if (order == nullptr) {
        record_reject(OmsReject::UnknownOrder);
        result.reject = OmsReject::UnknownOrder;
        return result;
    }

    // Never act on somebody else's order.
    if (!action.identity.name.empty() &&
        !order->ownership.matches(action.identity.name, order->ownership.slot)) {
        ++metrics_.ownership_violations;
        record_reject(OmsReject::OwnershipViolation);
        result.reject = OmsReject::OwnershipViolation;
        return result;
    }
    if (order->is_terminal()) {
        record_reject(OmsReject::TerminalOrder);
        result.reject = OmsReject::TerminalOrder;
        result.logical_id = order->logical_id;
        return result;
    }
    if (order->state == OrderState::PendingCancel) {
        // Already asked. Re-sending on every evaluation is how a client floods
        // a venue and gets rate-limited.
        ++metrics_.duplicate_requests;
        record_reject(OmsReject::DuplicateRequest);
        result.reject = OmsReject::DuplicateRequest;
        result.logical_id = order->logical_id;
        result.coalesced = true;
        return result;
    }
    if (execution_ == nullptr) {
        record_reject(OmsReject::ExecutionUnavailable);
        result.reject = OmsReject::ExecutionUnavailable;
        return result;
    }

    exchange::CancelRequest request;
    request.client_order_id = order->client_order_id;
    request.exchange_order_id = order->exchange_order_id;
    request.symbol = order->symbol;
    request.symbol_id = order->symbol_id;
    request.trace = order->trace;
    request.created_ns = clock_.steady();

    order->pending_request = next_request_id();
    order->pending_since_ns = clock_.steady();

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;
    // Exposure is NOT released here. A cancel is a request, not a result.
    static_cast<void>(transition(*order, OrderState::PendingCancel,
                                 OrderEventType::CancelRequested, detail));
    ++metrics_.cancel_requests;

    const Status sent = execution_->cancel(request);
    if (sent.is_error()) {
        // The request never left. The order is still working, so the state goes
        // back rather than being assumed gone.
        order->pending_request = RequestId::kInvalid;
        static_cast<void>(transition(*order, OrderState::Working,
                                     OrderEventType::CancelRejected, detail));
        result.reject = OmsReject::ExecutionUnavailable;
        result.logical_id = order->logical_id;
        return result;
    }

    result.accepted = true;
    result.logical_id = order->logical_id;
    result.request_id = order->pending_request;
    return result;
}

SubmitResult OrderManager::handle_replace(const OrderAction& action) {
    SubmitResult result;

    OrderRecord* order = nullptr;
    if (!action.target_client_order_id.empty()) {
        order = mutable_find(action.target_client_order_id);
    }
    if (order == nullptr) {
        order = find_slot_order(action.identity.name, action.symbol, action.slot);
    }
    if (order == nullptr) {
        record_reject(OmsReject::UnknownOrder);
        result.reject = OmsReject::UnknownOrder;
        return result;
    }
    if (!action.identity.name.empty() &&
        !order->ownership.matches(action.identity.name, order->ownership.slot)) {
        ++metrics_.ownership_violations;
        record_reject(OmsReject::OwnershipViolation);
        result.reject = OmsReject::OwnershipViolation;
        return result;
    }
    if (order->is_terminal()) {
        record_reject(OmsReject::TerminalOrder);
        result.reject = OmsReject::TerminalOrder;
        return result;
    }
    // A generation older than the one this order already serves must not
    // resurrect an earlier quote.
    if (action.generation < order->ownership.generation) {
        ++metrics_.stale_operations;
        record_reject(OmsReject::StaleGeneration);
        result.reject = OmsReject::StaleGeneration;
        return result;
    }
    if (order->state == OrderState::PendingReplace) {
        // Coalescing policy: the newest desired state wins, but no second
        // request is issued while the first is outstanding. The pending target
        // is updated so the eventual acknowledgement lands on current intent.
        order->pending_price = action.price;
        order->pending_quantity = action.quantity;
        order->ownership.generation = action.generation;
        ++metrics_.duplicate_requests;
        record_reject(OmsReject::DuplicateRequest);
        result.reject = OmsReject::DuplicateRequest;
        result.logical_id = order->logical_id;
        result.coalesced = true;
        return result;
    }
    if (order->has_pending_request()) {
        ++metrics_.duplicate_requests;
        record_reject(OmsReject::PendingOperation);
        result.reject = OmsReject::PendingOperation;
        return result;
    }
    if (execution_ == nullptr) {
        record_reject(OmsReject::ExecutionUnavailable);
        result.reject = OmsReject::ExecutionUnavailable;
        return result;
    }

    ClientOrderId replacement_id;
    if (!id_generator_.next(replacement_id)) {
        record_reject(OmsReject::IdentityGenerationFailed);
        result.reject = OmsReject::IdentityGenerationFailed;
        return result;
    }

    exchange::ReplaceRequest request;
    request.original_client_order_id = order->client_order_id;
    request.original_exchange_order_id = order->exchange_order_id;
    request.new_client_order_id = replacement_id;
    request.symbol = order->symbol;
    request.symbol_id = order->symbol_id;
    request.new_price = action.price;
    request.new_quantity = action.quantity;
    request.trace = order->trace;
    request.created_ns = clock_.steady();

    // Held separately, so a rejected replace leaves the live parameters
    // untouched rather than half-applied.
    order->pending_price = action.price;
    order->pending_quantity = action.quantity;
    order->pending_client_order_id = replacement_id;
    order->pending_request = next_request_id();
    order->pending_since_ns = clock_.steady();
    order->ownership.generation = action.generation;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;
    static_cast<void>(transition(*order, OrderState::PendingReplace,
                                 OrderEventType::ReplaceRequested, detail));
    ++metrics_.replace_requests;

    const Status sent = execution_->replace(request);
    if (sent.is_error()) {
        order->pending_request = RequestId::kInvalid;
        order->pending_client_order_id.clear();
        static_cast<void>(transition(*order, OrderState::Working,
                                     OrderEventType::ReplaceRejected, detail));
        result.reject = OmsReject::ExecutionUnavailable;
        return result;
    }

    result.accepted = true;
    result.logical_id = order->logical_id;
    result.request_id = order->pending_request;
    return result;
}

// ------------------------------------------------------------------- events

void OrderManager::on_execution(const exchange::ExecutionEvent& event) {
    // Exec-io thread. Enqueue only: every mutation happens on the OMS thread,
    // which is what makes the same event sequence produce the same state.
    if (!queue_.try_push(event)) {
        // Losing an execution event loses order truth, so it is counted loudly
        // rather than absorbed.
        ++dropped_events_;
        ++metrics_.quarantined_events;
    }
}

std::size_t OrderManager::process_events() {
    std::size_t applied = 0;
    exchange::ExecutionEvent event;
    while (queue_.try_pop(event)) {
        const Nanos started = clock_.steady();
        apply(event);
        event_latency_.record(clock_.steady() - started);
        ++applied;
    }
    return applied;
}

void OrderManager::apply(const exchange::ExecutionEvent& event) {
    // A self-contradictory event is quarantined rather than acted on.
    if (exchange::validate_execution_event(event).is_error()) {
        ++metrics_.quarantined_events;
        return;
    }
    switch (event.type) {
        case exchange::ExecutionEventType::OrderAck:      apply_ack(event.payload.ack); break;
        case exchange::ExecutionEventType::OrderReject:   apply_reject(event.payload.reject); break;
        case exchange::ExecutionEventType::Fill:          apply_fill(event.payload.fill); break;
        case exchange::ExecutionEventType::OrderCancel:   apply_cancel(event.payload.cancel); break;
        case exchange::ExecutionEventType::OrderReplace:  apply_replace(event.payload.replace); break;
        case exchange::ExecutionEventType::RequestFailure:
            apply_failure(event.payload.failure);
            break;
        default:
            break;
    }
}

void OrderManager::apply_ack(const exchange::OrderAckEvent& ack) {
    OrderRecord* order = mutable_find(ack.client_order_id);
    if (order == nullptr) {
        // An acknowledgement for an order we have no record of. Never assumed
        // to be ours; quarantined for reconciliation.
        ++metrics_.orphan_orders;
        ++metrics_.quarantined_events;
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.duplicate_events;
        return;
    }
    // An event older than what we have already applied is stale, not new.
    if (ack.seq != kNoSeq && order->last_applied_sequence != kNoSeq &&
        ack.seq < order->last_applied_sequence) {
        ++metrics_.out_of_order_events;
        return;
    }
    if (order->acknowledged_ns != 0) {
        // We have already acknowledged this order. A venue redelivering the
        // ack -- or two streams racing so it arrives after its own fill -- is
        // never a reason to rewind the order to a freshly-acknowledged state.
        // Counting it as an illegal transition would be the wrong diagnosis:
        // nothing illegal happened, the event is simply not news.
        ++metrics_.duplicate_events;
        return;
    }

    if (!ack.exchange_order_id.empty()) {
        OrderRecord* existing = mutable_find_by_exchange_id(ack.exchange_order_id);
        if (existing != nullptr && existing->logical_id != order->logical_id) {
            // One venue identity must map to at most one local order.
            ++metrics_.quarantined_events;
            record_reject(OmsReject::IdentityCollision);
            return;
        }
        order->exchange_order_id = ack.exchange_order_id;
        const auto it = by_logical_id_.find(static_cast<std::uint64_t>(order->logical_id));
        if (it != by_logical_id_.end()) {
            by_exchange_id_[ack.exchange_order_id] = it->second;
        }
    }

    order->pending_request = RequestId::kInvalid;
    order->pending_since_ns = 0;
    order->acknowledged_ns = clock_.steady();
    order->venue_ns = ack.transact_ns;
    order->last_applied_sequence = ack.seq;
    ++metrics_.acknowledgements;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;
    // Some venues coalesce the acknowledgement with an immediate fill, so the
    // reported status decides the resulting state rather than an assumption.
    const OrderState target = from_venue_status(ack.status);
    static_cast<void>(transition(*order, target == OrderState::Unknown ? OrderState::Working
                                                                       : target,
                                 OrderEventType::Acknowledged, detail));
}

void OrderManager::apply_reject(const exchange::OrderRejectEvent& reject) {
    OrderRecord* order = mutable_find(reject.client_order_id);
    if (order == nullptr) {
        ++metrics_.quarantined_events;
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.duplicate_events;
        return;
    }
    order->pending_request = RequestId::kInvalid;
    order->venue_ns = reject.transact_ns;
    ++metrics_.rejects;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;
    static_cast<void>(transition(*order, OrderState::Rejected, OrderEventType::Rejected, detail));
    release_deferred(order->logical_id);
}

void OrderManager::apply_fill(const exchange::FillEvent& fill) {
    OrderRecord* order = mutable_find(fill.client_order_id);
    if (order == nullptr) {
        ++metrics_.orphan_orders;
        ++metrics_.quarantined_events;
        return;
    }

    if (order->is_terminal()) {
        // A fill for an order the venue already told us was finished. Two
        // streams racing will produce this: the cancel confirmation arrives
        // before a fill that happened earlier.
        //
        // It must not be applied. The quantity would move while the state
        // machine correctly refused the transition, leaving a CANCELLED order
        // carrying fill quantity -- a record no consumer reads correctly,
        // because a terminal order releases its exposure. Quarantining it
        // surfaces the divergence for reconciliation instead of burying it in
        // a record that looks ordinary.
        ++metrics_.duplicate_events;
        ++metrics_.quarantined_events;
        record_reject(OmsReject::TerminalOrder);
        return;
    }

    // Never trust the venue's arithmetic blindly. Validated before the trade
    // id is recorded, so a corrupt event cannot evict a real trade id from the
    // dedup ring.
    if (!fill.quantity.is_positive() || !fill.price.is_positive()) {
        ++metrics_.quarantined_events;
        record_reject(OmsReject::ImpossibleFill);
        return;
    }

    // Idempotency by trade id. A duplicate fill is the one event whose
    // double-application is unrecoverable: it corrupts position and PnL at the
    // same moment, and nothing downstream can tell afterwards.
    if (!order->seen_trades.insert(fill.trade_id)) {
        ++metrics_.duplicate_fills;
        ++metrics_.duplicate_events;
        return;
    }
    Qty new_cumulative;
    if (!checked_add(order->cumulative_quantity, fill.quantity, new_cumulative)) {
        ++metrics_.quarantined_events;
        record_reject(OmsReject::ArithmeticOverflow);
        return;
    }
    if (new_cumulative > order->original_quantity) {
        // A fill beyond the order's size cannot be true.
        ++metrics_.quarantined_events;
        record_reject(OmsReject::ImpossibleFill);
        return;
    }

    // Volume-weighted average, in fixed point throughout.
    Notional previous_value;
    Notional fill_value;
    if (!checked_notional_of(order->average_fill_price, order->cumulative_quantity,
                             previous_value) ||
        !checked_notional_of(fill.price, fill.quantity, fill_value)) {
        ++metrics_.quarantined_events;
        record_reject(OmsReject::ArithmeticOverflow);
        return;
    }
    Notional total_value;
    if (!checked_add(previous_value, fill_value, total_value)) {
        ++metrics_.quarantined_events;
        record_reject(OmsReject::ArithmeticOverflow);
        return;
    }
    order->cumulative_quantity = new_cumulative;
    if (new_cumulative.is_positive()) {
        order->average_fill_price = Px::from_raw(
            static_cast<std::int64_t>((static_cast<int128>(total_value.raw()) * Px::kScale) /
                                      static_cast<int128>(new_cumulative.raw())));
    }
    static_cast<void>(checked_add(order->cumulative_fees, fill.fee, order->cumulative_fees));
    order->venue_ns = fill.transact_ns;
    order->last_applied_sequence = fill.seq;

    const bool complete = (order->remaining().is_zero());
    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;
    detail.trade_id = fill.trade_id;
    detail.price = fill.price;
    detail.quantity = fill.quantity;
    detail.cumulative_quantity = order->cumulative_quantity;

    if (complete) {
        ++metrics_.fills;
        static_cast<void>(transition(*order, OrderState::Filled, OrderEventType::Filled, detail));
        release_deferred(order->logical_id);
    } else {
        ++metrics_.partial_fills;
        static_cast<void>(transition(*order, OrderState::PartiallyFilled,
                                     OrderEventType::PartiallyFilled, detail));
    }
}

void OrderManager::apply_cancel(const exchange::OrderCancelEvent& cancel) {
    OrderRecord* order = mutable_find(cancel.client_order_id);
    if (order == nullptr && !cancel.exchange_order_id.empty()) {
        order = mutable_find_by_exchange_id(cancel.exchange_order_id);
    }
    if (order == nullptr) {
        ++metrics_.quarantined_events;
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.duplicate_events;
        return;
    }
    order->venue_ns = cancel.transact_ns;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;

    switch (cancel.cancel_status) {
        case exchange::CancelStatus::Accepted:
            order->pending_request = RequestId::kInvalid;
            static_cast<void>(transition(*order, OrderState::Cancelled,
                                         OrderEventType::CancelAcknowledged, detail));
            release_deferred(order->logical_id);
            break;

        case exchange::CancelStatus::Rejected:
            // The order is still working. Assuming otherwise would silently
            // drop live exposure.
            order->pending_request = RequestId::kInvalid;
            static_cast<void>(transition(*order,
                                         order->cumulative_quantity.is_positive()
                                             ? OrderState::PartiallyFilled
                                             : OrderState::Working,
                                         OrderEventType::CancelRejected, detail));
            break;

        case exchange::CancelStatus::TooLate:
            // The cancel lost the race to a fill. The final event sequence
            // decides the state, not which request we sent first.
            order->pending_request = RequestId::kInvalid;
            order->cumulative_quantity = order->original_quantity;
            static_cast<void>(transition(*order, OrderState::Filled, OrderEventType::Filled,
                                         detail));
            release_deferred(order->logical_id);
            break;

        case exchange::CancelStatus::OrderNotFound:
        case exchange::CancelStatus::Unknown:
            // The venue cannot say. Never read as cancelled.
            order->pending_request = RequestId::kInvalid;
            static_cast<void>(transition(*order, OrderState::Unknown,
                                         OrderEventType::OutcomeUnknown, detail));
            break;
    }
}

void OrderManager::apply_replace(const exchange::OrderReplaceEvent& replace) {
    OrderRecord* order = mutable_find(replace.original_client_order_id);
    if (order == nullptr) {
        ++metrics_.quarantined_events;
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.duplicate_events;
        return;
    }
    order->venue_ns = replace.transact_ns;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;

    if (replace.accepted) {
        // The swap happened: the pending parameters become live, and the
        // replacement's identity replaces the old one.
        const auto it = by_logical_id_.find(static_cast<std::uint64_t>(order->logical_id));
        if (!order->pending_client_order_id.empty() && it != by_logical_id_.end()) {
            // Erase only alongside the reinsert. An unsolicited replace event
            // -- one we have no pending identity for -- must not be able to
            // detach a live order from the index it is found by, which would
            // make every later event for it look like an orphan.
            by_client_id_.erase(order->client_order_id);
            order->client_order_id = order->pending_client_order_id;
            by_client_id_[order->client_order_id] = it->second;
        }
        order->price = order->pending_price;
        order->original_quantity = order->pending_quantity;
        if (!replace.new_exchange_order_id.empty() && it != by_logical_id_.end()) {
            by_exchange_id_.erase(order->exchange_order_id);
            order->exchange_order_id = replace.new_exchange_order_id;
            by_exchange_id_[order->exchange_order_id] = it->second;
        }
        order->pending_client_order_id.clear();
        order->pending_request = RequestId::kInvalid;
        static_cast<void>(transition(*order,
                                     order->cumulative_quantity.is_positive()
                                         ? OrderState::PartiallyFilled
                                         : OrderState::Working,
                                     OrderEventType::ReplaceAcknowledged, detail));
        return;
    }

    order->pending_request = RequestId::kInvalid;
    order->pending_client_order_id.clear();
    if (replace.error.outcome == exchange::RequestOutcome::Unknown) {
        // We do not know whether the swap happened.
        static_cast<void>(transition(*order, OrderState::Unknown, OrderEventType::OutcomeUnknown,
                                     detail));
        return;
    }
    // Refused: the original is untouched and still working.
    static_cast<void>(transition(*order,
                                 order->cumulative_quantity.is_positive()
                                     ? OrderState::PartiallyFilled
                                     : OrderState::Working,
                                 OrderEventType::ReplaceRejected, detail));
}

void OrderManager::apply_failure(const exchange::RequestFailureEvent& failure) {
    OrderRecord* order = mutable_find(failure.client_order_id);
    if (order == nullptr) {
        ++metrics_.quarantined_events;
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.duplicate_events;
        return;
    }
    order->pending_request = RequestId::kInvalid;

    OrderJournalRecord detail;
    detail.logical_id = order->logical_id;

    if (failure.error.outcome == exchange::RequestOutcome::NotSent) {
        // Definitively never left this process, so the order cannot exist.
        static_cast<void>(transition(*order, OrderState::Rejected, OrderEventType::Rejected,
                                     detail));
        release_deferred(order->logical_id);
        return;
    }
    // Unknown: the request may or may not have created or changed an order.
    // Never read as cancelled, filled or rejected.
    static_cast<void>(transition(*order, OrderState::Unknown, OrderEventType::OutcomeUnknown,
                                 detail));
}

void OrderManager::release_deferred(LogicalOrderId resolved) {
    if (deferred_count_ == 0) {
        return;
    }
    for (DeferredNew& slot : deferred_) {
        if (!slot.present || slot.waiting_on != resolved) {
            continue;
        }
        const OrderAction action = slot.action;
        slot.present = false;
        slot.waiting_on = LogicalOrderId::kInvalid;
        --deferred_count_;
        // The cancellation boundary has been crossed; only now may the
        // replacement exist.
        static_cast<void>(handle_new(action));
    }
}

// ------------------------------------------------------------------ timeout

void OrderManager::on_timer() {
    const Nanos now = clock_.steady();
    for (OrderRecord& order : orders_) {
        if (!order.has_pending_request() || order.pending_since_ns == 0) {
            continue;
        }
        Nanos limit = config_.new_request_timeout_ns;
        if (order.state == OrderState::PendingCancel) {
            limit = config_.cancel_request_timeout_ns;
        } else if (order.state == OrderState::PendingReplace) {
            limit = config_.replace_request_timeout_ns;
        }
        if ((now - order.pending_since_ns) < limit) {
            continue;
        }
        ++metrics_.timeouts;
        order.pending_request = RequestId::kInvalid;
        OrderJournalRecord detail;
        detail.logical_id = order.logical_id;
        // A timeout tells us only that we do not know. Marking the order
        // cancelled or rejected here would be fabricating an outcome nobody
        // observed.
        static_cast<void>(transition(order, OrderState::Unknown,
                                     OrderEventType::RequestTimedOut, detail));
    }
}

// ------------------------------------------------------------------- views

quote::WorkingQuoteState OrderManager::working_state(const Symbol& symbol,
                                                     const StrategyName& owner) const {
    quote::WorkingQuoteState state;
    for (const OrderRecord& order : orders_) {
        if (order.symbol != symbol || !order.consumes_exposure()) {
            continue;
        }
        if (!order.ownership.matches(owner, order.ownership.slot)) {
            // Somebody else's order on this symbol. Counted so the quote
            // manager knows it exists; never presented as one of its slots.
            ++state.foreign_order_count;
            continue;
        }
        quote::WorkingOrder& slot = state.slot(order.ownership.slot);
        slot.present = true;
        slot.owner.strategy = order.ownership.strategy;
        slot.owner.slot = order.ownership.slot;
        slot.side = order.side;
        slot.price = order.price;
        slot.original_quantity = order.original_quantity;
        slot.cumulative_quantity = order.cumulative_quantity;
        slot.client_order_id = order.client_order_id;
        slot.exchange_order_id = order.exchange_order_id;
        slot.generation = order.ownership.generation;
        slot.last_action_ns = order.sent_ns;

        switch (order.state) {
            case OrderState::PendingNew:
                slot.status = exchange::OrderStatus::New;
                slot.pending = quote::PendingOperation::New;
                break;
            case OrderState::PendingCancel:
                slot.status = order.cumulative_quantity.is_positive()
                                  ? exchange::OrderStatus::PartiallyFilled
                                  : exchange::OrderStatus::New;
                slot.pending = quote::PendingOperation::Cancel;
                break;
            case OrderState::PendingReplace:
                slot.status = order.cumulative_quantity.is_positive()
                                  ? exchange::OrderStatus::PartiallyFilled
                                  : exchange::OrderStatus::New;
                slot.pending = quote::PendingOperation::Replace;
                break;
            case OrderState::PartiallyFilled:
                slot.status = exchange::OrderStatus::PartiallyFilled;
                break;
            case OrderState::Working:
                slot.status = exchange::OrderStatus::New;
                break;
            case OrderState::Unknown:
                // Propagated as unknown so the quote manager freezes the slot
                // rather than quoting alongside something that may be live.
                slot.status = exchange::OrderStatus::Unknown;
                break;
            default:
                slot.status = exchange::OrderStatus::Unknown;
                break;
        }
    }
    return state;
}

risk::ExposureSnapshot OrderManager::exposure(const Symbol& symbol) const {
    risk::ExposureSnapshot snapshot;
    snapshot.determinate = true;

    for (const OrderRecord& order : orders_) {
        if (order.symbol != symbol || !order.consumes_exposure()) {
            continue;
        }
        if (order.state == OrderState::Unknown) {
            // Exposure cannot be established while an order's fate is unknown,
            // and risk must not add to an amount it cannot measure.
            snapshot.determinate = false;
            ++snapshot.indeterminate_order_count;
            continue;
        }

        // Pending-new contributes its full size: it may already be working at
        // the venue. Everything else contributes what is left to fill --
        // including a pending cancel, because a cancel is a request, not a
        // result.
        const Qty contribution =
            (order.state == OrderState::PendingNew) ? order.original_quantity : order.remaining();
        if (!contribution.is_positive()) {
            continue;
        }
        Qty& side = (order.side == Side::Buy) ? snapshot.working_buy : snapshot.working_sell;
        if (!checked_add(side, contribution, side)) {
            snapshot.determinate = false;
            continue;
        }
        Notional value;
        if (checked_notional_of(order.price, contribution, value)) {
            Notional& side_value = (order.side == Side::Buy) ? snapshot.working_buy_notional
                                                             : snapshot.working_sell_notional;
            static_cast<void>(checked_add(side_value, value, side_value));
        }
        ++snapshot.open_order_count;
    }
    return snapshot;
}

// ----------------------------------------------------------- reconciliation

ReconciliationReport OrderManager::reconcile(const VenueSnapshot& snapshot) {
    ReconciliationReport report;
    report.performed_ns = clock_.steady();

    if (!snapshot.is_complete) {
        // Concluding "the venue does not have this order" from a partial
        // listing would cancel-or-forget live orders. Refusing is counted
        // separately so a query that never returns complete results is
        // visible rather than looking like a run of clean reconciliations.
        ++metrics_.reconciliations_refused;
        return report;
    }
    ++metrics_.reconciliations;

    // A snapshot the venue built before it acknowledged an order legitimately
    // omits that order. Comparing against when we learned of the listing is
    // what separates a real divergence from that race.
    const Nanos snapshot_ns = snapshot.received_ns != 0 ? snapshot.received_ns
                                                        : report.performed_ns;

    std::vector<bool> matched(orders_.size(), false);

    for (const exchange::OrderStatusReport& venue_order : snapshot.orders) {
        ++report.orders_compared;
        const OrderRecord* local = find(venue_order.client_order_id);
        if (local == nullptr && !venue_order.exchange_order_id.empty()) {
            local = find_by_exchange_id(venue_order.exchange_order_id);
        }

        if (local == nullptr) {
            // The venue has an order we do not know. Never assumed to be ours:
            // it may belong to an operator, another process, or a previous run.
            Discrepancy d;
            d.kind = DiscrepancyKind::OrphanAtVenue;
            d.client_order_id = venue_order.client_order_id;
            d.exchange_order_id = venue_order.exchange_order_id;
            d.symbol = venue_order.symbol;
            d.venue_status = venue_order.status;
            d.venue_cumulative = venue_order.cumulative_qty;
            d.venue_price = venue_order.price;
            std::uint64_t session = 0;
            std::uint64_t sequence = 0;
            // Our own id scheme is what distinguishes our orphan from
            // somebody else's order.
            d.identity_is_ours = ClientOrderIdGenerator::parse(
                venue_order.client_order_id, config_.client_id_prefix.view(), session, sequence);
            report.discrepancies.push_back(d);
            ++metrics_.orphan_orders;
            continue;
        }

        const auto it = by_logical_id_.find(static_cast<std::uint64_t>(local->logical_id));
        if (it != by_logical_id_.end() && it->second < matched.size()) {
            matched[it->second] = true;
        }

        Discrepancy d;
        d.logical_id = local->logical_id;
        d.client_order_id = local->client_order_id;
        d.exchange_order_id = local->exchange_order_id;
        d.symbol = local->symbol;
        d.local_state = local->state;
        d.venue_status = venue_order.status;
        d.local_cumulative = local->cumulative_quantity;
        d.venue_cumulative = venue_order.cumulative_qty;
        d.local_price = local->price;
        d.venue_price = venue_order.price;
        d.identity_is_ours = true;

        if (local->cumulative_quantity != venue_order.cumulative_qty) {
            d.kind = DiscrepancyKind::QuantityMismatch;
            report.discrepancies.push_back(d);
        } else if (local->price != venue_order.price) {
            d.kind = DiscrepancyKind::PriceMismatch;
            report.discrepancies.push_back(d);
        } else if (from_venue_status(venue_order.status) != local->state &&
                   local->state != OrderState::PendingCancel &&
                   local->state != OrderState::PendingReplace) {
            // A pending request legitimately disagrees with the venue's view
            // until it resolves; that is not a discrepancy.
            d.kind = DiscrepancyKind::StatusMismatch;
            report.discrepancies.push_back(d);
        } else {
            ++report.orders_agreed;
        }
    }

    // Orders we believe are live that the complete listing does not contain.
    std::vector<LogicalOrderId> missing;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        const OrderRecord& order = orders_[i];
        if (matched[i] || !order.is_live()) {
            continue;
        }
        if (!snapshot.symbol_filter.empty() && order.symbol != snapshot.symbol_filter) {
            continue;
        }
        // Not assumed cancelled. A snapshot can race an acknowledgement, and
        // "not in this listing" is weaker evidence than it looks.
        Discrepancy d;
        d.kind = DiscrepancyKind::MissingAtVenue;
        d.logical_id = order.logical_id;
        d.client_order_id = order.client_order_id;
        d.exchange_order_id = order.exchange_order_id;
        d.symbol = order.symbol;
        d.local_state = order.state;
        d.local_cumulative = order.cumulative_quantity;
        d.local_price = order.price;
        d.identity_is_ours = true;
        report.discrepancies.push_back(d);
        ++metrics_.missing_orders;
        missing.push_back(order.logical_id);
    }

    // A complete listing that omits an order we believe is working contradicts
    // our own state. Continuing to assert Working would keep risk counting
    // exposure the venue says is not there, and would leave the slot occupied
    // forever. The order becomes Unknown -- not Cancelled, which would be
    // inventing an outcome nobody reported.
    for (const LogicalOrderId id : missing) {
        OrderRecord* order = mutable_find(id);
        if (order == nullptr || order->acknowledged_ns > snapshot_ns) {
            continue;
        }
        ++metrics_.unknown_states;
        OrderJournalRecord detail;
        detail.logical_id = order->logical_id;
        static_cast<void>(transition(*order, OrderState::Unknown,
                                     OrderEventType::ReconciliationDetected, detail));
    }

    for (const Discrepancy& d : report.discrepancies) {
        if (d.logical_id == LogicalOrderId::kInvalid) {
            continue;
        }
        OrderRecord* order = mutable_find(d.logical_id);
        if (order != nullptr && !order->is_terminal()) {
            OrderJournalRecord detail;
            detail.logical_id = order->logical_id;
            journal(OrderEventType::ReconciliationDetected, *order, order->state, order->state);
        }
    }
    return report;
}

Status OrderManager::resolve(LogicalOrderId id, OrderState resolved_state) {
    OrderRecord* order = mutable_find(id);
    if (order == nullptr) {
        return {ErrorCode::NotFound, "no such order"};
    }
    const OrderState from = order->state;
    if (!is_legal_transition(from, resolved_state)) {
        return {ErrorCode::FailedPrecondition, "illegal reconciliation resolution"};
    }
    order->state = resolved_state;
    if (is_terminal(resolved_state)) {
        order->terminal_ns = clock_.steady();
        order->pending_request = RequestId::kInvalid;
    }
    journal(OrderEventType::ReconciliationResolved, *order, from, resolved_state);
    release_deferred(order->logical_id);
    return Status::ok();
}

// --------------------------------------------------------------- recovery

RecoveryState OrderManager::snapshot_for_recovery() const {
    RecoveryState state;
    state.session_id = config_.session_id;
    state.next_client_id_sequence = id_generator_.sequence();
    state.next_logical_id = next_logical_id_;
    state.next_request_id = next_request_id_;
    state.journal_sequence = journal_sequence_;
    state.orders = orders_;
    return state;
}

Status OrderManager::restore(const RecoveryState& state) {
    if (!orders_.empty()) {
        return {ErrorCode::FailedPrecondition, "restore into a non-empty OMS"};
    }
    if (state.orders.size() > config_.max_orders) {
        return {ErrorCode::OutOfRange, "recovery state exceeds configured capacity"};
    }

    orders_ = state.orders;
    // Assignment sizes the vector to exactly what was restored, discarding the
    // constructor's reservation. Without this, the first submit after a restart
    // would reallocate -- an allocation on the submit path, which is the one
    // thing the reservation exists to prevent.
    orders_.reserve(config_.max_orders);
    next_logical_id_ = state.next_logical_id;
    next_request_id_ = state.next_request_id;
    journal_sequence_ = state.journal_sequence;
    // Reissuing a client order id a previous life already used would collide
    // with an order that may still be resting.
    id_generator_.restore(state.next_client_id_sequence);

    by_client_id_.clear();
    by_exchange_id_.clear();
    by_logical_id_.clear();
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        OrderRecord& order = orders_[i];
        by_logical_id_[static_cast<std::uint64_t>(order.logical_id)] = i;
        if (!order.client_order_id.empty()) {
            by_client_id_[order.client_order_id] = i;
        }
        if (!order.exchange_order_id.empty()) {
            by_exchange_id_[order.exchange_order_id] = i;
        }
        // A request that was in flight across a restart has an outcome nobody
        // observed. It is Unknown, not cancelled and not rejected.
        if (has_pending_request(order.state)) {
            // Journalled with the state the order was actually in, not with the
            // one we are about to put it in: a record that says Unknown ->
            // Unknown has lost the only thing it was written to preserve.
            const OrderState was = order.state;
            order.state = OrderState::Unknown;
            order.pending_request = RequestId::kInvalid;
            order.pending_since_ns = 0;
            ++metrics_.unknown_states;
            journal(OrderEventType::OutcomeUnknown, order, was, OrderState::Unknown);
        }
    }
    return Status::ok();
}

}  // namespace mm::oms
