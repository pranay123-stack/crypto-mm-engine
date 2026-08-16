#pragma once

/// \file IExchangeMarketData.hpp
/// The market-data side of the exchange boundary.
///
/// **Threading contract.** Every `IMarketDataSink` callback is invoked on the
/// adapter's I/O thread, never on the trading thread. The engine's sink
/// implementation does exactly one thing: stamp the event and push it into an
/// `SpscRing`. It must not touch the order book, strategy, risk, OMS or
/// portfolio -- those are owned by the trading thread (docs/concurrency.md §1),
/// and reaching them from here would reintroduce the shared mutable state the
/// single-writer design exists to eliminate.
///
/// **Nothing venue-specific appears in this file**, by construction: it
/// includes only `common/` and `exchange/common/`, so a Binance JSON field,
/// stream name, or error code has no route in.

#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/exchange/common/ExchangeTypes.hpp"
#include "mm/exchange/common/MarketDataEvents.hpp"

namespace mm::exchange {

/// What to receive for a symbol. A market maker quoting at the touch does not
/// need 1000 levels, and asking for them costs bandwidth, parse time and
/// venue rate-limit weight.
struct SubscriptionRequest {
    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    bool want_depth = true;
    bool want_trades = true;
    /// Honoured only when the venue reports `supports_native_bbo`; otherwise
    /// BBO is derived from depth and flagged `is_native = false`.
    bool want_bbo = false;

    /// Levels to maintain. The adapter clamps to the venue's maximum and
    /// reports the effective value through `InstrumentUpdateEvent`.
    std::uint32_t depth_levels = 50;
};

/// Implemented by the engine; called by the adapter on the I/O thread.
class IMarketDataSink {
public:
    virtual ~IMarketDataSink() = default;

    /// One normalized event. The reference is valid only for the duration of
    /// the call -- the adapter reuses its buffer immediately afterwards, so the
    /// sink must copy anything it intends to keep.
    virtual void on_market_data(const MarketDataEvent& event) = 0;
};

/// A venue's market-data connectivity.
///
/// Lifecycle: construct -> `start` -> `subscribe`* -> ... -> `stop` -> destroy.
/// Implementations own their I/O thread(s) and must have stopped them before
/// their destructor returns.
class IExchangeMarketData {
public:
    virtual ~IExchangeMarketData() = default;

    IExchangeMarketData() = default;
    IExchangeMarketData(const IExchangeMarketData&) = delete;
    IExchangeMarketData& operator=(const IExchangeMarketData&) = delete;

    /// Binds the sink and begins connecting. Returns immediately: connection
    /// progress is reported asynchronously through `ConnectionEvent` and
    /// `SessionStateEvent`. A synchronous "connected" return would be a lie on
    /// any real transport.
    ///
    /// The sink must outlive this object.
    [[nodiscard]] virtual Status start(IMarketDataSink& sink) = 0;

    /// Stops I/O and joins the adapter's threads. Idempotent. After it returns,
    /// no further callback will be delivered -- which is what makes it safe for
    /// the engine to tear down the sink afterwards.
    virtual void stop() = 0;

    [[nodiscard]] virtual Status subscribe(const SubscriptionRequest& request) = 0;
    [[nodiscard]] virtual Status unsubscribe(const Symbol& symbol) = 0;

    /// Requests a fresh book image. Called on startup and whenever the session
    /// enters `ResyncRequired`. The image arrives as `BookSnapshotEvent`,
    /// possibly chunked.
    [[nodiscard]] virtual Status request_snapshot(const Symbol& symbol) = 0;

    /// Requests the venue's trading rules; delivered as `InstrumentUpdateEvent`.
    /// Orders cannot be validated until these have arrived.
    [[nodiscard]] virtual Status request_instruments() = 0;

    /// Per-symbol session state. `SessionState::Ready` is the only value that
    /// permits quoting; see `is_quotable`.
    [[nodiscard]] virtual SessionState session_state(const Symbol& symbol) const = 0;

    /// Transport state for the connection as a whole.
    [[nodiscard]] virtual ConnectionState connection_state() const = 0;

    /// Age of the most recent event for a symbol, on the steady clock. The
    /// safety layer compares this against `safety.max_market_data_age_ms`; a
    /// socket that is open but silent is the failure this catches.
    [[nodiscard]] virtual Nanos data_age_ns(const Symbol& symbol) const = 0;

    [[nodiscard]] virtual const ExchangeCapabilities& capabilities() const = 0;
    [[nodiscard]] virtual VenueName venue() const = 0;
};

}  // namespace mm::exchange
