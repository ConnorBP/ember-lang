// ember single-function hot reload (HOT_RELOAD.md §3, the open v0.4 item).
//
// Recompile ONE function in isolation, atomically swap its dispatch-table slot
// to the new entry, retire the old exec page. Per-function independence
// (CODEGEN_SPEC.md §7: cross-function references go through the dispatch table,
// not baked addresses) is what makes this safe — a function recompiles without
// touching any other function's bytes.
//
// Protocol (HOT_RELOAD.md §3):
//   1. parse+sema+codegen the new body in isolation. On failure: abort BEFORE
//      touching the dispatch table — old code keeps running, module stays valid.
//   2. allocate fresh exec memory (never overwrite the old page in place —
//      in-flight calls may still be in the old bytes; the old page must stay
//      valid until provably unreferenced).
//   3. atomically store the new address into slots[slot] (release store).
//   4. retire the old page. The CLI is single-threaded (no concurrent caller),
//      so the old page is freed synchronously after the swap. The spec's epoch
//      reclamation (§5) is for the multi-threaded host case — YAGNI for the CLI.
//
// Slot stability (HOT_RELOAD.md §1/§4): the slot index never changes on reload,
// so callers that baked `slot*8` keep working — the swap only changes the slot's
// *content* (the function address), not the slot index.
#pragma once
#include "ast.hpp"          // Program, FuncDecl
#include "codegen.hpp"       // compile_func, CodeGenCtx, CompiledFn
#include "engine.hpp"        // finalize, free_executable
#include "sema.hpp"          // NativeSig, OpOverloadTable, StructLayoutTable
#include "dispatch_table.hpp"// DispatchTable
#include <string>
#include <cstdio>
#include <algorithm>

namespace ember {

// Result of a reload attempt.
struct ReloadResult {
    bool ok = false;
    std::string error;            // parse/sema/codegen failure detail (ok=false)
    void* old_entry = nullptr;    // the retired page (freed by the caller when safe)
    CompiledFn new_fn{};          // the new function — caller keeps it alive (its exec page is now live in the slot)
};

// Reload one function `fn_name` with a new body parsed from `new_fn_source`.
// `prog` is the module's existing Program (for slot lookup + the existing
// function's signature/types, which the new body must match); `table` is the
// module's dispatch table (the slot is swapped in place); `ctx` is the codegen
// context (globals/dispatch/natives/structs — must match the initial compile).
// `natives`/`overloads`/`structs` are re-used (a reload doesn't change the
// registered surface).
//
// `new_fn_source` is a COMPLETE function (`fn name(...) -> T { ... }`) — parsed
// in isolation. The new function's name + signature must match the old (slot is
// looked up by name; a signature mismatch is a reload error, not a silent
// type-corruption — the caller's baked `slot*8` is fine, but the args it passes
// must still fit).
//
// On failure: returns {ok=false, error=...}; the dispatch table is untouched,
// the old page is untouched, the module is in its previous valid state.
inline ReloadResult reload_function(const std::string& new_fn_source,
                                    Program& prog,
                                    DispatchTable& table,
                                    const CodeGenCtx& ctx,
                                    const std::unordered_map<std::string, NativeSig>& natives,
                                    const OpOverloadTable* overloads,
                                    const StructLayoutTable* structs) {
    ReloadResult r;
    // 1. parse the new function body in isolation.
    auto lr = tokenize(new_fn_source, "<reload>");
    if (!lr.ok) { r.error = "reload lex: " + lr.error; return r; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { r.error = "reload parse: " + pr.error; return r; }
    if (pr.program.funcs.size() != 1) { r.error = "reload: expected exactly one function"; return r; }
    auto& new_fn = pr.program.funcs[0];

    // find the existing function's slot (by name). The new fn must match it.
    auto it = std::find_if(prog.funcs.begin(), prog.funcs.end(),
                           [&](const FuncDecl& f){ return f.name == new_fn.name; });
    if (it == prog.funcs.end()) { r.error = "reload: function '" + new_fn.name + "' not in module"; return r; }
    int slot = it->slot;
    if (slot < 0) { r.error = "reload: slot not assigned for '" + new_fn.name + "'"; return r; }
    new_fn.slot = slot;

    // Sema the new fn in the context of the WHOLE module: swap the new fn into
    // `prog` in place (Program is non-copyable — unique_ptr members), sema, then
    // swap back if sema fails (so a failed reload leaves the module's prog
    // untouched, HOT_RELOAD.md §3 step 1's "abort before touching the dispatch table").
    // Save the old fn so we can restore it on failure.
    FuncDecl old_fn = std::move(*it);  // move out, then we'll move the new one in
    *it = std::move(new_fn);            // prog now has the new fn at the same slot
    // sema needs a slot map covering all funcs (calls in the new body resolve).
    std::unordered_map<std::string, int> reload_slots;
    for (const auto& f : prog.funcs) reload_slots[f.name] = f.slot;
    StructLayoutTable layouts = structs ? *structs : StructLayoutTable{};
    // struct layouts are struct-level (unaffected by a fn-body change); reuse as-is.
    auto sr = sema(prog, natives, reload_slots, 0, overloads, structs ? structs : nullptr);
    if (!sr.ok) {
        std::string e = "reload sema: ";
        for (auto& err : sr.errors) e += "line " + std::to_string(err.line) + ": " + err.msg + "; ";
        r.error = e;
        // restore the old fn (prog was mutated in place).
        *it = std::move(old_fn);
        return r;
    }
    // sema stamped the new fn's CallExprs in `prog` (now at `*it`). Compile it.
    CompiledFn cf = compile_func(*it, ctx);
    if (!finalize(cf)) { r.error = "reload: alloc_executable failed"; *it = std::move(old_fn); return r; }

    // 3. atomically swap the slot. Capture the old entry for the caller to retire.
    r.old_entry = table.get(slot);
    table.set(slot, cf.entry);
    r.new_fn = std::move(cf);
    r.ok = true;
    return r;
}

} // namespace ember
