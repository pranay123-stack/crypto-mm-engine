#include "mm/oms/ClientOrderIdGenerator.hpp"

#include <array>
#include <charconv>

namespace mm::oms {
namespace {

/// Base-36 keeps the id short enough to fit a 36-character venue limit while
/// staying alphanumeric, which every venue accepts.
constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

std::size_t encode_base36(std::uint64_t value, std::array<char, 16>& out) noexcept {
    if (value == 0) {
        out[0] = '0';
        return 1;
    }
    std::size_t n = 0;
    while (value > 0 && n < out.size()) {
        out[n++] = kDigits[value % 36];
        value /= 36;
    }
    // Written least-significant first; reverse in place.
    for (std::size_t i = 0; i < n / 2; ++i) {
        const char tmp = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = tmp;
    }
    return n;
}

[[nodiscard]] bool decode_base36(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty() || text.size() > 13) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            digit = static_cast<std::uint64_t>(c - 'a') + 10;
        } else {
            return false;
        }
        value = value * 36 + digit;
    }
    out = value;
    return true;
}

}  // namespace

ClientOrderIdGenerator::ClientOrderIdGenerator(std::string_view prefix, std::uint64_t session_id,
                                               std::uint32_t max_length) noexcept
    : session_id_(session_id), max_length_(max_length) {
    static_cast<void>(prefix_.assign(prefix));
}

bool ClientOrderIdGenerator::next(ClientOrderId& out) noexcept {
    // Shape: <prefix>-<session base36>-<sequence base36>
    // The session component is what stops an order from a previous run being
    // mistaken for one of ours after a restart.
    std::array<char, 16> session_buf{};
    std::array<char, 16> sequence_buf{};
    const std::size_t session_len = encode_base36(session_id_, session_buf);
    const std::uint64_t sequence = ++sequence_;
    const std::size_t sequence_len = encode_base36(sequence, sequence_buf);

    const std::size_t total = prefix_.size() + 1 + session_len + 1 + sequence_len;
    if (total > max_length_ || total > ClientOrderId::kCapacity) {
        // Truncation would produce something that is not an identifier: it can
        // collide, and the order it names becomes unfindable exactly when
        // reconciliation needs it.
        ++failures_;
        return false;
    }

    ClientOrderId id;
    if (!id.assign(prefix_.view()) || !id.append("-") ||
        !id.append(std::string_view(session_buf.data(), session_len)) || !id.append("-") ||
        !id.append(std::string_view(sequence_buf.data(), sequence_len))) {
        ++failures_;
        return false;
    }
    out = id;
    ++generated_;
    return true;
}

bool ClientOrderIdGenerator::parse(const ClientOrderId& id, std::string_view expected_prefix,
                                   std::uint64_t& session_id, std::uint64_t& sequence) noexcept {
    const std::string_view text = id.view();
    if (text.size() <= expected_prefix.size() + 2) {
        return false;
    }
    if (text.substr(0, expected_prefix.size()) != expected_prefix) {
        return false;
    }
    if (text[expected_prefix.size()] != '-') {
        return false;
    }
    const std::string_view rest = text.substr(expected_prefix.size() + 1);
    const auto separator = rest.find('-');
    if (separator == std::string_view::npos) {
        return false;
    }
    return decode_base36(rest.substr(0, separator), session_id) &&
           decode_base36(rest.substr(separator + 1), sequence);
}

}  // namespace mm::oms
