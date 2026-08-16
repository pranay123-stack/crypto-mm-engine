#include "mm/common/Config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

namespace mm {
namespace {

std::string join(std::string_view path, std::string_view key) {
    std::string out(path);
    if (!out.empty()) {
        out.push_back('.');
    }
    out.append(key);
    return out;
}

Status type_error(std::string_view path, std::string_view expected) {
    std::string msg = "config key '";
    msg.append(path).append("' is not a valid ").append(expected);
    return {ErrorCode::ParseError, msg};
}

/// An unrecognised key is an error rather than a warning. `max_postion: 0.5`
/// would otherwise parse cleanly, apply nothing, and leave the engine running
/// with an unbounded position limit -- a silent failure of exactly the control
/// that exists to prevent catastrophic loss.
Status check_known_keys(const YAML::Node& map, std::string_view path,
                        std::initializer_list<const char*> known) {
    if (!map || !map.IsMap()) {
        return Status::ok();
    }
    for (const auto& entry : map) {
        const std::string key = entry.first.as<std::string>();
        const bool recognised = std::any_of(known.begin(), known.end(),
                                            [&](const char* k) { return key == k; });
        if (!recognised) {
            std::string msg = "unknown config key '";
            msg.append(join(path, key)).append("'");
            return {ErrorCode::InvalidArgument, msg};
        }
    }
    return Status::ok();
}

template <class T>
Status read_scalar(const YAML::Node& map, const char* key, std::string_view path, T& out) {
    if (!map || !map.IsMap()) {
        return Status::ok();
    }
    const YAML::Node node = map[key];
    if (!node || node.IsNull()) {
        return Status::ok();  // absent: keep the default
    }
    try {
        out = node.as<T>();
    } catch (const YAML::Exception&) {
        return type_error(join(path, key), "scalar");
    }
    return Status::ok();
}

/// Fixed-point fields are read as *text* and parsed exactly. Reading them as
/// doubles and converting would reintroduce the binary rounding that the fixed
/// point representation exists to eliminate -- a max_position of 0.1 would
/// become 0.09999999999999999.
template <class Tag>
Status read_fixed(const YAML::Node& map, const char* key, std::string_view path,
                  Fixed<Tag>& out) {
    if (!map || !map.IsMap()) {
        return Status::ok();
    }
    const YAML::Node node = map[key];
    if (!node || node.IsNull()) {
        return Status::ok();
    }
    std::string text;
    try {
        text = node.as<std::string>();
    } catch (const YAML::Exception&) {
        return type_error(join(path, key), "number");
    }
    if (!Fixed<Tag>::parse(text, out)) {
        return type_error(join(path, key), "decimal number");
    }
    return Status::ok();
}

void flatten_params(const YAML::Node& node, const std::string& prefix, Params& out) {
    if (node.IsScalar()) {
        out.set(prefix, node.as<std::string>());
        return;
    }
    if (node.IsMap()) {
        for (const auto& entry : node) {
            const std::string key = entry.first.as<std::string>();
            flatten_params(entry.second, prefix.empty() ? key : prefix + "." + key, out);
        }
        return;
    }
    if (node.IsSequence()) {
        // Sequences flatten to indexed keys plus a count, so a strategy can read
        // e.g. levels.0, levels.1, levels.count without linking a YAML library.
        std::size_t i = 0;
        for (const auto& item : node) {
            flatten_params(item, prefix + "." + std::to_string(i), out);
            ++i;
        }
        out.set(prefix + ".count", std::to_string(i));
    }
}

Status parse_exchange(const YAML::Node& n, ExchangeConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(n, "exchange",
                                        {"name", "rest_base_url", "ws_base_url",
                                         "credentials_env_prefix", "recv_window_ms",
                                         "rest_timeout_ms", "book_depth"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "name", "exchange", cfg.name));
    MM_RETURN_IF_ERROR(read_scalar(n, "rest_base_url", "exchange", cfg.rest_base_url));
    MM_RETURN_IF_ERROR(read_scalar(n, "ws_base_url", "exchange", cfg.ws_base_url));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "credentials_env_prefix", "exchange", cfg.credentials_env_prefix));
    MM_RETURN_IF_ERROR(read_scalar(n, "recv_window_ms", "exchange", cfg.recv_window_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "rest_timeout_ms", "exchange", cfg.rest_timeout_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "book_depth", "exchange", cfg.book_depth));
    return Status::ok();
}

