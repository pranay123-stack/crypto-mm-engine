#pragma once

/// \file OrderRecord.hpp
/// One order's authoritative lifecycle record, and the identity that keys it.

#include <array>
#include <cstdint>

#include "mm/common/Time.hpp"
#include "mm/common/Types.hpp"
#include "mm/oms/OrderTypes.hpp"
#include "mm/quote/QuoteTypes.hpp"

namespace mm::oms {

/// Trade ids already applied to an order.
///
/// Fills are the one event where a duplicate is unrecoverable: double-counting
/// one corrupts position and PnL at the same moment, and nothing downstream can
/// tell afterwards. Venues *do* redeliver execution reports after a private
/// stream reconnect, so this is not defensive programming, it is the normal
/// case.
///
/// A fixed ring rather than a set: no allocation, and an order accumulating
/// more than this many fills is either enormous or a bug -- either way,
/// `overflowed` says so rather than silently forgetting the oldest.
class SeenTradeIds {
public:
    static constexpr std::size_t kCapacity = 32;

    /// Returns false when this id has already been applied.
    [[nodiscard]] bool insert(const TradeId& id) noexcept {
        if (contains(id)) {
            return false;
        }
        ids_[next_] = id;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) {
            ++count_;
        } else {
            // The oldest id has been evicted, so duplicate detection is no
            // longer complete. Surfaced rather than hidden.
            overflowed_ = true;
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

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    void clear() noexcept {
        count_ = 0;
        next_ = 0;
        overflowed_ = false;
    }

private:
    std::array<TradeId, kCapacity> ids_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    bool overflowed_ = false;
};

/// Who an order belongs to. Lets the quote manager, risk and the OMS agree on
/// which subsystem owns what, so no layer ever acts on another's order.
struct OrderOwnership {
    StrategyName strategy{};
    quote::QuoteSlot slot = quote::QuoteSlot::Bid;
    /// The strategy generation the order was created for.
    std::uint64_t generation = 0;
    /// Engine-wide creation order, for deterministic iteration and recovery.
    std::uint64_t creation_sequence = 0;

    [[nodiscard]] bool matches(const StrategyName& owner, quote::QuoteSlot which) const noexcept {
        return !strategy.empty() && strategy == owner && slot == which;
    }
};

/// The authoritative record. One per logical order.
struct OrderRecord {
    // ---- identity: three distinct notions, each absent at some stage ----
    LogicalOrderId logical_id = LogicalOrderId::kInvalid;
    ClientOrderId client_order_id{};
    /// Empty until the venue acknowledges.
    ExchangeOrderId exchange_order_id{};

    OrderOwnership ownership{};

    // ---- what was asked for ----
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Px price{};
    Qty original_quantity{};

    // ---- what has happened ----
    OrderState state = OrderState::Created;
    Qty cumulative_quantity{};
    /// Volume-weighted average of the fills applied so far.
    Px average_fill_price{};
    Notional cumulative_fees{};
    SeenTradeIds seen_trades{};

    // ---- outstanding request ----
    RequestId pending_request = RequestId::kInvalid;
    /// Steady clock, when the outstanding request was sent. Drives the timeout.
    Nanos pending_since_ns = 0;
    /// For a replace: what the order becomes if it is accepted. Held separately
    /// so a rejected replace leaves the live parameters untouched.
    Px pending_price{};
    Qty pending_quantity{};
    ClientOrderId pending_client_order_id{};

    // ---- timing, all steady clock except venue_ns ----
    Nanos created_ns = 0;
    Nanos sent_ns = 0;
    Nanos acknowledged_ns = 0;
    Nanos terminal_ns = 0;
    /// Venue-reported transaction time (wall clock, foreign machine). Never
    /// used to order state transitions.
    Nanos venue_ns = 0;

    /// The venue update sequence last applied. An event older than this is
    /// stale and discarded rather than replayed onto a newer state.
    Seq last_applied_sequence = kNoSeq;
    TraceId trace = kNoTrace;

    /// Remaining size, floored at zero. Never derived from an assumption that
    /// the original quantity still rests.
    [[nodiscard]] Qty remaining() const noexcept {
        Qty left;
        if (!checked_sub(original_quantity, cumulative_quantity, left)) {
            return Qty::zero();
        }
        return left.is_positive() ? left : Qty::zero();
    }
    [[nodiscard]] bool is_terminal() const noexcept { return oms::is_terminal(state); }
    [[nodiscard]] bool is_live() const noexcept { return oms::is_live(state); }
    [[nodiscard]] bool consumes_exposure() const noexcept {
        return oms::consumes_exposure(state);
    }
    [[nodiscard]] bool has_pending_request() const noexcept {
        return pending_request != RequestId::kInvalid;
    }
};

/// One journal record. Append-only, allocation-free, and carrying enough to
/// reconstruct the transition it describes -- which is what makes future replay
/// possible without building a replay engine now.
struct OrderJournalRecord {
    /// Engine sequence. Ordering comes from this, never from a venue timestamp
    /// produced on a machine whose clock we do not control.
    std::uint64_t sequence = 0;

    OrderEventType type = OrderEventType::None;
    LogicalOrderId logical_id = LogicalOrderId::kInvalid;
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Symbol symbol{};
    Side side = Side::Buy;

    OrderState from_state = OrderState::Created;
    OrderState to_state = OrderState::Created;

    Px price{};
    Qty quantity{};
    Qty cumulative_quantity{};
    TradeId trade_id{};

    OmsReject reject = OmsReject::None;
    RequestId request_id = RequestId::kInvalid;

    Nanos recorded_ns = 0;   ///< steady
    Nanos venue_ns = 0;      ///< wall, venue-reported
    std::uint64_t generation = 0;
    TraceId trace = kNoTrace;
};

static_assert(std::is_trivially_copyable_v<OrderJournalRecord>);

}  // namespace mm::oms
