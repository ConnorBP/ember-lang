# Final Red Team: Sandboxing Security Audit — ember

**Date:** 2026-07-11
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** acf01d0
**Scope:** READ-ONLY audit of the sandbox guarantees documented in `docs/spec/SAFETY_AND_SANDBOX.md` — the per-frame byte budget (§3), the combined call-stack depth guard (§4), the trap/checkpoint unwind (§2/§7), the call-target provenance guard (§7a), the `PERM_FFI` gating (§6), and the new features (constexpr, lambdas, coroutines, exceptions/try-catch-throw, in-context threads) for new sandbox-escape vectors (§8/§8a + the Tier 4 / #20 / #21 addons).
**Auditor posture:** READ-ONLY. No source files edited. No probes added to tracked source. This document is the deliverable.
**Relationship to prior audits:** Complements `SECURITY_AUDIT_2026-07-11.md` (JIT sandbox / .em loader / W^X / trap model / type safety), `EM_FORMAT_RED_TEAM_2026-07-11.md` (.em format attack surface / v5 IR validator / `PERM_FFI` load-side enforcement), and `FINAL_GENERAL_REDTEAM_2026-07-11.md` (C++ compiler/runtime source). Those audits closed the v5 IR `frame_off` stack-smash (FINDING A), the `PERM_FFI` hand-crafted-`.em` bypass (Finding B), and the raw-x86 default-reject (FIX 3). This audit re-verifies those closures from the sandbox-guarantee angle and examines the **feature-tier escape surface** (lambdas / coroutines / try-catch / threads) that the prior audits did not cover as sandbox guarantees.

---

## Executive Summary

The sandbox machinery documented in `SAFETY_AND_SANDBOX.md` is, where it is enabled, **correctly implemented**: the per-frame byte budget charges at function entry and every loop back-edge (including the `while (true) { continue; }` charged-latch case), saturates at `INT32_MAX` so a large legal function cannot wrap into a negative charge, and traps via the shared non-local unwind; the combined call-depth guard wraps every script/native/cross-module/indirect call path in both the tree-walker and the v5 IR re-emit; the call-target provenance guard validates range + allowlist bit at every indirect call site; and `PERM_FFI` is enforced at three independent gates (sema, `.em` v2–v4 bind, `.em` v5 IR rebind) with a secure default of `module_permissions = 0`. The `.em` attack surface is closed: raw-x86 formats are rejected by default, v4 requires Ed25519 verification against a trusted keyring **before** any executable page is allocated, and the v5 IR validator now bounds `frame_off` regardless of `frame_size` (FINDING A fix verified in place at `src/thin_ir_ser.cpp:657-672`).

The audit nonetheless finds **four sandbox-guarantee violations in the shipped feature tiers**, all of them new escape surfaces introduced by the Tier 4 / #20 / #21 features or by the default-off posture of the core guarantees:

| # | Severity | Finding | Area | Sandbox guarantee broken |
|---|----------|---------|------|--------------------------|
| S1 | **HIGH** | Per-frame byte budget and stack-depth guard are **OFF by default** — a host using the library API with `CodeGenCtx` defaults gets no budget and no depth protection | `src/codegen.hpp:100,109` | §3 / §4 (the guarantees are opt-in, not default-on) |
| S2 | **HIGH** | Lambda `env_ptr` escape — a lambda value carries a raw stack pointer; sema has no guard preventing it from outliving its frame (return / global-store / retains all check `is_slice` only, not `is_lambda`) | `src/sema.cpp:2797,2380,1992,2021`; `src/codegen.cpp:1845-1934` | §1 "no raw pointer exposed to script" + "buggy script cannot corrupt host memory" |
| S3 | **HIGH** | `catch_bufs` overflow — `try` indexes `catch_bufs[catch_depth]` and increments `catch_depth` with **no bounds check** against `MAX_CATCH_DEPTH=256`; no sema nesting limit; 257 active try blocks (reachable within `max_call_depth=512`) write out of bounds into `context_t` | `src/codegen.cpp:4254-4289`; `src/context.hpp:118-122` | §1 "buggy script cannot corrupt host memory" (struct corruption) |
| S4 | **MEDIUM** | Coroutine checkpoint misrouting — while a coroutine is suspended at a `yield`, `ctx->checkpoint` points at the trampoline's `setjmp` on the coroutine's stack; a caller trap longjmps there instead of the host checkpoint, so the host never observes the trap and the caller's script is time-warped; if the fiber was reset first, the longjmp lands on freed stack | `extensions/coroutine/ext_coroutine.cpp:179-205` (trampoline) vs `:120-125` (yield) | §2 "a trap always unwinds to the host without killing the process" |
| S5 | **MEDIUM** | Trap is process death without an explicit host checkpoint + trap stub — the raw `ember_call_void`/`ember_call_i64` thunks establish neither; `emit_trap` falls back to `ud2` when `trap_stub` is null and the stub `abort()`s when `has_checkpoint` is false | `src/engine.cpp:222-247`; `src/codegen.cpp:358-376`; `examples/ember_cli.cpp:120-129` | §2 (recoverability is host-opt-in, not a default guarantee) |
| S6 | **MEDIUM** | Call-target guard is a **no-op when no allowlist is configured** — `fn_slot_count == 0` short-circuits the guard to zero emitted bytes; there is no compile error if a module uses function refs without wiring the allowlist | `src/codegen.cpp:517`; `src/thin_emit.cpp:300` | §7a (the runtime last line is silently absent) |
| S7 | **LOW** | In-context thread contract requires the host to lock `ctx->call_mutex` around the outer `ember_call`; the raw thunk does not enforce it, so `thread_join`'s `call_mutex.unlock()` is UB on a host that forgot | `extensions/thread/ext_thread.cpp:284,300`; `src/engine.cpp:222-247` | §8a (host-misconfiguration UB, documented but unenforced) |

**Confirmed SAFE (no new escape vector):** `PERM_FFI` gating (§6) — enforced at sema + both `.em` load gates, secure default, not bypassable; the call-target guard **when configured** (§7a) — range + bitset, both indirect paths, tree-walker and IR; `constexpr` — compile-time fold bounded at 256 recursion / 100k total iterations, runs in the compiler not the sandbox; in-context **threads** frame access — separate OS stacks, no script primitive to read another thread's frame, `call_mutex` serializes context fields, checkpoint swap is clean because the caller is blocked while it is clobbered (the key contrast with S4).

---

## Finding S1 — Per-frame byte budget and stack-depth guard are OFF by default

### What the spec promises (`SAFETY_AND_SANDBOX.md` §3, §4)

§3: "A buggy or slow script cannot hang the game loop forever" — enforced by a per-frame byte budget charged at function entry and loop back-edges. §4: "A buggy script cannot ... crash the process" via stack overflow — enforced by a combined call-depth guard at every script-issued call.

### What the code does

`CodeGenCtx` (`src/codegen.hpp:100,109`) ships both guards **default-off**:

```cpp
bool emit_budget_checks = false;
bool emit_depth_checks = false;
```

`emit_budget_check` (`src/codegen.cpp:397-416`) and `emit_depth_check` (`src/codegen.cpp:442-469`) both early-return on these flags, so a module compiled with the library API defaults emits **zero** budget instructions and **zero** depth instructions. `ember_call_void`/`ember_call_i64` (`src/engine.cpp:222-247`) are raw B1 thunks that install `r14` and call — they do not enable anything. There is no `ember_compile` entry point that turns the guards on; the host must build a `CodeGenCtx` and set the flags themselves.

The reference host (`examples/ember_cli.cpp:508-510`) does opt in:

```cpp
ctx.emit_budget_checks = opts.emit_em_path.empty();
ctx.emit_depth_checks  = opts.emit_em_path.empty();
ectx.budget_remaining  = 100000000;
ectx.max_call_depth    = 512;
```

…but this is the CLI's choice, not the engine's default. A host embedding ember via the library API (the documented integration path, `docs/guide/20-api/`) that constructs a `CodeGenCtx` and calls `compile_func` per function without touching the flags gets a module in which:

- a `while (true) {}` loop runs forever (no budget, no wall-clock check — §3 explicitly defers wall-clock to the host, and the host got no budget to calibrate it against), and
- unbounded recursion (`fn f() { f(); }`) grows the C++ stack until the OS guard page fires (§4's "SEH-based overflow handling is a documented defense-in-depth backstop only" — i.e. the only remaining protection is the fragile SEH path the explicit counter was meant to replace).

### Is the mechanism correct when enabled? Yes.

When `emit_budget_checks = true`:
- The entry charge (`src/codegen.cpp:4924`) runs `emit_budget_check(block_cost(f.body), ...)` after the frame/params are established and before the body. `block_cost` (`src/codegen.cpp:4466`) is reach-aware: it counts statement nodes plus the emitted work they reach (expression operands, native-call setup + per-arg marshalling, aggregate byte copies, switch/match compare chains, for init/step). It floors at 1 and `cost_add` (`src/codegen.cpp:4452-4455`) saturates at `INT32_MAX`, so a large legal AST cannot truncate into a negative or small `imm32` charge. The emitted sequence is `sub qword [r14+off_budget], imm32; jg .continue; emit_trap(BudgetExceeded)` — signed `jg` against the post-subtract result, so a budget driven ≤ 0 traps.
- Every loop back-edge charges `block_cost(loop_body)`: while (`src/codegen.cpp:3851`), do-while (`:3858`), for (`:4051`), for-each (`:3938,4007`). The `while` loop's `continue` target is the **charged latch** (`loops.push_back({latch, end, ...})` at `:3838`; the `ContinueStmt` handler at `:4420` jumps to `loops.back().cont` = `latch`), so `while (true) { continue; }` charges every iteration rather than bypassing the charge via the condition. `for`-`continue` charges through `step_l` and `do-while`-`continue` through `cond` — both already charged.
- The v5 IR re-emit path mirrors this: `thin_lower.cpp:1103` charges at entry, `:2656,2687,2733` charge at back-edges, `thin_emit.cpp:820-823` emits the entry charge.

When `emit_depth_checks = true`:
- `emit_depth_check` emits `inc dword [r14+off_depth]; mov eax, [r14+off_max_depth]; cmp [r14+off_depth], eax; jl .ok; emit_trap(StackOverflow)` (B1 mode, `src/codegen.cpp:447-468`). In B1 mode the max is loaded **per-context** from `[r14+off_max_depth]`, so two contexts sharing one compiled body observe different runtime limits. The `jl` (signed less-than) means `depth < max` continues; the call that increments `depth` to `max` traps, so `max_call_depth = 512` permits 511 active edges (a conservative off-by-one, not a security issue).
- Every script-issued call path is wrapped: direct script calls (`src/codegen.cpp:1517,3049`), cross-module calls (`:1511,3020`), indirect/function-ref calls (`:2800,3020`), lambda calls (`:2800`), and native calls via `emit_counted_native_call` (`:493-496`, which also covers hidden-pointer struct returns, overloads, and string helpers). `emit_depth_leave` decrements after normal return. The v5 IR path emits `DepthCheck` ThinInstrs at the same sites (`thin_lower.cpp:1342,1427,2351,2362,2366,2371`).

### Verdict

The guarantees are real and correctly engineered, but they are **opt-in**. The spec frames them as shipped boundaries ("the shipped safety boundary"); the code frames them as opt-in cost ("a module-level compile flag … hosts that don't need budgets at all … can compile without the checks for zero overhead — explicit opt-in cost", §3). Both framings are internally consistent, but a host reading only §3/§4's "shipped" language and using the library API defaults is **unprotected**. The CLI is safe; a naïve embed is not.

### Recommendation

- Ship a `CodeGenCtx::safe_defaults()` helper (or an `ember_compile` entry point) that sets `emit_budget_checks = emit_depth_checks = use_context_reg = true`, a non-null `trap_stub`/`trap_ctx`, and a sane default `max_call_depth`, so the default integration path is the safe one. Keep the raw flags for the trusted-internal-tool opt-out the spec describes.
- Document in `docs/guide/20-api/` that the raw `ember_call_*` thunks + default `CodeGenCtx` are the **trusted-script** path (no budget, no depth, no recoverable traps), and that the sandboxed path requires the flags + a host checkpoint wrapper.

---

## Finding S2 — Lambda `env_ptr` escape: a captured stack pointer can outlive its frame

### The escape surface

A lambda value is a 16-byte pair `{fn_slot, env_ptr}` (`src/ast.hpp:59-68`, `src/types.cpp:30`). `env_ptr` is the address of a **compiler-hidden frame temp** allocated in the creating function's stack frame:

```cpp
// src/codegen.cpp:1852-1861
// (v1: stack-allocated env — the lambda must not outlive this frame.)
...
env_off = alloc_local(name, arr_temp_types.back().get());   // a frame slot
...
// src/codegen.cpp:1889-1893
if (le->env_size > 0) {
    e.mov_reg_reg(Reg::rdx, Reg::rbp);
    e.add_reg_imm32(Reg::rdx, env_off);          // rdx = env_ptr = lea [rbp + env_off]
} else {
    e.mov_reg_imm64(Reg::rdx, 0);
}
non_serializable_reason = "lambda env is a stack-frame-local allocation";
```

`env_ptr` is a raw `rbp`-relative stack pointer. Inside the lambda body, every capture access dereferences it: `load env_ptr from [rbp+__env_off]; load the value at [env_ptr + offset]` (`src/codegen.cpp:136-137`). The codegen comment at `:1852` is the **only** acknowledgment of the lifetime constraint: *"the lambda must not outlive this frame."* Nothing enforces it.

### Why sema does not catch the escape

The slice-escape safety system (Stage 1 + Stage 2, `src/sema.cpp:380-411,947-1037`) is the obvious place a lambda-escape guard would live. It tags stack-backed slices with `local_array_view` and rejects three escape sites:

- **C1 return** (`src/sema.cpp:2797`): `if (vt && vt->is_slice && is_local_array_view(*rs->value)) err("cannot return a slice/view derived from a stack local", ...);`
- **C2a global-Ident-store** (`src/sema.cpp:2380-2383`): `const bool local_view = rt && rt->is_slice && is_local_array_view(*a->value); ... if (gi != globals.end() && local_view) err("cannot store a slice/view derived from a stack local in a global", ...);`
- **C2b global-rooted field/element store** (`src/sema.cpp:2387-2407`): `... else if (rt && rt->is_slice && is_local_array_view(*a->value)) { ... root-chase ... if global → err(...); }`
- **C3 native retain** (`src/sema.cpp:1992,2021`): `if (nit->second.retains && got && got->is_slice && is_local_array_view(...)) err(...);`

**Every one of these guards tests `is_slice`. None tests `is_lambda`.** A lambda value flowing into any of these escape sites is not flagged. `check_func` (`src/sema.cpp:3153-3173`) does not reject an `is_lambda` return type. `check_lambda` (`src/sema.cpp:3480-3488`) builds the lambda type with `is_lambda = true` and a recorded sig, with no escape analysis. `Type::byte_size()` returns 16 for `is_lambda` (`src/types.cpp:30`), so a lambda-typed global gets a real 16-byte slot in the globals block (`src/globals.hpp` does not special-case `is_lambda`), and `types_compatible` accepts `is_lambda` against `is_lambda`/`is_fn_handle` (`src/sema.cpp:507-511`).

### Reachable escape shapes

1. **Return a lambda from its creating function.** `fn make() -> fn(i64)->i64 { let c = 40; return fn(x: i64) -> i64 { return x + c; }; }` — sema accepts it (return guard checks `is_slice` only). `make`'s frame is torn down on return; the returned `{slot, env_ptr}` carries `env_ptr = lea [rbp + env_off]` into a dead frame. Calling the returned lambda dereferences a dangling stack pointer.
2. **Store a lambda in a global.** `global g : fn(i64)->i64; fn setup() { let c = 7; g = fn(x) => x + c; }` — sema accepts it (global-store guard checks `is_slice` only). After `setup` returns, `g.env_ptr` dangles. A later `g(1)` reads stale stack.
3. **Pass a lambda to a `retains=true` native.** The retains guard (`src/sema.cpp:1992,2021`) checks `is_slice` only; a retaining native that stores the lambda keeps a dangling `env_ptr`.
4. **Hand a lambda to a coroutine / thread.** A lambda captured into a coroutine or spawned thread that outlives the creating frame dangles the same way.

### Impact

This is a **use-after-scope on the host stack through a script-controlled pointer**. The lambda body reads (and, for captured-mutable patterns the language may grow, writes) `[env_ptr + offset]` where `env_ptr` is a stale stack address. Whatever the host later places at that stack address — return addresses, saved registers, other frames' locals — is read as the lambda's captured values. The sandbox's core promise (`§1`: "no raw pointer arithmetic exposed to script … a buggy script cannot corrupt host memory") is broken: the lambda `env_ptr` **is** a raw host-stack pointer, and the script controls when it is dereferenced (by calling the escaped lambda after the frame is gone). At minimum this leaks host stack contents into script-visible values; in patterns that write through `env_ptr` (capture-by-reference growth, or a future mutable-capture feature) it is a stack-write primitive.

### Contrast with the slice system

The slice system exists **precisely** because a slice `{ptr, len}` carries a raw pointer and must not outlive its backing frame. A lambda `{slot, env_ptr}` is the same shape with the same hazard, and the guards that close the slice hole do not close the lambda hole because they predicate on `is_slice`.

### Recommendation

- **Immediate (defense-in-depth):** add `is_lambda` to the three escape guards. A lambda whose `env_size > 0` (it has a real env_ptr, not the null-env `&fn`-handle case) is exactly analogous to a `local_array_view` slice:
  - C1 return: reject returning a lambda with `env_size > 0` from a function whose frame does not outlive the call (conservatively: reject returning any `env_size > 0` lambda, as the tree-walker does for stack-backed slices).
  - C2a/C2b global-store: reject storing an `env_size > 0` lambda into a global or a global-rooted field/element.
  - C3 retains: reject passing an `env_size > 0` lambda to a `retains=true` native.
- **Structural (the real fix):** heap-allocate the env (or promote it to a GC-managed object per `docs/spec/MEMORY_AND_GC.md`) so a lambda value is self-owned and cannot dangle. The v1 "stack env" is the root cause; the escape guards are a stopgap matching the slice system's v1 posture. The codegen comment at `:1852` already names this as the constraint to lift.

---

## Finding S3 — `catch_bufs` overflow: 256+ nested `try` blocks write out of bounds in `context_t`

### The overflow

`context_t` (`src/context.hpp:118-122`) holds a fixed catch stack:

```cpp
static constexpr int32_t MAX_CATCH_DEPTH = 256;
int64_t catch_bufs[MAX_CATCH_DEPTH][8]{};            // 256 × 64 bytes
int32_t catch_saved_call_depths[MAX_CATCH_DEPTH]{};
```

The `try` codegen (`src/codegen.cpp:4254-4289`) uses `catch_depth` (a context field, shared across every script call on the context) as the index into `catch_bufs`, saves callee-saved regs + rsp + the catch-entry rip into `catch_bufs[catch_depth]`, then increments `catch_depth` — with **no bounds check against `MAX_CATCH_DEPTH`**:

```cpp
// src/codegen.cpp:4255-4289 (excerpt)
e.load_reg_mem(Reg::rax, Reg::r14, cd_off);          // rax = catch_depth
e.imul_reg_imm32(Reg::r8, Reg::rax, 64);             // r8 = catch_depth * 64
...                                                  // r9 = &catch_bufs[catch_depth]
e.store_reg_mem(Reg::r9, 0,  Reg::rbx);             // write 8 regs into catch_bufs[catch_depth]
... e.store_reg_mem(Reg::r9, 56, Reg::rax);          // [r9+56] = catch_entry_rip
...
e.add_reg_imm32(Reg::r10, 1);                        // r10 = catch_depth + 1
e.store_reg_mem(Reg::r14, cd_off, Reg::r10);         // catch_depth++
```

There is no `cmp catch_depth, 256; jge trap`. Sema has **no try-nesting limit** (grep for `MAX_CATCH_DEPTH` / `catch_depth` / `256` in `src/sema.cpp` returns only the constexpr recursion bound at `:4000`, unrelated). The spec's only hedge (`src/context.hpp` comment on `MAX_CATCH_DEPTH`): *"a try at depth == MAX is a sema error (or would overflow here) — v1 sets it high enough that a script can't reasonably hit it."* Neither the sema error nor the runtime trap exists.

### Reachability

`catch_depth` is a `context_t` field, so it persists across script-to-script calls (it is not reset on function entry — only `reset_for_call()` clears it, and that is host-invoked after a trap). A `try` entered but not yet completed (the body is still executing) holds its slot. So 256 **active** try blocks — nested across function calls — accumulate `catch_depth` to 256, and the 257th writes `catch_bufs[256]`, which is past the array:

```ember
fn f(n: i64) {
    try {
        if (n > 0) { f(n - 1); }     // recurse INSIDE the try body
    } catch (e: i64) { /* ... */ }
}
fn main() -> i64 { f(256); return 0; }
```

`f(256)` enters try (depth 0→1), calls `f(255)` inside the try body (depth 1→2), … `f(0)` enters try (depth 256→257). The try at depth 256 writes `catch_bufs[256]` — index 256, valid range `0..255`. That is 257 calls (256 down to 0), within the default `max_call_depth = 512` (and trivially within reach if depth checks are off per S1). Each intermediate try is still active (its body has not completed — control is inside the recursive call), so `catch_depth` is never decremented before the 257th try.

### What gets corrupted

`catch_bufs` is followed in `context_t` by `catch_saved_call_depths[256]` and then `call_mutex`. `catch_bufs[256]` is a 64-byte write (8 × `store_reg_mem` of callee-saved regs + rsp + rip) landing on `catch_saved_call_depths[0..]` and onward. The written values are the JIT's current `rbx/rbp/r12/r13/r14/r15/rsp` and a catch-entry rip — attacker-influenced via the script's register state at the try site. This is a **struct-corruption write into `context_t`**. Depending on the host's allocation of `context_t` (stack or heap), this is a stack/heap buffer overflow that can corrupt the depth-snapshot array (causing a later `throw` to restore a wrong `call_depth`), the mutex (UB on next lock), or adjacent host memory.

A symmetric OOB **read** exists on the `throw` path (`src/codegen.cpp:4334-4355`): `throw` reads `catch_bufs[catch_depth-1]` to restore regs + rsp + jump. If `catch_depth` was corrupted upward by the overflow, a throw reads past the array and restores a fabricated `rsp`/`rip` — a controlled stack pivot.

### Verdict

A buggy (or malicious) script can corrupt the `context_t` struct via 257 nested active try blocks. This is a memory-corruption primitive inside the sandbox's own state, violating `§1`'s "a buggy script cannot corrupt host memory." The bound is documented as a sema responsibility that was never implemented.

### Recommendation

- **Runtime trap (minimal):** in the `try` codegen, after loading `catch_depth` and before using it as an index, emit `cmp eax, MAX_CATCH_DEPTH; jge .trap; emit_trap(IllegalInstruction, "try nesting exceeded MAX_CATCH_DEPTH")`. This makes the 257th try a recoverable trap instead of an OOB write. Mirror it in the v5 IR `thin_emit` try path if/when try/catch is lowered to IR.
- **Sema limit (defense-in-depth):** add a try-nesting counter to the sema walk (`check_stmt`'s `TryCatchStmt` branch) and reject at a limit matching `MAX_CATCH_DEPTH` (e.g. 256), so the over-deep program never reaches codegen.
- **`context_offsets` constant:** bake `MAX_CATCH_DEPTH` into the codegen via `context_offsets` rather than the magic `64` stride + raw `256`, so the bound and the array stay in one place.

---

## Finding S4 — Coroutine checkpoint misrouting: a caller trap is swallowed while a coroutine is suspended at a `yield`

### The mechanism

The coroutine trampoline (`extensions/coroutine/ext_coroutine.cpp:172-205`) runs the coroutine's fn via its own `ember_call_i64` on the **shared** `context_t*` (`g_ctx`, the caller's context). To isolate coroutine traps, it snapshots the caller's per-call state and **overwrites `ctx->checkpoint` with its own `setjmp`**:

```cpp
// ext_coroutine.cpp:188-205 (excerpt)
ctx->has_checkpoint = true;
if (__builtin_setjmp(ctx->checkpoint)) {
    trapped = true;  reason = int(ctx->last_trap);  ctx->has_checkpoint = false;   // trap path
} else {
    result = ember_call_i64(co->entry, ctx, co->arg);                              // fn runs
    ctx->has_checkpoint = false;
}
restore_state(ctx, saved);   // restore caller's checkpoint — ONLY on done/trap
... SwitchToFiber(caller);
```

The snapshot is restored by `restore_state` **only when the coroutine finishes** (fn returns or traps). While the coroutine is **suspended at a `yield`**, the fn has not returned — `ember_call_i64` is still on the coroutine's stack (frozen) — so `restore_state` has not run. For the entire suspend window, `ctx->has_checkpoint = true` and `ctx->checkpoint` points at the trampoline's `setjmp` **on the coroutine's stack**.

### The misroute

`__ember_coro_yield` (`ext_coroutine.cpp:215-231`) does `SwitchToFiber(caller)`, returning control to the caller's `coroutine_next`, which returns the yielded value to the caller's script. The caller's script then **continues executing**. If it traps in that window — a bounds check, a budget exhaustion, a div-by-zero, an unhandled throw — the trap stub does:

```cpp
// examples/ember_cli.cpp:120-125
if (ctx->has_checkpoint) {
    __builtin_longjmp(ctx->checkpoint, 1);   // ctx->checkpoint == trampoline's setjmp
}
```

The longjmp restores `rsp` to the **coroutine's** trampoline frame (a different stack from the caller's current execution) and resumes the trampoline's setjmp-nonzero path. The trampoline sets `trapped = true`, runs `restore_state` (which finally restores the caller's *original* checkpoint), marks the coroutine done, and `SwitchToFiber(caller)`. The caller's fiber resumes at its frozen point **inside `n_coroutine_next`** (the fiber's saved context is from the `SwitchToFiber` that yielded) — so `coroutine_next` returns `TRAP_SENTINEL` to the script **at the `coroutine_next` call site**, not at the trap site. The script's frames between `coroutine_next` and the trap are abandoned.

