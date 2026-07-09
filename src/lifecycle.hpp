// ember lifecycle host-discovery API (LIFECYCLE.md §1-§2).
//
// Annotation-based discovery: the host queries a compiled module's annotated
// functions and drives them. @entry runs once at load; @on_tick runs per frame
// (host-controlled loop); @event("name") runs when the host dispatches that
// event. This is the static v1 model — no script-side function references
// (those are ROADMAP Tier 2), no dynamic registration. The host resolves a
// name to a slot (HOT_RELOAD.md §7: caching the slot is safe, indices never
// change) and calls it via the dispatch table.
//
// Pure introspection over Program::funcs[].annotations — no JIT change.
#pragma once
#include "ast.hpp"        // Program, FuncDecl, Annotation, Type
#include <string>
#include <vector>
#include <cstdint>

namespace ember {

// A discovered annotated function: its name, dispatch slot (stable across
// reloads — HOT_RELOAD.md §4), and signature (the host passes args per the
// signature, LIFECYCLE.md §2). The host caches the slot and calls per frame.
struct AnnotatedFn {
    std::string name;
    int slot = -1;
    Type ret;
    std::vector<Type> params;
    std::vector<std::string> annotation_args;  // e.g. @event("player_hit") -> {"player_hit"}
};

// Query every function annotated `annotation` (e.g. "@on_tick", "@entry").
// Returns one AnnotatedFn per matching function. Empty if none.
// `annotation` is matched with or without a leading '@' (both "@on_tick" and
// "on_tick" work — the parser stores annotation names without '@').
inline std::vector<AnnotatedFn> get_annotated_functions(const Program& prog,
                                                        const std::string& annotation) {
    std::string want = annotation;
    if (!want.empty() && want[0] == '@') want = want.substr(1);  // strip leading @
    std::vector<AnnotatedFn> out;
    for (const auto& fn : prog.funcs) {
        for (const auto& ann : fn.annotations) {
            if (ann.name == want) {
                AnnotatedFn af;
                af.name = fn.name;
                af.slot = fn.slot;
                af.ret = fn.ret ? *fn.ret : Type{};
                for (const auto& p : fn.params) af.params.push_back(p.ty ? *p.ty : Type{});
                af.annotation_args = ann.args;
                out.push_back(std::move(af));
                break;  // a function annotated the same name twice is a sema error; first match
            }
        }
    }
    return out;
}

// Query @event("name") functions — annotation "event" whose first arg matches
// `event_name`. This is the event-dispatch helper: the host fires event "X"
// and calls every @event("X") function. Empty if none match.
inline std::vector<AnnotatedFn> get_event_handlers(const Program& prog,
                                                    const std::string& event_name) {
    auto all = get_annotated_functions(prog, "event");
    std::vector<AnnotatedFn> out;
    for (auto& af : all) {
        if (!af.annotation_args.empty() && af.annotation_args[0] == event_name)
            out.push_back(std::move(af));
    }
    return out;
}

// The @entry function, if any (LIFECYCLE.md §1). Returns nullptr if the module
// has no @entry. A module should have at most one (sema could enforce; the
// host takes the first if multiple, matching "spec does not reserve a name").
inline const AnnotatedFn* get_entry_function(const Program& prog) {
    static thread_local AnnotatedFn cached;
    auto v = get_annotated_functions(prog, "entry");
    if (v.empty()) return nullptr;
    cached = std::move(v[0]);
    return &cached;
}

} // namespace ember
