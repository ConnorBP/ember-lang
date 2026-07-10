// ember execution context (SAFETY_AND_SANDBOX.md Section 2/Section 3/Section 4).
//
// The non-local abort primitive + the two quantity budgets (coarse execution
// budget, combined script/native call-stack depth). A host that requires
// recoverable traps establishes a checkpoint (setjmp) before entering JIT'd
// code; any trap (bounds check,
// budget exhaustion, stack overflow, @obf_keyed gate mismatch, bad call target)
// calls the host-provided trap stub, which longjmps back to that checkpoint.
// The raw B1 helpers ember_call_void/ember_call_i64 only install r14 and call;
// they do NOT reset this context or establish a recoverable checkpoint. Manual
// setjmp/has_checkpoint management (as in ember_cli), or a host safe-call
// wrapper providing it, is required.
//
// context_t currently stores one host-managed checkpoint (one jmp_buf).
// A nested checkpoint stack remains deferred; hosts use single-call-per-entry
// discipline and manage has_checkpoint around each raw B1 thunk invocation.
//
// THREAD-SAFETY (v1.0, Option D + B1): a context_t is NOT shared across
// threads. Each concurrent caller thread allocates its own context_t (private
// checkpoint/budget/combined depth); they share the dispatch table, JIT'd code,
// and module registry (all read-only after compile). The JIT'd budget/depth/trap
// reads go through a context pointer passed per-call (CodeGenCtx::use_context_reg),
// so one compiled body serves N per-thread contexts — no per-context recompile.
// The host's raw B1 thunk sets the context register before entry; script-to-script
// calls forward it (callee-saved). SAFETY §8 + HOT_RELOAD §5 document this
// multi-context model. Hot-reload participation is deliberately not stored in
// context_t: the host wraps each OUTER host-to-script call in the reloadable
// table's HotReloadDomain::ExecutionGuard. Nested script calls share that guard.
// This keeps contexts independent of domains and avoids a global registry.
//
// LAYOUT: the JIT-read fields (budget_remaining, call_depth, max_call_depth)
// are in a POD PREFIX at the top, so [ctx_reg + offsetof(field)] is POD-safe
// and stable across compilers (offsetof on the non-POD trailing std::string
// would be UB). context_offsets() below gives the exact byte offsets the
// codegen bakes into [r14 + off].
#pragma once
#include <cstdint>
#include <csetjmp>
#include <string>

namespace ember {

// Default maximum simultaneously active script-issued calls (script or native,
// including native-to-script re-entry; SAFETY_AND_SANDBOX.md Section 4).
inline constexpr int32_t DEFAULT_MAX_CALL_DEPTH = 512;

// A trap reason, set by the trap stub before it longjmps to the host-managed
// checkpoint (the exception-as-data model, minimum form).
enum class TrapReason : uint8_t {
    None = 0,
    BoundsCheck,       // runtime slice/array index out of range
    BudgetExceeded,    // coarse budget hit zero at function entry/back-edge
    StackOverflow,     // combined script/native call depth reached its limit
    IllegalInstruction,// @obf_keyed CPUID-gate mismatch (V7) or other ud2
    DivByZero,         // (reserved; current div traps are ud2-based too)
    BadCallTarget,     // v1.0 function-ref provenance guard: an i64 used as a
                       // call target wasn't a registered fn (REDSHELL guard #6)
};

struct context_t {
    // ---- POD PREFIX: the fields JIT'd code reads via [ctx_reg + off]. ----
    // Keep these first + POD so offsetof is standard + stable. codegen bakes
    // the offsets from context_offsets() below.
    int64_t budget_remaining = INT64_MAX;  // §3: INT64_MAX = unset (no false traps)
    // §4: active script-issued call edges. A native stays counted until it
    // returns, so native->script re-entry composes with ordinary script calls.
    // This is simultaneous depth, not a sequential invocation counter.
    int32_t call_depth = 0;
    int32_t max_call_depth = DEFAULT_MAX_CALL_DEPTH;  // §4
    TrapReason last_trap = TrapReason::None;          // set by the trap stub

    // ---- checkpoint + host-side state (NOT read by JIT'd [ctx_reg+off]) ----
    jmp_buf checkpoint{};
    bool has_checkpoint = false;     // host sets true after setjmp, before raw B1 call
    std::string last_error;          // human-readable detail for last_trap

    void reset_for_call() {
        // Required after longjmp recovery: balanced leave instructions on the
        // abandoned JIT/native stack cannot execute, so discard their depth.
        call_depth = 0;
        last_trap = TrapReason::None;
        last_error.clear();
        // budget_remaining is NOT reset here — a host may set one budget for
        // a whole batch of calls; reset is the host's responsibility.
    }
};

// Byte offsets of the JIT-read fields within context_t, for codegen to bake
// into [ctx_reg + off]. Computed (not hardcoded) so they track the struct
// layout automatically. POD-prefix fields only.
struct context_offsets {
    static int32_t budget()    { return int32_t(offsetof(context_t, budget_remaining)); }
    static int32_t depth()     { return int32_t(offsetof(context_t, call_depth)); }
    static int32_t max_depth() { return int32_t(offsetof(context_t, max_call_depth)); }
};

// The trap-stub function signature: a host-provided C function the JIT'd
// code calls (never returns) when a trap fires. It must longjmp to the
// innermost context_t checkpoint. The `reason` + `detail` let it record
// what happened on the context before the longjmp.
//   void my_trap(ember::context_t* ctx, ember::TrapReason reason, const char* detail);
using TrapStub = void(*)(context_t*, TrapReason, const char*);

// Map a TrapReason to a stable string (for error reporting / tests).
inline const char* trap_reason_str(TrapReason r) {
    switch (r) {
    case TrapReason::None:               return "none";
    case TrapReason::BoundsCheck:        return "bounds check";
    case TrapReason::BudgetExceeded:     return "budget exceeded";
    case TrapReason::StackOverflow:      return "stack overflow";
    case TrapReason::IllegalInstruction: return "illegal instruction";
    case TrapReason::DivByZero:          return "divide by zero";
    case TrapReason::BadCallTarget:      return "bad call target";
    }
    return "unknown";
}

} // namespace ember
