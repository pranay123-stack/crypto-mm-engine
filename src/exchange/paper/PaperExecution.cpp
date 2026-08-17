#include "mm/exchange/paper/PaperExecution.hpp"

#include <algorithm>
#include <cstdio>

namespace mm::exchange::paper {

std::string_view to_string(FillModel m) noexcept {
    switch (m) {
        case FillModel::DisplayedLiquidity: return "DISPLAYED_LIQUIDITY";
        case FillModel::FullOnCross:        return "FULL_ON_CROSS";
    }
    return "UNKNOWN";
}

std::string_view to_string(PostOnlyPolicy p) noexcept {
    switch (p) {
        case PostOnlyPolicy::Reject: return "REJECT";
        case PostOnlyPolicy::Expire: return "EXPIRE";
    }
    return "UNKNOWN";
}

std::string_view to_string(ReplaceMode m) noexcept {
    switch (m) {
        case ReplaceMode::Atomic:      return "ATOMIC";
        case ReplaceMode::Unsupported: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

ExchangeCapabilities paper_capabilities(ReplaceMode mode) noexcept {
    ExchangeCapabilities c;
    c.supports_post_only = true;
    // Follows the configured mode, so taking atomic replace away is a config
    // change rather than a code change -- and the quote manager's
    // cancel-then-new decomposition becomes testable against a venue that
    // genuinely lacks it.
    c.supports_replace = (mode == ReplaceMode::Atomic);
    c.supports_reduce_only = false;
    c.supports_client_order_id = true;
    c.supports_mass_cancel = true;
    c.supports_order_query = true;
    c.supports_orderbook_snapshot = true;
    c.supports_incremental_depth = true;
    c.supports_trade_stream = true;
    c.supports_balance_stream = false;
    c.supports_position_stream = false;
    c.has_positions = false;
    c.max_client_order_id_len = 36;
    return c;
}

Result<PaperExecutionConfig> paper_config_from(const PaperConfig& yaml) {
    PaperExecutionConfig c;
    c.latency.request_ns = micros(yaml.request_latency_us);
    c.latency.ack_ns = micros(yaml.ack_latency_us);
    c.latency.cancel_ns = micros(yaml.cancel_latency_us);
    c.latency.replace_ns = micros(yaml.replace_latency_us);
    c.latency.fill_ns = micros(yaml.fill_latency_us);
    c.latency.query_ns = micros(yaml.query_latency_us);

    if (yaml.fill_model == "displayed_liquidity") {
        c.fill_model = FillModel::DisplayedLiquidity;
    } else if (yaml.fill_model == "full_on_cross") {
        c.fill_model = FillModel::FullOnCross;
    } else {
        return Status{ErrorCode::InvalidArgument, "unknown paper.fill_model: " + yaml.fill_model};
    }

    if (yaml.post_only_policy == "reject") {
        c.post_only_policy = PostOnlyPolicy::Reject;
    } else if (yaml.post_only_policy == "expire") {
        c.post_only_policy = PostOnlyPolicy::Expire;
    } else {
        return Status{ErrorCode::InvalidArgument,
                      "unknown paper.post_only_policy: " + yaml.post_only_policy};
    }

    if (yaml.replace_mode == "atomic") {
        c.replace_mode = ReplaceMode::Atomic;
    } else if (yaml.replace_mode == "unsupported") {
        c.replace_mode = ReplaceMode::Unsupported;
    } else {
        return Status{ErrorCode::InvalidArgument,
                      "unknown paper.replace_mode: " + yaml.replace_mode};
    }

    if (yaml.queue_share_bps < 0 || yaml.queue_share_bps > 10'000) {
        return Status{ErrorCode::InvalidArgument, "paper.queue_share_bps out of range"};
    }
    c.queue_share_bps = static_cast<std::uint32_t>(yaml.queue_share_bps);
    c.max_book_age_ns = millis(yaml.max_book_age_ms);
    if (yaml.max_orders <= 0) {
        return Status{ErrorCode::InvalidArgument, "paper.max_orders must be positive"};
    }
    c.max_orders = static_cast<std::size_t>(yaml.max_orders);
    c.deterministic_seed = yaml.deterministic_seed;
    if (yaml.maker_fee_bps < 0.0 || yaml.taker_fee_bps < 0.0) {
        return Status{ErrorCode::InvalidArgument, "paper fee rates must not be negative"};
    }
    c.maker_fee_bps = static_cast<std::int32_t>(yaml.maker_fee_bps);
    c.taker_fee_bps = static_cast<std::int32_t>(yaml.taker_fee_bps);
    // Faults are never configured from a file. Failure injection is a testing
    // instrument, and a config that could switch it on is a config that could
    // make a paper run silently unrepresentative.
    return c;
}

PaperExecution::PaperExecution(PaperExecutionConfig config, const Clock& clock)
    : config_(std::move(config)), clock_(clock), caps_(paper_capabilities(config_.replace_mode)) {
    orders_.reserve(config_.max_orders);
    pending_.reserve(config_.max_pending_events);
}

// ------------------------------------------------------------------ lifecycle

Status PaperExecution::start(IExecutionSink& sink) {
    sink_ = &sink;
    started_ = true;
    // Connecting, then Connected -- both delivered through the event path, so
    // the engine sees the same transition sequence a real adapter produces.
    set_connection(ConnectionState::Connecting);
    authenticated_ = true;
    set_connection(ConnectionState::Connected);
    return Status::ok();
}

void PaperExecution::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    in_flight_.clear();
    // Deliberately does NOT cancel resting orders. Closing a socket is not a
    // trading decision, and a venue does not forget your orders because you
    // disconnected. The engine cancels explicitly during shutdown.
    set_connection(ConnectionState::Disconnected);
    pending_.clear();
    sink_ = nullptr;
}

void PaperExecution::set_connection(ConnectionState state, ExchangeError error) {
    if (state == connection_) {
        return;
    }
    const ConnectionState from = connection_;
    connection_ = state;
    ++metrics_.connection_transitions;
    if (sink_ == nullptr) {
        return;
    }
    ExecutionEvent e = make_event(ExecutionEventType::Connection);
    e.payload.connection.venue = config_.venue;
    e.payload.connection.from = from;
    e.payload.connection.to = state;
    e.payload.connection.error = error;
    // Connection transitions are delivered immediately: they describe the
    // transport, not a venue decision, and queueing them behind order latency
    // would tell the engine it is connected after it had already timed out.
    emit(e);
}

// -------------------------------------------------------------------- helpers

Status PaperExecution::require_connected() const {
    if (!started_ || sink_ == nullptr) {
        return {ErrorCode::Unavailable, "paper execution not started"};
    }
    if (connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "paper venue not connected"};
    }
    if (!authenticated_) {
        return {ErrorCode::PermissionDenied, "paper venue not authenticated"};
    }
    return Status::ok();
}

bool PaperExecution::consume(std::uint32_t& counter) noexcept {
    if (counter == 0) {
        return false;
    }
    --counter;
    return true;
}

Nanos PaperExecution::take_request_delay() noexcept {
    Nanos extra = 0;
    if (config_.faults.delay_next_request_ns != 0) {
        extra = config_.faults.delay_next_request_ns;
        config_.faults.delay_next_request_ns = 0;
    }
    return extra;
}

ExecutionEvent PaperExecution::make_event(ExecutionEventType type) const {
    ExecutionEvent e;
    e.type = type;
    e.stamps.recv_ns = clock_.steady();
    e.stamps.parse_ns = clock_.steady();
    e.stamps.exchange_ns = clock_.wall();
    return e;
}

ExchangeOrderId PaperExecution::next_exchange_id() {
    // Deliberately unlike a client order id in both prefix and shape (§21). If
    // any code ever assumes the two are interchangeable, it fails here rather
    // than against a real venue.
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "PX%011llu",
                                static_cast<unsigned long long>(next_exchange_seq_++));
    ExchangeOrderId id;
    static_cast<void>(id.assign(std::string_view(buf, n > 0 ? static_cast<std::size_t>(n) : 0)));
    return id;
}

