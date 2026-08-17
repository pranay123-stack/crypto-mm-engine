#pragma once

/// \file PositionAccount.hpp
/// One symbol's position, cost basis and PnL.
///
/// ## The accounting model, stated exactly
///
/// **Weighted-average cost basis.** Not FIFO, not LIFO, not specific-lot.
///
/// The choice is deliberate. §23 requires an *average entry price*, and a
/// market maker's inventory is a continuously churning net position rather
/// than a set of identifiable lots. FIFO would require carrying every open lot
/// and would answer a tax question this system does not ask, at the cost of
/// unbounded state on the hot path. Weighted average answers the question that
/// actually drives decisions -- "what did the inventory I am holding cost me
/// on average" -- in constant space.
///
/// Given position `q` (signed, negative short) at average entry `a`, a fill of
/// signed quantity `f` at price `p`:
///
/// **1. Increasing (same sign, or opening from flat)**
/// ```
///   a' = (|q| * a + |f| * p) / (|q| + |f|)
///   q' = q + f
///   realized = 0
/// ```
///
/// **2. Reducing (opposite sign, |f| <= |q|)**
/// ```
///   closed    = |f|
///   realized  = (p - a) * closed * sign(q)
///   a'        = a                      -- unchanged; see below
///   q'        = q + f
/// ```
/// The cost basis is deliberately **not** touched by a partial reduction.
/// What remains is the same inventory, bought at the same average price;
/// re-deriving `a` from the residual would make the average entry price of an
/// untouched holding move because something else was sold.
///
/// **3. Flip (opposite sign, |f| > |q|)**
/// ```
///   realized  = (p - a) * |q| * sign(q)   -- the whole old position closes
///   q'        = q + f                     -- residual, opposite sign
///   a'        = p                         -- residual opened at this fill
/// ```
/// The two halves are accounted separately and at the same price, because they
/// are two different events that happened to arrive in one message.
///
/// **Fees never enter the cost basis.** They accumulate separately and are
/// subtracted in net PnL. Folding them in would make `average_price` stop
/// being a price -- it would be a price plus an amortised cost, and every
/// downstream comparison against a market price would be subtly wrong.
///
/// **Realized PnL is gross.** `net = realized + unrealized + funding - fees`.
///
/// Every step uses checked fixed-point arithmetic and refuses rather than
/// wrapping. There is no floating point anywhere in this file.

#include <array>
#include <cstdint>

#include "mm/common/Fixed.hpp"
#include "mm/portfolio/AccountingTypes.hpp"

namespace mm::portfolio {

/// Remembers the trade ids already applied, so a redelivered fill cannot be
/// counted twice.
///
/// A ring, not a set: bounded memory on the hot path, and it reports when it
/// wraps so "we may no longer be able to detect a duplicate" is a visible fact
/// rather than a silent one. The same design as the OMS's per-order ring, for
/// the same reason -- but a *separate* instance, because the OMS asks "has this
/// order seen this trade" and accounting asks "has the ledger seen this trade",
/// and one is not derivable from the other.
class SeenTrades {
public:
    static constexpr std::size_t kCapacity = 64;

    /// Returns false if `id` was already present.
    [[nodiscard]] bool insert(const TradeId& id) noexcept {
        if (contains(id)) {
            return false;
        }
        if (count_ == kCapacity) {
            overflowed_ = true;
        }
        ids_[next_] = id;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) {
            ++count_;
        }
        return true;
    }

    [[nodiscard]] bool contains(const TradeId& id) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (ids_[i] == id) {
                return true;
            }
        }
        return false;
    }

    /// True once the ring has wrapped: older ids are no longer remembered, so
    /// a very late redelivery could slip through. Surfaced, never assumed away.
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    std::array<TradeId, kCapacity> ids_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    bool overflowed_ = false;
};

static_assert(std::is_trivially_copyable_v<SeenTrades>,
              "accounting state is copied into snapshots without allocating");

/// One symbol's authoritative position and PnL.
class PositionAccount {
public:
    PositionAccount() = default;
    explicit PositionAccount(const Symbol& symbol) : symbol_(symbol) {}

    /// Applies a confirmed fill. `signed_quantity` is positive for a buy.
    ///
    /// Refuses -- leaving every field untouched -- on invalid input or on any
    /// arithmetic that would wrap. A refusal is reported; it is never a
    /// silently-applied approximation.
    [[nodiscard]] FillOutcome apply_fill(const TradeId& trade_id, Side side, Px price,
                                         Qty quantity, Notional fee);

    /// Marks the open position. Leaves `unrealized_valid` false when the mark
    /// is unusable, rather than reporting a stale or invented number.
    [[nodiscard]] AccountingError apply_mark(const MarkPrice& mark, Nanos now_ns,
                                             Nanos max_age_ns);

    // ------------------------------------------------------------------ views

    [[nodiscard]] const Symbol& symbol() const noexcept { return symbol_; }
    /// Signed: negative is short.
    [[nodiscard]] Qty position() const noexcept { return position_; }
    [[nodiscard]] Px average_price() const noexcept { return average_price_; }
    [[nodiscard]] Notional realized() const noexcept { return realized_; }
    [[nodiscard]] Notional unrealized() const noexcept { return unrealized_; }
    [[nodiscard]] bool unrealized_valid() const noexcept { return unrealized_valid_; }
    [[nodiscard]] Notional fees() const noexcept { return fees_; }
    [[nodiscard]] std::uint64_t fill_count() const noexcept { return fill_count_; }
    [[nodiscard]] bool is_flat() const noexcept { return position_.is_zero(); }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }
    [[nodiscard]] bool dedup_overflowed() const noexcept { return seen_.overflowed(); }
    [[nodiscard]] Px last_mark() const noexcept { return last_mark_; }

    /// Signed notional of the open position at its own average price. This is
    /// cost, not market value -- `unrealized()` carries the difference.
    [[nodiscard]] bool checked_cost_basis(Notional& out) const noexcept {
        return checked_notional_of(average_price_, position_, out);
    }

    /// Absolute market value of the open position at the last usable mark.
    /// Zero and `false` when there is no usable mark: gross exposure that
    /// cannot be computed must not be reported as zero exposure.
    [[nodiscard]] bool checked_gross_exposure(Notional& out) const noexcept;

    [[nodiscard]] PnlBreakdown pnl() const;

    /// Places the account in a faulted state. Every subsequent fill is refused
    /// until an operator resolves it: once the ledger is known to be wrong,
    /// continuing to add to it produces a number that looks authoritative and
    /// is not.
    void fault() noexcept { faulted_ = true; }
    void clear_fault() noexcept { faulted_ = false; }

    // -------------------------------------------------------------- recovery

    /// Everything needed to reconstruct this account. Trivially copyable so a
    /// snapshot never allocates.
    struct State {
        Symbol symbol{};
        Qty position{};
        Px average_price{};
        Notional realized{};
        Notional fees{};
        std::uint64_t fill_count = 0;
        SeenTrades seen{};
        bool faulted = false;
    };

    [[nodiscard]] State save() const;
    /// Rejects a state that is not self-consistent rather than adopting it.
    [[nodiscard]] AccountingError restore(const State& state);

private:
    Symbol symbol_{};
    Qty position_{};
    Px average_price_{};
    Notional realized_{};
    Notional unrealized_{};
    Notional fees_{};
    Px last_mark_{};
    std::uint64_t fill_count_ = 0;
    bool unrealized_valid_ = true;  ///< trivially true while flat
    bool faulted_ = false;
    SeenTrades seen_{};
};

}  // namespace mm::portfolio
