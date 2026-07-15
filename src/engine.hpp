// ember engine (v0.1 minimal).
// docs/spec/CODEGEN_SPEC.md Section 6/Section 8: Win64 prologue/epilogue, dispatch table.
// v0.1: hand-built IR for a single function (no lexer/parser yet);
// the full engine API (docs/planning/DESIGN.md Section 8) grows from here.
#pragma once
#include "x64_emitter.hpp"
#include "jit_memory.hpp"
#include "context.hpp"   // context_t (v1.0 ember_call ctx-reg indirection)
#include "ast.hpp"
#include "key_provider.hpp"        // Red 5: DerivedMaterialProvider, DispatchKeyAdapter, ModuleId
#include "module_layout.hpp"       // Red 5: DispatchMode (for ModuleInstance assembly)
#include "module_instance.hpp"     // Red 8: ModuleInstance, LogicalCallableId, ember_current_keyed_runtime
#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <string_view>             // Red 8: resolve_entry_by_name_keyed
#include <memory>                  // std::shared_ptr (CompiledFn::gc_frame_map)

namespace ember {

// Forward decl: ModuleInstance (src/module_instance.hpp) — the host-owned
// per-runtime container the safe keyed APIs consult. (Also pulled in via
// module_instance.hpp above; kept for readability of the engine.hpp surface.)
struct ModuleInstance;

// A compiled function: its JIT'd bytes, the exec-memory pointer, and
// the entry address. Lives as long as the owning module/engine.
// `abs_fixups` are the absolute-imm64 relocations recorded by the emitter's
// mov_reg_imm64_external (docs/BUNDLING_AND_EM_MODULES.md Section 2.4) - the dispatch-
// table base and globals base loads that a `.em` serializer must repoint at
// load time. Populated by compile_func; empty for the hand-built engine.cpp
// proofs (which bake real addresses via raw mov_reg_imm64).
struct CompiledNativeBinding {
    uint32_t code_offset = 0;
    std::string name;
    Type ret;
    std::vector<Type> params;
};

struct CompiledFn {
    std::string name;
    std::vector<uint8_t> bytes;   // emitted bytes (kept for inspection/debug)
    void* exec = nullptr;         // alloc_executable result
    void* entry = nullptr;        // == exec (alias for clarity at call sites)
    std::vector<AbsFixup> abs_fixups; // relocatable imm64 slots (for .em serialization)
    std::vector<CompiledNativeBinding> native_fixups; // symbolic host-native binding slots
    std::vector<uint8_t> rodata;      // function-local bytes appended to loaded page
    std::string non_serializable_reason;
    // Precise GC root scanning: this function's compile-time GC-pointer
    // frame-slot map (the shadow-stack frame record's `map`). A heap-allocated
    // GcFrameMap whose ADDRESS is baked into the prologue (so it must live at a
    // stable address for the function's lifetime — shared_ptr guarantees
    // that). Null when precise GC is off (CodeGenCtx::use_gc_env == false): the
    // prologue emits no frame-record maintenance and the bytes are byte-
    // identical to the pre-precise-GC path. The backing `offs` vector is
    // filled during codegen and stable once compile completes (before any
    // call), so the collector may iterate it directly.
    std::shared_ptr<gc::GcFrameMap> gc_frame_map;
};

// v0.1: build the compiled form of `fn add(a: i64, b: i64) -> i64 { return a + b; }`
// directly via the emitter. No parser yet - this proves the codegen +
// jit_memory + call round-trip (docs/spec/CODEGEN_SPEC.md Section 12 criterion 1).
//
// Win64: a=rcx, b=rdx, return=rax.
//   push rbp
//   mov  rbp, rsp
//   mov  rax, rcx        ; a
//   add  rax, rdx        ; + b
//   mov  rsp, rbp
//   pop  rbp
//   ret
CompiledFn compile_add_i64();

// Build `fn sub(a: i64, b: i64) -> i64 { return a - b; }` (criterion: sub path)
CompiledFn compile_sub_i64();

// Build `fn mul(a: i64, b: i64) -> i64 { return a * b; }` (criterion: imul path)
CompiledFn compile_mul_i64();

// Build a leaf `fn ret_const() -> i64 { return <imm>; }` (criterion: imm64 path)
CompiledFn compile_ret_const(int64_t imm);

// v0.2: `fn max(a: i64, b: i64) -> i64 { if (a > b) return a; return b; }`
// Tests the forward-label fixup system (docs/spec/CODEGEN_SPEC.md Section 4/Section 12 criterion 3).
// Win64: a=rcx, b=rdx, return=rax.
CompiledFn compile_max_i64();

// v0.2: `fn fib(n: i64) -> i64 { if (n <= 1) return n; return fib(n-1)+fib(n-2); }`
// Recursive call through the dispatch table (docs/spec/CODEGEN_SPEC.md Section 7/Section 12 criterion 5).
// n=rcx (arg1). Uses callee-saved rbx (n) and r12 (fib(n-1)).
// table_base baked as absolute imm64; must be set on the table before finalize.
CompiledFn compile_fib_i64(int64_t table_base, uint32_t slot);

// v0.3: `fn(p: i64, a: i64) -> i64 { return native(p, a); }`
// Script->native call path (docs/spec/CODEGEN_SPEC.md Section 8). Forwards (proc, addr)
// to a host C++ function whose Win64 signature is i64(i64,i64), returning
// its result. Proves the proc_api binding pipeline end-to-end.
CompiledFn compile_native_passthrough_2arg(void* native_fn);

// Allocate exec memory for a CompiledFn's bytes and set its entry pointer.
// Returns false on alloc failure.
bool finalize(CompiledFn& fn);

// Call a finalized i64(i64,i64) function pointer.
int64_t call_i64_i64_i64(void* entry, int64_t a, int64_t b);

// Call a finalized i64() function pointer.
int64_t call_i64_i64(void* entry);

// Raw v1.0 B1 context thunks (docs/planning/plan_CONTEXT_THREADSAFETY.md): call a JIT'd
// entry with context_t* installed in r14. Use only for modules compiled with
// CodeGenCtx::use_context_reg=true. The thunk preserves its caller's incoming
// r14 and script-to-script calls inherit ctx through that callee-saved register.
// These helpers do NOT reset context_t and do NOT establish a setjmp checkpoint;
// a host requiring recoverable traps must do that around the thunk call.
//
// Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.8): these raw
// ember_call_* helpers are LEGACY-ONLY. They do NOT install r15 and are not a
// sanctioned keyed entry path. A keyed ModuleInstance must use the safe
// ember_call_keyed_* APIs below (which derive the route word, install r14/r15,
// establish a checkpoint, invoke, clean route state, and restore the caller's
// registers on every normal/trap path). The raw helpers remain for unkeyed
// modules and for hosts that manage their own checkpoint/reload discipline.
int64_t ember_call_void(void* entry, context_t* ctx);
int64_t ember_call_i64(void* entry, context_t* ctx, int64_t a);
int64_t ember_call_i64_i64(void* entry, context_t* ctx, int64_t a, int64_t b);

// ─── Red 5: safe keyed host-to-script call APIs (§9.8, §6.3) ────────────────
//
// The structured result of a keyed host-to-script call. `ok` is true only when
// the call was entered, ran, and returned normally; `value` carries the i64
// return for the i64/i64_i64 forms (0 for the void form). `trapped` is true
// when a trap fired and the trap stub longjmp'd back to the API's checkpoint —
// in that case `reason` carries the trap reason string and `value` is 0. On a
// pre-entry failure (provider failure, missing module/entry, bad logical
// identity), `ok` is false, `trapped` is false, and `reason` carries a
// structured diagnostic; the thunk was never entered and the caller's
// registers are untouched (the API never reached the asm thunk).
struct CallResult {
    bool ok = false;            // entered + returned normally
    bool trapped = false;       // a trap fired + longjmp'd back to the checkpoint
    int64_t value = 0;          // i64 return (void form: 0)
    std::string reason;         // structured diagnostic on failure/trap
    TrapReason trap_reason = TrapReason::None;  // numeric trap reason (set BEFORE
                                // reset_for_call clears ctx.last_trap, so a
                                // caller that needs the numeric code — e.g. a
                                // keyed thread worker reporting the slot's
                                // trap_reason — reads it here, not ctx.last_trap)
};

// Safe keyed outer-call APIs (§9.8). They establish:
//   1. derive the transient route word once from the provider via the adapter
//      (§6.3 step 1) — provider failure -> structured CallResult failure, the
//      thunk is never entered;
//   2. resolve the logical entry (the module's `main`/named export) against the
//      instance's dispatch record — missing/bad identity -> structured failure;
//   3. establish a setjmp checkpoint on the supplied context_t (when the
//      instance has a trap stub) so a trap longjmps back here cleanly;
//   4. install r14 = &ctx and r15 = route_word via the keyed asm thunk;
//   5. invoke the entry; nested script calls inherit r15 directly (§6.5);
//   6. on normal return, clear the transient r15 (xor r15,r15) and restore the
//      caller's original r14/r15 (the thunk pushes/pops them) (§6.3 step 5);
//   7. on a trapped exit, the trap stub longjmps to the checkpoint; the API's
//      own callee-saved r14/r15 are restored by the C++ epilogue (the thunk's
//      transient r15 is in the abandoned thunk frame), so the caller's values
//      survive and the transient r15 is cleared (§6.3 step 5).
//
// Red 8: resolution goes through the instance's CURRENT immutable
// ModuleDispatchRecord + the transient route word (NOT raw
// entry_table[logical_slot], §9.8), the hot-reload/generation guard is held
// from resolution through return or trap, and the current-keyed-runtime TLS is
// set on entry and cleared on every normal/trapped exit.
//
// `name` resolves against the module's export name directory (the instance's
// named_entries map); "main" is the conventional entry. The instance's mode +
// counts select identity or keyed resolution; the route word participates in
// keyed resolution (Red 4's resolver) and in r15 installation regardless of
// mode (the thunk reserves r15 for the whole call tree, §6.4).
CallResult ember_call_keyed_void(ModuleInstance& inst, const std::string& name,
                                 context_t& ctx, const DispatchKeyAdapter& adapter);
CallResult ember_call_keyed_i64(ModuleInstance& inst, const std::string& name,
                                context_t& ctx, int64_t a,
                                const DispatchKeyAdapter& adapter);
CallResult ember_call_keyed_i64_i64(ModuleInstance& inst, const std::string& name,
                                    context_t& ctx, int64_t a, int64_t b,
                                    const DispatchKeyAdapter& adapter);

// ─── Red 8: safe keyed host-call overloads BY LOGICAL SLOT (§9.8, §12.4) ────
// The name forms above are retained; these overloads resolve a logical slot
// directly (a host that already knows the slot — e.g. a lifecycle tick or a
// delayed thread worker — bypasses the name directory). Resolution goes
// through the instance's CURRENT immutable ModuleDispatchRecord + the
// transient provider-derived route word (NOT raw entry_table[logical_slot]),
// the applicable hot-reload/generation guard is held from resolution through
// return or trap, and all runtime/TLS state (the current-keyed-runtime TLS)
// is cleaned on every normal AND trapped exit. The caller's r14/r15 are
// restored by the thunk (normal) or the wrapper epilogue (trap).
CallResult ember_call_keyed_void_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                         context_t& ctx,
                                         const DispatchKeyAdapter& adapter);
