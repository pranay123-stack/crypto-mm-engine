#pragma once

/// Deliberately misbehaving strategies.
///
/// The runtime's job is to contain a plug-in it does not trust, so the tests
/// need plug-ins that are actually untrustworthy: one that throws, one that
/// takes too long, one that returns nonsense, one that lies about its identity.
/// Each is the minimum needed to exercise one containment path.

#include <stdexcept>

#include "mm/strategy/IStrategy.hpp"

namespace mm::test {

using strategy::IntentReason;
using strategy::QuoteAction;
using strategy::QuoteIntent;
using strategy::StrategyContext;
using strategy::StrategyIdentity;
using strategy::StrategyInit;

/// Base with the boilerplate every test strategy shares.
class TestStrategyBase : public strategy::IStrategy {
public:
    TestStrategyBase(const char* name, std::int32_t version) : version_(version) {
        static_cast<void>(name_.assign(name));
    }

    [[nodiscard]] Status initialize(const StrategyInit& init) override {
        instrument_ = init.instrument;
        return Status::ok();
    }
    [[nodiscard]] QuoteIntent on_timer(const StrategyContext& c) override {
        return on_market_update(c);
    }
    void on_fill(const strategy::StrategyFill&) override { ++fills; }
    void on_order_event(const strategy::StrategyOrderEvent&) override { ++order_events; }
    void reset() override { resets++; }
    [[nodiscard]] StrategyIdentity identity() const override {
        StrategyIdentity id;
        id.name = name_;
        id.version = version_;
        return id;
    }

    std::uint64_t fills = 0;
    std::uint64_t order_events = 0;
    std::uint64_t resets = 0;

protected:
    /// A valid two-sided quote one tick inside the touch.
    [[nodiscard]] QuoteIntent good_quote(const StrategyContext& c) const {
        QuoteIntent intent;
        intent.action = QuoteAction::Quote;
        intent.reason = IntentReason::Normal;
        intent.quote_bid = true;
        intent.quote_ask = true;
        intent.bid_price = c.bbo.bid_px;
        intent.ask_price = c.bbo.ask_px;
        intent.bid_quantity = quote_size;
        intent.ask_quantity = quote_size;
        intent.market_sequence = c.sequence;
        return intent;
    }

    StrategyName name_{};
    std::int32_t version_ = 1;
    InstrumentSpec instrument_{};

public:
    Qty quote_size = Qty::from_raw(100'000);  // 0.001
};

/// Quotes correctly. The control.
class WellBehavedStrategy final : public TestStrategyBase {
public:
    WellBehavedStrategy() : TestStrategyBase("test_good_v1", 1) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext& c) override {
        ++evaluations;
        return good_quote(c);
    }
    std::uint64_t evaluations = 0;
};

/// Throws on evaluation. An exception escaping into the trading thread would
/// take down a process holding live orders.
class ThrowingStrategy final : public TestStrategyBase {
public:
    ThrowingStrategy() : TestStrategyBase("test_throw_v1", 1) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext&) override {
        throw std::runtime_error("deliberate strategy failure");
    }
};

/// Throws something that is not a std::exception.
class ThrowingNonStandardStrategy final : public TestStrategyBase {
public:
    ThrowingNonStandardStrategy() : TestStrategyBase("test_throw2_v1", 1) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext&) override {
        throw 42;  // NOLINT(hicpp-exception-baseclass)
    }
};

/// Burns time on every evaluation, driven by a `ManualClock` the test advances
/// so slowness is deterministic rather than dependent on machine speed.
class SlowStrategy final : public TestStrategyBase {
public:
    SlowStrategy() : TestStrategyBase("test_slow_v1", 1) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext& c) override {
        if (clock != nullptr) {
            clock->advance(delay);
        }
        return good_quote(c);
    }
    ManualClock* clock = nullptr;
    Nanos delay = millis(1);
};

/// Returns whatever the test tells it to. Used to drive every rejection cause.
class ScriptedStrategy final : public TestStrategyBase {
public:
    ScriptedStrategy() : TestStrategyBase("test_scripted_v1", 1) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext& c) override {
        ++evaluations;
        QuoteIntent intent = next;
        if (use_context_sequence) {
            intent.market_sequence = c.sequence;
        }
        return intent;
    }
    QuoteIntent next{};
    bool use_context_sequence = true;
    std::uint64_t evaluations = 0;
};

/// Reports an identity that disagrees with the configuration. A strategy
/// running under another's name misattributes every fill it produces.
class MisidentifiedStrategy final : public TestStrategyBase {
public:
    MisidentifiedStrategy() : TestStrategyBase("test_wrong_name_v1", 7) {}
    [[nodiscard]] QuoteIntent on_market_update(const StrategyContext& c) override {
        return good_quote(c);
    }
};

}  // namespace mm::test
