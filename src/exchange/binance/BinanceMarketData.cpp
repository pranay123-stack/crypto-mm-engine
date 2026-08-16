#include "mm/exchange/binance/BinanceMarketData.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>

namespace mm::exchange::binance {

BestBidAsk BookView::bbo() const noexcept {
    BestBidAsk b;
    if (bid_count > 0) {
        b.bid_px = bids[0].price;
        b.bid_qty = bids[0].quantity;
    }
    if (ask_count > 0) {
        b.ask_px = asks[0].price;
        b.ask_qty = asks[0].quantity;
    }
    b.seq = last_update_id;
    b.exchange_ns = exchange_ns;
    return b;
}

BinanceMarketData::BinanceMarketData(Config config, const Clock& clock)
    : config_(std::move(config)), clock_(clock) {
    // Declared, not assumed. Binance Spot has no native position stream and no
    // atomic replace; the market-data half only needs the data capabilities.
    caps_.supports_orderbook_snapshot = true;
    caps_.supports_incremental_depth = true;
    caps_.supports_trade_stream = true;
    caps_.supports_native_bbo = true;   // bookTicker, not subscribed by default
    caps_.supports_client_order_id = true;
    caps_.max_client_order_id_len = 36;
    caps_.max_subscriptions_per_connection = 200;
    caps_.max_snapshot_depth = 5'000;
    decode_buffer_.reserve(64);
}

BinanceMarketData::~BinanceMarketData() { stop(); }

// ------------------------------------------------------------------ lookup

BinanceMarketData::SymbolState* BinanceMarketData::find(const Symbol& symbol) {
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (symbols_[i] && symbols_[i]->active && symbols_[i]->symbol == symbol) {
            return symbols_[i].get();
        }
    }
    return nullptr;
}

const BinanceMarketData::SymbolState* BinanceMarketData::find(const Symbol& symbol) const {
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (symbols_[i] && symbols_[i]->active && symbols_[i]->symbol == symbol) {
            return symbols_[i].get();
        }
    }
    return nullptr;
}

// ----------------------------------------------------------------- streams

std::vector<std::string> BinanceMarketData::stream_names_for(
    const SubscriptionRequest& request) const {
    std::vector<std::string> names;
    if (request.want_depth) {
        names.push_back(BinanceCodec::depth_stream_name(request.symbol, config_.depth_update_ms));
    }
    if (request.want_trades && config_.want_trades) {
        names.push_back(BinanceCodec::trade_stream_name(request.symbol));
    }
    return names;
}

std::string BinanceMarketData::build_stream_path() const {
    std::string path = "/stream?streams=";
    bool first = true;
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (!symbols_[i] || !symbols_[i]->active) {
            continue;
        }
        for (const std::string& name : stream_names_for(symbols_[i]->request)) {
            if (!first) {
                path.push_back('/');
            }
            first = false;
            path.append(name);
        }
    }
    // Connecting with no streams would leave a socket open receiving nothing,
    // which the staleness detector would then correctly flag as a fault.
    return first ? std::string{} : path;
}

// ----------------------------------------------------------------- lifecycle

Status BinanceMarketData::start(IMarketDataSink& sink) {
    if (running_.load(std::memory_order_acquire)) {
        return {ErrorCode::AlreadyExists, "adapter already started"};
    }
    sink_ = &sink;

    auto tls = make_tls_context();
    if (tls.is_error()) {
        return tls.status();
    }
    tls_ = tls.value();

    io_ = std::make_unique<net::io_context>();
    rest_ = std::make_unique<BinanceRestClient>(*io_, *tls_, config_.rest_host, config_.rest_port);
    timer_ = std::make_unique<net::steady_timer>(*io_);

    BinanceWsSession::Config ws_config;
    ws_config.host = config_.ws_host;
    ws_config.port = config_.ws_port;
    ws_config.target = build_stream_path();
    ws_config.backoff = config_.backoff;
    ws_config.backoff_seed = config_.backoff_seed;
    if (ws_config.target.empty()) {
        // Subscriptions may arrive after start; connect to a placeholder stream
        // rather than an empty target, which the venue rejects.
        ws_config.target = "/stream?streams=";
    }
    ws_ = std::make_shared<BinanceWsSession>(*io_, *tls_, ws_config);
    ws_->set_message_handler(
        [this](std::string_view frame, Nanos recv_ns) { on_ws_frame(frame, recv_ns); });
    ws_->set_state_handler(
        [this](ConnectionState state, std::string_view reason) { on_ws_state(state, reason); });

    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this] { io_thread_main(); });
    return Status::ok();
}

