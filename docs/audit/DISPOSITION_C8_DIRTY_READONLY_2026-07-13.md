# Disposition Table for c8 — DIRTY-READ-ONLY Mode

**Audience:** c8 (implementation chunk). **Author:** c7 (consolidation/recheck chunk).
**Mode:** Read-only disposition. The working tree is **dirty**. Per the c1 DIRTY-READ-ONLY
contract, **no source, test, todo, documentation, staging, or commit operation was performed
by this chunk.** Every confirmed-unresolved finding below is marked
`BLOCKED — DIRTY-READ-ONLY` with the exact dirty paths that block it, severity, evidence, the
concrete intended fix, the regression test that fails before and passes after, and an
explanation of why owner work cannot be overwritten. **No finding is left without a status.**

> **This file is an untracked analysis artifact. It must NOT be committed by anyone.** It
> exists solely to hand c8 a complete, source-grounded disposition. (Same untracked status as
> the sibling `docs/audit/DOCS_AUDIT_REVALIDATION_2026-07-13.md`.)

---

## 0. Corrected recursive dirty-tree verification (independent recheck by c7)

The prior c7 attempt's dirty-tree characterization was **factually incorrect**. c7 re-ran
recursive read-only status and corrects the record here.

**Repository structure:** `ember` is a git submodule of
`hyper_workspace` (gitlink `2ac6a01d5dfcc212fa7b49d1f0bfe9016a8d2881` at the superproject's
`ember` path). The audit operates *inside* the `ember` submodule. The superproject itself
(`hyper_workspace`) is out of scope for ember edits.

**ember submodule HEAD:** `2ac6a01d5dfcc212fa7b49d1f0bfe9016a8d2881` (confirmed via
`git rev-parse HEAD` inside `ember/`).

**Corrected dirty inventory (recursive, three levels):**

| Level | Repo | HEAD | Dirty entries |
|---|---|---|---|
| 1 | `ember/` | `2ac6a01` | `M examples/ember_bundle.cpp`, `M examples/ember_cli.cpp`, `M examples/ember_stub_main.cpp`, `M src/em_loader.cpp`, `M src/module_linker.hpp`, `M src/module_registry.cpp`, `M src/module_registry.hpp`, `M src/thin_ir_ser.cpp`, `M src/thin_ir_ser.hpp`, `M thirdparty/vst3sdk` (gitlink), `?? docs/audit/DOCS_AUDIT_REVALIDATION_2026-07-13.md` |
| 2 | `ember/thirdparty/vst3sdk/` | `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0` | `M public.sdk` (gitlink) |
| 3 | `ember/thirdparty/vst3sdk/public.sdk/` | `a3911a4615dabbfdfd9d181ee26b05c70c289a95` | `M source/vst/utility/alignedalloc.h` |

**Corrections to the prior c7 attempt:**

1. **`docs/MAINTENANCE_LOG.md` is NOT dirty.** `ember/docs/MAINTENANCE_LOG.md` does not exist
   on disk (`git hash-object` → "could not open … No such file or directory"), and no such
   path appears in `git status`. The prior attempt's claim of "1,054-line uncommitted
   modification to `docs/MAINTENANCE_LOG.md`" and "same 3 dirty items" is **false**. There is
   no `MAINTENANCE_LOG.md` in the ember tree at all.

2. **The confirmed dirty paths are the eight/nine ember source+example files listed above
   (owner WIP on the X1 redesign — see §1), the `thirdparty/vst3sdk` gitlink, the nested
   `vst3sdk/public.sdk` gitlink, and the one nested working-tree file
   `thirdparty/vst3sdk/public.sdk/source/vst/utility/alignedalloc.h`.** The
   `alignedalloc.h` modification is a MinGW-alignment fallback (`__MINGW32__`/`__MINGW64__`
   branch using `_aligned_malloc` via `<malloc.h>`, plus the matching `_aligned_free` guard)
   — thirdparty vendor-local build hardening, owned by the tree owner, **not** to be touched.

3. **Working-tree churn observed:** across consecutive `git status` runs within this chunk,
   the modified-example set changed (`ember_bundle.cpp` ↔ `ember_stub_main.cpp` appeared/
   disappeared). This indicates the owner is actively editing the tree. This *strengthens*
   the BLOCKED rationale: any edit by c8 would race owner work and risk clobbering uncommitted
   changes in the exact files several findings target.

**Scope guardrails respected by this chunk:** no edits to `src/`, `tests/`, `examples/`,
`docs/` (other than this untracked disposition), `tmp_edit/` (gitignored scratch — untouched),
`thirdparty/` (untouched), stable serialization boundaries (`thin_ir.hpp` `ThinOp`,
`em_file.hpp` format — untouched), and the `G:` drive / superproject (untouched). The
`tmp_edit/` directory is confirmed gitignored (`git check-ignore tmp_edit/` → `tmp_edit/`).

---

## 1. Why the tree is dirty — owner WIP characterization (read-only `git diff`)

The nine modified ember files are **owner work-in-progress implementing the X1 redesign**
(v5 IR `CallCrossModule` dispatch-slot-count validation). They are NOT pre-existing
unrelated dirt and NOT anything c8 produced. Specifically:

