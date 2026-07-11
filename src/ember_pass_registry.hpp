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
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §4.

#pragma once

#include "ember_pass.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// A pass factory: creates a fresh unique_ptr<PassConcept> for a named pass.
using PassFactory = std::function<std::unique_ptr<PassConcept>()>;

class EmberPassRegistry {
public:
    // Register a pass by name. The factory creates a fresh instance each call.
    // Usage: reg.add<ConstPropPass>("constprop");
    template <typename PassT>
    void add(const char* name) {
        factories_[name] = []() -> std::unique_ptr<PassConcept> {
            return std::make_unique<PassModel<PassT>>(PassT{});
        };
    }

    // Look up a pass by name. Returns nullptr if not found.
    std::unique_ptr<PassConcept> create(const char* name) const {
        auto it = factories_.find(name);
        if (it == factories_.end()) return nullptr;
        return it->second();
    }

    // Is a pass registered under this name?
    bool has(const char* name) const {
        return factories_.find(name) != factories_.end();
    }

    // List all registered pass names (for --list-passes).
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(factories_.size());
        for (const auto& [k, _] : factories_) out.push_back(k);
        return out;
    }

private:
    std::unordered_map<std::string, PassFactory> factories_;
};

} // namespace ember
