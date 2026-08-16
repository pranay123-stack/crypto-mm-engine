#include "mm/common/Fixed.hpp"

#include <array>
#include <cstring>

namespace mm::detail {
namespace {

constexpr std::int64_t kScale = 100'000'000;
constexpr int kDecimals = 8;

constexpr std::array<int128, 39> make_pow10() {
    std::array<int128, 39> p{};
    p[0] = 1;
    for (std::size_t i = 1; i < p.size(); ++i) {
        p[i] = p[i - 1] * 10;
    }
    return p;
}

const std::array<int128, 39> kPow10 = make_pow10();

constexpr int128 kInt64Max = static_cast<int128>(std::numeric_limits<std::int64_t>::max());
constexpr int128 kInt64Min = static_cast<int128>(std::numeric_limits<std::int64_t>::min());

// Guard the mantissa well below the 128-bit ceiling so the later shift by up to
// 10^8 cannot overflow either.
constexpr int128 kMantissaCeiling = int128{1} << 100;

}  // namespace

bool parse_fixed_raw(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }

    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = (text[i] == '-');
        ++i;
    }

    int128 mantissa = 0;
    int frac_digits = 0;
    bool any_digit = false;

    // Integer part.
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        mantissa = mantissa * 10 + static_cast<int128>(text[i] - '0');
        any_digit = true;
        if (mantissa > kMantissaCeiling) {
            return false;
        }
    }

    // Fractional part. Digits beyond the representable scale are accepted only
    // when they are zeros: dropping a zero loses nothing, dropping a non-zero
    // would silently discard money the venue considered significant.
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            const char c = text[i];
            if (frac_digits < kDecimals) {
                mantissa = mantissa * 10 + static_cast<int128>(c - '0');
                ++frac_digits;
            } else if (c != '0') {
                return false;
            }
            any_digit = true;
            if (mantissa > kMantissaCeiling) {
                return false;
            }
        }
    }

    if (!any_digit) {
        return false;
    }

    // Optional exponent: venues occasionally serialise small filter values as
    // "1E-8" rather than "0.00000001".
    int exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exp_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exp_negative = (text[i] == '-');
            ++i;
        }
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
        int exp_value = 0;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            exp_value = exp_value * 10 + (text[i] - '0');
            if (exp_value > 4096) {
                return false;
            }
        }
        exponent = exp_negative ? -exp_value : exp_value;
    }

    if (i != text.size()) {
        return false;  // trailing garbage
    }

    // value = mantissa * 10^(exponent - frac_digits)
    // raw   = value * 10^kDecimals
    const int shift = kDecimals - frac_digits + exponent;

    int128 raw = mantissa;
    if (shift > 0) {
        if (shift >= static_cast<int>(kPow10.size())) {
            return false;
        }
        raw *= kPow10[static_cast<std::size_t>(shift)];
    } else if (shift < 0) {
        const int down = -shift;
        if (down >= static_cast<int>(kPow10.size())) {
            return mantissa == 0 ? (out = 0, true) : false;
        }
        const int128 divisor = kPow10[static_cast<std::size_t>(down)];
        if (raw % divisor != 0) {
            return false;  // would discard significant digits
        }
        raw /= divisor;
    }

    if (negative) {
        raw = -raw;
    }
    if (raw > kInt64Max || raw < kInt64Min) {
        return false;
    }

    out = static_cast<std::int64_t>(raw);
    return true;
}

std::string format_fixed_raw(std::int64_t raw) {
    // Note the explicit unsigned magnitude: negating INT64_MIN is UB.
    const bool negative = raw < 0;
    const std::uint64_t magnitude =
        negative ? (~static_cast<std::uint64_t>(raw) + 1U) : static_cast<std::uint64_t>(raw);

    const std::uint64_t scale = static_cast<std::uint64_t>(kScale);
    const std::uint64_t whole = magnitude / scale;
    std::uint64_t frac = magnitude % scale;

    std::array<char, 8> frac_digits{};
    for (int d = kDecimals - 1; d >= 0; --d) {
        frac_digits[static_cast<std::size_t>(d)] = static_cast<char>('0' + (frac % 10));
        frac /= 10;
    }

    std::size_t frac_len = frac_digits.size();
    while (frac_len > 0 && frac_digits[frac_len - 1] == '0') {
        --frac_len;
    }

    std::string out;
    out.reserve(32);
    if (negative) {
        out.push_back('-');
    }
    out.append(std::to_string(whole));
    if (frac_len > 0) {
        out.push_back('.');
        out.append(frac_digits.data(), frac_len);
    }
    return out;
}

}  // namespace mm::detail
