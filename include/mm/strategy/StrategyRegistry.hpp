#pragma once

/// \file StrategyRegistry.hpp
/// Name -> strategy, resolved at startup.
///
/// The engine depends on `IStrategy` and on this registry; it never names a
/// concrete strategy. Selecting one is a configuration change, not a code
/// change, and there is no switch statement anywhere that enumerates them.
///
/// Registration is compile-time and self-service: a strategy adds one
/// `MM_REGISTER_STRATEGY` line in its own translation unit. No reflection, no
/// dynamic loading, no plug-in ABI — those buy flexibility this system does not
/// need and cost a class of failure it should not have.
///
/// **The linker caveat.** A self-registering translation unit that nothing
/// references is dead-stripped from a static library, and the strategy silently
/// disappears from the registry. `strategies/` is therefore built as a CMake
/// OBJECT library, whose objects are linked whole. `tests/strategy_swap/`
/// asserts the registry is actually populated, so a regression here fails a
/// test rather than a deployment.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mm/common/Status.hpp"
#include "mm/strategy/IStrategy.hpp"

namespace mm::strategy {

struct StrategyDescriptor {
    std::string name;
    std::int32_t version = 0;
    /// One line, for `mm_ctl --list-strategies` and startup logs.
    std::string description;
};

class StrategyRegistry {
public:
    using Factory = std::function<StrategyPtr()>;

    [[nodiscard]] static StrategyRegistry& instance();

    /// Returns false on a duplicate name. Called during static initialization,
    /// where throwing or aborting would be untraceable; `create` reports the
    /// resulting absence instead.
    bool register_strategy(std::string_view name, std::int32_t version,
                           std::string_view description, Factory factory);

    /// Constructs a strategy by name. Not found is an error listing what is
    /// available — a typo in the config should say so, not fail obscurely.
    [[nodiscard]] Result<StrategyPtr> create(std::string_view name) const;

    /// Verifies that the configured version matches the registered one. A
    /// config asking for v2 while the binary contains v1 is a deployment
    /// mistake, and it is far cheaper to catch at startup than in the journal.
    [[nodiscard]] Status check_version(std::string_view name, std::int32_t expected_version) const;

    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::vector<StrategyDescriptor> list() const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        StrategyDescriptor descriptor;
        Factory factory;
    };
    std::vector<Entry> entries_;
};

}  // namespace mm::strategy

/// Registers a strategy. Place once, at namespace scope, in the strategy's own
/// .cpp file. `Type` may be fully qualified.
#define MM_REGISTER_STRATEGY_IMPL(Type, name_literal, version_number, description_literal, tag) \
    namespace {                                                                                 \
    const bool k_mm_strategy_registered_##tag =                                                 \
        ::mm::strategy::StrategyRegistry::instance().register_strategy(                         \
            name_literal, version_number, description_literal,                                  \
            [] { return ::mm::strategy::StrategyPtr(std::make_unique<Type>()); });              \
    }  // namespace

#define MM_REGISTER_STRATEGY_EXPAND(Type, name_literal, version_number, description_literal, tag) \
    MM_REGISTER_STRATEGY_IMPL(Type, name_literal, version_number, description_literal, tag)

#define MM_REGISTER_STRATEGY(Type, name_literal, version_number, description_literal) \
    MM_REGISTER_STRATEGY_EXPAND(Type, name_literal, version_number, description_literal, __LINE__)
