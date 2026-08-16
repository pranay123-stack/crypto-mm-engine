#pragma once

/// \file Params.hpp
/// Opaque, typed parameter bag handed to a strategy.
///
/// Strategy parameters are chosen by the research platform and arrive as a YAML
/// block the engine never interprets. Passing the `YAML::Node` straight through
/// would make every strategy link yaml-cpp and would let a strategy reach the
/// rest of the config file. Instead the loader flattens that block into dotted
/// keys with primitive values, so `strategies/` depends on nothing but
/// `common/` -- which is what keeps a strategy a leaf in the module graph.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mm/common/Fixed.hpp"
#include "mm/common/Status.hpp"

namespace mm {

class Params {
public:
    void set(std::string key, std::string value) { values_.insert_or_assign(std::move(key), std::move(value)); }

    [[nodiscard]] bool has(std::string_view key) const;

    /// Every getter has a required and a defaulted form. The required form
    /// returns an error a strategy must surface at construction time -- a
    /// mistyped parameter name should stop the engine from starting, not
    /// silently trade with a zero spread.
    [[nodiscard]] Result<std::string> get_string(std::string_view key) const;
    [[nodiscard]] Result<double> get_double(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> get_int(std::string_view key) const;
    [[nodiscard]] Result<bool> get_bool(std::string_view key) const;
    [[nodiscard]] Result<Px> get_px(std::string_view key) const;
    [[nodiscard]] Result<Qty> get_qty(std::string_view key) const;

    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback) const;
    [[nodiscard]] double double_or(std::string_view key, double fallback) const;
    [[nodiscard]] std::int64_t int_or(std::string_view key, std::int64_t fallback) const;
    [[nodiscard]] bool bool_or(std::string_view key, bool fallback) const;

    [[nodiscard]] std::vector<std::string> keys() const;
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    /// Stable, sorted rendering for the journal and the dashboard: the exact
    /// parameter set a session ran with is part of its audit record.
    [[nodiscard]] std::string to_string() const;

private:
    std::map<std::string, std::string, std::less<>> values_;
};

}  // namespace mm
