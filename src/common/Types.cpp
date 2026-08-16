#include "mm/common/Types.hpp"

namespace mm {

std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:      return "LIMIT";
        case OrderType::Market:     return "MARKET";
        case OrderType::LimitMaker: return "LIMIT_MAKER";
    }
    return "UNKNOWN";
}

std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
    }
    return "UNKNOWN";
}

std::string_view to_string(Liquidity l) noexcept {
    switch (l) {
        case Liquidity::Unknown: return "UNKNOWN";
        case Liquidity::Maker:   return "MAKER";
        case Liquidity::Taker:   return "TAKER";
    }
    return "UNKNOWN";
}

std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:                   return "NONE";
        case RejectReason::Unknown:                return "UNKNOWN";
        case RejectReason::InsufficientBalance:    return "INSUFFICIENT_BALANCE";
        case RejectReason::PriceOutOfBounds:       return "PRICE_OUT_OF_BOUNDS";
        case RejectReason::QuantityOutOfBounds:    return "QUANTITY_OUT_OF_BOUNDS";
        case RejectReason::NotionalTooSmall:       return "NOTIONAL_TOO_SMALL";
        case RejectReason::TickSizeViolation:      return "TICK_SIZE_VIOLATION";
        case RejectReason::LotSizeViolation:       return "LOT_SIZE_VIOLATION";
        case RejectReason::PostOnlyWouldCross:     return "POST_ONLY_WOULD_CROSS";
        case RejectReason::DuplicateClientOrderId: return "DUPLICATE_CLIENT_ORDER_ID";
        case RejectReason::UnknownOrder:           return "UNKNOWN_ORDER";
        case RejectReason::RateLimited:            return "RATE_LIMITED";
        case RejectReason::MarketClosed:           return "MARKET_CLOSED";
        case RejectReason::SymbolNotTrading:       return "SYMBOL_NOT_TRADING";
        case RejectReason::ReduceOnlyViolation:    return "REDUCE_ONLY_VIOLATION";
        case RejectReason::RiskLimitExceeded:      return "RISK_LIMIT_EXCEEDED";
        case RejectReason::Connectivity:           return "CONNECTIVITY";
        case RejectReason::InternalError:          return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

std::string_view to_string(MarketStatus s) noexcept {
    switch (s) {
        case MarketStatus::Unknown:     return "UNKNOWN";
        case MarketStatus::Trading:     return "TRADING";
        case MarketStatus::Halted:      return "HALTED";
        case MarketStatus::AuctionOnly: return "AUCTION_ONLY";
        case MarketStatus::Delisted:    return "DELISTED";
    }
    return "UNKNOWN";
}

}  // namespace mm
