#pragma once

/// \file OrderManager.hpp
/// The authoritative owner of order lifecycle.
///
/// ## What it owns
///
/// After this phase the OMS is the single source of truth for order state. The
/// quote manager consumes `working_state()`; risk consumes `exposure()`. Neither
/// runs a competing state machine, because two opinions about whether an order
/// is live is one too many.
///
/// ## What it does not decide
///
/// Whether an order should exist (strategy and quote manager), whether it is
/// financially permitted (risk), or how it reaches a venue (the adapter). The
/// OMS answers exactly one question: *what is the lifecycle state of every
/// order?*
///
/// ## Risk cannot be routed around
///
/// `submit()` accepts only a `risk::ApprovedAction`, whose validity flag only
/// `RiskEngine` can set. A caller cannot construct one, so there is no way to
/// hand the OMS an order that risk has not seen.
///
/// ## Concurrency
///
/// One authoritative mutation path. Execution callbacks arrive on the exec-io
/// thread and do nothing but *enqueue* a normalized event; the OMS thread drains
/// the queue and applies transitions deterministically. That separation is what
/// makes the same event sequence produce the same final state, and therefore
/// what makes future incident replay possible.
///
/// ## Paper and live are identical
///
/// There is no mode flag anywhere in this file. Whichever `IExchangeExecution`
/// is attached decides paper or live, below this boundary.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mm/common/LatencyHistogram.hpp"
#include "mm/common/SpscRing.hpp"
#include "mm/exchange/common/IExchangeExecution.hpp"
#include "mm/oms/ClientOrderIdGenerator.hpp"
#include "mm/oms/OrderJournal.hpp"
#include "mm/oms/Reconciliation.hpp"
#include "mm/quote/WorkingQuoteState.hpp"
#include "mm/risk/PositionState.hpp"
#include "mm/risk/RiskDecision.hpp"

namespace mm::oms {

struct OmsConfig {
    /// How long a request may go unanswered before the order becomes `Unknown`.
    /// A timeout never fabricates success or failure -- it records that we do
    /// not know, which is the only thing a timeout actually tells us.
    Nanos new_request_timeout_ns = seconds(5);
    Nanos cancel_request_timeout_ns = seconds(5);
    Nanos replace_request_timeout_ns = seconds(5);
    /// How long an unresolved discrepancy may persist before it is escalated.
    Nanos reconciliation_timeout_ns = seconds(30);

    /// Client order id shape. `session_id` must differ between runs, or a
    /// restarted process cannot tell its own previous orders from current ones.
    InlineString<16> client_id_prefix{};
    std::uint64_t session_id = 0;
    std::uint32_t max_client_id_length = 36;

    /// Order records are pre-allocated; exhaustion is a bounded, observable
    /// condition rather than an allocation on the hot path.
    std::size_t max_orders = 256;
};

struct OmsMetrics {
    std::uint64_t orders_created = 0;
    std::uint64_t new_requests = 0;
    std::uint64_t cancel_requests = 0;
    std::uint64_t replace_requests = 0;
    std::uint64_t acknowledgements = 0;
    std::uint64_t rejects = 0;
    std::uint64_t fills = 0;
    std::uint64_t partial_fills = 0;

    std::uint64_t duplicate_events = 0;
    std::uint64_t duplicate_fills = 0;
    std::uint64_t duplicate_requests = 0;
    std::uint64_t out_of_order_events = 0;
    std::uint64_t quarantined_events = 0;
    std::uint64_t illegal_transitions = 0;

    std::uint64_t unknown_states = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t stale_operations = 0;
    std::uint64_t ownership_violations = 0;
    std::uint64_t deferred_news = 0;

    std::uint64_t reconciliations = 0;
    std::uint64_t reconciliations_refused = 0;
    std::uint64_t orphan_orders = 0;
    std::uint64_t missing_orders = 0;
    std::uint64_t not_risk_approved = 0;

    std::array<std::uint64_t, 20> rejects_by_reason{};

