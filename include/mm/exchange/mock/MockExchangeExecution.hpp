#pragma once

/// \file MockExchangeExecution.hpp
/// A deterministic in-memory implementation of `IExchangeExecution`.
///
/// This is **not** the paper-trading engine. It has no market model, no queue
/// simulation and no fee schedule; it exists to prove the exchange abstraction
/// is implementable and to let later phases test the OMS against every venue
/// behaviour -- including the ones a real venue produces only rarely and never
/// on demand: an ack that never arrives, a cancel that loses a race with a fill,
/// a redelivered execution report.
///
/// Determinism is the point. No threads, no wall clock, no randomness. Time
/// advances only when the test advances a `ManualClock`, and events are
/// delivered only when the test calls `pump()`. A test that passes here passes
/// every time.

#include <deque>
#include <unordered_map>
#include <vector>

#include "mm/common/Time.hpp"
#include "mm/exchange/common/IExchangeExecution.hpp"

namespace mm::exchange::mock {

/// How the mock should answer the next submit.
enum class SubmitBehaviour : std::uint8_t {
    /// Venue accepts; an `OrderAckEvent` is delivered after `ack_latency`.
    Ack = 0,
    /// Venue refuses; an `OrderRejectEvent` with `RequestOutcome::Rejected`.
    Reject,
    /// No answer ever comes. After `ack_timeout` a `RequestFailureEvent` with
    /// `Timeout`/`Unknown` is delivered. The order may or may not exist -- the
    /// scenario the whole unknown-state design is built for.
    Timeout,
    /// The socket dies mid-flight: `Transport`/`Unknown`.
    TransportLoss,
    /// Accepted, then immediately and fully filled.
    AckThenFullFill,
    /// Accepted, then partially filled for half the quantity.
    AckThenPartialFill,
};

enum class CancelBehaviour : std::uint8_t {
    /// Confirmed cancelled.
    Accept = 0,
    /// Venue refuses the cancel. **The order is still working.**
    Reject,
    /// Venue has no such order.
    NotFound,
    /// The order filled while the cancel was in flight.
    TooLate,
    /// No answer: `Timeout`/`Unknown`. Exposure must not be released.
    Timeout,
};

/// Phase 8 (§39). Replace has its own outcomes: a venue can refuse the
/// amendment while leaving the original order exactly where it is.
enum class ReplaceBehaviour : std::uint8_t {
    /// The amendment is applied and a new venue identity is issued.
    Accept,
    /// Refused. The original order is untouched and still working.
    Reject,
    /// The venue has never heard of the order being amended.
    NotFound,
    /// No answer at all.
    Timeout,
};


/// Capabilities of a venue that supports essentially everything, so tests opt
/// out of features rather than in.
[[nodiscard]] ExchangeCapabilities permissive_mock_capabilities() noexcept;
/// A minimal spot venue: no replace, no reduce-only, no positions.
[[nodiscard]] ExchangeCapabilities minimal_mock_capabilities() noexcept;

class MockExchangeExecution final : public IExchangeExecution {
public:
    MockExchangeExecution(ManualClock& clock, ExchangeCapabilities caps);
    explicit MockExchangeExecution(ManualClock& clock);
    ~MockExchangeExecution() override;

    // ----------------------------------------------------- IExchangeExecution
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
    [[nodiscard]] std::size_t in_flight_requests() const override { return pending_.size(); }
    [[nodiscard]] const ExchangeCapabilities& capabilities() const override { return caps_; }
    [[nodiscard]] VenueName venue() const override { return VenueName("mock"); }

    // ------------------------------------------------------------- scripting

    void set_default_submit_behaviour(SubmitBehaviour b) noexcept { default_submit_ = b; }
    void set_default_cancel_behaviour(CancelBehaviour b) noexcept { default_cancel_ = b; }
    void set_default_replace_behaviour(ReplaceBehaviour b) noexcept { default_replace_ = b; }

    /// Queued one-shot overrides, consumed in order before the default applies.
    void script_submit(SubmitBehaviour b) { scripted_submits_.push_back(b); }
    void script_cancel(CancelBehaviour b) { scripted_cancels_.push_back(b); }
    void script_replace(ReplaceBehaviour b) { scripted_replaces_.push_back(b); }

    void set_reject_reason(RejectReason r) noexcept { reject_reason_ = r; }
    void set_ack_latency(Nanos ns) noexcept { ack_latency_ = ns; }
    void set_ack_timeout(Nanos ns) noexcept { ack_timeout_ = ns; }

    void set_connected(bool connected);
    void set_authenticated(bool authenticated) noexcept { authenticated_ = authenticated; }
    /// Drops the connection. In-flight requests become `Unknown`, not rejected:
    /// orders sent before the drop may well be resting on the venue.
    void disconnect(std::string_view reason);
    void reconnect();

