#include "mm/exchange/mock/MockExchangeExecution.hpp"

#include <algorithm>

namespace mm::exchange::mock {
namespace {

ExecutionEvent make_event(ExecutionEventType type) {
    ExecutionEvent e;
    e.type = type;
    return e;
}

}  // namespace

ExchangeCapabilities permissive_mock_capabilities() noexcept {
    ExchangeCapabilities c;
    c.supports_post_only = true;
    c.supports_replace = true;
    c.supports_reduce_only = true;
    c.supports_client_order_id = true;
    c.supports_mass_cancel = true;
    c.supports_order_query = true;
    c.supports_native_bbo = true;
    c.supports_orderbook_snapshot = true;
    c.supports_incremental_depth = true;
    c.supports_trade_stream = true;
    c.supports_balance_stream = true;
    c.supports_position_stream = true;
    c.has_positions = true;
    c.max_client_order_id_len = 36;
    return c;
}

ExchangeCapabilities minimal_mock_capabilities() noexcept {
    ExchangeCapabilities c;
    c.supports_post_only = false;
    c.supports_replace = false;
    c.supports_reduce_only = false;
    c.supports_client_order_id = true;
    c.supports_mass_cancel = false;
    c.supports_order_query = true;
    c.supports_native_bbo = false;
    c.supports_orderbook_snapshot = true;
    c.supports_incremental_depth = true;
    c.supports_trade_stream = true;
    c.supports_balance_stream = false;
    c.supports_position_stream = false;
    c.has_positions = false;
    c.max_client_order_id_len = 36;
    return c;
}

MockExchangeExecution::MockExchangeExecution(ManualClock& clock, ExchangeCapabilities caps)
    : clock_(clock), caps_(caps) {}

MockExchangeExecution::MockExchangeExecution(ManualClock& clock)
    : MockExchangeExecution(clock, permissive_mock_capabilities()) {}

MockExchangeExecution::~MockExchangeExecution() = default;

Status MockExchangeExecution::start(IExecutionSink& sink) {
    sink_ = &sink;
    started_ = true;
    connection_ = ConnectionState::Connected;
    authenticated_ = true;

    ExecutionEvent e = make_event(ExecutionEventType::Connection);
    e.payload.connection.venue = venue();
    e.payload.connection.from = ConnectionState::Disconnected;
    e.payload.connection.to = ConnectionState::Connected;
    emit_now(e);
    return Status::ok();
}

void MockExchangeExecution::stop() {
    // Deliberately does NOT cancel resting orders: closing a socket is not a
    // trading decision, and a venue does not forget orders because we
    // disconnected.
    started_ = false;
    sink_ = nullptr;
    connection_ = ConnectionState::Disconnected;
    authenticated_ = false;
    queue_.clear();
}

void MockExchangeExecution::stamp(ExecutionEvent& event) const {
    const Nanos now = clock_.steady();
    event.stamps.exchange_ns = clock_.wall();
    event.stamps.recv_ns = now;
    event.stamps.parse_ns = now;
    event.stamps.process_ns = now;
}

void MockExchangeExecution::emit_now(const ExecutionEvent& event) {
    if (sink_ == nullptr) {
        return;
    }
    ExecutionEvent copy = event;
    stamp(copy);
    last_event_ = copy;
    has_last_event_ = true;
    sink_->on_execution(copy);
}

void MockExchangeExecution::schedule(Nanos delay, const ExecutionEvent& event) {
    Scheduled s;
    s.due_ns = clock_.steady() + delay;
    s.ordinal = ordinal_++;
    s.event = event;
    queue_.push_back(s);
}

ExchangeOrderId MockExchangeExecution::next_exchange_id() {
    ExchangeOrderId id;
    static_cast<void>(id.assign("EX-" + std::to_string(next_exchange_id_++)));
    return id;
}

TradeId MockExchangeExecution::next_trade_id() {
    TradeId id;
    static_cast<void>(id.assign("TR-" + std::to_string(next_trade_id_++)));
    return id;
}

SubmitBehaviour MockExchangeExecution::take_submit_behaviour() {
    if (!scripted_submits_.empty()) {
        const SubmitBehaviour b = scripted_submits_.front();
        scripted_submits_.pop_front();
        return b;
    }
    return default_submit_;
}

CancelBehaviour MockExchangeExecution::take_cancel_behaviour() {
    if (!scripted_cancels_.empty()) {
        const CancelBehaviour b = scripted_cancels_.front();
        scripted_cancels_.pop_front();
        return b;
    }
    return default_cancel_;
}

Status MockExchangeExecution::submit(const OrderRequest& request) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "execution adapter not started"};
    }
    // Refused locally: nothing was sent, so no order can exist and there is
    // nothing to reconcile. This is the NotSent half of the safety distinction.
    if (connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "not connected"};
    }
    if (!authenticated_) {
        return {ErrorCode::PermissionDenied, "not authenticated"};
    }
    if (request.client_order_id.empty()) {
        return {ErrorCode::InvalidArgument, "client order id is empty"};
    }
    if (orders_.find(request.client_order_id) != orders_.end()) {
        return {ErrorCode::AlreadyExists, "duplicate client order id"};
    }

    ++received_submits_;
    pending_.push_back(request.client_order_id);

    const SubmitBehaviour behaviour = take_submit_behaviour();

    if (behaviour == SubmitBehaviour::Reject) {
        ExecutionEvent e = make_event(ExecutionEventType::OrderReject);
        e.payload.reject.client_order_id = request.client_order_id;
        e.payload.reject.symbol = request.symbol;
        e.payload.reject.symbol_id = request.symbol_id;
        e.payload.reject.trace = request.trace;
        e.payload.reject.seq = ++seq_;
        e.payload.reject.error =
            ExchangeError::make(ExchangeErrorCategory::ExchangeRejection,
                                RequestOutcome::Rejected, "scripted reject", reject_reason_);
        schedule(ack_latency_, e);
        return Status::ok();
    }

    if (behaviour == SubmitBehaviour::Timeout || behaviour == SubmitBehaviour::TransportLoss) {
        // The critical path: the request WAS sent and no verdict ever arrives.
        // The failure must reach the OMS as Unknown, never as a rejection.
        const bool is_timeout = (behaviour == SubmitBehaviour::Timeout);
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::Submit;
        e.payload.failure.client_order_id = request.client_order_id;
        e.payload.failure.symbol = request.symbol;
        e.payload.failure.symbol_id = request.symbol_id;
        e.payload.failure.trace = request.trace;
        e.payload.failure.error = ExchangeError::make(
            is_timeout ? ExchangeErrorCategory::Timeout : ExchangeErrorCategory::Transport,
            RequestOutcome::Unknown,
            is_timeout ? "no response before the deadline" : "socket closed mid-request");
        schedule(is_timeout ? ack_timeout_ : ack_latency_, e);
        return Status::ok();
    }

    // Accepted paths. The venue now holds the order.
    MockOrder order;
    order.request = request;
    order.exchange_id = next_exchange_id();
    order.status = OrderStatus::New;
    order.seq = ++seq_;
    orders_.emplace(request.client_order_id, order);

    ExecutionEvent ack = make_event(ExecutionEventType::OrderAck);
    ack.payload.ack.client_order_id = request.client_order_id;
    ack.payload.ack.exchange_order_id = order.exchange_id;
    ack.payload.ack.symbol = request.symbol;
    ack.payload.ack.symbol_id = request.symbol_id;
    ack.payload.ack.side = request.side;
    ack.payload.ack.type = request.type;
    ack.payload.ack.tif = request.tif;
    ack.payload.ack.price = request.price;
    ack.payload.ack.original_qty = request.quantity;
    ack.payload.ack.status = OrderStatus::New;
    ack.payload.ack.exec_type = ExecutionType::New;
    ack.payload.ack.trace = request.trace;
    ack.payload.ack.seq = order.seq;
    ack.payload.ack.transact_ns = clock_.wall();
    schedule(ack_latency_, ack);

    if (behaviour == SubmitBehaviour::AckThenFullFill ||
        behaviour == SubmitBehaviour::AckThenPartialFill) {
        const Qty fill_qty = (behaviour == SubmitBehaviour::AckThenFullFill)
                                 ? request.quantity
                                 : request.quantity / 2;
        ExecutionEvent fill = make_event(ExecutionEventType::Fill);
        fill.payload.fill.client_order_id = request.client_order_id;
        fill.payload.fill.exchange_order_id = order.exchange_id;
        fill.payload.fill.trade_id = next_trade_id();
        fill.payload.fill.symbol = request.symbol;
        fill.payload.fill.symbol_id = request.symbol_id;
        fill.payload.fill.side = request.side;
        fill.payload.fill.price = request.price;
        fill.payload.fill.quantity = fill_qty;
        fill.payload.fill.cumulative_qty = fill_qty;
        fill.payload.fill.leaves_qty = request.quantity - fill_qty;
        fill.payload.fill.liquidity = Liquidity::Maker;
        fill.payload.fill.order_status = (fill_qty == request.quantity)
                                             ? OrderStatus::Filled
                                             : OrderStatus::PartiallyFilled;
        fill.payload.fill.trace = request.trace;
        fill.payload.fill.seq = ++seq_;
        fill.payload.fill.transact_ns = clock_.wall();
        schedule(ack_latency_ * 2, fill);
    }
    return Status::ok();
}