### Consequences

1. **The host never observes the trap.** The host's own `setjmp` checkpoint (the one the host established before the outer `ember_call`) is never longjmp'd to. `last_trap`/`last_error` are set on the context, but the host's recovery branch (the `if (__builtin_setjmp(...))` in `ember_cli.cpp:678` or equivalent) does not run. The host's error-handling/logging policy is bypassed — the trap is silently swallowed by the coroutine's trampoline. This breaks `§2`'s guarantee that "a trap always unwinds to the host."
2. **The caller's script is time-warped.** It resumes at the `coroutine_next` call site (receiving `TRAP_SENTINEL`) rather than at the trap site, with local state from the `coroutine_next` call, not from the trap site. This is a silent correctness corruption — the script is in an inconsistent state the host did not sanction.
3. **Use-after-free if the fiber was reset.** `coroutine_reset` (`ext_coroutine.cpp:299-318`) `DeleteFiber`s suspended coroutines (the comment says this is "safe — the fiber is simply never resumed"). But `ctx->checkpoint` still points into the deleted fiber's stack. If the caller traps after a reset (e.g. the host reset between runs but the caller's `ember_call` is still on the stack with a stale `ctx->checkpoint`), the longjmp lands on **freed memory** → use-after-free / crash. The reset-suspended-fiber path is safe only if the caller never traps afterward, which the checkpoint clobbering makes impossible to guarantee.

