#pragma once

/// \file PaperExecution.hpp
/// A simulated venue behind the real execution interface.
///
/// > **Paper execution is an execution simulator for validating the live engine
/// > architecture. It is NOT a historical backtesting engine.**
///
/// It consumes the current normalized book, holds its own order state, and
/// answers asynchronously -- the same shape a real venue has. What it proves is
/// that `OMS -> IExchangeExecution` works end to end before there is real money
/// on the other side of it.
///
/// Three properties are load-bearing and each is enforced by a test:
///
/// * **Asynchronous.** `submit()` hands off and returns. The sink is never
///   called from inside it. An adapter that answered synchronously would let
///   the engine pass tests it would fail against any real venue.
/// * **Separate state.** The venue's orders (`PaperOrder`) are its own; the OMS
///   never sees or mutates them, and vice versa (§23, §24).
/// * **Deterministic.** Same initial state, requests, market events and latency
///   produce a byte-identical event sequence (§31).
///
/// ## Threading (§35)
///
/// **`PaperExecution` is single-threaded and owns no thread.** Every entry
/// point -- `submit`, `cancel`, `replace`, `poll`, `on_market_update` -- must
/// be called from one thread, and in paper mode that is the trading thread.
///
/// This is a deliberate contract, not an oversight. A real venue adapter has a
/// socket whose I/O genuinely overlaps the trading thread, so it needs internal
/// synchronization; a simulator has no I/O to overlap, and adding a lock would
/// buy nothing while making the simulator's timing depend on lock contention
/// that production would not have.
///
/// The concurrency in this architecture is therefore unchanged from Phase 8 and
/// lives entirely in the OMS: the adapter emits an event, the OMS enqueues it
/// on whichever thread the adapter runs on, and the trading thread drains the
/// ring. That boundary -- not this class -- is what
/// tests/integration/test_execution_concurrency.cpp exercises under TSan.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mm/common/Time.hpp"
#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/exchange/common/IExchangeExecution.hpp"
#include "mm/exchange/paper/PaperMatcher.hpp"
#include "mm/exchange/paper/PaperOrder.hpp"
#include "mm/exchange/paper/PaperTypes.hpp"
#include "mm/common/Config.hpp"
#include "mm/orderbook/OrderBook.hpp"

namespace mm::exchange::paper {

/// Capabilities a paper venue declares. Modelled on a spot venue that offers
/// atomic replace and post-only, because those are the paths the engine needs
/// exercised. `supports_replace` follows `ReplaceMode`, so a test can take
/// atomic replace away and watch the quote manager decompose it.
[[nodiscard]] ExchangeCapabilities paper_capabilities(ReplaceMode mode) noexcept;

/// Builds the venue's configuration from the engine's YAML block.
///
/// Kept here rather than in `mm_common` so the config layer never has to name
/// a venue type -- the adapter reads the config, not the other way round.
/// Returns an error for a value the parser accepted but this venue cannot
/// honour, rather than silently substituting a default: a paper run whose fill
/// model was quietly not the one asked for is worse than one that refused to
/// start.
[[nodiscard]] Result<PaperExecutionConfig> paper_config_from(const PaperConfig& yaml);

class PaperExecution final : public IExchangeExecution {
public:
    PaperExecution(PaperExecutionConfig config, const Clock& clock);

    // ------------------------------------------------------- IExchangeExecution

    [[nodiscard]] Status start(IExecutionSink& sink) override;
    void stop() override;

    [[nodiscard]] Status submit(const OrderRequest& request) override;
    [[nodiscard]] Status cancel(const CancelRequest& request) override;
    [[nodiscard]] Status replace(const ReplaceRequest& request) override;
    [[nodiscard]] Status cancel_all(const Symbol& symbol) override;

    [[nodiscard]] Status query_open_orders(const Symbol& symbol) override;
    [[nodiscard]] Status query_order(const ClientOrderId& client_order_id,
                                     const ExchangeOrderId& exchange_order_id) override;
    [[nodiscard]] Status query_balances() override;
    [[nodiscard]] Status query_positions() override;

    [[nodiscard]] ConnectionState connection_state() const override { return connection_; }
    [[nodiscard]] bool is_authenticated() const override { return authenticated_; }
    [[nodiscard]] std::size_t in_flight_requests() const override {
        return in_flight_.size() + pending_.size();
    }
    [[nodiscard]] const ExchangeCapabilities& capabilities() const override { return caps_; }
    [[nodiscard]] VenueName venue() const override { return config_.venue; }

    // ------------------------------------------------------------ market data
    //
    // Paper execution is another consumer of the engine's normalized market
    // state (§29). It does not parse a feed, does not hold a second book, and
    // does not know which venue the data came from -- it reads the same
    // `book::OrderBook` the strategy reads.

    /// Registers the engine's book for a symbol. The book must outlive this.
    void attach_book(const Symbol& symbol, const book::OrderBook& book);

    /// Told that `symbol`'s book has changed as of `update_ns`. This is what
    /// drives matching: fills are a consequence of the market moving, never of
    /// a timer.
    void on_market_update(const Symbol& symbol, Nanos update_ns);

    // ------------------------------------------------------------- event pump

    /// Delivers every event whose simulated latency has elapsed, in
    /// (due time, emission order) order. Returns how many were delivered.
    ///
    /// This is the *only* path from the venue to the sink. Called by whoever
    /// owns the execution thread; in tests, by the test.
    std::size_t poll();

