// ember execution context (SAFETY_AND_SANDBOX.md Section 2/Section 3/Section 4).
//
// The non-local abort primitive + the two quantity budgets (instruction
// budget, call depth). ember_call establishes a checkpoint (setjmp) before
// entering JIT'd code; any trap (bounds check, budget exhaustion, stack
// overflow, @obf_keyed gate mismatch, bad call target) calls the host-provided
// trap stub, which longjmps back to the checkpoint. ember_call then returns
// with last_error set instead of the script crashing the process.
//
// v0.4 ships the single-call checkpoint (one jmp_buf). The spec's nested
// ember_call checkpoint STACK (a native calling back into ember_call) is
// v1.0 — the CLI and prism's single-call-per-entry model is covered now.
//
// THREAD-SAFETY (v1.0, Option D + B1): a context_t is NOT shared across
// threads. Each concurrent caller thread allocates its own context_t (private
// checkpoint/budget/depth); they share the dispatch table, JIT'd code, and
// module registry (all read-only after compile). The JIT'd budget/depth/trap
// reads go through a context pointer passed per-call (CodeGenCtx::use_context_reg),
// so one compiled body serves N per-thread contexts — no per-context recompile.
// The host's ember_call sets the context register before entry; script-to-script
// calls forward it (callee-saved). SAFETY §8 + HOT_RELOAD §5 document this
// multi-context model.
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

// Default max script-to-script call depth (SAFETY_AND_SANDBOX.md Section 4).
inline constexpr int32_t DEFAULT_MAX_CALL_DEPTH = 512;

// A trap reason, set by the trap stub before the longjmp so ember_call can
// report what happened (the spec's exception-as-data model, minimum form).
enum class TrapReason : uint8_t {
    None = 0,
    BoundsCheck,       // runtime slice/array index out of range
    BudgetExceeded,    // instruction budget hit zero at a loop back-edge
    StackOverflow,     // call_depth exceeded max_call_depth
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
    int32_t call_depth = 0;                // §4
    int32_t max_call_depth = DEFAULT_MAX_CALL_DEPTH;  // §4
    TrapReason last_trap = TrapReason::None;          // set by the trap stub

    // ---- checkpoint + host-side state (NOT read by JIT'd [ctx_reg+off]) ----
    jmp_buf checkpoint{};
    bool has_checkpoint = false;     // set true by ember_call after setjmp
    std::string last_error;          // human-readable detail for last_trap

    void reset_for_call() {
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
