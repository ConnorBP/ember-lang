# Codegen Spec — ARM64 (AArch64 / macOS Apple Silicon) backend

> **Status:** COMPLETE. The macOS Apple Silicon (ARM64) backend ships full
> language parity: **56/56 CTest, 471/471 lang_suite**, a self-hosted ARM64
> codegen target, and coroutines. This document is the citable backend
> reference — the structured distillation of `docs/planning/MACOS_ARM64_PROGRESS.md`
> (the implementation record) and the in-tree code comments. It is the ARM64
> companion to `CODEGEN_SPEC.md` (the x86-64 backend).
>
> **Scope:** the JIT backend that emits AArch64 machine code and executes it on
> Apple Silicon. The host-written compiler path: AST → sema → `thin_lower` →
> `ThinFunction` → **`emit_arm64`** → W^X `MAP_JIT` page → AAPCS64 host thunk →
> execute. The self-hosted path is covered in §13.
>
> **Sources of truth (verify encodings/offsets here, not this doc):**
> `src/thin_emit_arm64.cpp` (the emit pass + its header-block ABI contract),
> `src/arm64_emitter.hpp` (the assembler), `src/aapcs64_classify.hpp`
> (the classifier), `src/platform.cpp` (W^X), `src/darwin_arm64_thunks.S`
> (host thunks), `src/darwin_arm64_ctx_switch.S` (coroutines),
> `self_hosted/codegen.ember` (self-hosted ARM64 codegen). All encodings below
> were verified against `llvm-mc -show-encoding -triple=arm64-apple-darwin`
> and/or `clang -c` + `otool` (cited inline in `arm64_emitter.hpp` and
> `tests/arm64_emitter_test.cpp`).

---

## §1 Backend selection — ThinIR-only on ARM64

The ARM64 backend uses **ThinIR only**. The tree-walker codegen
(`codegen.cpp` + `X64Emitter`) is x86-64-only and is a **hard compile error**
on ARM64 — it never silently emits x86.

- **`compile_impl_` (`codegen.cpp`):** on ARM64 the IR backend is **forced on**
  (`ir_enabled_eff = true`) regardless of `--passes`. The emit dispatch is
  `#if __aarch64__ emit_arm64(thf, ctx) #else emit_x64 #endif`. The
  tree-walker fallback (`compile_tree_walker_`) throws a hard compile error on
  ARM64 (it would emit unrunnable x86).
- **`--passes` is a no-op** on ARM64: ThinIR is always on. The optimization
  passes (`extensions/opt`) are **arch-neutral** — they transform the
  `ThinFunction` IR, not the emit, so they keep working unchanged. (The x86
  tree-walker-only transforms — SmartImm peephole, linear-scan regalloc — do
  not run on the ARM64 IR path; tests assert `CompileBackend::IRBackend` +
  correct value instead of byte-difference. See `MACOS_ARM64_PROGRESS.md`
  "arch-aware IR/passes tests".)
- **Regalloc is skipped** on ARM64: `compile_func_checked` skips
  `run_regalloc` via an `&& false` guard. The ARM64 emit is **frame-only** —
  `thf.ra` is ignored (`ra.enabled` treated as false). A target-configured
  linear-scan over ARM64 callee-saved registers (`x19–x28`, avoiding `x18`) is
  an additive future item; the emit checks `ra.enabled` (always false now).

**Rationale** (from `plan_MACOS_ARM64.md` §1): x86 and ARM64 are not
isomorphic. The x86 register file (`rax..r15`, `xmm0..15`, REX encoding,
`rsp`/`rbp`, Win64 shadow space, SSE, RIP-relative) does not map cleanly onto
ARM64's orthogonal `x0..x30`/`v0..v31` + `sp`/`lr`/`pc` + AAPCS64 + NEON.
ThinIR already separates **IR** (arch-neutral spine) from **emit**
(arch-specific), so the ARM64 work was `emit_arm64` + target-aware lowering,
not a 6148-line tree-walker port.

---

## §2 AAPCS64 calling convention (the host boundary)

The host→JIT boundary and all **native calls + host thunks** obey AAPCS64.
**Script→script** calls use an Ember-private convention (we control both
sides; see §14 for the deliberate struct-return divergence).

