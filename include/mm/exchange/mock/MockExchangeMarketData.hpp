#pragma once

/// \file MockExchangeMarketData.hpp
/// A deterministic in-memory implementation of `IExchangeMarketData`.
///
/// Like the execution mock, this exists to validate the abstraction and to let
/// later phases drive the book and session machinery through sequences a real
/// venue produces only occasionally: a gap, a crossed snapshot, a socket that
/// stays open while data stops.
///
/// It enforces the documented session state machine on itself. A test that
/// tries to drive it from `Subscribed` straight to `Ready` fails, because a
/// real adapter cannot do that either -- the snapshot has to be applied first.

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "mm/common/Time.hpp"
#include "mm/exchange/common/IExchangeMarketData.hpp"

namespace mm::exchange::mock {

class MockExchangeMarketData final : public IExchangeMarketData {
public:
    MockExchangeMarketData(ManualClock& clock, ExchangeCapabilities caps);
    explicit MockExchangeMarketData(ManualClock& clock);
    ~MockExchangeMarketData() override;

    // ---------------------------------------------------- IExchangeMarketData
    [[nodiscard]] Status start(IMarketDataSink& sink) override;
    void stop() override;

    [[nodiscard]] Status subscribe(const SubscriptionRequest& request) override;
    [[nodiscard]] Status unsubscribe(const Symbol& symbol) override;
    [[nodiscard]] Status request_snapshot(const Symbol& symbol) override;
    [[nodiscard]] Status request_instruments() override;

    [[nodiscard]] SessionState session_state(const Symbol& symbol) const override;
    [[nodiscard]] ConnectionState connection_state() const override { return connection_; }
    [[nodiscard]] Nanos data_age_ns(const Symbol& symbol) const override;
    [[nodiscard]] const ExchangeCapabilities& capabilities() const override { return caps_; }
    [[nodiscard]] VenueName venue() const override { return VenueName("mock"); }

    // ------------------------------------------------------------- scripting

    /// Drives the session machine. Rejects an illegal transition rather than
    /// applying it, so a test cannot manufacture a state a real adapter could
    /// never reach.
    [[nodiscard]] Status set_session_state(const Symbol& symbol, SessionState to,
                                           std::string_view reason = {});

    void set_connection_state(ConnectionState state);

    /// Registers the venue's trading rules for a symbol. Publishes an
    /// `InstrumentUpdateEvent` when the session is running.
    void set_instrument(const InstrumentSpec& spec);

    /// Publishes a book image. Levels beyond `kMaxLevelsPerEvent` are chunked
    /// automatically -- exercising the chunking contract rather than quietly
    /// staying under the limit.
    [[nodiscard]] Status publish_snapshot(const Symbol& symbol, Seq last_update_id,
                                          const std::vector<PriceLevel>& bids,
                                          const std::vector<PriceLevel>& asks);

    [[nodiscard]] Status publish_update(const Symbol& symbol, Seq first_id, Seq final_id,
                                        const std::vector<PriceLevel>& bids,
                                        const std::vector<PriceLevel>& asks);

    [[nodiscard]] Status publish_trade(const Symbol& symbol, Px price, Qty quantity,
                                       Side aggressor, const TradeId& trade_id);

    [[nodiscard]] Status publish_bbo(const Symbol& symbol, const BestBidAsk& bbo, bool native);

    /// Re-sends the last event verbatim, as a venue does after a reconnect.
    [[nodiscard]] Status redeliver_last_event();

    [[nodiscard]] bool is_subscribed(const Symbol& symbol) const;
    [[nodiscard]] std::size_t subscription_count() const noexcept { return sessions_.size(); }
    [[nodiscard]] std::uint64_t published_events() const noexcept { return published_; }
    /// Snapshot requests received; lets a test assert that a resync actually
    /// asked the venue for a fresh image.
    [[nodiscard]] std::uint64_t snapshot_requests() const noexcept { return snapshot_requests_; }

private:
    struct SymbolSession {
        SessionState state = SessionState::Disconnected;
        SubscriptionRequest request{};
        Nanos last_event_ns = 0;
        Seq last_seq = kNoSeq;
    };

    void emit(MarketDataEvent& event);
    void emit_session_event(const Symbol& symbol, SessionState from, SessionState to,
                            std::string_view reason);
    [[nodiscard]] SymbolSession* find(const Symbol& symbol);
    [[nodiscard]] const SymbolSession* find(const Symbol& symbol) const;

    ManualClock& clock_;
    ExchangeCapabilities caps_{};
    IMarketDataSink* sink_ = nullptr;
    ConnectionState connection_ = ConnectionState::Disconnected;
    bool started_ = false;

    std::unordered_map<Symbol, SymbolSession> sessions_;
    std::unordered_map<Symbol, InstrumentSpec> instruments_;

    std::uint64_t published_ = 0;
    std::uint64_t snapshot_requests_ = 0;
    MarketDataEvent last_event_{};
    bool has_last_event_ = false;
};

}  // namespace mm::exchange::mock
