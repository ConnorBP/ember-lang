// module_build.hpp — the required host boundary for profile/pass compilation.
//
// The production compile/publication primitive shared by the JIT host
// (examples/ember_cli.cpp's --profile / --passes path) and the integration
// gate (examples/polymorphic_pass_test.cpp's PUB/LIFE section). It compiles
// every FuncDecl in a module with compile_func_checked, REJECTS a TreeWalker
// fallback (a required profile/pass build must route through the IR backend),
// retains every emitted + finalized CompiledFn in PRIVATE staged ownership,
// and publishes NO dispatch slot, module-registry entry, module name, or
// exported/name record until EVERY function has compiled AND finalized
// successfully. On any required failure (compile / checked validation / pass /
// backend / emission / finalization / publication) it frees every staged
// executable allocation and leaves the caller's dispatch table + ModuleRegistry
// + name state byte-for-byte / observably unchanged.
//
// Design (the staged-ownership / atomic-publication boundary):
//   1. COMPILE: for each FuncDecl, CompileResult cr = compile_func_checked(f, ctx).
//      Record a per-function FnBuildReport from cr.stage_trace + cr.pass_reports.
//      Reject !cr.ok() || cr.backend == CompileBackend::TreeWalker -> fail.
//      The CompiledFn is moved into a PRIVATE staged vector (no table.set yet).
//   2. FINALIZE: finalize every staged CompiledFn. On a finalize failure free
//      every staged exec allocation and fail (NO table/registry mutation).
//   3. PUBLISH (only reached when every staged fn is finalized):
//        a. validate the dispatch batch (DispatchTable::publish_batch validates
//           every (slot, entry) FIRST with no mutation);
//        b. preflight the __main__ registry registration
//           (ModuleRegistry::preflight_register_module — no mutation);
//        c. commit: register_module(__main__) -> id; set_dispatch_slot_count;
//           publish_batch (commit the slots).
//      Steps (a)+(b) are side-effect-free; the only mutating commits are (c),
//      sequenced so a failure in any of them leaves the prior state intact.
//      register_module is itself atomic (fails without mutating); publish_batch
//      is validate-then-commit (fails without mutating). So a publication
//      failure observes the caller's table + registry byte-for-byte unchanged.
//
// The caller owns the staged CompiledFns (returned via out_fns, indexed by
// source order) and must keep them alive for as long as any dispatch entry is
// callable. The helper does NOT reset extensions or free anything on success
// (the caller owns the published state); on failure it frees every staged exec
// allocation it allocated and leaves out_fns empty.
//
// The NO-profile / NO-pass path stays on the legacy compile_func behavior
// (ember_cli.cpp keeps that path unchanged); this helper is the required
// profile/pass boundary only. A harmless publication primitive (publish_batch)
// is shared, but the legacy path does not call this helper.
//
// See src/module_build.cpp for the implementation + the structured report.

#pragma once

#include "codegen.hpp"          // CodeGenCtx, CompileResult, CompileBackend, CompiledFn, CompileStage
#include "engine.hpp"           // CompiledFn, finalize
#include "dispatch_table.hpp"   // DispatchTable
#include "module_registry.hpp"  // ModuleRegistry
#include "ember_pass.hpp"       // PassRunReport, PassStopReason
#include "ast.hpp"              // FuncDecl

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// Per-function build status. Each stage flag is derived from the
// CompileResult::stage_trace (reached/ok) so a host can PROVE a validation
// failure reached none of the later stages (regalloc/emission/finalization
// are reached=false on a pass/validation failure). `published` is set ONLY by
// the publication step after every function succeeded.
struct FnBuildReport {
    std::string name;
    int slot = -1;
    CompileBackend backend = CompileBackend::TreeWalker;
    bool compile_ok = false;       // cr.ok()
    bool lowered = false;          // CompileStage::Lowering reached + ok
    bool passes_validated = false; // CheckedPasses reached + ok (or skipped: no PM)
    bool pre_emit_verified = false;// PreEmitVerify reached + ok (or skipped: legacy)
    bool regalloc = false;         // CompileStage::Regalloc reached (ok or skipped-ok)
    bool emitted = false;          // CompileStage::Emission reached + ok (bytes non-empty)
    bool finalized = false;        // the host's finalize() succeeded on the staged fn
    bool published = false;        // the dispatch slot was committed for this fn
    std::string reason;            // cr.reason (failure / fallback reason)
    std::vector<PassRunReport> pass_reports;  // cr.pass_reports (per checked run)
    // The first failing stage (CompileStage name as a short string, empty on
    // success). "checked-passes" / "pre-emit-verify" / "emission" / "finalize"
    // / "publication" / "" (success). Lets a host assert validation failure
    // stopped BEFORE regalloc/emission (first_failure in {checked-passes,
    // pre-emit-verify} and regalloc/emitted/finalized all false).
    std::string first_failure;
};

// The structured module-build report. `ok` is true iff every function
// compiled, finalized, AND the dispatch + __main__ registry metadata were
// published. On a failure `fail_reason` names the failing function + stage,
// `published` is false, and the caller's table/registry are unchanged. The
// per-function `fn_reports` carry the staged-ownership proof (a validation
// failure shows regalloc/emission/finalization/publication all false).
struct ModuleBuildReport {
    bool ok = false;
    bool published = false;        // dispatch slots + __main__ registry committed
    std::string module_name;       // the name published under (e.g. "__main__")
    uint32_t module_id = UINT32_MAX; // the registry id (valid only when published)
    std::string fail_reason;       // the first failing function + stage
    std::vector<FnBuildReport> fn_reports;  // one per FuncDecl, source order
};

// Compile + finalize + atomically publish a module's functions through the
// required checked boundary. See the file header for the staged-ownership
// contract. `ctx` must have dispatch_base + registry_base set to the STABLE
// bases baked into the JIT'd code (DispatchTable::base / ModuleRegistry::base,
// both stable from construction); the helper does NOT rebase them. `table`
// must be sized to funcs.size() and initially all-null. `registry` must already
// hold any linked modules (their ids are stable); __main__ is registered here
// as the publication step. `out_fns` receives the finalized CompiledFns in
// source order on success (caller-owned; keep alive while dispatch is live);
// on failure it is cleared and every staged exec allocation is freed.
//
// `reject_trewalker` controls the backend rejection: when true (the required
// profile/pass path) a CompileBackend::TreeWalker result is a hard failure
// (the IR backend is required); when false the helper accepts a TreeWalker
// fallback (used by tests that exercise the fallback-rejection path by
// toggling it). The CLI's required path passes true.
//
// Returns the structured ModuleBuildReport. Never throws (compile_func_checked
// is exception-safe; finalize/publish_batch/register_module do not throw on
// the validated paths; a thrown std::exception from finalize is caught and
// reported as a finalization failure with the staged fns freed).
ModuleBuildReport compile_publish_module_checked(
    const std::vector<FuncDecl>& funcs,
    const std::unordered_map<std::string, int>& slots,
    CodeGenCtx& ctx,
    DispatchTable& table,
    ModuleRegistry& registry,
    const std::string& module_name,
    std::vector<CompiledFn>& out_fns,
    bool reject_trewalker = true,
    std::string* err = nullptr);

} // namespace ember