TradeId PaperExecution::next_trade_id() {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "PT%011llu",
                                static_cast<unsigned long long>(next_trade_seq_++));
    TradeId id;
    static_cast<void>(id.assign(std::string_view(buf, n > 0 ? static_cast<std::size_t>(n) : 0)));
    return id;
}

PaperOrder* PaperExecution::mutable_find(const ClientOrderId& id) {
    const auto it = by_client_id_.find(id);
    return it == by_client_id_.end() ? nullptr : &orders_[it->second];
}

const PaperOrder* PaperExecution::find_order(const ClientOrderId& id) const {
    const auto it = by_client_id_.find(id);
    return it == by_client_id_.end() ? nullptr : &orders_[it->second];
}

const PaperOrder* PaperExecution::find_order(const ExchangeOrderId& id) const {
    const auto it = by_exchange_id_.find(id);
    return it == by_exchange_id_.end() ? nullptr : &orders_[it->second];
}

std::size_t PaperExecution::resting_order_count() const {
    return static_cast<std::size_t>(std::count_if(
        orders_.begin(), orders_.end(), [](const PaperOrder& o) { return o.is_resting(); }));
}

// ---------------------------------------------------------------- event path

void PaperExecution::schedule(Nanos delay, ExecutionEvent event) {
    // While a request is being processed, its answer is dated from the moment
    // the request arrived at the venue. Outside that -- connection events, for
    // instance -- the current clock is the right base.
    const Nanos base = processing_arrival_ns_ != 0 ? processing_arrival_ns_ : clock_.steady();
    schedule_at(base + delay, std::move(event));
}

void PaperExecution::schedule_at(Nanos due_ns, ExecutionEvent event) {
    if (pending_.size() >= config_.max_pending_events) {
        // Never a silent drop: a full queue is a condition an operator must
        // see, and pretending the event was delivered would be worse than
        // losing it loudly.
        ++metrics_.events_dropped;
        return;
    }
    const Nanos now = clock_.steady();
    metrics_.simulated_latency_ns += (due_ns > now ? due_ns - now : 0);
    Scheduled s;
    s.due_ns = due_ns;
    s.ordinal = next_ordinal_++;
    s.event = event;
    pending_.push_back(s);

    if (consume(config_.faults.duplicate_next_event)) {
        // The same event twice, at the same instant. The engine's idempotency
        // is what must absorb this, not the venue's good manners.
        ++metrics_.duplicates_injected;
        Scheduled dup = s;
        dup.ordinal = next_ordinal_++;
        pending_.push_back(dup);
    }
}