Status parse_symbols(const YAML::Node& n, std::vector<SymbolConfig>& out) {
    if (!n) {
        return Status::ok();
    }
    if (!n.IsSequence()) {
        return {ErrorCode::InvalidArgument, "'symbols' must be a sequence"};
    }
    for (const auto& item : n) {
        SymbolConfig sym;
        if (item.IsScalar()) {
            sym.symbol = item.as<std::string>();
        } else if (item.IsMap()) {
            MM_RETURN_IF_ERROR(check_known_keys(
                item, "symbols", {"symbol", "enabled", "max_position", "max_notional",
                                  "quote_size"}));
            MM_RETURN_IF_ERROR(read_scalar(item, "symbol", "symbols", sym.symbol));
            MM_RETURN_IF_ERROR(read_scalar(item, "enabled", "symbols", sym.enabled));
            MM_RETURN_IF_ERROR(read_fixed(item, "max_position", "symbols", sym.max_position));
            MM_RETURN_IF_ERROR(read_fixed(item, "max_notional", "symbols", sym.max_notional));
            MM_RETURN_IF_ERROR(read_fixed(item, "quote_size", "symbols", sym.quote_size));
        } else {
            return {ErrorCode::InvalidArgument, "each entry in 'symbols' must be a name or a map"};
        }
        out.push_back(std::move(sym));
    }
    return Status::ok();
}

Status parse_strategy(const YAML::Node& n, StrategyConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "strategy",
        {"name", "version", "enabled", "quoting_enabled", "evaluation_mode", "budget_ns",
         "max_consecutive_budget_violations", "max_consecutive_invalid_outputs",
         "timer_interval_ms", "max_quote_distance_bps", "params"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "name", "strategy", cfg.name));
    MM_RETURN_IF_ERROR(read_scalar(n, "version", "strategy", cfg.version));
    MM_RETURN_IF_ERROR(read_scalar(n, "enabled", "strategy", cfg.enabled));
    MM_RETURN_IF_ERROR(read_scalar(n, "quoting_enabled", "strategy", cfg.quoting_enabled));
    MM_RETURN_IF_ERROR(read_scalar(n, "evaluation_mode", "strategy", cfg.evaluation_mode));
    MM_RETURN_IF_ERROR(read_scalar(n, "budget_ns", "strategy", cfg.budget_ns));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_consecutive_budget_violations", "strategy",
                                   cfg.max_consecutive_budget_violations));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_consecutive_invalid_outputs", "strategy",
                                   cfg.max_consecutive_invalid_outputs));
    MM_RETURN_IF_ERROR(read_scalar(n, "timer_interval_ms", "strategy", cfg.timer_interval_ms));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "max_quote_distance_bps", "strategy", cfg.max_quote_distance_bps));
    if (n && n["params"] && !n["params"].IsNull()) {
        flatten_params(n["params"], "", cfg.params);
    }
    return Status::ok();
}

/// Strategy-specific parameter blocks, keyed by strategy name.
///
/// The keys are strategy names, so they cannot be checked against a fixed list
/// the way every other section is. What *is* checked is that the selected
/// strategy has a block; an unknown key here is a parameter set for a strategy
/// that is not running, which is inert rather than dangerous.
Status parse_strategy_params(const YAML::Node& n,
                             std::map<std::string, Params, std::less<>>& out) {
    if (!n || n.IsNull()) {
        return Status::ok();
    }
    if (!n.IsMap()) {
        return {ErrorCode::InvalidArgument, "'strategies' must be a map keyed by strategy name"};
    }
    for (const auto& entry : n) {
        const std::string name = entry.first.as<std::string>();
        if (name.empty()) {
            return {ErrorCode::InvalidArgument, "'strategies' contains an empty key"};
        }
        Params params;
        if (!entry.second.IsNull()) {
            flatten_params(entry.second, "", params);
        }
        out.insert_or_assign(name, std::move(params));
    }
    return Status::ok();
}

