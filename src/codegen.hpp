// ember codegen - tree-walking AST -> x86-64 (docs/spec/CODEGEN_SPEC.md).
// v1-of-frontend: a simple stack-spilling tree-walker (correctness first;
// the formal SSA-lite IR + linear-scan regalloc is a later refactor once
// v0.5 benchmarks say it matters - YAGNI). Handles bomb_timer's subset.
#pragma once
#include "ast.hpp"
#include "x64_emitter.hpp"
#include "jit_memory.hpp"
#include "sema.hpp"
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
    // All compile-flag GATED for zero overhead when disabled. A host running
    // trusted tool scripts leaves these off/null -> no new JIT instructions.
    // A host running untrusted mods sets them -> one coarse sub+jg at each
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
    // script or native, does inc [ptr] + cmp max + trap-before-call / dec after
    // normal return. This counts simultaneous nesting (including native code
    // that re-enters script), never cumulative sequential calls. Non-local trap
    // recovery must call context_t::reset_for_call() before the next entry.
    int32_t* depth_ptr = nullptr;
    int32_t max_call_depth = 512;
    bool emit_depth_checks = false;

    // v1.0 thread-safety (Option B1, docs/planning/plan_CONTEXT_THREADSAFETY.md): when true,
    // the budget/depth/trap emit reads context_t fields through a context register
    // (r14 = context_t*, host-set at entry, callee-saved so preserved across
    // script-to-script calls) instead of baked imm64 pointers. Lets ONE compiled
    // body serve N per-thread context_t's (no per-context recompile). Default
    // false = baked-ptr behavior unchanged (backward compat).
    bool use_context_reg = false;
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
CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx);

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