void PaperExecution::emit(const ExecutionEvent& event) {
    if (sink_ == nullptr) {
        return;
    }
    ExecutionEvent copy = event;
    copy.stamps.process_ns = clock_.steady();
    ++metrics_.events_emitted;
    sink_->on_execution(copy);
}

void PaperExecution::resolve_pending_cancel(ExecutionEvent& event) {
    PaperOrder* order = mutable_find(event.payload.cancel.client_order_id);
    if (order == nullptr) {
        event.payload.cancel.cancel_status = CancelStatus::OrderNotFound;
        event.payload.cancel.order_status = OrderStatus::Unknown;
        ++metrics_.cancel_rejects;
        return;
    }
    order->cancel_pending = false;

    if (order->status == OrderStatus::Filled) {
        // Filled while the cancel was in flight.
        ++metrics_.cancel_rejects;
        event.payload.cancel.cancel_status = CancelStatus::TooLate;
        event.payload.cancel.order_status = OrderStatus::Filled;
        event.payload.cancel.cumulative_qty = order->cumulative_qty;
        event.payload.cancel.leaves_qty = Qty{};
        return;
    }
    if (order->is_terminal()) {
        ++metrics_.cancel_rejects;
        event.payload.cancel.cancel_status = CancelStatus::OrderNotFound;
        event.payload.cancel.order_status = order->status;
        return;
    }

    // A partial fill in the window is not a lost race: the remainder really is
    // cancelled, and the cumulative quantity reported here is what the engine
    // must reconcile against.
    order->status = OrderStatus::Canceled;
    order->last_event_ns = clock_.steady();
    ++metrics_.cancel_acks;
    event.payload.cancel.cancel_status = CancelStatus::Accepted;
    event.payload.cancel.order_status = OrderStatus::Canceled;
    event.payload.cancel.cumulative_qty = order->cumulative_qty;
    event.payload.cancel.leaves_qty = order->remaining();
}

std::size_t PaperExecution::poll() {
    if (sink_ == nullptr) {
        return 0;
    }
    const Nanos now = clock_.steady();

    // Requests reach the venue before their answers leave it. Processing them
    // here -- rather than inside submit() -- is what makes `request_ns` mean
    // what it says, and is why an order cannot fill in the window before the
    // venue has received it.
    std::vector<PendingRequest> arrived;
    std::vector<PendingRequest> still_flying;
    arrived.reserve(in_flight_.size());
    still_flying.reserve(in_flight_.size());
    for (const PendingRequest& r : in_flight_) {
        (r.due_ns <= now ? arrived : still_flying).push_back(r);
    }
    in_flight_.swap(still_flying);
    std::sort(arrived.begin(), arrived.end(), [](const PendingRequest& a, const PendingRequest& b) {
        return a.due_ns != b.due_ns ? a.due_ns < b.due_ns : a.ordinal < b.ordinal;
    });
    for (const PendingRequest& r : arrived) {
        process_request(r);
    }

    std::vector<Scheduled> due;
    std::vector<Scheduled> remaining;
    due.reserve(pending_.size());
    remaining.reserve(pending_.size());
    for (const Scheduled& s : pending_) {
        (s.due_ns <= now ? due : remaining).push_back(s);
    }
    pending_.swap(remaining);

    // Due time first, emission order second. Nothing that could vary between
    // runs -- this ordering is what §31's determinism rests on.
    std::sort(due.begin(), due.end(), [](const Scheduled& a, const Scheduled& b) {
        return a.due_ns != b.due_ns ? a.due_ns < b.due_ns : a.ordinal < b.ordinal;
    });

    // Deterministic out-of-order delivery (§18). Swaps the last due event ahead
    // of the one before it, so the engine receives them in an order the venue
    // did not produce. Real venues do this whenever two streams race, and the
    // engine's sequence checks -- not the venue's good manners -- are what must
    // absorb it. Deterministic: always the same pair, never a shuffle.
    while (due.size() >= 2 && consume(config_.faults.reorder_next_event)) {
        std::swap(due[due.size() - 2], due[due.size() - 1]);
        ++metrics_.reorders_injected;
    }

    for (Scheduled& s : due) {
        // A cancel is applied at the instant it is confirmed, not when it was
        // requested. If the market filled the order in that window the cancel
        // lost the race, and the venue says so rather than reporting a
        // cancellation that did not happen (§15, invariant 6).
        if (s.event.type == ExecutionEventType::OrderCancel &&
            s.event.payload.cancel.cancel_status == CancelStatus::Accepted &&
            s.event.payload.cancel.order_status == OrderStatus::Canceled) {
            resolve_pending_cancel(s.event);
        }
        emit(s.event);
    }
    return due.size();
}

// -------------------------------------------------------------- order entry

