// ember codegen - tree-walking AST -> x86-64 (docs/spec/CODEGEN_SPEC.md).
// v1-of-frontend: a simple stack-spilling tree-walker (correctness first;
// the formal SSA-lite IR + linear-scan regalloc is a later refactor once
// v0.5 benchmarks say it matters - YAGNI). Handles bomb_timer's subset.
#pragma once
#include "ast.hpp"
#include "x64_emitter.hpp"
#include "jit_memory.hpp"
#include "sema.hpp"
#include "engine.hpp"        // Red 5: CompiledFn (complete type for CompileResult::compiled)
#include "thin_ir.hpp"        // Red 5: ThinFunction for CompileResult::transformed
#include "ember_pass.hpp"    // Red 5: PassRunReport / CheckedRunOptions for compile_func_checked
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// Forward decl: the pass manager (Stage C) is defined in ember_pass.hpp.
// Used by CodeGenCtx::pass_manager (run IR optimization passes between
// lower_function and emit_x64 when enable_ir_backend is true).
class EmberPassManager;
class EmberAnalysisManager;

struct CompiledFn;

// ─── Red 5 keyed dispatch codegen descriptor (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
//   §9.3, §6.4) ───────────────────────────────────────────────────────────────
// A borrowed immutable descriptor selecting keyed mode for a compile. Null in
// CodeGenCtx::keyed_dispatch means EXACT legacy behavior (the tree-walker +
// Thin IR emit are byte-identical to the pre-Red-5 path; r15 stays in the
// regalloc pool; no keyed resolution is emitted). When non-null, keyed mode:
//   - reserves r15 for the transient route word (removes r15 from the regalloc
//     pool, §6.4); the JIT'd code treats r15 as read-only route material.
//   - the keyed emit path is selected via CodeGenCtx (this descriptor), NOT by
//     rewriting logical slot metadata (§9.4: Thin IR retains logical slots).
//
// `strategy` and `layout` are borrowed pointers to the configured strategy /
// module-layout descriptors a future Red 6/7 emit consumes; null in this phase
// (Red 5 is the outer thunk + regalloc reservation, not the call-lowering emit).
// `runtime_key` pins WHERE the transient route word lives (r15 on Win64, §6.4).
enum class RuntimeKeyLocation : uint8_t {
    None = 0,   // legacy / unkeyed (no transient route register)
    R15  = 1,   // Win64 x64: r15 reserved for the transient route word
};

struct KeyedDispatchCodegen {
    const void* strategy = nullptr;        // configured DispatchStrategyConcept (borrowed; null in Red 5)
    const void* layout = nullptr;          // configured ModuleDispatchLayout (borrowed; null in Red 5)
    RuntimeKeyLocation runtime_key = RuntimeKeyLocation::None;
};

// Globals block: a TYPED layout (chunk c3) - one per-global (offset, size)
// pair, addressed [base + offset]. Scalars are 8 bytes at an 8-aligned offset;
// structs occupy StructLayout::size; fixed arrays occupy elem_size*array_len;
// slices occupy 16 bytes ({ptr,len}) with the ptr stored as a RELATIVE offset
// within the block (so the baked bytes round-trip through .em without loader
// fixup - codegen's global-slice-load adds globals_base at runtime). `index`
// is kept as the legacy flat slot index for backward compat with hosts that
// only seed scalar globals (offset == index*8 for an all-scalar set).
struct GlobalsBlock {
    int64_t base = 0;                       // set before compiling/calling
    std::unordered_map<std::string, uint32_t> index;   // name -> slot index (legacy)
    std::unordered_map<std::string, uint32_t> offsets; // name -> byte offset (c3)
    std::unordered_map<std::string, uint32_t> sizes;   // name -> byte size (c3)
    std::unordered_map<std::string, const Type*> types;
};

