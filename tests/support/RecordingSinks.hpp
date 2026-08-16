#pragma once

/// Test sinks that record what an adapter delivered.
///
/// These stand in for the engine's real sinks, which (per docs/concurrency.md)
/// do nothing but stamp an event and push it into a ring. Recording is the
/// test-time equivalent.

#include <vector>

#include "mm/exchange/common/IExchangeExecution.hpp"
#include "mm/exchange/common/IExchangeMarketData.hpp"

namespace mm::test {

class RecordingExecutionSink final : public exchange::IExecutionSink {
public:
    void on_execution(const exchange::ExecutionEvent& event) override {
        events.push_back(event);
        // Every adapter-produced event is checked for self-contradiction the
        // moment it arrives, so a malformed one fails the test that produced it
        // rather than a later, unrelated assertion.
        const Status s = exchange::validate_execution_event(event);
        if (s.is_error()) {
            invalid.push_back(s.to_string());
        }
    }

    [[nodiscard]] std::size_t count_of(exchange::ExecutionEventType type) const {
        std::size_t n = 0;
        for (const auto& e : events) {
            if (e.type == type) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] const exchange::ExecutionEvent* first_of(
        exchange::ExecutionEventType type) const {
        for (const auto& e : events) {
            if (e.type == type) {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const exchange::ExecutionEvent* last_of(
        exchange::ExecutionEventType type) const {
        const exchange::ExecutionEvent* found = nullptr;
        for (const auto& e : events) {
            if (e.type == type) {
                found = &e;
            }
        }
        return found;
    }

    void clear() {
        events.clear();
        invalid.clear();
    }

    std::vector<exchange::ExecutionEvent> events;
    std::vector<std::string> invalid;
};

class RecordingMarketDataSink final : public exchange::IMarketDataSink {
public:
    void on_market_data(const exchange::MarketDataEvent& event) override {
        events.push_back(event);
    }

    [[nodiscard]] std::size_t count_of(exchange::MarketDataEventType type) const {
        std::size_t n = 0;
        for (const auto& e : events) {
            if (e.type == type) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] const exchange::MarketDataEvent* last_of(
        exchange::MarketDataEventType type) const {
        const exchange::MarketDataEvent* found = nullptr;
        for (const auto& e : events) {
            if (e.type == type) {
                found = &e;
            }
        }
        return found;
    }

    void clear() { events.clear(); }

    std::vector<exchange::MarketDataEvent> events;
};

}  // namespace mm::test