Status parse_risk(const YAML::Node& n, RiskConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "risk",
        {"max_position", "max_notional", "max_portfolio_notional", "max_order_qty",
         "max_order_notional", "max_active_orders_per_symbol", "max_active_orders_total",
         "max_daily_loss", "max_session_loss", "emergency_loss", "max_quote_distance_bps"}));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_position", "risk", cfg.max_position));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_notional", "risk", cfg.max_notional));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_portfolio_notional", "risk", cfg.max_portfolio_notional));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_order_qty", "risk", cfg.max_order_qty));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_order_notional", "risk", cfg.max_order_notional));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "max_active_orders_per_symbol", "risk", cfg.max_active_orders_per_symbol));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "max_active_orders_total", "risk", cfg.max_active_orders_total));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_daily_loss", "risk", cfg.max_daily_loss));
    MM_RETURN_IF_ERROR(read_fixed(n, "max_session_loss", "risk", cfg.max_session_loss));
    MM_RETURN_IF_ERROR(read_fixed(n, "emergency_loss", "risk", cfg.emergency_loss));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "max_quote_distance_bps", "risk", cfg.max_quote_distance_bps));
    return Status::ok();
}

Status parse_execution(const YAML::Node& n, ExecutionRateConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "execution",
        {"max_orders_per_second", "max_cancels_per_second", "max_replaces_per_second",
         "max_messages_per_second", "ack_timeout_ms", "cancel_timeout_ms"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_orders_per_second", "execution", cfg.max_orders_per_second));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_cancels_per_second", "execution", cfg.max_cancels_per_second));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_replaces_per_second", "execution", cfg.max_replaces_per_second));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_messages_per_second", "execution", cfg.max_messages_per_second));
    MM_RETURN_IF_ERROR(read_scalar(n, "ack_timeout_ms", "execution", cfg.ack_timeout_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "cancel_timeout_ms", "execution", cfg.cancel_timeout_ms));
    return Status::ok();
}

Status parse_safety(const YAML::Node& n, SafetyConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "safety",
        {"max_market_data_age_ms", "max_processing_delay_ms", "max_clock_skew_ms",
         "max_consecutive_rejects", "reconcile_interval_ms", "live_trading_enabled",
         "cancel_all_on_shutdown"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_market_data_age_ms", "safety", cfg.max_market_data_age_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_processing_delay_ms", "safety", cfg.max_processing_delay_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_clock_skew_ms", "safety", cfg.max_clock_skew_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_consecutive_rejects", "safety", cfg.max_consecutive_rejects));
    MM_RETURN_IF_ERROR(read_scalar(n, "reconcile_interval_ms", "safety", cfg.reconcile_interval_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "live_trading_enabled", "safety", cfg.live_trading_enabled));
    MM_RETURN_IF_ERROR(read_scalar(n, "cancel_all_on_shutdown", "safety", cfg.cancel_all_on_shutdown));
    return Status::ok();
}

Status parse_paper(const YAML::Node& n, PaperConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "paper",
        {"ack_latency_us", "fill_latency_us", "cancel_latency_us", "maker_fee_bps",
         "taker_fee_bps", "queue_fill_ratio", "reject_probability"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "ack_latency_us", "paper", cfg.ack_latency_us));
    MM_RETURN_IF_ERROR(read_scalar(n, "fill_latency_us", "paper", cfg.fill_latency_us));
    MM_RETURN_IF_ERROR(read_scalar(n, "cancel_latency_us", "paper", cfg.cancel_latency_us));
    MM_RETURN_IF_ERROR(read_scalar(n, "maker_fee_bps", "paper", cfg.maker_fee_bps));
    MM_RETURN_IF_ERROR(read_scalar(n, "taker_fee_bps", "paper", cfg.taker_fee_bps));
    MM_RETURN_IF_ERROR(read_scalar(n, "queue_fill_ratio", "paper", cfg.queue_fill_ratio));
    MM_RETURN_IF_ERROR(read_scalar(n, "reject_probability", "paper", cfg.reject_probability));
    return Status::ok();
}

Status parse_monitoring(const YAML::Node& n, MonitoringConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "monitoring", {"http_host", "http_port", "snapshot_interval_ms", "enabled"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "http_host", "monitoring", cfg.http_host));
    MM_RETURN_IF_ERROR(read_scalar(n, "http_port", "monitoring", cfg.http_port));
    MM_RETURN_IF_ERROR(read_scalar(n, "snapshot_interval_ms", "monitoring", cfg.snapshot_interval_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "enabled", "monitoring", cfg.enabled));
    return Status::ok();
}

