// src/darwin_arm64_ctx_switch.S — Darwin ARM64 cooperative coroutine context
// switch (plan_MACOS_ARM64.md Phase 8). Replaces Windows fibers
// (CreateFiber/SwitchToFiber) on Apple Silicon. `ucontext` is deprecated /
// problematic on Apple and is deliberately NOT used (plan §8, §5).
//
// ember_ctx_switch(CoroCtx* from x0, CoroCtx* to x1):
//   Save the CURRENT callee-saved GP regs (x19-x28) + FP (x29) + LR (x30) +
//   SP into `from`, then load `to`'s saved regs + SP + LR and `ret` into `to`'s
//   saved LR (the resume PC). This is the standard symmetric cooperative
//   switch: a single function both saves `from` and restores `to`.
//
//   A fresh coroutine is started by giving its CoroCtx an initial LR
//   (regs[11]) = a C trampoline and an initial SP = the 16-aligned top of its
//   private mmap'd stack. The first ember_ctx_switch into it `ret`s to the
//   trampoline, which runs the JIT'd entry and, on completion or yield, calls
//   ember_ctx_switch back to the caller's saved context.
//
// CoroCtx layout (must match src/runtime_extension_state.hpp `struct CoroCtx`):
//   regs[0]  = x19      regs[1]  = x20      ...      regs[9]  = x28
//   regs[10] = x29 (FP) regs[11] = x30 (LR / resume PC)
//   sp       = saved SP (kept 16-byte aligned; the switch preserves alignment)
//
// AAPCS64 constraints honored:
//   * Only callee-saved GP regs (x19-x28, x29, x30) are switched — the JIT'd
//     entry + the C trampoline manage their own caller-saved state per call.
//   * x18 is Apple's PLATFORM register and is NOT saved/restored (the coroutine
//     does not own it; restoring a stale x18 would corrupt Apple runtime state).
//   * SP is read with `mov x9, sp` and written with `add sp, x9, #0` (reg 31 =
//     SP in ADD). The caller arranges a 16-byte-aligned initial SP; every
//     frame pushed by the trampoline / JIT is 16-aligned, so SP stays aligned
//     at every switch.
//   * The switch passes NO arguments and returns NO value in x0; yield/resume
//     values flow out-of-band through Coroutine::yield_value (the same model
//     the Windows fiber path uses). x0/x1 are scratch here and not preserved.

#ifdef __APPLE__  // Mach-O symbol names are prefixed with _

    .text
    .p2align 4

    .globl _ember_ctx_switch
_ember_ctx_switch:
    // --- save `from` (x0): x19-x28, x29, x30 ---
    stp     x19, x20, [x0, #0]       // regs[0], regs[1]
    stp     x21, x22, [x0, #16]      // regs[2], regs[3]
    stp     x23, x24, [x0, #32]      // regs[4], regs[5]
    stp     x25, x26, [x0, #48]      // regs[6], regs[7]
    stp     x27, x28, [x0, #64]      // regs[8], regs[9]
    stp     x29, x30, [x0, #80]      // regs[10] (FP), regs[11] (LR)
    mov     x9,  sp                  // read current SP
    str     x9,  [x0, #96]           // from->sp

    // --- load `to` (x1): x19-x28, x29, x30 ---
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9,  [x1, #96]           // to->sp
    add     sp,  x9,  #0             // install target SP (reg 31 = SP in ADD)

    ret                             // -> loaded x30 (the resume PC / trampoline)

#endif // __APPLE__
