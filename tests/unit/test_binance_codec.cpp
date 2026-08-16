/// \file test_binance_codec.cpp
/// Decoding tests against recorded Binance payloads.
///
/// Every payload here is a literal captured from the venue's documented format.
/// No network, no clock, no randomness — so a decoding regression fails
/// identically on every machine, which is not true of a test that talks to a
/// live exchange.

#include <gtest/gtest.h>

#include "mm/exchange/binance/BinanceCodec.hpp"

namespace mm::exchange::binance {
namespace {

class CodecTest : public ::testing::Test {
protected:
    std::vector<MarketDataEvent> out;
    BinanceCodec codec;

    [[nodiscard]] const BookUpdateEvent* first_update() const {
        for (const auto& e : out) {
            if (e.type == MarketDataEventType::BookUpdate) {
                return &e.payload.book_update;
            }
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------- symbols

TEST_F(CodecTest, SymbolMappingStaysInsideTheAdapter) {
    const Symbol btc("BTCUSDT");
    // Stream names are lower case, REST is upper case. Both spellings are venue
    // detail and neither reaches the core.
    EXPECT_EQ(BinanceCodec::to_stream_symbol(btc), "btcusdt");
    EXPECT_EQ(BinanceCodec::to_rest_symbol(btc), "BTCUSDT");
    EXPECT_EQ(BinanceCodec::from_venue_symbol("btcusdt"), btc);
    EXPECT_EQ(BinanceCodec::from_venue_symbol("BTCUSDT"), btc);
}

TEST_F(CodecTest, StreamNames) {
    const Symbol btc("BTCUSDT");
    EXPECT_EQ(BinanceCodec::depth_stream_name(btc, 100), "btcusdt@depth@100ms");
    EXPECT_EQ(BinanceCodec::depth_stream_name(btc, 1000), "btcusdt@depth");
    EXPECT_EQ(BinanceCodec::trade_stream_name(btc), "btcusdt@trade");
    EXPECT_EQ(BinanceCodec::book_ticker_stream_name(btc), "btcusdt@bookTicker");
}

// ------------------------------------------------------------ depth diffs

TEST_F(CodecTest, DecodesADepthUpdate) {
    const char* frame = R"({
        "e":"depthUpdate","E":1786924800123,"s":"BTCUSDT","U":157,"u":160,
        "b":[["63120.73000000","0.28760000"],["63120.72000000","0.00000000"]],
        "a":[["63120.74000000","8.03602000"]]
    })";
    ASSERT_TRUE(codec.decode_stream_frame(frame, 5'000, out).is_ok());
    ASSERT_EQ(out.size(), 1U);

    const BookUpdateEvent* u = first_update();
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->symbol, Symbol("BTCUSDT"));
    EXPECT_EQ(u->first_update_id, 157U);
    EXPECT_EQ(u->final_update_id, 160U);
    EXPECT_EQ(u->prev_final_update_id, kNoSeq) << "spot does not publish a previous-final id";
    EXPECT_TRUE(u->is_last);

    ASSERT_EQ(u->levels.bid_count, 2);
    EXPECT_EQ(u->levels.bids[0].price.to_string(), "63120.73");
    EXPECT_EQ(u->levels.bids[0].quantity.to_string(), "0.2876");
    EXPECT_TRUE(u->levels.bids[1].is_deletion()) << "zero quantity is a deletion";
    ASSERT_EQ(u->levels.ask_count, 1);
    EXPECT_EQ(u->levels.asks[0].quantity.to_string(), "8.03602");
}

TEST_F(CodecTest, PricesAreParsedExactlyNotThroughADouble) {
    const char* frame = R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":1,
        "b":[["0.10000000","0.20000000"]],"a":[]})";
    ASSERT_TRUE(codec.decode_stream_frame(frame, 0, out).is_ok());
    const BookUpdateEvent* u = first_update();
    ASSERT_NE(u, nullptr);
    // 0.1 has no exact binary representation; the fixed-point path keeps it exact.
    EXPECT_EQ(u->levels.bids[0].price.raw(), 10'000'000);
    EXPECT_EQ(u->levels.bids[0].quantity.raw(), 20'000'000);
}

TEST_F(CodecTest, HandlesTheCombinedStreamEnvelope) {
    const char* frame = R"({"stream":"btcusdt@depth@100ms","data":{
        "e":"depthUpdate","E":1,"s":"BTCUSDT","U":10,"u":12,
        "b":[["100.00","1.00"]],"a":[]}})";
    ASSERT_TRUE(codec.decode_stream_frame(frame, 0, out).is_ok());
    ASSERT_EQ(out.size(), 1U);
    const BookUpdateEvent* u = first_update();
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->final_update_id, 12U);
}

