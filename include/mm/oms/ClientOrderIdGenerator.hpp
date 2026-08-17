#pragma once

/// \file ClientOrderIdGenerator.hpp
/// Deterministic client order ids.
///
/// The id is the join key between our records and the venue's, and the only
/// thing that lets a restarted process recognise its own orders. It must
/// therefore be unique across restarts, deterministic enough to reconstruct,
/// and bounded to the venue's length limit.
///
/// **Truncation is failure, not a fallback.** A truncated identifier is not an
/// identifier: it may collide with another order, and the order it names
/// becomes unfindable at exactly the moment reconciliation needs it. Generation
/// returns false rather than producing a shortened id.

#include <cstdint>

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"

namespace mm::oms {

class ClientOrderIdGenerator {
public:
    /// `prefix` identifies the engine; `session` distinguishes one run from the
    /// next, so an order from a previous life can never be mistaken for a
    /// current one. Both are caller-supplied rather than derived from the wall
    /// clock, which would make ids non-reproducible.
    ClientOrderIdGenerator(std::string_view prefix, std::uint64_t session_id,
                           std::uint32_t max_length) noexcept;

    /// Produces the next id. Returns false when it would exceed the venue's
    /// limit -- never a truncated one.
    [[nodiscard]] bool next(ClientOrderId& out) noexcept;

    /// Restores the counter after a restart, so a recovered session cannot
    /// reissue an id its previous life already used.
    void restore(std::uint64_t next_sequence) noexcept { sequence_ = next_sequence; }

    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }
    [[nodiscard]] std::uint64_t generated() const noexcept { return generated_; }
    [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

    /// Recovers the session and sequence from an id this generator produced.
    /// Used at startup to tell our own orphans from somebody else's.
    [[nodiscard]] static bool parse(const ClientOrderId& id, std::string_view expected_prefix,
                                    std::uint64_t& session_id,
                                    std::uint64_t& sequence) noexcept;

private:
    InlineString<16> prefix_{};
    std::uint64_t session_id_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint32_t max_length_ = 36;
    std::uint64_t generated_ = 0;
    std::uint64_t failures_ = 0;
};

}  // namespace mm::oms
