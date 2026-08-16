#pragma once

/// \file IStrategy.hpp
/// The plug-in contract.
///
/// A strategy is a pure decision function over `StrategyContext`. It receives
/// what it needs to quote and returns what it would like; everything else is
/// the infrastructure's.
///
/// **What a strategy structurally cannot do**, by virtue of what it is handed
/// rather than by convention:
///
/// | Capability            | Why it is impossible                              |
/// | --------------------- | ------------------------------------------------- |
/// | Submit or cancel      | No OMS or exchange reference exists in the context |
/// | Reach the network     | `strategies/` links neither Asio, Beast nor OpenSSL|
/// | Mutate the book       | Given `BookDepthView` by value; it has no setters  |
/// | Read the clock        | `now_ns` is supplied; there is no other source     |
/// | Choose venue params   | `QuoteIntent` cannot express a TIF or an order id  |
/// | Bypass validation     | Its only output is a value the runtime inspects    |
///
/// **Determinism is part of the contract.** Given identical context, identical
/// internal state and identical parameters, `on_market_update` must return an
/// identical `QuoteIntent`. That means no wall clock, no random engine seeded
/// from entropy, no global mutable state, no I/O. A strategy that needs
/// randomness must take a seed through its parameters so a run is reproducible.

#include <memory>

#include "mm/common/Params.hpp"
#include "mm/common/Status.hpp"
#include "mm/strategy/QuoteIntent.hpp"
#include "mm/strategy/StrategyContext.hpp"
#include "mm/strategy/StrategyTypes.hpp"

namespace mm::strategy {

/// Everything a strategy is given at construction.
struct StrategyInit {
    Symbol symbol{};
    /// Venue trading rules. A strategy may read tick and lot size to round its
    /// own prices; the runtime re-checks alignment regardless, because a
    /// strategy is not trusted to have done it.
    InstrumentSpec instrument{};
    /// The finalized parameter block, chosen by the research platform. Opaque
    /// to the engine.
    Params params{};
    /// Assigned by the runtime from configuration; a strategy does not name
    /// itself, so a config cannot silently disagree with the binary.
    StrategyIdentity identity{};
};

class IStrategy {
public:
    virtual ~IStrategy() = default;

    IStrategy() = default;
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;

    /// Validates parameters and prepares internal state.
    ///
    /// An unreadable or nonsensical parameter must be reported here, not
    /// defaulted. A mistyped parameter name should stop the engine from
    /// starting rather than let it trade with a spread nobody chose.
    [[nodiscard]] virtual Status initialize(const StrategyInit& init) = 0;

    /// The market moved. Called only when the runtime's evaluation mode says so
    /// and the book is quotable.
    [[nodiscard]] virtual QuoteIntent on_market_update(const StrategyContext& context) = 0;

    /// Periodic evaluation. Lets a strategy refresh quotes in a quiet market
    /// and decay time-dependent state without inventing its own timer.
    [[nodiscard]] virtual QuoteIntent on_timer(const StrategyContext& context) = 0;

    /// One of our quotes traded. Position is supplied, so a strategy never has
    /// to accumulate it and cannot drift from the portfolio's view.
    virtual void on_fill(const StrategyFill& fill) = 0;

    /// One of our quotes changed state. `QuoteLifecycle::Unknown` means the
    /// infrastructure has lost track of it; the strategy should treat that side
    /// as uncertain rather than free.
    virtual void on_order_event(const StrategyOrderEvent& event) = 0;

    /// Discards internal state without destroying the instance. Used when
    /// market data resynchronizes: whatever the strategy inferred from the old
    /// book is no longer about the book that exists.
    virtual void reset() = 0;

    /// Self-reported identity, checked against configuration at initialization.
    /// A mismatch is a wiring error caught at startup rather than a strategy
    /// quietly running under another's name in the journal.
    [[nodiscard]] virtual StrategyIdentity identity() const = 0;
};

using StrategyPtr = std::unique_ptr<IStrategy>;

}  // namespace mm::strategy