Status MockExchangeExecution::cancel(const CancelRequest& request) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "execution adapter not started"};
    }
    if (connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "not connected"};
    }
    if (!request.identifies_an_order()) {
        return {ErrorCode::InvalidArgument, "cancel identifies no order"};
    }

    const CancelBehaviour behaviour = take_cancel_behaviour();
    const auto it = orders_.find(request.client_order_id);

    if (behaviour == CancelBehaviour::Timeout) {
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::Cancel;
        e.payload.failure.client_order_id = request.client_order_id;
        e.payload.failure.symbol = request.symbol;
        e.payload.failure.trace = request.trace;
        e.payload.failure.error = ExchangeError::make(ExchangeErrorCategory::Timeout,
                                                      RequestOutcome::Unknown,
                                                      "cancel not answered before the deadline");
        schedule(ack_timeout_, e);
        return Status::ok();
    }

    ExecutionEvent e = make_event(ExecutionEventType::OrderCancel);
    OrderCancelEvent& c = e.payload.cancel;
    c.client_order_id = request.client_order_id;
    c.exchange_order_id = request.exchange_order_id;
    c.symbol = request.symbol;
    c.symbol_id = request.symbol_id;
    c.trace = request.trace;
    c.seq = ++seq_;
    c.transact_ns = clock_.wall();

    if (behaviour == CancelBehaviour::NotFound || it == orders_.end()) {
        c.cancel_status = CancelStatus::OrderNotFound;
        c.order_status = OrderStatus::Unknown;
        c.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                      RequestOutcome::Unknown, "no such order");
    } else if (behaviour == CancelBehaviour::Reject) {
        // The order is still working. Reporting it terminal here would silently
        // drop live exposure, which is why validate_execution_event forbids it.
        c.cancel_status = CancelStatus::Rejected;
        c.order_status = it->second.status;
        c.cumulative_qty = it->second.cumulative;
        c.leaves_qty = it->second.request.quantity - it->second.cumulative;
        c.error = ExchangeError::make(ExchangeErrorCategory::ExchangeRejection,
                                      RequestOutcome::Rejected, "cancel refused");
    } else if (behaviour == CancelBehaviour::TooLate) {
        c.cancel_status = CancelStatus::TooLate;
        c.order_status = OrderStatus::Filled;
        c.cumulative_qty = it->second.request.quantity;
        c.leaves_qty = Qty::zero();
        it->second.status = OrderStatus::Filled;
        it->second.cumulative = it->second.request.quantity;
    } else {
        c.cancel_status = CancelStatus::Accepted;
        c.order_status = OrderStatus::Canceled;
        c.cumulative_qty = it->second.cumulative;
        c.leaves_qty = Qty::zero();
        it->second.status = OrderStatus::Canceled;
    }
    schedule(ack_latency_, e);
    return Status::ok();
}

