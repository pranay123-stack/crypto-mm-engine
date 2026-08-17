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
#include <map>
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
    /// No default. A venue is an explicit choice, and defaulting to one would
    /// bake an adapter's identity into the core -- the core must remain valid
    /// with every adapter removed from the tree. `validate()` requires it.
    std::string name;
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

    /// Whether the strategy runs at all.
    bool enabled = true;
    /// Operator quote switch, separate from `enabled`. When false the strategy
    /// is still evaluated so its internal state stays warm, but no quoting
    /// intent is accepted. Turning quoting off must not require restarting a
    /// strategy that has spent minutes warming up.
    bool quoting_enabled = true;

    /// When the runtime asks the strategy for intent. See
    /// `mm::strategy::EvaluationMode`; parsed and validated at load time.
    std::string evaluation_mode = "on_bbo_change_and_timer";

    /// Budget for one strategy callback. Exceeding it repeatedly faults the
    /// strategy (docs/concurrency.md §6, docs/strategy-runtime.md).
    std::int64_t budget_ns = 50'000;
    std::int32_t max_consecutive_budget_violations = 5;
    std::int32_t max_consecutive_invalid_outputs = 3;
    std::int64_t timer_interval_ms = 100;

    /// Sanity bound on how far a quote may sit from the touch. A strategy
    /// pricing further away has almost certainly miscalculated.
    std::int32_t max_quote_distance_bps = 500;

    /// Finalized strategy parameters. Populated from the `strategies.<name>`
    /// block for the selected strategy, so the core never has a field that
    /// belongs to one particular strategy. Legacy `strategy.params` is merged
    /// in for compatibility with Phase 2 configs.
    Params params;
};

/// Infrastructure controls for the quote manager (Phase 6 §29).
///
/// Deliberately separate from `strategies.<name>`: these bound how the
/// infrastructure behaves, not what the strategy wants. They decide *whether*
/// an order is worth rewriting, never *what* to write -- encoding strategy
/// behaviour here would make the strategy's stated quotes a fiction.
struct QuoteManagerConfigYaml {
    /// An intent older than this is not acted on, and resting quotes are
    /// withdrawn: intent describes a market that existed at a moment.
    std::int64_t max_intent_age_ms = 250;
    /// Market data older than this withdraws quotes regardless of intent.
    std::int64_t max_market_data_age_ms = 500;

    /// Smallest price difference, in ticks, worth a replacement. One means any
    /// tick of movement is material.
    std::int32_t min_price_move_ticks = 1;
    /// Smallest quantity difference, as a fraction of the desired quantity,
    /// worth a replacement. Zero means any difference is material.
    double min_quantity_move_fraction = 0.0;
    /// Minimum interval between replacements of the same quote.
    std::int64_t min_replace_interval_ms = 0;
    /// Quiet period after a cancel before the same side may be re-quoted.
    std::int64_t cooldown_after_cancel_ms = 0;
    /// How long to wait for the OMS to reflect a request already issued before
    /// assuming it was lost. Prevents one order per evaluation cycle.
    std::int64_t assume_request_lost_after_ms = 1'000;

    /// Replace a partially filled order to restore the intended resting size.
    bool replenish_partial_fills = true;
};

/// Per-symbol risk bounds. Symbols are configured, never hard-coded into risk
/// logic (Phase 7 §25).
struct SymbolRiskConfig {
    std::string symbol;
    Qty max_position{};
    Notional max_position_notional{};
    Qty max_order_quantity{};
    Notional max_order_notional{};
    Qty max_working_exposure{};
    Qty max_side_exposure{};
    std::int32_t max_open_orders = 0;
    /// Band around the reference price, in basis points.
    std::int32_t price_band_bps = 0;
};

struct RiskConfig {
    /// Whether the engine arms at all. A disarmed engine refuses every
    /// exposure-adding action, which is the correct default for a config that
    /// has not been reviewed.
    bool enabled = false;

    /// Per-symbol bounds. When present these are authoritative; the flat fields
    /// below remain for the pre-Phase-7 shape and are used as the default for
    /// symbols without their own entry.
    std::vector<SymbolRiskConfig> symbols;

    // ---- freshness ----
    std::int64_t max_market_data_age_ms = 500;
    std::int64_t max_position_age_ms = 5'000;

    // ---- rate limits ----
    std::int32_t max_new_orders_per_second = 0;
    std::int32_t max_cancels_per_second = 0;
    std::int32_t max_replaces_per_second = 0;
    std::int32_t max_actions_per_second = 0;
    /// Burst allowance. A market-data burst legitimately produces a cluster of
    /// actions; the bucket absorbs it while bounding the sustained rate.
    std::int32_t burst_capacity = 0;

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

/// Phase 9 §38. The paper execution environment.
///
/// Phase 1 sketched this block with two probabilities in it
/// (`queue_fill_ratio`, `reject_probability`). Both are gone. A simulator whose
/// default behaviour is random produces tests that fail one run in twenty,
/// which trains people to re-run rather than to look. Queue position is now a
/// deterministic share of displayed size, and failures are injected by count.
struct PaperConfig {
    /// Simulated latency, in microseconds. **These are simulation values, not
    /// measurements of any real venue**, and nothing should quote them as such.
    std::int64_t request_latency_us = 500;
    std::int64_t ack_latency_us = 1'500;
    std::int64_t cancel_latency_us = 1'500;
    std::int64_t replace_latency_us = 2'000;
    std::int64_t fill_latency_us = 1'000;
    std::int64_t query_latency_us = 5'000;