- `src/module_registry.hpp` (+30) — adds `dispatch_slot_count(module_id)` and
  `set_dispatch_slot_count(module_id, count)` (the per-module actual dispatch-table slot
  count, independent of the handle allowlist). Comment block names the X1 redesign and the
  OOB-dispatch-read / wild-call hole the prior 10000 ceiling left open.
- `src/module_registry.cpp` (+21) — implements the two accessors.
- `src/module_linker.hpp` (+6) — linker-side plumbing to publish the count after
  `register_module`.
- `src/thin_ir_ser.{cpp,hpp}` (+45/+21) — `validate_thin_function` `CallCrossModule` block:
  rejects `registry_size==0`, validates `meta.slot` against the *target* module's real
  dispatch size (negative + `>= target_slot_count`), not the prior arbitrary ceiling.
- `src/em_loader.cpp` (+19/-1) — the v5 re-emit loop builds `cross_module_slot_counts` from
  the registry and passes them into `validate_thin_function`. **This diff overlaps the exact
  `CodeGenCtx ictx` / `ictx.safe_defaults()` region (lines 714–725) that NEW-1's fix must
  touch.**
- `examples/ember_bundle.cpp` / `ember_cli.cpp` / `ember_stub_main.cpp` — host-side wiring of
  `set_dispatch_slot_count` after `register_module`, plus the `--load-em`/`emit-em` policy
  plumbing.

**Consequence for c8:** the owner is mid-stream on the X1 slot-count redesign and the dirty
`em_loader.cpp` already edits the `ictx` re-emit region. Several findings below (NEW-1
budget/depth/trap-stub; NEW-2 slot-test coverage) target the *same files* the owner is
actively modifying. **Any c8 edit to those files would (a) race owner work, (b) risk
clobbering uncommitted owner changes, and (c) be impossible to commit cleanly without
either absorbing or discarding the owner's WIP.** Owner work cannot be overwritten. The
correct action is to block and hand the disposition back; c8 must not edit until the owner
has committed (or reverted) and the recursive tree is clean.

---

## 2. Disposition table — every confirmed-unresolved finding

Status legend:
- `BLOCKED — DIRTY-READ-ONLY` — confirmed-unresolved; cannot be acted on because the tree is
  dirty and/or the fix collides with owner WIP.
- `FIXED+COMMITTED (revalidated)` — confirmed in current source; no action.
- `PARTIALLY-ADDRESSED — residual BLOCKED` — the dangerous half is fixed; the residual is
  blocked.
- `FALSE-POSITIVE` — rechecked, not a defect.

### 2.1 NEW findings from c2–c6 (consolidated, deduplicated)

#### NEW-1 — v5 IR re-emit strips budget/depth x86 and traps `ud2` (C1/S5 reopened, broader)

- **Severity:** HIGH
- **Status:** `BLOCKED — DIRTY-READ-ONLY`
- **Blocking dirty paths:** `src/em_loader.cpp` (owner WIP, +19/-1, overlapping the `ictx`
  re-emit region at lines 714–725); `examples/ember_cli.cpp` (owner WIP, `--load-em` host
  region lines 1619–1647); `src/thin_ir_ser.{cpp,hpp}`; `src/module_registry.{hpp,cpp}`;
  `src/module_linker.hpp`; `examples/ember_bundle.cpp`. Nested: `thirdparty/vst3sdk` gitlink,
  `vst3sdk/public.sdk` gitlink, `vst3sdk/public.sdk/source/vst/utility/alignedalloc.h`.
- **Evidence (source-grounded, HEAD `2ac6a01`):**
  - `src/em_loader.cpp` has **0 occurrences** of `use_context_reg` / `trap_stub` /
    `budget_ptr` / `depth_ptr` (`grep -c` confirmed). The v5 re-emit `CodeGenCtx ictx`
    (line 714) sets only `dispatch_base`, `globals_base`, `natives`, `script_slots`,
    `structs`, `enable_ir_backend`, then calls `ictx.safe_defaults()` (line 725).
  - `src/codegen.hpp:117–123` — `safe_defaults()` sets *only* `emit_budget_checks=true`
    and `emit_depth_checks=true`. It does **not** set `use_context_reg`, `budget_ptr`,
    `depth_ptr`, or `trap_stub` (defaults: `use_context_reg=false`,
    `budget_ptr=nullptr`, `depth_ptr=nullptr`, `trap_stub=nullptr`).
  - `src/thin_emit.cpp:263` — `emit_budget_check`: `if (!ctx.use_context_reg &&
    !ctx.budget_ptr) return;` → **early-returns, zero budget x86 emitted**, despite the
    `BudgetCheck` ThinInstr being present in the deserialized IR.
  - `src/thin_emit.cpp:289` — `emit_depth_check`: `if (!ctx.use_context_reg &&
    !ctx.depth_ptr) return;` → **zero depth x86 emitted**.
  - `src/thin_emit.cpp:241–258` and `src/codegen.cpp:365–388` — `emit_trap`: when
    `ctx.trap_stub` is null, falls back to `ud2` (`0F 0B`).
  - Host side: `examples/ember_cli.cpp:1619–1647` — the `--load-em` path constructs
    `ember::context_t ectx` with `budget_remaining=100000000`, `max_call_depth=512`, and a
    `__builtin_setjmp` checkpoint (line 1639), then calls `ember_call_void(entry, &ectx)`.
    **But it never supplies a `trap_stub` to the loader's re-emit `ictx`**, and the loader
    never reads one. So for a v5 `.em`: the host's budget/depth are set but never checked by
    the re-emitted x86; a trap routes to `ud2` (process death), not the host's
    `__builtin_setjmp` checkpoint. `ember_cli_trap` (`:125`) and the checkpoint (`:1639`)
    are **dead for v5 modules**.