    /// Delivers a fill against a live order. Returns an error if the order is
    /// not live or the quantity would exceed what remains, so a test cannot
    /// accidentally script a physically impossible sequence.
    [[nodiscard]] Status deliver_fill(const ClientOrderId& id, Px price, Qty quantity,
                                      Liquidity liquidity, const TradeId& trade_id);
    [[nodiscard]] Status deliver_full_fill(const ClientOrderId& id, Liquidity liquidity,
                                           const TradeId& trade_id);

    /// Re-sends the most recent event verbatim. Venues do this after a private
    /// stream reconnect, and the OMS must be idempotent against it.
    [[nodiscard]] Status redeliver_last_event();

    /// Phase 8 (§39). Emits an acknowledgement for an order the venue has
    /// already reported on, so a test can deliver events in an order the OMS
    /// would never have produced -- the late ack that arrives after its own
    /// fill. Real venues do this whenever two streams race.
    [[nodiscard]] Status deliver_late_ack(const ClientOrderId& id);

    /// Phase 8 (§39). Emits a fill without the mock's own sanity checks, so a
    /// test can deliver one larger than the order it claims to fill.
    [[nodiscard]] Status deliver_fill_unchecked(const ClientOrderId& id, Px price, Qty quantity,
                                                const TradeId& trade_id);

    /// Phase 8 (§39). Emits an event naming a client order id the engine never
    /// issued. Nothing local should move as a result.
    [[nodiscard]] Status deliver_event_for_unknown_order(const ClientOrderId& id);

    /// Holds every scheduled event until released, so a test can control the
    /// exact delivery order of events the venue produced independently.
    void hold_events(bool held) noexcept { events_held_ = held; }

    void deliver_balance(const Asset& asset, Qty free, Qty locked);
    void deliver_position(const Symbol& symbol, Qty net_quantity, Px average_entry);

    // ---------------------------------------------------------------- driving

    /// Delivers every event whose due time has arrived. Returns how many were
    /// delivered. Call after advancing the clock.
    std::size_t pump();

    /// Events queued but not yet due.
    [[nodiscard]] std::size_t queued_events() const noexcept { return queue_.size(); }

    /// Orders the mock venue currently considers live.
    [[nodiscard]] std::size_t live_order_count() const noexcept;

    [[nodiscard]] bool has_order(const ClientOrderId& id) const;
    [[nodiscard]] OrderStatus order_status(const ClientOrderId& id) const;

    /// Total submits the venue actually received, for asserting that a request
    /// refused locally never reached it.
    [[nodiscard]] std::uint64_t received_submits() const noexcept { return received_submits_; }

private:
    struct MockOrder {
        OrderRequest request{};
        ExchangeOrderId exchange_id{};
        Qty cumulative{};
        OrderStatus status = OrderStatus::Unknown;
        Seq seq = 0;
    };

    struct Scheduled {
        Nanos due_ns = 0;
        std::uint64_t ordinal = 0;  ///< ties broken by insertion order, never by chance
        ExecutionEvent event{};
    };

    void schedule(Nanos delay, const ExecutionEvent& event);
    void emit_now(const ExecutionEvent& event);
    [[nodiscard]] ExchangeOrderId next_exchange_id();
    [[nodiscard]] TradeId next_trade_id();
    [[nodiscard]] SubmitBehaviour take_submit_behaviour();
    [[nodiscard]] CancelBehaviour take_cancel_behaviour();
    [[nodiscard]] ReplaceBehaviour take_replace_behaviour();
    void stamp(ExecutionEvent& event) const;

    ManualClock& clock_;
    ExchangeCapabilities caps_{};
    IExecutionSink* sink_ = nullptr;

    ConnectionState connection_ = ConnectionState::Disconnected;
    bool authenticated_ = false;
    bool started_ = false;

    SubmitBehaviour default_submit_ = SubmitBehaviour::Ack;
    CancelBehaviour default_cancel_ = CancelBehaviour::Accept;
    ReplaceBehaviour default_replace_ = ReplaceBehaviour::Accept;
    std::deque<SubmitBehaviour> scripted_submits_;
    std::deque<CancelBehaviour> scripted_cancels_;
    std::deque<ReplaceBehaviour> scripted_replaces_;
    RejectReason reject_reason_ = RejectReason::InsufficientBalance;

    Nanos ack_latency_ = micros(500);
    Nanos ack_timeout_ = millis(5'000);

    std::vector<Scheduled> queue_;
    std::unordered_map<ClientOrderId, MockOrder> orders_;
    /// Client order ids of requests sent but not yet resolved.
    std::vector<ClientOrderId> pending_;

    std::uint64_t next_exchange_id_ = 1;
    std::uint64_t next_trade_id_ = 1;
    std::uint64_t ordinal_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t received_submits_ = 0;
    ExecutionEvent last_event_{};
    bool has_last_event_ = false;
    bool events_held_ = false;
};

}  // namespace mm::exchange::mock
