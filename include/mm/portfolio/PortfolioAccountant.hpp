#pragma once

/// \file PortfolioAccountant.hpp
/// The accounting layer: many symbols, one owner, one authoritative ledger.
///
/// Consumes normalized `exchange::FillEvent`s and normalized marks; publishes
/// the position and portfolio snapshots that risk consumes. It knows nothing
/// about orders, venues, transports or wire formats.
///
/// ## Concurrency
///
/// Owned entirely by the **trading thread**, like every other component that
/// mutates trading state (docs/concurrency.md §1). Fills reach it the same way
/// they reach the OMS -- drained from the execution ring on the trading thread
/// -- so there is no second ingress path and no lock.
///
/// Readers that are not the trading thread (monitoring, later phases) take a
/// `PortfolioSnapshot` by value. It is trivially copyable precisely so it can
/// be published through a seqlock without the reader ever touching accounting
/// state directly.

#include <array>
#include <cstdint>
#include <vector>

#include "mm/exchange/common/ExecutionEvents.hpp"
#include "mm/portfolio/PositionAccount.hpp"
#include "mm/risk/PortfolioState.hpp"
#include "mm/risk/PositionState.hpp"

namespace mm::portfolio {

/// Balances, per §24.
///
/// State representation only. Reconciliation against venue balances is
/// explicitly Phase 11 (master spec §26), so nothing here compares or resolves
/// -- it holds what the venue last told us and says how old that is.
struct AssetBalance {
    Asset asset{};
    Qty free{};
    Qty locked{};
    Nanos as_of_ns = 0;
    bool valid = false;

    [[nodiscard]] bool checked_total(Qty& out) const noexcept {
        return checked_add(free, locked, out);
    }
};

/// The portfolio-level view risk needs to enforce what Phase 7 deferred.
struct PortfolioSnapshot {
    /// False when accounting cannot vouch for these numbers. Risk must treat
    /// this exactly as it treats an invalid position: refuse new exposure.
    bool valid = false;
    /// False when any symbol's unrealized PnL is indeterminate -- a stale or
    /// missing mark. Loss limits cannot be evaluated against it.
    bool pnl_determinate = false;

    /// Sum of |position| * mark across symbols. Absolute, so a long and a
    /// short do not net to "no exposure".
    Notional gross_notional{};
    /// Signed sum, which is a different question and kept separately.
    Notional net_notional{};

    Notional realized{};
    Notional unrealized{};
    Notional fees{};
    Notional funding{};
    /// realized + unrealized + funding - fees.
    Notional net_pnl{};

    std::uint32_t symbols_with_position = 0;
    std::uint32_t symbol_count = 0;
    std::uint64_t sequence = 0;
    Nanos as_of_ns = 0;

    /// True when any account is faulted or any arithmetic failed. Risk uses
    /// this to refuse exposure rather than trusting the numbers beside it.
    bool degraded = false;
};

static_assert(std::is_trivially_copyable_v<PortfolioSnapshot>,
              "published through a seqlock to non-trading-thread readers");

/// Owns every symbol's account.
class PortfolioAccountant {
public:
    PortfolioAccountant(AccountingConfig config, const Clock& clock);

    /// Admits a symbol. Capacity is fixed at construction so no fill ever
    /// allocates; an unregistered symbol is refused rather than created on the
    /// hot path.
    [[nodiscard]] AccountingError register_symbol(const Symbol& symbol);

    /// Applies a confirmed fill from the execution layer.
    ///
    /// This is the only way position changes. There is deliberately no setter:
    /// §23 requires that positions update from actual fills and never from
    /// submitted orders, and an API that could set a position directly is an
    /// API through which that rule gets broken.
    [[nodiscard]] FillOutcome on_fill(const exchange::FillEvent& fill);

    /// Applies a normalized mark. Accounting never reads a book.
    [[nodiscard]] AccountingError on_mark(const MarkPrice& mark);

    /// Records the venue's view of a balance (§24).
    [[nodiscard]] AccountingError on_balance(const exchange::BalanceUpdateEvent& balance);

    // ------------------------------------------------------------------ views

    [[nodiscard]] const PositionAccount* find(const Symbol& symbol) const;
    [[nodiscard]] std::size_t symbol_count() const noexcept { return accounts_.size(); }

    /// The per-symbol snapshot risk already consumes (Phase 7). Produced here
    /// rather than by the caller so there is exactly one place that decides
    /// what "the position" is.
    [[nodiscard]] risk::PositionSnapshot position_snapshot(const Symbol& symbol) const;

    /// Portfolio aggregate. Recomputed on demand from the accounts, so it can
    /// never drift from them.
    [[nodiscard]] PortfolioSnapshot portfolio_snapshot() const;

    [[nodiscard]] const AssetBalance* balance(const Asset& asset) const;

    /// The portfolio view risk consumes, in risk's own vocabulary.
    ///
    /// Produced here so there is exactly one place that decides what "the
    /// portfolio" is. Risk never learns that an accounting layer exists; it is
    /// handed a struct it defined.
    [[nodiscard]] risk::PortfolioState risk_view() const;

    [[nodiscard]] const AccountingMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    /// True when any account is faulted. Sticky until cleared by an operator.
    [[nodiscard]] bool degraded() const noexcept;

    // -------------------------------------------------------------- recovery

    /// Everything needed to reconstruct the ledger.
    ///
    /// Durable storage is deliberately absent: the master spec places
    /// persistence and crash recovery in Phase 11 (§27, §28). What is
    /// implemented here is the state representation and the recovery contract
    /// -- exactly the boundary Phase 8 drew for the OMS, for the same reason.
    struct RecoveryState {
        std::uint64_t sequence = 0;
        std::vector<PositionAccount::State> accounts;
        std::vector<AssetBalance> balances;
    };

    [[nodiscard]] RecoveryState save() const;
    [[nodiscard]] AccountingError restore(const RecoveryState& state);

private:
    [[nodiscard]] PositionAccount* mutable_find(const Symbol& symbol);

    AccountingConfig config_;
    const Clock& clock_;
    std::vector<PositionAccount> accounts_;
    std::vector<AssetBalance> balances_;
    std::uint64_t sequence_ = 0;
    AccountingMetrics metrics_{};
};

}  // namespace mm::portfolio