// Obfuscation options (docs/spec/CODEGEN_SPEC.md Section obf).
// Applied at emission time (correct-by-construction, no post-JIT disassembly).
// These emitter-level transforms (MBA identities, opaque-predicate pattern,
// CPUID-keyed gate) operate directly on the X64Emitter. The full PE-pass
// integration is a build-time host-DLL protection phase.
struct ObfOptions {
    bool mba = false;          // MBA arithmetic substitution on add/sub/xor/and/or
    bool opaque = false;       // opaque-predicate junk insertion (4 variants)
    bool keyed = false;       // CPUID-keyed entry gate (system-keyed assembly)
    int64_t cpuid_key = 0;    // expected CPUID.1:EAX (CPU signature) for the gate
};

// Read the running machine's CPUID.1:EAX (CPU signature) - host helper
// for baking the key into @obf_keyed functions.
int64_t current_cpuid_signature();

// Dispatch table base (set before compiling functions that call scripts).
struct CodeGenCtx {
    int64_t globals_base = 0;
    int64_t dispatch_base = 0;
    // v0.5 cross-module: the per-process ModuleRegistry base address, baked into
    // kind-2 cross-module call sites (mov r11,[reg_base+mod_id*8]). Required when
    // a module uses `mod::fn()` calls; null/0 if no cross-module calls. docs/MODULES.md §3.
    int64_t registry_base = 0;
    const std::unordered_map<std::string, NativeSig>* natives = nullptr;
    const std::unordered_map<std::string, int>* script_slots = nullptr;
    ObfOptions obf;   // host-set defaults; @obf annotations layer on top
    const StructLayoutTable* structs = nullptr;  // struct value types (task 1.6)
    // String encryption is now pure codegen: an encrypted string literal is
    // decrypted inline into a compiler-hidden temp frame slot (see codegen's
    // StringLit eval case / alloc_str_temp). No host native is involved, so
    // there is no str_decrypt_fn / str_decrypt_name field here anymore. A host
    // turns encryption on simply by setting Program::string_xor_key != 0
    // before calling sema; the JIT'd code decrypts on the stack at each use.

    // --- v0.4 safety: non-local trap + budgets (docs/spec/SAFETY_AND_SANDBOX.md §2-§4) ---
    // All compile-flag GATED for zero overhead when disabled. Hosts should call
    // safe_defaults() before compiling untrusted code, then wire either the
    // pointer fields below or use_context_reg. Enabled checks add one coarse
    // sub+jg at each
    // function entry plus existing loop back-edges (budget), one balanced
    // inc+cmp+jcc/dec around every script-issued script or native invocation
    // (combined call-stack depth), and traps route through the stub instead
    // of ud2.

    // Host-provided trap stub (context.hpp TrapStub). When set, EVERY trap
    // site (bounds, budget, depth, @obf_keyed) emits `mov rax,stub; call rax`
    // instead of `ud2`, so the host can longjmp to a checkpoint and recover.
    // When null, traps emit ud2 (the pre-v0.4 behavior — backward compatible).
    void* trap_stub = nullptr;
    // context_t pointer baked into trap calls as the stub's first arg, so the
    // stub can record the reason + longjmp. Required when trap_stub is set.
    void* trap_ctx = nullptr;

    // Coarse execution budget (§3): each function entry charges the existing
    // recursive block_cost(body) after frame/parameter setup; loop back-edges
    // retain their body-cost charges for repeated work. Costs are saturated
    // to positive imm32 before encoding. emit_budget_checks gates all checks.
    // budget_remaining starts INT64_MAX (context.hpp), so an enabled host that
    // sets no budget gets no false traps.
    int64_t* budget_ptr = nullptr;
    bool emit_budget_checks = false;

    // Combined call-stack depth guard (§4): every script-issued invocation,
    // script or native, validates depth before incrementing, traps before the
    // call on overflow/corruption, and decrements after
    // normal return. This counts simultaneous nesting (including native code
    // that re-enters script), never cumulative sequential calls. Non-local trap
    // recovery must call context_t::reset_for_call() before the next entry.
    int32_t* depth_ptr = nullptr;
    int32_t max_call_depth = 512;
    bool emit_depth_checks = false;