    /// Requests whose answers are still pending, for tests and metrics.
    [[nodiscard]] std::size_t pending_events() const noexcept { return pending_.size(); }

    // ----------------------------------------------------------- simulation

    /// Moves the venue's connection state, emitting the transition (§19).
    void set_connection(ConnectionState state, ExchangeError error = {});
    void set_authenticated(bool authenticated) noexcept { authenticated_ = authenticated; }

    /// Deterministic failure injection. Counts down; never random (§18, §32).
    void inject(const PaperFaults& faults) { config_.faults = faults; }
    [[nodiscard]] PaperFaults& faults() noexcept { return config_.faults; }

    [[nodiscard]] const PaperMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const PaperExecutionConfig& config() const noexcept { return config_; }

    /// The venue's own view, for assertions. Deliberately read-only: a test
    /// that could reach in and change venue state would be able to hide the
    /// divergence it exists to detect (§24).
    [[nodiscard]] const PaperOrder* find_order(const ClientOrderId& id) const;
    [[nodiscard]] const PaperOrder* find_order(const ExchangeOrderId& id) const;
    [[nodiscard]] std::size_t resting_order_count() const;
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }
    /// A copy of the venue's order table, for replay comparison and tests.
    /// A copy, not a handle: nothing outside may hold a pointer it could write
    /// through (§24).
    [[nodiscard]] std::vector<PaperOrder> snapshot_orders() const { return orders_; }

private:
    /// A request in flight to the venue.
    ///
    /// Requests are held for `latency.request_ns` before the venue looks at
    /// them, so an order does not exist at the venue the instant `submit()`
    /// returns. Collapsing the two would make `request_ns` meaningless and --
    /// worse -- would let an order fill in a window when the venue had not yet
    /// received it.
    struct PendingRequest {
        Nanos due_ns = 0;
        std::uint64_t ordinal = 0;
        RequestKind kind = RequestKind::None;
        OrderRequest submit{};
        CancelRequest cancel{};
        ReplaceRequest replace{};
        /// Extra latency injected for this request only.
        Nanos decision_delay_ns = 0;
    };

    /// An event waiting out its simulated latency.
    struct Scheduled {
        Nanos due_ns = 0;
        /// Emission order, so equal due times resolve deterministically rather
        /// than by whatever order a container happens to hold them in.
        std::uint64_t ordinal = 0;
        ExecutionEvent event{};
    };

    [[nodiscard]] Status require_connected() const;
    [[nodiscard]] bool consume(std::uint32_t& counter) noexcept;
    [[nodiscard]] Nanos take_request_delay() noexcept;

    void schedule(Nanos delay, ExecutionEvent event);
    /// Schedules at an absolute time. Answers are due relative to when the
    /// request *arrived* at the venue, not to whenever `poll()` happened to
    /// run -- otherwise a late poll would silently stretch every latency.
    void schedule_at(Nanos due_ns, ExecutionEvent event);
    void emit(const ExecutionEvent& event);
    [[nodiscard]] ExecutionEvent make_event(ExecutionEventType type) const;
    [[nodiscard]] ExchangeOrderId next_exchange_id();
    [[nodiscard]] TradeId next_trade_id();
    [[nodiscard]] Seq next_seq() noexcept { return ++seq_; }

    [[nodiscard]] PaperOrder* mutable_find(const ClientOrderId& id);

    void enqueue_request(PendingRequest request);
    void process_request(const PendingRequest& request);
    /// When the request being processed arrived, so answers can be dated from
    /// it rather than from the current clock.
    Nanos processing_arrival_ns_ = 0;
    void process_submit(const OrderRequest& request, Nanos decision_delay);
    void process_cancel(const CancelRequest& request, Nanos decision_delay);
    void process_replace(const ReplaceRequest& request, Nanos decision_delay);
    void accept_order(const OrderRequest& request, Nanos base_delay);
    void reject_order(const OrderRequest& request, RejectReason reason, Nanos delay);
    void apply_match(PaperOrder& order, const MatchResult& match, Nanos at_ns);
    void schedule_fill(const PaperOrder& order, const MatchResult& match, Nanos at_ns);
    void match_symbol(const Symbol& symbol, Nanos now_ns);
    void resolve_pending_cancel(ExecutionEvent& event);

    PaperExecutionConfig config_;
    const Clock& clock_;
    IExecutionSink* sink_ = nullptr;
    ExchangeCapabilities caps_{};

    ConnectionState connection_ = ConnectionState::Disconnected;
    bool authenticated_ = false;
    bool started_ = false;

    /// The venue's orders, in arrival order. Its own state, not the OMS's.
    std::vector<PaperOrder> orders_;
    std::map<ClientOrderId, std::size_t, std::less<>> by_client_id_;
    std::map<ExchangeOrderId, std::size_t, std::less<>> by_exchange_id_;

    /// Engine books this venue reads. Never owned, never written.
    std::map<Symbol, const book::OrderBook*, std::less<>> books_;
    std::map<Symbol, Nanos, std::less<>> book_updated_ns_;

    std::vector<Scheduled> pending_;
    std::vector<PendingRequest> in_flight_;


    std::uint64_t next_exchange_seq_ = 1;
    std::uint64_t next_trade_seq_ = 1;
    std::uint64_t next_arrival_seq_ = 1;
    std::uint64_t next_ordinal_ = 0;
    Seq seq_ = 0;

    PaperMetrics metrics_{};
};

}  // namespace mm::exchange::paper
