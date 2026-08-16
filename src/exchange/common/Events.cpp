#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"

namespace mm::exchange {

std::string_view to_string(MarketDataEventType t) noexcept {
    switch (t) {
        case MarketDataEventType::None:             return "NONE";
        case MarketDataEventType::BookSnapshot:     return "BOOK_SNAPSHOT";
        case MarketDataEventType::BookUpdate:       return "BOOK_UPDATE";
        case MarketDataEventType::Trade:            return "TRADE";
        case MarketDataEventType::Bbo:              return "BBO";
        case MarketDataEventType::SessionState:     return "SESSION_STATE";
        case MarketDataEventType::Connection:       return "CONNECTION";
        case MarketDataEventType::InstrumentUpdate: return "INSTRUMENT_UPDATE";
    }
    return "UNKNOWN";
}

Symbol MarketDataEvent::symbol() const noexcept {
    switch (type) {
        case MarketDataEventType::BookSnapshot:     return payload.book_snapshot.symbol;
        case MarketDataEventType::BookUpdate:       return payload.book_update.symbol;
        case MarketDataEventType::Trade:            return payload.trade.symbol;
        case MarketDataEventType::Bbo:              return payload.bbo.symbol;
        case MarketDataEventType::SessionState:     return payload.session.symbol;
        case MarketDataEventType::InstrumentUpdate: return payload.instrument.spec.symbol;
        // Connection events are venue-wide, not per-symbol.
        case MarketDataEventType::Connection:
        case MarketDataEventType::None:
            break;
    }
    return Symbol{};
}

Seq MarketDataEvent::sequence() const noexcept {
    switch (type) {
        case MarketDataEventType::BookSnapshot: return payload.book_snapshot.last_update_id;
        case MarketDataEventType::BookUpdate:   return payload.book_update.final_update_id;
        case MarketDataEventType::Trade:        return payload.trade.seq;
        case MarketDataEventType::Bbo:          return payload.bbo.bbo.seq;
        case MarketDataEventType::SessionState:
        case MarketDataEventType::Connection:
        case MarketDataEventType::InstrumentUpdate:
        case MarketDataEventType::None:
            break;
    }
    return kNoSeq;
}

std::string_view to_string(RequestKind k) noexcept {
    switch (k) {
        case RequestKind::None:            return "NONE";
        case RequestKind::Submit:          return "SUBMIT";
        case RequestKind::Cancel:          return "CANCEL";
        case RequestKind::Replace:         return "REPLACE";
        case RequestKind::QueryOrder:      return "QUERY_ORDER";
        case RequestKind::QueryOpenOrders: return "QUERY_OPEN_ORDERS";
        case RequestKind::QueryBalances:   return "QUERY_BALANCES";
        case RequestKind::QueryPositions:  return "QUERY_POSITIONS";
        case RequestKind::MassCancel:      return "MASS_CANCEL";
    }
    return "UNKNOWN";
}

std::string_view to_string(ExecutionEventType t) noexcept {
    switch (t) {
        case ExecutionEventType::None:               return "NONE";
        case ExecutionEventType::OrderAck:           return "ORDER_ACK";
        case ExecutionEventType::OrderReject:        return "ORDER_REJECT";
        case ExecutionEventType::OrderCancel:        return "ORDER_CANCEL";
        case ExecutionEventType::OrderReplace:       return "ORDER_REPLACE";
        case ExecutionEventType::Fill:               return "FILL";
        case ExecutionEventType::BalanceUpdate:      return "BALANCE_UPDATE";
        case ExecutionEventType::PositionUpdate:     return "POSITION_UPDATE";
        case ExecutionEventType::Connection:         return "CONNECTION";
        case ExecutionEventType::RequestFailure:     return "REQUEST_FAILURE";
        case ExecutionEventType::OpenOrdersSnapshot: return "OPEN_ORDERS_SNAPSHOT";
        case ExecutionEventType::OrderStatus:        return "ORDER_STATUS";
    }
    return "UNKNOWN";
}

ClientOrderId ExecutionEvent::client_order_id() const noexcept {
    switch (type) {
        case ExecutionEventType::OrderAck:     return payload.ack.client_order_id;
        case ExecutionEventType::OrderReject:  return payload.reject.client_order_id;
        case ExecutionEventType::OrderCancel:  return payload.cancel.client_order_id;
        case ExecutionEventType::OrderReplace: return payload.replace.new_client_order_id;
        case ExecutionEventType::Fill:         return payload.fill.client_order_id;
        case ExecutionEventType::RequestFailure: return payload.failure.client_order_id;
        case ExecutionEventType::OrderStatus:  return payload.order_status.client_order_id;
        case ExecutionEventType::BalanceUpdate:
        case ExecutionEventType::PositionUpdate:
        case ExecutionEventType::Connection:
        case ExecutionEventType::OpenOrdersSnapshot:
        case ExecutionEventType::None:
            break;
    }
    return ClientOrderId{};
}

Symbol ExecutionEvent::symbol() const noexcept {
    switch (type) {
        case ExecutionEventType::OrderAck:           return payload.ack.symbol;
        case ExecutionEventType::OrderReject:        return payload.reject.symbol;
        case ExecutionEventType::OrderCancel:        return payload.cancel.symbol;
        case ExecutionEventType::OrderReplace:       return payload.replace.symbol;
        case ExecutionEventType::Fill:               return payload.fill.symbol;
        case ExecutionEventType::PositionUpdate:     return payload.position.symbol;
        case ExecutionEventType::RequestFailure:     return payload.failure.symbol;
        case ExecutionEventType::OpenOrdersSnapshot: return payload.open_orders.symbol_filter;
        case ExecutionEventType::OrderStatus:        return payload.order_status.symbol;
        case ExecutionEventType::BalanceUpdate:
        case ExecutionEventType::Connection:
        case ExecutionEventType::None:
            break;
    }
    return Symbol{};
}

RequestOutcome ExecutionEvent::outcome() const noexcept {
    switch (type) {
        // The venue spoke and confirmed the order exists.
        case ExecutionEventType::OrderAck:
        case ExecutionEventType::Fill:
        case ExecutionEventType::OrderStatus:
            return RequestOutcome::Acknowledged;

        // The venue spoke and refused it.
        case ExecutionEventType::OrderReject:
            return RequestOutcome::Rejected;

        // These carry their own verdict; a cancel reject leaves the order live.
        case ExecutionEventType::OrderCancel:  return payload.cancel.error.outcome;
        case ExecutionEventType::OrderReplace: return payload.replace.error.outcome;
        case ExecutionEventType::RequestFailure: return payload.failure.error.outcome;

        case ExecutionEventType::BalanceUpdate:
        case ExecutionEventType::PositionUpdate:
        case ExecutionEventType::Connection:
        case ExecutionEventType::OpenOrdersSnapshot:
        case ExecutionEventType::None:
            break;
    }
    return RequestOutcome::Acknowledged;
}

Status validate_execution_event(const ExecutionEvent& event) noexcept {
    switch (event.type) {
        case ExecutionEventType::None:
            return {ErrorCode::InvalidArgument, "execution event has no type"};

        case ExecutionEventType::OrderAck: {
            const OrderAckEvent& e = event.payload.ack;
            if (e.client_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "ack has no client order id"};
            }
            if (!e.original_qty.is_positive()) {
                return {ErrorCode::InvalidArgument, "ack has non-positive original quantity"};
            }
            if (e.cumulative_qty > e.original_qty) {
                return {ErrorCode::InvalidArgument, "ack cumulative quantity exceeds original"};
            }
            if (e.cumulative_qty.is_negative()) {
                return {ErrorCode::InvalidArgument, "ack cumulative quantity is negative"};
            }
            return Status::ok();
        }

        case ExecutionEventType::OrderReject: {
            const OrderRejectEvent& e = event.payload.reject;
            if (e.client_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "reject has no client order id"};
            }
            // The load-bearing check. A reject asserts the order does not
            // exist; if the adapter is not certain of that it must emit a
            // RequestFailureEvent carrying Unknown instead.
            if (e.error.outcome != RequestOutcome::Rejected) {
                return {ErrorCode::Internal,
                        "reject event does not carry RequestOutcome::Rejected - an uncertain "
                        "outcome must be reported as a RequestFailureEvent"};
            }
            if (!e.error.is_error()) {
                return {ErrorCode::InvalidArgument, "reject event carries no error"};
            }
            return Status::ok();
        }

