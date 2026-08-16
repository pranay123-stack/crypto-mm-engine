#pragma once

/// \file InlineString.hpp
/// Fixed-capacity, allocation-free string used for identifiers that cross the
/// trading thread: symbols, client order ids, exchange order ids, venue names.
///
/// `std::string` would allocate. The trading thread must not allocate in steady
/// state, and events living in ring buffers must be trivially copyable, so every
/// identifier on that path is an inline character array with an explicit bound.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace mm {

template <std::size_t Cap>
class InlineString {
    static_assert(Cap > 0 && Cap <= 250, "InlineString capacity must fit a uint8 length");

public:
    static constexpr std::size_t kCapacity = Cap;

    constexpr InlineString() noexcept = default;

    /// Truncating construction. Intended for literals and tests, where the
    /// bound is known statically. Code handling venue input must use `assign`
    /// and check the result -- a truncated exchange order id is not an
    /// identifier, it is a bug that surfaces as a phantom order.
    constexpr explicit InlineString(std::string_view sv) noexcept {
        static_cast<void>(assign(sv));
    }

    /// Copies at most `Cap` bytes. Returns false when `sv` did not fit, in which
    /// case the value holds the truncated prefix and must not be used as a key.
    [[nodiscard]] constexpr bool assign(std::string_view sv) noexcept {
        const std::size_t n = std::min(sv.size(), Cap);
        for (std::size_t i = 0; i < n; ++i) {
            data_[i] = sv[i];
        }
        data_[n] = '\0';
        len_ = static_cast<std::uint8_t>(n);
        return n == sv.size();
    }

    constexpr void clear() noexcept {
        len_ = 0;
        data_[0] = '\0';
    }

    /// Append; returns false (and appends nothing) when the result would not fit.
    [[nodiscard]] constexpr bool append(std::string_view sv) noexcept {
        if (len_ + sv.size() > Cap) {
            return false;
        }
        for (std::size_t i = 0; i < sv.size(); ++i) {
            data_[len_ + i] = sv[i];
        }
        len_ = static_cast<std::uint8_t>(len_ + sv.size());
        data_[len_] = '\0';
        return true;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(data_.data(), len_);
    }
    [[nodiscard]] constexpr const char* c_str() const noexcept { return data_.data(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return len_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len_ == 0; }
    [[nodiscard]] std::string to_string() const { return std::string(view()); }

    [[nodiscard]] friend constexpr bool operator==(const InlineString& a,
                                                   const InlineString& b) noexcept {
        return a.view() == b.view();
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(
        const InlineString& a, const InlineString& b) noexcept {
        const int c = a.view().compare(b.view());
        return c < 0 ? std::strong_ordering::less
                     : (c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
    }

    friend std::ostream& operator<<(std::ostream& os, const InlineString& s) {
        return os << s.view();
    }

private:
    std::array<char, Cap + 1> data_{};
    std::uint8_t len_ = 0;
};

/// FNV-1a. Cheap, well-distributed for short identifiers, and constexpr-able so
/// symbol lookup tables can be built at compile time.
[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view sv) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : sv) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace mm

namespace std {
template <std::size_t Cap>
struct hash<mm::InlineString<Cap>> {
    [[nodiscard]] std::size_t operator()(const mm::InlineString<Cap>& s) const noexcept {
        return static_cast<std::size_t>(mm::fnv1a(s.view()));
    }
};
}  // namespace std
