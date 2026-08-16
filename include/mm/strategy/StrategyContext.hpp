#pragma once

/// \file StrategyContext.hpp
/// Everything a market-making strategy is given, and nothing else.
///
/// The context is **immutable and non-owning**. It is assembled by the runtime
/// immediately before each evaluation and is valid only for the duration of
/// that call. It deliberately contains no socket, no REST client, no OMS
/// handle, no mutex, no queue and no venue object — a strategy cannot reach the
/// exchange because it is never handed anything that could.
///
/// It also contains no `OrderBook&`. The book is exposed through a read-only
/// `BookDepthView`, so a strategy cannot mutate it even by accident and does
/// not compile against the book's implementation.

#include <cstddef>
#include <cstdint>

#include "mm/common/Time.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"
#include "mm/strategy/StrategyTypes.hpp"

namespace mm::strategy {

using exchange::PriceLevel;
using exchange::SessionState;

/// A read-only window onto the top of the book. Non-owning and cheap to build;
/// the runtime points it at the live book with no copy.
class BookDepthView {
public:
    constexpr BookDepthView() noexcept = default;
    constexpr BookDepthView(const PriceLevel* bids, std::size_t bid_count, const PriceLevel* asks,
                            std::size_t ask_count) noexcept
        : bids_(bids), bid_count_(bid_count), asks_(asks), ask_count_(ask_count) {}

    [[nodiscard]] constexpr std::size_t bid_depth() const noexcept { return bid_count_; }
    [[nodiscard]] constexpr std::size_t ask_depth() const noexcept { return ask_count_; }
    [[nodiscard]] constexpr std::size_t depth(Side side) const noexcept {
        return side == Side::Buy ? bid_count_ : ask_count_;
    }

    /// Level `index` from the touch, 0 being best. Returns an empty level when
    /// out of range rather than reading past the end: a strategy asking for
    /// depth that is not there gets zeros, not undefined behaviour.
    [[nodiscard]] constexpr PriceLevel level(Side side, std::size_t index) const noexcept {
        const PriceLevel* data = (side == Side::Buy) ? bids_ : asks_;
        const std::size_t count = depth(side);
        if (data == nullptr || index >= count) {
            return PriceLevel{};
        }
        return data[index];
    }

    /// Total quantity across the best `levels` levels.
    [[nodiscard]] constexpr Qty cumulative_qty(Side side, std::size_t levels) const noexcept {
        const PriceLevel* data = (side == Side::Buy) ? bids_ : asks_;
        const std::size_t count = depth(side);
        if (data == nullptr) {
            return Qty::zero();
        }
        Qty total;
        const std::size_t n = levels < count ? levels : count;
        for (std::size_t i = 0; i < n; ++i) {
            total += data[i].quantity;
        }
        return total;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return bid_count_ == 0 && ask_count_ == 0;
    }

private:
    const PriceLevel* bids_ = nullptr;
    std::size_t bid_count_ = 0;
    const PriceLevel* asks_ = nullptr;
    std::size_t ask_count_ = 0;
};

/// One of our own resting quotes, as the strategy is allowed to see it.
///
/// Carries no order id and no venue state: a strategy that knew an order id
/// would be one step from being able to reference it, and referencing orders is
/// the OMS's job. This says only "there is a quote of this size at this price".
struct WorkingQuote {
    bool active = false;
    Px price{};
    Qty quantity{};       ///< remaining, not original
    Qty filled_quantity{};
    /// True when a cancel or replace is in flight. The strategy should treat
    /// such a quote as neither reliably present nor reliably gone.
    bool in_transition = false;
};

static_assert(std::is_trivially_copyable_v<WorkingQuote>);

/// Inventory as the strategy sees it. Signed: negative is short.
struct InventoryView {
    Qty position{};
    /// The hard bound the risk layer will enforce. Given to the strategy so it
    /// can skew *before* being clamped — being told "no" by risk is a worse
    /// outcome than not asking.
    Qty position_limit{};
    Notional notional_exposure{};

    /// Signed room remaining on a side, never negative.
    [[nodiscard]] Qty headroom(Side side) const noexcept {
        const Qty limit = position_limit;
        if (!limit.is_positive()) {
            return Qty::zero();
        }
        const Qty room =
            (side == Side::Buy) ? (limit - position) : (limit + position);
        return room.is_positive() ? room : Qty::zero();
    }

