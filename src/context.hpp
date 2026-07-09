// ember execution context (SAFETY_AND_SANDBOX.md Section 2/Section 3/Section 4).
//
// The non-local abort primitive + the two quantity budgets (instruction
// budget, call depth). ember_call establishes a checkpoint (setjmp) before
// entering JIT'd code; any trap (bounds check, budget exhaustion, stack
// overflow, @obf_keyed gate mismatch) calls the host-provided trap stub,
// which longjmps back to the checkpoint. ember_call then returns with
// last_error set instead of the script crashing the process.
//
// v0.4 ships the single-call checkpoint (one jmp_buf). The spec's nested
// ember_call checkpoint STACK (a native calling back into ember_call) is
// v1.0 — the CLI and prism's single-call-per-entry model is covered now.
//
// Performance: budget and depth checks are COMPILE-FLAG GATED in CodeGenCtx
// (emit_budget_checks / emit_depth_checks). A host running trusted tool
// scripts compiles with both off -> literally zero new JIT instructions.
// A host running untrusted mods compiles with them on -> one sub+jg per
// loop back-edge and one inc+cmp+jcc per script-to-script call (spec'd).
// The checkpoint itself is one setjmp per ember_call entry, not per
// instruction or per iteration.
#pragma once
#include <cstdint>
#include <csetjmp>
#include <string>

namespace ember {

// Default max script-to-script call depth (SAFETY_AND_SANDBOX.md Section 4).
// Generous for real game-logic call trees, well below native-stack-overflow
// territory (ember frames are small). Host-configurable via context_t.
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
};

struct context_t {
    // setjmp checkpoint: ember_call does `if (setjmp(ctx.checkpoint)) { ... trap happened ... }`
    // before calling into JIT'd code. The trap stub longjmps here.
    jmp_buf checkpoint{};
    bool has_checkpoint = false;     // set true by ember_call after setjmp

    // Instruction budget (SAFETY_AND_SANDBOX.md Section 3). INT64_MAX = unset
    // (no false traps — a host that compiles WITH budget checks but hasn't
    // set a budget gets effectively-unlimited, never falsely traps). A host
    // sets this to a finite value to cap runaway loops.
    int64_t budget_remaining = INT64_MAX;

    // Stack-depth guard (SAFETY_AND_SANDBOX.md Section 4).
    int32_t call_depth = 0;
    int32_t max_call_depth = DEFAULT_MAX_CALL_DEPTH;

    // What trap occurred (set by the trap stub). None = no trap yet.
    TrapReason last_trap = TrapReason::None;
    // Human-readable detail for last_trap (e.g. "budget exceeded at loop back-edge").
    std::string last_error;

    void reset_for_call() {
        call_depth = 0;
        last_trap = TrapReason::None;
        last_error.clear();
        // budget_remaining is NOT reset here — a host may set one budget for
        // a whole batch of calls; reset is the host's responsibility.
    }
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
    }
    return "unknown";
}

} // namespace ember
