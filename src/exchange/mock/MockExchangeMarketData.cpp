#include "mm/exchange/mock/MockExchangeMarketData.hpp"

#include <algorithm>
#include <limits>

#include "mm/exchange/mock/MockExchangeExecution.hpp"  // capability presets

namespace mm::exchange::mock {

MockExchangeMarketData::MockExchangeMarketData(ManualClock& clock, ExchangeCapabilities caps)
    : clock_(clock), caps_(caps) {}

MockExchangeMarketData::MockExchangeMarketData(ManualClock& clock)
    : MockExchangeMarketData(clock, permissive_mock_capabilities()) {}

MockExchangeMarketData::~MockExchangeMarketData() = default;

Status MockExchangeMarketData::start(IMarketDataSink& sink) {
    sink_ = &sink;
    started_ = true;
    set_connection_state(ConnectionState::Connected);
    return Status::ok();
}

void MockExchangeMarketData::stop() {
    started_ = false;
    sink_ = nullptr;
    connection_ = ConnectionState::Disconnected;
    sessions_.clear();
}

void MockExchangeMarketData::emit(MarketDataEvent& event) {
    if (sink_ == nullptr) {
        return;
    }
    const Nanos now = clock_.steady();
    event.stamps.exchange_ns = clock_.wall();
    event.stamps.recv_ns = now;
    event.stamps.parse_ns = now;
    event.stamps.process_ns = now;

    last_event_ = event;
    has_last_event_ = true;
    ++published_;
    sink_->on_market_data(event);
}

void MockExchangeMarketData::emit_session_event(const Symbol& symbol, SessionState from,
                                                SessionState to, std::string_view reason) {
    MarketDataEvent e;
    e.type = MarketDataEventType::SessionState;
    e.payload.session.symbol = symbol;
    e.payload.session.from = from;
    e.payload.session.to = to;
    static_cast<void>(e.payload.session.reason.assign(reason));
    emit(e);
}

MockExchangeMarketData::SymbolSession* MockExchangeMarketData::find(const Symbol& symbol) {
    const auto it = sessions_.find(symbol);
    return it == sessions_.end() ? nullptr : &it->second;
}

const MockExchangeMarketData::SymbolSession* MockExchangeMarketData::find(
    const Symbol& symbol) const {
    const auto it = sessions_.find(symbol);
    return it == sessions_.end() ? nullptr : &it->second;
}

Status MockExchangeMarketData::subscribe(const SubscriptionRequest& request) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "market data adapter not started"};
    }
    if (request.symbol.empty()) {
        return {ErrorCode::InvalidArgument, "subscription has no symbol"};
    }
    if (connection_ != ConnectionState::Connected) {
        return {ErrorCode::Unavailable, "not connected"};
    }
    if (caps_.max_subscriptions_per_connection > 0 &&
        sessions_.size() >= caps_.max_subscriptions_per_connection) {
        return {ErrorCode::OutOfRange, "subscription limit reached for this connection"};
    }

    SymbolSession session;
    session.request = request;
    session.state = SessionState::Disconnected;
    sessions_.insert_or_assign(request.symbol, session);

    // The full documented path. Nothing is allowed to skip a step, including
    // the mock itself.
    for (const SessionState next : {SessionState::Connecting, SessionState::Connected,
                                    SessionState::Subscribed}) {
        MM_RETURN_IF_ERROR(set_session_state(request.symbol, next, "subscribe"));
    }
    return Status::ok();
}

Status MockExchangeMarketData::unsubscribe(const Symbol& symbol) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }
    const SessionState from = session->state;
    sessions_.erase(symbol);
    emit_session_event(symbol, from, SessionState::Disconnected, "unsubscribe");
    return Status::ok();
}

Status MockExchangeMarketData::request_snapshot(const Symbol& symbol) {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    if (!caps_.supports_orderbook_snapshot) {
        return {ErrorCode::InvalidArgument, "venue does not provide snapshots"};
    }
    if (find(symbol) == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }
    ++snapshot_requests_;
    return Status::ok();
}

Status MockExchangeMarketData::request_instruments() {
    if (!started_) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    for (const auto& [symbol, spec] : instruments_) {
        MarketDataEvent e;
        e.type = MarketDataEventType::InstrumentUpdate;
        e.payload.instrument.spec = spec;
        emit(e);
    }
    return Status::ok();
}

SessionState MockExchangeMarketData::session_state(const Symbol& symbol) const {
    const SymbolSession* session = find(symbol);
    return session == nullptr ? SessionState::Disconnected : session->state;
}

Nanos MockExchangeMarketData::data_age_ns(const Symbol& symbol) const {
    const SymbolSession* session = find(symbol);
    if (session == nullptr || session->last_event_ns == 0) {
        // Never having received data is maximally stale, not fresh. Returning 0
        // here would let a symbol that has produced nothing look healthy.
        return std::numeric_limits<Nanos>::max();
    }
    return clock_.steady() - session->last_event_ns;
}

Status MockExchangeMarketData::set_session_state(const Symbol& symbol, SessionState to,
                                                 std::string_view reason) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }
    const SessionState from = session->state;
    if (!is_legal_transition(from, to)) {
        std::string msg = "illegal session transition ";
        msg.append(::mm::exchange::to_string(from)).append(" -> ").append(
            ::mm::exchange::to_string(to));
        return {ErrorCode::FailedPrecondition, msg};
    }
    session->state = to;
    if (from != to) {
        emit_session_event(symbol, from, to, reason);
    }
    return Status::ok();
}