Status parse_persistence(const YAML::Node& n, PersistenceConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(
        n, "persistence",
        {"journal_dir", "state_file", "journal_flush_interval_ms", "fsync_on_flush"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "journal_dir", "persistence", cfg.journal_dir));
    MM_RETURN_IF_ERROR(read_scalar(n, "state_file", "persistence", cfg.state_file));
    MM_RETURN_IF_ERROR(
        read_scalar(n, "journal_flush_interval_ms", "persistence", cfg.journal_flush_interval_ms));
    MM_RETURN_IF_ERROR(read_scalar(n, "fsync_on_flush", "persistence", cfg.fsync_on_flush));
    return Status::ok();
}

Status parse_logging(const YAML::Node& n, LoggingConfig& cfg) {
    MM_RETURN_IF_ERROR(
        check_known_keys(n, "logging", {"level", "dir", "max_file_bytes", "max_files", "console"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "level", "logging", cfg.level));
    MM_RETURN_IF_ERROR(read_scalar(n, "dir", "logging", cfg.dir));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_file_bytes", "logging", cfg.max_file_bytes));
    MM_RETURN_IF_ERROR(read_scalar(n, "max_files", "logging", cfg.max_files));
    MM_RETURN_IF_ERROR(read_scalar(n, "console", "logging", cfg.console));
    return Status::ok();
}

Status parse_io(const YAML::Node& n, IoConfig& cfg) {
    MM_RETURN_IF_ERROR(check_known_keys(n, "io", {"md_threads", "exec_threads", "trading_cpu"}));
    MM_RETURN_IF_ERROR(read_scalar(n, "md_threads", "io", cfg.md_threads));
    MM_RETURN_IF_ERROR(read_scalar(n, "exec_threads", "io", cfg.exec_threads));
    MM_RETURN_IF_ERROR(read_scalar(n, "trading_cpu", "io", cfg.trading_cpu));
    return Status::ok();
}

}  // namespace

std::string_view to_string(TradingMode m) noexcept {
    return m == TradingMode::Live ? "live" : "paper";
}

bool parse_trading_mode(std::string_view text, TradingMode& out) noexcept {
    if (text == "paper") {
        out = TradingMode::Paper;
        return true;
    }
    if (text == "live") {
        out = TradingMode::Live;
        return true;
    }
    return false;
}

const SymbolConfig* EngineConfig::find_symbol(std::string_view symbol) const {
    for (const auto& s : symbols) {
        if (s.symbol == symbol) {
            return &s;
        }
    }
    return nullptr;
}

