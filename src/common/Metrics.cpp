#include "mm/common/Metrics.hpp"

#include <algorithm>
#include <sstream>

namespace mm {
namespace {

/// The metric family name, i.e. everything before the label block. Used to
/// attach a single HELP line to every labelled series of the same family.
std::string_view family_of(std::string_view key) {
    const auto brace = key.find('{');
    return brace == std::string_view::npos ? key : key.substr(0, brace);
}

void escape_label_value(const std::string& in, std::string& out) {
    for (const char c : in) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"':  out.append("\\\""); break;
            case '\n': out.append("\\n");  break;
            default:   out.push_back(c);   break;
        }
    }
}

}  // namespace

std::string MetricsRegistry::key_of(std::string_view name, const Labels& labels) {
    std::string key(name);
    if (labels.empty()) {
        return key;
    }
    // Sorted so the same label set always yields the same key regardless of the
    // order the caller listed them in.
    Labels sorted = labels;
    std::sort(sorted.begin(), sorted.end());

    key.push_back('{');
    bool first = true;
    for (const auto& [k, v] : sorted) {
        if (!first) {
            key.push_back(',');
        }
        first = false;
        key.append(k).append("=\"");
        escape_label_value(v, key);
        key.push_back('"');
    }
    key.push_back('}');
    return key;
}

Counter& MetricsRegistry::counter(std::string_view name, const Labels& labels) {
    const std::string key = key_of(name, labels);
    const std::lock_guard<std::mutex> guard(mutex_);
    auto it = counters_.find(key);
    if (it == counters_.end()) {
        it = counters_.emplace(key, std::make_unique<Counter>()).first;
    }
    return *it->second;
}

Gauge& MetricsRegistry::gauge(std::string_view name, const Labels& labels) {
    const std::string key = key_of(name, labels);
    const std::lock_guard<std::mutex> guard(mutex_);
    auto it = gauges_.find(key);
    if (it == gauges_.end()) {
        it = gauges_.emplace(key, std::make_unique<Gauge>()).first;
    }
    return *it->second;
}

void MetricsRegistry::describe(std::string_view name, std::string_view help) {
    const std::lock_guard<std::mutex> guard(mutex_);
    help_.insert_or_assign(std::string(name), std::string(help));
}

std::string MetricsRegistry::render_prometheus() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::ostringstream out;

    std::string last_family;
    const auto emit_header = [&](std::string_view key, const char* type) {
        const std::string family(family_of(key));
        if (family == last_family) {
            return;
        }
        last_family = family;
        const auto h = help_.find(family);
        if (h != help_.end()) {
            out << "# HELP " << family << ' ' << h->second << '\n';
        }
        out << "# TYPE " << family << ' ' << type << '\n';
    };

    for (const auto& [key, metric] : counters_) {
        emit_header(key, "counter");
        out << key << ' ' << metric->value() << '\n';
    }
    last_family.clear();
    for (const auto& [key, metric] : gauges_) {
        emit_header(key, "gauge");
        out << key << ' ' << metric->value() << '\n';
    }
    return out.str();
}

std::size_t MetricsRegistry::size() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return counters_.size() + gauges_.size();
}

void MetricsRegistry::reset_all() {
    const std::lock_guard<std::mutex> guard(mutex_);
    for (auto& [key, metric] : counters_) {
        metric->reset();
    }
    for (auto& [key, metric] : gauges_) {
        metric->set(0);
    }
}

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

}  // namespace mm
