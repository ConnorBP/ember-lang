# Sandbox Re-validation Audit — ember

**Date:** 2026-07-12
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** f256ff9 (working tree: only `thirdparty/vst3sdk` submodule dirty — untouched)
**Prior report re-checked:** `docs/audit/FINAL_SANDBOX_REDTEAM_2026-07-11.md` (HEAD acf01d0)
**Spec re-checked:** `docs/spec/SAFETY_AND_SANDBOX.md` (read in full, 426 lines)
**Scope:** READ-ONLY re-validation of the sandbox guarantees in the current tree — per-frame budget, combined call-depth, call-target provenance, trap/checkpoint unwind — in **both** tree codegen (`src/codegen.cpp`) and ThinIR lowering/emission (`src/thin_lower.cpp`, `src/thin_emit.cpp`, `src/thin_ir_ser.cpp`), including the feature-tier surface (lambdas, coroutines, try/catch, in-context threads, GC env, optimization passes) and the CLI/loader/new-host configuration consistency. Re-checks every prior finding rather than assuming the 2026-07-11 report remains accurate.
**Posture:** READ-ONLY. No tracked source files edited. No commits. No probes added to tracked source. Existing test binaries in `build/` (MinGW g++) were executed read-only. This document is the deliverable (a new untracked file; not a commit).

---

## Executive Summary

The 2026-07-11 report is **partly stale**: its highest-severity finding (S3, `catch_bufs` overflow) has been **FIXED and test-covered** since that audit (commit `61aa818`), and the structural fix for S2 (lambda `env_ptr` escape) now exists as an opt-in GC-heap env backend. The remaining prior findings (S1, S4, S5, S6, S7) **still hold** in the current tree. The prior-audit closures (FINDING A `frame_off`, `PERM_FFI` 3 gates, raw-x86 default-reject, v5 IR validator completeness, `ext_array` bounds) are **all verified still in place**, and the relevant tests pass.

This re-validation found **three new issues** the prior report did not flag, all in the ThinIR lowering/emission path (the `enable_ir_backend` / `--passes` / v5-`.em`-re-emit path), plus **two configuration caveats** about guard retention across `.em` serialization:

