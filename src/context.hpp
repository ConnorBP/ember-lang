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
// THREAD-SAFETY (v1.0, Option D + B1): the canonical model is one context_t
// PER concurrently-entering OS thread. Each concurrent caller thread allocates
// its own context_t (private checkpoint/budget/combined depth/catch stack/
// GC shadow-stack head); they share the dispatch table, JIT'd code, and module
// registry (all read-only after compile). The JIT'd budget/depth/trap reads go
// through a context pointer passed per-call (CodeGenCtx::use_context_reg), so
// one compiled body serves N per-thread contexts — no per-context recompile.
// The host's raw B1 thunk sets the context register before entry; script-to-
// script calls forward it (callee-saved). SAFETY §8 + HOT_RELOAD §5 document
// this multi-context model. Hot-reload participation is deliberately not
// stored in context_t: the host wraps each OUTER host-to-script call in the
// reloadable table's HotReloadDomain::ExecutionGuard. Nested script calls
// share that guard. This keeps contexts independent of domains and avoids a
// global registry.
//
// IN-CONTEXT THREADS (v1 Tier 4, `thread` addon): the residual case where a
// compute-heavy mod needs parallelism WITHIN ONE script module — two ember-
// calling OS threads entering JIT'd code CONCURRENTLY through the one shared
// dispatch context active at thread_spawn. The per-call fields
// (checkpoint/budget/depth/catch stack/gc_frame_head) are NOT concurrently-
// safe in a single context_t, so the thread extension gives each spawned
// worker its OWN context_t seeded from the shared host context's settings
// (budget, max_call_depth, the shared typed-global root descriptor, and the
// shared GC runtime pointer below) while all workers + the host share the ONE
// dispatch table + the ONE context-owned GC heap. This is GENUINE CONCURRENT
// ENTRY: multiple OS threads run JIT'd code simultaneously (the test gate
// observes >= 2 workers inside JIT at once), each with private per-call state,
// each trap unwinding to its OWN checkpoint, and each registering its own
// shadow-stack head with the shared heap's cooperative stop-the-world
// collector. There is NO call_mutex serialization and NO save/restore of the
// host's per-call fields: the host keeps its own context_t untouched while the
// workers run concurrently. thread_join waits ONLY on slot synchronization
// (done + the OS thread join), so nested spawn/join is deadlock-free. The
// multi-context model above is unchanged (one context per thread).
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
#include "gc_roots.hpp"  // ember::gc::GcFrameRecord, GcGlobalRoots (precise GC root scanning)

// Portable setjmp/longjmp for the host-managed trap-recovery checkpoint.
// The JIT'd try/catch mechanism uses a CUSTOM register save/restore (not libc
// jmp_buf), so this only affects the HOST-side checkpoint in context_t. On
// Windows/MinGW, __builtin_setjmp/__builtin_longjmp save+restore the FULL
// register file (including callee-saved r14/r15 + SSE) the JIT'd code and the
// C++ frame rely on; the MSVCRT setjmp/longjmp save fewer registers, which
// corrupts the caller's callee-saved state across a trap longjmp and crashes
// the host (SEGFAULT before/inside main). On Linux/Clang the jmp_buf type is
// __jmp_buf_tag[1] (incompatible with __builtin_setjmp's void**), so the
// standard setjmp/longjmp is used there. The macros resolve to the correct
// primitive per platform; all host checkpoint call sites use them.
#if defined(__GNUC__) && (defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__))
  #define EMBER_SETJMP(buf) __builtin_setjmp(buf)
  #define EMBER_LONGJMP(buf, val) __builtin_longjmp(buf, val)
#else
  #define EMBER_SETJMP(buf) setjmp(buf)
  #define EMBER_LONGJMP(buf, val) longjmp(buf, val)