    /// Position as a fraction of the limit in [-1, 1]; 0 when no limit is set.
    /// Reporting and skew only — never for sizing, which must stay fixed point.
    [[nodiscard]] double utilisation() const noexcept {
        if (!position_limit.is_positive()) {
            return 0.0;
        }
        return static_cast<double>(position.raw()) / static_cast<double>(position_limit.raw());
    }
};

/// The complete input to one evaluation.
struct StrategyContext {
    // ---- identity ----
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    // ---- market ----
    BestBidAsk bbo{};
    BookDepthView depth{};
    /// Venue sequence the book is current as of.
    Seq sequence = kNoSeq;

    // ---- time ----
    /// Venue-reported event time (wall clock, foreign machine). For staleness
    /// heuristics only; never subtracted from a steady reading.
    Nanos exchange_ns = 0;
    /// Steady clock, stamped when the event was read from the socket.
    Nanos recv_ns = 0;
    /// Steady clock, now. **This is the only clock a strategy may use.** Taking
    /// the time itself would make evaluation non-deterministic and untestable.
    Nanos now_ns = 0;
    /// Steady-clock age of the most recent market data.
    Nanos data_age_ns = 0;

    // ---- venue rules ----
    /// Tick, lot, min notional. Non-owning; valid for the call only. Null when
    /// the venue has not published them yet, in which case the runtime will not
    /// evaluate at all — a strategy must never guess a tick size.
    const InstrumentSpec* instrument = nullptr;

    // ---- state ----
    SessionState session_state = SessionState::Disconnected;
    InventoryView inventory{};
    WorkingQuote working_bid{};
    WorkingQuote working_ask{};

    // ---- why we are here ----
    TriggerReason trigger = TriggerReason::None;
    /// False when the operator has quoting switched off. The strategy is still
    /// evaluated so its internal state stays warm, but the runtime will not
    /// accept a quoting intent.
    bool quoting_enabled = true;
    /// Bumped when parameters change; echoed into the intent for attribution.
    std::uint64_t config_generation = 0;

    // ---- convenience, all derived ----
    [[nodiscard]] Px mid() const noexcept { return bbo.mid(); }
    [[nodiscard]] Px micro_price() const noexcept { return bbo.micro_price(); }
    [[nodiscard]] Px spread() const noexcept { return bbo.spread(); }
    [[nodiscard]] bool has_two_sided_market() const noexcept { return bbo.is_sane(); }
    [[nodiscard]] bool is_book_quotable() const noexcept {
        return exchange::is_quotable(session_state);
    }
};

// ---------------------------------------------------------------------------
// Notifications
//
// Strategy-facing summaries, not the exchange's own events: no exchange order
// id, no venue code, no fee asset. A strategy that received an execution report
// would be receiving venue detail it has no business acting on.
// ---------------------------------------------------------------------------

struct StrategyFill {
    Symbol symbol{};
    Side side = Side::Buy;
    Px price{};
    Qty quantity{};
    /// Whether we provided or took liquidity. A maker cares: taking usually
    /// means its quote was stale.
    Liquidity liquidity = Liquidity::Unknown;
    /// Position after this fill, so the strategy never has to accumulate it.
    Qty resulting_position{};
    Nanos recv_ns = 0;
    TraceId trace = kNoTrace;
};

/// What happened to one of our quotes, in terms the strategy can use.
enum class QuoteLifecycle : std::uint8_t {
    None = 0,
    Placed,
    Amended,
    Cancelled,
    Rejected,
    /// The infrastructure no longer knows whether this quote is live. The
    /// strategy should treat the side as uncertain rather than free.
    Unknown,
};
[[nodiscard]] std::string_view to_string(QuoteLifecycle e) noexcept;

struct StrategyOrderEvent {
    Symbol symbol{};
    Side side = Side::Buy;
    QuoteLifecycle event = QuoteLifecycle::None;
    Px price{};
    Qty remaining_quantity{};
    Nanos recv_ns = 0;
    TraceId trace = kNoTrace;
};

}  // namespace mm::strategy
