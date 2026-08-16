#pragma once

/// \file BinanceMarketData.hpp
/// `IExchangeMarketData` for Binance Spot public market data.
///
/// ## Threading
///
/// The adapter owns one `io_context` and one thread (`md-io`). Every socket
/// handler, every decode, every synchronizer call and every book mutation
/// happens on that thread, so none of them needs a lock. Public methods called
/// from other threads (`subscribe`, `request_snapshot`, `stop`) post work onto
/// it rather than touching state directly.
///
/// Cross-thread *reads* — `session_state`, `data_age_ns`, `connection_state`,
/// `book_view` — are served from per-symbol atomics and a `Seqlock`, never from
/// a mutex. They are polled by the trading thread and by monitoring, and a
/// mutex there would let an observer stall the feed. The only lock in the class
/// guards the symbol registry, which changes on subscribe and unsubscribe and
/// never on the per-event path.
///
/// ## Ownership
///
/// The adapter owns the book. That follows from the interface: `session_state`,
/// `request_snapshot` and `data_age_ns` are adapter methods, and they only mean
/// anything if the adapter knows whether the book is synchronized. Building the
/// book beside the data that produces it also keeps sequence validation, gap
/// detection and resync in one place. Consumers receive a validated, torn-free
/// view; they never see a book mid-update.
///
/// **Public market data only.** No credentials, no signing, no account or order
/// endpoint.

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mm/common/Seqlock.hpp"
#include "mm/exchange/binance/BinanceCodec.hpp"
#include "mm/exchange/binance/BinanceTransport.hpp"
#include "mm/exchange/common/IExchangeMarketData.hpp"
#include "mm/orderbook/BookSynchronizer.hpp"

namespace mm::exchange::binance {

/// Levels published to consumers through the seqlock. A market maker quoting at
/// the touch needs the top of book and a little context; publishing the full
/// depth would make every snapshot copy far more expensive for data nobody
/// reads. The synchronizer's own book keeps the configured depth.
inline constexpr std::size_t kPublishedDepth = 10;

/// A consistent, self-contained picture of one symbol. Trivially copyable so it
/// can travel through a `Seqlock` without tearing.
struct BookView {
    Symbol symbol{};
    SessionState state = SessionState::Disconnected;
    Seq last_update_id = kNoSeq;
    Nanos exchange_ns = 0;
    Nanos recv_ns = 0;
    std::uint8_t bid_count = 0;
    std::uint8_t ask_count = 0;
    std::array<PriceLevel, kPublishedDepth> bids{};
    std::array<PriceLevel, kPublishedDepth> asks{};

    [[nodiscard]] bool is_quotable() const noexcept { return exchange::is_quotable(state); }
    [[nodiscard]] BestBidAsk bbo() const noexcept;
};

static_assert(std::is_trivially_copyable_v<BookView>);

class BinanceMarketData final : public IExchangeMarketData {
public:
    static constexpr std::size_t kMaxSymbols = 32;

    struct Config {
        std::string ws_host = "stream.binance.com";
        std::string ws_port = "9443";
        std::string rest_host = "api.binance.com";
        std::string rest_port = "443";

        /// 100 or 1000. A market maker wants 100.
        std::int32_t depth_update_ms = 100;
        /// Depth levels requested from REST. Binance allows up to 5000.
        std::int32_t snapshot_limit = 1000;
        Nanos rest_timeout = seconds(5);
        /// How often staleness is checked and pending snapshots are issued.
        Nanos timer_interval = millis(50);

        bool want_trades = true;

        book::SyncConfig sync{};
        ReconnectBackoff::Config backoff{};
        std::uint64_t backoff_seed = 0;
    };

    BinanceMarketData(Config config, const Clock& clock);
    ~BinanceMarketData() override;

    // ---------------------------------------------------- IExchangeMarketData
    [[nodiscard]] Status start(IMarketDataSink& sink) override;
    void stop() override;
    [[nodiscard]] Status subscribe(const SubscriptionRequest& request) override;
    [[nodiscard]] Status unsubscribe(const Symbol& symbol) override;
    [[nodiscard]] Status request_snapshot(const Symbol& symbol) override;
    [[nodiscard]] Status request_instruments() override;