    // Secure opt-in as one operation, avoiding the historically unsafe pattern
    // where a host remembered only one of the independent guard flags. Pointer
    // mode still requires budget_ptr/depth_ptr; context-register mode reads the
    // fields from the per-call context.
    CodeGenCtx& safe_defaults() noexcept {
        emit_budget_checks = true;
        emit_depth_checks = true;
        return *this;
    }

    // v1.0 thread-safety (Option B1, docs/planning/plan_CONTEXT_THREADSAFETY.md): when true,
    // the budget/depth/trap emit reads context_t fields through a context register
    // (r14 = context_t*, host-set at entry, callee-saved so preserved across
    // script-to-script calls) instead of baked imm64 pointers. Lets ONE compiled
    // body serve N per-thread context_t's (no per-context recompile). Default
    // false = baked-ptr behavior unchanged (backward compat).
    bool use_context_reg = false;
    // Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.3, §6.4): when
    // non-null, keyed mode is selected for this compile. The descriptor is a
    // BORROWED immutable pointer (the host owns it for the compile's lifetime);
    // null = exact legacy behavior (byte-identical tree-walker + Thin IR emit,
    // r15 stays in the regalloc pool, no keyed resolution emitted). When non-null
    // with runtime_key == R15, the regalloc excludes r15 from the pool (§6.4) and
    // the keyed emit path is selected via this descriptor (§9.4: Thin IR retains
    // logical slots; keyed behavior is selected during emission, not by rewriting
    // logical slot metadata).
    const KeyedDispatchCodegen* keyed_dispatch = nullptr;
    // v1.0 thread-safety: the globals index/types threaded through CodeGenCtx so
    // compile_func no longer reads the process-wide g_globals_for_codegen pointer
    // (which races under parallel compilation). If null, falls back to the legacy
    // process-wide pointer (backward compat for hosts that haven't migrated).
    const std::unordered_map<std::string, uint32_t>* globals_index = nullptr;
    const std::unordered_map<std::string, uint32_t>* globals_offsets = nullptr; // c3: typed byte offsets (null -> fall back to index*8)
    const std::unordered_map<std::string, const Type*>* globals_types = nullptr;
    // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §5.2): the registered-fn allowlist — a
    // host-allocated byte array of length ceil(fn_slot_count/8), one bit per
    // script-fn slot (set by the host from script_slots at compile time). The
    // provenance guard (emit_call_target_guard) validates a runtime i64 handle
    // against this bitset before indexing the dispatch table (REDSHELL #6).
    // fn_allowlist_base is baked as a raw imm64 (stable for the module's
    // lifetime, same as the allowlist itself); fn_slot_count is the range bound.
    // Both 0 = no allowlist -> the guard is skipped (function refs unused).
    int64_t fn_allowlist_base = 0;
    int64_t fn_slot_count = 0;
    // v1.0 Tier 2 cross-module handles (`&mod::fn`): the per-process module
    // handle-records table (ModuleRegistry::handle_records_base()) — a raw
    // imm64 baked into the cross-module guard/dispatch, indexed by the
    // module_id extracted from a cross-module handle (bit 63 set). Each record
    // gives the TARGET module's (dispatch_base, allowlist_base, slot_count) so
    // the guard validates a cross-module handle against the correct module's
    // allowlist and dispatches via that module's table. Process-local (like the
    // allowlist) -> functions using cross-module handles are non-serializable to
    // .em. Both 0 = cross-module handles not supported -> the guard is the
    // existing intra-only path (a cross-module handle, bit 63 set / huge, fails
    // the intra range check and traps, which is correct: no valid cross-module
    // handles exist in a module that did not wire the records table).
    int64_t module_handle_records_base = 0;
    int64_t module_handle_records_count = 0;