void BinanceMarketData::io_thread_main() {
    // Keeps the context alive across gaps between asynchronous operations.
    auto guard = net::make_work_guard(*io_);

    net::post(*io_, [this] {
        issue_exchange_info();
        if (ws_) {
            ws_->start();
        }
        arm_timer();
    });

    io_->run();
}

void BinanceMarketData::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (io_) {
        net::post(*io_, [this] {
            if (ws_) {
                ws_->stop();
            }
            if (timer_) {
                timer_->cancel();
            }
        });
        io_->stop();
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    // Only after the thread is joined is it safe for the caller to tear down
    // the sink: no further callback can be in flight.
    ws_.reset();
    timer_.reset();
    rest_.reset();
    io_.reset();
    sink_ = nullptr;
    connection_.store(ConnectionState::Disconnected, std::memory_order_release);
}

// -------------------------------------------------------------- subscribe

void BinanceMarketData::wire_synchronizer(SymbolState& symbol_state) {
    SymbolState* ptr = &symbol_state;
    symbol_state.sync->set_state_callback(
        [this, ptr](SessionState from, SessionState to, std::string_view reason) {
            ptr->published_state.store(to, std::memory_order_release);
            emit_session_event(*ptr, from, to, reason);
            publish_view(*ptr);
        });
}

Status BinanceMarketData::subscribe(const SubscriptionRequest& request) {
    if (request.symbol.empty()) {
        return {ErrorCode::InvalidArgument, "subscription has no symbol"};
    }

    const std::lock_guard<std::mutex> guard(registry_mutex_);
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (symbols_[i] && symbols_[i]->symbol == request.symbol) {
            return {ErrorCode::AlreadyExists, "already subscribed"};
        }
    }
    if (n >= kMaxSymbols) {
        return {ErrorCode::OutOfRange, "symbol capacity reached"};
    }

    auto state = std::make_unique<SymbolState>();
    state->symbol = request.symbol;
    state->request = request;

    book::SyncConfig sync_config = config_.sync;
    if (request.depth_levels > 0) {
        sync_config.max_depth = request.depth_levels;
    }
    state->sync =
        std::make_unique<book::BookSynchronizer>(request.symbol, sync_config, clock_);
    state->active = true;

    SymbolState* raw = state.get();
    symbols_[n] = std::move(state);
    // Publish the slot only after it is fully built, so a concurrent reader
    // never sees a half-constructed symbol.
    symbol_count_.store(n + 1, std::memory_order_release);
    wire_synchronizer(*raw);

    if (!running_.load(std::memory_order_acquire) || !io_) {
        return Status::ok();  // folded into the initial connect
    }

    net::post(*io_, [this, raw] {
        if (connection_.load(std::memory_order_acquire) == ConnectionState::Connected && ws_) {
            // Runtime SUBSCRIBE rather than a reconnect: reconnecting would
            // resynchronize every other symbol on this socket for the sake of
            // one new one.
            std::string payload = R"({"method":"SUBSCRIBE","params":[)";
            bool first = true;
            for (const std::string& name : stream_names_for(raw->request)) {
                if (!first) {
                    payload.push_back(',');
                }
                first = false;
                payload.append("\"").append(name).append("\"");
            }
            payload.append("],\"id\":")
                .append(std::to_string(subscribe_id_.fetch_add(1, std::memory_order_relaxed)))
                .append("}");
            if (ws_->send_text(payload)) {
                raw->sync->on_connected();
                raw->sync->on_subscribed();
            }
        }
    });
    return Status::ok();
}