CallResult ember_call_keyed_i64_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                        context_t& ctx, int64_t a,
                                        const DispatchKeyAdapter& adapter);
CallResult ember_call_keyed_i64_i64_by_slot(ModuleInstance& inst, uint32_t logical_slot,
                                            context_t& ctx, int64_t a, int64_t b,
                                            const DispatchKeyAdapter& adapter);

// ─── Red 8: structured keyed entry resolvers (§9.8) ────────────────────────
// A host that needs a resolved entry pointer (lifecycle tick, thread entry,
// or FFI hand-off) obtains it through a keyed resolver, NEVER by indexing the
// dispatch storage with a bare logical slot (§9.8). `resolve_entry_keyed`
// validates the logical identity against the instance's published logical
// count, derives the route word from the provider via the adapter, applies the
// strategy permutation through the immutable ModuleDispatchRecord (identity
// mode -> physical_slots[logical_slot]; keyed mode -> P(route_word, domain,
// ordinal)), and returns the finalized entry. `resolve_entry_by_name_keyed`
// maps an export name to a LogicalCallableId first, then performs the same
// resolution. Both return a structured ExtensionError BEFORE yielding any
// pointer if the provider cannot derive, the logical slot exceeds the
// published logical count, the name is absent, or the domain/ABI fingerprint
// does not match. They hold the applicable generation guard across the
// resolution (a scoped lease: the guard is held for the duration of the
// resolver call and released before returning; a host that lets the returned
// pointer escape a guarded region must use resolve_entry_keyed_leased below,
// which holds the guard across the callback, or take its own guard). The route
// word is a transient; it is never stored.
// (LogicalCallableId + ember_current_keyed_runtime + ember_current_keyed_context
// are declared in module_instance.hpp.)
ExtensionResult<void*> resolve_entry_keyed(ModuleInstance& inst,
                                           const LogicalCallableId& id,
                                           const DispatchKeyAdapter& adapter);