### Why the thread extension does NOT have this problem (the key contrast)

The in-context thread worker (`extensions/thread/ext_thread.cpp:154-185`) also overwrites `ctx->checkpoint` with its own `setjmp`. The difference is what happens to the **caller** while the checkpoint is clobbered:

- **Threads:** `thread_join` (`ext_thread.cpp:280-300`) **releases `call_mutex`** and blocks in `cv.wait`. The caller's `ember_call` is frozen inside `n_thread_join` — it is **not executing script code** while the worker holds the mutex. The worker restores the caller's checkpoint via `restore_state` **before** it unlocks + notifies. By the time the caller wakes, reacquires `call_mutex`, and returns from `thread_join`, `ctx->checkpoint` is back to the caller's original. The caller can never trap into a clobbered checkpoint because it is blocked for the entire clobber window.
- **Coroutines:** `yield` returns control to the caller **without restoring the checkpoint**. The caller executes script code (and can trap) for the entire suspend window with a clobbered checkpoint.

So S4 is coroutine-specific. The thread model is the correct one (swap-and-block); the coroutine model swaps-and-resumes, leaving the checkpoint clobbered across the resume.

### Verdict

A trap in the caller while a coroutine is suspended at a `yield` does not unwind to the host; it misroutes into the coroutine's trampoline, the host loses the trap, and the caller's script is silently time-warped (or, after a reset, the longjmp hits freed memory). This violates `§2`'s recoverable-unwind guarantee for the caller.

