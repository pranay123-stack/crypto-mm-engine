#include "mm/strategy/StrategyRegistry.hpp"

#include <algorithm>

namespace mm::strategy {

StrategyRegistry& StrategyRegistry::instance() {
    // Function-local static: constructed on first use, which is what makes
    // static-initialization-order safe for translation units that register from
    // their own static initializers.
    static StrategyRegistry registry;
    return registry;
}

bool StrategyRegistry::register_strategy(std::string_view name, std::int32_t version,
                                         std::string_view description, Factory factory) {
    if (name.empty() || version <= 0 || !factory) {
        return false;
    }
    if (contains(name)) {
        // Two strategies sharing a name would make the config ambiguous, and
        // which one won would depend on link order.
        return false;
    }
    entries_.push_back(
        Entry{StrategyDescriptor{std::string(name), version, std::string(description)},
              std::move(factory)});
    return true;
}

Result<StrategyPtr> StrategyRegistry::create(std::string_view name) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [name](const Entry& e) {
        return e.descriptor.name == name;
    });
    if (it == entries_.end()) {
        // Listing what is available turns a config typo into a one-line fix
        // rather than a hunt through the source tree.
        std::string msg = "no strategy named '";
        msg.append(name).append("'. Registered: ");
        if (entries_.empty()) {
            msg.append("(none - is the strategies object library linked?)");
        } else {
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (i > 0) {
                    msg.append(", ");
                }
                msg.append(entries_[i].descriptor.name);
            }
        }
        return Status{ErrorCode::NotFound, msg};
    }
    StrategyPtr instance = it->factory();
    if (!instance) {
        return Status{ErrorCode::Internal, "strategy factory returned nothing"};
    }
    return instance;
}

Status StrategyRegistry::check_version(std::string_view name,
                                       std::int32_t expected_version) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [name](const Entry& e) {
        return e.descriptor.name == name;
    });
    if (it == entries_.end()) {
        return {ErrorCode::NotFound, std::string("no strategy named '").append(name) + "'"};
    }
    if (expected_version != 0 && it->descriptor.version != expected_version) {
        // A config asking for v2 against a binary containing v1 is a deployment
        // mistake. Catching it at startup costs seconds; catching it in the
        // journal costs an investigation.
        std::string msg = "strategy '";
        msg.append(name)
            .append("' is version ")
            .append(std::to_string(it->descriptor.version))
            .append(" but the configuration asks for version ")
            .append(std::to_string(expected_version));
        return {ErrorCode::FailedPrecondition, msg};
    }
    return Status::ok();
}

bool StrategyRegistry::contains(std::string_view name) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [name](const Entry& e) { return e.descriptor.name == name; });
}

std::vector<StrategyDescriptor> StrategyRegistry::list() const {
    std::vector<StrategyDescriptor> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) {
        out.push_back(e.descriptor);
    }
    // Stable ordering so startup logs and `--list-strategies` do not depend on
    // link order.
    std::sort(out.begin(), out.end(), [](const StrategyDescriptor& a, const StrategyDescriptor& b) {
        return a.name < b.name;
    });
    return out;
}

std::size_t StrategyRegistry::size() const { return entries_.size(); }

}  // namespace mm::strategy
