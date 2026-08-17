#pragma once

/// \file AccountingTypes.hpp
/// Vocabulary for the accounting layer (master spec §23 Position Engine,
/// §24 Balance Engine, §25 PnL Engine).
///
/// ## Ownership
///
/// **Accounting owns position, cost basis, realized PnL, fees and balances.
/// It owns nothing else.** In particular it is not a second OMS: it does not
/// track orders, does not know about acknowledgements, cancels or replaces, and
/// cannot tell you whether an order is working. It consumes *confirmed fills*
/// and nothing else, exactly as §23 requires ("positions must update from
/// actual fills; never infer fills merely from submitted orders").
///
/// | State | Owner |
/// | --- | --- |
/// | order lifecycle, all three identities | `oms::OrderManager` |
/// | the book, BBO | `book::OrderBook` |
/// | working exposure | `oms::OrderManager` (derived from live orders) |
/// | **position, cost basis, realized PnL, fees, balances** | **`portfolio::`** |
/// | whether an action is permitted | `risk::RiskEngine` |
///
/// The OMS already computes a per-order cumulative quantity and VWAP. That is
/// *order* state — what happened to one order — and is deliberately not the
/// same thing as *position*, which is the net of every fill across every order
/// on a symbol. Two consumers of the same fills, answering two different
/// questions; neither derives its answer from the other.

#include <cstdint>
#include <string_view>

#include "mm/common/Fixed.hpp"
#include "mm/common/Time.hpp"
#include "mm/common/Types.hpp"

namespace mm::portfolio {

/// Why an accounting operation was refused.
///
/// Accounting fails closed: every one of these leaves state **unchanged** and
/// is reported. None of them is ever reported as a successful zero — a PnL of
/// zero and a PnL that could not be computed are different facts, and
/// conflating them is how a system comes to believe it is flat.
enum class AccountingError : std::uint8_t {
    None = 0,
    /// A fill with zero or negative quantity. Not applied.
    InvalidQuantity,
    /// A fill with a non-positive price.
    InvalidPrice,
    /// A trade id we have already applied. Ignored, not an error state --
    /// counted separately so redelivery is visible without being alarming.
    DuplicateFill,
    /// The event carries no trade id, so duplicate detection is impossible.
    /// Refused: applying it would make the ledger unverifiable.
    MissingTradeId,
    /// Fixed-point arithmetic would have wrapped. Refused rather than
    /// truncated: a wrapped notional turns a huge position into a small one.
    ArithmeticOverflow,
    /// The symbol is not registered with the accountant.
    UnknownSymbol,
    /// Capacity reached; a new symbol cannot be admitted.
    CapacityExhausted,
    /// A mark price that is not usable (non-positive, or crossed).
    InvalidMark,
    /// A mark older than the configured tolerance. Unrealized PnL becomes
    /// indeterminate rather than stale-but-plausible.
    StaleMark,
    /// An accounting sequence that moved backwards.
    SequenceRegression,
    /// A recovery snapshot that does not describe a consistent state.
    CorruptSnapshot,
    /// The account has been placed in a faulted state by an earlier error and
    /// requires explicit operator action.
    Faulted,
};
[[nodiscard]] std::string_view to_string(AccountingError e) noexcept;

/// Whether an error leaves the ledger trustworthy.
///
/// A duplicate is benign -- nothing changed and nothing needed to. Everything
/// else means a fill we were told about was *not* incorporated, so the position
/// on our books is no longer the position we hold.
[[nodiscard]] constexpr bool corrupts_ledger(AccountingError e) noexcept {
    return e != AccountingError::None && e != AccountingError::DuplicateFill;
}

/// What one fill did to an account.
struct FillOutcome {
    AccountingError error = AccountingError::None;
    /// Signed quantity actually applied. Zero when refused or duplicate.
    Qty applied_quantity{};
    /// Gross realized PnL produced by this fill alone, before fees.
    Notional realized_delta{};
    /// Fee reported by the venue for this fill. Never derived from notional.
    Notional fee{};
    /// True when this fill crossed through zero, closing one side and opening
    /// the other.
    bool flipped = false;
    /// True when the position reached exactly zero.
    bool closed = false;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == AccountingError::None;
    }
};

