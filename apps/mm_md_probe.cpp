/// \file mm_md_probe.cpp
/// Live public-market-data smoke test.
///
/// Connects to Binance public streams, synchronizes a real order book, and
/// reports what happened. **It places no orders, sends no credentials, and
/// touches no account endpoint** — the adapter it drives has no order-entry
/// code in it at all.
///
///     ./build/mm_md_probe BTCUSDT 20
///
/// Exits non-zero if the book never reached a synchronized state, so it is
/// usable as a gate rather than something a human has to read.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "mm/exchange/binance/BinanceMarketData.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

/// Counts what arrives. Stands in for the engine's real sink, which pushes into
/// an SPSC ring; counting is the equivalent for a probe.
class ProbeSink final : public mm::exchange::IMarketDataSink {
public:
    void on_market_data(const mm::exchange::MarketDataEvent& event) override {
        using T = mm::exchange::MarketDataEventType;
        switch (event.type) {
            case T::BookSnapshot: ++snapshots; break;
            case T::BookUpdate:   ++updates;   break;
            case T::Trade:        ++trades;    break;
            case T::SessionState:
                ++session_events;
                std::printf("  session %-14s -> %-14s  %.*s\n",
                            mm::exchange::to_string(event.payload.session.from).data(),
                            mm::exchange::to_string(event.payload.session.to).data(),
                            static_cast<int>(event.payload.session.reason.size()),
                            event.payload.session.reason.c_str());
                if (event.payload.session.to == mm::exchange::SessionState::Ready) {
                    reached_ready = true;
                }
                break;
            case T::Connection:
                std::printf("  connection %s -> %s\n",
                            mm::exchange::to_string(event.payload.connection.from).data(),
                            mm::exchange::to_string(event.payload.connection.to).data());
                break;
            case T::InstrumentUpdate:
                ++instruments;
                break;
            case T::Bbo:
            case T::None:
                break;
        }
    }

    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> updates{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<std::uint64_t> session_events{0};
    std::atomic<std::uint64_t> instruments{0};
    std::atomic<bool> reached_ready{false};
};

}  // namespace

int main(int argc, char** argv) {
    const std::string symbol_text = (argc > 1) ? argv[1] : "BTCUSDT";
    const int seconds_to_run = (argc > 2) ? std::atoi(argv[2]) : 20;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::printf("Binance public market-data probe\n");
    std::printf("  symbol   : %s\n", symbol_text.c_str());
    std::printf("  duration : %ds\n", seconds_to_run);
    std::printf("  NOTE     : public data only; no credentials, no orders.\n\n");

    mm::exchange::binance::BinanceMarketData::Config config;
    config.depth_update_ms = 100;
    config.snapshot_limit = 1000;
    config.sync.max_depth = 50;
    config.sync.max_data_age_ns = mm::millis(2'000);

    mm::exchange::binance::BinanceMarketData feed(config, mm::SystemClock::instance());
    ProbeSink sink;

    mm::exchange::SubscriptionRequest request;
    request.symbol = mm::Symbol(symbol_text);
    request.want_depth = true;
    request.want_trades = true;
    request.depth_levels = 50;
    if (const mm::Status s = feed.subscribe(request); s.is_error()) {
        std::printf("subscribe failed: %s\n", s.to_string().c_str());
        return 2;
    }
    if (const mm::Status s = feed.start(sink); s.is_error()) {
        std::printf("start failed: %s\n", s.to_string().c_str());
        return 2;
    }

    const mm::Symbol symbol(symbol_text);
    for (int elapsed = 0; elapsed < seconds_to_run && !g_stop.load(std::memory_order_relaxed);
         ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        mm::exchange::binance::BookView view;
        const bool have_view = feed.book_view(symbol, view);
        const auto age_ms =
            static_cast<double>(feed.data_age_ns(symbol)) / static_cast<double>(mm::kNanosPerMilli);

        std::printf("[%2ds] %-14s", elapsed + 1,
                    mm::exchange::to_string(feed.session_state(symbol)).data());
        if (have_view && view.bid_count > 0 && view.ask_count > 0) {
            const mm::BestBidAsk bbo = view.bbo();
            std::printf("  bid %s x %s | ask %s x %s | spread %s | seq %llu | age %.0fms",
                        bbo.bid_px.to_string().c_str(), bbo.bid_qty.to_string().c_str(),
                        bbo.ask_px.to_string().c_str(), bbo.ask_qty.to_string().c_str(),
                        (bbo.ask_px - bbo.bid_px).to_string().c_str(),
                        static_cast<unsigned long long>(view.last_update_id),
                        age_ms > 1e12 ? -1.0 : age_ms);
            if (!bbo.is_sane()) {
                std::printf("  <-- BOOK NOT SANE");
            }
        } else {
            std::printf("  (no synchronized book yet)");
        }
        std::printf("\n");
    }

    const auto diag = feed.diagnostics();
    feed.stop();

    std::printf("\nresults\n");
    std::printf("  frames received  : %llu\n", static_cast<unsigned long long>(diag.frames_received));
    std::printf("  events published : %llu\n", static_cast<unsigned long long>(diag.events_published));
    std::printf("  decode errors    : %llu\n", static_cast<unsigned long long>(diag.decode_errors));
    std::printf("  snapshot reqs    : %llu (%llu failed)\n",
                static_cast<unsigned long long>(diag.snapshot_requests),
                static_cast<unsigned long long>(diag.snapshot_failures));
    std::printf("  reconnects       : %u\n", diag.reconnects);
    std::printf("  snapshots/updates/trades: %llu / %llu / %llu\n",
                static_cast<unsigned long long>(sink.snapshots.load()),
                static_cast<unsigned long long>(sink.updates.load()),
                static_cast<unsigned long long>(sink.trades.load()));
    std::printf("  instruments      : %llu loaded, %llu failed\n",
                static_cast<unsigned long long>(diag.instruments_loaded),
                static_cast<unsigned long long>(diag.instrument_failures));

    const bool ok = sink.reached_ready.load() && diag.decode_errors == 0 &&
                    sink.updates.load() > 0 && diag.instruments_loaded > 0 &&
                    diag.instrument_failures == 0;
    std::printf("\n%s\n", ok ? "PASS: book synchronized and stayed clean"
                             : "FAIL: book did not reach a clean synchronized state");
    return ok ? 0 : 1;
}