Status MockExchangeExecution::replace(const ReplaceRequest& request) {
    if (!caps_.supports_replace) {
        return {ErrorCode::InvalidArgument, "venue does not support atomic replace"};
    }
    if (!started_ || connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "not connected"};
    }
    const auto it = orders_.find(request.original_client_order_id);
    if (it == orders_.end()) {
        ExecutionEvent e = make_event(ExecutionEventType::OrderReplace);
        e.payload.replace.original_client_order_id = request.original_client_order_id;
        e.payload.replace.new_client_order_id = request.new_client_order_id;
        e.payload.replace.symbol = request.symbol;
        e.payload.replace.accepted = false;
        e.payload.replace.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                                      RequestOutcome::Unknown, "no such order");
        schedule(ack_latency_, e);
        return Status::ok();
    }

    MockOrder replaced = it->second;
    replaced.request.client_order_id = request.new_client_order_id;
    replaced.request.price = request.new_price;
    replaced.request.quantity = request.new_quantity;
    replaced.exchange_id = next_exchange_id();
    replaced.status = OrderStatus::New;
    orders_.erase(it);
    orders_.emplace(request.new_client_order_id, replaced);

    ExecutionEvent e = make_event(ExecutionEventType::OrderReplace);
    e.payload.replace.original_client_order_id = request.original_client_order_id;
    e.payload.replace.new_client_order_id = request.new_client_order_id;
    e.payload.replace.new_exchange_order_id = replaced.exchange_id;
    e.payload.replace.symbol = request.symbol;
    e.payload.replace.symbol_id = request.symbol_id;
    e.payload.replace.accepted = true;
    e.payload.replace.new_status = OrderStatus::New;
    e.payload.replace.new_price = request.new_price;
    e.payload.replace.new_qty = request.new_quantity;
    e.payload.replace.cumulative_qty = replaced.cumulative;
    e.payload.replace.trace = request.trace;
    e.payload.replace.seq = ++seq_;
    e.payload.replace.transact_ns = clock_.wall();
    schedule(ack_latency_, e);
    return Status::ok();
}

