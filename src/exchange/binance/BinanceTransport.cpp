#include "mm/exchange/binance/BinanceTransport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <utility>

namespace mm::exchange::binance {
namespace {

namespace http = boost::beast::http;

/// Identifies this client in venue logs. Useful when asking a venue why a
/// connection was dropped.
constexpr const char* kUserAgent = "mm-engine/0.1 (market-data)";

/// `SSL_set_tlsext_host_name` is an OpenSSL macro that expands to a C-style
/// cast, which our warning set rejects. Wrapping it once keeps the suppression
/// to a single line instead of scattering pragmas through the call sites.
///
/// This sets SNI, which is what makes the venue present the right certificate;
/// without it the handshake succeeds against a default vhost and hostname
/// verification then fails for reasons that look unrelated.
[[nodiscard]] bool set_sni_hostname(SSL* ssl, const char* host) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    return SSL_set_tlsext_host_name(ssl, host) != 0;
#pragma GCC diagnostic pop
}

}  // namespace

Result<std::shared_ptr<ssl::context>> make_tls_context() {
    auto ctx = std::make_shared<ssl::context>(ssl::context::tlsv12_client);
    boost::system::error_code ec;

    ctx->set_default_verify_paths(ec);
    if (ec) {
        return Status{ErrorCode::Unavailable,
                      "cannot load the system trust store: " + ec.message()};
    }
    // Verification on, and the hostname checked against the certificate.
    // Without both, TLS here would encrypt a conversation with anyone.
    ctx->set_verify_mode(ssl::verify_peer);
    ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                         ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                         ssl::context::no_tlsv1_1,
                     ec);
    if (ec) {
        return Status{ErrorCode::Internal, "cannot set TLS options: " + ec.message()};
    }
    return ctx;
}

// ============================================================== REST client

BinanceRestClient::BinanceRestClient(net::io_context& io, ssl::context& tls, std::string host,
                                     std::string port)
    : io_(io), tls_(tls), host_(std::move(host)), port_(std::move(port)) {}

namespace {

/// One in-flight GET, as an explicit sequence of named steps.
///
/// The obvious alternative -- a chain of nested completion lambdas -- nests six
/// deep by the time the response is read, which is unreadable and which GCC 13
/// cannot see through well enough to avoid false null-dereference diagnostics.
/// Each step here is a named member, and `finish` is the single exit so the
/// caller is answered exactly once on every path.
class RestRequest : public std::enable_shared_from_this<RestRequest> {
public:
    RestRequest(net::io_context& io, ssl::context& tls, std::string host, std::string port,
                std::string target, Nanos timeout, BinanceRestClient::Handler handler,
                std::uint64_t* failure_counter)
        : resolver_(net::make_strand(io)),
          stream_(net::make_strand(io), tls),
          host_(std::move(host)),
          port_(std::move(port)),
          deadline_(timeout),
          handler_(std::move(handler)),
          failure_counter_(failure_counter) {
        request_.version(11);
        request_.method(http::verb::get);
        request_.target(target);
        request_.set(http::field::host, host_);
        request_.set(http::field::user_agent, kUserAgent);
    }

    void run() {
        if (!set_sni_hostname(stream_.native_handle(), host_.c_str())) {
            finish(Status{ErrorCode::Internal, "cannot set the TLS SNI hostname"});
            return;
        }
        arm_deadline();
        resolver_.async_resolve(host_, port_,
                                [self = shared_from_this()](
                                    beast::error_code ec,
                                    const net::ip::tcp::resolver::results_type& endpoints) {
                                    self->on_resolve(ec, endpoints);
                                });
    }

private:
    void arm_deadline() {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::nanoseconds(deadline_));
    }

    void on_resolve(beast::error_code ec, const net::ip::tcp::resolver::results_type& endpoints) {
        if (ec) {
            finish(Status{ErrorCode::Unavailable, "DNS resolution failed: " + ec.message()});
            return;
        }
        arm_deadline();
        beast::get_lowest_layer(stream_).async_connect(
            endpoints, [self = shared_from_this()](beast::error_code connect_ec,
                                                   const net::ip::tcp::endpoint&) {
                self->on_connect(connect_ec);
            });
    }

    void on_connect(beast::error_code ec) {
        if (ec) {
            finish(Status{ErrorCode::Unavailable, "connect failed: " + ec.message()});
            return;
        }
        arm_deadline();
        stream_.async_handshake(ssl::stream_base::client,
                                [self = shared_from_this()](beast::error_code tls_ec) {
                                    self->on_handshake(tls_ec);
                                });
    }