Status EngineConfig::validate() const {
    if (symbols.empty()) {
        return {ErrorCode::InvalidArgument, "no symbols configured"};
    }
    for (const auto& s : symbols) {
        if (s.symbol.empty()) {
            return {ErrorCode::InvalidArgument, "a symbol entry has an empty name"};
        }
        if (s.symbol.size() > Symbol::kCapacity) {
            return {ErrorCode::InvalidArgument, "symbol name is too long: " + s.symbol};
        }
    }
    {
        std::set<std::string> seen;
        for (const auto& s : symbols) {
            if (!seen.insert(s.symbol).second) {
                return {ErrorCode::InvalidArgument, "duplicate symbol: " + s.symbol};
            }
        }
    }
    if (strategy.name.empty()) {
        return {ErrorCode::InvalidArgument, "strategy.name is required"};
    }
    if (exchange.name.empty()) {
        return {ErrorCode::InvalidArgument, "exchange.name is required"};
    }
    if (strategy.budget_ns <= 0) {
        return {ErrorCode::InvalidArgument, "strategy.budget_ns must be positive"};
    }
    if (strategy.timer_interval_ms <= 0) {
        return {ErrorCode::InvalidArgument, "strategy.timer_interval_ms must be positive"};
    }
    if (strategy.max_consecutive_budget_violations <= 0 ||
        strategy.max_consecutive_invalid_outputs <= 0) {
        return {ErrorCode::InvalidArgument, "strategy fault thresholds must be positive"};
    }
    if (strategy.max_quote_distance_bps <= 0) {
        return {ErrorCode::InvalidArgument, "strategy.max_quote_distance_bps must be positive"};
    }
    {
        // Validated here rather than at first evaluation: an unrecognised mode
        // would otherwise silently fall back to a default nobody chose.
        static constexpr const char* kModes[] = {"on_book_update", "on_bbo_change", "on_timer",
                                                 "on_bbo_change_and_timer"};
        const bool known = std::any_of(std::begin(kModes), std::end(kModes),
                                       [this](const char* m) { return strategy.evaluation_mode == m; });
        if (!known) {
            return {ErrorCode::InvalidArgument,
                    "strategy.evaluation_mode is not recognised: " + strategy.evaluation_mode};
        }
    }
    if (exchange.book_depth <= 0) {
        return {ErrorCode::InvalidArgument, "exchange.book_depth must be positive"};
    }

    if (execution.max_orders_per_second <= 0 || execution.max_cancels_per_second <= 0 ||
        execution.max_messages_per_second <= 0) {
        return {ErrorCode::InvalidArgument, "execution rate limits must be positive"};
    }
    if (execution.ack_timeout_ms <= 0 || execution.cancel_timeout_ms <= 0) {
        return {ErrorCode::InvalidArgument, "execution timeouts must be positive"};
    }
    if (risk.max_active_orders_per_symbol <= 0 || risk.max_active_orders_total <= 0) {
        return {ErrorCode::InvalidArgument, "risk active-order limits must be positive"};
    }
    if (risk.max_active_orders_total < risk.max_active_orders_per_symbol) {
        return {ErrorCode::InvalidArgument,
                "risk.max_active_orders_total is below max_active_orders_per_symbol"};
    }
    if (risk.max_quote_distance_bps <= 0) {
        return {ErrorCode::InvalidArgument, "risk.max_quote_distance_bps must be positive"};
    }

    // Risk limits are never allowed to be negative in any mode. A negative
    // limit would compare as "always breached" or "never breached" depending on
    // the check, and neither is a defensible behaviour for a safety control.
    if (risk.max_position.is_negative() || risk.max_notional.is_negative() ||
        risk.max_order_qty.is_negative() || risk.max_order_notional.is_negative() ||
        risk.max_daily_loss.is_negative() || risk.max_session_loss.is_negative() ||
        risk.emergency_loss.is_negative() || risk.max_portfolio_notional.is_negative()) {
        return {ErrorCode::InvalidArgument, "risk limits must not be negative"};
    }

    if (safety.max_market_data_age_ms <= 0) {
        return {ErrorCode::InvalidArgument, "safety.max_market_data_age_ms must be positive"};
    }
    if (safety.reconcile_interval_ms <= 0) {
        return {ErrorCode::InvalidArgument, "safety.reconcile_interval_ms must be positive"};
    }
    if (monitoring.enabled && (monitoring.http_port <= 0 || monitoring.http_port > 65535)) {
        return {ErrorCode::InvalidArgument, "monitoring.http_port is out of range"};
    }
    if (paper.queue_fill_ratio < 0.0 || paper.queue_fill_ratio > 1.0) {
        return {ErrorCode::InvalidArgument, "paper.queue_fill_ratio must be in [0, 1]"};
    }
    if (paper.reject_probability < 0.0 || paper.reject_probability > 1.0) {
        return {ErrorCode::InvalidArgument, "paper.reject_probability must be in [0, 1]"};
    }
    if (io.md_threads <= 0 || io.exec_threads <= 0) {
        return {ErrorCode::InvalidArgument, "io thread counts must be positive"};
    }
    return Status::ok();
}