Status MockExchangeExecution::cancel_all(const Symbol& symbol) {
    if (!caps_.supports_mass_cancel) {
        return {ErrorCode::InvalidArgument, "venue does not support mass cancel"};
    }
    if (!started_ || connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "not connected"};
    }
    for (auto& [id, order] : orders_) {
        if (!symbol.empty() && order.request.symbol != symbol) {
            continue;
        }
        if (is_terminal(order.status)) {
            continue;
        }
        order.status = OrderStatus::Canceled;

        ExecutionEvent e = make_event(ExecutionEventType::OrderCancel);
        e.payload.cancel.client_order_id = id;
        e.payload.cancel.exchange_order_id = order.exchange_id;
        e.payload.cancel.symbol = order.request.symbol;
        e.payload.cancel.cancel_status = CancelStatus::Accepted;
        e.payload.cancel.order_status = OrderStatus::Canceled;
        e.payload.cancel.cumulative_qty = order.cumulative;
        e.payload.cancel.seq = ++seq_;
        e.payload.cancel.transact_ns = clock_.wall();
        schedule(ack_latency_, e);
    }
    return Status::ok();
}

Status MockExchangeExecution::query_open_orders(const Symbol& symbol) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    std::vector<OrderStatusReport> live;
    for (const auto& [id, order] : orders_) {
        if (is_terminal(order.status)) {
            continue;
        }
        if (!symbol.empty() && order.request.symbol != symbol) {
            continue;
        }
        OrderStatusReport r;
        r.client_order_id = id;
        r.exchange_order_id = order.exchange_id;
        r.symbol = order.request.symbol;
        r.symbol_id = order.request.symbol_id;
        r.side = order.request.side;
        r.type = order.request.type;
        r.tif = order.request.tif;
        r.price = order.request.price;
        r.original_qty = order.request.quantity;
        r.cumulative_qty = order.cumulative;
        r.status = order.status;
        r.transact_ns = clock_.wall();
        live.push_back(r);
    }
    // Deterministic ordering: an unordered_map's iteration order is not stable
    // across runs, and a test that depends on it is a flaky test.
    std::sort(live.begin(), live.end(), [](const OrderStatusReport& a, const OrderStatusReport& b) {
        return a.client_order_id < b.client_order_id;
    });

    // Chunked, exactly as a real adapter must do for an unbounded reply.
    std::size_t index = 0;
    do {
        ExecutionEvent e = make_event(ExecutionEventType::OpenOrdersSnapshot);
        OpenOrdersSnapshotEvent& s = e.payload.open_orders;
        s.symbol_filter = symbol;
        s.is_first = (index == 0);
        std::uint8_t n = 0;
        while (index < live.size() && n < kMaxOrdersPerSnapshotEvent) {
            s.orders[n++] = live[index++];
        }
        s.count = n;
        s.is_last = (index >= live.size());
        schedule(ack_latency_, e);
    } while (index < live.size());
    return Status::ok();
}