    [[nodiscard]] SessionState session_state(const Symbol& symbol) const override;
    [[nodiscard]] ConnectionState connection_state() const override;
    [[nodiscard]] Nanos data_age_ns(const Symbol& symbol) const override;
    [[nodiscard]] const ExchangeCapabilities& capabilities() const override { return caps_; }
    [[nodiscard]] VenueName venue() const override { return VenueName("binance"); }

    // -------------------------------------------------------------- extras

    /// Torn-free consistent view. Returns false if the symbol is unknown or the
    /// writer outran this reader; the caller keeps its previous copy.
    [[nodiscard]] bool book_view(const Symbol& symbol, BookView& out) const;

    struct Diagnostics {
        std::uint64_t frames_received = 0;
        std::uint64_t decode_errors = 0;
        std::uint64_t events_published = 0;
        std::uint64_t snapshot_requests = 0;
        std::uint64_t snapshot_failures = 0;
        std::uint32_t reconnects = 0;
        std::uint64_t instruments_loaded = 0;
        /// Instrument fetches that produced nothing. Non-zero means order
        /// validation has no venue rules to work from.
        std::uint64_t instrument_failures = 0;
    };
    [[nodiscard]] Diagnostics diagnostics() const;

private:
    /// Per-symbol state. Everything mutable lives on the md-io thread except
    /// the atomics and the seqlock, which exist so observers never block it.
    struct SymbolState {
        Symbol symbol{};
        SubscriptionRequest request{};
        std::unique_ptr<book::BookSynchronizer> sync;
        std::atomic<SessionState> published_state{SessionState::Disconnected};
        std::atomic<Nanos> published_data_ns{0};
        std::atomic<bool> has_data{false};
        Seqlock<BookView> view;
        bool active = false;
    };

    void io_thread_main();
    void on_ws_frame(std::string_view frame, Nanos recv_ns);
    void on_ws_state(ConnectionState state, std::string_view reason);
    void arm_timer();
    void on_timer();
    void issue_snapshot(SymbolState& symbol_state);
    void issue_exchange_info();

    void emit(const MarketDataEvent& event);
    void emit_session_event(const SymbolState& symbol_state, SessionState from, SessionState to,
                            std::string_view reason);
    void emit_connection_event(ConnectionState from, ConnectionState to, std::string_view reason);
    void publish_view(SymbolState& symbol_state);
    void wire_synchronizer(SymbolState& symbol_state);

    /// Called only on the md-io thread.
    [[nodiscard]] SymbolState* find(const Symbol& symbol);
    [[nodiscard]] const SymbolState* find(const Symbol& symbol) const;
    [[nodiscard]] std::string build_stream_path() const;
    [[nodiscard]] std::vector<std::string> stream_names_for(const SubscriptionRequest& r) const;

    Config config_;
    const Clock& clock_;
    ExchangeCapabilities caps_{};

    IMarketDataSink* sink_ = nullptr;
    std::atomic<bool> running_{false};

    std::unique_ptr<net::io_context> io_;
    std::shared_ptr<ssl::context> tls_;
    std::shared_ptr<BinanceWsSession> ws_;
    std::unique_ptr<BinanceRestClient> rest_;
    std::unique_ptr<net::steady_timer> timer_;
    std::thread io_thread_;

    BinanceCodec codec_;
    std::vector<MarketDataEvent> decode_buffer_;

    /// Fixed storage so a subscription never reallocates state another thread
    /// may be reading through.
    std::array<std::unique_ptr<SymbolState>, kMaxSymbols> symbols_;
    std::atomic<std::size_t> symbol_count_{0};
    /// Guards registration only. Never taken on the per-event path.
    mutable std::mutex registry_mutex_;

    std::atomic<ConnectionState> connection_{ConnectionState::Disconnected};
    std::atomic<std::uint64_t> decode_errors_{0};
    std::atomic<std::uint64_t> events_published_{0};
    std::atomic<std::uint64_t> snapshot_requests_{0};
    std::atomic<std::uint64_t> snapshot_failures_{0};
    std::atomic<std::uint64_t> instruments_loaded_{0};
    std::atomic<std::uint64_t> instrument_failures_{0};
    std::atomic<std::uint32_t> subscribe_id_{1};
};

}  // namespace mm::exchange::binance
