#pragma once

/// \file ReconnectBackoff.hpp
/// Bounded exponential reconnect backoff with deterministic jitter.
///
/// Jitter is not decoration. When a venue has a brief outage, every client
/// reconnects at once; without jitter they all retry on the same schedule and
/// the reconnect storm keeps the venue down. The jitter here is derived from
/// the attempt number and a caller-supplied seed rather than from a random
/// engine, so it decorrelates clients while remaining exactly reproducible in
/// tests — a backoff schedule that cannot be asserted on is a backoff schedule
/// nobody checks.

#include <algorithm>
#include <cstdint>

#include "mm/common/Time.hpp"

namespace mm::exchange {

class ReconnectBackoff {
public:
    struct Config {
        Nanos initial_delay = millis(200);
        Nanos max_delay = seconds(30);
        /// Fraction of the delay that jitter may subtract, in percent.
        std::uint32_t jitter_percent = 25;
        /// Attempts before the caller should stop retrying and declare the
        /// session unrecoverable. Zero means unlimited.
        std::uint32_t max_attempts = 0;
    };

    ReconnectBackoff() = default;
    explicit ReconnectBackoff(Config config, std::uint64_t seed = 0)
        : config_(config), seed_(seed) {}

    /// Delay before the next attempt, and counts that attempt.
    [[nodiscard]] Nanos next_delay() noexcept {
        const std::uint32_t attempt = attempts_++;

        // Double per attempt, saturating at max_delay. Shifting is capped
        // before it can overflow rather than after.
        Nanos delay = config_.initial_delay;
        for (std::uint32_t i = 0; i < attempt && delay < config_.max_delay; ++i) {
            delay *= 2;
        }
        delay = std::min(delay, config_.max_delay);

        if (config_.jitter_percent > 0) {
            // Deterministic hash of (seed, attempt): decorrelates clients that
            // have different seeds, reproduces exactly for a given one.
            std::uint64_t h = seed_ ^ (static_cast<std::uint64_t>(attempt) * 0x9E3779B97F4A7C15ULL);
            h ^= h >> 30;
            h *= 0xBF58476D1CE4E5B9ULL;
            h ^= h >> 27;
            const auto span = static_cast<std::uint64_t>(delay) *
                              static_cast<std::uint64_t>(config_.jitter_percent) / 100U;
            if (span > 0) {
                // Subtract only: never delay longer than the cap promises.
                delay -= static_cast<Nanos>(h % span);
            }
        }
        return delay;
    }

    /// A connection succeeded. The schedule restarts from the initial delay,
    /// so a long-lived session that blips once does not inherit an old backoff.
    void reset() noexcept { attempts_ = 0; }

    [[nodiscard]] std::uint32_t attempts() const noexcept { return attempts_; }

    /// True once the caller should stop retrying. Retrying forever in an
    /// unrecoverable state hides the real fault and risks a venue ban.
    [[nodiscard]] bool exhausted() const noexcept {
        return config_.max_attempts != 0 && attempts_ >= config_.max_attempts;
    }

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    Config config_{};
    std::uint64_t seed_ = 0;
    std::uint32_t attempts_ = 0;
};

}  // namespace mm::exchange
