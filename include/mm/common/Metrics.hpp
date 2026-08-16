#pragma once

/// \file Metrics.hpp
/// Counters and gauges with a Prometheus text exposition.
///
/// Registration takes a lock and happens at startup; updates are relaxed atomic
/// increments on a cached reference. Hot-path code therefore resolves a metric
/// once and pays roughly one uncontended `lock xadd` per update -- cheap enough
/// to instrument the quoting path without distorting what it measures.
///
/// Relaxed ordering is deliberate: a counter is an observation, not a
/// synchronisation point. Nothing in the engine makes a control-flow decision
/// on another thread's counter value, so ordering guarantees would buy nothing
/// and cost a fence on the hot path.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mm {

class Counter {
public:
    void inc(std::uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
    void reset() noexcept { value_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

class Gauge {
public:
    void set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(std::int64_t d) noexcept { value_.fetch_add(d, std::memory_order_relaxed); }
    void inc() noexcept { add(1); }
    void dec() noexcept { add(-1); }
    [[nodiscard]] std::int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> value_{0};
};

/// Labels are rendered in the Prometheus style: `{symbol="BTCUSDT",side="BUY"}`.
using Labels = std::vector<std::pair<std::string, std::string>>;

class MetricsRegistry {
public:
    /// Stable references: metrics are never removed, so a reference cached by
    /// hot-path code at startup stays valid for the process lifetime.
    [[nodiscard]] Counter& counter(std::string_view name, const Labels& labels = {});
    [[nodiscard]] Gauge& gauge(std::string_view name, const Labels& labels = {});

    /// Free-text description, emitted as a `# HELP` line.
    void describe(std::string_view name, std::string_view help);

    [[nodiscard]] std::string render_prometheus() const;

    [[nodiscard]] std::size_t size() const;
    void reset_all();

    [[nodiscard]] static MetricsRegistry& instance();

private:
    [[nodiscard]] static std::string key_of(std::string_view name, const Labels& labels);

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::string, std::less<>> help_;
};

}  // namespace mm