Status MockExchangeExecution::query_order(const ClientOrderId& client_order_id,
                                          const ExchangeOrderId& exchange_order_id) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    const auto it = orders_.find(client_order_id);
    if (it == orders_.end()) {
        // The venue cannot say. This is Unknown, not "no such order" -- the
        // difference decides whether the OMS may forget the order.
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::QueryOrder;
        e.payload.failure.client_order_id = client_order_id;
        e.payload.failure.exchange_order_id = exchange_order_id;
        e.payload.failure.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                                      RequestOutcome::Unknown,
                                                      "venue has no record of this order");
        schedule(ack_latency_, e);
        return Status::ok();
    }

    ExecutionEvent e = make_event(ExecutionEventType::OrderStatus);
    OrderStatusReport& r = e.payload.order_status;
    r.client_order_id = client_order_id;
    r.exchange_order_id = it->second.exchange_id;
    r.symbol = it->second.request.symbol;
    r.side = it->second.request.side;
    r.type = it->second.request.type;
    r.tif = it->second.request.tif;
    r.price = it->second.request.price;
    r.original_qty = it->second.request.quantity;
    r.cumulative_qty = it->second.cumulative;
    r.status = it->second.status;
    r.transact_ns = clock_.wall();
    schedule(ack_latency_, e);
    return Status::ok();
}

Status MockExchangeExecution::query_balances() {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    return Status::ok();
}

Status MockExchangeExecution::query_positions() {
    if (!caps_.has_positions) {
        return {ErrorCode::InvalidArgument, "venue has no positions"};
    }
    return Status::ok();
}

void MockExchangeExecution::set_connected(bool connected) {
    if (connected) {
        reconnect();
    } else {
        disconnect("scripted disconnect");
    }
}

void MockExchangeExecution::disconnect(std::string_view reason) {
    const ConnectionState previous = connection_;
    connection_ = ConnectionState::Disconnected;
    authenticated_ = false;

    // Every in-flight request becomes Unknown. Not rejected: an order sent
    // moments before the socket died may well be resting on the venue right now.
    for (const ClientOrderId& id : pending_) {
        if (orders_.find(id) != orders_.end()) {
            continue;  // already resolved
        }
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::Submit;
        e.payload.failure.client_order_id = id;
        e.payload.failure.error = ExchangeError::make(ExchangeErrorCategory::ConnectionFailure,
                                                      RequestOutcome::Unknown,
                                                      "connection lost with request in flight");
        emit_now(e);
    }
    pending_.clear();

    ExecutionEvent e = make_event(ExecutionEventType::Connection);
    e.payload.connection.venue = venue();
    e.payload.connection.from = previous;
    e.payload.connection.to = ConnectionState::Disconnected;
    e.payload.connection.error =
        ExchangeError::make(ExchangeErrorCategory::ConnectionFailure, RequestOutcome::NotSent,
                            reason);
    emit_now(e);
}

void MockExchangeExecution::reconnect() {
    const ConnectionState previous = connection_;
    connection_ = ConnectionState::Connected;
    authenticated_ = true;

    ExecutionEvent e = make_event(ExecutionEventType::Connection);
    e.payload.connection.venue = venue();
    e.payload.connection.from = previous;
    e.payload.connection.to = ConnectionState::Connected;
    emit_now(e);
}

