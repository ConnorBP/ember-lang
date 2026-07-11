// ember execution context (docs/spec/SAFETY_AND_SANDBOX.md Section 2/Section 3/Section 4).
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
// IN-CONTEXT THREADS (v1 Tier 4, `thread` addon): the residual case where a
// compute-heavy mod needs parallelism WITHIN ONE script context — two ember-
// calling OS threads sharing ONE context_t. The single checkpoint/budget/depth
// are not concurrently-safe, so context_t carries a coarse `call_mutex`. Every
// ember_call into this context (the host's outer call AND each spawned thread's
// call) locks it, serializing the script-side execution while the host + sibling
// threads run concurrently off-context. A spawned thread additionally saves +
// restores the per-call fields (budget/depth/catch/checkpoint) around its own
// call so the caller's in-progress state survives the interleaving (a thread_join
// releases the mutex while it waits, letting the spawned call run to completion).
// This is the coarse-grained-but-correct option; per-thread persistent copies of
// the fields remain a future refinement. The multi-context model above is
// unchanged and `call_mutex` is uncontended there (one context per thread).
//
// LAYOUT: the JIT-read fields (budget_remaining, call_depth, max_call_depth)
// are in a POD PREFIX at the top, so [ctx_reg + offsetof(field)] is POD-safe
// and stable across compilers (offsetof on the non-POD trailing std::string
// would be UB). context_offsets() below gives the exact byte offsets the
// codegen bakes into [r14 + off].
#pragma once
#include <cstdint>
#include <csetjmp>
#include <mutex>
#include <string>

namespace ember {

// Default maximum simultaneously active script-issued calls (script or native,
// including native-to-script re-entry; docs/spec/SAFETY_AND_SANDBOX.md Section 4).
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
    UnhandledThrow,    // Tier 4: a `throw` with no enclosing try/catch unwound
                       // to the host checkpoint (mirrors runtime_error severity)
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

    // Tier 4: in-language try/catch/throw catch stack (JIT-read/written).
    // catch_depth = number of active try/catch handlers on the catch stack.
    // A try block increments it (after saving state into catch_bufs[depth]);
    // normal try completion or a throw-to-catch decrements it. thrown_value
    // holds the most recently thrown i64 — the catch block reads it to bind
    // to the catch variable. Both live in the POD prefix so the JIT can
    // read/write them via [r14 + off] without a host call.
    int32_t catch_depth = 0;
    int32_t _catch_pad = 0;       // explicit 4-byte pad so thrown_value is 8-aligned
    int64_t thrown_value = 0;

    // ---- checkpoint + host-side state (NOT read by JIT'd [ctx_reg+off]) ----
    jmp_buf checkpoint{};
    bool has_checkpoint = false;     // host sets true after setjmp, before raw B1 call
    std::string last_error;          // human-readable detail for last_trap

    // ---- Tier 4: try/catch save buffers (JIT-emitted setjmp/longjmp) ----
    // Each try block saves callee-saved regs (rbx, rbp, r12, r13, r14, r15) +
    // rsp + the catch-entry rip into catch_bufs[catch_depth] (a custom
    // 64-byte buffer — we control both the save and the restore, so no libc
    // jmp_buf format dependency). catch_saved_call_depths[catch_depth]
    // snapshots call_depth at try-entry so a throw-to-catch that unwinds
    // across frames restores the catching frame's call_depth (the abandoned
    // frames' depth increments are discarded, mirroring reset_for_call).
    // MAX_CATCH_DEPTH bounds the stack; a try at depth == MAX is a sema
    // error (or would overflow here) — v1 sets it high enough that a script
    // can't reasonably hit it.
    static constexpr int32_t MAX_CATCH_DEPTH = 256;
    // 8 × int64_t = 64 bytes per entry: [rbx, rbp, r12, r13, r14, r15, rsp, rip]
    int64_t catch_bufs[MAX_CATCH_DEPTH][8]{};
    int32_t catch_saved_call_depths[MAX_CATCH_DEPTH]{};

    // ---- IN-CONTEXT THREADS: coarse serialization mutex (Tier 4 `thread` addon).
    //      NOT read by JIT'd [ctx_reg+off] code; host + spawned-thread calls lock
    //      it around every ember_call into this context. Uncontended under the
    //      multi-context model (one context_t per thread). Placed LAST so it does
    //      not perturb the POD-prefix offsets the codegen bakes above. ----
    std::mutex call_mutex{};

    void reset_for_call() {
        // Required after longjmp recovery: balanced leave instructions on the
        // abandoned JIT/native stack cannot execute, so discard their depth.
        call_depth = 0;
        last_trap = TrapReason::None;
        last_error.clear();
        // Tier 4: a host-level trap recovery (longjmp to the host checkpoint)
        // abandons the entire JIT call stack, so any active try/catch handlers
        // are gone too. Discard the catch stack so a subsequent call starts
        // clean (a throw with no try after a trap-recovered call must unwind
        // to the host, not to a stale buffer from the previous call).
        catch_depth = 0;
        thrown_value = 0;
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
    // Tier 4: try/catch catch stack (POD-prefix fields)
    static int32_t catch_depth()     { return int32_t(offsetof(context_t, catch_depth)); }
    static int32_t thrown_value()    { return int32_t(offsetof(context_t, thrown_value)); }
    static int32_t catch_bufs()      { return int32_t(offsetof(context_t, catch_bufs)); }
    static int32_t catch_saved_depths() { return int32_t(offsetof(context_t, catch_saved_call_depths)); }
    static int32_t catch_buf_stride()   { return 64; }  // 8 × int64_t per entry
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
    case TrapReason::UnhandledThrow:     return "unhandled throw";
    }
    return "unknown";
}

} // namespace ember