TEST_F(CodecTest, CarriesPreviousFinalIdWhenTheVenuePublishesIt) {
    const char* frame = R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":10,"u":12,"pu":9,
        "b":[["100.00","1.00"]],"a":[]})";
    ASSERT_TRUE(codec.decode_stream_frame(frame, 0, out).is_ok());
    const BookUpdateEvent* u = first_update();
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->prev_final_update_id, 9U);
}

TEST_F(CodecTest, OversizedDepthUpdateIsChunkedNotTruncated) {
    std::string frame = R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[)";
    for (int i = 0; i < 40; ++i) {
        if (i > 0) {
            frame += ',';
        }
        frame += "[\"" + std::to_string(1000 - i) + ".00\",\"1.00\"]";
    }
    frame += R"(],"a":[]})";

    ASSERT_TRUE(codec.decode_stream_frame(frame, 0, out).is_ok());
    // 40 levels against a 16-level event: three chunks, all 40 preserved.
    ASSERT_EQ(out.size(), 3U);
    std::size_t total = 0;
    std::size_t last_markers = 0;
    for (const auto& e : out) {
        ASSERT_EQ(e.type, MarketDataEventType::BookUpdate);
        total += e.payload.book_update.levels.bid_count;
        // Every chunk of one logical update reports the same id range.
        EXPECT_EQ(e.payload.book_update.first_update_id, 1U);
        EXPECT_EQ(e.payload.book_update.final_update_id, 2U);
        last_markers += e.payload.book_update.is_last ? 1 : 0;
    }
    EXPECT_EQ(total, 40U) << "chunking must not lose a level";
    EXPECT_EQ(last_markers, 1U);
}

// ----------------------------------------------------------------- trades

TEST_F(CodecTest, DecodesATradeAndResolvesTheAggressor) {
    // "m": true means the BUYER was the maker, so the seller lifted.
    const char* maker_buy = R"({"e":"trade","E":1786924800123,"s":"BTCUSDT","t":12345,
        "p":"63120.50000000","q":"0.00500000","T":1786924800100,"m":true})";
    ASSERT_TRUE(codec.decode_stream_frame(maker_buy, 7'000, out).is_ok());
    ASSERT_EQ(out.size(), 1U);
    const TradeEvent& t = out[0].payload.trade;
    EXPECT_EQ(t.symbol, Symbol("BTCUSDT"));
    EXPECT_EQ(t.price.to_string(), "63120.5");
    EXPECT_EQ(t.quantity.to_string(), "0.005");
    EXPECT_EQ(t.aggressor, Side::Sell) << "buyer-is-maker means the seller was the aggressor";
    EXPECT_EQ(t.trade_id, TradeId("12345"));

    out.clear();
    const char* maker_sell = R"({"e":"trade","E":1,"s":"BTCUSDT","t":2,
        "p":"100.00","q":"1.00","T":1,"m":false})";
    ASSERT_TRUE(codec.decode_stream_frame(maker_sell, 0, out).is_ok());
    EXPECT_EQ(out[0].payload.trade.aggressor, Side::Buy);
}

TEST_F(CodecTest, TradeCarriesExchangeAndReceiveTimestampsSeparately) {
    const char* frame = R"({"e":"trade","E":1786924800123,"s":"BTCUSDT","t":1,
        "p":"100.00","q":"1.00","T":1786924800100,"m":false})";
    const Nanos recv = 9'999;
    ASSERT_TRUE(codec.decode_stream_frame(frame, recv, out).is_ok());
    const EventStamps& st = out[0].stamps;
    // The venue's own time is preserved, not overwritten with ours.
    EXPECT_EQ(st.exchange_ns, 1'786'924'800'100LL * kNanosPerMilli / 1);
    EXPECT_EQ(st.recv_ns, recv);
    EXPECT_GT(st.parse_ns, 0);
}

// -------------------------------------------------------- malformed input