- **Impact:** A v5 IR `.em` with `while(true){}` or unbounded recursion runs unbounded; only
  the process-wide `safety::check_memory_limit` RSS failsafe and the OS guard page remain —
  the fragile backstops the spec (§3/§4) says the counters were meant to replace. A
  bounds/div/throw trap → `ud2` → process death, not recovery. `ictx.safe_defaults()` gives
  a false sense of closure: the comment "Never strip serialized budget/depth guard
  instructions" is true at the *IR-instr* level but **false at the emitted-x86 level**.
- **Concrete intended fix (two coordinated halves — NOT the one-line fix the prior attempt
  proposed; that proposal was incorrect, see §3):**
  1. **Budget/depth half:** in the v5 re-emit `ictx` setup (`em_loader.cpp` ~line 720, after
     `enable_ir_backend`), set `ictx.use_context_reg = true` so the re-emitted x86 reads
     budget/depth/trap-context from `[r14 + offset]` (r14 = `context_t*`, host-set at entry).
     This requires the loading host to pass a `context_t*` through the call (the CLI
     `--load-em` path already does: `ember_call_void(entry, &ectx)` at `:1647`).
  2. **Trap-stub half (REQUIRED for any longjmp-based regression; cannot be baked into a
     portable `.em`):** add a host-bound trap-stub plumbing path. Concretely, add fields to
     `EmLoadPolicy` (`src/em_loader.hpp:135–138`, currently only `module_permissions` and
     `allow_raw_x64`): `void* trap_stub = nullptr; void* trap_ctx = nullptr; bool
     use_context_reg = false;`. In `load_em_bytes_impl`, when re-emit is needed and the
     policy supplies a `trap_stub`/`use_context_reg`, copy them into the re-emit `ictx`
     (`ictx.trap_stub = policy->trap_stub; ictx.trap_ctx = policy->trap_ctx;
     ictx.use_context_reg = policy->use_context_reg;`). The CLI `--load-em` host then
     supplies `ember_cli_trap` as the stub (matching the JIT run path at `:538`) and sets
     `use_context_reg=true` in `em_policy`. The VST3 `guardedCall` host supplies its
     `vst3EmberTrap` stub analogously.
  - Both halves are **small, narrow changes**. The budget/depth half is one line in the
    loader; the trap-stub half is an `EmLoadPolicy` field addition + three assignment lines
    in the loader + one-line policy construction at each host. No redesign of the `.em`
    format, the serialization boundary, or the trap ABI is required — `EmLoadPolicy` is a
    load-side, non-serialized struct, so this does not touch a stable serialization
    boundary.
- **Regression test (fails before the fix, passes after — corrected; see §3):**
  Add `examples/em_v5_ir_safety_test.cpp` (CMake target `em_v5_ir_safety`, `add_test`):
  - **Budget case:** build a v5 IR `.em` in memory whose `main` is `while (true) {}` (a
    `BudgetCheck`-bearing function). Load via `load_em_bytes` with an `EmLoadPolicy` that
    sets `trap_stub = &test_trap_stub` and `use_context_reg = true`. Construct a `context_t`
    with `budget_remaining = 1000`, `max_call_depth = 512`, `has_checkpoint = true` via
    `__builtin_setjmp(ectx.checkpoint)`. Call `ember_call_void(entry, &ectx)`. **Assert:**
    the call does **not** crash (no `ud2`); control returns to the checkpoint via
    `__builtin_longjmp`; `ectx.last_error` contains `"budget exceeded"`; the loaded
    `LoadedModule::pages` is freed on the trap (no leaked exec page). **This assertion
    requires BOTH halves of the fix** — `use_context_reg=true` alone leaves `trap_stub=null`
    so exhaustion still executes `ud2` and the longjmp assertion cannot hold (see §3).
  - **Depth case:** a v5 IR `.em` `fn f(){ f(); }` invoked once. Same policy/context.
    **Assert:** traps `StackOverflow` ("stack overflow: call depth exceeded"), longjmps to
    checkpoint, no crash, pages freed.
  - **No-stub policy case (documents the secure default):** load the budget `.em` with an
    `EmLoadPolicy` whose `trap_stub = nullptr` and `use_context_reg = true`. Run under a
    `try`/`__except` (Windows SEH) or `fork`+`WIFSIGNALED` harness. **Assert:** the call
    terminates via `ud2` (`SIGILL`/`EXCEPTION_ILLEGAL_INSTRUCTION`) rather than hanging —
    this locks the "no-stub = ud2, not silent unbounded run" contract and prevents a future
    regression where budget reads silently no-op again.
  - Mirror the three cases as `em_v5_mixed` raw-x86-fallback is **not** required (raw-x64
    fns are skipped in re-emit; the IR path is the surface).
