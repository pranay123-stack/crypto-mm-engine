#include "mm/common/Time.hpp"

#include <array>
#include <cstdio>
#include <ctime>

namespace mm {

Nanos steady_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<Nanos>(ts.tv_sec) * kNanosPerSecond + static_cast<Nanos>(ts.tv_nsec);
}

Nanos wall_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<Nanos>(ts.tv_sec) * kNanosPerSecond + static_cast<Nanos>(ts.tv_nsec);
}

std::string format_wall_iso8601(Nanos wall_nanos) {
    const std::time_t secs = static_cast<std::time_t>(wall_nanos / kNanosPerSecond);
    const auto millis_part = static_cast<int>((wall_nanos % kNanosPerSecond) / kNanosPerMilli);

    std::tm tm_utc{};
    ::gmtime_r(&secs, &tm_utc);

    std::array<char, 40> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, millis_part);
    if (n <= 0) {
        return {};
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

SystemClock& SystemClock::instance() noexcept {
    static SystemClock clock;
    return clock;
}

}  // namespace mm