Status PaperExecution::submit(const OrderRequest& request) {
    MM_RETURN_IF_ERROR(require_connected());
    if (orders_.size() >= config_.max_orders) {
        return {ErrorCode::Overflow, "paper venue order capacity reached"};
    }
    if (request.client_order_id.empty()) {
        return {ErrorCode::InvalidArgument, "client order id required"};
    }
    if (request.client_order_id.size() > caps_.max_client_order_id_len) {
        return {ErrorCode::InvalidArgument, "client order id too long for this venue"};
    }
    if (find_order(request.client_order_id) != nullptr) {
        // Invariant 3: one client identity, one order. A venue that quietly
        // accepted a second would make every subsequent event ambiguous.
        return {ErrorCode::AlreadyExists, "duplicate client order id"};
    }
    // Only LIMIT is implemented (§13). MARKET is not used by the quote manager
    // and inventing support would be untested surface.
    if (request.type != OrderType::Limit && request.type != OrderType::LimitMaker) {
        return {ErrorCode::FailedPrecondition, "paper venue implements LIMIT only"};
    }
    if (!request.price.is_positive() || !request.quantity.is_positive()) {
        return {ErrorCode::InvalidArgument, "price and quantity must be positive"};
    }

    ++metrics_.new_requests;

    if (consume(config_.faults.drop_next_request)) {
        // No answer at all, and no venue-side order either. The engine must
        // time out and conclude Unknown -- never that the order does not exist.
        ++metrics_.requests_dropped;
        return Status::ok();
    }

    PendingRequest p;
    p.kind = RequestKind::Submit;
    p.submit = request;
    p.decision_delay_ns = config_.latency.ack_ns;
    enqueue_request(p);
    return Status::ok();
}

void PaperExecution::enqueue_request(PendingRequest request) {
    request.due_ns = clock_.steady() + config_.latency.request_ns + take_request_delay();
    request.ordinal = next_ordinal_++;
    in_flight_.push_back(request);
}

void PaperExecution::process_request(const PendingRequest& request) {
    processing_arrival_ns_ = request.due_ns;
    switch (request.kind) {
        case RequestKind::Submit:
            process_submit(request.submit, request.decision_delay_ns);
            break;
        case RequestKind::Cancel:
            process_cancel(request.cancel, request.decision_delay_ns);
            break;
        case RequestKind::Replace:
            process_replace(request.replace, request.decision_delay_ns);
            break;
        default:
            break;
    }
    processing_arrival_ns_ = 0;
}

void PaperExecution::process_submit(const OrderRequest& request, Nanos decision_delay) {
    if (consume(config_.faults.error_next_request)) {
        ++metrics_.execution_errors;
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::Submit;
        e.payload.failure.client_order_id = request.client_order_id;
        e.payload.failure.symbol = request.symbol;
        e.payload.failure.error = ExchangeError::make(ExchangeErrorCategory::Transport,
                                                      RequestOutcome::Unknown,
                                                      "simulated execution error");
        e.payload.failure.trace = request.trace;
        schedule(decision_delay, e);
        return;
    }
    if (consume(config_.faults.reject_next_new)) {
        reject_order(request, RejectReason::InternalError, decision_delay);
        return;
    }
    if (find_order(request.client_order_id) != nullptr) {
        // Two requests with the same id were in flight together. Caught here
        // as well as at submit(), because the first only reaches venue state
        // when it is processed.
        reject_order(request, RejectReason::DuplicateClientOrderId, decision_delay);
        return;
    }
    accept_order(request, decision_delay);
}

void PaperExecution::reject_order(const OrderRequest& request, RejectReason reason, Nanos delay) {
    ++metrics_.new_rejects;
    ExecutionEvent e = make_event(ExecutionEventType::OrderReject);
    e.payload.reject.client_order_id = request.client_order_id;
    e.payload.reject.symbol = request.symbol;
    e.payload.reject.symbol_id = request.symbol_id;
    e.payload.reject.error =
        ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                            "paper venue rejected the order", reason);
    e.payload.reject.transact_ns = clock_.wall();
    e.payload.reject.seq = next_seq();
    e.payload.reject.trace = request.trace;
    schedule(delay, e);
}

void PaperExecution::accept_order(const OrderRequest& request, Nanos base_delay) {
    PaperOrder order;
    order.exchange_order_id = next_exchange_id();
    order.client_order_id = request.client_order_id;
    order.symbol = request.symbol;
    order.symbol_id = request.symbol_id;
    order.side = request.side;
    order.type = request.type;
    order.tif = request.tif;
    order.post_only = request.is_post_only();
    order.price = request.price;
    order.original_qty = request.quantity;
    order.status = OrderStatus::New;
    order.arrival_sequence = next_arrival_seq_++;
    order.accepted_ns = clock_.steady();
    order.last_event_ns = order.accepted_ns;

    // §12: marketable-on-arrival is decided explicitly, never left to fall out
    // of the matching path.
    const auto book_it = books_.find(request.symbol);
    const book::OrderBook* book = book_it == books_.end() ? nullptr : book_it->second;
    const bool marketable = book != nullptr && is_marketable_on_arrival(order, *book);

    if (marketable && order.post_only) {
        // A post-only order that would take is refused. Silently converting it
        // into a taker would have the strategy paying the spread it explicitly
        // asked not to pay, and no downstream record would show why.
        ++metrics_.post_only_rejections;
        if (config_.post_only_policy == PostOnlyPolicy::Reject) {
            reject_order(request, RejectReason::PostOnlyWouldCross, base_delay);
            return;
        }
        ++metrics_.expiries;
        ExecutionEvent e = make_event(ExecutionEventType::OrderCancel);
        e.payload.cancel.client_order_id = request.client_order_id;
        e.payload.cancel.exchange_order_id = order.exchange_order_id;
        e.payload.cancel.symbol = request.symbol;
        e.payload.cancel.cancel_status = CancelStatus::Accepted;
        e.payload.cancel.order_status = OrderStatus::Expired;
        e.payload.cancel.leaves_qty = Qty{};
        e.payload.cancel.transact_ns = clock_.wall();
        e.payload.cancel.seq = next_seq();
        e.payload.cancel.trace = request.trace;
        schedule(base_delay, e);
        return;
    }

    const std::size_t index = orders_.size();
    orders_.push_back(order);
    by_client_id_[order.client_order_id] = index;
    by_exchange_id_[order.exchange_order_id] = index;
    ++metrics_.new_acks;

    ExecutionEvent ack = make_event(ExecutionEventType::OrderAck);
    ack.payload.ack.client_order_id = order.client_order_id;
    ack.payload.ack.exchange_order_id = order.exchange_order_id;
    ack.payload.ack.symbol = order.symbol;
    ack.payload.ack.symbol_id = order.symbol_id;
    ack.payload.ack.side = order.side;
    ack.payload.ack.type = order.type;
    ack.payload.ack.tif = order.tif;
    ack.payload.ack.price = order.price;
    ack.payload.ack.original_qty = order.original_qty;
    ack.payload.ack.cumulative_qty = Qty{};
    ack.payload.ack.status = OrderStatus::New;
    ack.payload.ack.exec_type = ExecutionType::New;
    ack.payload.ack.transact_ns = clock_.wall();
    ack.payload.ack.seq = next_seq();
    ack.payload.ack.trace = request.trace;
    schedule(base_delay, ack);

    if (marketable && book != nullptr) {
        // Immediate taker execution, scheduled after the acknowledgement so
        // the engine sees them in the order a venue would send them.
        const MatchResult match =
            match_against_book(orders_[index], *book, config_.fill_model,
                               config_.queue_share_bps);
        if (match.quantity.is_positive()) {
            MatchResult taker = match;
            taker.liquidity = Liquidity::Taker;
            apply_match(orders_[index], taker, clock_.steady());
            schedule_fill(orders_[index], taker, base_delay + config_.latency.fill_ns);
        }
    }
}

