#include "mm/risk/RiskLimits.hpp"
#include "mm/risk/RiskTypes.hpp"

namespace mm::risk {

std::string_view to_string(RiskState s) noexcept {
    switch (s) {
        case RiskState::Disarmed: return "DISARMED";
        case RiskState::Armed:    return "ARMED";
        case RiskState::Warning:  return "WARNING";
        case RiskState::Killed:   return "KILLED";
        case RiskState::Faulted:  return "FAULTED";
    }
    return "UNKNOWN";
}

bool is_legal_transition(RiskState from, RiskState to) noexcept {
    if (from == to) {
        return true;
    }
    // A kill or a fault can be reached from anywhere: the condition that
    // demands them does not wait for a convenient state.
    if (to == RiskState::Killed || to == RiskState::Faulted) {
        return true;
    }
    switch (from) {
        case RiskState::Disarmed:
            return to == RiskState::Armed;
        case RiskState::Armed:
            return to == RiskState::Warning;
        case RiskState::Warning:
            return to == RiskState::Armed;
        case RiskState::Killed:
        case RiskState::Faulted:
            // Only an explicit rearm, and only back to Disarmed -- from which
            // arming re-validates the limits. Returning straight to Armed would
            // let a kill be undone without anyone re-checking why it happened.
            return to == RiskState::Disarmed;
    }
    return false;
}

std::string_view to_string(RiskVerdict v) noexcept {
    switch (v) {
        case RiskVerdict::Approve: return "APPROVE";
        case RiskVerdict::Reduce:  return "REDUCE";
        case RiskVerdict::Reject:  return "REJECT";
    }
    return "UNKNOWN";
}

std::string_view to_string(RiskReason r) noexcept {
    switch (r) {
        case RiskReason::None:                      return "NONE";
        case RiskReason::MaxPosition:               return "MAX_POSITION";
        case RiskReason::MaxPositionNotional:       return "MAX_POSITION_NOTIONAL";
        case RiskReason::MaxOrderQuantity:          return "MAX_ORDER_QUANTITY";
        case RiskReason::MaxOrderNotional:          return "MAX_ORDER_NOTIONAL";
        case RiskReason::MaxWorkingExposure:        return "MAX_WORKING_EXPOSURE";
        case RiskReason::MaxSideExposure:           return "MAX_SIDE_EXPOSURE";
        case RiskReason::MaxOpenOrders:             return "MAX_OPEN_ORDERS";
        case RiskReason::MaxPortfolioNotional:      return "MAX_PORTFOLIO_NOTIONAL";
        case RiskReason::StaleMarket:               return "STALE_MARKET";
        case RiskReason::InvalidPrice:              return "INVALID_PRICE";
        case RiskReason::InvalidQuantity:           return "INVALID_QUANTITY";
        case RiskReason::PriceOutsideBand:          return "PRICE_OUTSIDE_BAND";
        case RiskReason::ReferencePriceUnavailable: return "REFERENCE_PRICE_UNAVAILABLE";
        case RiskReason::InstrumentInvalid:         return "INSTRUMENT_INVALID";
        case RiskReason::RiskDisarmed:              return "RISK_DISARMED";
        case RiskReason::RiskKilled:                return "RISK_KILLED";
        case RiskReason::RiskFaulted:               return "RISK_FAULTED";
        case RiskReason::ReduceOnly:                return "REDUCE_ONLY";
        case RiskReason::SystemNotReady:            return "SYSTEM_NOT_READY";
        case RiskReason::PositionUnavailable:       return "POSITION_UNAVAILABLE";
        case RiskReason::PositionStale:             return "POSITION_STALE";
        case RiskReason::UnknownExposure:           return "UNKNOWN_EXPOSURE";
        case RiskReason::RateLimit:                 return "RATE_LIMIT";
        case RiskReason::OwnershipViolation:        return "OWNERSHIP_VIOLATION";
        case RiskReason::ArithmeticOverflow:        return "ARITHMETIC_OVERFLOW";
        case RiskReason::ConfigurationMissing:      return "CONFIGURATION_MISSING";
        case RiskReason::ReductionInvalid:          return "REDUCTION_INVALID";
    }
    return "UNKNOWN";
}

Status RiskLimits::validate_for_arming() const {
    if (symbol_count == 0) {
        return {ErrorCode::FailedPrecondition, "risk has no configured symbols"};
    }
    for (std::uint8_t i = 0; i < symbol_count; ++i) {
        const SymbolLimits& s = symbols[i];
        // Zero is "unset", and unset is not enforced. Arming into that state
        // would leave the engine nominally present and actually absent, which
        // is the single most dangerous configuration a risk system can have.
        if (!s.max_position.is_positive()) {
            return {ErrorCode::FailedPrecondition,
                    std::string("risk.symbols.").append(s.symbol.view()) +
                        ".max_position must be set before arming"};
        }
        if (!s.max_order_quantity.is_positive()) {
            return {ErrorCode::FailedPrecondition,
                    std::string("risk.symbols.").append(s.symbol.view()) +
                        ".max_order_quantity must be set before arming"};
        }
        if (!s.max_order_notional.is_positive()) {
            return {ErrorCode::FailedPrecondition,
                    std::string("risk.symbols.").append(s.symbol.view()) +
                        ".max_order_notional must be set before arming"};
        }
        if (s.max_open_orders <= 0) {
            return {ErrorCode::FailedPrecondition,
                    std::string("risk.symbols.").append(s.symbol.view()) +
                        ".max_open_orders must be set before arming"};
        }
    }
    if (global.max_market_data_age_ns <= 0 || global.max_position_age_ns <= 0) {
        return {ErrorCode::FailedPrecondition, "risk freshness limits must be positive"};
    }
    return Status::ok();
}

}  // namespace mm::risk