- **Why owner work cannot be overwritten:** the fix edits `src/em_loader.cpp` in the exact
  `ictx` region (lines 714–725) the owner is currently modifying for X1, plus
  `src/em_loader.hpp` `EmLoadPolicy` and `examples/ember_cli.cpp` `--load-em` policy
  construction — all three are dirty right now. Committing this fix would require either
  discarding the owner's uncommitted X1 WIP (data loss) or committing on top of a dirty tree
  (absorbing unreviewed owner changes into c8's commit). Both are forbidden. **BLOCKED until
  the owner commits/reverts and the recursive tree is clean.**

#### NEW-2 — X1 slot/no-registry validation lacks regression-test coverage

- **Severity:** MEDIUM (test-coverage gap on an already-fixed finding; the fix itself is
  correct and verified by source)
- **Status:** `BLOCKED — DIRTY-READ-ONLY`
- **Blocking dirty paths:** `examples/ember_cli.cpp` (test-host harness region);
  `src/thin_ir_ser.{cpp,hpp}` (the validator under test — owner WIP);
  `src/em_loader.cpp` (registry plumbing — owner WIP); `examples/ember_bundle.cpp`. Also
  the test file to add (`examples/thin_ir_ser_test.cpp` is **not** currently dirty, but
  adding cases to it while the validator it tests is uncommitted owner WIP would test against
  a moving target and could not be committed without colliding with the owner's
  `thin_ir_ser.{cpp,hpp}` changes).
- **Evidence (source-grounded):**
  - **X1 itself is FIXED+COMMITTED.** `src/thin_ir_ser.cpp` `validate_thin_function`
    `CallCrossModule` block now rejects `registry_size==0`, validates `meta.mod_id` range,
    and validates `meta.slot` (negative + `>= MAX_CROSS_MODULE_SLOT=10000`, and against the
    target module's real `dispatch_slot_count` per the X1 redesign). `em_loader.cpp` passes
    `registry ? registry->count() : 0u`. The `registry_size==0` rejection closes the
    no-registry fail-open *before* `alloc_executable_rw`.
  - **The gap:** `examples/thin_ir_ser_test.cpp:405–414` only tests `CallCrossModule
    mod_id=99, registry_size=1` (mod_id range). No test asserts: (a) valid `mod_id` +
    out-of-range/negative `slot`; (b) `slot >= target_dispatch_slot_count` (the real X1
    bound, not the 10000 ceiling); (c) `registry_size=0` (no-registry rejection); (d) the
    end-to-end loader path: build a v5 `.em` with `CallCrossModule`, load with
    `registry=null` → `load_em_file` returns false with `pages.empty()` (no page published).
- **Impact:** A future regression that re-opens the slot hole (e.g. someone removes the
  `slot` check, or reverts to the 10000 ceiling, or drops the `registry_size==0` rejection)
  would not be caught by any existing test. The fix is verified-by-source but not
  verified-by-test.
- **Concrete intended fix (small, narrow):** add to `examples/thin_ir_ser_test.cpp`:
  1. `CallCrossModule mod_id=0, slot=-1, registry_size=2` → reject (expect "slot out of
     range" / negative).
  2. `CallCrossModule mod_id=0, slot=999999999, registry_size=2` → reject (slot >= ceiling).
  3. `CallCrossModule mod_id=0, slot=0, registry_size=0` → reject (no-registry case).
  4. `CallCrossModule mod_id=0, slot=5, target_dispatch_slot_count=2, registry_size=2` →
     reject (slot >= *target's real* count — the X1 bound; this case specifically locks the
     X1 redesign against a revert to the 10000 ceiling).
  5. Loader end-to-end: build a v5 `.em` with `CallCrossModule`, load via `load_em_file`
     with `registry=null` → assert `load_em_file` returns false and `LoadedModule::pages`
     is empty (no exec page allocated).
- **Regression test:** the added cases ARE the regression tests (each fails before the X1
  fix, passes after). Cases (4) and (5) specifically guard the owner's in-flight X1
  redesign; they must be added *after* the owner commits X1 so they lock the committed
  behavior, not the transient WIP.
- **Why owner work cannot be overwritten:** the validator under test
  (`thin_ir_ser.{cpp,hpp}`) and the loader's registry plumbing (`em_loader.cpp`) are dirty
  owner WIP. Adding test cases now would (a) encode assertions against uncommitted behavior
  that may still shift, and (b) be un-committable without colliding with the owner's
  `thin_ir_ser` changes. **BLOCKED until X1 WIP is committed and the tree is clean.**

---

### 2.2 Revalidated historical findings — status against current source (HEAD `2ac6a01`)

#### FIXED + COMMITTED (no action; revalidated by source this run)

- **X1** (v5 IR `CallCrossModule` slot hole + no-registry fail-open) — **FIXED+COMMITTED.**
  See NEW-2 for the residual test-coverage gap (blocked).
- **N1** (ThinIR cross-module handle miscompilation, `&mod::fn` → `ConstInt(-1)`) —
  **FIXED+COMMITTED.** `src/thin_lower.cpp:1666–1675` (`FnHandleExpr` handler): cross-module
  handles force `non_serializable=true` → tree-walker fallback emits the full tagged handle
  `(1<<63)|(id<<32)|slot` + cross-aware guard. The `-1` sentinel is never baked into IR.
  `cross_module_handles_test` is in the ctest suite.
- **H1** (VST3/DSP harness default-off, no checkpoint/bypass) — **FIXED+COMMITTED.**
  `examples/vst3_wrapper/vst3_ember_processor.cpp`: `vst3EmberTrap` stub, `context.safe_defaults()`,
  `trap_stub`/`max_call_depth` set, `guardedCall` wrapper establishes `__builtin_setjmp`
  checkpoint and returns false on trap-recovery (the bypass — caller zeroes output). Three
  separate `context_t` (process/state/report).
  - **Caveat noted for a future run:** `examples/vst_dsp_harness.cpp` was not re-checked
    line-by-line this run; the VST3 processor is the canonical host and is fixed. Recommend
    a future read-only pass confirms the DSP harness matches.
- **S3** (`catch_bufs` overflow, 257th nested try) — **FIXED+COMMITTED.**
  `src/codegen.cpp:4537–4541`: `cmp rax, MAX_CATCH_DEPTH; jb ok; emit_trap(StackOverflow)`.
  `try_catch_test` G4 traps the 257th active try before OOB.
- **PERM_FFI end-to-end** — **SAFE (no bypass).** Enforced at all independent gates:
  (1) `binding_builder.hpp:40` `PERM_FFI = 1u<<0`; `BindingBuilder::add` default permission 0;
  (2) `sema.cpp:2055–2058` compile-time call-site rejection (`permission & PERM_FFI` && `!(perms & PERM_FFI)` → error);
  (3) `em_loader.cpp` v2–v4 bind + v5 re-emit native rebind both enforce PERM_FFI at load;
  (4) `EmLoadPolicy::module_permissions` threads the bit through `load_em_bytes_impl`.

#### PARTIALLY-ADDRESSED — residual OPEN, BLOCKED

- **S2** (lambda `env_ptr` escape) — **PARTIALLY-ADDRESSED; residual BLOCKED.**
  - Fixed half: `src/sema.cpp` now hard-errors the **by-ref** escape at 8 sites via
    `is_by_ref_capturing_lambda` + `reject_by_ref_lambda_escape` (return, global-store,
    retains, etc.). The dangerous by-ref path is correctly hard-rejected.
  - Residual: a **by-value** lambda that escapes (return/global/retains) without `--gc-env`
    gets `report_lambda_env_escape` — a **warning, not an error**. A host using defaults that
    ignores warnings still gets a use-after-scope on the host stack. The structural fix
    (`use_gc_env`, GC-heap env) is opt-in (`codegen.hpp:241` default false; CLI `--gc-env`
    default false).
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. The fix (default `use_gc_env=true` for sandboxed
    compiles, OR escalate the by-value warning to an error when `!use_gc_env`) touches
    `src/codegen.hpp` (`safe_defaults()`/`use_gc_env` default) and/or `src/sema.cpp` (warning
    → error) — `sema.cpp` is not currently in the dirty set, but `codegen.hpp` is a shared
    header central to the owner's X1 + NEW-1-adjacent work and any change to `safe_defaults`
    semantics would interact with the owner's in-flight `ictx.safe_defaults()` usage.
    Committing a `safe_defaults` semantics change while the owner is mid-stream on
    `em_loader.cpp`'s `safe_defaults()` call risks a behavioral collision. **BLOCKED until
    the tree is clean.**
  - **Intended fix (narrow):** in `safe_defaults()`, when the compile is sandboxed (no
    `--trusted-internal` opt-in), set `use_gc_env=true` and/or escalate the by-value escape
    warning to a hard error gated on `!use_gc_env`. Add a regression test
    (`lambda_env_escape_by_value_test`): a by-value escaping lambda compiled with default
    flags → **compile error** (fails before; passes after as a clean rejection), and the
    same source with `--gc-env` → compiles and runs without UAF (the opt-in path).

- **C2** (CLI `--emit-em` compiles with all guards off) — **PARTIALLY-ADDRESSED; residual
  BLOCKED (tied to NEW-1).**
  - Fixed half: `examples/ember_cli.cpp:536` calls `ctx.safe_defaults()` unconditionally for
    emit-em and prints a note (`:539–541`) that the loading host must provide runtime
    context/checkpoint storage. `trap_stub`/`use_context_reg`/`fn_allowlist` stay off for
    emit-em (correct — process-local, not serializable).
  - Residual: the retained `BudgetCheck`/`DepthCheck` IR instrs only become runtime-enforced
    x86 if the *loading host's* re-emit configures storage — which (per NEW-1) the loader does
    not. So C2's "metadata retained" is true but "runtime enforced" depends on NEW-1's fix.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY` (blocked on NEW-1 for the same dirty-path
    reasons; `ember_cli.cpp` and `em_loader.cpp` are both dirty).
  - **Intended fix:** once NEW-1's `EmLoadPolicy` trap-stub/use_context_reg wiring lands, the
    emit-em note should be updated to name the new policy fields the loading host must set,
    and a regression test should emit a v5 `.em` with budget/depth, load it via the new policy
    path, and assert enforcement (this is the NEW-1 `em_v5_ir_safety_test` — C2's residual is
    closed by the same test).

- **S1** (budget/depth/trap OFF by default) — **PARTIALLY-ADDRESSED; residual BLOCKED.**
  - Fixed half: `CodeGenCtx::safe_defaults()` exists and is called by all reference hosts
    (CLI `run_ember_file` + `compile_static` + VST3 processor + `em_loader.cpp` v5 re-emit).
    Raw `CodeGenCtx` defaults stay false (backward compat for trusted-internal-tool hosts,
    per spec §3).
  - Residual: a library-API host that constructs `CodeGenCtx` and calls `compile_func`
    without `safe_defaults()` still gets default-off. The unsafe pattern is no longer the
    only path, but the *default integration path* is not yet sandboxed-by-default.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. The fix (ship an `ember_compile` entry point
    that calls `safe_defaults()` + wires a default trap stub, so the default library
    integration path is sandboxed) touches `src/codegen.hpp` (new entry point or
    `safe_defaults` default-on semantics) and `src/engine.{hpp,cpp}` — `codegen.hpp` is
    shared-central and interacts with the owner's in-flight `safe_defaults()` usage in
    `em_loader.cpp`. **BLOCKED until the tree is clean.**
  - **Intended fix (narrow):** add `ember_compile_sandboxed(...)` in `engine.hpp` that
    constructs a `CodeGenCtx`, calls `safe_defaults()`, installs a default
    `abort`-with-diagnostic trap stub, and dispatches to `compile_func`. Regression test:
    a library host that calls the new entry point on `while(true){}` with a small budget →
    traps `BudgetExceeded` (not hangs); fails before (raw `compile_func` default-off → hang
    under a timeout), passes after.

#### STILL OPEN (LOW/MEDIUM, documented) — all BLOCKED on dirty tree

- **S6** (call-target guard silent no-op when unconfigured) — **OPEN MEDIUM, BLOCKED.**
  - Evidence: `src/codegen.cpp:536` and `src/thin_emit.cpp:330`:
    `if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;` — zero emitted, no
    diagnostic. The CLI JIT run path wires the allowlist (`ember_cli.cpp:542–545`), so the
    CLI is safe; the gap is a host using function refs without wiring the allowlist.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches `src/codegen.cpp` (the guard emit
    site) and/or `src/thin_emit.cpp` — `thin_emit.cpp` is not dirty, but `codegen.cpp` is
    central and the fix (emit a compile-time error when an indirect call site exists but
    `fn_slot_count==0`, or fail-closed with a trap-on-any-indirect-call stub) changes
    semantics that interact with the owner's X1 work on call-target guards. **BLOCKED until
    clean.**
  - **Intended fix (narrow):** in the indirect-call emit path, if `fn_slot_count==0` AND an
    indirect call / function-ref handle is present, emit a compile-time error ("function refs
    used but no allowlist wired — call ember_compile_sandboxed or set fn_allowlist_base") OR
    fail-closed by emitting `emit_trap(BadCallTarget)` on every indirect call. Regression
    test (`fn_allowlist_missing_test`): a module using a function ref compiled without an
    allowlist → **compile error / trap** (fails before = silent no-op = a wild call succeeds;
    passes after = hard rejection).

- **S7** (in-context thread `call_mutex` contract unenforced by raw thunks) — **OPEN LOW,
  BLOCKED.**
  - Evidence: `src/engine.cpp:264+` `ember_call_void`/`ember_call_i64` are raw thunks, no
    `call_mutex` lock. Contract is host-enforced (`in_context_threads_test` honors it).
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches `src/engine.{hpp,cpp}` — `engine.hpp`
    is dirty? No: `engine.{hpp,cpp}` are **not** in the dirty set. **However**, the task
    contract forbids any edit while the tree is dirty (the dirty tree is the global gate, not
    a per-file gate), and committing an isolated `engine.cpp` change while the owner has
    uncommitted WIP elsewhere would still require staging only that file — but `git diff
    --cached` is empty and the instruction is "never commit pre-existing dirt" + "never stage
    unrelated files"; an `engine.cpp`-only commit is theoretically stage-able in isolation,
    BUT the global dirty-tree gate from c1 ("If the tree is dirty… perform no source, test,
    todo, documentation, staging, or commit operations") takes precedence. **BLOCKED until
    the recursive tree is clean.**
  - **Intended fix (narrow):** add `ember_call_void_in_context` in `engine.hpp` that locks
    `ctx->call_mutex` when the thread addon is active before dispatching. Regression test:
    two threads calling the same context-bound function concurrently via the raw thunk →
    data race (fails before, detected by TSan/DrMemory); via the in-context wrapper →
    serialized, no race (passes after).

- **N2** (ThinIR double call-target guard for indirect calls) — **OPEN LOW, BLOCKED.**
  - Evidence: `src/thin_lower.cpp:2366` emits a `CallTargetGuard` ThinInstr, AND
    `src/thin_emit.cpp:1997–1999` re-emits the guard inside `emit_call` for `CallIndirect`.
    The `CallTargetGuard` instr handler (`thin_emit.cpp:1638`) also fires. Benign for
    correctness (idempotent), redundant work.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches `src/thin_lower.cpp` (remove the
    `CallTargetGuard` emission in `lower_call`, keep only the `emit_call` site) —
    `thin_lower.cpp` is **not** dirty, but the global dirty-tree gate applies (same rationale
    as S7). **BLOCKED until clean.**
  - **Intended fix (narrow):** delete the `CallTargetGuard` emission in `lower_call`; keep the
    `emit_call` site as the single guard. Regression test: an indirect-call module's emitted
    x86 has exactly one guard sequence per call site (disassemble or count label emissions);
    fails before (two), passes after (one); runtime behavior unchanged (`fn_handle_test`
    still passes).

- **N3** (ThinIR call-target guard lacks cross-aware bit-63 routing) — **OPEN LOW, MOOT for
  cross-module, BLOCKED.**
  - Evidence: `src/thin_emit.cpp:329–352` lacks the `bt rax,63; jc cross_skip` the tree guard
    has (`codegen.cpp:551–565`). N1's fix forces cross-module handles to `non_serializable`
    (tree fallback), so no cross-module handle reaches the ThinIR guard. The gap is
    defense-in-depth only.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches `src/thin_emit.cpp` (not dirty, but
    global gate applies). **BLOCKED until clean.**
  - **Intended fix (narrow):** add the bit-63 branch to the ThinIR guard for defense-in-depth,
    OR add a static_assert/comment documenting that cross-module handles never reach ThinIR
    (N1's fix). Regression test: a cross-module handle passed through the ThinIR path is
    routed to the cross skip (the test is defense-in-depth; with N1's fix it is not reachable
    at runtime, so the test is a unit test on the guard emission, not an end-to-end call).

- **S4** (coroutine checkpoint + per-call state misrouting across yield) — **OPEN MEDIUM,
  untested, BLOCKED.**
  - Evidence: `extensions/coroutine/ext_coroutine.cpp` `n_coro_yield` (~246–273) does
    `SwitchToFiber(caller)` without restoring `ctx->checkpoint`/`call_depth`/
    `budget_remaining`/`catch_depth`; `restore_state` only on done/trap (~222). A caller trap
    while a coroutine is suspended misroutes to the trampoline's setjmp; host loses the trap;
    UAF after `coroutine_reset`. No test covers a caller-trap-during-suspension.
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches
    `extensions/coroutine/ext_coroutine.cpp` (not dirty, but global gate applies). **BLOCKED
    until clean.**
  - **Intended fix (narrow):** restore the caller's `checkpoint`/`call_depth`/
    `budget_remaining`/`catch_depth` across each yield (swap the coroutine's saved snapshot
    in/out), OR give each coroutine its own `context_t`. Regression test
    (`coroutine_trap_during_suspend_test`): start a coroutine, yield mid-frame, then trigger
    a budget trap in the caller while the coroutine is suspended → assert the trap routes to
    the caller's checkpoint (not the trampoline's), and `coroutine_reset` does not UAF. Fails
    before (misroute/UAF), passes after.

- **S5** (trap = process death without host checkpoint+stub) — **OPEN MEDIUM, documented,
  BLOCKED; and demonstrably affects v5 `--load-em` (see NEW-1).**
  - Evidence: `src/codegen.cpp:365–376` `emit_trap` → `ud2` when `trap_stub=null`;
    `examples/ember_cli.cpp:128` `abort()` on no checkpoint. The CLI JIT run path opts in
    (`:534` trap_stub, checkpoints at `:673`/`:740`/`:352`); VST3 opts in (`guardedCall`); the
    raw library path does not. For v5 `--load-em`, the loader's re-emit `ictx` has
    `trap_stub=null` even though the host has a checkpoint → the trap is `ud2` (process death)
    and the checkpoint is theater (NEW-1).
  - **Status:** `BLOCKED — DIRTY-READ-ONLY`. Fix touches `src/codegen.cpp` (`emit_trap`
    default) and/or `src/engine.{hpp,cpp}` (safe-call wrapper) — `codegen.cpp`/`engine.*` are
    central and `codegen.hpp`'s `safe_defaults` interacts with the owner's in-flight
    `em_loader.cpp` `safe_defaults()` call. **BLOCKED until clean.**
  - **Intended fix (narrow):** ship a safe-call wrapper (`ember_call_sandboxed`) + a default
    `abort`-with-diagnostic trap stub installed by `safe_defaults()` when the host does not
    supply one, so the *default* trap is "abort with a reason message" rather than a bare
    `ud2` crash, and the *default* integration path is sandboxed. Regression test: a module
    that traps via the raw library path (no host stub) → prints a diagnostic and aborts with a
    recognizable exit code (fails before = bare `ud2`/`SIGILL` with no message; passes after
    = diagnostic + clean exit). The NEW-1 `em_v5_ir_safety_test` no-stub case locks the same
    contract on the v5 load path.

#### FALSE-POSITIVE / SAFE (revalidated, no action)

- **PERM_FFI end-to-end** — SAFE (above).
- **All "stable serialization boundary" claims** — revalidated: `src/thin_ir.hpp` `ThinOp`
  enum and `src/em_file.hpp` format comments were NOT touched by this chunk and are not
  proposed to be touched by any fix above (NEW-1's `EmLoadPolicy` is load-side,
  non-serialized; NEW-2's tests touch only test sources).

---

## 3. Correction to the prior c7 attempt's NEW-1 regression proposal

The prior c7 attempt proposed "one-line fix: `ictx.use_context_reg = true`" as closing the
budget/depth half and asserted a regression test that `longjmp`s to the checkpoint on budget
exhaustion. **That assertion is incorrect and this chunk corrects it.**

Setting **only** `ictx.use_context_reg = true` in the loader's re-emit `ictx`:
- Makes `emit_budget_check` (`thin_emit.cpp:263`) and `emit_depth_check` (`:289`) **emit**
  the budget/depth reads from `[r14 + offset]` (they no longer early-return, because
  `use_context_reg` is true).
- On exhaustion, they call `emit_trap(BudgetExceeded)` / `emit_trap(StackOverflow)`.
- **But** `emit_trap` (`thin_emit.cpp:241–258`; `codegen.cpp:365–388`) checks `ctx.trap_stub`:
  if null (which it remains, because `use_context_reg=true` does not set `trap_stub`),
  `emit_trap` falls back to `ud2` (`0F 0B`) → **process death**, not a `__builtin_longjmp` to
  the host checkpoint.

Therefore the stated regression assertion — "the call traps `BudgetExceeded` and longjmps to
the checkpoint (not `ud2`-crashes)" — **cannot be satisfied by the one-line `use_context_reg`
fix alone.** The host's `__builtin_setjmp` checkpoint (`ember_cli.cpp:1639`) and
`ember_cli_trap` (`:125`) are still dead because no stub is wired into the re-emitted x86.

**The corrected, complete fix (§2.1 NEW-1) requires BOTH halves:**
1. `ictx.use_context_reg = true` (enables the budget/depth *reads* + the *trap calls* in the
   emitted x86), AND
2. host-bound `trap_stub` + `trap_ctx` plumbed via a new `EmLoadPolicy` field into the re-emit
   `ictx` (so the trap *routes to the host's `__builtin_setjmp` checkpoint* instead of `ud2`).

Only with **both** halves does the regression's longjmp assertion hold. With only half 1,
exhaustion still `ud2`s and the test's "longjmps to the checkpoint (not `ud2`-crashes)"
assertion fails — which is exactly the test's intended **fails-before** behavior (it fails
before the fix, passes after the *complete* two-half fix). The no-stub policy case in §2.1
additionally locks the "no-stub = `ud2`, not silent unbounded run" contract so a future
regression where `use_context_reg` is set but the stub is forgotten is caught as a `ud2`
rather than a silent hang.

---

## 4. Summary — every finding has a status

| ID | Severity | Status |
|---|---|---|
| NEW-1 | HIGH | **BLOCKED — DIRTY-READ-ONLY** (fix collides with dirty `em_loader.cpp`/`ember_cli.cpp`/`em_loader.hpp`; complete fix is two coordinated halves, NOT the one-line proposal) |
| NEW-2 | MEDIUM | **BLOCKED — DIRTY-READ-ONLY** (test would lock uncommitted X1 WIP; validator under test is dirty) |
| X1 | HIGH | FIXED+COMMITTED (revalidated; NEW-2 is the residual coverage gap, blocked) |
| N1 | HIGH | FIXED+COMMITTED (revalidated) |
| H1 | HIGH | FIXED+COMMITTED (revalidated; DSP-harness line-check deferred to a future run) |
| S3 | HIGH | FIXED+COMMITTED (revalidated) |
| S2 | MEDIUM | PARTIALLY-ADDRESSED; residual **BLOCKED — DIRTY-READ-ONLY** |
| C2 | MEDIUM | PARTIALLY-ADDRESSED; residual **BLOCKED** (tied to NEW-1) |
| S1 | MEDIUM | PARTIALLY-ADDRESSED; residual **BLOCKED — DIRTY-READ-ONLY** |
| S6 | MEDIUM | OPEN, **BLOCKED — DIRTY-READ-ONLY** |
| S7 | LOW | OPEN, **BLOCKED — DIRTY-READ-ONLY** (global dirty-tree gate) |
| N2 | LOW | OPEN, **BLOCKED — DIRTY-READ-ONLY** (global gate) |
| N3 | LOW | OPEN/moot, **BLOCKED — DIRTY-READ-ONLY** (global gate) |
| S4 | MEDIUM | OPEN/untested, **BLOCKED — DIRTY-READ-ONLY** (global gate) |
| S5 | MEDIUM | OPEN, **BLOCKED — DIRTY-READ-ONLY** (affects v5 `--load-em` per NEW-1) |
| PERM_FFI | — | SAFE (no bypass; revalidated) |

**No finding is left without a status.** No source/test/todo/doc/staging/commit operation was
performed. The tree is dirty; owner WIP on the X1 redesign (and a nested `vst3sdk`/
`public.sdk`/`alignedalloc.h` MinGW hardening) occupies the exact files the HIGH/MEDIUM fixes
require. Owner work cannot be overwritten. The disposition is handed to c8 for action **after
the owner commits or reverts and the recursive tree is clean** — at which point c8 should
implement the fixes in the order: NEW-1 (two halves) → NEW-2 (locks X1) → S5/S1 (default
sandbox path) → S2 → S6 → S4 → N2/N3/S7, each with its regression test, focused ctest, `cmake
--build buildt -j 8`, full `ctest --test-dir buildt --output-on-failure --timeout 120`, and
the exit-177 validation
(`ember_cli.exe run tests/lang/optimization_validation.ember --fn main --passes constprop,forward,copyprop,instcombine,dce,licm,cse,dse`
→ exit 177) before committing that fix, with `@`-free commit subjects/bodies, no
amend/rebase/force-push, no `thirdparty/`/`tmp_edit/`/serialization-boundary/`G:` touches.