// ------------------------------------------------------------------- cancel

Status PaperExecution::cancel(const CancelRequest& request) {
    // Cancellation stays available while degraded where the simulated
    // environment allows it (§19): refusing to cancel because connectivity is
    // imperfect traps exposure exactly when releasing it matters most. Only a
    // fully down transport refuses.
    if (!started_ || sink_ == nullptr) {
        return {ErrorCode::Unavailable, "paper execution not started"};
    }
    if (connection_ == ConnectionState::Disconnected || connection_ == ConnectionState::Failed) {
        return {ErrorCode::Unavailable, "paper venue not connected"};
    }
    if (!request.identifies_an_order()) {
        return {ErrorCode::InvalidArgument, "cancel identifies no order"};
    }

    ++metrics_.cancel_requests;

    if (consume(config_.faults.drop_next_request)) {
        ++metrics_.requests_dropped;
        return Status::ok();
    }

    PendingRequest p;
    p.kind = RequestKind::Cancel;
    p.cancel = request;
    p.decision_delay_ns = config_.latency.cancel_ns;
    enqueue_request(p);
    return Status::ok();
}

void PaperExecution::process_cancel(const CancelRequest& request, Nanos delay) {
    ExecutionEvent e = make_event(ExecutionEventType::OrderCancel);
    e.payload.cancel.client_order_id = request.client_order_id;
    e.payload.cancel.exchange_order_id = request.exchange_order_id;
    e.payload.cancel.symbol = request.symbol;
    e.payload.cancel.symbol_id = request.symbol_id;
    e.payload.cancel.transact_ns = clock_.wall();
    e.payload.cancel.seq = next_seq();
    e.payload.cancel.trace = request.trace;

    PaperOrder* order = mutable_find(request.client_order_id);
    if (order == nullptr && !request.exchange_order_id.empty()) {
        const auto it = by_exchange_id_.find(request.exchange_order_id);
        if (it != by_exchange_id_.end()) {
            order = &orders_[it->second];
        }
    }

    if (order == nullptr) {
        ++metrics_.cancel_rejects;
        e.payload.cancel.cancel_status = CancelStatus::OrderNotFound;
        e.payload.cancel.order_status = OrderStatus::Unknown;
        e.payload.cancel.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                                     RequestOutcome::Unknown, "no such order",
                                                     RejectReason::UnknownOrder);
        schedule(delay, e);
        return;
    }

    if (consume(config_.faults.reject_next_cancel)) {
        // A refused cancel leaves the order working. The venue says so
        // explicitly rather than staying silent, because silence would be
        // indistinguishable from a lost request.
        ++metrics_.cancel_rejects;
        e.payload.cancel.cancel_status = CancelStatus::Rejected;
        e.payload.cancel.order_status = order->status;
        e.payload.cancel.cumulative_qty = order->cumulative_qty;
        e.payload.cancel.leaves_qty = order->remaining();
        e.payload.cancel.error =
            ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                                "cancel refused");
        schedule(delay, e);
        return;
    }

    if (order->is_terminal()) {
        // Already finished. Whether it filled or was cancelled earlier, the
        // cancel is too late -- and which of those it was matters, so the
        // status is reported rather than flattened.
        ++metrics_.cancel_rejects;
        e.payload.cancel.cancel_status =
            order->status == OrderStatus::Filled ? CancelStatus::TooLate : CancelStatus::OrderNotFound;
        e.payload.cancel.order_status = order->status;
        e.payload.cancel.cumulative_qty = order->cumulative_qty;
        e.payload.cancel.leaves_qty = Qty{};
        schedule(delay, e);
        return;
    }

    // Accepted, but NOT yet applied. The order keeps resting for the duration
    // of the simulated latency, so a market move in that window can still fill
    // it -- the cancel/fill race the OMS has to survive (§15). The venue does
    // not guarantee cancellation just because it was asked.
    order->cancel_pending = true;
    e.payload.cancel.cancel_status = CancelStatus::Accepted;
    e.payload.cancel.order_status = OrderStatus::Canceled;
    e.payload.cancel.exchange_order_id = order->exchange_order_id;
    e.payload.cancel.client_order_id = order->client_order_id;
    e.payload.cancel.cumulative_qty = order->cumulative_qty;
    e.payload.cancel.leaves_qty = order->remaining();
    schedule(delay, e);
}