/// The PnL decomposition §25 requires.
///
/// `gross` is trading PnL before costs; `net` is what is actually left.
/// Keeping them separate matters: a strategy can be right about direction and
/// still lose money, and a single blended number hides which happened.
struct PnlBreakdown {
    Notional realized{};    ///< closed trading PnL, gross of fees
    Notional unrealized{};  ///< open position marked to market, gross of fees
    Notional fees{};        ///< cumulative, always >= 0
    Notional funding{};     ///< perpetual funding; see AccountingConfig

    /// Realized plus unrealized, before costs.
    [[nodiscard]] Notional gross() const noexcept { return realized + unrealized; }

    /// What is actually left after costs. Fees are subtracted, never folded
    /// into the cost basis -- folding them in would make "average entry price"
    /// stop being a price.
    [[nodiscard]] bool checked_net(Notional& out) const noexcept {
        Notional total;
        if (!checked_add(realized, unrealized, total)) {
            return false;
        }
        if (!checked_add(total, funding, total)) {
            return false;
        }
        return checked_sub(total, fees, out);
    }

    /// True when every component is determinate. An indeterminate mark leaves
    /// `unrealized_valid` false and the whole breakdown unusable for a limit.
    bool unrealized_valid = true;
};

/// A normalized mark, supplied through the Phase 10 boundary.
///
/// Accounting never reads a book and never talks to a venue: it is handed the
/// two prices it needs. That is what keeps it independent of which venue --
/// or which simulator -- produced them.
struct MarkPrice {
    Symbol symbol{};
    /// What a long could be sold into.
    Px bid{};
    /// What a short would have to buy back at.
    Px ask{};
    Nanos as_of_ns = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool usable() const noexcept {
        return bid.is_positive() && ask.is_positive() && ask >= bid;
    }

    /// **Longs mark at the bid, shorts at the ask** -- the side you would
    /// actually have to trade against to get out.
    ///
    /// Marking both at the mid is the common alternative and is optimistic by
    /// half the spread in both directions. For a market maker holding
    /// inventory it wants to unwind, that half-spread is precisely the cost
    /// being ignored, and the error is always in the flattering direction.
    [[nodiscard]] Px for_position(Qty position) const noexcept {
        return position.is_negative() ? ask : bid;
    }
};

struct AccountingConfig {
    /// Marks older than this make unrealized PnL indeterminate.
    Nanos max_mark_age_ns = seconds(5);
    /// Symbols the accountant can hold. Fixed at construction so the hot path
    /// never allocates.
    std::size_t max_symbols = 32;
    // Duplicate-detection depth is deliberately NOT configurable. The ring is
    // fixed at `SeenTrades::kCapacity` so a fill never allocates, and a
    // configuration knob that appeared to size it while changing nothing would
    // give an operator a false sense of how far back redelivery is detected.
    // Funding is not configurable either: nothing in this repository produces
    // a funding event, so a flag enabling it would enable nothing. The
    // `funding` component of PnlBreakdown exists because §25 lists it, and
    // carrying an always-zero component is more honest than pretending the
    // concept does not exist -- but a switch for it would be a lie.
};

/// §33-style counters for an operator. No dashboard is built here.
struct AccountingMetrics {
    std::uint64_t fills_applied = 0;
    std::uint64_t duplicate_fills = 0;
    std::uint64_t rejected_fills = 0;
    std::uint64_t flips = 0;
    std::uint64_t closes = 0;
    std::uint64_t marks_applied = 0;
    std::uint64_t marks_rejected = 0;
    std::uint64_t stale_marks = 0;
    std::uint64_t overflows = 0;
    std::uint64_t faults = 0;
};

}  // namespace mm::portfolio
