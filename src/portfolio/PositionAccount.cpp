#include "mm/portfolio/PositionAccount.hpp"

namespace mm::portfolio {
namespace {

/// |a| * p, as a notional, with an explicit range check.
[[nodiscard]] bool abs_notional(Px price, Qty quantity, Notional& out) noexcept {
    return checked_notional_of(price, saturating_abs(quantity), out);
}

/// Weighted average of two prices by absolute quantity, in fixed point
/// throughout. Returns false rather than wrapping or dividing by zero.
[[nodiscard]] bool weighted_average(Px a, Qty qa, Px b, Qty qb, Px& out) noexcept {
    const Qty wa = saturating_abs(qa);
    const Qty wb = saturating_abs(qb);
    Qty total;
    if (!checked_add(wa, wb, total) || !total.is_positive()) {
        return false;
    }
    // 128-bit throughout: the products routinely exceed 64 bits before the
    // division brings them back into range.
    const int128 num = static_cast<int128>(a.raw()) * static_cast<int128>(wa.raw()) +
                       static_cast<int128>(b.raw()) * static_cast<int128>(wb.raw());
    const int128 avg = num / static_cast<int128>(total.raw());
    constexpr int128 kMax = static_cast<int128>(std::numeric_limits<std::int64_t>::max());
    constexpr int128 kMin = static_cast<int128>(std::numeric_limits<std::int64_t>::min());
    if (avg > kMax || avg < kMin) {
        return false;
    }
    out = Px::from_raw(static_cast<std::int64_t>(avg));
    return true;
}

/// (exit - entry) * closed_quantity, signed by the direction being closed.
///
/// `closed` is always positive; `long_position` decides the sign. Closing a
/// long above its cost makes money; closing a short above its cost loses.
[[nodiscard]] bool realized_of(Px entry, Px exit_price, Qty closed, bool long_position,
                               Notional& out) noexcept {
    Px move;
    if (!checked_sub(exit_price, entry, move)) {
        return false;
    }
    if (!long_position) {
        move = Px::from_raw(0) - move;
    }
    return checked_notional_of(move, saturating_abs(closed), out);
}

}  // namespace

std::string_view to_string(AccountingError e) noexcept {
    switch (e) {
        case AccountingError::None:               return "NONE";
        case AccountingError::InvalidQuantity:    return "INVALID_QUANTITY";
        case AccountingError::InvalidPrice:       return "INVALID_PRICE";
        case AccountingError::DuplicateFill:      return "DUPLICATE_FILL";
        case AccountingError::MissingTradeId:     return "MISSING_TRADE_ID";
        case AccountingError::ArithmeticOverflow: return "ARITHMETIC_OVERFLOW";
        case AccountingError::UnknownSymbol:      return "UNKNOWN_SYMBOL";
        case AccountingError::CapacityExhausted:  return "CAPACITY_EXHAUSTED";
        case AccountingError::InvalidMark:        return "INVALID_MARK";
        case AccountingError::StaleMark:          return "STALE_MARK";
        case AccountingError::SequenceRegression: return "SEQUENCE_REGRESSION";
        case AccountingError::CorruptSnapshot:    return "CORRUPT_SNAPSHOT";
        case AccountingError::Faulted:            return "FAULTED";
    }
    return "UNKNOWN";
}

FillOutcome PositionAccount::apply_fill(const TradeId& trade_id, Side side, Px price, Qty quantity,
                                        Notional fee) {
    FillOutcome outcome;

    if (faulted_) {
        // Once the ledger is known to be wrong, adding to it produces a number
        // that looks authoritative and is not.
        outcome.error = AccountingError::Faulted;
        return outcome;
    }
    if (trade_id.empty()) {
        // Without an identity the fill cannot be deduplicated, so applying it
        // would make the whole ledger unverifiable.
        outcome.error = AccountingError::MissingTradeId;
        return outcome;
    }
    if (!quantity.is_positive()) {
        outcome.error = AccountingError::InvalidQuantity;
        return outcome;
    }
    if (!price.is_positive()) {
        outcome.error = AccountingError::InvalidPrice;
        return outcome;
    }
    if (fee.is_negative()) {
        // A negative fee is a rebate, which this phase does not model. Refused
        // rather than silently treated as a cost.
        outcome.error = AccountingError::InvalidQuantity;
        return outcome;
    }
    if (seen_.contains(trade_id)) {
        outcome.error = AccountingError::DuplicateFill;
        return outcome;
    }

    const Qty signed_fill = side == Side::Buy ? quantity : Qty::from_raw(0) - quantity;

    // --- everything below is computed into locals first -------------------
    //
    // Nothing touches member state until every step has succeeded. A fill that
    // overflows halfway through must leave the account exactly as it was, not
    // half-applied -- a half-applied fill is worse than a rejected one because
    // it looks like a valid state.
    Qty new_position;
    if (!checked_add(position_, signed_fill, new_position)) {
        outcome.error = AccountingError::ArithmeticOverflow;
        return outcome;
    }

    Px new_average = average_price_;
    Notional realized_delta{};
    bool flipped = false;

    const bool was_flat = position_.is_zero();
    const bool same_direction =
        !was_flat && (position_.is_negative() == signed_fill.is_negative());

    if (was_flat) {
        // Opening: the fill price is the cost basis.
        new_average = price;
    } else if (same_direction) {
        // Increasing: weighted average of what we held and what we just added.
        if (!weighted_average(average_price_, position_, price, signed_fill, new_average)) {
            outcome.error = AccountingError::ArithmeticOverflow;
            return outcome;
        }
    } else {
        // Opposite direction: reducing, closing, or flipping.
        const Qty held = saturating_abs(position_);
        const Qty incoming = saturating_abs(signed_fill);
        const bool was_long = position_.is_positive();
        const Qty closed = incoming < held ? incoming : held;

        if (!realized_of(average_price_, price, closed, was_long, realized_delta)) {
            outcome.error = AccountingError::ArithmeticOverflow;
            return outcome;
        }
        if (incoming > held) {
            // Flip: the old position closed entirely and a new one opened at
            // this same price. Two events in one message, accounted as two.
            flipped = true;
            new_average = price;
        } else if (new_position.is_zero()) {
            // Flat: no basis to carry.
            new_average = Px::from_raw(0);
        } else {
            // Partial reduction: the residual is the same inventory bought at
            // the same average price, so the basis does not move.
            new_average = average_price_;
        }
    }

    // The resulting position must be valuable. An opening fill never
    // multiplies price by quantity, so without this check a position whose
    // cost basis cannot be represented would be accepted here and only fail
    // later, at a query -- by which time the fill has been acknowledged and
    // the exposure is real but unquantifiable. Refusing now keeps the failure
    // at the moment we can still report it against the fill that caused it.
    Notional new_cost{};
    if (!checked_notional_of(new_average, new_position, new_cost)) {
        outcome.error = AccountingError::ArithmeticOverflow;
        return outcome;
    }

    Notional new_realized;
    if (!checked_add(realized_, realized_delta, new_realized)) {
        outcome.error = AccountingError::ArithmeticOverflow;
        return outcome;
    }
    Notional new_fees;
    if (!checked_add(fees_, fee, new_fees)) {
        outcome.error = AccountingError::ArithmeticOverflow;
        return outcome;
    }

    // --- commit -----------------------------------------------------------
    position_ = new_position;
    average_price_ = new_average;
    realized_ = new_realized;
    fees_ = new_fees;
    ++fill_count_;
    static_cast<void>(seen_.insert(trade_id));

    // The open position changed, so the previous mark no longer describes it.
    // Left invalid until a fresh mark arrives rather than carrying the old
    // number forward against a different position.
    if (position_.is_zero()) {
        unrealized_ = Notional{};
        unrealized_valid_ = true;  // flat is unambiguously zero
    } else {
        unrealized_ = Notional{};
        unrealized_valid_ = false;
    }

    outcome.applied_quantity = signed_fill;
    outcome.realized_delta = realized_delta;
    outcome.fee = fee;
    outcome.flipped = flipped;
    outcome.closed = position_.is_zero();
    return outcome;
}

AccountingError PositionAccount::apply_mark(const MarkPrice& mark, Nanos now_ns,
                                            Nanos max_age_ns) {
    if (!mark.usable()) {
        unrealized_valid_ = position_.is_zero();
        unrealized_ = Notional{};
        return AccountingError::InvalidMark;
    }
    if (max_age_ns > 0 && mark.as_of_ns != 0 && (now_ns - mark.as_of_ns) > max_age_ns) {
        // A stale mark is not a small error in unrealized PnL; it is an
        // unknown. Reporting the old number would let a loss limit pass on
        // evidence that no longer exists.
        unrealized_valid_ = position_.is_zero();
        unrealized_ = Notional{};
        return AccountingError::StaleMark;
    }

    const Px mark_price = mark.for_position(position_);
    last_mark_ = mark_price;

    if (position_.is_zero()) {
        unrealized_ = Notional{};
        unrealized_valid_ = true;
        return AccountingError::None;
    }

    Notional value;
    if (!realized_of(average_price_, mark_price, position_, position_.is_positive(), value)) {
        unrealized_valid_ = false;
        unrealized_ = Notional{};
        return AccountingError::ArithmeticOverflow;
    }
    unrealized_ = value;
    unrealized_valid_ = true;
    return AccountingError::None;
}

bool PositionAccount::checked_gross_exposure(Notional& out) const noexcept {
    if (position_.is_zero()) {
        out = Notional{};
        return true;
    }
    // Market value where a mark exists, cost otherwise. Reporting zero because
    // no mark arrived would understate exposure, which is the one direction
    // that must never happen.
    const Px price = last_mark_.is_positive() ? last_mark_ : average_price_;
    if (!price.is_positive()) {
        return false;
    }
    return abs_notional(price, position_, out);
}

PnlBreakdown PositionAccount::pnl() const {
    PnlBreakdown p;
    p.realized = realized_;
    p.unrealized = unrealized_;
    p.fees = fees_;
    p.unrealized_valid = unrealized_valid_;
    return p;
}

PositionAccount::State PositionAccount::save() const {
    State s;
    s.symbol = symbol_;
    s.position = position_;
    s.average_price = average_price_;
    s.realized = realized_;
    s.fees = fees_;
    s.fill_count = fill_count_;
    s.seen = seen_;
    s.faulted = faulted_;
    return s;
}

AccountingError PositionAccount::restore(const State& state) {
    // A snapshot that is not self-consistent is refused, not adopted. Adopting
    // one would replace a known-unknown with a confident wrong answer.
    if (state.fees.is_negative()) {
        return AccountingError::CorruptSnapshot;
    }
    if (!state.position.is_zero() && !state.average_price.is_positive()) {
        // A position must have a cost basis. Without one, every subsequent
        // realized-PnL calculation would be measured from zero.
        return AccountingError::CorruptSnapshot;
    }
    if (state.position.is_zero() && state.average_price.is_positive()) {
        return AccountingError::CorruptSnapshot;
    }

    symbol_ = state.symbol;
    position_ = state.position;
    average_price_ = state.average_price;
    realized_ = state.realized;
    fees_ = state.fees;
    fill_count_ = state.fill_count;
    seen_ = state.seen;
    faulted_ = state.faulted;

    // Unrealized PnL is never restored: it is a function of a market price,
    // and the market has moved since the snapshot was taken. It stays
    // indeterminate until a fresh mark arrives.
    unrealized_ = Notional{};
    unrealized_valid_ = position_.is_zero();
    last_mark_ = Px{};
    return AccountingError::None;
}

}  // namespace mm::portfolio