Status PaperExecution::cancel_all(const Symbol& symbol) {
    MM_RETURN_IF_ERROR(require_connected());
    for (PaperOrder& order : orders_) {
        if (!order.is_resting()) {
            continue;
        }
        if (!symbol.empty() && order.symbol != symbol) {
            continue;
        }
        CancelRequest request;
        request.client_order_id = order.client_order_id;
        request.exchange_order_id = order.exchange_order_id;
        request.symbol = order.symbol;
        MM_RETURN_IF_ERROR(cancel(request));
    }
    return Status::ok();
}

// ------------------------------------------------------------------ replace

Status PaperExecution::replace(const ReplaceRequest& request) {
    if (config_.replace_mode == ReplaceMode::Unsupported) {
        // Refused outright rather than decomposed here. The two have different
        // exposure profiles and that choice belongs to the quote manager.
        return {ErrorCode::InvalidArgument, "paper venue does not support atomic replace"};
    }
    MM_RETURN_IF_ERROR(require_connected());
    if (!request.new_price.is_positive() || !request.new_quantity.is_positive()) {
        return {ErrorCode::InvalidArgument, "replace price and quantity must be positive"};
    }
    if (request.new_client_order_id.empty()) {
        return {ErrorCode::InvalidArgument, "replace requires a new client order id"};
    }

    ++metrics_.replace_requests;

    if (consume(config_.faults.drop_next_request)) {
        ++metrics_.requests_dropped;
        return Status::ok();
    }

    PendingRequest p;
    p.kind = RequestKind::Replace;
    p.replace = request;
    p.decision_delay_ns = config_.latency.replace_ns;
    enqueue_request(p);
    return Status::ok();
}

void PaperExecution::process_replace(const ReplaceRequest& request, Nanos delay) {
    ExecutionEvent e = make_event(ExecutionEventType::OrderReplace);
    e.payload.replace.original_client_order_id = request.original_client_order_id;
    e.payload.replace.new_client_order_id = request.new_client_order_id;
    e.payload.replace.symbol = request.symbol;
    e.payload.replace.symbol_id = request.symbol_id;
    e.payload.replace.transact_ns = clock_.wall();
    e.payload.replace.seq = next_seq();
    e.payload.replace.trace = request.trace;

    PaperOrder* order = mutable_find(request.original_client_order_id);
    if (order == nullptr || !order->is_resting()) {
        ++metrics_.replace_rejects;
        e.payload.replace.accepted = false;
        e.payload.replace.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                                      RequestOutcome::Unknown, "no such order",
                                                      RejectReason::UnknownOrder);
        schedule(delay, e);
        return;
    }

    if (consume(config_.faults.reject_next_replace)) {
        // Invariant 8: the original keeps every parameter it had. A refused
        // amendment that had already moved the price would leave the engine
        // and the venue describing different orders.
        ++metrics_.replace_rejects;
        e.payload.replace.accepted = false;
        e.payload.replace.original_exchange_order_id = order->exchange_order_id;
        e.payload.replace.new_price = order->price;
        e.payload.replace.new_qty = order->original_qty;
        e.payload.replace.cumulative_qty = order->cumulative_qty;
        e.payload.replace.error =
            ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                                "replace refused");
        schedule(delay, e);
        return;
    }

    // Reducing quantity below what is already filled cannot be honoured.
    if (request.new_quantity < order->cumulative_qty) {
        ++metrics_.replace_rejects;
        e.payload.replace.accepted = false;
        e.payload.replace.original_exchange_order_id = order->exchange_order_id;
        e.payload.replace.error = ExchangeError::make(
            ExchangeErrorCategory::InvalidRequest, RequestOutcome::Rejected,
            "new quantity below filled quantity", RejectReason::QuantityOutOfBounds);
        schedule(delay, e);
        return;
    }

    const ExchangeOrderId old_exchange_id = order->exchange_order_id;
    const ClientOrderId old_client_id = order->client_order_id;
    const std::size_t index = by_client_id_.at(old_client_id);

    // Atomic swap: new identity, new parameters, filled quantity carried over.
    // A price change surrenders queue position, which is why the arrival
    // sequence is reassigned -- keeping it would model a free improvement no
    // venue offers.
    order->exchange_order_id = next_exchange_id();
    order->client_order_id = request.new_client_order_id;
    const bool price_moved = order->price != request.new_price;
    order->price = request.new_price;
    order->original_qty = request.new_quantity;
    order->status = order->cumulative_qty.is_positive() ? OrderStatus::PartiallyFilled
                                                        : OrderStatus::New;
    if (price_moved) {
        order->arrival_sequence = next_arrival_seq_++;
    }
    order->last_event_ns = clock_.steady();

    by_client_id_.erase(old_client_id);
    by_exchange_id_.erase(old_exchange_id);
    by_client_id_[order->client_order_id] = index;
    by_exchange_id_[order->exchange_order_id] = index;

    ++metrics_.replace_acks;
    e.payload.replace.accepted = true;
    e.payload.replace.original_exchange_order_id = old_exchange_id;
    e.payload.replace.new_exchange_order_id = order->exchange_order_id;
    e.payload.replace.new_status = order->status;
    e.payload.replace.new_price = order->price;
    e.payload.replace.new_qty = order->original_qty;
    e.payload.replace.cumulative_qty = order->cumulative_qty;
    schedule(delay, e);
}