### Recommendation

- **Restore the caller's checkpoint across each yield.** `__ember_coro_yield` (or the trampoline's yield-return path) should `restore_state(ctx, saved)`'s checkpoint portion before `SwitchToFiber(caller)`, and re-save+re-setjmp on resume. Equivalently: the trampoline should save/restore `ctx->checkpoint` around each `SwitchToFiber` boundary, not just around the whole fn. This makes a caller trap longjmp to the caller's checkpoint (correct) and a coroutine-internal trap longjmp to the trampoline (correct) — the two are distinguished by which checkpoint is live at the moment of the trap.
- **Alternatively (simpler, coarser):** give the coroutine its **own** `context_t` (separate checkpoint/budget/depth) rather than sharing the caller's, so the two never alias a checkpoint. This mirrors the multi-context model (`§8a`) and removes the save/restore dance entirely; the cost is a per-coroutine context allocation.
- **Defense-in-depth:** `coroutine_reset` should refuse to `DeleteFiber` a suspended coroutine whose `ctx->checkpoint` still points into its stack while a caller `ember_call` is live, or at minimum document that the host must not reset with a live caller call.

---

## Finding S5 — Trap is process death without an explicit host checkpoint + trap stub

### What the code does

`emit_trap` (`src/codegen.cpp:358-376`) has two paths:

```cpp
void emit_trap(int reason_ord, const char* detail) {
    if (ctx.trap_stub) {
        ... call the stub (Win64 ABI: rcx=ctx, edx=reason, r8=detail) ...
    } else {
        e.byte(0x0F); e.byte(0x0B); // ud2 (raises #UD, pre-v0.4 trap)
    }
}
```

If the host did not set `ctx.trap_stub`, every trap (bounds, budget, depth, div-by-zero, bad-call-target, unhandled-throw) emits `ud2` → `#UD` → process death (no VEH installed by default). `CodeGenCtx::trap_stub` defaults to `nullptr` (the field is uninitialized-by-default in the aggregate; the CLI sets it at `ember_cli.cpp:506,1030`).

If the host *did* set `trap_stub` but did not establish a checkpoint, the stub (`ember_cli.cpp:111-129`) does:

```cpp
extern "C" void ember_cli_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = ...; ctx->last_error = ...;
        if (ctx->has_checkpoint) {
            __builtin_longjmp(ctx->checkpoint, 1);
        }
    }
    std::fprintf(stderr, "ember: unhandled trap (no checkpoint): %s\n", ...);
    std::abort();
}
```

`has_checkpoint` defaults to `false` (`src/context.hpp`), and the raw `ember_call_void`/`ember_call_i64` thunks (`src/engine.cpp:222-247`) do **not** set it. So a host that calls the raw thunk without wrapping it in a `setjmp` gets `abort()` on the first trap.

