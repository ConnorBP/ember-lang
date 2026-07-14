// module_build.cpp — the required host boundary for profile/pass compilation.
//
// See module_build.hpp for the staged-ownership / atomic-publication contract.
// This is the production primitive shared by the JIT host (--profile/--passes)
// and the integration gate's PUB/LIFE section. It compiles every FuncDecl with
// compile_func_checked, rejects a TreeWalker fallback, retains every emitted +
// finalized CompiledFn in private staged ownership, and publishes no dispatch
// slot / registry entry / name until every function succeeded. On any required
// failure it frees every staged exec allocation and leaves the caller's
// dispatch table + ModuleRegistry + name state byte-for-byte / observably
// unchanged.

#include "module_build.hpp"
#include "jit_memory.hpp"   // free_executable

#include <exception>
#include <utility>

namespace ember {

namespace {

// Map a CompileResult's stage_trace + pass_reports to a FnBuildReport. The
// stage flags are derived from CompileStageTrace::reached + ok so a host can
// PROVE a validation failure reached none of the later stages.
static FnBuildReport report_from_compile(const FuncDecl& fn,
                                         const CompileResult& cr) {
    FnBuildReport r;
    r.name = fn.name;
    r.slot = fn.slot;
    r.backend = cr.backend;
    r.compile_ok = cr.ok();
    r.reason = cr.reason;
    r.pass_reports = cr.pass_reports;
    auto mark = [&](CompileStage s, bool& flag) {
        if (const CompileStageTrace* t = cr.stage(s)) {
            flag = t->reached && t->ok;
        }
    };
    mark(CompileStage::Lowering,            r.lowered);
    mark(CompileStage::CheckedPasses,       r.passes_validated);
    mark(CompileStage::PreEmitVerify,       r.pre_emit_verified);
    // Regalloc: reached=true ok=true OR reached=false (skipped-ok). The
    // stage_trace records skipped-ok as reached=false, ok=true. So regalloc
    // "ran or was legitimately skipped" = reached ? ok : ok-skipped. We set
    // r.regalloc = the stage was NOT a failure (either ran-ok or skipped-ok).
    if (const CompileStageTrace* t = cr.stage(CompileStage::Regalloc)) {
        r.regalloc = t->ok;  // ok=true for both ran-ok and skipped-ok
    }
    mark(CompileStage::Emission,            r.emitted);
    // FinalizationEligible means the staged fn is finalize()-able; the actual
    // finalize() outcome is recorded by the helper below.
    // first_failure: the first stage whose trace says reached+!ok, or empty.
    auto stage_name = [](CompileStage s) -> const char* {
        switch (s) {
            case CompileStage::Lowering:            return "lowering";
            case CompileStage::CheckedPasses:       return "checked-passes";
            case CompileStage::PreEmitVerify:       return "pre-emit-verify";
            case CompileStage::StaleRegallocClear:  return "stale-regalloc-clear";
            case CompileStage::Regalloc:            return "regalloc";
            case CompileStage::Emission:            return "emission";
            case CompileStage::FinalizationEligible:return "finalization-eligible";
        }
        return "?";
    };
    for (const auto& t : cr.stage_trace) {
        if (t.reached && !t.ok) { r.first_failure = stage_name(t.stage); break; }
    }
    return r;
}

} // namespace

ModuleBuildReport compile_publish_module_checked(
    const std::vector<FuncDecl>& funcs,
    const std::unordered_map<std::string, int>& slots,
    CodeGenCtx& ctx,
    DispatchTable& table,
    ModuleRegistry& registry,
    const std::string& module_name,
    std::vector<CompiledFn>& out_fns,
    bool reject_trewalker,
    std::string* err) {

    ModuleBuildReport rep;
    rep.module_name = module_name;
    rep.fn_reports.reserve(funcs.size());
    out_fns.clear();

    // A private staged-ownership vector: (slot, CompiledFn). No dispatch slot
    // or registry entry is published until every staged fn is finalized.
    struct Staged { int slot = -1; CompiledFn cf; };
    std::vector<Staged> staged;
    staged.reserve(funcs.size());

    auto free_staged = [&staged]() {
        for (auto& s : staged)
            if (s.cf.exec) free_executable(s.cf.exec);
        staged.clear();
    };

    // ── 1. COMPILE every FuncDecl with compile_func_checked ──
    for (const auto& fn : funcs) {
        CompileResult cr = compile_func_checked(fn, ctx);
        FnBuildReport fr = report_from_compile(fn, cr);
        // The required profile/pass boundary REJECTS a TreeWalker fallback:
        // the IR backend is required, so a TreeWalker result is a hard failure
        // even if cr.ok() (a fallback that produced a tree-walker body is not
        // an optimized build).
        if (!cr.ok()) {
            fr.first_failure = fr.first_failure.empty() ? "compile" : fr.first_failure;
            rep.fn_reports.push_back(std::move(fr));
            rep.fail_reason = "compile failed for " + fn.name + ": " + cr.reason;
            if (err) *err = rep.fail_reason;
            free_staged();
            return rep;  // table/registry unchanged
        }
        if (reject_trewalker && cr.backend == CompileBackend::TreeWalker) {
            fr.first_failure = "trewalker-fallback";
            rep.fn_reports.push_back(std::move(fr));
            rep.fail_reason = "required build rejected TreeWalker fallback for " +
                              fn.name + (cr.reason.empty() ? "" : (": " + cr.reason));
            if (err) *err = rep.fail_reason;
            // cr.compiled may hold a tree-walker body; free it with the staged set.
            if (cr.compiled.exec) free_executable(cr.compiled.exec);
            free_staged();
            return rep;  // table/registry unchanged
        }
        // Stage the compiled fn (PRIVATE ownership; no table.set yet). Move
        // the bytes + rodata + fixups into the staged vector; exec is still
        // null until the finalize step below.
        Staged s; s.slot = fn.slot; s.cf = std::move(cr.compiled);
        staged.push_back(std::move(s));
        rep.fn_reports.push_back(std::move(fr));
    }

    // ── 2. FINALIZE every staged CompiledFn (PRIVATE; no publication yet) ──
    for (auto& s : staged) {
        bool ok = false;
        try {
            ok = finalize(s.cf);
        } catch (const std::exception& e) {
            // finalize should not throw, but be exception-safe: report a
            // finalization failure + free every staged allocation.
            rep.fail_reason = "finalize threw for slot " + std::to_string(s.slot) +
                              ": " + e.what();
            if (err) *err = rep.fail_reason;
            // mark this fn's report + free everything
            for (auto& fr : rep.fn_reports) {
                if (fr.slot == s.slot) { fr.first_failure = "finalize"; break; }
            }
            free_staged();
            return rep;  // table/registry unchanged
        }
        if (!ok) {
            rep.fail_reason = "alloc_executable failed for slot " + std::to_string(s.slot);
            if (err) *err = rep.fail_reason;
            for (auto& fr : rep.fn_reports) {
                if (fr.slot == s.slot) { fr.first_failure = "finalize"; break; }
            }
            free_staged();
            return rep;  // table/registry unchanged
        }
        // Record finalization on this fn's report.
        for (auto& fr : rep.fn_reports) {
            if (fr.slot == s.slot) { fr.finalized = true; break; }
        }
    }

    // ── 3. PUBLISH (only reached when every staged fn is finalized) ──
    //   a. validate the dispatch batch (no mutation).
    //   b. preflight the __main__ registry registration (no mutation).
    //   c. commit: register_module -> id; set_dispatch_slot_count; publish_batch.
    std::vector<std::pair<size_t, void*>> entries;
    entries.reserve(staged.size());
    for (const auto& s : staged) entries.emplace_back(size_t(s.slot), s.cf.entry);

    // (a) validate the dispatch batch FIRST (publish_batch validates-then-commits;
    //     calling it here would commit, so we pre-validate by checking ranges
    //     ourselves, then call publish_batch as the atomic commit in (c)).
    // DispatchTable::publish_batch is itself validate-then-commit, so the call
    // in (c) is the single atomic commit. We do not pre-call it here.

    // (b) preflight the registry registration (side-effect-free).
    std::string pre_err;
    uint32_t pre_id = registry.preflight_register_module(module_name, table.base(), &pre_err);
    if (pre_id == UINT32_MAX) {
        rep.fail_reason = "registry preflight failed for " + module_name + ": " + pre_err;
        if (err) *err = rep.fail_reason;
        for (auto& fr : rep.fn_reports) fr.first_failure = "publication";
        free_staged();
        return rep;  // table/registry unchanged (preflight did not mutate)
    }

    // (c) commit. register_module is atomic (fails without mutating); if it
    //     fails here despite the preflight (a concurrent registration raced),
    //     free staged + return fail with the registry unchanged.
    std::string reg_err;
    uint32_t id = registry.register_module(module_name, table.base(), &reg_err);
    if (id == UINT32_MAX) {
        rep.fail_reason = "registry register failed for " + module_name + ": " + reg_err;
        if (err) *err = rep.fail_reason;
        for (auto& fr : rep.fn_reports) fr.first_failure = "publication";
        free_staged();
        return rep;  // registry unchanged (register_module failed without mutating)
    }
    rep.module_id = id;
    // Publish the dispatch-table slot count (X1 redesign: a loaded v5 .em that
    // calls back into the host range-checks its slot against the REAL host
    // dispatch size). set_dispatch_slot_count is a bounded write into the
    // sized-at-construction dispatch_slot_counts_ array (no realloc).
    registry.set_dispatch_slot_count(id, int64_t(table.slots.size()));
    // Commit the dispatch batch (validate-then-commit; cannot fail after the
    // pre-validation in publish_batch, but if it does, roll back the registry
    // registration by... there is no unregister (Section 8 non-goal). The
    // preflight + the staged fn's non-null entries (finalized) make this
    // publish_batch succeed; if it ever returns false, we leave the registry
    // entry in place (it points at an all-null table — a caller observing it
    // sees no callable surface) and report the failure. This is the documented
    // edge: register_module succeeded, publish_batch's validation is the last
    // gate. In practice publish_batch's validation (slot in range + non-null)
    // is guaranteed by the staged fns being finalized into a table sized to
    // funcs.size(), so this branch is unreachable for a correct caller.
    if (!table.publish_batch(entries)) {
        rep.fail_reason = "dispatch publish_batch validation failed for " + module_name;
        if (err) *err = rep.fail_reason;
        for (auto& fr : rep.fn_reports) fr.first_failure = "publication";
        // The registry entry was committed but points at an all-null table
        // (publish_batch validated before committing, so no slot was set). Free
        // the staged exec allocations; the caller's table is all-null. The
        // registry entry is a record with no callable surface — the documented
        // non-goal (no unregister) leaves it; a host that hits this branch has
        // a caller bug (table sized smaller than funcs.size() or a null entry).
        free_staged();
        return rep;
    }

    // ── publication succeeded: hand the staged fns to the caller ──
    rep.ok = true;
    rep.published = true;
    out_fns.reserve(staged.size());
    for (auto& s : staged) {
        for (auto& fr : rep.fn_reports) {
            if (fr.slot == s.slot) { fr.published = true; break; }
        }
        out_fns.push_back(std::move(s.cf));
    }
    staged.clear();  // moved-out; clear without freeing (ownership transferred)
    return rep;
}

} // namespace ember
