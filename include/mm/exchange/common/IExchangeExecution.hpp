#pragma once

/// \file IExchangeExecution.hpp
/// The execution side of the exchange boundary.
///
/// **Everything is asynchronous.** `submit`, `cancel` and `replace` return a
/// `Status` describing only whether the request was successfully *handed off*.
/// They never report what the venue did, because at the moment they return the
/// venue has not been asked yet. Outcomes arrive later through the sink.
///
/// This split is what preserves the unknown-state distinction. A synchronous
/// `submit` returning an error would force the caller to guess whether the
/// order exists; here, a definitive refusal is an `OrderRejectEvent` and an
/// uncertain one is a `RequestFailureEvent` carrying `RequestOutcome::Unknown`,
/// and the two can never be confused.
///
/// **Threading contract.** Sink callbacks run on the adapter's I/O thread. The
/// engine's implementation stamps the event and pushes it into an `SpscRing`;
/// it touches no trading state.

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/exchange/common/OrderRequest.hpp"

namespace mm::exchange {

/// Implemented by the engine; called by the adapter on the I/O thread.
class IExecutionSink {
public:
    virtual ~IExecutionSink() = default;

    /// The reference is valid only for the duration of the call.
    virtual void on_execution(const ExecutionEvent& event) = 0;
};

/// A venue's order-entry connectivity.
///
/// Both `PaperExecution` and a future `BinanceExecution` implement this
/// unchanged; that identity is what makes paper/live parity structural rather
/// than a property somebody has to remember to maintain.
class IExchangeExecution {
public:
    virtual ~IExchangeExecution() = default;

    IExchangeExecution() = default;
    IExchangeExecution(const IExchangeExecution&) = delete;
    IExchangeExecution& operator=(const IExchangeExecution&) = delete;

    /// Binds the sink, authenticates, and opens the private stream. Progress
    /// arrives asynchronously as `ConnectionEvent`. The sink must outlive this.
    [[nodiscard]] virtual Status start(IExecutionSink& sink) = 0;

    /// Stops I/O and joins threads. Idempotent. Does **not** cancel resting
    /// orders -- that is a trading decision the engine makes deliberately
    /// during shutdown, not a side effect of closing a socket.
    virtual void stop() = 0;

    // ------------------------------------------------------------- order entry

    /// Hands off an order.
    ///
    /// A returned error means the request was **not sent** and no order exists:
    /// validation failed, the transport is down, or the local queue is full.
    /// A returned `ok` means only that it was handed off; the outcome arrives
    /// as `OrderAckEvent`, `OrderRejectEvent`, or -- when the venue's answer
    /// never comes -- `RequestFailureEvent` with `RequestOutcome::Unknown`.
    [[nodiscard]] virtual Status submit(const OrderRequest& request) = 0;

    /// Requests a cancel. Success here does not mean the order is gone; only a
    /// confirmed `OrderCancelEvent` means that. A cancel that is rejected or
    /// times out leaves the order working, and exposure must not be released.
    [[nodiscard]] virtual Status cancel(const CancelRequest& request) = 0;

    /// Atomic cancel-replace. Callers must check
    /// `capabilities().supports_replace` first; where unsupported this returns
    /// an error rather than silently decomposing into cancel-then-submit, since
    /// the two have different exposure profiles and that choice belongs to the
    /// quote manager.
    [[nodiscard]] virtual Status replace(const ReplaceRequest& request) = 0;

    /// Cancels every resting order, optionally limited to one symbol. Only
    /// meaningful when `capabilities().supports_mass_cancel`.
    [[nodiscard]] virtual Status cancel_all(const Symbol& symbol) = 0;

    // ---------------------------------------------------------------- queries
    //
    // All asynchronous; answers arrive through the sink. These are the inputs
    // to startup reconciliation and to the periodic truth check against the
    // venue, so they must be able to run while trading continues.

    /// Answered by one or more `OpenOrdersSnapshotEvent`. An empty symbol means
    /// account-wide. Reconciliation may only conclude an order is absent after
    /// the chunk marked `is_last`.
    [[nodiscard]] virtual Status query_open_orders(const Symbol& symbol) = 0;

    /// Answered by `OrderStatusReport`, or by `RequestFailureEvent` with
    /// `UnknownOrderState` when the venue cannot say.
    [[nodiscard]] virtual Status query_order(const ClientOrderId& client_order_id,
                                             const ExchangeOrderId& exchange_order_id) = 0;

    /// Answered by `BalanceUpdateEvent` per asset.
    [[nodiscard]] virtual Status query_balances() = 0;

    /// Answered by `PositionUpdateEvent` per symbol. Spot venues report
    /// `capabilities().has_positions == false` and this returns unsupported.
    [[nodiscard]] virtual Status query_positions() = 0;

    // --------------------------------------------------------------- status

    [[nodiscard]] virtual ConnectionState connection_state() const = 0;

    /// Distinct from being connected: a socket can be open while credentials
    /// have expired, and orders sent in that window are refused.
    [[nodiscard]] virtual bool is_authenticated() const = 0;

    /// Requests sent but not yet resolved. The safety layer watches this: a
    /// growing count means the venue has stopped answering.
    [[nodiscard]] virtual std::size_t in_flight_requests() const = 0;

    [[nodiscard]] virtual const ExchangeCapabilities& capabilities() const = 0;
    [[nodiscard]] virtual VenueName venue() const = 0;
};

}  // namespace mm::exchange
