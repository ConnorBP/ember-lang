// ember single-function hot reload (docs/HOT_RELOAD.md §3/§5).
//
// A HotReloadDomain serializes guard entry with publication, assigns one
// monotonic epoch to every successful slot publication, and owns replaced
// executable pages until they are safe to free. Every OUTER host-to-script
// invocation using the associated dispatch table must hold an ExecutionGuard
// from before loading the slot through return from JIT code. Script-to-script
// and recursive calls are covered by that outer guard. Loading/caching a raw
// entry outside a guard is unsupported.
//
// Recompilation is still per-function: parse, sema, codegen, and finalize all
// complete before HotReloadDomain::publish is called. A failure in any of those
// phases neither touches the dispatch table nor advances the domain epoch.
//
// Red 8 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.8, §12.4): the
// HotReloadDomain class + ExecutionGuard now live in hot_reload_domain.hpp so
// the CORE `ember` lib (engine.cpp) can hold an ExecutionGuard without pulling
// these frontend (codegen/sema/parser) headers. This file keeps
// `reload_function`, which needs those frontend headers.
#pragma once
#include "ast.hpp"           // Program, FuncDecl
#include "codegen.hpp"       // compile_func, CodeGenCtx, CompiledFn
#include "engine.hpp"        // finalize, free_executable
#include "sema.hpp"          // NativeSig, OpOverloadTable, StructLayoutTable
#include "hot_reload_domain.hpp"  // HotReloadDomain + ExecutionGuard (core-safe)
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember {

// (HotReloadDomain + ExecutionGuard are defined in hot_reload_domain.hpp, now
// included above. reload_function lives here because it needs the codegen /
// sema / parser headers this TU pulls in.)

// Result of a reload attempt. On success new_fn/current entry remains owned by
// the host while it is current. The replaced page has already transferred to
// `domain`; no owning old-entry pointer is returned, preventing caller/domain
// double-free. A later successful publication transfers new_fn.exec to the
// domain in the same way.
struct ReloadResult {
    bool ok = false;
    std::string error;
    uint64_t publication_epoch = 0;
    uint64_t retirement_epoch = 0;
    bool old_page_retired = false;
    CompiledFn new_fn{};
};

// Reload one existing function from a COMPLETE replacement declaration.
// Slot indices and the canonical call signature cannot change. The caller must
// use `domain.guard()` around every outer invocation sharing `table`.
inline ReloadResult reload_function(const std::string& new_fn_source,
                                    Program& prog,
                                    DispatchTable& table,
                                    HotReloadDomain& domain,
                                    const CodeGenCtx& ctx,
                                    const std::unordered_map<std::string, NativeSig>& natives,
                                    const OpOverloadTable* overloads,
                                    const StructLayoutTable* structs) {
    ReloadResult r;
    auto lr = tokenize(new_fn_source, "<reload>");
    if (!lr.ok) { r.error = "reload lex: " + lr.error; return r; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { r.error = "reload parse: " + pr.error; return r; }
    if (pr.program.funcs.size() != 1) { r.error = "reload: expected exactly one function"; return r; }
    auto& new_fn = pr.program.funcs[0];

    auto it = std::find_if(prog.funcs.begin(), prog.funcs.end(),
                           [&](const FuncDecl& f){ return f.name == new_fn.name; });
    if (it == prog.funcs.end()) { r.error = "reload: function '" + new_fn.name + "' not in module"; return r; }
    int slot = it->slot;
    if (slot < 0) { r.error = "reload: slot not assigned for '" + new_fn.name + "'"; return r; }

    auto words = [&](const Type& t) -> int {
        if (t.is_slice) return 2;
        if (!t.struct_name.empty() && structs) {
            auto si = structs->find(t.struct_name);
            if (si != structs->end()) return (si->second.size + 7) / 8;
        }
        return 1;
    };
    auto mismatch = [&](const std::string& what) {
        r.error = "reload: incompatible signature for '" + new_fn.name + "': " + what;
    };
    if (new_fn.params.size() != it->params.size()) {
        mismatch("arity changed from " + std::to_string(it->params.size()) + " to " +
                 std::to_string(new_fn.params.size()));
        return r;
    }
    for (size_t i = 0; i < it->params.size(); ++i) {
        const Type& old_ty = *it->params[i].ty;
        const Type& new_ty = *new_fn.params[i].ty;
        if (!old_ty.same(new_ty) || words(old_ty) != words(new_ty)) {
            mismatch("parameter " + std::to_string(i + 1) + " changed from " +
                     old_ty.to_string() + " to " + new_ty.to_string());
            return r;
        }
    }
    if (!it->ret->same(*new_fn.ret) || words(*it->ret) != words(*new_fn.ret)) {
        mismatch("return type changed from " + it->ret->to_string() + " to " +
                 new_fn.ret->to_string());
        return r;
    }
    new_fn.slot = slot;

    // Program is non-copyable. Temporarily install the replacement so whole-
    // module sema can resolve calls, restoring the old declaration on failure.
    FuncDecl old_fn = std::move(*it);
    *it = std::move(new_fn);
    std::unordered_map<std::string, int> reload_slots;
    for (const auto& f : prog.funcs) reload_slots[f.name] = f.slot;
    auto sr = sema(prog, natives, reload_slots, 0, overloads, structs ? structs : nullptr);
    if (!sr.ok) {
        std::string e = "reload sema: ";
        for (auto& err : sr.errors) e += "line " + std::to_string(err.line) + ": " + err.msg + "; ";
        r.error = e;
        *it = std::move(old_fn);
        return r;
    }

    CompiledFn cf = compile_func(*it, ctx);
    if (!finalize(cf)) {
        r.error = "reload: alloc_executable failed";
        *it = std::move(old_fn);
        return r;
    }

    // This is the only publication point. Recording retirement is prepared
    // before the release store; a publication failure leaves epoch/table intact.
    auto publication = domain.publish(table, size_t(slot), cf.entry);
    if (!publication.ok) {
        free_executable(cf.exec);
        cf.exec = nullptr;
        cf.entry = nullptr;
        r.error = "reload: publication/retirement failed";
        if (publication.error) r.error += std::string(": ") + publication.error;
        *it = std::move(old_fn);
        return r;
    }

    r.publication_epoch = publication.publication_epoch;
    r.retirement_epoch = publication.retirement_epoch;
    r.old_page_retired = publication.old_page_retired;
    r.new_fn = std::move(cf);
    r.ok = true;
    return r;
}

} // namespace ember