Status BinanceMarketData::unsubscribe(const Symbol& symbol) {
    const std::lock_guard<std::mutex> guard(registry_mutex_);
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (symbols_[i] && symbols_[i]->active && symbols_[i]->symbol == symbol) {
            symbols_[i]->active = false;
            symbols_[i]->published_state.store(SessionState::Disconnected,
                                               std::memory_order_release);
            return Status::ok();
        }
    }
    return {ErrorCode::NotFound, "not subscribed"};
}

// --------------------------------------------------------------- snapshots

Status BinanceMarketData::request_snapshot(const Symbol& symbol) {
    if (!running_.load(std::memory_order_acquire) || !io_) {
        return {ErrorCode::FailedPrecondition, "adapter not started"};
    }
    net::post(*io_, [this, symbol] {
        SymbolState* state = find(symbol);
        if (state != nullptr) {
            issue_snapshot(*state);
        }
    });
    return Status::ok();
}

void BinanceMarketData::issue_snapshot(SymbolState& symbol_state) {
    if (!rest_) {
        return;
    }
    symbol_state.sync->note_snapshot_requested();
    snapshot_requests_.fetch_add(1, std::memory_order_relaxed);

    const std::string target = "/api/v3/depth?symbol=" +
                               BinanceCodec::to_rest_symbol(symbol_state.symbol) +
                               "&limit=" + std::to_string(config_.snapshot_limit);
    SymbolState* ptr = &symbol_state;

    rest_->async_get(target, config_.rest_timeout, [this, ptr](Result<std::string> result) {
        if (result.is_error()) {
            snapshot_failures_.fetch_add(1, std::memory_order_relaxed);
            ptr->sync->note_snapshot_failed(result.status().message());
            return;
        }
        decode_buffer_.clear();
        const Nanos recv_ns = clock_.steady();
        const Status decoded = codec_.decode_depth_snapshot(result.value(), ptr->symbol, recv_ns,
                                                            decode_buffer_);
        if (decoded.is_error()) {
            decode_errors_.fetch_add(1, std::memory_order_relaxed);
            snapshot_failures_.fetch_add(1, std::memory_order_relaxed);
            ptr->sync->on_protocol_error(decoded.message());
            ptr->sync->note_snapshot_failed(decoded.message());
            return;
        }
        for (const MarketDataEvent& event : decode_buffer_) {
            // The synchronizer decides whether this makes the book usable; the
            // sink receives the image either way, for the journal.
            static_cast<void>(ptr->sync->on_snapshot(event.payload.book_snapshot));
            emit(event);
        }
        publish_view(*ptr);
    });
}

Status BinanceMarketData::request_instruments() {
    if (!running_.load(std::memory_order_acquire) || !io_) {
        return {ErrorCode::FailedPrecondition, "adapter not started"};
    }
    net::post(*io_, [this] { issue_exchange_info(); });
    return Status::ok();
}

void BinanceMarketData::issue_exchange_info() {
    if (!rest_) {
        return;
    }
    std::vector<Symbol> wanted;
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (symbols_[i] && symbols_[i]->active) {
            wanted.push_back(symbols_[i]->symbol);
        }
    }

    // Ask only for the symbols we trade. The unfiltered endpoint returns about
    // 17 MB -- far past the decode limit, and a needless load on both the venue
    // and this process. The filtered form is a few kilobytes.
    //
    // Found by the live smoke test: the unfiltered request failed silently and
    // the engine ran with no instrument specs at all, which would have left
    // order validation with nothing to validate against.
    std::string target = "/api/v3/exchangeInfo";
    if (wanted.size() == 1) {
        target.append("?symbol=").append(BinanceCodec::to_rest_symbol(wanted.front()));
    } else if (!wanted.empty()) {
        // Percent-encoded JSON array: symbols=["A","B"]
        target.append("?symbols=%5B");
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (i > 0) {
                target.append(",");
            }
            target.append("%22").append(BinanceCodec::to_rest_symbol(wanted[i])).append("%22");
        }
        target.append("%5D");
    }

    rest_->async_get(target, config_.rest_timeout,
                     [this, wanted](Result<std::string> result) {
                         if (result.is_error()) {
                             // Never silent: without instrument specs nothing
                             // downstream can validate an order.
                             instrument_failures_.fetch_add(1, std::memory_order_relaxed);
                             return;
                         }
                         decode_buffer_.clear();
                         if (codec_.decode_exchange_info(result.value(), wanted, clock_.steady(),
                                                         decode_buffer_)
                                 .is_error()) {
                             decode_errors_.fetch_add(1, std::memory_order_relaxed);
                             instrument_failures_.fetch_add(1, std::memory_order_relaxed);
                             return;
                         }
                         if (decode_buffer_.empty()) {
                             instrument_failures_.fetch_add(1, std::memory_order_relaxed);
                         }
                         for (const MarketDataEvent& event : decode_buffer_) {
                             instruments_loaded_.fetch_add(1, std::memory_order_relaxed);
                             emit(event);
                         }
                     });
}

