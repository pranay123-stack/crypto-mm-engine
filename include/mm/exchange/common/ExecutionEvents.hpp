#pragma once

/// \file ExecutionEvents.hpp
/// Normalized inbound execution events.
///
/// These are the only way the engine learns what happened to an order. Nothing
/// above the adapter infers order state from the fact that a request was sent:
/// an order exists when an execution event says it does, and not before.

#include <cstdint>

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeError.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"  // ConnectionEvent

namespace mm::exchange {

/// Which request a failure or response refers to. Needed because the recovery
/// action differs: an unknown submit may have created an order, while an
/// unknown cancel may have left one working.
enum class RequestKind : std::uint8_t {
    None = 0,
    Submit,
    Cancel,
    Replace,
    QueryOrder,
    QueryOpenOrders,
    QueryBalances,
    QueryPositions,
    MassCancel,
};
[[nodiscard]] std::string_view to_string(RequestKind k) noexcept;

/// The venue accepted an order onto the book.
struct OrderAckEvent {
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Px price{};
    Qty original_qty{};
    /// Already-filled quantity at ack time. Non-zero when an order filled
    /// immediately and the venue coalesced the ack with the fill.
    Qty cumulative_qty{};

    OrderStatus status = OrderStatus::New;
    ExecutionType exec_type = ExecutionType::New;

    /// Venue transaction time (wall clock) and its update sequence.
    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
    TraceId trace = kNoTrace;
};

/// The venue refused the order. This is a definitive statement: the order does
/// not exist and never will. Anything less certain is a `RequestFailureEvent`.
struct OrderRejectEvent {
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};  ///< usually empty
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// Always carries `RequestOutcome::Rejected`; anything else here would be a
    /// contradiction and is caught by `validate_execution_event`.
    ExchangeError error{};

    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
    TraceId trace = kNoTrace;
};

struct OrderCancelEvent {
    ClientOrderId client_order_id{};        ///< the cancelled order
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    CancelStatus cancel_status = CancelStatus::Unknown;
    /// Where the order stands after the attempt. On a cancel reject this is
    /// still New or PartiallyFilled -- the order is working.
    OrderStatus order_status = OrderStatus::Unknown;

    Qty cumulative_qty{};
    Qty leaves_qty{};
    ExchangeError error{};

    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
    TraceId trace = kNoTrace;
};

struct OrderReplaceEvent {
    ClientOrderId original_client_order_id{};
    ClientOrderId new_client_order_id{};
    ExchangeOrderId original_exchange_order_id{};
    ExchangeOrderId new_exchange_order_id{};
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// True when the venue confirmed the swap. When false, `error` says whether
    /// the original is still working (reject) or the outcome is unknown.
    bool accepted = false;
    OrderStatus new_status = OrderStatus::Unknown;
    Px new_price{};
    Qty new_qty{};
    Qty cumulative_qty{};
    ExchangeError error{};

    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
    TraceId trace = kNoTrace;
};

/// A trade against one of our orders.
struct FillEvent {
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    /// The venue's trade identifier. This is the idempotency key: the OMS keeps
    /// the set of trade ids seen per order, because venues do redeliver
    /// execution reports after a user-stream reconnect and a double-counted
    /// fill corrupts position and PnL simultaneously.
    TradeId trade_id{};

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    Side side = Side::Buy;

    Px price{};
    Qty quantity{};

    /// Running totals as the venue sees them, used to cross-check our own.
    Qty cumulative_qty{};
    Qty leaves_qty{};

    /// Fees are charged in an asset that is often neither the base nor the
    /// quote (a venue's own token), so the asset travels with the amount.
    Notional fee{};
    Asset fee_asset{};
    Liquidity liquidity = Liquidity::Unknown;

    OrderStatus order_status = OrderStatus::PartiallyFilled;

    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
    TraceId trace = kNoTrace;
};

struct BalanceUpdateEvent {
    Asset asset{};
    Qty free{};
    Qty locked{};
    Nanos transact_ns = 0;
    Seq seq = kNoSeq;

    [[nodiscard]] Qty total() const noexcept { return free + locked; }
};

struct PositionUpdateEvent {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    /// Signed: negative is short. `Qty` is a signed fixed-point type, so no
    /// separate direction field is needed and none can disagree with the sign.
    Qty net_quantity{};
    Px average_entry_price{};
    Notional unrealized_pnl{};
    Nanos transact_ns = 0;
    Seq seq = kNoSeq;
};