ExtensionResult<void*> resolve_entry_by_name_keyed(ModuleInstance& inst,
                                                   std::string_view name,
                                                   const DispatchKeyAdapter& adapter);

// ─── Red 8: the lifetime-safe scoped-lease resolver form (§9.8, §12.4) ──────
// `resolve_entry_keyed` returns a raw pointer that a host CAN let escape the
// resolver's guarded region — and the guard held during resolution is released
// before the pointer is returned, so it does NOT protect the returned pointer
// once the host uses it. To avoid falsely claiming a dropped guard protects an
// escaped pointer, this scoped-lease form holds the generation guard for the
// duration of `body(entry)` and releases it (on normal return OR a recovered
// trap inside `body`) before returning to the host. A host that needs a
// resolved pointer to escape (lifecycle tick, thread entry, FFI hand-off, or
// any use beyond a single guarded invocation) MUST route through this form OR
// take its own ExecutionGuard on inst.reload_domain and keep it for the
// pointer's whole use — never assume the bare resolver's guard outlives the
// call. `body` is invoked with a non-null entry; it returns an i64 the lease
// form returns in its result. The TLS current-keyed-runtime is set for `body`
// and cleared on every exit. If `body` traps (longjmp to the lease's
// checkpoint, when inst.trap_stub is set), the guard + TLS are cleaned and the
// structured result reports trapped=true. NO route material is stored: the
// route word is derived transiently and discarded; only the entry pointer
// reaches `body`.
struct LeaseResult {
    bool ok = false;            // body ran and returned normally
    bool trapped = false;       // body trapped + longjmp'd back to the lease checkpoint
    int64_t value = 0;          // body's i64 return (normal)
    std::string reason;         // structured diagnostic on failure/trap
    TrapReason trap_reason = TrapReason::None;  // numeric trap reason (set BEFORE
                                // reset_for_call clears ctx.last_trap)
};
// The callback body signature: receives the resolved non-null entry + the
// caller's context (so body can invoke via the keyed re-entry thunk or a raw
// call) + the arg the host passed through. Returns an i64.
using KeyedLeaseBody = int64_t(*)(void* entry, context_t* ctx, int64_t arg);
LeaseResult resolve_entry_keyed_leased(ModuleInstance& inst,
                                       const LogicalCallableId& id,
                                       context_t& ctx, int64_t arg,
                                       KeyedLeaseBody body,
                                       const DispatchKeyAdapter& adapter);