- **Argument registers — two INDEPENDENT streams:** GP args go in `x0–x7`;
  FP args go in `v0–v7`. A float arg does **NOT** consume a GP slot (unlike
  Win64's slot-parallel model). `emit_param_spills` / `marshal_call_args_gp`
  track `gp_used` and `fp_used` counts separately.
- **Return:** scalar/ptr/handle → `x0`; float → `v0`; a slice/lambda (2 GP
  words) → `{x0, x1}`.
- **`x8` indirect-result** for a >16-byte composite return: the caller
  allocates the return slot and passes its address in `x8` as a hidden first
  arg (the classifier accounts for this).
- **HFA (Homogeneous Floating-point Aggregate):** a struct/fixed-array of 1–4
  **identical** `f32` (or `f64`) members is passed/returned in that many FP
  registers (`v0..v3` max for one arg). Win64 never does this; this is the
  headline AAPCS64 divergence handled in Phase 6c.
- **Slice / lambda = 2 consecutive GP words** (`{ptr, len}` / `{fn, env}`).
  A 2-word arg needs `gp_idx + 2 > 8` (the quality-audit off-by-one fix: the
  old `gp_idx + 1 > 8` accessed `kGpArgRegs[8]` when `gp_idx == 7`).
- **>8 GP or >8 FP args → stack:** currently **throws** (`emit_arm64:
  <op> not yet supported (stack args)`). AAPCS64 stack-arg marshaling is a
  future item; rare in ember scripts. No variadics.
- **No shadow space** (that's Win64-only). AAPCS64 puts ≤8 GP args in
  registers and the callee saves its own regs.
- **SP 16-aligned** at every public call boundary (the prologue/epilogue and
  the thunks preserve this).

The classifier that produces these slots lives in `src/aapcs64_classify.hpp`
(see §12); `emit_arm64`'s param spills + call marshaling consume its result.

---

## §3 Register reservations

Fixed reservations (the emit + the host thunks agree — see the header block of
`src/thin_emit_arm64.cpp`):

| Register | Role | Notes |
|---|---|---|
| `x19` | `context_t*` | Callee-saved. The `r14` role on Win64. **Reserved — never scratch.** The host thunk installs it; the entry preserves it across script→script calls. NOT saved in the prologue (it's reserved, not clobbered). Guards read `[x19 + off]` when `use_context_reg`. |
| `x20` | "rbx role" (callee-saved temp) | Saved in the prologue at `thf.frame.rbx_save_offset` (`= -8`): `stur x20, [x29, -8]`; restored in the epilogue. |
| `x29` | FP (frame pointer) | Set by the prologue; frame offsets are `x29`-NEGATIVE. |
| `x30` | LR (link register) | Saved/restored by the prologue/epilogue `stp`/`ldp`. |
| `x9–x12` | scratch (caller-saved) | `x9`/`x10` are the primary scratch pair (the `rax`/`rcx` roles). `x0–x8` are also caller-saved scratch outside the arg-marshaling window. |
| `v0` | FP scratch / FP return | The "xmm0" tracking role; FP VRegs materialize into `v0`. |
| `v8–v15` | callee-saved FP | **Low 64 bits only** (must be preserved as 8-byte pairs). The frame-only emit does not currently use them; a future regalloc must preserve the low 64. |

- **NEVER `x18`** — Apple's reserved platform register. The emitter does not
  enforce this (it's a dumb byte emitter); callers MUST NOT choose
  `XReg::x18`. `darwin_arm64_ctx_switch.S` deliberately does NOT save/restore
  `x18` (restoring a stale `x18` would corrupt Apple runtime state).
- `x16`/`x17` are inter-procedure scratch (IP0/IP1) — usable as scratch but
  not across calls.
- Encoding 31 is **SP** for most ops and **XZR** for others (e.g. the `Rd` of
  `subs`/`cmp`, the `Rm`/`Rn` of `mov`/`orr`). The emitter emits the raw 0–31
  bits; callers pick `XReg::sp` or `XReg::xzr` as semantics demand. This is
  the root of the SP-encoding pitfall (§9).

---

## §4 Frame model — frame-only, no regalloc

Every VReg materializes from its **frame slot** to `x9` (int) / `v0` (float);
every def computes the result in `x9` / `v0` then stores to the destination
frame slot. There is no live-register tracking beyond a best-effort `x9_vreg`
/ `v0_vreg` fallback for VRegs the lowering left in `x9` without a frame slot
(rare; a well-formed lowering frame-backs every live value).

- **Frame-pointer-NEGATIVE offsets, used verbatim with `ldur`/`stur`.** The
  IR's frame plan offsets are absolute `x29`-negative offsets (same semantics
  as `rbp`-negative on x86). `ldur64`/`stur64` (signed 9-bit, `[-256, 255]`)
  handle the common case; **out-of-range** offsets materialize the slot
  address in `x10` (`sub_reg_imm` for a negative offset, or
  `mov_reg_imm64` + `add` for a huge one) then load/store through `x10`.
- **Scalar slots are 8 bytes** (mirrors `emit_x64`: the store is an 8-byte
  store). **Narrow ints are normalized in the register** via `lsl` + `asr`
  (signed) / `lsr` (unsigned) to the target width, then stored as 8 bytes and
  loaded as 8 bytes + normalized. Narrow **param spills** store the full
  8-byte arg reg (upper bits may be garbage); the use-site normalize fixes
  them — exactly as `emit_x64`'s `spill_word` + `normalize_rax`.
- **Prologue:** `stp x29, x30, [sp, -16]!` ; `add x29, sp, #0` ;
  `sub sp, sp, frame_size` ; `stur x20, [x29, -8]`.
  (The `mov x29, sp` alias uses **ADD immediate** — see §9.)
- **Epilogue:** `ldur x20, [x29, -8]` ; `add sp, x29, #0` ;
  `ldp x29, x30, [sp], 16` ; `ret`.
- **The `__retsave$slice` / `__retsave$scalar` frame-backing convention
  (Phase 6e):** a `Return` whose value survives `defer` cleanups must persist
  the return value across the cleanup calls. The lowering allocates a frame
  slot (`__retsave$slice` for a slice, `__retsave$scalar` for a scalar) and
  sets the save `Move`'s `meta.frame_off` so the saved return value is
  frame-backed. Without this, the defer's `mark` call clobbered the return
  regs and the caller dereffed a garbage slice pointer (SIGSEGV).
- **The gap-2j computed-address frame-backing (Phase 8):**
  `LoadFrame`/`StoreFrame` from/to a **computed address** (e.g. `arr[i].b` —
  base + index·stride + field_off) must frame-back the computed address,
  because it is produced by a `FieldAddr`/`IndexAddr` VReg that can be
  clobbered by the next load. The fix: `LoadFrame.meta.frame_off` (overloaded
  as both a spill slot and a field offset) is resolved so the computed
  address is spilled to its own frame slot before the element load. This was
  the `field_of_index` + `aggregate_global` crash.
- **Opaque-handle frame-backing (Phase 6e):** a `CallNative`/`CallScript`
  result that is an **opaque host handle** (e.g. `string` — `Prim::I64` with
  `struct_name="string"`, NOT a registered struct-by-value) is frame-backed
  by the spill-slot pass. The old condition excluded any non-empty
  `struct_name`, so intermediate `string` concat results (`a+b+c`) weren't
  frame-backed → the second `+` read a stale register → wrong length. The
  condition now frame-backs opaque-handle returns (non-empty `struct_name`
  BUT not in `ctx.structs`).
- **GC frame record (Phase 6/8):** when `thf.frame.gc_ptr_frame_offs` is
  non-empty, the prologue links a `GcFrameRecord` (in the frame's reserved
  24-byte region at `gc_rec_off`) onto `context_t::gc_frame_head`; the
  epilogue unlinks it. `emit_gc_frame_record_prologue` /
  `emit_gc_frame_record_epilogue` do this via `[x19 + gc_frame_head_off]` (or
  a baked `gc_frame_head_ptr`). The collector walks the chain; the root map is
  `thf.frame.gc_ptr_frame_offs` (serialized in the v5 IR). See §8 of the open
  follow-up resolution.

---

## §5 W^X JIT memory — MAP_JIT + toggle + MANDATORY mprotect + icache invalidate

`src/platform.cpp` (the `__APPLE__` branch) implements the Apple Silicon W^X
path. The naive "MAP_JIT + `pthread_jit_write_protect_np(1)` makes it
executable" model is **WRONG** — this was the key empirical finding
(`MACOS_ARM64_PROGRESS.md` Phase 1).

**The correct sequence** (verified by a 3-mode probe: plain RWX `mmap` is
`Permission denied`; MAP_JIT + toggle-only **bus-errors** (SIGBUS 10); the
full sequence works):

```
mmap(MAP_JIT | PROT_READ | PROT_WRITE)        // alloc_rw
pthread_jit_write_protect_np(0)               // enable THREAD-LOCAL writes
<write/patch code>
pthread_jit_write_protect_np(1)               // disable thread-local writes
mprotect(ptr, size, PROT_READ | PROT_EXEC)    // MANDATORY: grant executability
sys_icache_invalidate(ptr, size)              // MANDATORY: ARM D/I-cache coherence
```

- **`MAP_JIT`** is required for JIT pages on arm64 macOS under the hardened
  runtime (`mmap(..., PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT, -1, 0)`).
- **`pthread_jit_write_protect_np(0/1)`** is a **thread-local** toggle, NOT a
  per-page `mprotect` replacement. It controls *writability* only. Because it
  is thread-local, concurrent threads must each manage their own window.
- **MANDATORY `mprotect(PROT_READ|PROT_EXEC)`** — the toggle alone leaves the
  page non-executable and executing it **bus-errors**. `mprotect` is what
  actually grants `PROT_EXEC`. (This is the empirical finding that the plan's
  §3 anticipated but understated.)
- **MANDATORY `sys_icache_invalidate(addr, len)`** (`<libkern/OSCacheControl.h>`)
  — ARM's D-cache and I-cache are not coherent; freshly written code can
  execute stale I-cache lines without it.
- **`protect_rw`** (for the `.em` loader's post-seal patch path): flips the
  page back to `PROT_READ|PROT_WRITE` (dropping `PROT_EXEC` — W^X, never RWX)
  + `pthread_jit_write_protect_np(0)`; re-seal with `protect_rx` after patching.
- **16 KiB pages** are common on Apple Silicon (`getconf PAGE_SIZE` = 16384).
  Page rounding uses `sysconf(_SC_PAGESIZE)`, not a hardcoded 4096. `platform::page_size()`
  exposes this.
- **The `free_executable` size-tracking fix (Phase 1):** `free_executable(ptr)`
  used to pass size `0` to `free_page` → `munmap(ptr, 0)` frees nothing on
  POSIX (a JIT-memory leak; Windows `VirtualFree(.., 0, MEM_RELEASE)` ignores
  size). `jit_memory` now records the rounded allocation size in a
  process-wide map keyed by pointer and passes the tracked size to
  `free_page`. **The page-size map is mutex-guarded** (the quality-audit
  data-race fix: the `unordered_map` was accessed without synchronization —
  concurrent compilation could corrupt it even on distinct keys; a
  `page_sizes_mutex()` now guards every insert/find/erase).
- **Entitlement:** the hardened runtime requires the
  `com.apple.security.cs.allow-jit` entitlement for the host process. A plain
  `clang++` dev build (adhoc/linker-signed, no `runtime` flag) does **not**
  need it; a hardened-runtime **distribution** does.

---

## §6 Host thunks + coroutines

### Host thunks — `src/darwin_arm64_thunks.S` (Apple-only)

The x86 path uses inline GNU asm in `engine.cpp` (`pushq %r14`, shadow space,
Win64). On ARM64 there is **no inline-asm thunk path** in `engine.cpp` (the
`#else` stubs), so out-of-line AAPCS64 thunks in a `.S` file provide the
`ember_call_*` symbols. They install `x19 = context_t*`, marshal the script
arg into `x0`, `blr` the JIT entry, restore the caller's `x19`, and return the
`i64` in `x0`. No shadow space. SP stays 16-aligned (`stp x29, x30, [sp, -16]!`
+ `str x19, [sp, -16]!`).

```
ember_call_void_thunk(void* entry x0, context_t* ctx x1)            -> x0
ember_call_i64_thunk(void* entry x0, context_t* ctx x1, i64 a x2)   -> x0
ember_call_i64_i64_thunk(void* entry x0, context_t* ctx x1,
                         i64 a x2, i64 b x3)                        -> x0
```

The JIT entry is a standard AAPCS64 function: first script arg in `x0` (and
`x1` for the 2-arg form), returns `i64` in `x0`. `x19` is callee-saved (the
entry preserves it across script→script calls). These deliberately do NOT
establish a `setjmp` checkpoint or reset context state — they are the raw B1
helpers (matching the x86 `ember_call_*`); the keyed safe-call wrappers +
checkpoint live in the C++ driver (`engine.cpp`). The keyed/re-entry thunks
remain stubs (Phase 8 keyed tail — see §14).

### Coroutines — `src/darwin_arm64_ctx_switch.S` (Apple-only)

Coroutines (`yield`/`resume`) work natively via a hand-written AAPCS64
**cooperative context switch** — **no `ucontext`** (deprecated/problematic on
Apple; `plan_MACOS_ARM64.md` §8/§5). `ember_ctx_switch(CoroCtx* from x0,
CoroCtx* to x1)`:

- Saves the CURRENT callee-saved GP regs (`x19–x28`) + FP (`x29`) + LR (`x30`)
  + SP into `from`, then loads `to`'s saved regs + SP + LR and `ret`s into
  `to`'s saved LR (the resume PC). Standard symmetric cooperative switch.
- **`CoroCtx` layout** (`src/runtime_extension_state.hpp`, matched exactly in
  the `.S`): `int64_t regs[12]` (`[0..9]` = `x19..x28`, `[10]` = `x29`/FP,
  `[11]` = `x30`/LR) + `int64_t sp`. SP is read with `mov x9, sp` and written
  with `add sp, x9, #0`.
- **No `x18`** — not saved/restored (Apple's platform register; the coroutine
  does not own it). The switch passes NO args and returns NO value in `x0`;
  yield/resume values flow out-of-band through `Coroutine::yield_value`.
- SP stays 16-byte aligned at every switch: the initial coroutine SP is the
  16-aligned top of its `mmap`'d private stack; every frame pushed by the
  trampoline/JIT is 16-aligned.

**Lifecycle** (mirrors the Windows fiber path): `n_coroutine_start` `mmap`s a
16-aligned **1 MiB** private data stack (NOT `MAP_JIT`), sets
`coro_ctx.regs[11]` (LR) = `coro_trampoline_darwin` + `coro_ctx.sp` = the
16-aligned stack top. `n_coroutine_next` (resume) saves the resumer into
`caller_ctx` and switches into `coro_ctx`. `n_coroutine_yield` saves the
coroutine into `coro_ctx` and restores `caller_ctx`. The trampoline
(`coro_trampoline_darwin`, `extern "C"`) calls `ember_call_i64(entry, ctx, arg)`
(the AAPCS64 thunk installs `x19 = ctx`); on return it marks `done = true` and
switches back. A trap inside the entry `longjmp`s to the
`EMBER_SETJMP(ctx->checkpoint)` in the trampoline (the checkpoint frame lives
on the coroutine's private stack) — mirrors the Windows `SavedState` recovery.

**ThinIR lowering of `yield`** (required — ARM64 is ThinIR-only): the old
`is_coroutine` `non_serializable` gate was removed; a `YieldStmt` now lowers
to a 1-arg `CallNative` to `__ember_coro_yield(i64)` (the native does the
context switch; on resume it returns and the fn continues after the stmt).
This is the ONLY yield path on ARM64. (`ember_cli run <coroutine.ember>` was
briefly blocked by a separate `ir_backend_unavailable_reason` "function is a
coroutine" gate in `codegen.cpp`; that gate is now lifted — see
`MACOS_ARM64_PROGRESS.md` Phase 8.)

---

## §7 Traps + try/catch/throw

The host `setjmp`/`longjmp` checkpoint is portable (`EMBER_SETJMP`/
`EMBER_LONGJMP` use `__builtin_setjmp`/`__builtin_longjmp` on MinGW, plain
`setjmp`/`longjmp` in the `else` branch which covers clang/macOS). The
**in-JIT** try/catch/throw uses a target-defined save area:

- **ARM64 `catch_bufs` save-area layout.** The x86 layout was
  `catch_bufs[256][8] = [rbx, rbp, r12, r13, r14, r15, rsp, rip]` — x86
  callee-saved + rip. The ARM64 layout (in `context_t::catch_bufs[catch_depth]`,
  64-byte stride, opaque) is:
  `[0]=x19 [8]=x20 [16]=x29 [24]=x30 [32]=SP [40]=catch-PC [48..63]=reserved`.
- **Inline setjmp/longjmp** — self-contained register save/restore, no host
  `setjmp` checkpoint needed for in-JIT catch (advisor-confirmed). `TryCatch` =
  save `x19`/`x20`/`x29`/`x30` + SP (via `add x9, sp, #0`) + catch-PC +
  `call_depth` snapshot + `catch_depth++`. `Throw` = store `thrown_value`,
  `catch_depth--`, restore `call_depth`, load catch-PC → `x9`, restore
  `x19`/`x20`/`x29`/`x30`, restore SP **last** (via `add sp, x11, #0`),
  `br x9`; unhandled → host trap. `CatchEntry` loads `thrown_value` → the
  catch-name slot; `CatchCleanup` decrements `catch_depth`.
- **`adrp_add_label` for catch-entry.** Catch-entry address materialization
  uses `adrp_add_label` (ALWAYS `ADRP` + `ADD :lo12:`, 2 instructions, ±4 GiB
  reach) — NOT `adr_label` (ADR, ±1 MiB). The advisor flagged that `adr`'s
  ±1 MiB limit would fail on huge try bodies; `adrp`+`add` avoids the
  mid-buffer splice problem by emitting both instructions upfront. Verified:
  resolves both >1 MiB-distant and near labels without throwing.
- **`ld_ctx32`/`ld_ctx64`/`st_ctx32`/`st_ctx64`** helpers handle
  `catch_bufs` (offset 280) + `catch_saved_depths` (offset 16664), which
  exceed the `ldur` imm9 ±256 range, via `materialize_ctx_addr` (materialize
  the offset in a scratch reg, then load/store).
- **UDF hard-fault fallback.** The trap instruction is an ARM **UDF** encoding
  (`arm64_emitter.hpp` `udf(uint16_t)`); it is the hard-fault fallback only.
  Normal recoverable traps continue to call the host trap stub (`blr trap_stub`
  after marshaling ctx/reason/detail into `x0`/`x1`/`x2`); a `udf` safety net
  follows. The recoverable model is unchanged — only the illegal-instruction
  encoding differs from x86 `ud2`.

---

## §8 Float compare pitfall — `fcmp` sets V=1 on unordered/NaN

ARM `fcmp` sets the **V** (overflow) flag = 1 on unordered/NaN operands. This
makes the obvious `<`→`lt` / `<=`→`le` mapping **WRONG**: `lt` treats
unordered as true, so `NaN < x` would return true. The advisor-confirmed
condition mapping (in `emit_arm64` float `Cmp`):

| Predicate | ARM64 cond | Note |
|---|---|---|
| `==` | `eq` | |
| `!=` | `ne` | |
| `<`  | **`mi`** (NOT `lt`) | `mi` = N=1, false on unordered |
| `<=` | **`ls`** (NOT `le`) | `ls` = C=0 ∨ Z=1, false on unordered |
| `>`  | `gt` | |
| `>=` | `ge` | |

The result is an int bool (0/1) via `cset`. The NaN test
(`emit_arm64_test`): `NaN < x` AND `NaN <= x` both FALSE; `NaN == x` FALSE;
`NaN != x` TRUE — the `mi`/`ls` mapping. `fcmp_f32`/`fcmp_f64` then `cset(x9, cond)`.

---

## §9 SP encoding pitfall — `mov`/ORR treats reg 31 as XZR

`mov_reg`/ORR treats register 31 as **XZR**, not SP — so `mov x29, sp` /
`mov sp, x29` via the `mov`/ORR alias **cannot read or write SP** and silently
produces XZR. This was a real bug caught + fixed during validation. Use
**`add`/`sub` immediate 0** instead (reg 31 = SP in ADD/SUB):

- `mov x29, sp`  →  `add x29, sp, #0`   (`add_reg_imm(x29, sp, 0)`)
- `mov sp, x29`  →  `add sp, x29, #0`   (`add_reg_imm(sp, x29, 0)`)

The prologue/epilogue and the self-hosted codegen both use the ADD-immediate
form. Likewise `sub sp, sp, frame_size` MUST use the IMMEDIATE form
(`0xD1000000`, SP-aware) — the register-form `sub` treats R31 as XZR and
silently doesn't decrement SP. The self-hosted codegen documents this in its
prologue comment (`self_hosted/codegen.ember` ~line 1206).

---

## §10 Cross-module handle dispatch

A cross-module function handle (`&lib::double`) is a **packed** value:

```
handle = (1 << 63) | (mod_id << 32) | slot
```

**Bit 63** is the cross-module flag (an intra-module handle is a bare logical
slot, never bit 63 set, so the spaces never collide). The handle is baked at
sema into a `ConstInt` (mirroring the tree-walker's `FnHandleExpr` eval in
`codegen.cpp`); the process-local records/registry bases keep the function
non-serializable to `.em` (same constraint as the intra-module allowlist —
`non_serializable_reason` is set, but `non_serializable` is NOT, so the
function lowers to IR + emits ARM64 bytes).

- **`emit_indirect_call` bit-63 test → registry-hop.** `h(args)` where `h` is
  a cross-module handle lowers to `CallIndirect` with the handle vreg as
  `src1`. `emit_indirect_call` tests bit 63 (`lsr x9, x10, 63; cbnz x9, cross`);
  if set, it extracts `slot = handle & 0xFFFFFFFF` + `mod_id = (handle >> 32)
  & 0x7FFFFFFF`, validates via the handle-records table, and dispatches through
  the target module's dispatch table:
  - range-check `mod_id < module_handle_records_count` → `BadCallTarget` trap
  - `record_ptr = handle_records_base + mod_id * 24`
  - range-check `slot < [record_ptr + 16]` (slot_count) → trap
  - **allowlist bit test** `bt [record_ptr + 8], slot` → trap if clear
  - `x11 = [record_ptr + 0]` (dispatch_base); `x11 = [x11 + slot*8]`; `blr x11`
  If bit 63 is clear, the intra-module `DispatchTableBase + handle*8` dispatch
  runs (byte-identical to the pre-change path when the records table is not
  configured — `cross_aware` gates the whole cross path).
- **`CallTargetGuard` bit-63 skip.** When `module_handle_records_base != 0`,
  the guard tests bit 63 (`lsr x10, x9, 63; cbnz x10, cross_skip`) BEFORE the
  intra-module range/bit checks; a cross-module handle (huge, bit 63 set)
  skips the intra allowlist (which would otherwise wrongly fail THIS module's
  range check) — the cross-module validation is `emit_indirect_call`'s job.
  When the records table is NOT configured, no bit-63 test is emitted.
- **The allowlist bit-test `& 1` mask (quality-audit fix).** The allowlist
  bit test did `lsr_reg x12, byte, bit; cbz x12` — testing the WHOLE shifted
  byte, not bit 0. A forged handle whose own slot bit was CLEAR but a HIGHER
  bit in the same byte was SET bypassed the allowlist (`cbz` saw nonzero →
  authorized). Fix: isolate bit 0 after the variable shift (`lsl 63; lsr 63`)
  before `cbz` — i.e. actually compute `(byte >> bit) & 1` as the comments
  always said. Applied to both `emit_call_target_guard` and the cross-module
  path in `emit_indirect_call`.
- **Slot-count range check `>=` (quality-audit fix).** `emit_call_target_guard`
  used `b_cond(hi, trap)` (strictly greater) so `handle == fn_slot_count` fell
  through to the allowlist read (1 bit beyond range). Fix: `b_cond(cs, trap)`
  (unsigned `>=`, matching the cross-module path which already used `cs`).

---

## §11 The `.em` `code=arm64-v1` ABI tag

`em_file.hpp`'s `EM_TARGET_ABI_HASH` used to hardcode `"code=x64-v1"`. Phase 2
split this into an `EMBER_EM_CODE` macro: **`code=x64-v1`** for x86,
**`code=arm64-v1`** for ARM64. `EM_TARGET_ABI_HASH` is **arch-specific**, so a
macOS `.em` carries a distinct codegen tag and a **cross-codegen load is
rejected** by the ABI hash (a Windows-host loading a macOS `.em` — or vice
versa — is correctly rejected). The `.em` identity layer was already
arch-aware (`EMBER_EM_ARCH` = `arch=arm64`, `EMBER_EM_OS` = `os=darwin`,
`EMBER_EM_CC` = `cc=aapcs64`); the `code=` component is the codegen-tag piece.

The v5 IR-on-disk loader (`em_loader.cpp`) re-emit path dispatches to
`emit_arm64` on ARM64 (was `emit_x64` only) — so an IR `.em` written on macOS
re-emits to AArch64, not x86. Raw-ARM-machine-code `.em` modules (backend-
specific reloc metadata) remain deferred; IR `.em` is the supported form.

---

## §12 AAPCS64 classifier — `src/aapcs64_classify.hpp`

A shared, **emit-independent** + tested classifier (`aapcs64_classify.hpp` +
`aapcs64_classify.cpp` + `tests/aapcs64_classify_test.cpp`). `emit_arm64`'s
call marshaling + param spills + return handling consume its result. It
mirrors the role of Win64's word-class logic but for AAPCS64.

**Staging (advisor-guided) — what the first version supports:**
- **Scalars:** `f32`/`f64` → 1 FP reg (`v0–v7`); `i8..i64`/`u8..u64`/`bool`/
  `ptr`/`handle` → 1 GP reg (`x0–x7`).
- **Slices `{ptr, len}` + lambdas `{fn, env}`:** 2 consecutive GP words.
- **Composites ≤ 16 bytes:**
  - **HFA:** a struct/fixed-array of 1–4 IDENTICAL `f32` (or `f64`) members →
    that many FP regs (`v0–v3` max for one arg).
  - **otherwise** (POD int/handle aggregates, packed) → GP regs (up to 2
    eightbyte-equivalent words; ember structs are alignment-1 packed so they
    occupy `ceil(size/8)` GP words).
- **Composites > 16 bytes:** INDIRECT (passed/returned by pointer — `x8` for
  the RETURN dest; the caller allocates + passes the pointer for an arg).
- **NOT yet (deferred, throw):** mixed-float+int HFAs that aren't pure HFA,
  >4-member HFAs, stack args (>8 GP or >8 FP args in one call), variadics.
  Rare in ember scripts; expand later if a real script needs them.

**Independent streams:** a GP arg consumes an `x` reg and an FP arg consumes a
`v` reg, in parallel. The classifier tracks both `gp_used` / `fp_used` counts
so a caller can place args correctly. `classify_aapcs64_arg(ty, structs,
gp_used, fp_used)` advances both counts; `classify_aapcs64_return(ty, structs)`
classifies the return (scalar/ptr → `x0`; float → `v0`; HFA → `v0..v(N-1)`;
≤16B composite → `x0`/`x1`; >16B → INDIRECT via `x8`). `ctest -R aapcs64_classify`
→ 10/10 PASS.

---

## §13 Self-hosted ARM64 codegen target

The self-hosted compiler (`self_hosted/codegen.ember`) originally emitted
**x86-64 bytes** and ran them via `call_raw` — unrunnable on ARM64. Phase 7
(t24–t26) added an **ARM64 target** so the self-hosted codegen emits correct
executing AArch64. All 5 self-hosted tests PASS (lex/parse/sema/codegen/
full_pipeline).

- **Target abstraction:** `cg_target` (a global, `0` = x64, `1` = arm64) +
  the `CG_TARGET_X64` / `CG_TARGET_ARM64` constants. `cg_set_target(t)` sets
  `cg_target` AND re-points the register/condition setup
  (`cg_setup_target_regs`). Every emitter (`cg_mov_reg_reg`, `cg_add_reg_reg`,
  `cg_cmp_reg_reg`, `cg_setcc_al`, `cg_load_float`, …) branches on
  `cg_target == CG_TARGET_ARM64` and calls the `cg_arm_*` family, preserving
  the x64 path verbatim under `else`.
- **`cg_detect_target()` / `native_target_arch()`:** the self-hosted pipeline
  auto-targets the host. `cg_detect_target()` calls the host-injected
  `native_target_arch()` (registered in `extensions/call_raw/ext_call_raw.cpp`
  via `#if defined(__aarch64__)` — returns `0` on x86-64, `1` on aarch64), so
  `full_pipeline.ember` emits the right arch without a manual flag.
- **The ARM64 emitter port** (`cg_arm_insn` + the `cg_arm_*` family): a port
  of `src/arm64_emitter.hpp`'s encodings into ember. Every AArch64 instruction
  is exactly 4 bytes, little-endian (`cg_arm_insn` writes 4 LE bytes). Notable
  emitters: `cg_arm_stp_x29_x30_pre` (`0xA9BF7BFD` — the AAPCS64 prologue),
  `cg_arm_ldp_x29_x30_post` (`0xA8C17BFD` — the epilogue), `cg_arm_ret`
  (`0xD65F03C0`), `cg_arm_mov_reg_imm64` (movz/movk), the ALU/shift/cmp/cset/
  load/store/FP forms, and `cg_arm_call_rel32` (BRANCH26). ARM64 branches use
  BRANCH26 fixups (patch 4 bytes, PC-rel); x64 uses REL32.
- **The prologue-check gate:** the codegen verifies the prologue was emitted
  before allowing calls/returns (guards against emitting a body without a
  frame). t26 debugged + fixed the emitter bug in the prologue/epilogue/ret
  path (t25's implementation HUNG even on `return 42` — an infinite loop in
  that path; t26 found + fixed it).
- **The prologue/epilogue** (`cg_emit_prologue` / `cg_emit_epilogue`):
  - Prologue: `stp x29, x30, [sp, -16]!` ; `add x29, sp, #0`
    (`0x91000000 | (0 << 10) | (31 << 5) | 29`) ; `sub sp, sp, frame_size`
    (IMMEDIATE form, SP-aware).
  - Epilogue: `add sp, x29, #0` (`0x91000000 | (0 << 10) | (29 << 5) | 31`) ;
    `ldp x29, x30, [sp], 16` ; `ret`.
  - The `mov x29, sp` / `mov sp, x29` aliases use **ADD immediate** (the §9
    pitfall); `sub sp` uses the IMMEDIATE form (register-form treats R31 as
    XZR). This is documented inline in `self_hosted/codegen.ember`.

The self-hosted codegen's register model mirrors the x64 path with ARM64
encodings: `RAX = x0 = 0`, `RCX = x1 = 1` (rhs-scratch, split from `ARG0` so
the ARM64 rhs-scratch `x1` is distinct from the first arg `x0`), `RBP = x29`,
`ARG0..3 = x0..x3`. `cset x0, cc` directly yields 0/1 in `x0` (replaces the
x86 setcc+movzx).

---

## §14 Known limitations

- **Keyed dispatch — stubbed.** The keyed `ember_call_*` / re-entry thunks are
  still Phase-0 stubs on ARM64 (`arm64_exec_unimplemented`). The pure-C++ keyed
  logic (resolvers, record assembly, route derivation, `keyed_call_core`) was
  factored out of the x86 `#if` so it compiles on all targets, but the Darwin
  ARM64 **keyed thunks** (the V6 keyed-dispatch tail) are not yet authored.
  Identity/legacy dispatch works; keyed dispatch does not.
- **`@obf_keyed` — disabled.** No CPUID on ARM64; `MIDR_EL1` is kernel-only on
  Apple Silicon and unreadable from user space. `@obf_keyed` is diagnosed
  unsupported on ARM64. Routing it through the existing host key-provider
  abstraction (`src/key_provider.cpp`) — never a fake hardware identity — is a
  future item.
- **Keyed coroutines — fail-closed.** `coroutine_start` in keyed mode returns
  the typed unsupported-mode status on all platforms; the Darwin keyed
  re-entry thunks are unimplemented regardless. Identity/legacy coroutine mode
  (the only mode the tests exercise) is fully supported.
- **Keyed cross-module — traps.** The keyed cross-module paths (a keyed
  caller's `h(args)` where `h` is a cross-module handle into a keyed target)
  need the Darwin ARM64 keyed thunks (`ember_resolve_keyed_dispatch`) — the
  same Phase 8 keyed tail. `emit_cross_module_call` traps with a clear "keyed
  cross-module not yet supported (Phase 8 keyed thunks)" message. The
  legacy/identity cross-module handle path is fully supported (§10).
- **`arm64e` (PAC) — not targeted.** Apple's pointer-authenticated variant adds
  signing/signing-key concerns; a later target, not this effort.
- **Stack args / variadics — throw.** >8 GP or >8 FP args in one call, and
  variadics, are not yet marshaled (AAPCS64 stack-arg marshaling is a future
  item). The classifier throws; rare in ember scripts.
- **Register allocator — not yet on ARM64.** The emit is frame-only (every
  VReg spills to its frame slot). A target-configured linear-scan over ARM64
  callee-saved registers (`x19–x28`, avoiding `x18`) is additive — the emit
  checks `ra.enabled` (always false now). The x86 regalloc assigns x86
  callee-saved IDs that `emit_arm64` does not consume.
- **Struct return uses a private indirect-x8 convention — NOT AAPCS64 for
  ≤16B / HFA native returns.** This is **deliberate** for script-to-script
  calls (we control both sides): the ARM64 backend routes struct returns
  through a hidden destination pointer (the `returns_struct_by_ptr` path +
  the `arg_frame_offs[0]` dest-slot convention) rather than the AAPCS64
  "≤16B composite returned in `x0`/`x1` (or `v0`/`v1`)" / "HFA returned in
  `v0..v(N-1)`" native-return rules. **Documented native-interop gap:** this
  means a script function that returns a small struct/HFA does NOT currently
  interop with a *native* C caller expecting the AAPCS64 return-in-regs
  convention — the return is always via the indirect dest pointer. The
  *argument* side (HFA passed in FP regs, ≤16B in GP regs, >16B indirect via
  `x8`) IS AAPCS64-compliant (§2/§12). Closing the native-return-interop gap
  (an alternate emit path for native-exported struct-returning functions) is a
  future item; for script-to-script calls the private convention is correct
  and simpler. The `self_hosted_lex` crash (t23) was a related bug — a
  struct-by-value return `CopyBytes`-to-null because the hidden dest pointer
  wasn't honored; fixed, but the underlying convention remains the private one.

---

## Cross-reference

- **x86-64 backend:** `CODEGEN_SPEC.md` (calling convention, encodings,
  regalloc, traps). The two backends share the `ThinFunction` IR
  (`thin_ir.hpp`) + the arch-neutral optimization passes
  (`docs/spec/PASS_SYSTEM_DESIGN.md`, `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md`).
- **Implementation record:** `docs/planning/MACOS_ARM64_PROGRESS.md` (per-phase
  changes, validation, the empirical findings, the quality-audit bug fixes).
- **Plan (design record):** `docs/planning/plan_MACOS_ARM64.md` (the phased
  plan + the platform-coupling map, now annotated ✅ DONE).
- **The IR + lowering:** `thin_ir.hpp`, `thin_lower.cpp`, `thin_emit.hpp`
  (`emit_arm64` declaration). `COMPILER_PIPELINE.md` for the frontend→IR flow.
- **The assembler:** `src/arm64_emitter.hpp` (`Arm64Emitter`: `XReg`/`WReg`/
  `ArmVReg`/`ArmCond`, the label/fixup system, B.cond veneers, literal pools;
  encodings verified against `llvm-mc`).
- **The classifier:** `src/aapcs64_classify.hpp` (§12).
- **W^X + OS:** `src/platform.cpp` (§5), `src/jit_memory.cpp` (the size-tracking
  fix + the mutex).
- **Thunks + coroutines:** `src/darwin_arm64_thunks.S`, `src/darwin_arm64_ctx_switch.S`,
  `src/runtime_extension_state.hpp` (`CoroCtx`), `extensions/coroutine/ext_coroutine.cpp`.
- **Self-hosted:** `self_hosted/codegen.ember` (§13), `extensions/call_raw/ext_call_raw.cpp`
  (`native_target_arch`).
- **`.em` identity:** `src/em_file.hpp` (`EMBER_EM_CODE`, `EM_TARGET_ABI_HASH`),
  `src/em_loader.cpp` (v5 IR re-emit dispatch). `docs/MODULES.md`.
