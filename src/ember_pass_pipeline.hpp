// ember_pass_pipeline.hpp — Stage C: pipeline string parser.
//
// Parses a comma-separated pipeline string ("constprop,cse,dce") into an
// EmberPassManager by looking up each name in the registry. Supports nested
// parentheses for future sub-pipelines ("flatten(subst,mba)") — for now, with
// one IR unit, nesting is flattened.
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §4.4.

#pragma once

#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"

#include <string>

namespace ember {

// Parse `spec` into `out` by looking up each name in `reg`.
// Returns true on success; false + *err on an unknown name.
// The spec is comma-separated: "constprop,cse,dce".
// Whitespace around names is trimmed. Empty spec = no passes (success).
inline bool build_pipeline_from_string(const std::string& spec,
                                       const EmberPassRegistry& reg,
                                       EmberPassManager& out,
                                       std::string* err) {
    if (spec.empty()) return true;
    size_t start = 0;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == ',') {
            // Extract the name [start, i), trim whitespace.
            while (start < i && (spec[start] == ' ' || spec[start] == '\t')) ++start;
            size_t end = i;
            while (end > start && (spec[end-1] == ' ' || spec[end-1] == '\t')) --end;
            if (end > start) {
                std::string name = spec.substr(start, end - start);
                auto pass = reg.create(name.c_str());
                if (!pass) {
                    if (err) *err = "unknown pass: \"" + name + "\"";
                    return false;
                }
                // Move the type-erased pass directly into the manager.
                out.add_pass_concept(std::move(pass));
            }
            start = i + 1;
        }
    }
    return true;
}

} // namespace ember