    void on_handshake(beast::error_code ec) {
        if (ec) {
            finish(Status{ErrorCode::Unavailable, "TLS handshake failed: " + ec.message()});
            return;
        }
        arm_deadline();
        http::async_write(stream_, request_,
                          [self = shared_from_this()](beast::error_code write_ec, std::size_t) {
                              self->on_write(write_ec);
                          });
    }

    void on_write(beast::error_code ec) {
        if (ec) {
            finish(Status{ErrorCode::Unavailable, "write failed: " + ec.message()});
            return;
        }
        arm_deadline();
        http::async_read(stream_, buffer_, response_,
                         [self = shared_from_this()](beast::error_code read_ec, std::size_t) {
                             self->on_read(read_ec);
                         });
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            // A deadline expiry surfaces here as operation_aborted. Reporting
            // it as a timeout matters: the caller's retry budget treats a
            // timeout differently from a refusal.
            finish(Status{ErrorCode::Timeout, "read failed: " + ec.message()});
            return;
        }
        const auto status = response_.result_int();
        if (status != 200) {
            // 429 and 418 are the venue telling us to slow down; classifying
            // them as Unavailable lets the retry budget back off rather than
            // treating the request as permanently rejected.
            const bool throttled = (status == 429 || status == 418);
            finish(Status{throttled ? ErrorCode::Unavailable : ErrorCode::Rejected,
                          "HTTP " + std::to_string(status) + ": " +
                              response_.body().substr(0, 120)});
            return;
        }
        finish(std::move(response_.body()));
    }

    void finish(Result<std::string> result) {
        if (completed_) {
            return;
        }
        completed_ = true;
        if (result.is_error() && failure_counter_ != nullptr) {
            ++(*failure_counter_);
        }
        beast::error_code ignored;
        beast::get_lowest_layer(stream_).socket().close(ignored);
        handler_(std::move(result));
    }

    net::ip::tcp::resolver resolver_;
    beast::ssl_stream<beast::tcp_stream> stream_;
    beast::flat_buffer buffer_;
    http::request<http::empty_body> request_;
    http::response<http::string_body> response_;
    std::string host_;
    std::string port_;
    Nanos deadline_;
    BinanceRestClient::Handler handler_;
    std::uint64_t* failure_counter_ = nullptr;
    bool completed_ = false;
};

}  // namespace

void BinanceRestClient::async_get(std::string target, Nanos timeout, Handler handler) {
    ++requests_issued_;
    std::make_shared<RestRequest>(io_, tls_, host_, port_, std::move(target), timeout,
                                  std::move(handler), &requests_failed_)
        ->run();
}

// ============================================================ WebSocket

BinanceWsSession::BinanceWsSession(net::io_context& io, ssl::context& tls, Config config)
    : io_(io),
      tls_(tls),
      config_(std::move(config)),
      resolver_(net::make_strand(io)),
      reconnect_timer_(net::make_strand(io)),
      backoff_(config_.backoff, config_.backoff_seed) {}

void BinanceWsSession::set_state(ConnectionState state, std::string_view reason) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (on_state_) {
        on_state_(state, reason);
    }
}

void BinanceWsSession::start() {
    stopping_ = false;
    do_connect();
}

void BinanceWsSession::stop() {
    stopping_ = true;
    boost::system::error_code ignored;
    write_queue_.clear();
    writing_ = false;
    reconnect_timer_.cancel();
    if (stream_) {
        // Best-effort close; the socket is torn down regardless so `stop` can
        // be called from a shutdown path that cannot wait.
        beast::get_lowest_layer(*stream_).socket().close(ignored);
    }
    set_state(ConnectionState::Disconnected, "stopped");
}

void BinanceWsSession::fail(beast::error_code ec, std::string_view what) {
    std::string reason(what);
    reason.append(": ").append(ec.message());
    schedule_reconnect(reason);
}

void BinanceWsSession::schedule_reconnect(std::string_view reason) {
    if (stopping_) {
        return;
    }
    if (backoff_.exhausted()) {
        // Never retry forever: it hides the real fault and risks a venue ban.
        set_state(ConnectionState::Failed, "reconnect budget exhausted");
        return;
    }

    ++reconnects_;
    set_state(ConnectionState::Reconnecting, reason);

    const Nanos delay = backoff_.next_delay();
    reconnect_timer_.expires_after(std::chrono::nanoseconds(delay));
    reconnect_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec || self->stopping_) {
            return;
        }
        self->do_connect();
    });
}

