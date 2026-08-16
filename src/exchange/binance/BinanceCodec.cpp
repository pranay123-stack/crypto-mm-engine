#include "mm/exchange/binance/BinanceCodec.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace mm::exchange::binance {
namespace {

using Json = nlohmann::json;

/// Binance sends every price and quantity as a decimal string. Parsing it
/// exactly is the whole reason the wire format uses strings; going through a
/// double here would reintroduce the rounding the fixed-point types exist to
/// eliminate.
template <class Tag>
[[nodiscard]] bool parse_decimal(const Json& node, Fixed<Tag>& out) {
    if (!node.is_string()) {
        return false;
    }
    return Fixed<Tag>::parse(node.get_ref<const std::string&>(), out);
}

[[nodiscard]] bool read_seq(const Json& node, Seq& out) {
    if (node.is_number_unsigned()) {
        out = node.get<std::uint64_t>();
        return true;
    }
    if (node.is_number_integer()) {
        const auto v = node.get<std::int64_t>();
        if (v < 0) {
            return false;
        }
        out = static_cast<Seq>(v);
        return true;
    }
    return false;
}

[[nodiscard]] bool read_millis_as_nanos(const Json& node, Nanos& out) {
    if (!node.is_number_integer() && !node.is_number_unsigned()) {
        return false;
    }
    const auto ms = node.get<std::int64_t>();
    if (ms < 0) {
        return false;
    }
    out = ms * kNanosPerMilli;
    return true;
}

/// Reads one `["price","qty"]` pair.
[[nodiscard]] bool read_level(const Json& node, PriceLevel& out) {
    if (!node.is_array() || node.size() < 2) {
        return false;
    }
    if (!parse_decimal(node[0], out.price)) {
        return false;
    }
    if (!parse_decimal(node[1], out.quantity)) {
        return false;
    }
    // A zero quantity is a deletion and is legal; a negative one is not, and a
    // non-positive price never is.
    return out.price.is_positive() && !out.quantity.is_negative();
}

/// Copies levels into a chunk run, so an oversized depth message becomes
/// several events rather than a truncated one.
struct LevelCursor {
    const Json* array = nullptr;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t remaining() const {
        return (array == nullptr || !array->is_array()) ? 0 : array->size() - offset;
    }
};

[[nodiscard]] bool fill_chunk(LevelCursor& cursor, std::array<PriceLevel, kMaxLevelsPerEvent>& dst,
                              std::uint8_t& count) {
    std::size_t n = 0;
    while (cursor.remaining() > 0 && n < kMaxLevelsPerEvent) {
        if (!read_level((*cursor.array)[cursor.offset], dst[n])) {
            return false;
        }
        ++cursor.offset;
        ++n;
    }
    count = static_cast<std::uint8_t>(n);
    return true;
}

void stamp(MarketDataEvent& event, Nanos exchange_ns, Nanos recv_ns) {
    event.stamps.exchange_ns = exchange_ns;
    event.stamps.recv_ns = recv_ns;
    // Decode has just finished; the trading thread fills process_ns when it
    // dequeues. All three internal stamps are steady-clock readings.
    event.stamps.parse_ns = steady_ns();
}

}  // namespace

std::string_view to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None:               return "NONE";
        case DecodeError::NotJson:            return "NOT_JSON";
        case DecodeError::NotAnObject:        return "NOT_AN_OBJECT";
        case DecodeError::UnknownMessageType: return "UNKNOWN_MESSAGE_TYPE";
        case DecodeError::MissingField:       return "MISSING_FIELD";
        case DecodeError::InvalidNumber:      return "INVALID_NUMBER";
        case DecodeError::InvalidSequence:    return "INVALID_SEQUENCE";
        case DecodeError::UnknownSymbol:      return "UNKNOWN_SYMBOL";
        case DecodeError::TooLarge:           return "TOO_LARGE";
        case DecodeError::LevelParseFailed:   return "LEVEL_PARSE_FAILED";
    }
    return "UNKNOWN";
}

Status BinanceCodec::fail(DecodeError error, std::string_view detail) {
    last_error_ = error;
    std::string msg(to_string(error));
    if (!detail.empty()) {
        msg.append(": ").append(detail);
    }
    return {ErrorCode::ParseError, msg};
}

// ------------------------------------------------------------ symbol mapping

