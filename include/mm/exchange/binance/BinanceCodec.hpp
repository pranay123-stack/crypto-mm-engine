#pragma once

/// \file BinanceCodec.hpp
/// Binance Spot wire format -> normalized events.
///
/// This is the *only* place in the repository that knows what Binance calls
/// things. It is a pure function of its input: no sockets, no clock, no state
/// beyond a reusable output buffer. That is deliberate — it means every parsing
/// and malformed-input test runs offline against recorded payloads and is
/// reproducible, rather than depending on what the venue happens to send today.
///
/// Nothing here throws. `nlohmann::json` is used in its non-throwing mode and
/// every field access is checked, because a market-data process that dies on an
/// unexpected payload is a market-data process that dies during exactly the
/// venue incident you needed it for.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"

namespace mm::exchange::binance {

/// Largest frame we will attempt to decode. A depth snapshot at limit=5000 is
/// roughly 300 KB; anything an order of magnitude beyond that is not a message
/// we understand, and attempting to parse it wastes an I/O thread.
inline constexpr std::size_t kMaxFrameBytes = 4u * 1024u * 1024u;

/// Why a frame could not be decoded. Distinguished so repeated corruption of
/// one kind is visible in diagnostics rather than averaged into "parse error".
enum class DecodeError : std::uint8_t {
    None = 0,
    NotJson,
    NotAnObject,
    UnknownMessageType,
    MissingField,
    InvalidNumber,      ///< a price/quantity string that is not an exact decimal
    InvalidSequence,
    UnknownSymbol,      ///< a symbol we never subscribed to
    TooLarge,
    LevelParseFailed,
};
[[nodiscard]] std::string_view to_string(DecodeError e) noexcept;

class BinanceCodec {
public:
    /// Decodes one WebSocket text frame, appending normalized events to `out`.
    ///
    /// Handles both the bare payload and the combined-stream envelope
    /// (`{"stream":..., "data":{...}}`). Depth diffs wider than
    /// `kMaxLevelsPerEvent` are emitted as an ordered chunk run, never truncated.
    ///
    /// Returns an error for anything it cannot decode; `last_error()` carries
    /// the classification.
    [[nodiscard]] Status decode_stream_frame(std::string_view frame, Nanos recv_ns,
                                             std::vector<MarketDataEvent>& out);

    /// Decodes a REST depth snapshot. The symbol is supplied by the caller
    /// because the REST payload does not carry one.
    [[nodiscard]] Status decode_depth_snapshot(std::string_view body, const Symbol& symbol,
                                               Nanos recv_ns,
                                               std::vector<MarketDataEvent>& out);

    /// Decodes `exchangeInfo` into `InstrumentUpdateEvent`s. Only symbols in
    /// `wanted` are emitted; the full payload lists thousands.
    [[nodiscard]] Status decode_exchange_info(std::string_view body,
                                              const std::vector<Symbol>& wanted, Nanos recv_ns,
                                              std::vector<MarketDataEvent>& out);

    [[nodiscard]] DecodeError last_error() const noexcept { return last_error_; }

    // -------------------------------------------------------- symbol mapping
    //
    // Binance uses upper case in REST and payloads but lower case in stream
    // names. Both spellings stay inside this adapter; the core only ever sees
    // the normalized `Symbol`.

    [[nodiscard]] static std::string to_stream_symbol(const Symbol& symbol);
    [[nodiscard]] static std::string to_rest_symbol(const Symbol& symbol);
    [[nodiscard]] static Symbol from_venue_symbol(std::string_view venue_symbol);

    /// e.g. "btcusdt@depth@100ms"
    [[nodiscard]] static std::string depth_stream_name(const Symbol& symbol,
                                                       std::int32_t update_interval_ms);
    /// e.g. "btcusdt@trade"
    [[nodiscard]] static std::string trade_stream_name(const Symbol& symbol);
    /// e.g. "btcusdt@bookTicker"
    [[nodiscard]] static std::string book_ticker_stream_name(const Symbol& symbol);

private:
    [[nodiscard]] Status fail(DecodeError error, std::string_view detail);

    DecodeError last_error_ = DecodeError::None;
};

}  // namespace mm::exchange::binance