### Verdict

A trap unwinds to the host **only if** the host set both `trap_stub` and a `setjmp` checkpoint. With library-API defaults (`trap_stub = null`, `has_checkpoint = false`), every trap is process death — exactly the `SIGSEGV`/`SIGILL` → crash the trap model was designed to replace (`REDSHELL V6-DoS / V7`, per the CLI comment). `§2` is explicit about this ("Calling trap-capable code through a raw thunk without a checkpoint is not recoverable"), so this is **documented**, but it means the answer to the audit question *"does a trap always unwind to the host without killing the process?"* is **no, not by default** — only under the host's explicit opt-in. The spec's framing ("The shipped `ember_call_void` and `ember_call_i64` functions are raw B1 thunks … they do not establish a recoverable resume point") is honest about this; the risk is that a host reading only the §7 "all traps funnel through one unwind primitive" language assumes recoverability is automatic.

### When the unwind IS established (the correct path), it is sound

When the host wraps the call (as the CLI does at `:678`, `:1207`, `:1291`):

```cpp
ectx.has_checkpoint = true;
if (__builtin_setjmp(ectx.checkpoint)) { /* trap recovery */ }
else { ember::ember_call_void(entry, &ectx); }
ectx.has_checkpoint = false;
```

…the stub's `__builtin_longjmp` (not `std::longjmp` — JIT'd frames have no `.pdata`, so the libc table walk would fault; the builtin is a direct register restore) lands in the host's recovery branch. This path is correct. `reset_for_call()` (`src/context.hpp:149-167`) clears the abandoned `call_depth`/`catch_depth`/`thrown_value` after the longjmp (the balanced `dec`s on abandoned frames cannot run). The one subtlety is a **native→script→native C++ exception** (`§2` last paragraph): a real C++ exception thrown across the boundary is undefined by ember (natives are trusted not to throw; if they must, they catch internally and translate to `runtime_exception()`). No top-level `catch(...)` wraps JIT'd code in v1 — documented as a known boundary, not silently handled.

### Recommendation

- Ship the safe-call wrapper the spec defers ("A host may wrap that pattern in its own safe-call API … a nested checkpoint stack and a core safe-call wrapper remain deferred"). Until then, the `docs/guide/20-api/` host-integration guide must lead with the checkpoint+stub boilerplate, not the raw thunk.
- Consider defaulting `CodeGenCtx::trap_stub` to a built-in `abort`-on-no-checkpoint stub (the current `ud2` fallback gives no diagnostic; a default stub at least prints the trap reason before dying).

---

## Finding S6 — Call-target guard is a no-op when no allowlist is configured

### What the code does

`emit_call_target_guard` (`src/codegen.cpp:516-565`) and its IR mirror (`src/thin_emit.cpp:299-317`) both short-circuit to zero emitted bytes when the allowlist is unconfigured:

```cpp
void emit_call_target_guard() {
    if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;  // no allowlist = no guard
    ...
}
```

The spec (`§7a`) documents this: *"The guard is a no-op (zero emitted) when no allowlist is configured (`fn_slot_count == 0`), so every existing module that doesn't use function refs pays nothing."* That is a correct performance posture for modules that do not use function refs. The problem is the **failure mode when a module DOES use function refs but the host forgot to wire the allowlist**: there is no compile error, no warning — the indirect call sites emit `call [dispatch_base + handle*8]` with **no validation**.

### The residual exposure

Sema's compile-time first line (`src/sema.cpp`) forbids `i64 ↔ fn` assignment either direction, so a script cannot forge a fn handle by writing an arbitrary `i64` into a `fn`-typed slot. The only ways to produce a fn-typed value are `&fn` (a known slot) or a native that returns `fn`. With no runtime guard:

- A native that returns a `fn` handle that is **out of range** (a buggy host binding, or a handle format mismatch) is used unchecked as `call [dispatch_base + handle*8]`. The dispatch table is a `vector<atomic<void*>>` (`src/dispatch_table.hpp`) sized to the slot count; an out-of-range handle reads past the vector → OOB read → wild `call`. This is a crash (DoS) and potentially a controlled call target if the read lands on attacker-influenced memory.
- A **stale handle after a hot-reload slot swap** (`docs/HOT_RELOAD.md §5`) points at a slot whose entry was republished or whose allowlist bit is now clear. With no guard, the stale handle calls whatever is now at that slot (the new code, or `nullptr` → `call [nullptr]` fault). The guard's job is to catch exactly this; absent the guard, the stale handle is honored.
- The **bare-`fn` signature hole** the spec names (`ROADMAP` Tier 2): a `fn`-typed param with no recorded sig does not type-check args. The guard still validates the handle *when present*; with the guard absent, the hole widens from "type-soundness hole, not a sandbox violation" to "no validation at all."

### Verdict

The guard is correct and complete **when configured** (range `cmp rax, slot_count; jae trap` + bitset `bt [allowlist + handle>>3], handle&7; jnc trap`, both indirect paths — the lambda call at `src/codegen.cpp:2802` and the function-ref call at `:2880` — and the cross-module indirect dispatch at `:521-565`; the IR path lowers a `CallTargetGuard` ThinInstr before each `CallIndirect` at `thin_lower.cpp:2286` and emits it at `thin_emit.cpp:1507,1867`). The finding is that **the opt-in is silent**: a host using function refs without `fn_allowlist_base`/`fn_slot_count` gets no guard and no diagnostic. `§7a`'s "runtime last line" is silently absent.

