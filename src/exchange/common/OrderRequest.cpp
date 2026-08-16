#include "mm/exchange/common/OrderRequest.hpp"

namespace mm::exchange {
namespace {

/// Every validation failure is definitively NotSent: we are refusing before a
/// byte is written, so the order cannot exist and reconciliation is not needed.
ExchangeError invalid(RejectReason reason, std::string_view detail) noexcept {
    return ExchangeError::make(ExchangeErrorCategory::InvalidRequest, RequestOutcome::NotSent,
                               detail, reason);
}

ExchangeError unsupported(std::string_view detail) noexcept {
    return ExchangeError::make(ExchangeErrorCategory::UnsupportedOperation,
                               RequestOutcome::NotSent, detail);
}

}  // namespace

ExchangeError validate_order(const OrderRequest& req, const InstrumentSpec& spec,
                             const ExchangeCapabilities& caps) noexcept {
    // ---- identity ----
    if (req.client_order_id.empty()) {
        return invalid(RejectReason::Unknown, "client order id is empty");
    }
    if (req.client_order_id.size() > caps.max_client_order_id_len) {
        return invalid(RejectReason::Unknown, "client order id exceeds venue length limit");
    }
    if (req.symbol.empty()) {
        return invalid(RejectReason::SymbolNotTrading, "symbol is empty");
    }
    if (!spec.is_valid()) {
        return invalid(RejectReason::SymbolNotTrading, "instrument spec is not loaded");
    }
    if (req.symbol != spec.symbol) {
        return invalid(RejectReason::SymbolNotTrading, "request symbol does not match spec");
    }
    if (spec.status != MarketStatus::Trading) {
        return invalid(RejectReason::SymbolNotTrading, "instrument is not trading");
    }

    // ---- quantity ----
    if (!req.quantity.is_positive()) {
        return invalid(RejectReason::QuantityOutOfBounds, "quantity must be positive");
    }
    if (!is_on_step(req.quantity, spec.lot_size)) {
        return invalid(RejectReason::LotSizeViolation, "quantity is not on the lot grid");
    }
    if (spec.min_qty.is_positive() && req.quantity < spec.min_qty) {
        return invalid(RejectReason::QuantityOutOfBounds, "quantity below the venue minimum");
    }
    if (spec.max_qty.is_positive() && req.quantity > spec.max_qty) {
        return invalid(RejectReason::QuantityOutOfBounds, "quantity above the venue maximum");
    }

    // ---- price ----
    if (req.requires_price()) {
        if (!req.price.is_positive()) {
            return invalid(RejectReason::PriceOutOfBounds, "limit order requires a positive price");
        }
        if (!is_on_step(req.price, spec.tick_size)) {
            return invalid(RejectReason::TickSizeViolation, "price is not on the tick grid");
        }
        if (spec.price_band_lo.is_positive() && req.price < spec.price_band_lo) {
            return invalid(RejectReason::PriceOutOfBounds, "price below the venue band");
        }
        if (spec.price_band_hi.is_positive() && req.price > spec.price_band_hi) {
            return invalid(RejectReason::PriceOutOfBounds, "price above the venue band");
        }
        // Minimum notional only means anything once a price is known.
        if (spec.min_notional.is_positive() && req.notional() < spec.min_notional) {
            return invalid(RejectReason::NotionalTooSmall, "notional below the venue minimum");
        }
    } else if (!req.price.is_zero()) {
        // A market order carrying a price is almost always a caller bug, and
        // silently dropping the price would hide it.
        return invalid(RejectReason::PriceOutOfBounds, "market order must not carry a price");
    }

    // ---- order type / time in force coherence ----
    if (req.type == OrderType::Market && req.tif == TimeInForce::GTC) {
        return invalid(RejectReason::Unknown, "market order cannot be GTC");
    }
    if (req.is_post_only() && req.type == OrderType::Market) {
        return invalid(RejectReason::Unknown, "market order cannot be post-only");
    }
    if (req.is_post_only() && (req.tif == TimeInForce::IOC || req.tif == TimeInForce::FOK)) {
        // Post-only means "rest or die"; IOC/FOK mean "trade now or die". A
        // venue asked for both will reject, so refuse here instead.
        return invalid(RejectReason::Unknown, "post-only is incompatible with IOC/FOK");
    }

    // ---- venue capabilities ----
    if (req.is_post_only() && !caps.supports_post_only) {
        return unsupported("venue does not support post-only orders");
    }
    if (req.reduce_only && !caps.supports_reduce_only) {
        return unsupported("venue does not support reduce-only orders");
    }
    if (!caps.supports_client_order_id) {
        return unsupported("venue does not support client order ids");
    }

    return ExchangeError::none();
}

ExchangeError validate_cancel(const CancelRequest& req, const ExchangeCapabilities& caps) noexcept {
    if (!req.identifies_an_order()) {
        return invalid(RejectReason::UnknownOrder, "cancel identifies no order");
    }
    if (req.symbol.empty()) {
        return invalid(RejectReason::SymbolNotTrading, "cancel has no symbol");
    }
    if (req.exchange_order_id.empty() && !caps.supports_client_order_id) {
        return unsupported("venue cannot cancel by client order id");
    }
    return ExchangeError::none();
}

ExchangeError validate_replace(const ReplaceRequest& req, const InstrumentSpec& spec,
                               const ExchangeCapabilities& caps) noexcept {
    if (!caps.supports_replace) {
        // Not an error the caller should route around silently: the quote
        // manager must consciously decompose into cancel-then-submit, because
        // the exposure profile of the two differs.
        return unsupported("venue does not support atomic replace");
    }
    if (req.original_client_order_id.empty() && req.original_exchange_order_id.empty()) {
        return invalid(RejectReason::UnknownOrder, "replace identifies no original order");
    }
    if (req.new_client_order_id.empty()) {
        return invalid(RejectReason::Unknown, "replace requires a new client order id");
    }
    if (req.new_client_order_id == req.original_client_order_id) {
        // Reusing the ID makes the original and the replacement
        // indistinguishable in reports that arrive out of order.
        return invalid(RejectReason::DuplicateClientOrderId,
                       "replacement must use a fresh client order id");
    }
    if (req.new_client_order_id.size() > caps.max_client_order_id_len) {
        return invalid(RejectReason::Unknown, "client order id exceeds venue length limit");
    }
    if (!spec.is_valid()) {
        return invalid(RejectReason::SymbolNotTrading, "instrument spec is not loaded");
    }
    if (!req.new_quantity.is_positive()) {
        return invalid(RejectReason::QuantityOutOfBounds, "replacement quantity must be positive");
    }
    if (!is_on_step(req.new_quantity, spec.lot_size)) {
        return invalid(RejectReason::LotSizeViolation, "replacement quantity is off the lot grid");
    }
    if (!req.new_price.is_positive()) {
        return invalid(RejectReason::PriceOutOfBounds, "replacement price must be positive");
    }
    if (!is_on_step(req.new_price, spec.tick_size)) {
        return invalid(RejectReason::TickSizeViolation, "replacement price is off the tick grid");
    }
    if (spec.min_notional.is_positive() &&
        notional_of(req.new_price, req.new_quantity) < spec.min_notional) {
        return invalid(RejectReason::NotionalTooSmall, "replacement notional below the minimum");
    }
    return ExchangeError::none();
}

}  // namespace mm::exchange
