#pragma once

/// \file ObjectPool.hpp
/// Fixed-capacity object pool with stable indices.
///
/// The trading thread must not allocate in steady state: `malloc` can take a
/// lock, can take a page fault, and has a tail measured in microseconds at
/// exactly the wrong moment. Orders and other per-event objects come from a
/// pool sized at startup, so the engine's memory footprint is decided before it
/// ever quotes -- and running out is a bounded, observable condition rather
/// than an OOM kill.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace mm {

/// Not thread-safe: a pool is owned by exactly one thread (see
/// docs/concurrency.md §1).
template <class T, std::size_t Capacity>
class ObjectPool {
    static_assert(Capacity > 0);
    static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max() - 1);
    static_assert(std::is_default_constructible_v<T>);

public:
    using Index = std::uint32_t;
    static constexpr Index kInvalidIndex = std::numeric_limits<Index>::max();
    static constexpr std::size_t kCapacity = Capacity;

    ObjectPool() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_list_[i] = static_cast<Index>(Capacity - 1 - i);  // hand out 0 first
        }
        free_count_ = Capacity;
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /// Returns `kInvalidIndex` when exhausted. Callers must treat exhaustion as
    /// a real condition -- for the order pool it means "stop quoting", not
    /// "try harder".
    [[nodiscard]] Index acquire() noexcept {
        if (free_count_ == 0) {
            return kInvalidIndex;
        }
        const Index idx = free_list_[--free_count_];
        in_use_[idx] = true;
        slots_[idx] = T{};  // every acquire yields a defined object
        ++acquired_total_;
        if (Capacity - free_count_ > high_water_) {
            high_water_ = Capacity - free_count_;
        }
        return idx;
    }

    /// Double-release and out-of-range release are detected and refused rather
    /// than corrupting the free list, which would hand the same order object to
    /// two logical owners.
    [[nodiscard]] bool release(Index idx) noexcept {
        if (idx >= Capacity || !in_use_[idx]) {
            ++invalid_releases_;
            return false;
        }
        in_use_[idx] = false;
        free_list_[free_count_++] = idx;
        return true;
    }

    [[nodiscard]] T& operator[](Index idx) noexcept { return slots_[idx]; }
    [[nodiscard]] const T& operator[](Index idx) const noexcept { return slots_[idx]; }

    /// Bounds- and liveness-checked access. Returns nullptr for a stale handle,
    /// which is how the OMS distinguishes "order I still own" from "handle that
    /// was recycled".
    [[nodiscard]] T* get(Index idx) noexcept {
        return (idx < Capacity && in_use_[idx]) ? &slots_[idx] : nullptr;
    }
    [[nodiscard]] const T* get(Index idx) const noexcept {
        return (idx < Capacity && in_use_[idx]) ? &slots_[idx] : nullptr;
    }

    [[nodiscard]] bool is_live(Index idx) const noexcept {
        return idx < Capacity && in_use_[idx];
    }

    [[nodiscard]] std::size_t in_use() const noexcept { return Capacity - free_count_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] std::size_t high_water_mark() const noexcept { return high_water_; }
    [[nodiscard]] std::uint64_t acquired_total() const noexcept { return acquired_total_; }
    [[nodiscard]] std::uint64_t invalid_releases() const noexcept { return invalid_releases_; }

    /// Utilisation in [0, 1]; surfaced on the dashboard so pool exhaustion is
    /// visible long before it happens.
    [[nodiscard]] double utilisation() const noexcept {
        return static_cast<double>(in_use()) / static_cast<double>(Capacity);
    }

    template <class Fn>
    void for_each_live(Fn&& fn) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (in_use_[i]) {
                fn(static_cast<Index>(i), slots_[i]);
            }
        }
    }

private:
    std::array<T, Capacity> slots_{};
    std::array<Index, Capacity> free_list_{};
    std::array<bool, Capacity> in_use_{};
    std::size_t free_count_ = 0;
    std::size_t high_water_ = 0;
    std::uint64_t acquired_total_ = 0;
    std::uint64_t invalid_releases_ = 0;
};

}  // namespace mm