    // --- Stage 1 codegen optimization (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4) ---
    // Two independent flags, BOTH default false -> the codegen is BYTE-IDENTICAL
    // to the pre-Stage-1 tree-walker (the 24/24 ctest gate + 268/0/0 lang gate
    // hold unchanged). Enabled per-function for benchmark comparison; the bench
    // harness (bench/bench_codegen_paths.cpp) is the perf validation.
    //
    // enable_peephole: run the post-emit peephole pipeline (src/peephole.{hpp,cpp})
    // over the function's final emitted byte buffer after resolve_fixups + the
    // AbsFixup/native-fixup patching. Ships SmartImmPass (W4: `mov r,imm64`->
    // cheaper imm32 forms for small literals, skipping relocatable AbsFixup/
    // NativeFixup loads). SetccMovzxPass (W10) is inert in Stage 1 (the in-place
    // `xor;setcc` rewrite clobbers the cmp's flags; a correct W10 needs a
    // cross-instruction pre-cmp zeroing = Stage 2). The peephole is a strictly
    // local in-place rewrite padded with trailing NOPs, so NO label offset ever
    // shifts and NO branch fixup needs re-resolving.
    //
    // enable_local_regalloc: keep BinExpr integer operands in a VOLATILE scratch
    // register (r10) across the RHS eval instead of `push rax; ...; pop rax`
    // (design W1, the single most-executed spill). The outermost no-call-in-RHS
    // BinExpr in a statement uses r10; a RHS containing a call falls back to
    // push/pop (r10 is volatile, clobbered by calls); a nested BinExpr whose r10
    // is occupied also falls back (correctness: a single holding register can't
    // nest). r10 is VOLATILE so there is NO prologue save/restore tax — zero
    // overhead on any function (a callee-saved holding register would need per-
    // function save/restore and net WORSE on call-heavy code where the tax
    // exceeds the per-BinExpr win; the volatile design avoids that). r10 is free
    // by default (MBA uses rdx, dispatch/guard use r11, opaque junk is off by
    // default); the regalloc is also disabled when obf.mba/obf.opaque is on as a
    // defensive guard against the r10-using obfuscation paths. This generalizes
    // the existing hot-local pinning (§1.2) from "one register, one loop" to "a
    // second volatile accumulator, every no-call integer BinExpr".
    bool enable_peephole = false;
    bool enable_local_regalloc = false;

    // --- Stage 3: linear-scan register allocation over the ThinFunction ---
    // When true AND enable_ir_backend is true, compile_func runs run_regalloc()
    // AFTER the optimization passes and BEFORE emit_x64. The regalloc assigns
    // scalar int/bool VRegs to Win64 callee-saved registers (rbx/rsi/rdi/r12/
    // r13/r15) instead of frame slots, with linear-scan spilling when all pool
    // registers are in use. Value-preserving: the emit uses the regalloc map;
    // spilled VRegs use their existing frame slots. Default false = the IR path
    // spills every VReg to a frame slot (the Stage A/C behavior, unchanged).
    bool enable_regalloc = false;

    // --- Stage A: thin three-address IR backend (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.3 Stage 2) ---
    // A compile-time AST -> ThinFunction -> x86 path, alternative to the
    // tree-walker. Default false -> compile_func runs the existing CG tree-walk
    // UNCHANGED (byte-identical to pre-Stage-A; the ctest gate + lang suite hold).
    // When true, compile_func calls lower_function() then emit_x64() instead.
    // The IR path is VALUE-EQUIVALENT (not byte-identical); it is the foundation
    // for Stage B (.em IR serialization) and Stage C (IR optimization passes).
    // Composes with enable_peephole (the IR path runs the same post-emit
    // peephole). Obfuscated functions (@obf_keyed/mba/opaque) fall back to the
    // tree-walker (the lowering marks them non_serializable; see thin_lower.hpp).
    bool enable_ir_backend = false;

