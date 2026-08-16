#pragma once

/// \file Config.hpp
/// Typed configuration, loaded from YAML at startup and immutable thereafter.
///
/// Two rules shape this file:
///
///  1. **Credentials never appear here.** They are read from the environment at
///     the point of use, inside the exchange adapter. A config file gets
///     committed, copied into a ticket, and pasted into a chat window; an API
///     key must not be able to travel that way.
///  2. **Live trading cannot happen by accident.** `mode: paper` is the default,
///     and reaching live requires several independent things to be true at once
///     (see `validate`). A typo, a missing field, or a half-edited file yields a
///     refusal to start -- never a live session.

#include <cstdint>
#include <string>
#include <vector>

#include "mm/common/Fixed.hpp"
#include "mm/common/Params.hpp"
#include "mm/common/Status.hpp"
#include "mm/common/Types.hpp"

namespace mm {

enum class TradingMode : std::uint8_t { Paper = 0, Live = 1 };
[[nodiscard]] std::string_view to_string(TradingMode m) noexcept;
[[nodiscard]] bool parse_trading_mode(std::string_view text, TradingMode& out) noexcept;

struct ExchangeConfig {
    std::string name = "binance";
    std::string rest_base_url;
    std::string ws_base_url;
    /// Environment variables holding credentials, e.g. "BINANCE_PAPER" ->
    /// BINANCE_PAPER_API_KEY / BINANCE_PAPER_API_SECRET.
    std::string credentials_env_prefix;
    std::int64_t recv_window_ms = 5'000;
    std::int64_t rest_timeout_ms = 3'000;
    /// Depth levels to maintain locally. Deeper books cost memory and update
    /// time; a market maker quoting at the touch rarely needs more than 50.
    std::int32_t book_depth = 50;
};

struct SymbolConfig {
    std::string symbol;
    bool enabled = true;
    /// Per-symbol overrides of the global risk block; unset fields inherit.
    Qty max_position{};
    Notional max_notional{};
    Qty quote_size{};
};

struct StrategyConfig {
    std::string name;
    std::int32_t version = 0;
    /// Wall-clock budget for one strategy callback. Exceeding it repeatedly
    /// disables the strategy (docs/concurrency.md §6).
    std::int64_t budget_ns = 50'000;
    std::int64_t timer_interval_ms = 100;
    Params params;
};

struct RiskConfig {
    // Position and exposure
    Qty max_position{};              ///< absolute, per symbol
    Notional max_notional{};         ///< absolute, per symbol
    Notional max_portfolio_notional{};

    // Per-order
    Qty max_order_qty{};
    Notional max_order_notional{};
    std::int32_t max_active_orders_per_symbol = 10;
    std::int32_t max_active_orders_total = 100;

    // Loss limits (positive numbers denoting a loss magnitude)
    Notional max_daily_loss{};
    Notional max_session_loss{};
    Notional emergency_loss{};  ///< trips the global kill switch

    // Quote sanity: refuse to quote further than this from the touch, which
    // catches a misconfigured strategy before the exchange does.
    std::int32_t max_quote_distance_bps = 500;
};

struct ExecutionRateConfig {
    std::int32_t max_orders_per_second = 20;
    std::int32_t max_cancels_per_second = 20;
    std::int32_t max_replaces_per_second = 20;
    std::int32_t max_messages_per_second = 50;
    /// How long an unacknowledged order may stay in SUBMITTING before it is
    /// declared UNKNOWN and handed to reconciliation.
    std::int64_t ack_timeout_ms = 5'000;
    std::int64_t cancel_timeout_ms = 5'000;
};

struct SafetyConfig {
    std::int64_t max_market_data_age_ms = 500;
    std::int64_t max_processing_delay_ms = 100;
    std::int64_t max_clock_skew_ms = 1'000;
    std::int32_t max_consecutive_rejects = 5;
    std::int64_t reconcile_interval_ms = 30'000;
    /// Explicit, separate from `mode`. Both must agree before an order can
    /// reach a real venue.
    bool live_trading_enabled = false;
    /// Cancel every resting order on a clean shutdown.
    bool cancel_all_on_shutdown = true;
};

struct PaperConfig {
    std::int64_t ack_latency_us = 2'000;
    std::int64_t fill_latency_us = 1'000;
    std::int64_t cancel_latency_us = 2'000;
    double maker_fee_bps = 1.0;
    double taker_fee_bps = 4.0;
    /// Probability that a resting order at the touch is filled when the book
    /// trades through it. Modelling queue position honestly matters: assuming
    /// every touch fill lands is how a paper run flatters a strategy.
    double queue_fill_ratio = 0.5;
    double reject_probability = 0.0;
};

struct MonitoringConfig {
    std::string http_host = "127.0.0.1";
    std::int32_t http_port = 8787;
    std::int64_t snapshot_interval_ms = 100;
    bool enabled = true;
};

struct PersistenceConfig {
    std::string journal_dir = "var/journal";
    std::string state_file = "var/state/engine_state.json";
    std::int64_t journal_flush_interval_ms = 200;
    bool fsync_on_flush = true;
};

struct LoggingConfig {
    std::string level = "info";
    std::string dir = "var/log";
    std::int64_t max_file_bytes = 256LL * 1024 * 1024;
    std::int32_t max_files = 10;
    bool console = true;
};

struct IoConfig {
    std::int32_t md_threads = 1;
    std::int32_t exec_threads = 1;
    /// Pin the trading thread to this CPU; -1 disables pinning. Off by default
    /// because on a shared machine pinning reliably makes latency worse.
    std::int32_t trading_cpu = -1;
};

struct EngineConfig {
    TradingMode mode = TradingMode::Paper;
    std::string session_name = "default";

    ExchangeConfig exchange;
    std::vector<SymbolConfig> symbols;
    StrategyConfig strategy;
    RiskConfig risk;
    ExecutionRateConfig execution;
    SafetyConfig safety;
    PaperConfig paper;
    MonitoringConfig monitoring;
    PersistenceConfig persistence;
    LoggingConfig logging;
    IoConfig io;

    /// Structural validation, applied to every mode.
    [[nodiscard]] Status validate() const;

    /// The live gate. Additive on top of `validate`, and deliberately paranoid:
    /// every one of these must hold before a single order can reach a real
    /// venue. `operator_confirmed_live` comes from an explicit CLI flag, not
    /// from any file, so a copied config alone can never go live.
    [[nodiscard]] Status validate_live_gates(bool operator_confirmed_live) const;

    [[nodiscard]] const SymbolConfig* find_symbol(std::string_view symbol) const;
};

/// Load and validate. Unknown top-level keys are an error, not a warning: a
/// silently ignored `max_postion` typo is a risk limit that was never applied.
[[nodiscard]] Result<EngineConfig> load_config_file(const std::string& path);
[[nodiscard]] Result<EngineConfig> load_config_string(const std::string& yaml_text);

}  // namespace mm