### Recommendation

- Emit a **compile-time warning** (or, for a sandboxed compile, an error) when a module contains an indirect call site (`&fn` / lambda call / `fn`-typed-param call) and `fn_slot_count == 0`. The tree-walker knows at `emit_call_target_guard`'s call site whether the guard was skipped; a `non_serializable_reason` or an explicit diagnostic there would surface the misconfiguration.
- Alternatively, when `fn_slot_count == 0` but an indirect call is present, emit a **trap-on-any-indirect-call** stub instead of an unguarded `call`, so a misconfigured module fails closed (traps on every function-ref call) rather than open (honors every handle).

---


## Finding S7 — In-context thread contract requires the host to lock `call_mutex`; the raw thunk does not enforce it

### The contract

The in-context thread addon (`extensions/thread/ext_thread.hpp:67-75`, `ext_thread.cpp:94-185`) serializes script-side execution on a shared `context_t` via `ctx->call_mutex`. The contract is: **every `ember_call` into this context locks `call_mutex`** — the host's outer call AND each spawned thread's call. `thread_join` (`ext_thread.cpp:280-300`) relies on this: it does `g_ctx->call_mutex.unlock()` to let the worker acquire the mutex, then `g_ctx->call_mutex.lock()` before returning. The comment at `:246-250` states it: *"the caller is inside an ember_call that holds call_mutex."*

The reference test (`examples/in_context_threads_test.cpp:163`) honors it explicitly:

```cpp
m.ctx.call_mutex.lock();                 // in-context: serialize with spawned workers
... ember::ember_call_void(entry, &m.ctx) ...
m.ctx.call_mutex.unlock();
```

### The gap

`ember_call_void`/`ember_call_i64` (`src/engine.cpp:222-247`) are raw thunks — they install `r14` and call; they do **not** lock `call_mutex`. So a host that uses the in-context thread extension via the raw thunk **without** manually locking `call_mutex` around the outer call violates the contract. The consequence is `thread_join`'s `g_ctx->call_mutex.unlock()` at `:284` **unlocking a mutex that was never locked** — undefined behavior on a `std::mutex` (Windows `SRWLOCK`/`CRITICAL_SECTION` behavior on unbalanced unlock ranges from silent corruption to a fast-fail, but `std::mutex`'s precondition is that the calling thread owns it). Symptomatically this is a host-misconfiguration footgun, not a script-driven escape, but it is **unenforced** by the API the host is most likely to reach for.

### Verdict

Documented (the test + the hpp spell out the contract) but unenforced. A host that reads `ext_thread.hpp` and uses `ember_call_void` directly (the documented raw entry point) without the `call_mutex` boilerplate gets UB the moment a spawned thread joins. This is the same default-off posture as S1/S5: the safe path requires host boilerplate that the raw API does not provide or require.

### Recommendation

- Provide an `ember_call_void_in_context(entry, ctx)` (or have `thread_init` install a context-mode flag) that locks `ctx->call_mutex` around the call when the in-context thread addon is active, so the contract is enforced by the call site rather than by host discipline.
- Document the lock requirement in `docs/guide/20-api/` next to the thread extension, not only in the extension header and the test.

---

## Confirmed SAFE — `PERM_FFI` gating (audit question 5)

`PERM_FFI` (`0x01`, `src/binding_builder.hpp:40`) is enforced at **three** independent gates, all checking `module_permissions & PERM_FFI`:

1. **Compile time (sema):** `src/sema.cpp:1962-1967` — at the call-site sema check, a native flagged `PERM_FFI` is a **compile error** if the compiling module's `perms` lack the bit. `perms` is threaded from `sema(..., permissions, ...)` (`src/binding_builder.hpp:20`, `src/sema.cpp:4252-4259`). Zero runtime cost: a module without the permission literally cannot produce code that calls the function.
2. **`.em` load (v2–v4 bind):** `src/em_loader.cpp:429-435` — `parse_file` checks `NativeSig::permission` against `module_permissions` when binding a native by name. A hand-crafted `.em` that bypasses sema still hits this gate.
3. **`.em` load (v5 IR rebind):** `src/em_loader.cpp:699-716` — the v5 IR re-emitter checks the same when re-binding `CallNative` instrs to host native pointers.

The default is secure: `EmLoadPolicy::module_permissions = 0` (`src/em_loader.hpp:129`), and a null `load_policy` resolves to `module_permissions = 0` (`src/em_loader.cpp:567`). So a host that loads `.em` without explicitly granting FFI rejects every `PERM_FFI`-gated native at all three gates. A hand-crafted `.em` cannot bypass sema because the load-side gates are independent of sema. **No bypass found.** The `call_raw` native (`extensions/call_raw/`) is `PERM_FFI`-gated, so a sandboxed script (no FFI bit) cannot reach it; within FFI, the script is trusted by definition (`§1`). This matches `FINAL_GENERAL_REDTEAM_2026-07-11.md` G3's conclusion.

---

## Confirmed SAFE — call-target guard when configured, constexpr, threads frame access

- **Call-target guard (§7a), when the allowlist is configured:** correct on both indirect paths and both emit paths (see S6 for the missing-allowlist gap). The intra-module guard does range + bitset; the cross-module indirect dispatch (`emit_cross_module_indirect_dispatch`, `src/codegen.cpp:521-565`) validates the handle against the **target** module's allowlist (looked up from the per-module records table) and traps `BadCallTarget` on any mismatch — the REDSHELL #6 invariant lifted cross-module. A forged handle (out-of-range module/slot or unregistered slot) traps, never reaches `call r11` with garbage.
- **`constexpr`:** a compile-time fold (`src/sema.cpp:3708-4030`), bounded at recursion depth 256 (`:4000`) and a **shared total** iteration budget of `CE_MAX_TOTAL_ITERS = 100000` (`:1424`) across all loops at all nesting levels in one eval (so nested loops cannot multiply past it). It runs in the compiler, not the sandbox; a too-deep or too-long constexpr fn falls back to runtime (the `CallExpr` is left as-is). No runtime escape vector; the only residual is compile-time cost, which is capped.
- **In-context threads — frame access (audit question 6):** a spawned thread runs on its **own OS stack** (`std::thread`); there is no script primitive to read another thread's stack frame (no pointer to it is ever exposed to script). The only shared mutable state is `context_t`'s fields (serialized by `call_mutex`) and **globals** (shared mutable, documented as a host-design non-goal in `§8`: *"Script-visible global mutable state shared across contexts is a host-design question, not something ember arbitrates"*). The checkpoint swap is clean because the caller is **blocked** on `call_mutex`/`cv.wait` for the entire clobber window and the worker restores the checkpoint before the caller resumes (the key contrast with S4). A thread cannot access another thread's local frame; it can race on globals, which is the documented non-goal, not a sandbox escape.

