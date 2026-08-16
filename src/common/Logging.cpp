#include "mm/common/Logging.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace mm::log {
namespace {

/// Sized so a burst of quote churn does not reach the overflow policy under any
/// realistic message rate: 64k records at ~200 bytes is ~13 MB of headroom.
constexpr std::size_t kQueueSize = 65'536;
constexpr std::size_t kWriterThreads = 1;

std::atomic<bool> g_initialized{false};
std::atomic<std::uint64_t> g_dropped{0};
std::shared_ptr<spdlog::logger> g_logger;
std::mutex g_init_mutex;

spdlog::level::level_enum parse_level(const std::string& text) {
    const auto lvl = spdlog::level::from_str(text);
    // from_str returns `off` for anything unrecognised, which would silence the
    // engine entirely. A bad level string must not be a way to lose all logs.
    if (lvl == spdlog::level::off && text != "off") {
        return spdlog::level::info;
    }
    return lvl;
}

}  // namespace

std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::MarketData:       return "MARKET_DATA";
        case EventType::BookSync:         return "BOOK_SYNC";
        case EventType::BookResync:       return "BOOK_RESYNC";
        case EventType::StrategyDecision: return "STRATEGY_DECISION";
        case EventType::QuoteUpdate:      return "QUOTE_UPDATE";
        case EventType::QuoteSuppressed:  return "QUOTE_SUPPRESSED";
        case EventType::RiskApproval:     return "RISK_APPROVAL";
        case EventType::RiskRejection:    return "RISK_REJECTION";
        case EventType::OrderSubmitted:   return "ORDER_SUBMITTED";
        case EventType::OrderAck:         return "ORDER_ACK";
        case EventType::OrderCancel:      return "ORDER_CANCEL";
        case EventType::OrderReplace:     return "ORDER_REPLACE";
        case EventType::OrderReject:      return "ORDER_REJECT";
        case EventType::Fill:             return "FILL";
        case EventType::PositionChange:   return "POSITION_CHANGE";
        case EventType::PnlChange:        return "PNL_CHANGE";
        case EventType::Reconciliation:   return "RECONCILIATION";
        case EventType::KillSwitch:       return "KILL_SWITCH";
        case EventType::SafetyHalt:       return "SAFETY_HALT";
        case EventType::Connectivity:     return "CONNECTIVITY";
        case EventType::Lifecycle:        return "LIFECYCLE";
        case EventType::Error:            return "ERROR";
    }
    return "UNKNOWN";
}

Status init(const LoggingConfig& cfg, std::string_view session_name) {
    const std::lock_guard<std::mutex> guard(g_init_mutex);
    if (g_initialized.load(std::memory_order_acquire)) {
        return Status::ok();
    }

    std::error_code ec;
    std::filesystem::create_directories(cfg.dir, ec);
    if (ec) {
        return {ErrorCode::Unavailable, "cannot create log directory: " + ec.message()};
    }

    try {
        spdlog::init_thread_pool(kQueueSize, kWriterThreads);

        std::vector<spdlog::sink_ptr> sinks;
        const std::string file = cfg.dir + "/mm-" + std::string(session_name) + ".log";
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file, static_cast<std::size_t>(cfg.max_file_bytes),
            static_cast<std::size_t>(cfg.max_files)));
        if (cfg.console) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        g_logger = std::make_shared<spdlog::async_logger>(
            "mm", sinks.begin(), sinks.end(), spdlog::thread_pool(),
            // Never block the producer: a full queue drops the oldest record.
            spdlog::async_overflow_policy::overrun_oldest);

        // Machine-parseable and stable: timestamp, level, thread, then the
        // record. Incident tooling greps this, so the shape is part of the
        // contract, not a cosmetic choice.
        g_logger->set_pattern("%Y-%m-%dT%H:%M:%S.%eZ %^%-8l%$ [%t] %v");
        g_logger->set_level(parse_level(cfg.level));
        g_logger->flush_on(spdlog::level::err);

        spdlog::set_default_logger(g_logger);
        spdlog::set_level(parse_level(cfg.level));
        spdlog::flush_every(std::chrono::seconds(1));
    } catch (const std::exception& e) {
        return {ErrorCode::Internal, std::string("logger init failed: ") + e.what()};
    }

    g_initialized.store(true, std::memory_order_release);
    return Status::ok();
}

void shutdown() {
    const std::lock_guard<std::mutex> guard(g_init_mutex);
    if (!g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    if (g_logger) {
        g_logger->flush();
    }
    spdlog::shutdown();
    g_logger.reset();
    g_initialized.store(false, std::memory_order_release);
}

std::uint64_t dropped_records() noexcept { return g_dropped.load(std::memory_order_relaxed); }

bool is_initialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

void emit(EventType type, spdlog::level::level_enum level, std::string_view message,
          std::string_view fields) {
    if (!g_initialized.load(std::memory_order_acquire)) {
        // Before the logger exists, startup errors still have to be visible --
        // a configuration failure that logs nowhere looks like a silent hang.
        if (level >= spdlog::level::warn) {
            std::fprintf(stderr, "[pre-init] %s %.*s %.*s\n", to_string(type).data(),
                         static_cast<int>(message.size()), message.data(),
                         static_cast<int>(fields.size()), fields.data());
        }
        return;
    }
    if (fields.empty()) {
        g_logger->log(level, "{} {}", to_string(type), message);
    } else {
        g_logger->log(level, "{} {} {}", to_string(type), message, fields);
    }
}

}  // namespace mm::log
