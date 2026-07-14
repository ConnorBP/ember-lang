// ember_pass_pipeline.hpp — Stage C: strict transactional pipeline string parser.
//
// Parses a comma-separated pipeline string ("constprop,cse,dce") into an
// EmberPassManager by looking up each name in the registry.
//
// Accepted initial grammar (plan_POLYMORPHIC_CODE_ENGINE.md §4.6):
//
//   pipeline := <empty> | name (',' name)*
//
// Surrounding spaces/tabs are permitted around each name and are trimmed. An
// entirely empty specification is a successful no-op. The parser is STRICT and
// TRANSACTIONAL:
//
//   - every token is resolved into temporary ownership first;
//   - the caller's EmberPassManager is mutated only after the COMPLETE
//     specification succeeds;
//   - on any failure the manager's pass vector AND its PassInstrumentation are
//     left completely unchanged (nothing is moved, cleared, or silently
//     replaced).
//
// Rejected with a useful error written to *err:
//   - empty elements (middle "a,,b", leading ",b", trailing "a,", and
//     whitespace-only "a,   ,b");
//   - unsupported parentheses ("a,(b)", "a,b)", "(a,b", "flatten(subst,mba)");
//   - invalid characters / trailing junk inside a token ("a,b!", "a;b",
//     "a b");
//   - unknown names ("a,nonexistent") -> reported as `unknown pass: "name"`;
//   - a registered factory whose operator() returns nullptr -> reported
//     distinctly as `pass factory returned null for "name"` (the name IS
//     registered, so this is not collapsed to the unknown-name diagnostic).
//     The registry is checked with has() before create() so the two
//     failure modes stay distinguishable.
//
// Parameterized sub-pipeline syntax is intentionally not supported in this
// first cut; configured factories and named profiles cover the immediate need
// with less parser complexity.
//
// `build_pipeline_from_string` APPENDS resolved passes to any already in `out`.
// `replace_pipeline_from_string` REPLACES `out`'s passes with the resolved
// sequence. Neither touches the caller's PassInstrumentation on success or
// failure.
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §4.4,
//             docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.6, §9.3 Red 2.

#pragma once

#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"

#include <string>
#include <vector>

namespace ember {

namespace detail {

// Trim leading/trailing spaces and tabs from spec[start, end) and write the
// trimmed bounds to [tstart, tend). Returns false if the trimmed element is
// empty (covers middle/leading/trailing and whitespace-only elements).
inline bool trim_element(const std::string& s, size_t start, size_t end,
                         size_t& tstart, size_t& tend) {
    while (start < end && (s[start] == ' ' || s[start] == '\t')) ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    tstart = start;
    tend = end;
    return end > start;
}

// A pipeline name is alphanumeric plus '_' and '-' (every shipped pass name
// uses only these characters). Any other character — parenthesis, '!', ';',
// internal whitespace, etc. — makes the token invalid. This is what lets the
// parser reject parentheses and trailing junk with a precise diagnostic
// instead of a generic "unknown pass".
inline bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// Classify one trimmed token. Returns true if the token is a syntactically
// valid name (clean, no parentheses, no junk); false and a useful message
// otherwise. A clean token still has to be looked up in the registry.
inline bool validate_token(const std::string& s, size_t tstart, size_t tend,
                           std::string& msg) {
    // Check for parentheses first so the diagnostic is specific.
    for (size_t i = tstart; i < tend; ++i) {
        if (s[i] == '(' || s[i] == ')') {
            msg = "parentheses are not supported in pipeline specification "
                  "(token: \"" + s.substr(tstart, tend - tstart) + "\")";
            return false;
        }
    }
    // Then reject any other non-name character (trailing junk, internal
    // whitespace, ';', '!', etc.).
    for (size_t i = tstart; i < tend; ++i) {
        if (!is_name_char(s[i])) {
            msg = "invalid character in pass name \"" +
                  s.substr(tstart, tend - tstart) + "\"";
            return false;
        }
    }
    return true;
}

// Resolve every token of `spec` into `resolved` without touching `out`.
// Returns true on complete success; false + *err on the first failure, with
// `resolved` left empty and `out` untouched. An empty spec resolves to zero
// passes (success).
inline bool resolve_pipeline(const std::string& spec,
                             const EmberPassRegistry& reg,
                             std::vector<std::unique_ptr<PassConcept>>& resolved,
                             std::string* err) {
    resolved.clear();
    if (spec.empty()) return true;

    size_t start = 0;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == ',') {
            size_t tstart, tend;
            if (!trim_element(spec, start, i, tstart, tend)) {
                if (err) *err = "empty pass name in pipeline specification";
                resolved.clear();
                return false;
            }
            std::string msg;
            if (!validate_token(spec, tstart, tend, msg)) {
                if (err) *err = msg;
                resolved.clear();
                return false;
            }
            std::string name = spec.substr(tstart, tend - tstart);
            // Distinguish a truly unknown name from a registered factory
            // whose operator() returns nullptr. has() is checked first so the
            // two failure modes get different diagnostics: an unknown name is
            // "unknown pass", while a callable factory that yields no pass is
            // "pass factory returned null". Without this split the null-result
            // branch would be unreachable (create() returns nullptr for both).
            if (!reg.has(name.c_str())) {
                if (err) *err = "unknown pass: \"" + name + "\"";
                resolved.clear();
                return false;
            }
            auto pass = reg.create(name.c_str());
            // The name is registered, so a null result here means the factory
            // itself returned nullptr. Treat that as a hard failure so a
            // pipeline never silently omits a requested pass.
            if (!pass) {
                if (err) *err = "pass factory returned null for \"" + name + "\"";
                resolved.clear();
                return false;
            }
            resolved.push_back(std::move(pass));
            start = i + 1;
        }
    }
    return true;
}

} // namespace detail

// Parse `spec` and APPEND the resolved passes to `out`. Transactional: `out`
// is mutated only after the complete specification succeeds; on failure its
// pass vector and PassInstrumentation are left unchanged. Empty spec = no
// passes appended (success).
inline bool build_pipeline_from_string(const std::string& spec,
                                       const EmberPassRegistry& reg,
                                       EmberPassManager& out,
                                       std::string* err) {
    std::vector<std::unique_ptr<PassConcept>> resolved;
    if (!detail::resolve_pipeline(spec, reg, resolved, err)) return false;
    out.append_passes(std::move(resolved));
    return true;
}

// Parse `spec` and REPLACE `out`'s passes with the resolved sequence.
// Transactional: on complete success the old passes are swapped out and the
// caller's PassInstrumentation is preserved; on failure both the pass vector
// and PassInstrumentation are left completely unchanged. Empty spec = replace
// with the empty pipeline (success, `out` ends up with zero passes).
inline bool replace_pipeline_from_string(const std::string& spec,
                                         const EmberPassRegistry& reg,
                                         EmberPassManager& out,
                                         std::string* err) {
    std::vector<std::unique_ptr<PassConcept>> resolved;
    if (!detail::resolve_pipeline(spec, reg, resolved, err)) return false;
    out.replace_passes(std::move(resolved));
    return true;
}

} // namespace ember