---

## Re-verification of prior-audit closures (from the sandbox-guarantee angle)

- **v5 IR `frame_off` stack-smash (SECURITY_AUDIT_2026-07-11 Finding 1 / FINDING A):** FIXED. `validate_thin_function` (`src/thin_ir_ser.cpp:657-672`) now checks `frame_off` for **every** instr that uses it, **regardless of `frame_size`** (the `frame_size > 0` gate that let a `frame_size == 0` IR skip the check — and overwrite `[rbp+8]` = return address — is gone). When `frame_size == 0`, `-frame_size == 0`, so any non-zero `frame_off` fails the range check. Verified in place.
- **`PERM_FFI` hand-crafted-`.em` bypass (EM_FORMAT_RED_TEAM Finding B):** FIXED. All three gates (above) enforce `module_permissions`. Verified in place.
- **Raw-x86 default-accept (EM_FORMAT_RED_TEAM FIX 3):** FIXED. `load_em_bytes_impl` (`src/em_loader.cpp:579-585`) rejects v1–v4 by default (`allow_raw_x86 = false` is the null-policy default); only v5 IR is accepted by default. The check runs **before** the signature/dev-mode policy. Verified in place.
- **v5 IR validator completeness (EM_FORMAT_RED_TEAM Sec-6 / Item 11/12):** FIXED. The validator now rejects duplicate block IDs, negative `len` for `ConstStringRef`/`StringDecrypt`/`BoundsCheck` (preventing the sign-extend bypass of the bounds check), out-of-range `CallScript` slot / `CallCrossModule` mod_id, out-of-range `Cmp` predicate, and rodata OOB. Verified in place at `src/thin_ir_ser.cpp:547-712`.
- **`ext_array` element-bounds multiplication overflow (SECURITY_AUDIT_2026-07-11 Finding 2):** FIXED. `n_array_get/set_*` (`extensions/array/ext_array.cpp:70-75`) now bounds `i` against `bytes.size()/elem_size` (the element count) before the `i*elem_size` indexing, so the multiplication cannot overflow past the allocation (`checked_bytes` caps `bytes` at 2^30). Verified in place.

---

## Audit-question-by-question answers (the seven asked)

| # | Question | Answer |
|---|----------|--------|
| 1 | Per-frame byte budget: enforced? Can a script exceed it? | **Off by default (S1).** When enabled: enforced at function entry + every loop back-edge (incl. `while-continue` charged latch), saturates at `INT32_MAX`, traps on <= 0. A script cannot exceed it when it is on; a script can run unbounded when it is off (the default for library-API hosts). |
| 2 | Stack-depth guard: enforced? Can a script overflow the stack? | **Off by default (S1).** When enabled: enforced at every call path (script/native/cross-module/indirect/lambda), per-context max in B1 mode, traps at `max`. A script cannot overflow via script recursion when it is on; unbounded recursion can hit the OS guard page when it is off (the default). |
| 3 | Trap/checkpoint unwind: does a trap always unwind to the host without killing the process? | **No, not by default (S5).** Only when the host set `trap_stub` + a `setjmp` checkpoint. Defaults: `trap_stub = null` -> `ud2` (process death); stub set but no checkpoint -> `abort()`. The unwind is sound when the host opts in. Coroutine suspension breaks it for the caller even when opted in (S4). |
| 4 | Call-target guard: can a script call an unregistered function? | **Not when the allowlist is configured** — range + bitset trap `BadCallTarget` at every indirect site. **Silently absent when unconfigured (S6)** — no guard, no diagnostic; a bad native-returned or stale handle calls unchecked. Sema's `i64<->fn` prohibition prevents forging, but not stale/out-of-range handles. |
| 5 | `PERM_FFI`: enforced at compile (sema) AND load (em_loader)? Bypassable? | **Yes, enforced at three gates (sema + v2-v4 bind + v5 IR rebind); not bypassable.** Secure default `module_permissions = 0`. (Confirmed SAFE.) |
| 6 | New features introduce escape vectors? Coroutine trap bypass checkpoint? Thread access another thread's frame? Lambda capture escape? | **Yes — three new vectors:** lambda `env_ptr` escape (S2, HIGH), `catch_bufs` overflow (S3, HIGH), coroutine checkpoint misrouting (S4, MEDIUM). A coroutine's own trap is correctly isolated (trampoline setjmp) but a **caller** trap while a coroutine is suspended misroutes (S4). A thread **cannot** access another thread's local frame (separate stacks, no pointer); globals are shared (documented non-goal). A lambda **can** capture a reference that escapes the sandbox (S2). `constexpr` introduces no escape vector (compile-time, bounded). |
| 7 | (Report written + committed) | This document, `docs/audit/FINAL_SANDBOX_REDTEAM_2026-07-11.md`. |

---

## Severity-ordered fix priorities

1. **S3 (`catch_bufs` overflow)** — one runtime `cmp/jge trap` in the `try` codegen + a sema nesting limit. Smallest fix, highest blast radius if left (struct corruption in `context_t`).
2. **S2 (lambda `env_ptr` escape)** — add `is_lambda` to the three slice-escape guards as a stopgap; heap-allocate the env as the structural fix. The stopgap is small and mirrors existing guards.
3. **S1 + S5 + S6 + S7 (the default-off posture)** — ship a `safe_defaults` `CodeGenCtx` / safe-call wrapper / `ember_call_in_context` so the default integration path is the sandboxed one. This is one API addition that closes four findings at once.
4. **S4 (coroutine checkpoint misrouting)** — restore the caller's checkpoint across each yield (or give coroutines their own `context_t`). Correctness-critical for any host using coroutines with traps.

---

*End of audit. READ-ONLY; no source files were modified. Findings verified against `src/` at HEAD `acf01d0`.*