    [[nodiscard]] std::uint64_t count(OmsReject reason) const noexcept {
        const auto i = static_cast<std::size_t>(reason);
        return i < rejects_by_reason.size() ? rejects_by_reason[i] : 0;
    }
};

/// The outcome of handing the OMS an approved action.
struct SubmitResult {
    bool accepted = false;
    OmsReject reject = OmsReject::None;
    LogicalOrderId logical_id = LogicalOrderId::kInvalid;
    RequestId request_id = RequestId::kInvalid;
    /// True when the action was accepted but held rather than sent -- a New
    /// waiting for a cancel to confirm on the same slot.
    bool deferred = false;
    /// The request was folded into an amendment already in flight rather than
    /// issued separately. The desired state was recorded; no second request
    /// went to the venue. Distinct from a plain rejection, where the caller's
    /// intent was dropped entirely.
    bool coalesced = false;
};

/// What must survive a restart for the OMS to recover rather than start blind.
///
/// Deliberately small and serialisable. A production OMS needs restart
/// semantics; it does not need a database, and building one here would be
/// inventing infrastructure the phase does not call for.
struct RecoveryState {
    std::uint64_t session_id = 0;
    std::uint64_t next_client_id_sequence = 0;
    std::uint64_t next_logical_id = 0;
    std::uint64_t next_request_id = 0;
    std::uint64_t journal_sequence = 0;
    std::vector<OrderRecord> orders;
};

class OrderManager final : public exchange::IExecutionSink {
public:
    OrderManager(OmsConfig config, IOrderJournal& journal, const Clock& clock);

    /// Binds the execution adapter. Paper or live -- the OMS cannot tell.
    void attach_execution(exchange::IExchangeExecution& execution) noexcept {
        execution_ = &execution;
    }

    // ------------------------------------------------------------- inbound

    /// The only way an order enters the OMS.
    ///
    /// Refuses anything without valid risk approval, which is what makes the
    /// risk boundary structural rather than conventional.
    [[nodiscard]] SubmitResult submit(const risk::ApprovedAction& approved);

    /// Called on the exec-io thread. Does nothing but enqueue: all state
    /// mutation happens on the OMS thread, in `process_events`.
    void on_execution(const exchange::ExecutionEvent& event) override;

    /// Drains the queue and applies transitions. OMS thread only. Returns the
    /// number of events applied.
    std::size_t process_events();

    /// Timeout sweep. A request past its deadline becomes `Unknown`, never
    /// cancelled or rejected.
    void on_timer();

    // ------------------------------------------------------------ outbound

    /// What the quote manager consumes. Reflects only orders owned by
    /// `owner`; anything else is counted as foreign and left alone.
    [[nodiscard]] quote::WorkingQuoteState working_state(const Symbol& symbol,
                                                          const StrategyName& owner) const;

    /// What risk consumes. `determinate` is false when any relevant order is in
    /// `Unknown`, which stops risk adding to an exposure it cannot measure.
    [[nodiscard]] risk::ExposureSnapshot exposure(const Symbol& symbol) const;

    // ------------------------------------------------------------- lookup

    [[nodiscard]] const OrderRecord* find(LogicalOrderId id) const;
    [[nodiscard]] const OrderRecord* find(const ClientOrderId& id) const;
    [[nodiscard]] const OrderRecord* find_by_exchange_id(const ExchangeOrderId& id) const;
    [[nodiscard]] std::size_t live_order_count() const;
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

    // ----------------------------------------------------- reconciliation

    /// Compares local records against a complete venue listing. Detects; does
    /// not silently correct. An incomplete snapshot is refused outright.
    [[nodiscard]] ReconciliationReport reconcile(const VenueSnapshot& snapshot);

    /// Applies the conclusion of a discrepancy an operator or a later policy
    /// has resolved. Explicit, because automatic correction of an ambiguous
    /// disagreement is how a real position gets discarded.
    [[nodiscard]] Status resolve(LogicalOrderId id, OrderState resolved_state);

    // --------------------------------------------------------- recovery

    [[nodiscard]] RecoveryState snapshot_for_recovery() const;
    /// Rebuilds from a persisted snapshot. Restored orders keep their state;
    /// anything that was pending becomes `Unknown`, because a request in flight
    /// across a restart has an outcome nobody observed.
    [[nodiscard]] Status restore(const RecoveryState& state);