// -------------------------------------------------------------- market data

void PaperExecution::attach_book(const Symbol& symbol, const book::OrderBook& book) {
    books_[symbol] = &book;
}

void PaperExecution::on_market_update(const Symbol& symbol, Nanos update_ns) {
    // `update_ns` is when this data was produced; matching happens now. The
    // two are the same in a healthy feed and diverge exactly when staleness
    // matters, so the age below must be measured against the clock rather than
    // against the data's own timestamp -- comparing it with itself would make
    // the staleness guard dead code.
    book_updated_ns_[symbol] = update_ns;
    match_symbol(symbol, clock_.steady());
}

void PaperExecution::match_symbol(const Symbol& symbol, Nanos now_ns) {
    const auto book_it = books_.find(symbol);
    if (book_it == books_.end() || book_it->second == nullptr) {
        return;
    }
    const book::OrderBook& book = *book_it->second;
    if (!book.is_valid()) {
        return;
    }

    // §30. A book we cannot vouch for produces no fills. Inventing an
    // execution from data we already know is stale would put a position on the
    // engine's books that never existed anywhere else. Cancellation and
    // reconciliation are deliberately unaffected -- they are how the engine
    // gets out of trouble, and gating them on fresh data would trap it.
    const Nanos age = now_ns - book_updated_ns_[symbol];
    if (age > config_.max_book_age_ns) {
        ++metrics_.stale_book_skips;
        return;
    }

    // §10. Price first, then arrival sequence. Deterministic, and documented in
    // docs/paper-execution.md as an approximation of a real exchange queue
    // rather than a reproduction of one.
    std::vector<std::size_t> order_indices;
    order_indices.reserve(orders_.size());
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        if (orders_[i].symbol == symbol && orders_[i].is_resting()) {
            order_indices.push_back(i);
        }
    }
    std::sort(order_indices.begin(), order_indices.end(),
              [this](std::size_t a, std::size_t b) {
                  const PaperOrder& x = orders_[a];
                  const PaperOrder& y = orders_[b];
                  if (x.side != y.side) {
                      return x.side < y.side;
                  }
                  if (x.price != y.price) {
                      // Better price first: higher for bids, lower for asks.
                      return x.side == Side::Buy ? x.price > y.price : x.price < y.price;
                  }
                  return x.arrival_sequence < y.arrival_sequence;
              });

    // One working copy of each side per update. Every order draws from the
    // same pool, so the simulated venue can never fill more than the market
    // displayed -- however many of our orders are sitting at that price.
    std::vector<PriceLevel> asks_available = executable_side(book, Side::Buy);
    std::vector<PriceLevel> bids_available = executable_side(book, Side::Sell);

    for (const std::size_t i : order_indices) {
        PaperOrder& order = orders_[i];
        if (!order.is_resting()) {
            continue;
        }
        std::vector<PriceLevel>& available =
            order.side == Side::Buy ? asks_available : bids_available;
        const MatchResult match =
            match_and_consume(order, available, config_.fill_model, config_.queue_share_bps);
        if (!match.quantity.is_positive()) {
            continue;
        }
        apply_match(order, match, now_ns);
        schedule_fill(order, match, config_.latency.fill_ns);
    }
}

void PaperExecution::apply_match(PaperOrder& order, const MatchResult& match, Nanos at_ns) {
    // Checked throughout: overfilling is the one arithmetic mistake here that
    // would corrupt every downstream number at once (§9, invariant 4).
    Qty next;
    if (!checked_add(order.cumulative_qty, match.quantity, next)) {
        return;
    }
    if (next > order.original_qty) {
        return;
    }
    order.cumulative_qty = next;
    order.last_event_ns = at_ns;
    order.status = order.remaining().is_zero() ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
}

