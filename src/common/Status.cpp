#include "mm/common/Status.hpp"

namespace mm {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                 return "Ok";
        case ErrorCode::InvalidArgument:    return "InvalidArgument";
        case ErrorCode::FailedPrecondition: return "FailedPrecondition";
        case ErrorCode::NotFound:           return "NotFound";
        case ErrorCode::AlreadyExists:      return "AlreadyExists";
        case ErrorCode::OutOfRange:         return "OutOfRange";
        case ErrorCode::PermissionDenied:   return "PermissionDenied";
        case ErrorCode::Unavailable:        return "Unavailable";
        case ErrorCode::Timeout:            return "Timeout";
        case ErrorCode::ParseError:         return "ParseError";
        case ErrorCode::Rejected:           return "Rejected";
        case ErrorCode::Overflow:           return "Overflow";
        case ErrorCode::Unsafe:             return "Unsafe";
        case ErrorCode::Internal:           return "Internal";
    }
    return "Unknown";
}

std::string Status::to_string() const {
    std::string out(::mm::to_string(code_));
    if (!message_.empty()) {
        out += ": ";
        out.append(message_.view());
    }
    return out;
}

}  // namespace mm
