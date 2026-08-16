#pragma once

/// \file SpscRing.hpp
/// Bounded single-producer / single-consumer queue.
///
/// This is the only mechanism by which data crosses a thread boundary into or
/// out of the trading thread (see docs/concurrency.md). It never blocks either
/// side: `try_push` fails when full and `try_pop` fails when empty, forcing the
/// caller to make the overflow policy explicit rather than inheriting an
/// accidental one.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace mm {

/// Conservative false-sharing granularity. `std::hardware_destructive_
/// interference_size` is 64 on x86-64 but is not universally available as a
/// constant expression, and a wrong value here silently reintroduces the cache
/// contention this class exists to avoid.
inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "capacity must leave room for at least one element");
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two for masking");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_trivially_copyable_v<T>,
                  "ring elements must transfer without throwing: the producer cannot unwind");

public:
    using value_type = T;
    static constexpr std::size_t kCapacity = Capacity;

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // ------------------------------------------------------------- producer
    [[nodiscard]] bool try_push(const T& item) noexcept {
        return emplace_with([&item](T& slot) { slot = item; });
    }

    [[nodiscard]] bool try_push(T&& item) noexcept {
        return emplace_with([&item](T& slot) { slot = std::move(item); });
    }

    /// Construct in place. Saves a copy for wide event structs: the producer
    /// fills the ring slot directly rather than filling a stack temporary and
    /// copying it in.
    template <class Fn>
    [[nodiscard]] bool emplace_with(Fn&& fill) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = tail + 1;

        // Fast path uses the cached consumer index; only when it says "full" do
        // we pay for a coherence miss to re-read the real one.
        if (next - cached_head_ > Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next - cached_head_ > Capacity) {
                return false;
            }
        }

        std::forward<Fn>(fill)(buffer_[tail & kMask]);
        // Release: everything written into the slot above is visible to a
        // consumer that acquires this tail.
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // ------------------------------------------------------------- consumer
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return false;
            }
        }

        out = std::move(buffer_[head & kMask]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Read the front element without consuming it.
    [[nodiscard]] T* peek() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return nullptr;
            }
        }
        return &buffer_[head & kMask];
    }

    /// Drop the element returned by the last successful `peek`.
    void pop_peeked() noexcept {
        head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // ---------------------------------------------------------- observation
    /// Approximate: both endpoints move concurrently. Suitable for telemetry
    /// and backpressure heuristics, never for control flow.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Peak occupancy observed by the producer, for capacity tuning. A ring
    /// that habitually runs near full is one burst away from an overflow, and
    /// overflow on the market-data ring means SAFE_MODE.
    [[nodiscard]] std::size_t high_water_mark() const noexcept {
        return high_water_.load(std::memory_order_relaxed);
    }

    void observe_high_water() noexcept {
        const std::size_t n = size_approx();
        if (n > high_water_.load(std::memory_order_relaxed)) {
            high_water_.store(n, std::memory_order_relaxed);
        }
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer writes
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer writes

    // Each side keeps a private, stale-tolerant copy of the other's index.
    alignas(kCacheLine) std::size_t cached_head_{0};  // producer-private
    alignas(kCacheLine) std::size_t cached_tail_{0};  // consumer-private

    alignas(kCacheLine) std::atomic<std::size_t> high_water_{0};
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

}  // namespace mm