void PaperExecution::schedule_fill(const PaperOrder& order, const MatchResult& match, Nanos delay) {
    const bool complete = order.remaining().is_zero();
    if (complete) {
        ++metrics_.fills;
    } else {
        ++metrics_.partial_fills;
    }

    ExecutionEvent e = make_event(ExecutionEventType::Fill);
    e.payload.fill.client_order_id = order.client_order_id;
    e.payload.fill.exchange_order_id = order.exchange_order_id;
    e.payload.fill.trade_id = next_trade_id();
    e.payload.fill.symbol = order.symbol;
    e.payload.fill.symbol_id = order.symbol_id;
    e.payload.fill.side = order.side;
    e.payload.fill.price = match.price;
    e.payload.fill.quantity = match.quantity;
    e.payload.fill.cumulative_qty = order.cumulative_qty;
    e.payload.fill.leaves_qty = order.remaining();
    e.payload.fill.liquidity = match.liquidity;
    e.payload.fill.order_status = order.status;
    // A fee, not the trade value. Setting this to the notional -- which an
    // earlier revision did -- would have every consumer believe the whole
    // execution was cost.
    const std::int32_t rate_bps =
        match.liquidity == Liquidity::Taker ? config_.taker_fee_bps : config_.maker_fee_bps;
    const Notional value = notional_of(match.price, match.quantity);
    e.payload.fill.fee = Notional::from_raw(static_cast<std::int64_t>(
        (static_cast<int128>(value.raw()) * static_cast<int128>(rate_bps)) /
        static_cast<int128>(10'000)));
    e.payload.fill.transact_ns = clock_.wall();
    e.payload.fill.seq = next_seq();
    schedule(delay, e);
}

// ------------------------------------------------------------------ queries

Status PaperExecution::query_open_orders(const Symbol& symbol) {
    MM_RETURN_IF_ERROR(require_connected());
    ++metrics_.snapshots;

    std::vector<OrderStatusReport> rows;
    std::uint32_t hidden = config_.faults.hide_next_snapshot_orders;
    config_.faults.hide_next_snapshot_orders = 0;
    for (const PaperOrder& order : orders_) {
        if (!order.is_resting()) {
            continue;
        }
        if (!symbol.empty() && order.symbol != symbol) {
            continue;
        }
        if (hidden > 0) {
            // Deliberately omitted, so reconciliation meets a venue that says
            // an order is absent when it is not (§20).
            --hidden;
            continue;
        }
        OrderStatusReport row;
        row.client_order_id = order.client_order_id;
        row.exchange_order_id = order.exchange_order_id;
        row.symbol = order.symbol;
        row.symbol_id = order.symbol_id;
        row.side = order.side;
        row.type = order.type;
        row.tif = order.tif;
        row.price = order.price;
        row.original_qty = order.original_qty;
        row.cumulative_qty = order.cumulative_qty;
        row.status = order.status;
        row.transact_ns = clock_.wall();
        rows.push_back(row);
    }

    // Chunked exactly as a paged venue answer is, including the empty case --
    // reconciliation may only conclude an order is absent after `is_last`, and
    // that rule needs a last chunk to exist even when there is nothing to send.
    const std::size_t chunks =
        rows.empty() ? 1 : (rows.size() + kMaxOrdersPerSnapshotEvent - 1) / kMaxOrdersPerSnapshotEvent;
    for (std::size_t c = 0; c < chunks; ++c) {
        ExecutionEvent e = make_event(ExecutionEventType::OpenOrdersSnapshot);
        e.payload.open_orders.symbol_filter = symbol;
        e.payload.open_orders.is_first = (c == 0);
        e.payload.open_orders.is_last = (c + 1 == chunks);
        std::uint8_t n = 0;
        for (std::size_t k = c * kMaxOrdersPerSnapshotEvent;
             k < rows.size() && n < kMaxOrdersPerSnapshotEvent; ++k, ++n) {
            e.payload.open_orders.orders[n] = rows[k];
        }
        e.payload.open_orders.count = n;
        schedule(config_.latency.query_ns, e);
    }
    return Status::ok();
}

Status PaperExecution::query_order(const ClientOrderId& client_order_id,
                                   const ExchangeOrderId& exchange_order_id) {
    MM_RETURN_IF_ERROR(require_connected());
    const PaperOrder* order = find_order(client_order_id);
    if (order == nullptr && !exchange_order_id.empty()) {
        order = find_order(exchange_order_id);
    }
    if (order == nullptr) {
        ExecutionEvent e = make_event(ExecutionEventType::RequestFailure);
        e.payload.failure.kind = RequestKind::QueryOrder;
        e.payload.failure.client_order_id = client_order_id;
        e.payload.failure.exchange_order_id = exchange_order_id;
        e.payload.failure.error = ExchangeError::make(ExchangeErrorCategory::UnknownOrderState,
                                                      RequestOutcome::Unknown, "no such order",
                                                      RejectReason::UnknownOrder);
        schedule(config_.latency.query_ns, e);
        return Status::ok();
    }

    ExecutionEvent e = make_event(ExecutionEventType::OrderStatus);
    e.payload.order_status.client_order_id = order->client_order_id;
    e.payload.order_status.exchange_order_id = order->exchange_order_id;
    e.payload.order_status.symbol = order->symbol;
    e.payload.order_status.symbol_id = order->symbol_id;
    e.payload.order_status.side = order->side;
    e.payload.order_status.type = order->type;
    e.payload.order_status.tif = order->tif;
    e.payload.order_status.price = order->price;
    e.payload.order_status.original_qty = order->original_qty;
    e.payload.order_status.cumulative_qty = order->cumulative_qty;
    e.payload.order_status.status = order->status;
    e.payload.order_status.transact_ns = clock_.wall();
    schedule(config_.latency.query_ns, e);
    return Status::ok();
}

Status PaperExecution::query_balances() {
    // A paper venue holds no custody. Answering with an invented balance would
    // be worse than saying so plainly.
    return {ErrorCode::FailedPrecondition, "paper venue does not model balances"};
}

Status PaperExecution::query_positions() {
    return {ErrorCode::FailedPrecondition, "paper venue does not model positions"};
}

}  // namespace mm::exchange::paper
