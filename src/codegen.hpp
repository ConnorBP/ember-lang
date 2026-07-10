// ember codegen - tree-walking AST -> x86-64 (CODEGEN_SPEC.md).
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

struct CompiledFn;

// Globals block: a flat 8-byte-per-global region the host reads/writes.
// Codegen bakes `base` as an absolute imm64; global i is at [base + i*8].
struct GlobalsBlock {
    int64_t base = 0;                       // set before compiling/calling
    std::unordered_map<std::string, uint32_t> index;  // name -> slot index
    std::unordered_map<std::string, const Type*> types;
};

// Obfuscation options (CODEGEN_SPEC.md Section obf).
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
    // a module uses `mod::fn()` calls; null/0 if no cross-module calls. MODULES.md §3.
    int64_t registry_base = 0;
    const std::unordered_map<std::string, NativeSig>* natives = nullptr;
    const std::unordered_map<std::string, int>* script_slots = nullptr;
    ObfOptions obf;   // host-set defaults; @obf annotations layer on top
    const StructLayoutTable* structs = nullptr;  // struct value types (task 1.6)
    void* str_decrypt_fn = nullptr;  // __str_decrypt native (string encryption)

    // --- v0.4 safety: non-local trap + budgets (SAFETY_AND_SANDBOX.md §2-§4) ---
    // All compile-flag GATED for zero overhead when disabled. A host running
    // trusted tool scripts leaves these off/null -> no new JIT instructions.
    // A host running untrusted mods sets them -> one sub+jg per loop
    // back-edge (budget), one inc+cmp+jcc per script-to-script call (depth),
    // and traps route through the stub instead of ud2.

    // Host-provided trap stub (context.hpp TrapStub). When set, EVERY trap
    // site (bounds, budget, depth, @obf_keyed) emits `mov rax,stub; call rax`
    // instead of `ud2`, so the host can longjmp to a checkpoint and recover.
    // When null, traps emit ud2 (the pre-v0.4 behavior — backward compatible).
    void* trap_stub = nullptr;
    // context_t pointer baked into trap calls as the stub's first arg, so the
    // stub can record the reason + longjmp. Required when trap_stub is set.
    void* trap_ctx = nullptr;

    // Loop budget (§3): the context pointer is available to generated code;
    // each loop back-edge does sub [ptr],body_cost +
    // jg continue / else trap. emit_budget must be true for the checks to
    // be emitted. budget_remaining starts INT64_MAX (context.hpp) so a host
    // that enables checks but sets no budget gets no false traps.
    int64_t* budget_ptr = nullptr;
    bool emit_budget_checks = false;

    // Script-call depth guard (§4): if non-null, each script-to-script call does
    // inc [ptr] + cmp max + trap-before-call / dec after. emit_depth gates
    // emission. The depth counter + max live in context_t; codegen needs the
    // counter address + the max to compare against.
    int32_t* depth_ptr = nullptr;
    int32_t max_call_depth = 512;
    bool emit_depth_checks = false;

    // v1.0 thread-safety (Option B1, plan_CONTEXT_THREADSAFETY.md): when true,
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
    const std::unordered_map<std::string, const Type*>* globals_types = nullptr;
    // v1.0 Tier 2 (plan_FUNCTION_REFS.md §5.2): the registered-fn allowlist — a
    // host-allocated byte array of length ceil(fn_slot_count/8), one bit per
    // script-fn slot (set by the host from script_slots at compile time). The
    // provenance guard (emit_call_target_guard) validates a runtime i64 handle
    // against this bitset before indexing the dispatch table (REDSHELL #6).
    // fn_allowlist_base is baked as a raw imm64 (stable for the module's
    // lifetime, same as the allowlist itself); fn_slot_count is the range bound.
    // Both 0 = no allowlist -> the guard is skipped (function refs unused).
    int64_t fn_allowlist_base = 0;
    int64_t fn_slot_count = 0;
};

// Compile one function. Returns the JIT'd bytes + (after finalize) entry.
CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx);

// Globals block used by codegen (set by the host before compiling/calling).
// A single process-wide pointer (v1 frontend; the host wires one block per
// engine). Defined in codegen.cpp.
extern GlobalsBlock* g_globals_for_codegen;

// v1.0 Tier 2 (plan_FUNCTION_REFS.md §5.2): build the registered-fn allowlist —
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
