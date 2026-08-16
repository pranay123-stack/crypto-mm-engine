#pragma once

/// \file ExchangeCapabilities.hpp
/// What a given venue can actually do.
///
/// Venues differ, and the core must not be written against the union of every
/// venue's features nor the intersection. Instead, a venue declares its
/// capabilities and the layers above adapt: the quote manager degrades an
/// atomic replace into cancel-then-submit where replace is unsupported, and an
/// order carrying an unsupported flag is refused locally rather than sent and
/// rejected -- a round trip and a rate-limit slot saved, and a reject burst
/// avoided in the safety layer.
///
/// This stays generic on purpose. A capability earns its place only if some
/// layer above genuinely branches on it; a flag nobody reads is documentation
/// pretending to be code.

#include <cstdint>
#include <string>

#include "mm/common/Types.hpp"

namespace mm::exchange {

struct ExchangeCapabilities {
    // ---- order features ----
    /// Post-only / maker-only orders, rejected rather than crossing.
    bool supports_post_only = false;
    /// Atomic cancel-replace. When false the quote manager must decompose it,
    /// and exposure is the union of both orders during the window.
    bool supports_replace = false;
    /// Reduce-only orders (derivatives venues).
    bool supports_reduce_only = false;
    /// Client-assigned order IDs echoed back on every report. When false the
    /// OMS must join on the venue's ID alone, which materially complicates
    /// recovery after a restart.
    bool supports_client_order_id = true;
    /// Cancel-all, per symbol or account.
    bool supports_mass_cancel = false;
    /// Querying a single order by ID.
    bool supports_order_query = true;

    // ---- market data features ----
    /// A dedicated best-bid/offer stream, rather than deriving BBO from depth.
    bool supports_native_bbo = false;
    /// A REST or stream snapshot to seed the book.
    bool supports_orderbook_snapshot = true;
    /// Incremental depth diffs with sequence numbers.
    bool supports_incremental_depth = true;
    /// A public trade stream.
    bool supports_trade_stream = true;

    // ---- account features ----
    bool supports_balance_stream = false;
    bool supports_position_stream = false;
    /// Spot venues have balances but no positions in the derivatives sense.
    bool has_positions = false;

    // ---- limits ----
    /// Longest client order ID the venue accepts. The OMS generates IDs to fit
    /// the smallest configured venue; a truncated ID is a lost order.
    std::uint32_t max_client_order_id_len = 36;
    /// Maximum symbols on one market-data connection; 0 means unlimited.
    std::uint32_t max_subscriptions_per_connection = 0;
    /// Deepest snapshot the venue will return; 0 means unspecified.
    std::uint32_t max_snapshot_depth = 0;

    [[nodiscard]] std::string to_string() const;
};

/// Why a request cannot be expressed on this venue. Returned by
/// `unsupported_feature` so the caller can report precisely which flag failed
/// rather than a bare "unsupported".
enum class Feature : std::uint8_t {
    None = 0,
    PostOnly,
    Replace,
    ReduceOnly,
    ClientOrderId,
    MassCancel,
    OrderQuery,
    NativeBbo,
    OrderbookSnapshot,
    IncrementalDepth,
    TradeStream,
    BalanceStream,
    PositionStream,
};
[[nodiscard]] std::string_view to_string(Feature f) noexcept;

[[nodiscard]] bool supports(const ExchangeCapabilities& caps, Feature f) noexcept;

}  // namespace mm::exchange