| # | Severity | Status | Finding | Evidence |
|---|----------|--------|---------|---------|
| **N1** | **HIGH** | NEW, untested | ThinIR lowerer silently miscompiles cross-module function handles (`&mod::fn`): emits `ConstInt(h->slot)` where `h->slot = -1` for cross-module handles (sema sets `cross_module_id`/`cross_module_slot`, not `slot`), with no `non_serializable` fallback and no cross-aware call-target guard. Tree codegen builds the handle + sets `non_serializable`; ThinIR does neither. Trigger: `enable_ir_backend=true` (e.g. `--passes`) + a module using `&mod::fn`. With the call-target guard configured → traps `BadCallTarget` on a valid handle (wrong behavior); with the guard off (S6) → `lea [dispatch + (-1)*8]` OOB read → wild call. | `src/thin_lower.cpp:1631-1637` (FnHandleExpr, ignores `is_cross_module`); `src/thin_emit.cpp:329-352` (guard, no bit-63 cross-aware branch vs tree `src/codegen.cpp:551-565`); `is_cross_module`/`module_handle_records_base` appear nowhere in `thin_lower.cpp`/`thin_emit.cpp`/`thin_ir.cpp`/`thin_ir_ser.cpp` |
| **N2** | **LOW** | NEW | ThinIR double-emits the call-target guard for indirect calls: once as a `CallTargetGuard` ThinInstr in `lower_call` (`thin_lower.cpp:2319`) and again inside `emit_call` (`thin_emit.cpp:1987-1989`) after reloading the handle. Tree codegen guards once (`codegen.cpp:3170`). Redundant (stricter, not a hole) but divergent; wastes a range+bt sequence per indirect call under `--passes`. | `src/thin_lower.cpp:2317-2320` + `src/thin_emit.cpp:1985-1989` (two `emit_call_target_guard` sites for one `CallIndirect`) vs `src/codegen.cpp:3168-3171` (one) |
| **N3** | **LOW** | NEW | ThinIR `emit_call_target_guard` lacks the cross-module bit-63 routing the tree guard has (N1's mechanism); a cross-module handle that reached the IR guard would fail the intra range check. Benign only because N1 means cross-module handles never reach this guard correctly — but it is a second, independent place the IR path diverges from the tree path's cross-module story. | `src/thin_emit.cpp:329-352` (no `bt rax,63` / `cross_skip`) vs `src/codegen.cpp:551-565` |
| **C1** | **MEDIUM** | NEW caveat | Sandbox guards are **stripped on v5 `.em` load-time re-emit**: `em_loader.cpp` builds the re-emit `CodeGenCtx` with only `dispatch_base`/`globals_base`/`natives`/`script_slots`/`structs`/`enable_ir_backend` — it does NOT set `emit_budget_checks`/`emit_depth_checks`/`trap_stub`/`fn_allowlist_base`/`fn_slot_count`/`use_context_reg` (all default-off). So a module compiled with `--passes` + guards, serialized to v5 `.em`, re-emits with **zero budget/depth/call-target guards and `ud2` traps**. No test asserts guard retention across serialization. | `src/em_loader.cpp:668-676` (ictx setup, no guard flags); defaults at `src/codegen.hpp:88,90,100,110,118,134-135` |
| **C2** | **MEDIUM** | NEW caveat | The CLI's `--emit-em` path compiles with **all sandbox guards off** (`trap_stub`/`use_context_reg`/`emit_budget_checks`/`emit_depth_checks`/`fn_allowlist_base` all gated on `opts.emit_em_path.empty()`). A `.em` emitted by the CLI bakes in no budget/depth/trap/call-target guards regardless of the source. Combined with C1, a CLI-emitted `.em` is sandbox-guard-free end to end. | `examples/ember_cli.cpp:526-534` (guards gated on `emit_em_path.empty()`) |

**Prior findings, re-checked:**

| Prior # | Prior Sev | Current status | Evidence |
|---|---|---|---|
| S1 budget/depth OFF by default | HIGH | **STILL HOLDS** | `src/codegen.hpp:100,110` (`emit_budget_checks=false`, `emit_depth_checks=false`); no `safe_defaults()` helper added. CLI opts in only for the JIT path (`ember_cli.cpp:528-530`), gated on `emit_em_path.empty()`. |
| S2 lambda `env_ptr` escape | HIGH | **PARTIALLY ADDRESSED** — structural fix exists but opt-in; sema stopgap NOT added | GC-heap env backend added (`ctx.use_gc_env`, `src/codegen.hpp:230` default false; `src/codegen.cpp:1888-1934` heap path "the lambda CAN outlive this frame"). CLI default `gc_env=false` (`ember_cli.cpp:315,551`). Sema escape guards still check `is_slice` only — `is_lambda` not added (return `sema.cpp:2822-2823`, global-store `:2393,2407`, retains `:2012,2041`). Default stack-env path still emits raw `rbp`-relative `env_ptr` (`codegen.cpp:2028-2034`). |
| S3 `catch_bufs` overflow | HIGH | **FIXED + TEST-COVERED** | Runtime guard added: `src/codegen.cpp:4521-4525` (`cmp catch_depth, MAX_CATCH_DEPTH; jae trap StackOverflow`) before indexing; throw reads `cd-1` (bounded ≤255). `context.hpp:113-116` comment updated. Test G4 at `examples/try_catch_test.cpp:343-360` (257 nested active try → traps, `reset_for_call` clears) PASSES. Landed in commit `61aa818`. |
| S4 coroutine checkpoint misrouting | MEDIUM | **STILL HOLDS, untested** | `ext_coroutine.cpp:209-227` sets `ctx->checkpoint`=trampoline setjmp; `restore_state` only on done/trap. `n_coro_yield` (`:260-274`) + `n_coroutine_next` (`:343-373`) `SwitchToFiber` without restoring checkpoint/**call_depth**/**budget_remaining**/**catch_depth** — broader than the prior report (whole per-call state clobbered across a yield, not just the checkpoint). No test exercises a caller trap during suspension. |
| S5 trap = process death by default | MEDIUM | **STILL HOLDS** (documented) | `emit_trap` falls back to `ud2` when `ctx.trap_stub=null` (`codegen.cpp:358-376`); stub `abort()`s when `has_checkpoint=false` (`ember_cli.cpp:128-129`). Defaults: `trap_stub=null`, `has_checkpoint=false`. |
| S6 call-target guard no-op when unconfigured | MEDIUM | **STILL HOLDS** in both paths | Tree `codegen.cpp:533`; ThinIR `thin_emit.cpp:330` — both `if (fn_slot_count<=0 \|\| fn_allowlist_base==0) return;`. No compile warning/error when function refs used without the allowlist. |
| S7 in-context thread `call_mutex` contract unenforced | LOW | **STILL HOLDS** (documented) | `ext_thread.cpp:284` `call_mutex.unlock()` unconditional; raw `ember_call_*` thunks (`engine.cpp:222-247`) do not lock. `in_context_threads_test` honors the contract explicitly (locks before `ember_call`), so it passes; contract is host-discipline. |

**Prior closures, re-verified (all still in place, tests pass):**
- FINDING A (v5 IR `frame_off` stack-smash): FIXED — `src/thin_ir_ser.cpp:668-672` checks `frame_off` for every instr regardless of `frame_size`; `thin_ir_ser_test` "frame_off OOB" case PASSES.
- `PERM_FFI` 3 gates: sema `sema.cpp:1985-1986`, v2-v4 bind `em_loader.cpp:435`, v5 IR rebind `em_loader.cpp:713-716`; default `module_permissions=0` (`em_loader.hpp:129`, `em_loader.cpp:567`). `em_v5_ir_test` "unknown native name -> rejected" PASSES.
- Raw-x86 default-reject: `em_loader.hpp:130` (`allow_raw_x86=false`), `em_loader.cpp:581-585` rejects v1-v4 unless `allow_raw_x86=true`. `em_redteam_audit_test` (a)(b)(c)(d) PASS.
- v5 IR validator completeness: `thin_ir_ser.cpp:547-712` (duplicate block IDs, negative lens, CallScript/CallCrossModule range, Cmp predicate, rodata OOB, frame_off). `thin_ir_ser_test` "all validation edge cases rejected" PASSES.
- `ext_array` element-bounds overflow: `ext_array.cpp:70-75` bounds `i` against `bytes.size()/elem_size`. `ext_bounds_test` PASSES.

---

## Test Results (read-only execution of pre-built `build/` binaries, MinGW g++)

**Sandbox/security/ThinIR/GC/codegen suite (27 tests, all PASS, 0 FAIL):**
`v0_4_hardening` · `try_catch` · `function_refs` · `cross_module_handles` · `em_redteam_audit` · `em_loader_hardening` · `em_signed` · `thread_safety` · `in_context_threads` · `thin_ir_ser` · `thin_ir` · `thin_ir_struct` · `em_v5_ir` · `em_v5_mixed` · `ext_bounds` · `gc_core` · `gc_integration` · `gc_full` · `ir_passes` · `regalloc` · `codegen_opt` · `ember_pass` · `fn_types` · `constexpr` · `type_stress` · `win64_abi` · `binding_abi`.

**Broader suite (23 tests, all PASS after excluding 2 arg-requiring harnesses):**
`aggregate_global` · `array_lit` · `call_raw` · `em_bytes` · `em_roundtrip` · `ext_lifecycle` · `ext_map` · `ext_registration` · `ext_runtime` · `ext_sync` · `field_of_index` · `float_global_regression` · `for_each_array` · `host_struct` · `import_roundtrip` · `pub_priv` · `static_assert` · `typed_enum` · `v0_5_live_modules` · `v0_6_hot_reload` · `v0_6_lifecycle` (plus `win64_abi`/`binding_abi` already counted).
- `bundler_test` and `sema_check` "failed" only with usage errors (`usage: bundler_test <ember_bundle_exe> <stub_exe>` / `usage: sema_check <file.ember>`) — they are ctest harnesses requiring args, not real failures.

**Language suite (`tests/run_lang_tests.sh ./build`): 463 passed, 0 failed, 0 skipped.**

**Key test confirmations of specific guarantees:**
- `v0_4_hardening_test` H-M4-2: 2000-term expression / 300 native calls / 8×8KiB aggregate copies all trap under small budgets (budget charging works when enabled). M-M4-3: B1 per-context `max_call_depth` observed (1 traps, 100 permits).
- `try_catch_test` G4: 257 nested active try blocks → 257th traps `StackOverflow` before `catch_bufs` OOB; `reset_for_call` clears abandoned `call_depth`+`catch_depth`. **S3 fix verified by test.**
- `function_refs_test` C1/C2: out-of-range forged handle (99999) and in-range-unregistered handle (slot 2, bit clear) both trap `BadCallTarget` (call-target guard works when configured).
- `cross_module_handles_test` A/B/C/D/E: all pass — but via the **tree-walker** path (the test does not set `enable_ir_backend`; see N1).
- `gc_full_test` / `gc_integration_test`: GC-heap env (`use_gc_env=true`) works — reachable lambdas survive collection. **S2 structural fix verified by test (opt-in).**
- `thread_safety_test`: B1 per-context isolation — thread B's tiny budget traps on its own context, no cross-thread longjmp corruption.
- `em_signed_test` C/D1/D2/E: Ed25519 content authentication; corrupted signature rejected; signed-only rejects unsigned; dev mode rejects v4 signed.

**No regressions found.** The full tree is green on the executed suite.

---

## Guarantee-by-guarantee re-validation

### G1. Per-frame byte budget — charging at entry + every taken loop back-edge (incl. continue)

**Tree codegen — VERIFIED.**
- Entry charge: `src/codegen.cpp:4924` region (`emit_budget_check(block_cost(f.body), ...)` after frame/param setup). `block_cost` (`:4760-4776` via `cost_add`) is reach-aware (statements + expr operands + native-call setup + per-arg marshalling + aggregate byte copies + switch chains + for init/step), floors at 1, **saturates at `INT32_MAX`** (`cost_add` `:4760-4763`: `a >= cap-b ? cap : a+b`).
- `emit_budget_check` (`:399-423`): saturates `encoded_cost` at `INT32_MAX` (`:401-402`); **compare-before-subtract** (`cmp budget,0; jle trap; cmp budget,cost; jbe trap; sub`) so an exhausted negative budget cannot wrap positive.
- Loop back-edges all charge `block_cost(loop_body)`:
  - while: `loops.push_back({latch,...})` `:4130`; `latch` bound `:4134`; `emit_budget_check` `:4135`. **continue → `latch`** (`ContinueStmt` `:4733` → `loops.back().cont`=latch), so `while (true) { continue; }` charges every iteration. ✓
  - do-while: continue → `cond` `:4145`; charge `:4150`. ✓
  - for: continue → `step_l` `:4338`; charge `:4343`. ✓
  - for-each (array): continue → `latch` `:4223`; charge `:4230`. ✓
  - for-each (slice): continue → `latch` `:4292`; charge `:4299`. ✓

**ThinIR lowering — VERIFIED.** Mirrors tree codegen.
- `cost_add` saturates at `INT32_MAX` (`src/thin_lower.cpp:609-611`). `block_cost` reach-aware (`:620-706`).
- Entry charge: `thin_lower.cpp:1108` `emit_budget_check(block_cost(f.body), f.loc)`.
- while: `loops.push_back({latch,...})` `:2681`; `latch` charges `:2689`; continue → `latch` (`:2796` `loops.back().cond_bb`=latch). ✓
- do-while: continue → `cond_bb` `:2700`; back-edge latch charges `:2722-2724`. ✓
- for: continue → `step_bb` `:2768`; `step_bb` charges `:2777`. ✓

**ThinIR emission — VERIFIED.** `emit_budget_check` (`thin_emit.cpp:259-285`) saturates + compare-before-subtract (matches tree). Entry-charge double-charge guard at `:860-873` (if block 0 already has a `BudgetCheck`, skip the fail-safe entry charge; a deserialized/hand-built IR without an explicit entry op still gets a coarse instr-count charge).

**Status: VERIFIED in both paths. Opt-in (S1): `emit_budget_checks=false` default (`codegen.hpp:100`).**

### G2. Saturating/checked cost arithmetic — VERIFIED

`cost_add` saturates at `INT32_MAX` in both tree (`codegen.cpp:4760-4763`) and ThinIR (`thin_lower.cpp:609-611`). `emit_budget_check` clamps `encoded_cost` to `INT32_MAX` (tree `:401-402`; ThinIR `:260`). Depth check compares before incrementing (tree `:462-476`; ThinIR `:287-313`). No wrap-to-negative path found.

### G3. No bypass through new constructs — VERIFIED

- `defer`: cleanup exprs run at scope exit (return/break/continue/throw) via `emit_cleanups_to`; the defer expr's cost is counted in `block_cost`/`stmt_cost` (`DeferStmt` cost in both tree and ThinIR), so a defer inside a loop is charged as part of the body cost at the back-edge. No bypass.
- `match`/`switch`: compare chains counted in `block_cost` (`sw->cases.size()` + per-case bodies). Switch/match `continue` targets the nearest real loop (skip switch frames). No bypass.
- `pin` (loop-carried reg promotion): a pin reloads once per iteration from a frame slot; does not skip the back-edge charge. No bypass.
- `regalloc` (`enable_regalloc`): assigns VRegs to callee-saved regs; value-preserving; does not remove `BudgetCheck`/`DepthCheck`/`CallTargetGuard`/`BoundsCheck` (classified `is_side_effecting`, `ext_opt.cpp:35-41`). Opt-in via `--passes` (`ember_cli.cpp:566`); default off.
- `bounds-elim` pass: removes a `BoundsCheck` only after proving the canonical `i=0; while(i<N){ a[i]; i=i+1 }` shape with fixed array + provable induction (`ext_opt.cpp:1330-1344`); "a false positive turns a recoverable bounds trap into memory unsafety" — conservative. Matches spec §5 (compile-time-constant-index optimization, not a safety weakening). Opt-in via `--passes`.
- `SimplifyCFG` v2: explicitly avoids merging loop-header blocks to prevent moving a latch's `BudgetCheck` into the body (the `:608-611` comment names the 177→116 regression it prevents). Safe.
- **DCE/CSE never remove side-effecting instrs** (`ext_opt.cpp:37-39,428`). Verified.

### G4. Combined call-depth checks — all paths, balanced dec/restore — VERIFIED (tree + ThinIR)

**Tree codegen:**
- `emit_depth_check` (`:453-481`): load depth+max; require `depth >= 0` and `depth < max-1` (compare before increment; `INT32_MAX` cannot wrap negative); increment only on valid path; trap `StackOverflow` otherwise. `emit_depth_leave` (`:489-497`) decrements after.
- Direct script: `:1551-1554` (expr) + `:3339-3342` (stmt) — check + call + leave. ✓
- Cross-module direct: `:1543-1545` + `:3290-3292` — check + `emit_cross_module_call` + leave. ✓
- Indirect (incl. lambda): `:3310-3332` — check + reload handle + (cross-aware dispatch) + leave. ✓
- Native (incl. hidden struct-return helpers, overloads, string helpers, for-each natives, coroutine yield, GC alloc): all via `emit_counted_native_call` (`:507-514`: check + call + leave) / `emit_counted_named_native` (`:515-518`). Sites: `:1540,2308,3287,3478,4200,4217,4700,4704,1908`. ✓
- **Re-entry**: a native that re-enters script via `ember_call_*` — the raw thunk does not depth-check, but the re-entered script's calls do; the native remains counted (the `inc` happened at the script→native boundary, the `dec` at return). Combined nesting composes per spec §4.
- **Thread/coroutine**: the script-side call to `thread_spawn`/`coroutine_next`/`coroutine_start` is depth-counted (native call). The trampoline's internal `ember_call` does not depth-check (raw thunk), but `save_state`/`restore_state` snapshots+restores `call_depth` (`ext_coroutine.cpp:155-167`, `ext_thread.cpp:116-132`) on done/trap, isolating the worker's depth. ✓ on done/trap. **Caveat: across a coroutine yield, `call_depth` is NOT restored** (see S4) — the caller resumes with the coroutine's leftover `call_depth`.
- **Exception (throw)**: `throw` restores `call_depth` from `catch_saved_call_depths[cd-1]` (`codegen.cpp:4638-4644`), so a cross-frame throw restores the catching frame's depth (abandoned frames' incs discarded, mirroring `reset_for_call`). ✓

**ThinIR lowering/emission — VERIFIED.**
- `emit_depth_check` (`thin_lower.cpp:732-735`) emits a `DepthCheck` ThinInstr before each call; `emit_depth_leave` is emitted directly in `thin_emit.cpp` after each call type (`emit_native_call:819`, `emit_script_call:826`, `emit_cross_module_call:835`, `emit_indirect_call:846`). The `:795-797` comment explicitly notes double-incrementing would leak (a regression they avoided).
- All four call types covered: native `:2384`, cross-module `:2395`, indirect `:2399`, script `:2404`. ✓
- `emit_depth_check`/`emit_depth_leave` in `thin_emit.cpp:287-322` match the tree compare-before-increment logic (B1 per-context + baked-ptr paths).

**Status: VERIFIED in both paths. Opt-in (S1): `emit_depth_checks=false` default (`codegen.hpp:110`).**

### G5. Call-target provenance/range/bitset guards — every indirect + cross-module path

**Tree codegen — VERIFIED when configured.**
- `emit_call_target_guard` (`codegen.cpp:533-575`): range `cmp rax, slot_count; jae trap` + bitset `bt [allowlist + handle>>3], handle&7; jnc trap`; **cross-aware** (`:551-565`): `bt rax,63; jc cross_skip` — a cross-module handle (bit 63 set) skips the intra guard and routes to `emit_cross_module_indirect_dispatch` (`:630`) which validates against the **target** module's allowlist from the records table. Both indirect paths (lambda call `:2800`, function-ref call `:2880`/`:3170`, cross-module indirect `:3323`).
- `BadCallTarget` trap on any mismatch. `function_refs_test` C1/C2 + `cross_module_handles_test` C/D verify (PASS).

**ThinIR lowering/emission — GAP (N1/N3).**
- `emit_call_target_guard` (`thin_emit.cpp:329-352`): range + bitset, but **NO cross-aware bit-63 branch** (N3). A cross-module handle fails the intra range check.
- `CallTargetGuard` ThinInstr emitted in `lower_call` for indirect calls (`thin_lower.cpp:2319`) AND re-emitted in `emit_call` (`thin_emit.cpp:1989`) — **double guard** (N2).
- **Critical (N1):** the ThinIR `FnHandleExpr` handler (`thin_lower.cpp:1631-1637`) emits `ConstInt(h->slot)` and ignores `h->is_cross_module`/`cross_module_id`/`cross_module_slot`. For a cross-module handle, `h->slot = -1` (sema sets the cross-module fields, not `slot`), so ThinIR bakes `-1`. No `non_serializable` is set, so `compile_func` emits the bad IR. Tree codegen (`codegen.cpp:2580-2588`) correctly builds `(1<<63)|(id<<32)|slot` AND sets `non_serializable_reason` (for `.em` portability) — ThinIR does neither. `module_handle_records_base`/`is_cross_module` appear nowhere in `thin_*.cpp`.
- Trigger: `enable_ir_backend=true` (`--passes` or `codegen.hpp:205` set) + a module using `&mod::fn`. `compile_func` gates ThinIR on `enable_ir_backend` (`codegen.cpp:4907`); `cross_module_handles_test` does NOT set it (uses tree-walker), so it passes and does not cover this.

**Status: tree VERIFIED; ThinIR has N1 (HIGH, cross-module handle miscompilation), N2 (LOW, double guard), N3 (LOW, no cross-aware guard). Opt-in (S6): guard no-op when `fn_slot_count==0` in both paths.**

### G6. Trap/checkpoint unwind — reset_for_call, try/catch, nested, coroutines, in-context threads

- `reset_for_call` (`context.hpp:142-152`): clears `call_depth`, `last_trap`, `last_error`, **`catch_depth`**, **`thrown_value`**; explicitly does NOT reset `budget_remaining` (host-owned). `try_catch_test` G4 verifies it clears abandoned depth+catch. ✓
- **try/catch state**: try saves regs+rsp+rip into `catch_bufs[catch_depth]` + `call_depth` into `catch_saved_call_depths[catch_depth]` after the **bounded-index guard** (`codegen.cpp:4521-4525`, the S3 fix); increments `catch_depth`; normal completion decrements; throw restores regs+rsp+rip + `call_depth` + longjmps to catch-entry rip; `catch_depth==0` throw → `UnhandledThrow` trap to host. Nested try across frames: `catch_saved_call_depths` snapshots the catching frame's depth. ✓ (S3 fixed; G4 test passes.)
- **Nested calls**: `call_depth`/`catch_depth` are `context_t` fields shared across calls; `catch_depth` persists across script-to-script calls (only `reset_for_call` clears it). The S3 guard bounds it at every try-entry, so nesting cannot overflow. ✓
- **Coroutines (S4 — STILL HOLDS)**: trampoline (`ext_coroutine.cpp:182-243`) snapshots caller state, overwrites `ctx->checkpoint` with its setjmp, runs fn, restores on done/trap. `n_coro_yield` (`:260-274`) + `n_coroutine_next` (`:343-373`) return control to the caller **without restoring** `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth`. A caller trap while a coroutine is suspended longjmps to the trampoline's setjmp (on the coroutine's stack), not the host checkpoint — host loses the trap, caller's script is time-warped; after `coroutine_reset` (`:441-456`) `DeleteFiber`s a suspended coroutine, a later caller trap longjmps to freed stack (UAF). **Broader than the prior report: not just the checkpoint, the whole per-call state is clobbered across a yield.** No test covers this.
- **In-context threads (S7 — STILL HOLDS, documented)**: worker (`ext_thread.cpp:154-185`) locks `call_mutex`, saves state, runs, restores, unlocks. `thread_join` (`:280-300`) unlocks `call_mutex` (so the worker can acquire it), waits, reacquires. The caller is **blocked** on `call_mutex`/`cv.wait` for the whole clobber window and the worker restores before the caller resumes — the key contrast with S4 (threads swap-and-block; coroutines swap-and-resume). Contract: host must lock `call_mutex` around the outer `ember_call`; raw thunks do not enforce it. `in_context_threads_test` honors it (PASS); unenforced.
- **Trap recoverability (S5 — STILL HOLDS, documented)**: `emit_trap` → `ud2` when `trap_stub=null` (`codegen.cpp:358-376`); stub `abort()`s when `has_checkpoint=false` (`ember_cli.cpp:128-129`). Recoverable only when host sets both `trap_stub` + a `setjmp` checkpoint. `__builtin_longjmp` (not `std::longjmp`) — JIT'd frames have no `.pdata`. Spec §2 is honest about this; risk is a host reading only §7's "one unwind primitive" language assuming recoverability is automatic.

**Status: try/catch + reset_for_call VERIFIED (S3 fixed). Coroutine (S4), trap-default (S5), thread-contract (S7) still opt-in/documented.**

### G7. Opt-in/default-off posture + CLI/loader/new-host consistency

**Defaults (all OFF, `src/codegen.hpp`):** `trap_stub=null` (`:88`), `trap_ctx=null` (`:91`), `emit_budget_checks=false` (`:100`), `emit_depth_checks=false` (`:110`), `use_context_reg=false` (`:118`), `fn_allowlist_base=0`/`fn_slot_count=0` (`:134-135`), `use_gc_env=false` (`:230`), `enable_ir_backend=false` (`:205`). No `safe_defaults()` helper exists (the prior report's recommendation was not implemented).

**CLI (`examples/ember_cli.cpp`) consistency:**
- JIT run path (`run_ember_file`, `:523-534`): opts IN — `trap_stub`, `use_context_reg`, `emit_budget_checks`, `emit_depth_checks`, `fn_allowlist_base`/`fn_slot_count`, all gated on `opts.emit_em_path.empty()`. `budget_remaining=100000000`, `max_call_depth=512`. `use_gc_env=opts.gc_env` (default false, `:315`). Checkpoint via `__builtin_setjmp` (`:648,705`).
- `--emit-em` path: **all guards OFF** (C2) — the `emit_em_path.empty()` gating disables budget/depth/trap/call-target for emitted `.em`.
- `compile_static` (`:1061-1067`, used by `ember pipe`): guards ON unconditionally (not gated on emit-em).
- `ember pipe` (`:1230-1251`): per-call `budget_remaining=100000000`, `max_call_depth=512`, checkpoint + `call_depth=0` reset per stage. Consistent.
- `ember live` (`:1319,1360,1402`): each tick/reload call sets budget+depth+checkpoint. Consistent.
- `--tick` path (`:732`): own `tick_ctx` (B1 isolation). Consistent.
- **Inconsistent**: `compile_static` sets guards unconditionally while `run_ember_file` gates on emit-em — a `.em` emitted via the CLI has no guards (C2), but `ember pipe` (which uses `compile_static`) does. A host copying the `--emit-em` shape gets an unguarded module.

**Loader (`src/em_loader.cpp`) — C1:** v5 IR re-emit `ictx` (`:668-676`) sets only `dispatch_base`/`globals_base`/`natives`/`script_slots`/`structs`/`enable_ir_backend`. Does NOT set `emit_budget_checks`/`emit_depth_checks`/`trap_stub`/`fn_allowlist_base`/`fn_slot_count`/`use_context_reg`. **A v5 `.em` re-emits with zero sandbox guards and `ud2` traps**, regardless of what the source compile had. No test asserts guard retention across serialization. The `BudgetCheck`/`DepthCheck`/`CallTargetGuard` ThinInstrs in the deserialized IR are dispatched in `emit_instr` but early-return on the false flags. `PERM_FFI` is the one guarantee the loader DOES enforce (3 gates, `module_permissions=0` default).

**New hosts:** a host using the library API with `CodeGenCtx` defaults (the documented `docs/guide/20-api/` integration path) gets no budget, no depth, no recoverable traps, no call-target guard, no GC env — the trusted-script path. The sandboxed path requires explicitly setting the flags + a checkpoint wrapper + (for cross-module handles) the records table + (for lambda safety) `use_gc_env`. None of this is enforced by the raw `ember_call_*` thunks (`engine.cpp:222-247`).

---

## Newly discovered issues — ranked by severity

### N1 (HIGH) — ThinIR silently miscompiles cross-module function handles
**Evidence:** `src/thin_lower.cpp:1631-1637` — `FnHandleExpr` handler emits `ConstInt(h->slot)` unconditionally; for `&mod::fn`, `h->slot=-1` (sema `src/sema.cpp:1619-1621` sets `cross_module_id`/`cross_module_slot`, not `slot`). No `is_cross_module` check, no `non_serializable`. `src/thin_emit.cpp:329-352` — guard has no bit-63 cross-aware branch. `is_cross_module`/`module_handle_records_base` appear nowhere in `thin_lower.cpp`/`thin_emit.cpp`/`thin_ir.cpp`/`thin_ir_ser.cpp`. Tree codegen handles it correctly (`codegen.cpp:2580-2588` + cross-aware guard `:551-565` + `emit_cross_module_indirect_dispatch` `:630`).
**Trigger:** `enable_ir_backend=true` (`--passes` or host-set) + a module using `&mod::fn`. `compile_func` gates ThinIR on `enable_ir_backend` (`codegen.cpp:4907`).
**Impact:** valid cross-module handle becomes `-1`. With call-target guard configured → traps `BadCallTarget` (wrong behavior / DoS on a valid call). With guard off (S6, the default) → `lea [dispatch_base + (-1)*8]` → OOB read → wild `call r11` (memory corruption / controlled-call primitive). Either way the intended cross-module call never works through ThinIR.
**Untested:** no test enables `enable_ir_backend` with `&mod::fn` (`cross_module_handles_test` uses tree-walker; `em_v5_ir_test`/`ir_passes_test`/`regalloc_test` enable IR but don't use cross-module handles).
**Fix shape (recommendation, not applied):** in `thin_lower.cpp`'s `FnHandleExpr` handler, branch on `h->is_cross_module`: either (a) set `non_serializable = true` with reason "cross-module function handle requires process-local module-records storage" (mirror tree codegen `:2586-2587`, forcing tree-walker fallback), or (b) emit the full `(1<<63)|(id<<32)|slot` handle + add the cross-aware bit-63 branch to `thin_emit.cpp`'s `emit_call_target_guard` + emit a `CallCrossModuleIndirect` path. (a) is the minimal safe fix; (b) is the complete fix.

### C1 (MEDIUM) — Sandbox guards stripped on v5 `.em` load-time re-emit
**Evidence:** `src/em_loader.cpp:668-676` — re-emit `ictx` omits all guard flags (defaults from `codegen.hpp:88,90,100,110,118,134-135` are all off/null/0). `emit_x64` dispatches `BudgetCheck`/`DepthCheck`/`CallTargetGuard` ThinInstrs but the emitters early-return on the false flags.
**Impact:** a sandbox-guarded source compile, serialized to v5 `.em`, re-emits unguarded. Combined with C2, a CLI-emitted `.em` is guard-free end to end. `PERM_FFI` is preserved (load-side gates); budget/depth/call-target/trap-recoverability are not.
**Fix shape:** thread guard flags through `EmLoadPolicy` (or a new `EmReemitCtx`) so a host loading sandboxed `.em` can opt the re-emit into budget/depth/call-target guards + a trap stub. Default could remain off (trusted-`.em`) but the secure path must be expressible. Add a test that compiles with guards, serializes, loads, and asserts the re-emitted code traps on budget/depth/bad-handle.

### C2 (MEDIUM) — CLI `--emit-em` compiles with all sandbox guards off
**Evidence:** `examples/ember_cli.cpp:526-534` — `trap_stub`/`use_context_reg`/`emit_budget_checks`/`emit_depth_checks`/`fn_allowlist_base`/`fn_slot_count` all gated on `opts.emit_em_path.empty()`.
**Impact:** a `.em` produced by `ember --emit-em` bakes in no budget/depth/trap/call-target guards. With C1, loading it never restores them. The `.em` path is the trusted-artifact path (v4 Ed25519 signing is the security boundary per spec §1), so this is arguably consistent with the threat model — but a host assuming "compile with `--passes` + guards, ship `.em`, load" retains guards would be wrong.
**Fix shape:** document explicitly that `.em` is the guard-free trusted-artifact path and that sandbox guards are a JIT-only (in-memory) property; or offer a `--emit-em-sandboxed` flag that bakes guards into the IR and a loader policy that re-enables them.

### S4 (MEDIUM, prior — still holds, broader than reported) — coroutine checkpoint + per-call state misrouting across yield
**Evidence:** `extensions/coroutine/ext_coroutine.cpp:209-227` (trampoline owns `ctx->checkpoint` for the fn's duration), `:260-274` (`n_coro_yield` `SwitchToFiber` with no state restore), `:343-373` (`n_coroutine_next` returns `yield_value` with no state restore), `:155-167` (`save_state` captures `call_depth`/`budget_remaining`/`catch_depth`/`checkpoint` but `restore_state` runs only on done/trap `:227`).
**Impact:** while a coroutine is suspended at a yield, the caller's `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth` all reflect the coroutine's state. A caller trap longjmps to the trampoline's setjmp (host loses the trap, caller time-warped; UAF after `coroutine_reset` `:441-456`). The prior report named only the checkpoint; `call_depth`/`budget`/`catch_depth` are also clobbered, so the caller's subsequent depth/budget checks run against the coroutine's leftovers.
**Untested:** no test triggers a caller trap during coroutine suspension.
**Fix shape (prior report's, still valid):** restore the caller's checkpoint (and `call_depth`/`budget_remaining`/`catch_depth`) across each yield boundary (save/restore around each `SwitchToFiber`), OR give the coroutine its own `context_t`.

### S2 (HIGH, prior — partially addressed) — lambda `env_ptr` escape
**Status:** structural fix (GC-heap env) added but **opt-in/default-off**; sema stopgap **NOT added**.
**Evidence:** GC path `src/codegen.cpp:1888-1934` (`ctx.use_gc_env && le->env_size > 0` → `__ember_gc_alloc_env`, "the lambda CAN outlive this frame"); default `use_gc_env=false` (`codegen.hpp:230`), CLI default `gc_env=false` (`ember_cli.cpp:315,551`). Default stack-env path (`codegen.cpp:2028-2034`) still emits `env_ptr = lea [rbp+env_off]` with the `:1886-1887` comment "the lambda must not outlive this frame." Sema guards still `is_slice`-only: return `sema.cpp:2822-2823`, global-store `:2393,2407`, retains `:2012,2041` — **no `is_lambda` check**. `gc_full_test`/`gc_integration_test` verify the GC path works (opt-in).
**Impact:** on the default path, returning / global-storing / retaining an `env_size>0` lambda dangles `env_ptr` into a dead frame (use-after-scope, host-stack read; write in future mutable-capture patterns).
**Fix shape (prior report's):** add `is_lambda` (with `env_size>0`) to the three sema escape guards as a stopgap; keep the GC env as the structural fix and consider defaulting it on for sandboxed compiles.

### S1 (HIGH, prior — still holds) — budget/depth/trap OFF by default
**Evidence:** `src/codegen.hpp:100,110,88,118` defaults; no `safe_defaults()`. CLI opts in for JIT only (`ember_cli.cpp:528-530`). Library-API default integration = no budget, no depth, `ud2` traps, no call-target guard, no GC env.

### S5 (MEDIUM, prior — still holds, documented) / S6 (MEDIUM, prior — still holds) / S7 (LOW, prior — still holds, documented)
See the prior-findings table. All three remain in the current tree with the same evidence and (for S5/S7) the same documented/opt-in posture. S6 holds in both tree (`codegen.cpp:533`) and ThinIR (`thin_emit.cpp:330`) — the guard is a silent no-op when `fn_slot_count==0`, with no compile diagnostic.

### N2 (LOW, new) / N3 (LOW, new)
See the executive-summary table. N2 is a redundant double-guard in ThinIR indirect calls (stricter, not a hole); N3 is the missing cross-aware branch in the ThinIR guard (benign only because N1 prevents cross-module handles from reaching it correctly). Both are divergence-between-paths issues, not independent security holes.

---

## Severity-ordered recommendations (no edits made — this audit is read-only)

1. **N1** — fix the ThinIR `FnHandleExpr` handler to either set `non_serializable` for cross-module handles (minimal: force tree-walker fallback, mirroring tree `codegen.cpp:2586`) or emit the full handle + add the cross-aware guard + cross-module indirect dispatch to ThinIR. Add a test that enables `enable_ir_backend` with `&mod::fn` (the current `cross_module_handles_test` does not cover ThinIR).
2. **S2** — add `is_lambda` (with `env_size>0`) to the three sema escape guards (stopgap); consider defaulting `use_gc_env=true` for sandboxed compiles so the structural fix is the default.
3. **C1 + C2** — decide and document whether `.em` is the guard-free trusted-artifact path (then document it loudly in `docs/guide/20-api/` + `docs/spec/SAFETY_AND_SANDBOX.md`), or thread guard flags through `EmLoadPolicy` so a host can load a sandboxed `.em`. Add a guard-retention-across-serialization test.
4. **S4** — restore caller `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth` across each yield, or give coroutines their own `context_t`. Add a caller-trap-during-suspension test.
5. **S1 + S5 + S6 + S7** — ship a `CodeGenCtx::safe_defaults()` (or `ember_compile` / `ember_call_in_context`) so the default integration path is the sandboxed one, with the raw flags as the trusted-internal opt-out. One API addition closes four findings.
6. **N2** — remove the redundant `CallTargetGuard` ThinInstr in `lower_call` (keep only the `emit_call` site), or vice versa, so ThinIR guards indirect calls once like tree codegen.

---

*End of re-validation. READ-ONLY; no tracked source files modified, no commits. Findings verified against `src/` at HEAD `f256ff9`. All executed tests (27 sandbox/security/ThinIR/GC + 23 broader + 463 lang) PASS.*
