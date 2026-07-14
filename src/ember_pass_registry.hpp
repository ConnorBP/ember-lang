// ember_pass_registry.hpp — Stage C: pass discovery & wiring (the extension half).
//
// The registry maps pass names to factories (name → unique_ptr<PassConcept>).
// This is the "BindingBuilder::add" equivalent — one call per pass. Each
// extension lib provides a register_passes(EmberPassRegistry&) function the
// same way it provides register_natives(NativeTable&):
//
//   // extensions/opt/ext_opt.cpp
//   void register_passes(EmberPassRegistry& reg) {
//       reg.add<ConstPropPass>("constprop");
//       reg.add<CSEPass>("cse");
//   }
//
// The host wires it:
//   EmberPassRegistry pass_reg;
//   ext_opt::register_passes(pass_reg);
//
// Configured (collision-aware) registration:
//   reg.add_factory("mba_expand", [options]() {
//       return ember::make_pass_concept(MBAExpansionPass{options});
//   });
//
// Strict registration policy:
//   - empty names are rejected (not stored);
//   - null/empty std::function factories are rejected (not stored);
//   - duplicate names are rejected without replacing the first registration;
//   - names() is deterministic (sorted lexicographically).
//
// Legacy `reg.add<PassT>("name")` call sites remain source-compatible: the
// returned ExtensionStatus may be ignored, and the default-constructed pass
// factory remains compatible with existing passes.
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §4.
// Polymorphic engine ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.3.

#pragma once

#include "ember_pass.hpp"
#include "extension_registry.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// A pass factory: creates a fresh unique_ptr<PassConcept> for a named pass.
// A *configured* factory captures immutable constructor options by value and
// returns a fresh PassConcept instance on every invocation, so two create()
// calls for the same name yield distinct pass objects.
using PassFactory = std::function<std::unique_ptr<PassConcept>()>;

// Wrap a concrete pass value into a type-erased PassConcept. Used by configured
// factories: `make_pass_concept(MyPass{options})`. The returned model owns the
// pass by value, so the captured options travel with each fresh instance.
template <class PassT>
std::unique_ptr<PassConcept> make_pass_concept(PassT pass) {
    return std::make_unique<PassModel<PassT>>(std::move(pass));
}

class EmberPassRegistry {
public:
    // The registry identity carried in ExtensionError.registry for diagnostics.
    static constexpr const char* registry_id = "ember-pass";

    // Register a default-constructed pass by name. Source-compatible with the
    // original `reg.add<PassT>("name")` call sites: callers may ignore the
    // returned status. Strict registration: rejects empty names and duplicate
    // names (the first registration is retained, not replaced). The generated
    // factory is never null.
    template <typename PassT>
    ExtensionStatus add(std::string name) {
        if (name.empty()) {
            return make_extension_error(registry_id, name,
                                        "pass name must not be empty");
        }
        if (factories_.find(name) != factories_.end()) {
            return make_extension_error(registry_id, name,
                                        "pass name already registered");
        }
        factories_.emplace(std::move(name),
                           []() -> std::unique_ptr<PassConcept> {
                               return std::make_unique<PassModel<PassT>>(PassT{});
                           });
        return make_extension_ok();
    }

    // Register a configured pass factory by name. Strict registration: rejects
    // empty names, null/empty std::function factories, and duplicate names
    // (the first registration is retained, not replaced). A configured factory
    // captures immutable constructor options and creates a fresh PassConcept
    // instance on every create().
    ExtensionStatus add_factory(std::string name, PassFactory factory) {
        if (name.empty()) {
            return make_extension_error(registry_id, name,
                                        "pass name must not be empty");
        }
        if (!factory) {
            return make_extension_error(registry_id, name,
                                        "pass factory must not be null");
        }
        if (factories_.find(name) != factories_.end()) {
            return make_extension_error(registry_id, name,
                                        "pass name already registered");
        }
        factories_.emplace(std::move(name), std::move(factory));
        return make_extension_ok();
    }

    // Look up a pass by name. Returns nullptr if not found. Each call yields a
    // fresh PassConcept instance produced by the registered factory.
    std::unique_ptr<PassConcept> create(const char* name) const {
        auto it = factories_.find(name);
        if (it == factories_.end()) return nullptr;
        return it->second();
    }

    // Is a pass registered under this name?
    bool has(const char* name) const {
        return factories_.find(name) != factories_.end();
    }

    // List all registered pass names (for --list-passes). Deterministic: the
    // result is sorted lexicographically so output does not depend on
    // unordered_map iteration order.
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(factories_.size());
        for (const auto& [k, _] : factories_) out.push_back(k);
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    std::unordered_map<std::string, PassFactory> factories_;
};

} // namespace ember