Status EngineConfig::validate_live_gates(bool operator_confirmed_live) const {
    if (mode != TradingMode::Live) {
        return Status::ok();
    }

    // Gate 1: the config file must opt in, separately from `mode`.
    if (!safety.live_trading_enabled) {
        return {ErrorCode::PermissionDenied,
                "mode is 'live' but safety.live_trading_enabled is false"};
    }
    // Gate 2: the operator must opt in at launch. No file alone can go live.
    if (!operator_confirmed_live) {
        return {ErrorCode::PermissionDenied,
                "live mode requires the explicit --live flag on the command line"};
    }
    // Gate 3: an unconfigured limit is not "unlimited", it is a refusal.
    if (!risk.max_position.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.max_position > 0"};
    }
    if (!risk.max_notional.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.max_notional > 0"};
    }
    if (!risk.max_order_qty.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.max_order_qty > 0"};
    }
    if (!risk.max_order_notional.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.max_order_notional > 0"};
    }
    if (!risk.max_daily_loss.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.max_daily_loss > 0"};
    }
    if (!risk.emergency_loss.is_positive()) {
        return {ErrorCode::PermissionDenied, "live mode requires risk.emergency_loss > 0"};
    }
    // Gate 4: the emergency threshold must sit above the daily limit, or the
    // ordinary halt would never fire before the emergency one.
    if (risk.emergency_loss < risk.max_daily_loss) {
        return {ErrorCode::InvalidArgument,
                "risk.emergency_loss must be at least risk.max_daily_loss"};
    }
    // Gate 5: credentials must be sourced from a named environment prefix, so
    // the adapter has somewhere to look that is not this file.
    if (exchange.credentials_env_prefix.empty()) {
        return {ErrorCode::PermissionDenied,
                "live mode requires exchange.credentials_env_prefix"};
    }
    // Gate 6: at least one symbol must actually be enabled.
    const bool any_enabled =
        std::any_of(symbols.begin(), symbols.end(), [](const SymbolConfig& s) { return s.enabled; });
    if (!any_enabled) {
        return {ErrorCode::InvalidArgument, "live mode requires at least one enabled symbol"};
    }
    return Status::ok();
}

Result<EngineConfig> load_config_string(const std::string& yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        return Status{ErrorCode::ParseError, std::string("YAML parse error: ") + e.what()};
    }
    if (!root || !root.IsMap()) {
        return Status{ErrorCode::ParseError, "config root must be a map"};
    }

    MM_RETURN_IF_ERROR_RESULT(check_known_keys(
        root, "",
        {"mode", "session_name", "exchange", "symbols", "strategy", "risk", "execution", "safety",
         "paper", "monitoring", "persistence", "logging", "io", "strategies"}));

    EngineConfig cfg;

    std::string mode_text = "paper";
    MM_RETURN_IF_ERROR_RESULT(read_scalar(root, "mode", "", mode_text));
    if (!parse_trading_mode(mode_text, cfg.mode)) {
        return Status{ErrorCode::InvalidArgument,
                      "mode must be 'paper' or 'live', got '" + mode_text + "'"};
    }
    MM_RETURN_IF_ERROR_RESULT(read_scalar(root, "session_name", "", cfg.session_name));

    MM_RETURN_IF_ERROR_RESULT(parse_exchange(root["exchange"], cfg.exchange));
    MM_RETURN_IF_ERROR_RESULT(parse_symbols(root["symbols"], cfg.symbols));
    MM_RETURN_IF_ERROR_RESULT(parse_strategy(root["strategy"], cfg.strategy));
    MM_RETURN_IF_ERROR_RESULT(parse_strategy_params(root["strategies"], cfg.strategy_params));
    // Merge the selected strategy's block into its parameters. Doing it here
    // means the runtime receives one flat set and never has to know that two
    // sources existed.
    {
        const auto it = cfg.strategy_params.find(cfg.strategy.name);
        if (it != cfg.strategy_params.end()) {
            for (const std::string& key : it->second.keys()) {
                const auto value = it->second.get_string(key);
                if (value.is_ok()) {
                    cfg.strategy.params.set(key, value.value());
                }
            }
        }
    }
    MM_RETURN_IF_ERROR_RESULT(parse_risk(root["risk"], cfg.risk));
    MM_RETURN_IF_ERROR_RESULT(parse_execution(root["execution"], cfg.execution));
    MM_RETURN_IF_ERROR_RESULT(parse_safety(root["safety"], cfg.safety));
    MM_RETURN_IF_ERROR_RESULT(parse_paper(root["paper"], cfg.paper));
    MM_RETURN_IF_ERROR_RESULT(parse_monitoring(root["monitoring"], cfg.monitoring));
    MM_RETURN_IF_ERROR_RESULT(parse_persistence(root["persistence"], cfg.persistence));
    MM_RETURN_IF_ERROR_RESULT(parse_logging(root["logging"], cfg.logging));
    MM_RETURN_IF_ERROR_RESULT(parse_io(root["io"], cfg.io));

    MM_RETURN_IF_ERROR_RESULT(cfg.validate());
    return cfg;
}

Result<EngineConfig> load_config_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return Status{ErrorCode::NotFound, "cannot open config file: " + path};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return load_config_string(buffer.str());
}

}  // namespace mm
