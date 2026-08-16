#include "mm/strategy/QuoteIntent.hpp"
#include "mm/strategy/StrategyContext.hpp"
#include "mm/strategy/StrategyTypes.hpp"

namespace mm::strategy {

std::string_view to_string(StrategyState s) noexcept {
    switch (s) {
        case StrategyState::Created:     return "CREATED";
        case StrategyState::Initialized: return "INITIALIZED";
        case StrategyState::Running:     return "RUNNING";
        case StrategyState::Paused:      return "PAUSED";
        case StrategyState::Stopped:     return "STOPPED";
        case StrategyState::Faulted:     return "FAULTED";
    }
    return "UNKNOWN";
}

bool is_legal_transition(StrategyState from, StrategyState to) noexcept {
    if (from == to) {
        return true;
    }
    // A strategy can fault from anywhere: it can throw during initialization
    // just as easily as during evaluation.
    if (to == StrategyState::Faulted) {
        return true;
    }
    switch (from) {
        case StrategyState::Created:
            return to == StrategyState::Initialized;
        case StrategyState::Initialized:
            return to == StrategyState::Running || to == StrategyState::Stopped;
        case StrategyState::Running:
            return to == StrategyState::Paused || to == StrategyState::Stopped;
        case StrategyState::Paused:
            return to == StrategyState::Running || to == StrategyState::Stopped;
        case StrategyState::Stopped:
            // Re-initialization is the only way back, which forces parameters
            // to be re-validated rather than silently reused.
            return to == StrategyState::Initialized;
        case StrategyState::Faulted:
            // Only an explicit clear_fault, which returns it to Initialized.
            return to == StrategyState::Initialized;
    }
    return false;
}

std::string_view to_string(EvaluationMode m) noexcept {
    switch (m) {
        case EvaluationMode::OnBookUpdate:        return "on_book_update";
        case EvaluationMode::OnBboChange:         return "on_bbo_change";
        case EvaluationMode::OnTimer:             return "on_timer";
        case EvaluationMode::OnBboChangeAndTimer: return "on_bbo_change_and_timer";
    }
    return "unknown";
}

bool parse_evaluation_mode(std::string_view text, EvaluationMode& out) noexcept {
    if (text == "on_book_update") {
        out = EvaluationMode::OnBookUpdate;
        return true;
    }
    if (text == "on_bbo_change") {
        out = EvaluationMode::OnBboChange;
        return true;
    }
    if (text == "on_timer") {
        out = EvaluationMode::OnTimer;
        return true;
    }
    if (text == "on_bbo_change_and_timer") {
        out = EvaluationMode::OnBboChangeAndTimer;
        return true;
    }
    return false;
}

std::string_view to_string(TriggerReason r) noexcept {
    switch (r) {
        case TriggerReason::None:             return "NONE";
        case TriggerReason::BookUpdate:       return "BOOK_UPDATE";
        case TriggerReason::BboChange:        return "BBO_CHANGE";
        case TriggerReason::Trade:            return "TRADE";
        case TriggerReason::Timer:            return "TIMER";
        case TriggerReason::InventoryChange:  return "INVENTORY_CHANGE";
        case TriggerReason::OrderStateChange: return "ORDER_STATE_CHANGE";
    }
    return "UNKNOWN";
}

std::string_view to_string(QuoteAction a) noexcept {
    switch (a) {
        case QuoteAction::Quote:    return "QUOTE";
        case QuoteAction::Pull:     return "PULL";
        case QuoteAction::NoChange: return "NO_CHANGE";
    }
    return "UNKNOWN";
}

std::string_view to_string(IntentReason r) noexcept {
    switch (r) {
        case IntentReason::None:              return "NONE";
        case IntentReason::Normal:            return "NORMAL";
        case IntentReason::InventoryLimit:    return "INVENTORY_LIMIT";
        case IntentReason::SpreadTooTight:    return "SPREAD_TOO_TIGHT";
        case IntentReason::SpreadTooWide:     return "SPREAD_TOO_WIDE";
        case IntentReason::InsufficientDepth: return "INSUFFICIENT_DEPTH";
        case IntentReason::Warmup:            return "WARMUP";
        case IntentReason::StrategyDisabled:  return "STRATEGY_DISABLED";
        case IntentReason::Custom:            return "CUSTOM";
    }
    return "UNKNOWN";
}

std::string_view to_string(IntentRejection r) noexcept {
    switch (r) {
        case IntentRejection::None:                 return "NONE";
        case IntentRejection::UnknownAction:        return "UNKNOWN_ACTION";
        case IntentRejection::NonPositivePrice:     return "NON_POSITIVE_PRICE";
        case IntentRejection::NonPositiveQuantity:  return "NON_POSITIVE_QUANTITY";
        case IntentRejection::NegativeQuantity:     return "NEGATIVE_QUANTITY";
        case IntentRejection::TickMisaligned:       return "TICK_MISALIGNED";
        case IntentRejection::LotMisaligned:        return "LOT_MISALIGNED";
        case IntentRejection::NotionalTooSmall:     return "NOTIONAL_TOO_SMALL";
        case IntentRejection::QuantityAboveVenueMax:return "QUANTITY_ABOVE_VENUE_MAX";
        case IntentRejection::CrossedQuote:         return "CROSSED_QUOTE";
        case IntentRejection::QuoteTooFarFromTouch: return "QUOTE_TOO_FAR_FROM_TOUCH";
        case IntentRejection::StaleSequence:        return "STALE_SEQUENCE";
        case IntentRejection::IdentityMissing:      return "IDENTITY_MISSING";
        case IntentRejection::IdentityMismatch:     return "IDENTITY_MISMATCH";
        case IntentRejection::SymbolMismatch:       return "SYMBOL_MISMATCH";
        case IntentRejection::InstrumentNotLoaded:  return "INSTRUMENT_NOT_LOADED";
    }
    return "UNKNOWN";
}

std::string_view to_string(QuoteLifecycle e) noexcept {
    switch (e) {
        case QuoteLifecycle::None:      return "NONE";
        case QuoteLifecycle::Placed:    return "PLACED";
        case QuoteLifecycle::Amended:   return "AMENDED";
        case QuoteLifecycle::Cancelled: return "CANCELLED";
        case QuoteLifecycle::Rejected:  return "REJECTED";
        case QuoteLifecycle::Unknown:   return "UNKNOWN_STATE";
    }
    return "UNKNOWN";
}

}  // namespace mm::strategy