Status MockExchangeExecution::deliver_fill(const ClientOrderId& id, Px price, Qty quantity,
                                           Liquidity liquidity, const TradeId& trade_id) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return {ErrorCode::NotFound, "no such order"};
    }
    MockOrder& order = it->second;
    if (is_terminal(order.status)) {
        return {ErrorCode::FailedPrecondition, "order is already terminal"};
    }
    if (!quantity.is_positive()) {
        return {ErrorCode::InvalidArgument, "fill quantity must be positive"};
    }
    const Qty remaining = order.request.quantity - order.cumulative;
    if (quantity > remaining) {
        // Refusing here keeps tests from scripting a sequence no venue could
        // produce, which would prove nothing about the code under test.
        return {ErrorCode::OutOfRange, "fill exceeds the order's remaining quantity"};
    }

    order.cumulative += quantity;
    const bool complete = (order.cumulative == order.request.quantity);
    order.status = complete ? OrderStatus::Filled : OrderStatus::PartiallyFilled;

    ExecutionEvent e = make_event(ExecutionEventType::Fill);
    FillEvent& f = e.payload.fill;
    f.client_order_id = id;
    f.exchange_order_id = order.exchange_id;
    f.trade_id = trade_id.empty() ? next_trade_id() : trade_id;
    f.symbol = order.request.symbol;
    f.symbol_id = order.request.symbol_id;
    f.side = order.request.side;
    f.price = price;
    f.quantity = quantity;
    f.cumulative_qty = order.cumulative;
    f.leaves_qty = order.request.quantity - order.cumulative;
    f.liquidity = liquidity;
    f.order_status = order.status;
    f.trace = order.request.trace;
    f.seq = ++seq_;
    f.transact_ns = clock_.wall();
    emit_now(e);
    return Status::ok();
}

Status MockExchangeExecution::deliver_full_fill(const ClientOrderId& id, Liquidity liquidity,
                                                const TradeId& trade_id) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return {ErrorCode::NotFound, "no such order"};
    }
    const Qty remaining = it->second.request.quantity - it->second.cumulative;
    return deliver_fill(id, it->second.request.price, remaining, liquidity, trade_id);
}

Status MockExchangeExecution::redeliver_last_event() {
    if (!has_last_event_) {
        return {ErrorCode::FailedPrecondition, "no event has been delivered yet"};
    }
    if (sink_ == nullptr) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    // Byte-identical replay, exactly as a venue does after a stream reconnect.
    sink_->on_execution(last_event_);
    return Status::ok();
}

void MockExchangeExecution::deliver_balance(const Asset& asset, Qty free, Qty locked) {
    ExecutionEvent e = make_event(ExecutionEventType::BalanceUpdate);
    e.payload.balance.asset = asset;
    e.payload.balance.free = free;
    e.payload.balance.locked = locked;
    e.payload.balance.seq = ++seq_;
    e.payload.balance.transact_ns = clock_.wall();
    emit_now(e);
}

void MockExchangeExecution::deliver_position(const Symbol& symbol, Qty net_quantity,
                                             Px average_entry) {
    ExecutionEvent e = make_event(ExecutionEventType::PositionUpdate);
    e.payload.position.symbol = symbol;
    e.payload.position.net_quantity = net_quantity;
    e.payload.position.average_entry_price = average_entry;
    e.payload.position.seq = ++seq_;
    e.payload.position.transact_ns = clock_.wall();
    emit_now(e);
}

std::size_t MockExchangeExecution::pump() {
    if (sink_ == nullptr) {
        return 0;
    }
    const Nanos now = clock_.steady();

    std::vector<Scheduled> due;
    std::vector<Scheduled> remaining;
    due.reserve(queue_.size());
    remaining.reserve(queue_.size());
    for (const Scheduled& s : queue_) {
        (s.due_ns <= now ? due : remaining).push_back(s);
    }
    queue_.swap(remaining);

    // Due time first, insertion order second. Never anything that could vary
    // between runs.
    std::sort(due.begin(), due.end(), [](const Scheduled& a, const Scheduled& b) {
        return a.due_ns != b.due_ns ? a.due_ns < b.due_ns : a.ordinal < b.ordinal;
    });

    for (const Scheduled& s : due) {
        const ClientOrderId id = s.event.client_order_id();
        if (!id.empty()) {
            const auto it = std::find(pending_.begin(), pending_.end(), id);
            if (it != pending_.end()) {
                pending_.erase(it);
            }
        }
        emit_now(s.event);
    }
    return due.size();
}

std::size_t MockExchangeExecution::live_order_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [id, order] : orders_) {
        if (!is_terminal(order.status)) {
            ++n;
        }
    }
    return n;
}

bool MockExchangeExecution::has_order(const ClientOrderId& id) const {
    return orders_.find(id) != orders_.end();
}

OrderStatus MockExchangeExecution::order_status(const ClientOrderId& id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? OrderStatus::Unknown : it->second.status;
}

}  // namespace mm::exchange::mock