#endif

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
    KeyedDispatchPadding, // Red 4 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
                          // §7.3): a keyed-dispatch resolution selected an ABI-
                          // compatible padding/trap ordinal. The padding target
                          // does NOT compare a key (§7.3); it simply occupies a
                          // physical ordinal selected by the build permutation
                          // and traps through Ember's recoverable mechanism. A
                          // wrong route word that lands here is memory-safe
                          // (the entry is non-null finalized RX code) but
                          // semantically incorrect, so the trap stub fires this
                          // reason before longjmp to the host checkpoint.
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

    // ---- Precise GC root scanning (shadow stack + global roots) ----
    // The head of the active-frame chain (the "shadow stack"). Each active
    // JIT frame, on prologue, links a GcFrameRecord (living inside its own
    // stack frame) onto this head; the epilogue unlinks it. The tracing
    // collector (ext_gc) walks the chain from this head and reports each
    // frame's mapped GC-pointer slots as roots. nullptr = no JIT frames
    // active (collection finds no stack roots). JIT'd code reads/writes it via
    // [r14 + context_offsets::gc_frame_head()]. reset_for_call() clears it
    // after a trap longjmp (the abandoned frames' balanced epilogues never
    // ran, so their records are stale; clearing the head prevents the next
    // collection from walking a dead frame and dereferencing a freed env).
    gc::GcFrameRecord* gc_frame_head = nullptr;
    // The typed-global GC-root descriptor (globals base + the byte offsets of
    // the block's GC-pointer words). Host-attached before running JIT code.
    // The collector reads *(global_roots->base + off) for each off. nullptr
    // = no global roots registered. JIT'd code does NOT read this (it is
    // consumed only by the collector's trace callback), but it lives in the
    // POD prefix so the offset is stable and a host can set it once.
    gc::GcGlobalRoots* gc_global_roots = nullptr;

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
    // MAX_CATCH_DEPTH bounds the stack. Tree codegen emits an unsigned runtime
    // depth < MAX guard before indexing either array, so an over-deep or
    // corrupted catch stack traps rather than writing out of bounds.
    static constexpr int32_t MAX_CATCH_DEPTH = 256;
    // 8 × int64_t = 64 bytes per entry: [rbx, rbp, r12, r13, r14, r15, rsp, rip]
    int64_t catch_bufs[MAX_CATCH_DEPTH][8]{};
    int32_t catch_saved_call_depths[MAX_CATCH_DEPTH]{};

    // ---- IN-CONTEXT THREADS: shared-runtime back-pointer (Tier 4 `thread` addon).
    //      NOT read by JIT'd [ctx_reg+off] code. When the host attaches this
    //      context to a shared GC runtime (ext_gc::gc_attach_context), this is
    //      the opaque pointer to that shared runtime (a ember::ext_gc::GcRuntime*,
    //      cast by the GC extension — context_t stays layer-clean and only holds
    //      the raw pointer). A spawned worker reads it off the host context and
    //      hands it to ext_gc::gc_thread_enter so the worker's own context_t
    //      joins the SAME shared heap + registers its shadow-stack head with the
    //      cooperative stop-the-world collector. nullptr = no shared GC runtime
    //      attached (the thread-local-heap fallback / non-GC operation). NOT read
    //      by JIT'd code (no context_offsets entry); the extensions reach it via
    //      the C++ struct. A host sets it once when attaching the context.
    void* gc_runtime = nullptr;
    // ---- Retained for ABI/source compatibility: the coarse mutex that the OLD
    //      serialized in-context-thread model locked around every ember_call. It
    //      is NO LONGER taken by the thread extension (concurrent entry replaced
    //      serialization), and NO host should lock it around an outer call. Kept
    //      so existing code that references it still links; it is uncontended and
    //      inert under the concurrent-entry model. Placed LAST so it does not
    //      perturb the POD-prefix offsets the codegen bakes above. ----
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
        // Precise GC: a host-level trap recovery (longjmp to the host
        // checkpoint) abandons the entire JIT call stack. The abandoned frames'
        // balanced epilogues never executed, so their GcFrameRecords are still
        // linked onto gc_frame_head — walking them would dereference freed env
        // pointers and corrupt the collector. Clear the head so the next call
        // starts with an empty shadow stack (the freed envs become collectable
        // on the next collect, which is correct: nothing reachable reaches them
        // after the trap). The global-root descriptor is retained (it is host-
        // owned and independent of the call stack).
        gc_frame_head = nullptr;
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
    // Precise GC root scanning: shadow-stack head + global-root descriptor.
    // Both are POD-prefix pointers the JIT reads/writes via [r14 + off].
    static int32_t gc_frame_head()    { return int32_t(offsetof(context_t, gc_frame_head)); }
    static int32_t gc_global_roots()  { return int32_t(offsetof(context_t, gc_global_roots)); }
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
    case TrapReason::KeyedDispatchPadding: return "keyed dispatch padding";
    }
    return "unknown";
}

} // namespace ember
