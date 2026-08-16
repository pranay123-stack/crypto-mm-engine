#pragma once

/// \file Seqlock.hpp
/// Single-writer / multi-reader snapshot publication.
///
/// This is the mechanism behind the hard rule that the dashboard is never in the
/// trading hot path. The trading thread publishes a telemetry snapshot; monitor
/// threads read it. The writer never inspects reader state and never waits, so a
/// monitoring thread that is slow, wedged, or dead cannot apply backpressure to
/// trading. A reader that races the writer simply retries.
///
/// A textbook seqlock copies the payload with a plain `memcpy` and relies on the
/// sequence check to discard torn reads. That read genuinely races the writer,
/// and "the result is discarded anyway" is not a defence the C++ memory model
/// accepts -- it is undefined behaviour, and ThreadSanitizer is right to flag it.
/// The payload here is therefore transferred word by word through
/// `std::atomic_ref` with relaxed ordering: the race becomes a well-defined
/// sequence of atomic accesses, the sequence counter still decides whether the
/// snapshot is coherent, and the cost is a few hundred relaxed loads on a
/// structure published at 10 Hz.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "mm/common/SpscRing.hpp"  // kCacheLine

namespace mm {

template <class T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a seqlock payload is transferred as raw words; a type with non-trivial "
                  "copy semantics would be corrupted or double-freed");
    static_assert(std::is_default_constructible_v<T>);

public:
    using Word = std::uint64_t;
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(Word) - 1) / sizeof(Word);

    Seqlock() = default;
    Seqlock(const Seqlock&) = delete;
    Seqlock& operator=(const Seqlock&) = delete;

    /// Wait-free. An odd sequence marks a write in progress.
    void store(const T& value) noexcept {
        std::array<Word, kWords> src{};
        std::memcpy(src.data(), static_cast<const void*>(&value), sizeof(T));

        const Word s = seq_.load(std::memory_order_relaxed);

        seq_.store(s + 1, std::memory_order_relaxed);
        // Keeps the payload stores below from being hoisted above the odd
        // sequence: a reader must never see even-sequence-with-new-payload.
        std::atomic_thread_fence(std::memory_order_release);

        for (std::size_t i = 0; i < kWords; ++i) {
            std::atomic_ref<Word>(words_[i]).store(src[i], std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_relaxed);
    }

    /// Retries until it observes a coherent snapshot. Returns false when the
    /// writer outran this reader `max_tries` times, in which case the caller
    /// keeps its previous copy -- a marginally stale panel is always preferable
    /// to blocking the trading thread.
    [[nodiscard]] bool load(T& out, int max_tries = 16) const noexcept {
        for (int attempt = 0; attempt < max_tries; ++attempt) {
            const Word before = seq_.load(std::memory_order_relaxed);
            if ((before & 1U) != 0U) {
                continue;  // write in progress
            }
            std::atomic_thread_fence(std::memory_order_acquire);

            std::array<Word, kWords> dst{};
            for (std::size_t i = 0; i < kWords; ++i) {
                dst[i] = std::atomic_ref<Word>(words_[i]).load(std::memory_order_relaxed);
            }

            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before) {
                std::memcpy(static_cast<void*>(&out), dst.data(), sizeof(T));
                return true;
            }
        }
        return false;
    }

    /// Number of completed writes. Lets a reader cheaply detect "nothing new
    /// since last time" without copying the payload at all.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return seq_.load(std::memory_order_acquire) / 2;
    }

private:
    alignas(kCacheLine) std::atomic<Word> seq_{0};
    alignas(kCacheLine) mutable std::array<Word, kWords> words_{};
};

}  // namespace mm
