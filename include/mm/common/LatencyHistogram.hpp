#pragma once

/// \file LatencyHistogram.hpp
/// Log-linear latency histogram.
///
/// Averages hide exactly the behaviour that matters to a market maker: the tail.
/// A 40 us mean with a 9 ms p99.9 is a system that is occasionally two orders of
/// magnitude late, and a mean will never show it. Every latency claim in this
/// repository is backed by percentiles from this structure or by Google
/// Benchmark -- never by a mean, and never by assertion.
///
/// Single-threaded by design: the trading thread owns its histograms and
/// publishes computed percentiles into the telemetry snapshot. That keeps
/// recording to a handful of instructions with no atomics.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mm/common/InlineString.hpp"
#include "mm/common/Time.hpp"

namespace mm {

class LatencyHistogram {
public:
    /// 64 sub-buckets per power of two: values are resolved to within ~1.6%,
    /// which is far finer than the run-to-run variation of the thing measured.
    static constexpr int kSubBits = 6;
    static constexpr std::uint64_t kSubCount = std::uint64_t{1} << kSubBits;
    static constexpr std::size_t kBucketCount = 64 - static_cast<std::size_t>(kSubBits) + 1;
    static constexpr std::size_t kSlots = kBucketCount * kSubCount;

    LatencyHistogram() = default;
    explicit LatencyHistogram(std::string_view name) : name_(name) {}

    void record(Nanos value) noexcept {
        const std::uint64_t v = value > 0 ? static_cast<std::uint64_t>(value) : 0U;
        ++counts_[slot_of(v)];
        ++count_;
        sum_ += v;
        if (v > max_) { max_ = v; }
        if (v < min_) { min_ = v; }
    }

    /// Convenience for the ubiquitous "stamp, do work, record" pattern.
    void record_since(Nanos start_steady_ns) noexcept { record(steady_ns() - start_steady_ns); }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] Nanos min() const noexcept {
        return count_ == 0 ? 0 : static_cast<Nanos>(min_);
    }
    [[nodiscard]] Nanos max() const noexcept { return static_cast<Nanos>(max_); }
    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    /// Highest value in the bucket that contains the requested quantile, i.e.
    /// the reported number is never optimistic. `p` is in [0, 100].
    [[nodiscard]] Nanos percentile(double p) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const double clamped = std::clamp(p, 0.0, 100.0);
        // Rank of the observation we want, 1-based. Ceiling rather than nearest:
        // rounding down would return a value below the requested quantile, which
        // is the one direction a latency report must never err in.
        auto rank = static_cast<std::uint64_t>(
            std::ceil((clamped / 100.0) * static_cast<double>(count_)));
        if (rank == 0) { rank = 1; }
        if (rank > count_) { rank = count_; }

        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kSlots; ++i) {
            cumulative += counts_[i];
            if (cumulative >= rank) {
                // The slot's upper bound overstates by at most one bucket width;
                // clamping to the largest observed sample keeps every reported
                // percentile inside [min, max] and p100 exactly equal to max.
                return static_cast<Nanos>(std::min(slot_upper_bound(i), max_));
            }
        }
        return static_cast<Nanos>(max_);
    }

    [[nodiscard]] Nanos p50() const noexcept { return percentile(50.0); }
    [[nodiscard]] Nanos p90() const noexcept { return percentile(90.0); }
    [[nodiscard]] Nanos p95() const noexcept { return percentile(95.0); }
    [[nodiscard]] Nanos p99() const noexcept { return percentile(99.0); }
    [[nodiscard]] Nanos p999() const noexcept { return percentile(99.9); }

    void reset() noexcept {
        counts_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = kUnsetMin;
        max_ = 0;
    }

    /// Fold another histogram in. Used to aggregate per-symbol histograms for a
    /// venue-level view; both must be quiescent or owned by the caller.
    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kSlots; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        max_ = std::max(max_, other.max_);
        min_ = std::min(min_, other.min_);
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_.view(); }
    void set_name(std::string_view n) { static_cast<void>(name_.assign(n)); }

    /// Exposed for tests and for the metrics exporter.
    [[nodiscard]] static std::size_t slot_of(std::uint64_t v) noexcept {
        if (v < kSubCount) {
            return static_cast<std::size_t>(v);
        }
        const int msb = 63 - std::countl_zero(v);
        const auto bucket = static_cast<std::size_t>(msb - kSubBits + 1);
        const std::uint64_t sub = (v >> (bucket - 1)) - kSubCount;
        return bucket * kSubCount + static_cast<std::size_t>(sub);
    }

    [[nodiscard]] static std::uint64_t slot_lower_bound(std::size_t slot) noexcept {
        const std::size_t bucket = slot / kSubCount;
        const std::uint64_t sub = slot % kSubCount;
        if (bucket == 0) {
            return sub;
        }
        return (sub + kSubCount) << (bucket - 1);
    }

    [[nodiscard]] static std::uint64_t slot_upper_bound(std::size_t slot) noexcept {
        const std::size_t bucket = slot / kSubCount;
        const std::uint64_t lower = slot_lower_bound(slot);
        if (bucket == 0) {
            return lower;
        }
        return lower + (std::uint64_t{1} << (bucket - 1)) - 1;
    }

private:
    static constexpr std::uint64_t kUnsetMin = ~std::uint64_t{0};

    std::array<std::uint64_t, kSlots> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = kUnsetMin;
    std::uint64_t max_ = 0;
    InlineString<32> name_{};
};

/// Percentile bundle small enough to live inside a telemetry snapshot.
struct LatencySummary {
    std::uint64_t count = 0;
    Nanos min_ns = 0;
    Nanos p50_ns = 0;
    Nanos p90_ns = 0;
    Nanos p95_ns = 0;
    Nanos p99_ns = 0;
    Nanos p999_ns = 0;
    Nanos max_ns = 0;
    double mean_ns = 0.0;

    [[nodiscard]] static LatencySummary from(const LatencyHistogram& h) noexcept {
        return LatencySummary{h.count(), h.min(),   h.p50(), h.p90(),  h.p95(),
                              h.p99(),   h.p999(),  h.max(), h.mean()};
    }
};

static_assert(std::is_trivially_copyable_v<LatencySummary>);

/// RAII stopwatch: records the enclosing scope's duration on destruction.
class ScopedLatency {
public:
    explicit ScopedLatency(LatencyHistogram& hist) noexcept
        : hist_(hist), start_(steady_ns()) {}
    ScopedLatency(const ScopedLatency&) = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;
    ~ScopedLatency() { hist_.record(steady_ns() - start_); }

private:
    LatencyHistogram& hist_;
    Nanos start_;
};

}  // namespace mm
