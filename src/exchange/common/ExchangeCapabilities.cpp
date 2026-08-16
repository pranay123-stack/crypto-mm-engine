#include "mm/exchange/common/ExchangeCapabilities.hpp"

#include <sstream>

namespace mm::exchange {

std::string_view to_string(Feature f) noexcept {
    switch (f) {
        case Feature::None:              return "NONE";
        case Feature::PostOnly:          return "POST_ONLY";
        case Feature::Replace:           return "REPLACE";
        case Feature::ReduceOnly:        return "REDUCE_ONLY";
        case Feature::ClientOrderId:     return "CLIENT_ORDER_ID";
        case Feature::MassCancel:        return "MASS_CANCEL";
        case Feature::OrderQuery:        return "ORDER_QUERY";
        case Feature::NativeBbo:         return "NATIVE_BBO";
        case Feature::OrderbookSnapshot: return "ORDERBOOK_SNAPSHOT";
        case Feature::IncrementalDepth:  return "INCREMENTAL_DEPTH";
        case Feature::TradeStream:       return "TRADE_STREAM";
        case Feature::BalanceStream:     return "BALANCE_STREAM";
        case Feature::PositionStream:    return "POSITION_STREAM";
    }
    return "UNKNOWN";
}

bool supports(const ExchangeCapabilities& caps, Feature f) noexcept {
    switch (f) {
        case Feature::None:              return true;
        case Feature::PostOnly:          return caps.supports_post_only;
        case Feature::Replace:           return caps.supports_replace;
        case Feature::ReduceOnly:        return caps.supports_reduce_only;
        case Feature::ClientOrderId:     return caps.supports_client_order_id;
        case Feature::MassCancel:        return caps.supports_mass_cancel;
        case Feature::OrderQuery:        return caps.supports_order_query;
        case Feature::NativeBbo:         return caps.supports_native_bbo;
        case Feature::OrderbookSnapshot: return caps.supports_orderbook_snapshot;
        case Feature::IncrementalDepth:  return caps.supports_incremental_depth;
        case Feature::TradeStream:       return caps.supports_trade_stream;
        case Feature::BalanceStream:     return caps.supports_balance_stream;
        case Feature::PositionStream:    return caps.supports_position_stream;
    }
    return false;
}

std::string ExchangeCapabilities::to_string() const {
    std::ostringstream out;
    const auto flag = [&out](const char* name, bool value) {
        out << name << '=' << (value ? '1' : '0') << ' ';
    };
    flag("post_only", supports_post_only);
    flag("replace", supports_replace);
    flag("reduce_only", supports_reduce_only);
    flag("client_order_id", supports_client_order_id);
    flag("mass_cancel", supports_mass_cancel);
    flag("order_query", supports_order_query);
    flag("native_bbo", supports_native_bbo);
    flag("snapshot", supports_orderbook_snapshot);
    flag("incremental_depth", supports_incremental_depth);
    flag("trade_stream", supports_trade_stream);
    flag("balance_stream", supports_balance_stream);
    flag("position_stream", supports_position_stream);
    flag("has_positions", has_positions);
    out << "max_coid_len=" << max_client_order_id_len;
    return out.str();
}

}  // namespace mm::exchange