// ------------------------------------------------------------------- feed

void BinanceMarketData::on_ws_state(ConnectionState state, std::string_view reason) {
    const ConnectionState previous = connection_.exchange(state, std::memory_order_acq_rel);
    emit_connection_event(previous, state, reason);

    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (!symbols_[i] || !symbols_[i]->active) {
            continue;
        }
        book::BookSynchronizer& sync = *symbols_[i]->sync;
        switch (state) {
            case ConnectionState::Connected:
                sync.on_connected();
                // The combined-stream URL subscribes on connect, so the socket
                // being up is also the subscription being live.
                sync.on_subscribed();
                break;
            case ConnectionState::Connecting:
                sync.on_connecting();
                break;
            case ConnectionState::Reconnecting:
                sync.on_disconnected(reason);
                sync.on_connecting();
                sync.on_backoff(reason);
                break;
            case ConnectionState::Failed:
                sync.on_fatal(reason);
                break;
            case ConnectionState::Disconnected:
                sync.on_disconnected(reason);
                break;
        }
        publish_view(*symbols_[i]);
    }
}

void BinanceMarketData::on_ws_frame(std::string_view frame, Nanos recv_ns) {
    decode_buffer_.clear();
    const Status decoded = codec_.decode_stream_frame(frame, recv_ns, decode_buffer_);
    if (decoded.is_error()) {
        decode_errors_.fetch_add(1, std::memory_order_relaxed);
        // Attribute corruption to every live symbol: without a decoded payload
        // there is no way to tell which one it belonged to, and sustained
        // corruption must be able to reach a safe state.
        const std::size_t n = symbol_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            if (symbols_[i] && symbols_[i]->active) {
                symbols_[i]->sync->on_protocol_error(decoded.message());
            }
        }
        return;
    }

    for (const MarketDataEvent& event : decode_buffer_) {
        SymbolState* state = find(event.symbol());
        if (state != nullptr) {
            if (event.type == MarketDataEventType::BookUpdate) {
                static_cast<void>(state->sync->on_update(event.payload.book_update));
                state->published_data_ns.store(event.stamps.recv_ns, std::memory_order_release);
                state->has_data.store(true, std::memory_order_release);
                publish_view(*state);
            } else if (event.type == MarketDataEventType::Trade) {
                state->published_data_ns.store(event.stamps.recv_ns, std::memory_order_release);
                state->has_data.store(true, std::memory_order_release);
            }
        }
        emit(event);
    }
}

void BinanceMarketData::arm_timer() {
    if (!timer_ || !running_.load(std::memory_order_acquire)) {
        return;
    }
    timer_->expires_after(std::chrono::nanoseconds(config_.timer_interval));
    timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            return;
        }
        on_timer();
        arm_timer();
    });
}

void BinanceMarketData::on_timer() {
    const std::size_t n = symbol_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (!symbols_[i] || !symbols_[i]->active) {
            continue;
        }
        SymbolState& state = *symbols_[i];
        state.sync->on_timer();
        // The synchronizer rate-limits this, so polling every tick cannot
        // become a REST request storm.
        if (state.sync->wants_snapshot()) {
            issue_snapshot(state);
        }
    }
}

// -------------------------------------------------------------- publishing

void BinanceMarketData::emit(const MarketDataEvent& event) {
    if (sink_ == nullptr) {
        return;
    }
    events_published_.fetch_add(1, std::memory_order_relaxed);
    sink_->on_market_data(event);
}