// Keyed re-entry thunk (§6.5): a native that re-enters the script under the
// SAME route word calls this instead of a raw ember_call_*. It installs r14 =
// ctx and r15 = route_word, invokes `entry`, clears r15, restores the caller's
// r14/r15, and returns the i64 result. It does NOT derive the route word (the
// caller supplies it — the outer thunk's route word, captured by the native) and
// does NOT establish a checkpoint (the enclosing outer call's checkpoint is
// still live). A native re-entry MUST use this thunk so the keyed r15 invariant
// holds across native->script re-entry (§6.5: "a native-to-script re-entry must
// use a keyed re-entry thunk that preserves or explicitly reinstalls the same
// route word"). The void/i64 forms cover the common re-entry shapes.
int64_t ember_keyed_reentry_void(void* entry, context_t* ctx, uint64_t route_word);
int64_t ember_keyed_reentry_i64(void* entry, context_t* ctx, int64_t a, uint64_t route_word);
int64_t ember_keyed_reentry_i64_i64(void* entry, context_t* ctx, int64_t a, int64_t b,
                                    uint64_t route_word);

// ─── Red 5: test/observation asm helpers ───────────────────────────────────
// Read/set the caller's r15. Used by the Red 5 focused test to observe the
// transient route register (reentry_probe reads r15) and to seed a distinctive
// caller r15 before a call so the test can assert it's restored after. These
// are NOT part of the keyed runtime contract; they are test scaffolding exposed
// here so the test does not need its own inline asm.
uint64_t ember_read_r15();
void ember_set_r15(uint64_t v);

} // namespace ember
