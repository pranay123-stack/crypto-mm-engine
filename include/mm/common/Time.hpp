#pragma once

/// \file Time.hpp
/// Time sources.
///
/// Two clocks, deliberately distinct types of truth:
///
///  * **steady** (`CLOCK_MONOTONIC`) is the only clock used for latency,
///    timeouts, rate windows and staleness. It cannot jump backwards when NTP
///    steps the machine, which is exactly when a wall-clock-based timeout would
///    either fire spuriously or never fire at all.
///  * **wall** (`CLOCK_REALTIME`) exists to compare against venue timestamps, to
///    stamp journal records, and to monitor clock skew. It is never subtracted
///    from a steady reading.

#include <cstdint>
#include <string>

namespace mm {

using Nanos = std::int64_t;

constexpr Nanos kNanosPerMicro = 1'000;
constexpr Nanos kNanosPerMilli = 1'000'000;
constexpr Nanos kNanosPerSecond = 1'000'000'000;

[[nodiscard]] constexpr Nanos micros(Nanos n) noexcept { return n * kNanosPerMicro; }
[[nodiscard]] constexpr Nanos millis(Nanos n) noexcept { return n * kNanosPerMilli; }
[[nodiscard]] constexpr Nanos seconds(Nanos n) noexcept { return n * kNanosPerSecond; }

[[nodiscard]] constexpr double to_micros(Nanos n) noexcept {
    return static_cast<double>(n) / static_cast<double>(kNanosPerMicro);
}
[[nodiscard]] constexpr double to_millis(Nanos n) noexcept {
    return static_cast<double>(n) / static_cast<double>(kNanosPerMilli);
}

/// Monotonic. Use for every duration the engine computes about itself.
[[nodiscard]] Nanos steady_ns() noexcept;

/// Realtime. Use for venue-timestamp comparison and human-readable output only.
[[nodiscard]] Nanos wall_ns() noexcept;

/// ISO-8601 with millisecond precision, e.g. "2026-08-17T09:14:22.317Z".
[[nodiscard]] std::string format_wall_iso8601(Nanos wall_nanos);

/// Injectable clock for components whose behaviour is time-dependent (order
/// timeouts, rate limiters, staleness detectors). Tests drive a `ManualClock`
/// so those paths are deterministic rather than sleep-based.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual Nanos steady() const noexcept = 0;
    [[nodiscard]] virtual Nanos wall() const noexcept = 0;
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] Nanos steady() const noexcept override { return steady_ns(); }
    [[nodiscard]] Nanos wall() const noexcept override { return wall_ns(); }

    /// Process-wide instance; the engine holds a reference, never ownership.
    [[nodiscard]] static SystemClock& instance() noexcept;
};

class ManualClock final : public Clock {
public:
    explicit ManualClock(Nanos steady_start = 0, Nanos wall_start = 0) noexcept
        : steady_(steady_start), wall_(wall_start) {}

    [[nodiscard]] Nanos steady() const noexcept override { return steady_; }
    [[nodiscard]] Nanos wall() const noexcept override { return wall_; }

    void advance(Nanos delta) noexcept {
        steady_ += delta;
        wall_ += delta;
    }
    void set_steady(Nanos v) noexcept { steady_ = v; }
    void set_wall(Nanos v) noexcept { wall_ = v; }

private:
    Nanos steady_;
    Nanos wall_;
};

}  // namespace mm