    // --- Stage C: IR optimization passes (docs/spec/PASS_SYSTEM_DESIGN.md) ---
    // When non-null AND enable_ir_backend is true, compile_func runs the pass
    // manager's passes over the ThinFunction BETWEEN lower_function and
    // emit_x64. Default null = no passes (unchanged IR-path behavior). The
    // host builds the pass manager from a registry + pipeline string:
    //   EmberPassRegistry reg; ext_opt::register_passes(reg);
    //   EmberPassManager pm;
    //   build_pipeline_from_string("constprop,cse,dce", reg, pm, &err);
    //   ctx.pass_manager = &pm;
    EmberPassManager* pass_manager = nullptr;
    EmberAnalysisManager* analysis_manager = nullptr;
    // Red 5: when true, compile_func_checked populates CompileResult::
    // transformed with the post-pass ThinFunction (a deep copy of the IR as
    // it stood after the checked pass pipeline, before regalloc/emit). Default
    // false = `transformed` is left empty (the opt-out path; no extra copy).
    // The legacy compile_func wrapper ignores this flag (it returns only the
    // CompiledFn).
    bool request_transformed_ir = false;

    // --- GC-managed lambda environments (#20, ext_gc) ---
    // When true, the lambda-env codegen path allocates the closure env on the
    // tracing GC heap (via a call to the __ember_gc_alloc_env native) instead
    // of as a stack-frame-local temp, so a lambda can outlive its creating
    // frame (the #20/#21 escape requirement). The env is pinned at alloc by
    // ext_gc so it survives auto-collects; full JIT'd root tracking (unpin at
    // the owning frame's epilogue) is the documented follow-up (see
    // ext_gc.hpp WHAT REMAINS #1). Default false = the existing stack-env
    // path unchanged (byte-identical to pre-GC; the lang suite + opt gate
    // hold). The host registers the __ember_gc_* natives + calls gc_init/
    // gc_reset around runs when this is enabled.
    bool use_gc_env = false;
};

// Compile one function. Returns the JIT'd bytes + (after finalize) entry.
// Source-compatible legacy wrapper (Red 5): a thin shim over the internal
// checked implementation that returns just the CompiledFn. It is
// EXCEPTION-SAFE: a thrown pass or backend error becomes an empty CompiledFn
// (exec == nullptr), never a propagated exception across this public
// boundary. The checked, structured path is compile_func_checked below.
CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx);

// ─── Red 5: the checked compile boundary ───
//
// Which backend a compile used (or attempted). Obf / coroutine functions
// fall back to the tree-walker; the IR backend is the optimized path.
// Tier 4 try/catch/throw no longer forces a fallback — they lower to
// TryCatch/CatchCleanup/CatchEntry/Throw ThinOps through the IR backend.
enum class CompileBackend : uint8_t {
    TreeWalker,   // the v1 stack-spilling tree-walker (default / fallback)
    IRBackend,    // lower_function -> [checked passes] -> regalloc -> emit_x64
};

// A single stage of the checked compile pipeline, recorded for evidence.
// `reached` is false only for stages that were never attempted because an
// earlier stage failed (e.g. regalloc/emit after a validation failure).
// `ok` is the stage's own outcome; `detail` carries a short human-readable
// note (counts, the failure reason, or "skipped: prior stage failed").
// This is the structured evidence the Red 9 gate + any audit consults to
// prove: lowering ran, the checked passes validated after every reported
// mutation, stale regalloc was cleared, regalloc ran zero or exactly once,
// emission was attempted + produced bytes, and the result is finalizable —
// OR, on a validation/pass failure, that regalloc AND emission were NOT
// reached (no partial executable).
enum class CompileStage : uint8_t {
    Lowering,            // lower_function: AST -> ThinFunction
    CheckedPasses,       // run_checked over the pass manager (validated after each mutation)
    PreEmitVerify,       // verify_thin_function_for_codegen (the pre-regalloc/emit gate)
    StaleRegallocClear,  // thf.ra = RegAllocResult{} before the single allocation stage
    Regalloc,            // run_regalloc (zero or exactly one invocation)
    Emission,            // emit_x64: ThinFunction -> x86-64 bytes
    FinalizationEligible,// compiled.exec != nullptr + bytes non-empty (finalize()-able)
};
struct CompileStageTrace {
    CompileStage stage = CompileStage::Lowering;
    bool reached = false;     // was this stage attempted at all?
    bool ok = false;          // the stage's own outcome
    std::string detail;       // short note (counts / reason / skip reason)
};