void BinanceMarketData::emit_session_event(const SymbolState& symbol_state, SessionState from,
                                           SessionState to, std::string_view reason) {
    MarketDataEvent event;
    event.type = MarketDataEventType::SessionState;
    event.payload.session.symbol = symbol_state.symbol;
    event.payload.session.from = from;
    event.payload.session.to = to;
    static_cast<void>(event.payload.session.reason.assign(reason));
    const Nanos now = clock_.steady();
    event.stamps.recv_ns = now;
    event.stamps.parse_ns = now;
    emit(event);
}

void BinanceMarketData::emit_connection_event(ConnectionState from, ConnectionState to,
                                              std::string_view reason) {
    MarketDataEvent event;
    event.type = MarketDataEventType::Connection;
    event.payload.connection.venue = venue();
    event.payload.connection.from = from;
    event.payload.connection.to = to;
    event.payload.connection.reconnect_count = ws_ ? ws_->reconnect_count() : 0;
    static_cast<void>(event.payload.connection.error.detail.assign(reason));
    const Nanos now = clock_.steady();
    event.stamps.recv_ns = now;
    event.stamps.parse_ns = now;
    emit(event);
}

void BinanceMarketData::publish_view(SymbolState& symbol_state) {
    const book::OrderBook& b = symbol_state.sync->book();

    BookView view;
    view.symbol = symbol_state.symbol;
    view.state = symbol_state.sync->state();
    view.last_update_id = b.last_update_id();
    view.recv_ns = symbol_state.published_data_ns.load(std::memory_order_acquire);

    // Only a synchronized book is published with levels. Publishing a partially
    // rebuilt one would hand a consumer a book that looks usable and is not.
    if (symbol_state.sync->is_quotable()) {
        const std::size_t bids = std::min(b.bids().size(), kPublishedDepth);
        const std::size_t asks = std::min(b.asks().size(), kPublishedDepth);
        for (std::size_t i = 0; i < bids; ++i) {
            view.bids[i] = b.bids()[i];
        }
        for (std::size_t i = 0; i < asks; ++i) {
            view.asks[i] = b.asks()[i];
        }
        view.bid_count = static_cast<std::uint8_t>(bids);
        view.ask_count = static_cast<std::uint8_t>(asks);
    }
    symbol_state.view.store(view);
}

// ------------------------------------------------------------------ queries

SessionState BinanceMarketData::session_state(const Symbol& symbol) const {
    const SymbolState* state = find(symbol);
    return state == nullptr ? SessionState::Disconnected
                            : state->published_state.load(std::memory_order_acquire);
}

ConnectionState BinanceMarketData::connection_state() const {
    return connection_.load(std::memory_order_acquire);
}

Nanos BinanceMarketData::data_age_ns(const Symbol& symbol) const {
    const SymbolState* state = find(symbol);
    if (state == nullptr || !state->has_data.load(std::memory_order_acquire)) {
        // Never having received data is maximally stale, not fresh.
        return std::numeric_limits<Nanos>::max();
    }
    return clock_.steady() - state->published_data_ns.load(std::memory_order_acquire);
}

bool BinanceMarketData::book_view(const Symbol& symbol, BookView& out) const {
    const SymbolState* state = find(symbol);
    if (state == nullptr) {
        return false;
    }
    return state->view.load(out);
}

BinanceMarketData::Diagnostics BinanceMarketData::diagnostics() const {
    Diagnostics d;
    d.frames_received = ws_ ? ws_->frames_received() : 0;
    d.decode_errors = decode_errors_.load(std::memory_order_relaxed);
    d.events_published = events_published_.load(std::memory_order_relaxed);
    d.snapshot_requests = snapshot_requests_.load(std::memory_order_relaxed);
    d.snapshot_failures = snapshot_failures_.load(std::memory_order_relaxed);
    d.reconnects = ws_ ? ws_->reconnect_count() : 0;
    d.instruments_loaded = instruments_loaded_.load(std::memory_order_relaxed);
    d.instrument_failures = instrument_failures_.load(std::memory_order_relaxed);
    return d;
}

}  // namespace mm::exchange::binance
