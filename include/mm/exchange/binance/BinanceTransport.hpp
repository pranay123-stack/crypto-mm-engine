#pragma once

/// \file BinanceTransport.hpp
/// TLS WebSocket and REST transport for Binance public market data.
///
/// **Threading.** Both classes run entirely on the `io_context` they are
/// constructed with, driven by the adapter's `md-io` thread. Handlers therefore
/// never race each other and need no locks. Nothing here touches the trading
/// thread; normalized events reach it through the sink and an `SpscRing`
/// (docs/concurrency.md §1).
///
/// **No authentication.** Public market data only: no signing, no API key, no
/// account or order endpoint. Those belong to Phase 12.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mm/common/Status.hpp"
#include "mm/common/Time.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/ReconnectBackoff.hpp"

namespace mm::exchange::binance {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;

/// Creates a TLS context with the host's trust store loaded and peer
/// verification switched on. Disabling verification would make the encryption
/// decorative, since anything on the path could then impersonate the venue.
[[nodiscard]] Result<std::shared_ptr<ssl::context>> make_tls_context();

/// One-shot unauthenticated GET with a hard deadline.
///
/// Snapshot fetches are infrequent — startup and resync — so the request is
/// composed as a fresh connection each time rather than pooled. That costs a
/// handshake and buys the guarantee that a stale or half-closed socket can
/// never silently serve a stale snapshot.
class BinanceRestClient {
public:
    using Handler = std::function<void(Result<std::string>)>;

    BinanceRestClient(net::io_context& io, ssl::context& tls, std::string host, std::string port);

    /// `target` is a path with query, e.g. "/api/v3/depth?symbol=BTCUSDT&limit=1000".
    /// The handler runs on the io_context and is invoked exactly once.
    void async_get(std::string target, Nanos timeout, Handler handler);

    [[nodiscard]] std::uint64_t requests_issued() const noexcept { return requests_issued_; }
    [[nodiscard]] std::uint64_t requests_failed() const noexcept { return requests_failed_; }

private:
    net::io_context& io_;
    ssl::context& tls_;
    std::string host_;
    std::string port_;
    std::uint64_t requests_issued_ = 0;
    std::uint64_t requests_failed_ = 0;
};

/// A TLS WebSocket session that reconnects on its own.
///
/// Owns connect, read, ping/pong, disconnect detection and backoff. It reports
/// transport state and hands raw frames upward; it does not parse them, because
/// decoding belongs to the codec and mixing the two would make neither testable.
class BinanceWsSession : public std::enable_shared_from_this<BinanceWsSession> {
public:
    /// Raw frame plus the steady-clock instant it was read. Stamping here, at
    /// the socket, is what makes ingress latency measurable rather than
    /// estimated later.
    using MessageHandler = std::function<void(std::string_view frame, Nanos recv_ns)>;
    using StateHandler = std::function<void(ConnectionState state, std::string_view reason)>;

    struct Config {
        std::string host = "stream.binance.com";
        std::string port = "9443";
        /// Combined-stream path, e.g. "/stream?streams=btcusdt@depth@100ms/btcusdt@trade".
        std::string target = "/stream";
        /// A socket that is open but silent is dead for our purposes. Binance
        /// pings every ~3 minutes; anything much past that means no data.
        Nanos idle_timeout = seconds(30);
        Nanos handshake_timeout = seconds(10);
        ReconnectBackoff::Config backoff{};
        std::uint64_t backoff_seed = 0;
    };

    BinanceWsSession(net::io_context& io, ssl::context& tls, Config config);

    void set_message_handler(MessageHandler handler) { on_message_ = std::move(handler); }
    void set_state_handler(StateHandler handler) { on_state_ = std::move(handler); }

    /// Begins connecting. Returns immediately; progress arrives through the
    /// state handler.
    void start();
    /// Stops reconnecting and closes. Idempotent.
    void stop();

    /// Sends a text frame on the live socket. Used for runtime SUBSCRIBE, which
    /// adds a stream without reconnecting -- a reconnect would resynchronize
    /// every other symbol on the socket for the sake of one new one.
    /// Returns false when not connected; the caller falls back to including the
    /// stream in the next connect.
    [[nodiscard]] bool send_text(const std::string& payload);

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t reconnect_count() const noexcept { return reconnects_; }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_; }
    /// True once the backoff budget is spent; the caller declares the session
    /// unrecoverable rather than retrying forever.
    [[nodiscard]] bool backoff_exhausted() const noexcept { return backoff_.exhausted(); }

private:
    using Stream = beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    void do_connect();
    void schedule_reconnect(std::string_view reason);
    void fail(beast::error_code ec, std::string_view what);
    void set_state(ConnectionState state, std::string_view reason);
    void read_loop();
    void write_next();

    net::io_context& io_;
    ssl::context& tls_;
    Config config_;

    std::unique_ptr<Stream> stream_;
    net::ip::tcp::resolver resolver_;
    net::steady_timer reconnect_timer_;
    beast::flat_buffer buffer_;
    std::vector<std::string> write_queue_;
    bool writing_ = false;

    ConnectionState state_ = ConnectionState::Disconnected;
    ReconnectBackoff backoff_;
    bool stopping_ = false;
    std::uint32_t reconnects_ = 0;
    std::uint64_t frames_ = 0;

    MessageHandler on_message_;
    StateHandler on_state_;
};

}  // namespace mm::exchange::binance