// Structured compile result. `ok()` is true only when an executable CompiledFn
// was produced AND every gate passed (passes verified, regalloc/emit reached).
// On any failure `compiled.exec == nullptr`, the failure `reason` is set, and
// the pass_reports carry the per-pass checked outcome. `stage_trace` carries
// the ordered per-stage evidence (Lowering..FinalizationEligible); a
// validation/pass failure records Regalloc/Emission as NOT reached so a host
// can assert no partial executable was produced. Validation failure can NOT
// reach run_regalloc or emit_x64 (the checked path stops before them).
// Exceptions never cross this boundary: a thrown pass or backend error becomes
// a structured failure here, not a propagated exception.
struct CompileResult {
    bool ok_ = false;                       // true iff an executable was produced
    CompileBackend backend = CompileBackend::TreeWalker;
    std::string reason;                      // fallback/failure reason (empty on success)
    std::optional<ThinFunction> transformed; // the post-pass ThinFunction when requested
    std::vector<PassRunReport> pass_reports; // one report per checked run that executed
    std::vector<CompileStageTrace> stage_trace; // ordered per-stage evidence
    CompiledFn compiled;                     // emitted fn; exec == nullptr on failure/fallback-not-emitted
    bool ok() const { return ok_; }
    // Lookup a stage's trace (reached=false when absent / not attempted).
    const CompileStageTrace* stage(CompileStage s) const {
        for (const auto& t : stage_trace) if (t.stage == s) return &t;
        return nullptr;
    }
};

// Compile one function with the checked pass path. Honors ctx.pass_manager
// (run in checked mode between lower_function and regalloc/emit) AND
// ctx.analysis_manager (passes receive the host-provided manager instead of a
// freshly-constructed local one). On a pass validation/growth/error failure the
// result reports the failure and emits NO executable (run_regalloc/emit_x64
// are not reached). Stale/pre-existing regalloc on the lowered function is
// cleared before the single allowed allocation stage. Exceptions are caught at
// the boundary. When the IR backend is unavailable for this function (obf /
// coroutine / empty lowering) the result falls back to the
// tree-walker and reports the fallback reason. (Tier 4 try/catch/throw do NOT
// force a fallback — they lower to the IR backend.) The reported `backend` is
// the
// ACTUAL backend used (IRBackend whenever the IR path ran, including when
// pass_manager is null; TreeWalker only on a real fallback). When
// ctx.request_transformed_ir is true, `transformed` is populated with the
// post-pass ThinFunction; otherwise it is left empty. compile_func(...) stays
// source-compatible as the exception-safe legacy wrapper returning just
// CompiledFn.
CompileResult compile_func_checked(const FuncDecl& f, const CodeGenCtx& ctx);

// Globals block used by codegen (set by the host before compiling/calling).
// A single process-wide pointer (v1 frontend; the host wires one block per
// engine). Defined in codegen.cpp.
extern GlobalsBlock* g_globals_for_codegen;

// v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §5.2): build the registered-fn allowlist —
// a byte array of length ceil(slot_count/8), one bit per slot, set iff that
// slot is a registered script function of this module (derived from
// script_slots). Returned as a std::vector<uint8_t> the host owns; the host
// pins its .data() base for the module's lifetime and sets CodeGenCtx::
// fn_allowlist_base / fn_slot_count before compiling. The guard validates a
// runtime i64 handle against this bitset before indexing the dispatch table.
// Slot 0 is valid iff it's in script_slots (handles are 0-based slot indices).
std::vector<uint8_t> build_fn_allowlist(
    const std::unordered_map<std::string, int>& script_slots, int slot_count);

} // namespace ember