        case ExecutionEventType::OrderCancel: {
            const OrderCancelEvent& e = event.payload.cancel;
            if (e.client_order_id.empty() && e.exchange_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "cancel event identifies no order"};
            }
            if (e.cumulative_qty.is_negative() || e.leaves_qty.is_negative()) {
                return {ErrorCode::InvalidArgument, "cancel event has negative quantities"};
            }
            // A rejected cancel means the order is still working, so reporting
            // it as terminal would silently drop live exposure.
            if (e.cancel_status == CancelStatus::Rejected && is_terminal(e.order_status) &&
                e.order_status != OrderStatus::Filled) {
                return {ErrorCode::Internal,
                        "cancel was rejected, so the order is still working, but it is "
                        "reported terminal"};
            }
            if (!is_consistent(e.error.category, e.error.outcome)) {
                return {ErrorCode::Internal, "cancel event carries an inconsistent error"};
            }
            return Status::ok();
        }

        case ExecutionEventType::OrderReplace: {
            const OrderReplaceEvent& e = event.payload.replace;
            if (e.new_client_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "replace event has no new client order id"};
            }
            if (e.accepted && e.error.is_error()) {
                return {ErrorCode::Internal, "replace is both accepted and failed"};
            }
            if (!is_consistent(e.error.category, e.error.outcome)) {
                return {ErrorCode::Internal, "replace event carries an inconsistent error"};
            }
            return Status::ok();
        }

        case ExecutionEventType::Fill: {
            const FillEvent& e = event.payload.fill;
            if (e.client_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "fill has no client order id"};
            }
            if (e.trade_id.empty()) {
                // Without a trade id there is no idempotency key, and a
                // redelivered report would double-count into position and PnL.
                return {ErrorCode::InvalidArgument, "fill has no trade id"};
            }
            if (!e.quantity.is_positive()) {
                return {ErrorCode::InvalidArgument, "fill quantity must be positive"};
            }
            if (!e.price.is_positive()) {
                return {ErrorCode::InvalidArgument, "fill price must be positive"};
            }
            if (e.cumulative_qty < e.quantity) {
                return {ErrorCode::InvalidArgument,
                        "fill cumulative quantity is below this fill's quantity"};
            }
            if (e.leaves_qty.is_negative()) {
                return {ErrorCode::InvalidArgument, "fill leaves quantity is negative"};
            }
            return Status::ok();
        }

        case ExecutionEventType::BalanceUpdate: {
            const BalanceUpdateEvent& e = event.payload.balance;
            if (e.asset.empty()) {
                return {ErrorCode::InvalidArgument, "balance update has no asset"};
            }
            if (e.free.is_negative() || e.locked.is_negative()) {
                return {ErrorCode::InvalidArgument, "balance components cannot be negative"};
            }
            return Status::ok();
        }

        case ExecutionEventType::PositionUpdate:
            if (event.payload.position.symbol.empty()) {
                return {ErrorCode::InvalidArgument, "position update has no symbol"};
            }
            return Status::ok();

        case ExecutionEventType::RequestFailure: {
            const RequestFailureEvent& e = event.payload.failure;
            if (e.kind == RequestKind::None) {
                return {ErrorCode::InvalidArgument, "request failure has no request kind"};
            }
            if (!e.error.is_error()) {
                return {ErrorCode::InvalidArgument, "request failure carries no error"};
            }
            if (e.error.outcome == RequestOutcome::Acknowledged) {
                return {ErrorCode::Internal, "request failure claims the request succeeded"};
            }
            if (!is_consistent(e.error.category, e.error.outcome)) {
                return {ErrorCode::Internal, "request failure carries an inconsistent error"};
            }
            return Status::ok();
        }

        case ExecutionEventType::OpenOrdersSnapshot: {
            const OpenOrdersSnapshotEvent& e = event.payload.open_orders;
            if (e.count > kMaxOrdersPerSnapshotEvent) {
                return {ErrorCode::OutOfRange, "open orders snapshot count exceeds capacity"};
            }
            return Status::ok();
        }

        case ExecutionEventType::OrderStatus: {
            const OrderStatusReport& e = event.payload.order_status;
            if (e.client_order_id.empty() && e.exchange_order_id.empty()) {
                return {ErrorCode::InvalidArgument, "order status identifies no order"};
            }
            if (e.cumulative_qty > e.original_qty) {
                return {ErrorCode::InvalidArgument, "order status cumulative exceeds original"};
            }
            return Status::ok();
        }

        case ExecutionEventType::Connection:
            return Status::ok();
    }
    return {ErrorCode::InvalidArgument, "unrecognised execution event type"};
}

}  // namespace mm::exchange