    /// "displayed_liquidity" (conservative, the default) or "full_on_cross".
    std::string fill_model = "displayed_liquidity";
    /// "reject" (what most venues do) or "expire".
    std::string post_only_policy = "reject";
    /// "atomic" or "unsupported" -- the latter forces the quote manager to
    /// decompose a replace into cancel-then-new, which is worth being able to
    /// test against a venue that genuinely lacks it.
    std::string replace_mode = "atomic";

    /// Share of displayed size assumed available to us, in basis points.
    /// 10000 means "always first in the queue", which is the assumption that
    /// makes a paper run look better than reality.
    std::int32_t queue_share_bps = 5'000;

    /// A book older than this stops producing fills. Cancellation and
    /// reconciliation continue regardless.
    std::int64_t max_book_age_ms = 500;

    std::int32_t max_orders = 1'024;

    /// Reserved for optional stochastic simulation. Zero -- the default, and
    /// the only value any test uses -- means fully deterministic.
    std::uint64_t deterministic_seed = 0;

    double maker_fee_bps = 1.0;
    double taker_fee_bps = 4.0;
};

/// Which execution environment the engine runs against.
enum class ExecutionMode : std::uint8_t {
    /// The simulated venue. The only implemented mode.
    Paper,
    /// A real venue. **No live execution adapter exists yet**; selecting this
    /// fails at startup rather than falling back to paper, because a silent
    /// fallback is how a run that was supposed to be live quietly is not --
    /// or, far worse, the reverse.
    Live,
};
[[nodiscard]] std::string_view to_string(ExecutionMode m) noexcept;
[[nodiscard]] bool parse_execution_mode(std::string_view text, ExecutionMode& out) noexcept;

struct ExecutionConfig {
    ExecutionMode mode = ExecutionMode::Paper;
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

/// Phase 8 §38. Everything the OMS needs from a config file. Timeouts are the
/// load-bearing ones: they decide how long the engine is willing to believe a
/// request is still in flight before admitting it does not know.
struct OmsConfigYaml {
    /// Prefix stamped into every client order id we mint. Must leave room for
    /// the session and sequence within the venue's id length limit.
    std::string client_id_prefix = "mm";
    /// Distinguishes ids minted by one run from another's. Zero means "derive
    /// from the session start time" and is resolved before the OMS is built.
    std::uint64_t session_id = 0;
    /// The venue's maximum client-order-id length. Truncating an id would make
    /// two orders indistinguishable, so generation fails instead.
    std::uint32_t max_client_id_length = 36;

    /// How long a request may stay unanswered before the order becomes
    /// Unknown. Not a cancellation -- see docs/oms.md §12.
    std::uint32_t new_request_timeout_ms = 5'000;
    std::uint32_t cancel_request_timeout_ms = 5'000;
    std::uint32_t replace_request_timeout_ms = 5'000;

    /// How often to reconcile against the venue's open-order listing. Zero
    /// disables periodic reconciliation; startup reconciliation is separate.
    std::uint32_t reconciliation_interval_ms = 30'000;

    /// Capacity of the order table. Reached means new orders are refused, not
    /// that older ones are evicted.
    std::uint32_t max_orders = 1'024;

    /// Journal retention. Zero disables in-memory journalling entirely.
    std::uint32_t journal_capacity = 65'536;
    bool journal_enabled = true;
};

struct EngineConfig {
    TradingMode mode = TradingMode::Paper;
    std::string session_name = "default";

    ExchangeConfig exchange;
    std::vector<SymbolConfig> symbols;
    StrategyConfig strategy;
    QuoteManagerConfigYaml quote;
    /// Parameter blocks for every strategy present in the file, keyed by name.
    /// Only the selected strategy's block is merged into `strategy.params`;
    /// the rest are kept so a config can carry several and switch between them
    /// by changing one line.
    std::map<std::string, Params, std::less<>> strategy_params;
    RiskConfig risk;
    OmsConfigYaml oms;
    ExecutionConfig execution_env;
    ExecutionRateConfig execution;
    SafetyConfig safety;
    PaperConfig paper;
    MonitoringConfig monitoring;
    PersistenceConfig persistence;
    LoggingConfig logging;
    IoConfig io;

    /// Which execution environment will actually be built. `mode: live` implies
    /// live execution: a file that asked for live trading and silently got a
    /// simulator would be the worst possible surprise.
    [[nodiscard]] ExecutionMode effective_execution_mode() const noexcept;

    /// Whether that environment exists in this build. Separate from structural
    /// validity, which must not depend on which phase the project is in.
    [[nodiscard]] Status validate_execution_available() const;

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