std::string BinanceCodec::to_stream_symbol(const Symbol& symbol) {
    std::string out(symbol.view());
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string BinanceCodec::to_rest_symbol(const Symbol& symbol) {
    std::string out(symbol.view());
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

Symbol BinanceCodec::from_venue_symbol(std::string_view venue_symbol) {
    std::string upper(venue_symbol);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    Symbol out;
    static_cast<void>(out.assign(upper));
    return out;
}

std::string BinanceCodec::depth_stream_name(const Symbol& symbol,
                                            std::int32_t update_interval_ms) {
    std::string name = to_stream_symbol(symbol);
    name.append("@depth");
    // Binance offers 1000ms (the default, expressed by omitting the suffix) and
    // 100ms. A market maker wants the faster one.
    if (update_interval_ms == 100) {
        name.append("@100ms");
    }
    return name;
}

std::string BinanceCodec::trade_stream_name(const Symbol& symbol) {
    return to_stream_symbol(symbol) + "@trade";
}

std::string BinanceCodec::book_ticker_stream_name(const Symbol& symbol) {
    return to_stream_symbol(symbol) + "@bookTicker";
}

// ---------------------------------------------------------------- WS frames

Status BinanceCodec::decode_stream_frame(std::string_view frame, Nanos recv_ns,
                                         std::vector<MarketDataEvent>& out) {
    last_error_ = DecodeError::None;

    if (frame.size() > kMaxFrameBytes) {
        return fail(DecodeError::TooLarge, "frame exceeds the decode limit");
    }
    // Non-throwing parse: a malformed frame must be a value we inspect, not an
    // exception crossing the I/O handler.
    const Json root = Json::parse(frame, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return fail(DecodeError::NotJson, "frame is not valid JSON");
    }
    if (!root.is_object()) {
        return fail(DecodeError::NotAnObject, "frame is not a JSON object");
    }

    // Combined-stream envelope, or a bare payload on a single-stream socket.
    const Json& payload = (root.contains("data") && root["data"].is_object()) ? root["data"] : root;

    if (!payload.contains("e") || !payload["e"].is_string()) {
        // Subscription acknowledgements look like {"result":null,"id":1} and are
        // not market data; they are expected, not corrupt.
        if (root.contains("result") || root.contains("id")) {
            return Status::ok();
        }
        return fail(DecodeError::UnknownMessageType, "payload has no event type");
    }

    const std::string& event_type = payload["e"].get_ref<const std::string&>();

    if (event_type == "depthUpdate") {
        if (!payload.contains("s") || !payload["s"].is_string()) {
            return fail(DecodeError::MissingField, "depthUpdate has no symbol");
        }
        const Symbol symbol = from_venue_symbol(payload["s"].get_ref<const std::string&>());
        if (symbol.empty()) {
            return fail(DecodeError::UnknownSymbol, "depthUpdate symbol is empty");
        }

        Seq first_id = kNoSeq;
        Seq final_id = kNoSeq;
        if (!payload.contains("U") || !read_seq(payload["U"], first_id)) {
            return fail(DecodeError::InvalidSequence, "depthUpdate has no valid first update id");
        }
        if (!payload.contains("u") || !read_seq(payload["u"], final_id)) {
            return fail(DecodeError::InvalidSequence, "depthUpdate has no valid final update id");
        }
        if (first_id > final_id) {
            return fail(DecodeError::InvalidSequence, "depthUpdate id range is inverted");
        }

        // Spot does not publish a previous-final id; futures does (`pu`). Carry
        // it when present so the synchronizer can use the stronger check.
        Seq prev_final = kNoSeq;
        if (payload.contains("pu")) {
            static_cast<void>(read_seq(payload["pu"], prev_final));
        }

        Nanos exchange_ns = 0;
        if (payload.contains("E")) {
            static_cast<void>(read_millis_as_nanos(payload["E"], exchange_ns));
        }

        LevelCursor bids{payload.contains("b") ? &payload["b"] : nullptr, 0};
        LevelCursor asks{payload.contains("a") ? &payload["a"] : nullptr, 0};
        if ((bids.array != nullptr && !bids.array->is_array()) ||
            (asks.array != nullptr && !asks.array->is_array())) {
            return fail(DecodeError::MissingField, "depthUpdate levels are not arrays");
        }

        const std::size_t before = out.size();
        do {
            MarketDataEvent event;
            event.type = MarketDataEventType::BookUpdate;
            BookUpdateEvent& u = event.payload.book_update;
            u.symbol = symbol;
            u.first_update_id = first_id;
            u.final_update_id = final_id;
            u.prev_final_update_id = prev_final;
            if (!fill_chunk(bids, u.levels.bids, u.levels.bid_count) ||
                !fill_chunk(asks, u.levels.asks, u.levels.ask_count)) {
                out.resize(before);  // emit all chunks or none
                return fail(DecodeError::LevelParseFailed, "depthUpdate level is malformed");
            }
            u.is_last = (bids.remaining() == 0 && asks.remaining() == 0);
            stamp(event, exchange_ns, recv_ns);
            out.push_back(event);
        } while (bids.remaining() > 0 || asks.remaining() > 0);
        return Status::ok();
    }

    if (event_type == "trade") {
        if (!payload.contains("s") || !payload["s"].is_string()) {
            return fail(DecodeError::MissingField, "trade has no symbol");
        }
        MarketDataEvent event;
        event.type = MarketDataEventType::Trade;
        TradeEvent& t = event.payload.trade;
        t.symbol = from_venue_symbol(payload["s"].get_ref<const std::string&>());

        if (!payload.contains("p") || !parse_decimal(payload["p"], t.price)) {
            return fail(DecodeError::InvalidNumber, "trade price is not an exact decimal");
        }
        if (!payload.contains("q") || !parse_decimal(payload["q"], t.quantity)) {
            return fail(DecodeError::InvalidNumber, "trade quantity is not an exact decimal");
        }
        if (!t.price.is_positive() || !t.quantity.is_positive()) {
            return fail(DecodeError::InvalidNumber, "trade price/quantity must be positive");
        }

        // `m` is "was the BUYER the maker?". If so the seller lifted, so the
        // aggressor is the sell side. Getting this backwards inverts every
        // order-flow signal built on it.
        bool buyer_is_maker = false;
        if (payload.contains("m") && payload["m"].is_boolean()) {
            buyer_is_maker = payload["m"].get<bool>();
        }
        t.aggressor = buyer_is_maker ? Side::Sell : Side::Buy;

        Seq trade_seq = kNoSeq;
        if (payload.contains("t") && read_seq(payload["t"], trade_seq)) {
            t.seq = trade_seq;
            static_cast<void>(t.trade_id.assign(std::to_string(trade_seq)));
        }

        Nanos exchange_ns = 0;
        if (payload.contains("T")) {
            static_cast<void>(read_millis_as_nanos(payload["T"], exchange_ns));
        } else if (payload.contains("E")) {
            static_cast<void>(read_millis_as_nanos(payload["E"], exchange_ns));
        }
        stamp(event, exchange_ns, recv_ns);
        out.push_back(event);
        return Status::ok();
    }

    // A stream we did not ask for, or a type this adapter does not model.
    return fail(DecodeError::UnknownMessageType, event_type);
}

// -------------------------------------------------------------- REST depth

Status BinanceCodec::decode_depth_snapshot(std::string_view body, const Symbol& symbol,
                                           Nanos recv_ns, std::vector<MarketDataEvent>& out) {
    last_error_ = DecodeError::None;

    if (body.size() > kMaxFrameBytes) {
        return fail(DecodeError::TooLarge, "snapshot exceeds the decode limit");
    }
    const Json root = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return fail(DecodeError::NotJson, "snapshot is not valid JSON");
    }
    if (!root.is_object()) {
        return fail(DecodeError::NotAnObject, "snapshot is not a JSON object");
    }
    // The venue reports errors as {"code":-1121,"msg":"Invalid symbol."} with a
    // 200 in some paths, so a missing lastUpdateId is not necessarily a parse
    // bug on our side.
    if (root.contains("code") && root.contains("msg")) {
        return fail(DecodeError::MissingField, "snapshot request was refused by the venue");
    }

    Seq last_update_id = kNoSeq;
    if (!root.contains("lastUpdateId") || !read_seq(root["lastUpdateId"], last_update_id)) {
        return fail(DecodeError::InvalidSequence, "snapshot has no valid lastUpdateId");
    }

    LevelCursor bids{root.contains("bids") ? &root["bids"] : nullptr, 0};
    LevelCursor asks{root.contains("asks") ? &root["asks"] : nullptr, 0};
    if (bids.array == nullptr || !bids.array->is_array() || asks.array == nullptr ||
        !asks.array->is_array()) {
        return fail(DecodeError::MissingField, "snapshot has no bid/ask arrays");
    }

    const std::size_t before = out.size();
    bool first = true;
    do {
        MarketDataEvent event;
        event.type = MarketDataEventType::BookSnapshot;
        BookSnapshotEvent& s = event.payload.book_snapshot;
        s.symbol = symbol;
        s.last_update_id = last_update_id;
        if (!fill_chunk(bids, s.levels.bids, s.levels.bid_count) ||
            !fill_chunk(asks, s.levels.asks, s.levels.ask_count)) {
            out.resize(before);
            return fail(DecodeError::LevelParseFailed, "snapshot level is malformed");
        }
        s.is_first = first;
        s.is_last = (bids.remaining() == 0 && asks.remaining() == 0);
        first = false;
        stamp(event, 0, recv_ns);
        out.push_back(event);
    } while (bids.remaining() > 0 || asks.remaining() > 0);

    return Status::ok();
}

// ----------------------------------------------------------- exchange info

Status BinanceCodec::decode_exchange_info(std::string_view body,
                                          const std::vector<Symbol>& wanted, Nanos recv_ns,
                                          std::vector<MarketDataEvent>& out) {
    last_error_ = DecodeError::None;

    if (body.size() > kMaxFrameBytes) {
        return fail(DecodeError::TooLarge, "exchangeInfo exceeds the decode limit");
    }
    const Json root = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        return fail(DecodeError::NotJson, "exchangeInfo is not a JSON object");
    }
    if (!root.contains("symbols") || !root["symbols"].is_array()) {
        return fail(DecodeError::MissingField, "exchangeInfo has no symbols array");
    }

    for (const Json& entry : root["symbols"]) {
        if (!entry.is_object() || !entry.contains("symbol") || !entry["symbol"].is_string()) {
            continue;
        }
        const Symbol symbol = from_venue_symbol(entry["symbol"].get_ref<const std::string&>());
        if (!wanted.empty() &&
            std::find(wanted.begin(), wanted.end(), symbol) == wanted.end()) {
            continue;  // the full payload lists thousands of instruments
        }

        InstrumentSpec spec;
        spec.symbol = symbol;
        if (entry.contains("baseAsset") && entry["baseAsset"].is_string()) {
            static_cast<void>(spec.base.assign(entry["baseAsset"].get_ref<const std::string&>()));
        }
        if (entry.contains("quoteAsset") && entry["quoteAsset"].is_string()) {
            static_cast<void>(spec.quote.assign(entry["quoteAsset"].get_ref<const std::string&>()));
        }
        if (entry.contains("status") && entry["status"].is_string()) {
            spec.status = (entry["status"].get_ref<const std::string&>() == "TRADING")
                              ? MarketStatus::Trading
                              : MarketStatus::Halted;
        }

        if (entry.contains("filters") && entry["filters"].is_array()) {
            for (const Json& filter : entry["filters"]) {
                if (!filter.is_object() || !filter.contains("filterType") ||
                    !filter["filterType"].is_string()) {
                    continue;
                }
                const std::string& type = filter["filterType"].get_ref<const std::string&>();
                if (type == "PRICE_FILTER" && filter.contains("tickSize")) {
                    static_cast<void>(parse_decimal(filter["tickSize"], spec.tick_size));
                } else if (type == "LOT_SIZE") {
                    if (filter.contains("stepSize")) {
                        static_cast<void>(parse_decimal(filter["stepSize"], spec.lot_size));
                    }
                    if (filter.contains("minQty")) {
                        static_cast<void>(parse_decimal(filter["minQty"], spec.min_qty));
                    }
                    if (filter.contains("maxQty")) {
                        static_cast<void>(parse_decimal(filter["maxQty"], spec.max_qty));
                    }
                } else if ((type == "NOTIONAL" || type == "MIN_NOTIONAL") &&
                           filter.contains("minNotional")) {
                    static_cast<void>(parse_decimal(filter["minNotional"], spec.min_notional));
                }
            }
        }

        // An instrument without a tick or lot size cannot validate an order, so
        // publishing it would create a spec that silently permits anything.
        if (!spec.is_valid()) {
            continue;
        }
        MarketDataEvent event;
        event.type = MarketDataEventType::InstrumentUpdate;
        event.payload.instrument.spec = spec;
        stamp(event, 0, recv_ns);
        out.push_back(event);
    }
    return Status::ok();
}

}  // namespace mm::exchange::binance