/// A request that failed **without the venue telling us it refused the order**.
///
/// This event is the whole point of the unknown-state design. A REST timeout,
/// a socket dying mid-write, an unparseable response -- all arrive here, never
/// as an `OrderRejectEvent`, and the OMS must treat the affected order as
/// possibly-live until reconciliation proves otherwise.
struct RequestFailureEvent {
    RequestKind kind = RequestKind::None;
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    /// Carries the outcome. `Unknown` obliges reconciliation; `NotSent` does not.
    ExchangeError error{};

    TraceId trace = kNoTrace;
};

/// One order as the venue currently reports it, used to answer open-order
/// queries and single-order status queries during reconciliation.
struct OrderStatusReport {
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Px price{};
    Qty original_qty{};
    Qty cumulative_qty{};
    OrderStatus status = OrderStatus::Unknown;
    Nanos transact_ns = 0;
};

/// Orders per open-orders chunk.
///
/// Kept small on purpose. This payload is the largest member of the
/// `ExecutionEvent` union, so it sets the ring slot size for *every* execution
/// event -- and open-order snapshots arrive only at startup and during periodic
/// reconciliation, while fills and acks arrive constantly. At 8 orders per
/// chunk the envelope was 1432 bytes, making every 224-byte fill pay a 6x
/// copy tax. Four keeps the envelope under 700 bytes at the cost of one extra
/// chunk per eight open orders, which is a trade worth making in that direction.
inline constexpr std::size_t kMaxOrdersPerSnapshotEvent = 4;

/// Open orders as the venue sees them. Chunked, for the same reason book
/// snapshots are: the reply is unbounded and the ring slot is not.
///
/// This event reports only success. A failed or unanswered query arrives as a
/// `RequestFailureEvent` with `RequestKind::QueryOpenOrders`, so there is
/// exactly one channel for failure rather than two that could disagree.
///
/// `is_last` matters more here than convenience: reconciliation may only
/// conclude "the venue does not have this order" after a *complete* listing.
/// Acting on a partial one would cancel-or-forget live orders.
struct OpenOrdersSnapshotEvent {
    std::array<OrderStatusReport, kMaxOrdersPerSnapshotEvent> orders{};
    std::uint8_t count = 0;
    bool is_first = true;
    bool is_last = true;
    /// Empty means account-wide.
    Symbol symbol_filter{};
};

enum class ExecutionEventType : std::uint8_t {
    None = 0,
    OrderAck,
    OrderReject,
    OrderCancel,
    OrderReplace,
    Fill,
    BalanceUpdate,
    PositionUpdate,
    Connection,
    RequestFailure,
    OpenOrdersSnapshot,
    OrderStatus,
};
[[nodiscard]] std::string_view to_string(ExecutionEventType t) noexcept;

struct ExecutionEvent {
    ExecutionEventType type = ExecutionEventType::None;
    EventStamps stamps{};

    union Payload {
        Payload() noexcept : open_orders{} {}
        OrderAckEvent ack;
        OrderRejectEvent reject;
        OrderCancelEvent cancel;
        OrderReplaceEvent replace;
        FillEvent fill;
        BalanceUpdateEvent balance;
        PositionUpdateEvent position;
        ConnectionEvent connection;
        RequestFailureEvent failure;
        OpenOrdersSnapshotEvent open_orders;
        OrderStatusReport order_status;
    } payload;

    [[nodiscard]] ClientOrderId client_order_id() const noexcept;
    [[nodiscard]] Symbol symbol() const noexcept;
    /// What this event implies about the referenced order's existence.
    [[nodiscard]] RequestOutcome outcome() const noexcept;
};

static_assert(std::is_trivially_copyable_v<ExecutionEvent>,
              "execution events cross a ring by value and must copy trivially");

/// Checks an event for self-contradiction before the OMS acts on it: a reject
/// that does not carry `Rejected`, a failure that claims `Acknowledged`, a
/// fill whose cumulative quantity moves backwards. An adapter bug caught here
/// is a rejected event; the same bug uncaught is a corrupted position.
[[nodiscard]] Status validate_execution_event(const ExecutionEvent& event) noexcept;

}  // namespace mm::exchange