    [[nodiscard]] const OmsMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const LatencyHistogram& submit_latency() const noexcept {
        return submit_latency_;
    }
    [[nodiscard]] const LatencyHistogram& event_latency() const noexcept {
        return event_latency_;
    }
    [[nodiscard]] std::uint64_t journal_sequence() const noexcept { return journal_sequence_; }
    /// Events enqueued but not yet applied.
    [[nodiscard]] std::size_t queued_events() const noexcept { return queue_.size_approx(); }

private:
    /// Held until the cancel it waits on resolves. This is the whole of the
    /// cancel-then-new guarantee: the replacement order is not created before
    /// the cancellation boundary.
    struct DeferredNew {
        bool present = false;
        quote::OrderAction action{};
        LogicalOrderId waiting_on = LogicalOrderId::kInvalid;
    };

    /// How many entries of `deferred_` are occupied.
    ///
    /// The table is sized to `max_orders` so a deferral never allocates, but
    /// deferrals are rare: they happen only while a cancel-then-new is in
    /// flight. Without this counter every order resolution would walk the whole
    /// table -- 221 KB at the shipped `max_orders: 1024` -- to almost always
    /// find nothing.
    std::size_t deferred_count_ = 0;

    [[nodiscard]] OrderRecord* mutable_find(LogicalOrderId id);
    [[nodiscard]] OrderRecord* mutable_find(const ClientOrderId& id);
    [[nodiscard]] OrderRecord* mutable_find_by_exchange_id(const ExchangeOrderId& id);
    /// The live order occupying a strategy's slot, if any.
    [[nodiscard]] OrderRecord* find_slot_order(const StrategyName& owner, const Symbol& symbol,
                                               quote::QuoteSlot slot);

    [[nodiscard]] SubmitResult handle_new(const quote::OrderAction& action);
    [[nodiscard]] SubmitResult handle_cancel(const quote::OrderAction& action);
    [[nodiscard]] SubmitResult handle_replace(const quote::OrderAction& action);

    [[nodiscard]] bool transition(OrderRecord& order, OrderState to, OrderEventType event,
                                  const OrderJournalRecord& detail);
    void journal(OrderEventType type, const OrderRecord& order, OrderState from, OrderState to,
                 OmsReject reject = OmsReject::None);
    void journal_rejection(OrderEventType type, const quote::OrderAction& action,
                           OmsReject reject);
    void record_reject(OmsReject reject);

    void apply(const exchange::ExecutionEvent& event);
    void apply_ack(const exchange::OrderAckEvent& ack);
    void apply_reject(const exchange::OrderRejectEvent& reject);
    void apply_fill(const exchange::FillEvent& fill);
    void apply_cancel(const exchange::OrderCancelEvent& cancel);
    void apply_replace(const exchange::OrderReplaceEvent& replace);
    void apply_failure(const exchange::RequestFailureEvent& failure);

    /// Releases a deferred New once the cancel it was waiting on has resolved.
    void release_deferred(LogicalOrderId resolved);

    [[nodiscard]] RequestId next_request_id() noexcept {
        return static_cast<RequestId>(++next_request_id_);
    }

    OmsConfig config_;
    ClientOrderIdGenerator id_generator_;
    IOrderJournal& journal_;
    const Clock& clock_;
    exchange::IExchangeExecution* execution_ = nullptr;

    /// Stable storage: records are never moved, so an index stays valid.
    std::vector<OrderRecord> orders_;
    std::unordered_map<ClientOrderId, std::size_t> by_client_id_;
    std::unordered_map<ExchangeOrderId, std::size_t> by_exchange_id_;
    std::unordered_map<std::uint64_t, std::size_t> by_logical_id_;

    /// One deferred New per (strategy, symbol, slot).
    std::vector<DeferredNew> deferred_;

    /// Exec-io thread produces; OMS thread consumes. The single crossing point.
    SpscRing<exchange::ExecutionEvent, 1024> queue_;
    std::uint64_t dropped_events_ = 0;

    std::uint64_t next_logical_id_ = 0;
    std::uint64_t next_request_id_ = 0;
    std::uint64_t journal_sequence_ = 0;
    std::uint64_t creation_sequence_ = 0;

    OmsMetrics metrics_{};
    LatencyHistogram submit_latency_{"oms_submit"};
    LatencyHistogram event_latency_{"oms_event"};
};

}  // namespace mm::oms
