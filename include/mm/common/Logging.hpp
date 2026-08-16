#pragma once

/// \file Logging.hpp
/// Structured, asynchronous logging.
///
/// The trading thread must never block on a disk write. Logging therefore runs
/// through spdlog's asynchronous logger with an **overrun_oldest** overflow
/// policy: when the queue is full the oldest record is dropped rather than the
/// producer being made to wait. Dropping a log line degrades observability;
/// blocking the trading thread on a slow disk degrades the market maker's
/// position. The drop count is itself exported as a metric, so the degradation
/// is visible rather than silent.

#include <string>
#include <string_view>

#include "mm/common/Config.hpp"
#include "mm/common/Status.hpp"

// spdlog is included here because the logging macros expand at call sites; it
// is the one third-party header the core modules see, and it reaches no
// further than logging.
#include <spdlog/spdlog.h>

namespace mm::log {

/// The event taxonomy from docs/architecture.md. Every structured record
/// carries exactly one of these, so an incident investigation can filter the
/// journal by event class without parsing free text.
enum class EventType : std::uint8_t {
    MarketData = 0,
    BookSync,
    BookResync,
    StrategyDecision,
    QuoteUpdate,
    QuoteSuppressed,
    RiskApproval,
    RiskRejection,
    OrderSubmitted,
    OrderAck,
    OrderCancel,
    OrderReplace,
    OrderReject,
    Fill,
    PositionChange,
    PnlChange,
    Reconciliation,
    KillSwitch,
    SafetyHalt,
    Connectivity,
    Lifecycle,
    Error,
};

[[nodiscard]] std::string_view to_string(EventType t) noexcept;

/// Idempotent. Creates the log directory, installs the rotating file sink and
/// (optionally) a console sink, and starts the background writer thread.
[[nodiscard]] Status init(const LoggingConfig& cfg, std::string_view session_name);

/// Flushes and stops the background thread. Safe to call without a prior init.
void shutdown();

/// Records dropped because the asynchronous queue was full.
[[nodiscard]] std::uint64_t dropped_records() noexcept;

/// True once `init` has succeeded; before that, macros fall back to stderr so
/// early-startup failures are never invisible.
[[nodiscard]] bool is_initialized() noexcept;

/// Emit one structured record. `fields` is a pre-formatted `key=value` string;
/// the macros below build it.
void emit(EventType type, spdlog::level::level_enum level, std::string_view message,
          std::string_view fields);

}  // namespace mm::log

// ---------------------------------------------------------------------------
// Macros
//
// Formatting is deferred behind a level check so a disabled debug record costs
// one predictable branch, not an argument-formatting pass.
// ---------------------------------------------------------------------------
#define MM_LOG_AT(level_enum, event_type, msg, ...)                                        \
    do {                                                                                   \
        if (::spdlog::should_log(level_enum)) {                                            \
            ::mm::log::emit((event_type), (level_enum), (msg),                              \
                            ::fmt::format(__VA_ARGS__));                                   \
        }                                                                                  \
    } while (false)

#define MM_LOG_TRACE(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::trace, event_type, msg, __VA_ARGS__)
#define MM_LOG_DEBUG(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::debug, event_type, msg, __VA_ARGS__)
#define MM_LOG_INFO(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::info, event_type, msg, __VA_ARGS__)
#define MM_LOG_WARN(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::warn, event_type, msg, __VA_ARGS__)
#define MM_LOG_ERROR(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::err, event_type, msg, __VA_ARGS__)
#define MM_LOG_CRITICAL(event_type, msg, ...) \
    MM_LOG_AT(::spdlog::level::critical, event_type, msg, __VA_ARGS__)

/// Records with no structured fields.
#define MM_LOG_PLAIN(level_enum, event_type, msg)                     \
    do {                                                             \
        if (::spdlog::should_log(level_enum)) {                      \
            ::mm::log::emit((event_type), (level_enum), (msg), {});  \
        }                                                            \
    } while (false)