TEST_F(CodecTest, MalformedInputIsRejectedNotThrown) {
    struct Case {
        const char* frame;
        DecodeError expected;
    };
    const Case cases[] = {
        {"not json at all", DecodeError::NotJson},
        {"[1,2,3]", DecodeError::NotAnObject},
        {R"({"e":"depthUpdate","E":1,"U":1,"u":2,"b":[],"a":[]})", DecodeError::MissingField},
        {R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","u":2,"b":[],"a":[]})",
         DecodeError::InvalidSequence},
        {R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":200,"u":100,"b":[],"a":[]})",
         DecodeError::InvalidSequence},
        {R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[["abc","1.0"]],"a":[]})",
         DecodeError::LevelParseFailed},
        {R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[["100.0"]],"a":[]})",
         DecodeError::LevelParseFailed},
        {R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":"nope","a":[]})",
         DecodeError::MissingField},
        {R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"xyz","q":"1.0","m":true})",
         DecodeError::InvalidNumber},
        {R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"0","q":"1.0","m":true})",
         DecodeError::InvalidNumber},
        {R"({"e":"kline","E":1,"s":"BTCUSDT"})", DecodeError::UnknownMessageType},
        {R"({"foo":"bar"})", DecodeError::UnknownMessageType},
    };

    for (const auto& c : cases) {
        out.clear();
        const Status s = codec.decode_stream_frame(c.frame, 0, out);
        EXPECT_TRUE(s.is_error()) << c.frame;
        EXPECT_EQ(codec.last_error(), c.expected) << c.frame << " -> " << s.to_string();
        EXPECT_TRUE(out.empty()) << "a rejected frame must emit nothing: " << c.frame;
    }
}

TEST_F(CodecTest, ChunkedUpdateEmitsAllChunksOrNone) {
    // The malformed level sits in the third chunk; the first two must not
    // survive as a partial update.
    std::string frame = R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[)";
    for (int i = 0; i < 35; ++i) {
        if (i > 0) {
            frame += ',';
        }
        frame += (i == 34) ? R"(["bad","1.00"])"
                           : ("[\"" + std::to_string(1000 - i) + ".00\",\"1.00\"]");
    }
    frame += R"(],"a":[]})";

    EXPECT_TRUE(codec.decode_stream_frame(frame, 0, out).is_error());
    EXPECT_TRUE(out.empty()) << "a partially decoded update would leave a hole in the book";
}

TEST_F(CodecTest, SubscriptionAcknowledgementIsNotAnError) {
    // {"result":null,"id":1} is the expected reply to a SUBSCRIBE request.
    ASSERT_TRUE(codec.decode_stream_frame(R"({"result":null,"id":1})", 0, out).is_ok());
    EXPECT_TRUE(out.empty());
}

TEST_F(CodecTest, OversizedFrameIsRefusedWithoutParsing) {
    const std::string huge(kMaxFrameBytes + 1, 'x');
    EXPECT_TRUE(codec.decode_stream_frame(huge, 0, out).is_error());
    EXPECT_EQ(codec.last_error(), DecodeError::TooLarge);
}

// ---------------------------------------------------------- REST snapshot

TEST_F(CodecTest, DecodesADepthSnapshot) {
    const char* body = R"({"lastUpdateId":98578496686,
        "bids":[["63120.73000000","0.28760000"],["63120.72000000","0.00115000"]],
        "asks":[["63120.74000000","8.03602000"],["63120.75000000","0.00034000"]]})";
    ASSERT_TRUE(codec.decode_depth_snapshot(body, Symbol("BTCUSDT"), 100, out).is_ok());
    ASSERT_EQ(out.size(), 1U);

    const BookSnapshotEvent& s = out[0].payload.book_snapshot;
    EXPECT_EQ(s.symbol, Symbol("BTCUSDT"));
    EXPECT_EQ(s.last_update_id, 98'578'496'686ULL);
    EXPECT_TRUE(s.is_first);
    EXPECT_TRUE(s.is_last);
    EXPECT_EQ(s.levels.bid_count, 2);
    EXPECT_EQ(s.levels.bids[0].price.to_string(), "63120.73");
    EXPECT_EQ(s.levels.asks[0].quantity.to_string(), "8.03602");
}

