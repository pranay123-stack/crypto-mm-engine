#pragma once

/// \file Status.hpp
/// Error propagation without exceptions.
///
/// The trading thread does not unwind. Every fallible operation on or near the
/// hot path returns `Status` or `Result<T>`, so a failure is a value the caller
/// is forced to look at rather than a control-flow event that can silently
/// escape a handler and leave the engine in an unknown state.

#include <optional>
#include <string_view>
#include <utility>

#include "mm/common/InlineString.hpp"

namespace mm {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    InvalidArgument,     ///< caller passed something structurally wrong
    FailedPrecondition,  ///< system is not in a state where this is meaningful
    NotFound,
    AlreadyExists,
    OutOfRange,
    PermissionDenied,  ///< e.g. live trading attempted without the live gates
    Unavailable,       ///< transient: disconnected, rate limited, venue down
    Timeout,
    ParseError,      ///< malformed venue message or config
    Rejected,        ///< a counterparty or internal component said no
    Overflow,        ///< a bounded buffer or numeric range was exceeded
    Unsafe,          ///< refused because the system is not in a safe state
    Internal,        ///< invariant violated; a bug, not an environment problem
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

class Status {
public:
    constexpr Status() noexcept = default;

    Status(ErrorCode code, std::string_view message) noexcept : code_(code) {
        static_cast<void>(message_.assign(message));
    }

    explicit Status(ErrorCode code) noexcept : code_(code) {}

    [[nodiscard]] static Status ok() noexcept { return Status{}; }

    [[nodiscard]] constexpr bool is_ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] constexpr bool is_error() const noexcept { return code_ != ErrorCode::Ok; }
    [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr std::string_view message() const noexcept { return message_.view(); }

    /// "Unavailable: user data stream closed"
    [[nodiscard]] std::string to_string() const;

private:
    ErrorCode code_ = ErrorCode::Ok;
    InlineString<160> message_{};
};

/// A value or the reason there isn't one.
template <class T>
class Result {
public:
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)  // NOLINT(*-explicit-*)
        : value_(std::move(value)) {}

    Result(Status status) noexcept : status_(status) {  // NOLINT(*-explicit-*)
        // A "successful failure" is always a bug at the call site.
        if (status.is_ok()) {
            status_ = Status(ErrorCode::Internal, "Result constructed from an ok Status");
        }
    }

    Result(ErrorCode code, std::string_view message) noexcept : status_(code, message) {}

    [[nodiscard]] bool is_ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] bool is_error() const noexcept { return !value_.has_value(); }
    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

    /// Precondition: `is_ok()`. Callers must check first; there is no throwing
    /// accessor, because a throw here would cross the no-unwind boundary.
    [[nodiscard]] const T& value() const noexcept { return *value_; }
    [[nodiscard]] T& value() noexcept { return *value_; }

    [[nodiscard]] T value_or(T fallback) const {
        return value_.has_value() ? *value_ : std::move(fallback);
    }

private:
    std::optional<T> value_{};
    Status status_{};
};

}  // namespace mm

/// Early-return on error. Mirrors the shape of the code it replaces closely
/// enough that the happy path stays readable.
#define MM_RETURN_IF_ERROR(expr)             \
    do {                                     \
        const ::mm::Status _mm_st = (expr);  \
        if (_mm_st.is_error()) {             \
            return _mm_st;                   \
        }                                    \
    } while (false)

/// Same, for a function returning `Result<T>`: the error travels as a Status
/// while the success type stays whatever the caller declared.
#define MM_RETURN_IF_ERROR_RESULT(expr)      \
    do {                                     \
        const ::mm::Status _mm_st = (expr);  \
        if (_mm_st.is_error()) {             \
            return _mm_st;                   \
        }                                    \
    } while (false)
