// src/darwin_arm64_thunks.S — Darwin ARM64 host->JIT ABI thunks.
// plan_MACOS_ARM64.md Phase 4. The x86 path uses inline GNU asm in engine.cpp
// (pushq %r14 / shadow space / Win64). On ARM64 there is NO inline-asm thunk
// path in engine.cpp (the #else stubs), so these out-of-line AAPCS64 thunks
// provide the ember_call_* symbols. They install x19 = context_t* (the r14
// role — see plan §2), marshal the script arg into x0, call the JIT entry,
// restore the caller's x19, and return the i64 result in x0.
//
// ABI (AAPCS64):
//   ember_call_void_thunk(void* entry x0, context_t* ctx x1)            -> x0
//   ember_call_i64_thunk(void* entry x0, context_t* ctx x1, i64 a x2)   -> x0
//   ember_call_i64_i64_thunk(void* entry x0, context_t* ctx x1,
//                            i64 a x2, i64 b x3)                         -> x0
//
// The JIT entry is a standard AAPCS64 function: first script arg in x0 (and
// x1 for the 2-arg form), returns i64 in x0. x19 is callee-saved (the entry
// preserves it across script->script calls). No shadow space (Win64-only);
// AAPCS64 puts ≤8 GP args in registers and the callee saves its own regs.
//
// These deliberately do NOT establish a setjmp checkpoint or reset context
// state — they are the raw B1 helpers (matching the x86 ember_call_*). The
// keyed safe-call wrappers + checkpoint live in the C++ driver (engine.cpp).

#ifdef __APPLE__  // Mach-O symbol names are prefixed with _

    .text
    .p2align 4

    .globl _ember_call_void_thunk
_ember_call_void_thunk:
    stp     x29, x30, [sp, #-16]!     // save fp, lr (keep sp 16-aligned)
    str     x19, [sp, #-16]!          // save caller's x19 (we install ctx there)
    mov     x19, x1                   // x19 = context_t*
    blr     x0                        // call entry(void) — entry was in x0, no arg
    ldr     x19, [sp], #16            // restore caller's x19
    ldp     x29, x30, [sp], #16       // restore fp, lr
    ret                                 // result in x0

    .p2align 4
    .globl _ember_call_i64_thunk
_ember_call_i64_thunk:
    stp     x29, x30, [sp, #-16]!
    str     x19, [sp, #-16]!
    mov     x9,  x0                   // x9 = entry (caller-saved scratch)
    mov     x19, x1                   // x19 = context_t*
    mov     x0,  x2                   // x0 = a (first script arg)
    blr     x9                        // call entry(a)
    ldr     x19, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

    .p2align 4
    .globl _ember_call_i64_i64_thunk
_ember_call_i64_i64_thunk:
    stp     x29, x30, [sp, #-16]!
    str     x19, [sp, #-16]!
    mov     x9,  x0                   // x9 = entry
    mov     x19, x1                   // x19 = context_t*
    mov     x0,  x2                   // x0 = a (first script arg)
    mov     x1,  x3                   // x1 = b (second script arg)
    blr     x9                        // call entry(a, b)
    ldr     x19, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

#endif // __APPLE__
