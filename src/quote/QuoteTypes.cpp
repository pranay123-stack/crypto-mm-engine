#include "mm/quote/OrderAction.hpp"
#include "mm/quote/QuoteTypes.hpp"

namespace mm::quote {

std::string_view to_string(QuoteSlot s) noexcept {
    return s == QuoteSlot::Bid ? "BID" : "ASK";
}

std::string_view to_string(PendingOperation p) noexcept {
    switch (p) {
        case PendingOperation::None:    return "NONE";
        case PendingOperation::New:     return "NEW";
        case PendingOperation::Cancel:  return "CANCEL";
        case PendingOperation::Replace: return "REPLACE";
    }
    return "UNKNOWN";
}

std::string_view to_string(SlotOutcome o) noexcept {
    switch (o) {
        case SlotOutcome::Idle:                     return "IDLE";
        case SlotOutcome::Keep:                     return "KEEP";
        case SlotOutcome::New:                      return "NEW";
        case SlotOutcome::Cancel:                   return "CANCEL";
        case SlotOutcome::Replace:                  return "REPLACE";
        case SlotOutcome::CancelThenNew:            return "CANCEL_THEN_NEW";
        case SlotOutcome::AwaitingPending:          return "AWAITING_PENDING";
        case SlotOutcome::FrozenUnknown:            return "FROZEN_UNKNOWN";
        case SlotOutcome::SuppressedByChurnControl: return "SUPPRESSED_BY_CHURN_CONTROL";
        case SlotOutcome::BlockedInvalid:           return "BLOCKED_INVALID";
    }
    return "UNKNOWN";
}

std::string_view to_string(QuoteRejection r) noexcept {
    switch (r) {
        case QuoteRejection::None:                 return "NONE";
        case QuoteRejection::GenerationRegression: return "GENERATION_REGRESSION";
        case QuoteRejection::IdentityMismatch:     return "IDENTITY_MISMATCH";
        case QuoteRejection::SymbolMismatch:       return "SYMBOL_MISMATCH";
        case QuoteRejection::IntentExpired:        return "INTENT_EXPIRED";
        case QuoteRejection::MarketDataStale:      return "MARKET_DATA_STALE";
        case QuoteRejection::InstrumentNotLoaded:  return "INSTRUMENT_NOT_LOADED";
        case QuoteRejection::NotRunning:           return "NOT_RUNNING";
    }
    return "UNKNOWN";
}

std::string_view to_string(OrderActionType t) noexcept {
    switch (t) {
        case OrderActionType::Noop:    return "NOOP";
        case OrderActionType::New:     return "NEW";
        case OrderActionType::Cancel:  return "CANCEL";
        case OrderActionType::Replace: return "REPLACE";
    }
    return "UNKNOWN";
}

std::string_view to_string(ActionReason r) noexcept {
    switch (r) {
        case ActionReason::None:                 return "NONE";
        case ActionReason::NoQuoteResting:       return "NO_QUOTE_RESTING";
        case ActionReason::PriceChanged:         return "PRICE_CHANGED";
        case ActionReason::QuantityChanged:      return "QUANTITY_CHANGED";
        case ActionReason::PartialFillReplenish: return "PARTIAL_FILL_REPLENISH";
        case ActionReason::QuoteDisabled:        return "QUOTE_DISABLED";
        case ActionReason::IntentExpired:        return "INTENT_EXPIRED";
        case ActionReason::MarketDataStale:      return "MARKET_DATA_STALE";
        case ActionReason::StrategyNotRunning:   return "STRATEGY_NOT_RUNNING";
        case ActionReason::OperatorDisabled:     return "OPERATOR_DISABLED";
        case ActionReason::GenerationSuperseded: return "GENERATION_SUPERSEDED";
        case ActionReason::ReplaceUnsupported:   return "REPLACE_UNSUPPORTED";
    }
    return "UNKNOWN";
}

}  // namespace mm::quote
