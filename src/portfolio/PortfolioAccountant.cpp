#include "mm/portfolio/PortfolioAccountant.hpp"

#include <algorithm>

namespace mm::portfolio {

PortfolioAccountant::PortfolioAccountant(AccountingConfig config, const Clock& clock)
    : config_(config), clock_(clock) {
    // Reserved once. A fill must never allocate, and capacity exhaustion must
    // be a bounded, observable condition rather than unbounded growth.
    accounts_.reserve(config_.max_symbols);
    balances_.reserve(config_.max_symbols * 2);
}

AccountingError PortfolioAccountant::register_symbol(const Symbol& symbol) {
    if (symbol.empty()) {
        return AccountingError::UnknownSymbol;
    }
    if (find(symbol) != nullptr) {
        return AccountingError::None;
    }
    if (accounts_.size() >= config_.max_symbols) {
        return AccountingError::CapacityExhausted;
    }
    accounts_.emplace_back(symbol);
    return AccountingError::None;
}

PositionAccount* PortfolioAccountant::mutable_find(const Symbol& symbol) {
    // A linear scan over a handful of symbols, held contiguously.
    //
    // Deliberately not an unordered_map: a market maker runs tens of symbols,
    // not thousands, and at that size a contiguous scan of 24-byte keys beats
    // a hash lookup that chases a pointer into a bucket. It also allocates
    // nothing and keeps the accounts adjacent for the aggregation pass, which
    // touches all of them anyway.
    for (PositionAccount& a : accounts_) {
        if (a.symbol() == symbol) {
            return &a;
        }
    }
    return nullptr;
}

const PositionAccount* PortfolioAccountant::find(const Symbol& symbol) const {
    for (const PositionAccount& a : accounts_) {
        if (a.symbol() == symbol) {
            return &a;
        }
    }
    return nullptr;
}

FillOutcome PortfolioAccountant::on_fill(const exchange::FillEvent& fill) {
    FillOutcome outcome;

    PositionAccount* account = mutable_find(fill.symbol);
    if (account == nullptr) {
        // Never auto-registered. A fill for a symbol we are not accounting for
        // means either a configuration error or somebody else's trade on our
        // account; inventing an account would bury both.
        ++metrics_.rejected_fills;
        outcome.error = AccountingError::UnknownSymbol;
        return outcome;
    }

    // The fee comes from the execution event and is never recomputed here.
    //
    // Recomputing it would require this layer to know the venue's schedule,
    // which is exactly the venue coupling the boundary exists to prevent -- and
    // it would produce a second, competing answer to a question the venue has
    // already answered authoritatively.
    outcome = account->apply_fill(fill.trade_id, fill.side, fill.price, fill.quantity, fill.fee);

    switch (outcome.error) {
        case AccountingError::None:
            ++metrics_.fills_applied;
            if (outcome.flipped) {
                ++metrics_.flips;
            }
            if (outcome.closed) {
                ++metrics_.closes;
            }
            ++sequence_;
            break;
        case AccountingError::DuplicateFill:
            ++metrics_.duplicate_fills;
            break;
        case AccountingError::ArithmeticOverflow:
            // Arithmetic that would have wrapped means the ledger can no
            // longer be trusted for this symbol. Faulting is the fail-closed
            // response: every subsequent fill is refused, and risk sees a
            // degraded portfolio and stops adding exposure.
            ++metrics_.overflows;
            ++metrics_.rejected_fills;
            ++metrics_.faults;
            account->fault();
            break;
        default:
            ++metrics_.rejected_fills;
            break;
    }
    return outcome;
}

AccountingError PortfolioAccountant::on_mark(const MarkPrice& mark) {
    PositionAccount* account = mutable_find(mark.symbol);
    if (account == nullptr) {
        ++metrics_.marks_rejected;
        return AccountingError::UnknownSymbol;
    }
    const Nanos now = clock_.steady();
    const AccountingError e = account->apply_mark(mark, now, config_.max_mark_age_ns);

    if (e == AccountingError::None) {
        ++metrics_.marks_applied;
        ++sequence_;
    } else {
        if (e == AccountingError::StaleMark) {
            ++metrics_.stale_marks;
        }
        ++metrics_.marks_rejected;
    }
    return e;
}

AccountingError PortfolioAccountant::on_balance(const exchange::BalanceUpdateEvent& update) {
    if (update.asset.empty()) {
        return AccountingError::UnknownSymbol;
    }
    if (update.free.is_negative() || update.locked.is_negative()) {
        // A negative balance is not a small balance; it is a message we cannot
        // interpret. Refused rather than stored.
        return AccountingError::InvalidQuantity;
    }
    for (AssetBalance& b : balances_) {
        if (b.asset == update.asset) {
            b.free = update.free;
            b.locked = update.locked;
            b.as_of_ns = clock_.steady();
            b.valid = true;
            return AccountingError::None;
        }
    }
    if (balances_.size() >= balances_.capacity()) {
        return AccountingError::CapacityExhausted;
    }
    AssetBalance b;
    b.asset = update.asset;
    b.free = update.free;
    b.locked = update.locked;
    b.as_of_ns = clock_.steady();
    b.valid = true;
    balances_.push_back(b);
    return AccountingError::None;
}

const AssetBalance* PortfolioAccountant::balance(const Asset& asset) const {
    for (const AssetBalance& b : balances_) {
        if (b.asset == asset) {
            return &b;
        }
    }
    return nullptr;
}

bool PortfolioAccountant::degraded() const noexcept {
    return std::any_of(accounts_.begin(), accounts_.end(),
                       [](const PositionAccount& a) { return a.faulted(); });
}

risk::PositionSnapshot PortfolioAccountant::position_snapshot(const Symbol& symbol) const {
    risk::PositionSnapshot snapshot;
    snapshot.symbol = symbol;
    const PositionAccount* account = find(symbol);
    if (account == nullptr || account->faulted()) {
        // `valid` stays false. Risk refuses to add exposure against a position
        // it cannot vouch for, which is exactly the required behaviour: a
        // faulted account must not read as flat.
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.quantity = account->position();
    snapshot.average_price = account->average_price();
    Notional cost{};
    if (account->checked_cost_basis(cost)) {
        snapshot.notional = cost;
    }
    snapshot.as_of_ns = clock_.steady();
    snapshot.sequence = sequence_;
    return snapshot;
}

PortfolioSnapshot PortfolioAccountant::portfolio_snapshot() const {
    PortfolioSnapshot snapshot;
    snapshot.symbol_count = static_cast<std::uint32_t>(accounts_.size());
    snapshot.sequence = sequence_;
    snapshot.as_of_ns = clock_.steady();
    snapshot.pnl_determinate = true;
    snapshot.valid = true;

    for (const PositionAccount& a : accounts_) {
        if (a.faulted()) {
            snapshot.degraded = true;
            snapshot.valid = false;
            snapshot.pnl_determinate = false;
            continue;
        }
        if (!a.is_flat()) {
            ++snapshot.symbols_with_position;
        }

        Notional gross{};
        if (!a.checked_gross_exposure(gross)) {
            // Exposure that cannot be computed must never aggregate as zero.
            snapshot.degraded = true;
            snapshot.valid = false;
            snapshot.pnl_determinate = false;
            continue;
        }
        if (!checked_add(snapshot.gross_notional, saturating_abs(gross), snapshot.gross_notional)) {
            snapshot.degraded = true;
            snapshot.valid = false;
            snapshot.pnl_determinate = false;
            continue;
        }

        Notional signed_value{};
        const Px price = a.last_mark().is_positive() ? a.last_mark() : a.average_price();
        if (checked_notional_of(price, a.position(), signed_value)) {
            if (!checked_add(snapshot.net_notional, signed_value, snapshot.net_notional)) {
                snapshot.degraded = true;
                snapshot.valid = false;
            }
        }

        const PnlBreakdown p = a.pnl();
        if (!checked_add(snapshot.realized, p.realized, snapshot.realized) ||
            !checked_add(snapshot.fees, p.fees, snapshot.fees)) {
            snapshot.degraded = true;
            snapshot.valid = false;
            snapshot.pnl_determinate = false;
            continue;
        }
        if (!p.unrealized_valid) {
            // One indeterminate mark makes the portfolio's PnL indeterminate.
            // Summing the symbols we *can* mark and presenting the total as
            // the portfolio PnL would understate a loss by exactly the symbols
            // we cannot see.
            snapshot.pnl_determinate = false;
        } else if (!checked_add(snapshot.unrealized, p.unrealized, snapshot.unrealized)) {
            snapshot.degraded = true;
            snapshot.valid = false;
            snapshot.pnl_determinate = false;
        }
    }

    PnlBreakdown total;
    total.realized = snapshot.realized;
    total.unrealized = snapshot.unrealized;
    total.fees = snapshot.fees;
    total.funding = snapshot.funding;
    if (!total.checked_net(snapshot.net_pnl)) {
        snapshot.degraded = true;
        snapshot.valid = false;
        snapshot.pnl_determinate = false;
        snapshot.net_pnl = Notional{};
    }
    return snapshot;
}

risk::PortfolioState PortfolioAccountant::risk_view() const {
    const PortfolioSnapshot s = portfolio_snapshot();
    risk::PortfolioState view;
    // `valid` deliberately mirrors the snapshot rather than being set
    // unconditionally: a degraded ledger must reach risk as unusable, not as a
    // set of plausible-looking numbers.
    view.valid = s.valid;
    view.pnl_determinate = s.pnl_determinate;
    view.gross_notional = s.gross_notional;
    view.net_notional = s.net_notional;
    view.realized = s.realized;
    view.unrealized = s.unrealized;
    view.fees = s.fees;
    view.net_pnl = s.net_pnl;
    view.as_of_ns = s.as_of_ns;
    view.sequence = s.sequence;
    return view;
}

PortfolioAccountant::RecoveryState PortfolioAccountant::save() const {
    RecoveryState state;
    state.sequence = sequence_;
    state.accounts.reserve(accounts_.size());
    for (const PositionAccount& a : accounts_) {
        state.accounts.push_back(a.save());
    }
    state.balances = balances_;
    return state;
}

AccountingError PortfolioAccountant::restore(const RecoveryState& state) {
    if (!accounts_.empty()) {
        return AccountingError::CorruptSnapshot;
    }
    if (state.accounts.size() > config_.max_symbols) {
        return AccountingError::CapacityExhausted;
    }
    for (const PositionAccount::State& s : state.accounts) {
        PositionAccount account;
        const AccountingError e = account.restore(s);
        if (e != AccountingError::None) {
            // One bad account invalidates the whole restore. A partially
            // restored ledger is worse than none: it looks complete.
            accounts_.clear();
            return e;
        }
        accounts_.push_back(account);
    }
    balances_ = state.balances;
    sequence_ = state.sequence;
    return AccountingError::None;
}

}  // namespace mm::portfolio