void BinanceWsSession::do_connect() {
    if (stopping_) {
        return;
    }
    set_state(ConnectionState::Connecting, "dialling");

    // A fresh stream per attempt. Reusing one after a failure risks inheriting
    // half-closed TLS state that fails in confusing ways much later.
    stream_ = std::make_unique<Stream>(net::make_strand(io_), tls_);
    buffer_.consume(buffer_.size());

    const auto handshake_deadline = std::chrono::nanoseconds(config_.handshake_timeout);

    resolver_.async_resolve(
        config_.host, config_.port,
        [self = shared_from_this(), handshake_deadline](
            beast::error_code ec, const net::ip::tcp::resolver::results_type& endpoints) {
            if (ec) {
                self->fail(ec, "resolve");
                return;
            }
            beast::get_lowest_layer(*self->stream_).expires_after(handshake_deadline);
            beast::get_lowest_layer(*self->stream_)
                .async_connect(endpoints, [self, handshake_deadline](
                                              beast::error_code connect_ec,
                                              const net::ip::tcp::endpoint&) {
                    if (connect_ec) {
                        self->fail(connect_ec, "connect");
                        return;
                    }
                    if (!set_sni_hostname(self->stream_->next_layer().native_handle(),
                                          self->config_.host.c_str())) {
                        self->schedule_reconnect("cannot set the TLS SNI hostname");
                        return;
                    }
                    beast::get_lowest_layer(*self->stream_).expires_after(handshake_deadline);
                    self->stream_->next_layer().async_handshake(
                        ssl::stream_base::client, [self](beast::error_code tls_ec) {
                            if (tls_ec) {
                                self->fail(tls_ec, "TLS handshake");
                                return;
                            }
                            // Beast's own timeout policy takes over from the
                            // tcp_stream timer once the WebSocket is running.
                            beast::get_lowest_layer(*self->stream_).expires_never();
                            self->stream_->set_option(
                                beast::websocket::stream_base::timeout::suggested(
                                    beast::role_type::client));
                            self->stream_->set_option(
                                beast::websocket::stream_base::decorator(
                                    [](beast::websocket::request_type& req) {
                                        req.set(http::field::user_agent, kUserAgent);
                                    }));

                            self->stream_->async_handshake(
                                self->config_.host, self->config_.target,
                                [self](beast::error_code ws_ec) {
                                    if (ws_ec) {
                                        self->fail(ws_ec, "WebSocket handshake");
                                        return;
                                    }
                                    // Only a completed handshake counts as
                                    // success, so a venue that accepts TCP and
                                    // then rejects the upgrade still backs off.
                                    self->backoff_.reset();
                                    self->set_state(ConnectionState::Connected, "connected");
                                    self->read_loop();
                                });
                        });
                });
        });
}

bool BinanceWsSession::send_text(const std::string& payload) {
    if (state_ != ConnectionState::Connected || !stream_) {
        return false;
    }
    // Beast permits one outstanding write at a time, so frames queue.
    write_queue_.push_back(payload);
    if (!writing_) {
        write_next();
    }
    return true;
}

void BinanceWsSession::write_next() {
    if (write_queue_.empty() || !stream_ || stopping_) {
        writing_ = false;
        return;
    }
    writing_ = true;
    stream_->text(true);
    stream_->async_write(net::buffer(write_queue_.front()),
                         [self = shared_from_this()](beast::error_code ec, std::size_t) {
                             if (!self->write_queue_.empty()) {
                                 self->write_queue_.erase(self->write_queue_.begin());
                             }
                             if (ec) {
                                 self->writing_ = false;
                                 if (!self->stopping_) {
                                     self->fail(ec, "write");
                                 }
                                 return;
                             }
                             self->write_next();
                         });
}

void BinanceWsSession::read_loop() {
    if (stopping_ || !stream_) {
        return;
    }
    stream_->async_read(buffer_, [self = shared_from_this()](beast::error_code ec,
                                                             std::size_t bytes) {
        if (ec) {
            if (self->stopping_) {
                return;
            }
            self->fail(ec, "read");
            return;
        }
        ++self->frames_;

        // Stamp at the socket. Anything later would fold our own queuing into
        // what is supposed to be the venue-to-process latency.
        const Nanos recv_ns = steady_ns();
        if (self->on_message_) {
            const auto data = self->buffer_.data();
            self->on_message_(
                std::string_view(static_cast<const char*>(data.data()), bytes), recv_ns);
        }
        self->buffer_.consume(self->buffer_.size());
        self->read_loop();
    });
}

}  // namespace mm::exchange::binance
