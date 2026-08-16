#include "mm/common/Params.hpp"

#include <charconv>

namespace mm {
namespace {

Status missing(std::string_view key) {
    std::string msg = "missing required parameter '";
    msg.append(key);
    msg.push_back('\'');
    return {ErrorCode::NotFound, msg};
}

Status bad_value(std::string_view key, std::string_view value, std::string_view expected) {
    std::string msg = "parameter '";
    msg.append(key).append("' = '").append(value).append("' is not a valid ").append(expected);
    return {ErrorCode::ParseError, msg};
}

}  // namespace

bool Params::has(std::string_view key) const { return values_.find(key) != values_.end(); }

Result<std::string> Params::get_string(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    return it->second;
}

Result<double> Params::get_double(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    // std::from_chars for double is the only locale-independent parse in the
    // standard library; strtod would honour LC_NUMERIC and read "1.5" as 1 in a
    // comma-decimal locale.
    double out = 0.0;
    const char* first = it->second.data();
    const char* last = first + it->second.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last) {
        return bad_value(key, it->second, "number");
    }
    return out;
}

Result<std::int64_t> Params::get_int(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    std::int64_t out = 0;
    const char* first = it->second.data();
    const char* last = first + it->second.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last) {
        return bad_value(key, it->second, "integer");
    }
    return out;
}

Result<bool> Params::get_bool(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    const std::string& v = it->second;
    if (v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "on" || v == "1") {
        return true;
    }
    if (v == "false" || v == "False" || v == "FALSE" || v == "no" || v == "off" || v == "0") {
        return false;
    }
    return bad_value(key, v, "boolean");
}

Result<Px> Params::get_px(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    Px out;
    if (!Px::parse(it->second, out)) {
        return bad_value(key, it->second, "price");
    }
    return out;
}

Result<Qty> Params::get_qty(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return missing(key);
    }
    Qty out;
    if (!Qty::parse(it->second, out)) {
        return bad_value(key, it->second, "quantity");
    }
    return out;
}

std::string Params::string_or(std::string_view key, std::string fallback) const {
    const auto r = get_string(key);
    return r.is_ok() ? r.value() : std::move(fallback);
}

double Params::double_or(std::string_view key, double fallback) const {
    const auto r = get_double(key);
    return r.is_ok() ? r.value() : fallback;
}

std::int64_t Params::int_or(std::string_view key, std::int64_t fallback) const {
    const auto r = get_int(key);
    return r.is_ok() ? r.value() : fallback;
}

bool Params::bool_or(std::string_view key, bool fallback) const {
    const auto r = get_bool(key);
    return r.is_ok() ? r.value() : fallback;
}

std::vector<std::string> Params::keys() const {
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& [k, v] : values_) {
        out.push_back(k);
    }
    return out;
}

std::string Params::to_string() const {
    std::string out;
    out.push_back('{');
    bool first = true;
    for (const auto& [k, v] : values_) {
        if (!first) {
            out.append(", ");
        }
        first = false;
        out.append(k).push_back('=');
        out.append(v);
    }
    out.push_back('}');
    return out;
}

}  // namespace mm