TEST_F(CodecTest, LargeSnapshotIsChunkedWithMarkers) {
    std::string body = R"({"lastUpdateId":500,"bids":[)";
    for (int i = 0; i < 50; ++i) {
        if (i > 0) {
            body += ',';
        }
        body += "[\"" + std::to_string(1000 - i) + ".00\",\"1.00\"]";
    }
    body += R"(],"asks":[]})";

    ASSERT_TRUE(codec.decode_depth_snapshot(body, Symbol("BTCUSDT"), 0, out).is_ok());
    ASSERT_EQ(out.size(), 4U);  // ceil(50/16)
    std::size_t total = 0, firsts = 0, lasts = 0;
    for (const auto& e : out) {
        const BookSnapshotEvent& s = e.payload.book_snapshot;
        total += s.levels.bid_count;
        firsts += s.is_first ? 1 : 0;
        lasts += s.is_last ? 1 : 0;
        EXPECT_EQ(s.last_update_id, 500U) << "every chunk carries the same image id";
    }
    EXPECT_EQ(total, 50U);
    EXPECT_EQ(firsts, 1U);
    EXPECT_EQ(lasts, 1U);
}

TEST_F(CodecTest, SnapshotErrorsAreClassified) {
    EXPECT_TRUE(codec.decode_depth_snapshot("{", Symbol("BTCUSDT"), 0, out).is_error());
    EXPECT_EQ(codec.last_error(), DecodeError::NotJson);

    EXPECT_TRUE(codec.decode_depth_snapshot(R"({"bids":[],"asks":[]})", Symbol("BTCUSDT"), 0, out)
                    .is_error());
    EXPECT_EQ(codec.last_error(), DecodeError::InvalidSequence);

    // A venue-level refusal returned with a 200 body.
    EXPECT_TRUE(codec.decode_depth_snapshot(R"({"code":-1121,"msg":"Invalid symbol."})",
                                            Symbol("NOSUCH"), 0, out)
                    .is_error());
    EXPECT_EQ(codec.last_error(), DecodeError::MissingField);
}

// --------------------------------------------------------- exchange info

TEST_F(CodecTest, DecodesInstrumentFilters) {
    const char* body = R"({"symbols":[{
        "symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT",
        "filters":[
            {"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
            {"filterType":"LOT_SIZE","stepSize":"0.00001000","minQty":"0.00001000",
             "maxQty":"9000.00000000"},
            {"filterType":"NOTIONAL","minNotional":"5.00000000"}]},
        {"symbol":"ETHUSDT","status":"TRADING","baseAsset":"ETH","quoteAsset":"USDT",
         "filters":[{"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
                    {"filterType":"LOT_SIZE","stepSize":"0.00010000"}]}]})";

    ASSERT_TRUE(codec.decode_exchange_info(body, {Symbol("BTCUSDT")}, 0, out).is_ok());
    ASSERT_EQ(out.size(), 1U) << "only requested symbols are emitted";

    const InstrumentSpec& spec = out[0].payload.instrument.spec;
    EXPECT_EQ(spec.symbol, Symbol("BTCUSDT"));
    EXPECT_EQ(spec.base, Asset("BTC"));
    EXPECT_EQ(spec.tick_size.to_string(), "0.01");
    EXPECT_EQ(spec.lot_size.to_string(), "0.00001");
    EXPECT_EQ(spec.min_qty.to_string(), "0.00001");
    EXPECT_EQ(spec.max_qty.to_string(), "9000");
    EXPECT_EQ(spec.min_notional.to_string(), "5");
    EXPECT_EQ(spec.status, MarketStatus::Trading);
    EXPECT_TRUE(spec.is_valid());
}

TEST_F(CodecTest, InstrumentWithoutTickOrLotIsNotPublished) {
    // A spec that cannot validate an order would silently permit anything.
    const char* body = R"({"symbols":[{"symbol":"BTCUSDT","status":"TRADING","filters":[]}]})";
    ASSERT_TRUE(codec.decode_exchange_info(body, {}, 0, out).is_ok());
    EXPECT_TRUE(out.empty());
}

TEST_F(CodecTest, EveryDecodeErrorHasAName) {
    for (std::uint8_t i = 1; i <= static_cast<std::uint8_t>(DecodeError::LevelParseFailed); ++i) {
        EXPECT_NE(to_string(static_cast<DecodeError>(i)), "UNKNOWN") << int{i};
    }
}

}  // namespace
}  // namespace mm::exchange::binance