void MockExchangeMarketData::set_connection_state(ConnectionState state) {
    const ConnectionState previous = connection_;
    connection_ = state;
    if (previous == state) {
        return;
    }
    MarketDataEvent e;
    e.type = MarketDataEventType::Connection;
    e.payload.connection.venue = venue();
    e.payload.connection.from = previous;
    e.payload.connection.to = state;
    emit(e);
}

void MockExchangeMarketData::set_instrument(const InstrumentSpec& spec) {
    instruments_.insert_or_assign(spec.symbol, spec);
    if (!started_) {
        return;
    }
    MarketDataEvent e;
    e.type = MarketDataEventType::InstrumentUpdate;
    e.payload.instrument.spec = spec;
    emit(e);
}

namespace {

/// Copies up to `kMaxLevelsPerEvent` levels starting at `offset`.
std::size_t fill_side(const std::vector<PriceLevel>& src, std::size_t offset,
                      std::array<PriceLevel, kMaxLevelsPerEvent>& dst, std::uint8_t& count) {
    std::size_t n = 0;
    while (offset + n < src.size() && n < kMaxLevelsPerEvent) {
        dst[n] = src[offset + n];
        ++n;
    }
    count = static_cast<std::uint8_t>(n);
    return n;
}

}  // namespace

Status MockExchangeMarketData::publish_snapshot(const Symbol& symbol, Seq last_update_id,
                                                const std::vector<PriceLevel>& bids,
                                                const std::vector<PriceLevel>& asks) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }

    std::size_t bid_offset = 0;
    std::size_t ask_offset = 0;
    bool first = true;

    // Chunk until both sides are exhausted. A single-chunk snapshot is just the
    // degenerate case, so the chunking path is exercised even by small books.
    do {
        MarketDataEvent e;
        e.type = MarketDataEventType::BookSnapshot;
        BookSnapshotEvent& s = e.payload.book_snapshot;
        s.symbol = symbol;
        s.symbol_id = session->request.symbol_id;
        s.last_update_id = last_update_id;
        bid_offset += fill_side(bids, bid_offset, s.levels.bids, s.levels.bid_count);
        ask_offset += fill_side(asks, ask_offset, s.levels.asks, s.levels.ask_count);
        s.is_first = first;
        s.is_last = (bid_offset >= bids.size() && ask_offset >= asks.size());
        first = false;

        session->last_event_ns = clock_.steady();
        session->last_seq = last_update_id;
        emit(e);
    } while (bid_offset < bids.size() || ask_offset < asks.size());

    return Status::ok();
}

Status MockExchangeMarketData::publish_update(const Symbol& symbol, Seq first_id, Seq final_id,
                                              const std::vector<PriceLevel>& bids,
                                              const std::vector<PriceLevel>& asks) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }

    std::size_t bid_offset = 0;
    std::size_t ask_offset = 0;
    const Seq previous = session->last_seq;

    do {
        MarketDataEvent e;
        e.type = MarketDataEventType::BookUpdate;
        BookUpdateEvent& u = e.payload.book_update;
        u.symbol = symbol;
        u.symbol_id = session->request.symbol_id;
        u.first_update_id = first_id;
        u.final_update_id = final_id;
        u.prev_final_update_id = previous;
        bid_offset += fill_side(bids, bid_offset, u.levels.bids, u.levels.bid_count);
        ask_offset += fill_side(asks, ask_offset, u.levels.asks, u.levels.ask_count);
        u.is_last = (bid_offset >= bids.size() && ask_offset >= asks.size());

        session->last_event_ns = clock_.steady();
        emit(e);
    } while (bid_offset < bids.size() || ask_offset < asks.size());

    session->last_seq = final_id;
    return Status::ok();
}

Status MockExchangeMarketData::publish_trade(const Symbol& symbol, Px price, Qty quantity,
                                             Side aggressor, const TradeId& trade_id) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }
    if (!caps_.supports_trade_stream) {
        return {ErrorCode::InvalidArgument, "venue has no trade stream"};
    }

    MarketDataEvent e;
    e.type = MarketDataEventType::Trade;
    e.payload.trade.symbol = symbol;
    e.payload.trade.symbol_id = session->request.symbol_id;
    e.payload.trade.price = price;
    e.payload.trade.quantity = quantity;
    e.payload.trade.aggressor = aggressor;
    e.payload.trade.trade_id = trade_id;
    e.payload.trade.seq = ++session->last_seq;

    session->last_event_ns = clock_.steady();
    emit(e);
    return Status::ok();
}

Status MockExchangeMarketData::publish_bbo(const Symbol& symbol, const BestBidAsk& bbo,
                                           bool native) {
    SymbolSession* session = find(symbol);
    if (session == nullptr) {
        return {ErrorCode::NotFound, "not subscribed"};
    }
    if (native && !caps_.supports_native_bbo) {
        return {ErrorCode::InvalidArgument, "venue has no native BBO stream"};
    }

    MarketDataEvent e;
    e.type = MarketDataEventType::Bbo;
    e.payload.bbo.symbol = symbol;
    e.payload.bbo.symbol_id = session->request.symbol_id;
    e.payload.bbo.bbo = bbo;
    e.payload.bbo.is_native = native;

    session->last_event_ns = clock_.steady();
    emit(e);
    return Status::ok();
}

Status MockExchangeMarketData::redeliver_last_event() {
    if (!has_last_event_) {
        return {ErrorCode::FailedPrecondition, "no event has been published yet"};
    }
    if (sink_ == nullptr) {
        return {ErrorCode::FailedPrecondition, "not started"};
    }
    sink_->on_market_data(last_event_);
    return Status::ok();
}

bool MockExchangeMarketData::is_subscribed(const Symbol& symbol) const {
    return find(symbol) != nullptr;
}

}  // namespace mm::exchange::mock
