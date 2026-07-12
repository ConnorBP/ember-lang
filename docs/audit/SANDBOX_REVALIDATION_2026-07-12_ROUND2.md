# Sandbox Re-validation Audit (Round 2) — ember

**Date:** 2026-07-12
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** `f256ff9` (working tree: only `thirdparty/vst3sdk` submodule dirty — untouched)
**Prior reports re-checked:**
- `docs/audit/FINAL_SANDBOX_REDTEAM_2026-07-11.md` (HEAD `acf01d0`) — the original 7-finding sandbox red-team.
- `docs/audit/SANDBOX_REVALIDATION_2026-07-12.md` (untracked, HEAD `f256ff9`) — round 1 re-validation (found N1–N3, C1–C2).
- `docs/audit/SECURITY_AUDIT_20COMMITS_2026-07-12.md` (untracked, HEAD `f256ff9`) — the 20-commit VST3/opt-pass audit.
**Spec re-checked:** `docs/spec/SAFETY_AND_SANDBOX.md` (read in full, 426 lines).
**Scope:** READ-ONLY re-validation of every sandbox guarantee in the current tree — per-frame budget, combined call-depth, saturating/checked cost arithmetic, no-bypass-through-new-constructs, call-target provenance/range/bitset guards on **every** indirect and cross-module call path, trap/checkpoint unwind (reset_for_call, try/catch, nested, coroutines, in-context threads), opt-in/default-off posture, and CLI/loader/new-host configuration consistency. Re-checks every prior finding rather than assuming any prior report remains accurate. **Explicitly addresses the round-1 gap:** the v5 IR `CallCrossModule` slot-validation hole and the new-host (VST3/DSP-harness) sandbox posture.
**Posture:** READ-ONLY. No tracked source files edited. No commits. No probes added to tracked source. Two proof-of-concept binaries were built under the gitignored `tmp_edit/sandbox_poc/` directory (NEW, untracked, not committed) to verify the cross-module hole end-to-end; they do not touch the tracked tree. Existing test binaries in `build/` (MinGW g++) were executed read-only. This document is the deliverable (a new untracked file; not a commit).

---

## Executive Summary

This round closes the **material gap the round-1 re-validation missed**: the v5 IR `CallCrossModule` validator checks **only `meta.mod_id`** and **only when `registry_size > 0`**; it **never validates `meta.slot`**. `emit_cross_module_call` then indexes the target dispatch table with that unchecked slot and calls the loaded pointer. A crafted v5 IR module can use a valid module ID with a negative or oversized slot to cause an OOB read / wild call. With no registry, `em_loader.cpp` passes `registry_size = 0` so validation is skipped entirely, while the emitted relocation later dereferences `registry` (null) — **after** the executable page is allocated. This **contradicts** the round-1 report's claims that v5 `CallCrossModule` range validation is complete and that every cross-module call path is guarded. Two end-to-end proofs confirm the hole (one segfault, one fail-open page publish).

This round also **explicitly audits the newly added hosts** the round-1/20-commit reports did not characterize from the sandbox-guarantee angle: `examples/vst3_wrapper/vst3_ember_processor.cpp` and `examples/vst_dsp_harness.cpp` both construct default-off `CodeGenCtx`s and the VST3 processor invokes raw JIT function pointers with **no** `context_t`, **no** checkpoint, **no** trap stub — a trusted/realtime posture that the wrapper's own plan document says should use a `setjmp` checkpoint + bypass-on-trap, but the implementation does not.

| # | Severity | Status | Finding | Evidence |
|---|----------|--------|---------|---------|
| **X1** | **HIGH** | **NEW, verified by PoC (segfault + fail-open)** | v5 IR `CallCrossModule` never validates `meta.slot`; validator checks only `mod_id` and only when `registry_size > 0`. `emit_cross_module_call` indexes the target dispatch table with the unchecked slot → OOB read → wild call. With `registry == null`, validation is skipped entirely and the `ModuleRegistryBase` reloc dereferences `registry->base()` (null) **after** `alloc_executable_rw`. Contradicts round-1's "v5 CallCrossModule range validation complete" + "every cross-module call path guarded." | `src/thin_ir_ser.cpp:683-688` (mod_id-only, registry_size>0-gated); `src/thin_emit.cpp:830-835` (unchecked `in.meta.slot` at :833); `src/em_loader.cpp:745-746` (passes `registry ? registry->count() : 0u`); `src/em_loader.cpp:798` (alloc) → `:814-815` (null deref); no `!registry` guard for `CallCrossModule` (contrast `CallNative` guard at `:733-737`). PoC: `tmp_edit/sandbox_poc/cross_module_em_poc.cpp` (CASE A segfaults; CASE B publishes a page with slot=999999999). |
| **H1** | **HIGH** | **NEW (new-host posture)** | VST3 processor + DSP harness compile with **all sandbox guards default-off** and invoke raw JIT function pointers with **no** `context_t`/checkpoint/trap stub. A trap (bounds, div-by-zero) → `ud2` → `#UD` → DAW process death. The wrapper's plan (line 72/285) says traps should be caught via `setjmp` checkpoint + bypass-on-trap; the implementation does neither. | `examples/vst3_wrapper/vst3_ember_processor.cpp:251-259` (default-off `CodeGenCtx`); `:802-803,814` (raw `process_f32`/`selectedProcess` JIT ptr calls, no context/checkpoint); `examples/vst_dsp_harness.cpp:118-123` (default-off `CodeGenCtx`); `docs/planning/plan_VST3_EMBER_WRAPPER.md:72,285` (stated but unimplemented checkpoint+bypass). |
| **N1** | **HIGH** | **STILL HOLDS** (round-1, untested) | ThinIR lowerer silently miscompiles cross-module function handles (`&mod::fn`): emits `ConstInt(h->slot)` where `h->slot = -1` for cross-module handles, with no `non_serializable` fallback and no cross-aware call-target guard. | `src/thin_lower.cpp:1631-1637`; `src/thin_emit.cpp:329-352`; tree handles it at `src/codegen.cpp:2580-2588` + `:551-565`. |
| **S2** | **HIGH** | **PARTIALLY ADDRESSED** (prior — structural fix opt-in; sema stopgap NOT added) | Lambda `env_ptr` escape: default stack-env path emits a raw `rbp`-relative `env_ptr`; sema escape guards check `is_slice` only, not `is_lambda`. GC-heap env backend exists but is opt-in/default-off. | GC path `src/codegen.cpp:1885-1934` (`use_gc_env`, default false `codegen.hpp:230`); sema guards `src/sema.cpp:2393,2407,2822,2012,2041` (is_slice only); CLI default `gc_env=false` (`ember_cli.cpp`). |
| **S1** | **HIGH** | **STILL HOLDS** (prior) | Per-frame byte budget + stack-depth guard OFF by default; no `safe_defaults()` helper. | `src/codegen.hpp:100,110,88,118`; CLI opts in for JIT only (`ember_cli.cpp:526-534`). |
| **S3** | **HIGH** | **FIXED + TEST-COVERED** (prior) | `catch_bufs` overflow — runtime guard added. | `src/codegen.cpp:4521-4525` (`cmp catch_depth, MAX_CATCH_DEPTH; jae trap`); `context.hpp:113-116`; `try_catch_test` G4 PASSES. |
| **C1** | **MEDIUM** | **STILL HOLDS** (round-1) | Sandbox guards stripped on v5 `.em` load-time re-emit: the re-emit `CodeGenCtx` omits all guard flags. | `src/em_loader.cpp:668-676`. |
| **C2** | **MEDIUM** | **STILL HOLDS** (round-1) | CLI `--emit-em` compiles with all sandbox guards off (gated on `emit_em_path.empty()`). | `examples/ember_cli.cpp:526-534`. |
| **S4** | **MEDIUM** | **STILL HOLDS, untested** (prior — broader than reported) | Coroutine checkpoint + per-call state misrouting across yield: `n_coro_yield` `SwitchToFiber` without restoring `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth`. | `extensions/coroutine/ext_coroutine.cpp:209-227,260-274`; `restore_state` only on done/trap `:227`. |
| **S5** | **MEDIUM** | **STILL HOLDS, documented** (prior) | Trap = process death without explicit host checkpoint + trap stub. | `src/codegen.cpp:358-376` (`ud2` fallback); `examples/ember_cli.cpp:128-129` (`abort()` on no checkpoint). |
| **S6** | **MEDIUM** | **STILL HOLDS** (prior, both paths) | Call-target guard is a silent no-op when no allowlist is configured. | Tree `src/codegen.cpp:533`; ThinIR `src/thin_emit.cpp:330`. |
| **S7** | **LOW** | **STILL HOLDS, documented** (prior) | In-context thread `call_mutex` contract unenforced by raw thunks. | `extensions/thread/ext_thread.cpp:284`; `src/engine.cpp:222-247`. |
| **N2** | **LOW** | **STILL HOLDS** (round-1) | ThinIR double-emits the call-target guard for indirect calls. | `src/thin_lower.cpp:2319` + `src/thin_emit.cpp:1989`. |
| **N3** | **LOW** | **STILL HOLDS** (round-1) | ThinIR `emit_call_target_guard` lacks the cross-module bit-63 routing the tree guard has. | `src/thin_emit.cpp:329-352` vs `src/codegen.cpp:551-565`. |

**Prior closures re-verified (all still in place, tests pass):** FINDING A (v5 IR `frame_off`, `thin_ir_ser.cpp:668-672`), `PERM_FFI` 3 gates (`sema.cpp`, `em_loader.cpp:435,713-716`), raw-x86 default-reject (`em_loader.cpp:581-585`), v5 IR validator completeness (other checks at `thin_ir_ser.cpp:547-712`), `ext_array` bounds (`ext_array.cpp:70-75`). See §"Prior closures re-verified."

---

## Finding X1 (HIGH) — v5 IR `CallCrossModule` slot validation hole + no-registry fail-open

### The hole

The v5 IR validator's `CallCrossModule` check (`src/thin_ir_ser.cpp:683-688`):
```cpp
// P3 fix: CallCrossModule mod_id < registry_size.
if (in.op == ThinOp::CallCrossModule && registry_size > 0 &&
    uint32_t(in.meta.mod_id) >= registry_size) {
    if (err) *err = "thin_ir_ser: validate: CallCrossModule mod_id out of range";
    return false;
}
```
does two things wrong:
1. **It validates only `meta.mod_id`, never `meta.slot`.** Contrast the `CallScript` check immediately above (`src/thin_ir_ser.cpp:677-682`), which validates `meta.slot` against `dispatch_size`. The `CallCrossModule` check has no slot analog. `meta.slot` is an `int32_t` read directly from the `.em` blob (`thin_ir_ser.cpp:477`) — attacker-controlled in a hand-crafted v5 `.em`.
2. **The entire check is gated on `registry_size > 0`.** When `registry_size == 0` (the no-registry case), the check is skipped completely — a `CallCrossModule` with a **negative** `mod_id` AND a **negative** `slot` passes validation.

### The unchecked index → wild call

`emit_cross_module_call` (`src/thin_emit.cpp:830-835`) then uses the unchecked `meta.slot` as an index:
```cpp
void emit_cross_module_call(const ThinInstr& in) {
    e.mov_reg_imm64_external(Reg::r11, AbsFixup::ModuleRegistryBase);
    e.load_reg_mem(Reg::r11, Reg::r11, int32_t(in.meta.mod_id) * 8);  // registry[mod_id]
    e.load_reg_mem(Reg::r11, Reg::r11, int32_t(in.meta.slot) * 8);    // registry[mod_id].dispatch[slot]  <-- UNCHECKED
    e.call_reg(Reg::r11);                                              // call the loaded pointer
    emit_depth_leave();
}
```
`int32_t(in.meta.slot) * 8` with a negative or oversized `slot` reads outside the target module's dispatch table → OOB read → the loaded value is used as a call target → **wild call**. The cross-module call sequence (`module_registry.hpp:9-17`) is `mov r11,[registry_base+mod_id*8]; mov r11,[r11+slot*8]; call r11`; only `registry_base` is a reloc, `mod_id*8` and `slot*8` are compile-time displacements baked from the (unvalidated) IR.

### The no-registry fail-open (crash after executable allocation)

When a v5 `.em` is loaded with `registry == null`, `em_loader.cpp:745-746` passes `registry ? registry->count() : 0u` → `registry_size = 0` → the `CallCrossModule` check is skipped entirely. **There is no `!registry` guard for `CallCrossModule`** in the v5 re-emit path — contrast the `CallNative` guard at `em_loader.cpp:733-737` which explicitly scans for `CallNative` instrs and rejects when `!natives`. No equivalent scan exists for `CallCrossModule` when `!registry`.

So the chain is:
1. Deserialize → `CallCrossModule` with `mod_id=-1`, `slot=-1` (or any values).
2. `validate_thin_function(thf, &verr, dispatch_size, 0)` → `registry_size=0` → **check skipped**.
3. No `!registry` guard for `CallCrossModule` (unlike `CallNative`).
4. `emit_x64` re-emits the code, producing a `ModuleRegistryBase` abs_fixup (`thin_emit.cpp:831`).
5. `alloc_executable_rw(combined)` at `em_loader.cpp:798` — **executable page allocated**.
6. Reloc loop hits `ModuleRegistryBase` → `address = reinterpret_cast<uintptr_t>(registry->base())` at `em_loader.cpp:814-815` → **null pointer dereference** (crash), after the page was allocated.

The v1–v4 reloc parse loop **does** guard this: `em_loader.cpp:406-409` rejects `ModuleRegistryBase` relocs when `!registry`. But v5 IR relocs are generated fresh by `emit_x64` (`em_loader.cpp:760-766`) and do **not** pass through the v1–v4 parse loop, so the `:406` guard is never reached for v5 IR. This is the asymmetry that lets v5 through.

### Proof (end-to-end PoC, NEW untracked file)

`tmp_edit/sandbox_poc/cross_module_em_poc.cpp` builds a v5 `.em` whose entry function contains a `CallCrossModule` instr and loads it via `load_em_file`. Two cases:

- **CASE A (no registry, `mod_id=0`, `slot=0`):** `load_em_file` with `registry=null` → **SEGFAULT (exit 139)** at the `registry->base()` dereference (`em_loader.cpp:815`), after the executable page was allocated (`:798`). The crash is the proof: the page was allocated + the reloc ran; validation did not fail closed before allocation.
- **CASE B (registry present, `mod_id=0` valid, `slot=999999999` oversized):** `load_em_file` with a real registry (`count=1`) → **returns true, publishes 1 executable page.** The validator checked `mod_id < 1` (passed) and never checked `slot`. The published page's emitted code will do `load r11,[r11+999999999*8]` → OOB read → wild call at runtime. **Fail-open confirmed.**

A second PoC, `tmp_edit/sandbox_poc/cross_module_slot_poc.cpp`, calls `validate_thin_function` directly and confirms:
- `CallCrossModule mod_id=0, slot=-1, registry_size=2` → **accepted** (slot unchecked).
- `CallCrossModule mod_id=0, slot=999999999, registry_size=2` → **accepted** (slot unchecked).
- `CallCrossModule mod_id=-1, slot=-1, registry_size=0` → **accepted** (check skipped).
- Control: `CallScript slot=99, dispatch_size=1` → **rejected** (slot IS validated for CallScript — asymmetry proven).
- Control: `CallCrossModule mod_id=99, registry_size=2` → **rejected** (mod_id IS validated when registry>0).

### Why the existing tests do not catch this

`thin_ir_ser_test.cpp:405-416` tests `CallCrossModule mod_id=99, registry_size=1` (rejected) but:
- does **not** test a valid `mod_id` with an out-of-range/negative `slot` (the slot gap);
- does **not** test `registry_size=0` (the no-registry skip).

`em_v5_ir_test.cpp` does not use `CallCrossModule` at all. `cross_module_handles_test.cpp` uses the **tree-walker** path (does not set `enable_ir_backend`), so it does not exercise the v5 IR `CallCrossModule` validator. No test asserts guard retention across v5 serialization for cross-module calls. The round-1 report's claim "v5 IR validator completeness … `CallCrossModule` range … verified in place" was true for `mod_id` only; the `slot` and no-registry cases were never covered.

### Severity rationale

HIGH. The attacker controls `meta.slot` and `meta.mod_id` via a hand-crafted v5 `.em`. In dev mode (unsigned v5 accepted by default — `em_loader.cpp:581-585`), a malicious `.em` from the same compiler/ABI is the stated threat model for the v5 path. With a registry present (the normal cross-module case), an oversized/negative slot yields an OOB read whose value is called — a controlled-call primitive if the OOB read lands on attacker-influenced memory, or a crash (DoS) otherwise. With no registry, the loader crashes (DoS) but only after allocating an executable page (the fail-closed-before-allocation guarantee is broken). The spec §1 frames `.em` as a "DISTINCT, STRONGER attack surface"; the v5 IR path is the one that re-emits from validated IR rather than mapping raw x86, and this hole is exactly the kind of validator-incompleteness the v5 design exists to prevent.

### Recommended fix (NOT applied — read-only audit)

**Fail closed before executable allocation.** In `validate_thin_function` (`src/thin_ir_ser.cpp`):
1. **Reject any `CallCrossModule` instr when `registry_size == 0`.** Mirror the `CallNative`-when-`!natives` guard (`em_loader.cpp:733-737`) at the validator level: a `CallCrossModule` with no registry is malformed (the emitted reloc would dereference null). This closes the no-registry fail-open (CASE A) before `alloc_executable_rw`.
2. **Validate `meta.slot` when `registry_size > 0`.** The validator cannot know each target module's per-module `slot_count` (the registry exposes `count()`, not per-module dispatch sizes), so the conservative correct check is: reject any `CallCrossModule` whose `meta.slot < 0` (a negative slot can never be a valid dispatch index), and — for a complete fix — thread the per-module slot counts (or the `ModuleHandleRecord::slot_count` from `handle_records_`) into the validator so `slot` can be range-checked against the **target** module's `slot_count`. At minimum, a negative-slot rejection closes the wild-call primitive for the common case; the full per-module range check is the complete fix.

**Additionally, in `em_loader.cpp` (defense-in-depth, before the reloc deref):** add a `CallCrossModule`-when-`!registry` scan in the v5 re-emit path mirroring the `CallNative` scan at `:728-737`, so a `CallCrossModule` in a no-registry load is rejected with a clear error before `emit_x64` (and before `alloc_executable_rw`). This makes the fail-closed guarantee structural, not dependent on the validator alone.

### Recommended tests (NOT added — read-only audit; the PoCs under `tmp_edit/sandbox_poc/` demonstrate the shape)

1. **`thin_ir_ser_test`**: add `CallCrossModule mod_id=0, slot=-1, registry_size=2` → expect **reject** (currently accepted). Add `CallCrossModule mod_id=0, slot=999999999, registry_size=2` → expect **reject**. Add `CallCrossModule mod_id=-1, slot=-1, registry_size=0` → expect **reject** (currently accepted).
2. **`em_v5_ir_test` (or a new `em_v5_cross_module_test`)**: build a v5 `.em` with a `CallCrossModule`, load with `registry=null` → expect `load_em_file` returns false with no pages (currently segfaults). Load with a real registry + oversized slot → expect `load_em_file` returns false (currently returns true + publishes a page). Assert `lm.pages.empty()` in both.

---

## Finding H1 (HIGH) — New hosts (VST3 processor + DSP harness) compile default-off + invoke raw JIT pointers with no sandbox context

### The new hosts

Two hosts added in the VST3 window (commits `2eae6d4`–`3176a6b`) construct `CodeGenCtx` with **all sandbox guards at their default-off values**:

**`examples/vst3_wrapper/vst3_ember_processor.cpp:251-259`:**
```cpp
ember::CodeGenCtx context;
context.globals_base = globals.base;
context.globals_index = &globals.index;
context.globals_offsets = &globals.offsets;
context.globals_types = &globals.types;
context.dispatch_base = reinterpret_cast<int64_t>(dispatch->base());
context.natives = &natives;
context.script_slots = &slots;
context.structs = &layouts;
```
**`examples/vst_dsp_harness.cpp:118-123`:**
```cpp
ember::CodeGenCtx context;
context.globals_base = module->globals.base;
context.dispatch_base = reinterpret_cast<int64_t>(module->dispatch->base());
context.natives = &module->natives;
context.script_slots = &module->slots;
context.structs = &module->layouts;
```
Neither sets `emit_budget_checks`, `emit_depth_checks`, `trap_stub`, `use_context_reg`, `fn_allowlist_base`/`fn_slot_count`, or `use_gc_env` — all default-off (`src/codegen.hpp:88,100,110,118,134-135,230`). Both call `sema(..., ember::PERM_FFI, ...)` (`vst3_ember_processor.cpp:222`, `vst_dsp_harness.cpp:100`), granting the script full FFI.

### The raw JIT pointer invocation (no context, no checkpoint)

The VST3 processor extracts the JIT'd `process_f32`/`process_f64` entry as a raw `ProcessFn = void(*)(int64_t,int64_t)` (`vst3_ember_processor.cpp:271`) and invokes it directly:
- `vst3_ember_processor.cpp:802-803`: `old->process_f32(reinterpret_cast<int64_t>(&oldContext), static_cast<int64_t>(fadeFrames));` (the crossfade path).
- `vst3_ember_processor.cpp:814`: `selectedProcess(reinterpret_cast<int64_t>(&context), data.numSamples);` (the main DSP path).

There is **no** `context_t`, **no** `ember_call_void`/`ember_call_i64` thunk, **no** `__builtin_setjmp` checkpoint, **no** `trap_stub`, **no** `reset_for_call`, and **no** `try/catch` around these calls (confirmed: `grep -rn "context_t\|ember_call\|checkpoint\|setjmp\|trap_stub\|has_checkpoint\|budget_remaining\|max_call_depth\|reset_for_call" examples/vst3_wrapper/ examples/vst_dsp_harness.cpp` returns **zero** matches). A trap in the JIT'd `process_f32` (bounds check, div-by-zero, unhandled throw) → `emit_trap` falls back to `ud2` (`src/codegen.cpp:358-376`, because `trap_stub=null`) → `#UD` → **DAW process death**.

### Is the trusted/realtime posture intentional? Partially — but it contradicts the wrapper's own plan

The **trusted-script** framing is consistent with the spec §1 threat model for `PERM_FFI`-granted scripts (within FFI, the script is trusted by definition), and the **realtime** framing (no budget/depth checks on the audio thread, no lock/allocation in the trap path) is a legitimate realtime-contract choice — a `setjmp`/`longjmp` unwind + `reset_for_call` on a realtime thread is itself questionable under the no-allocation/no-lock realtime contract. The `@realtime` sema checker (`085eaaf`) enforces no-alloc/no-IO/no-lock/no-GC/no-thread/no-exception in `@realtime` functions, which is the realtime-safety posture.

**However**, the wrapper's own plan document says the trap posture should be recoverable, not process-death:
- `docs/planning/plan_VST3_EMBER_WRAPPER.md:72`: *"Ember traps — caught via the existing `__builtin_setjmp` checkpoint mechanism. A trap in the audio thread triggers bypass (output = input or silence)."*
- `docs/planning/plan_VST3_EMBER_WRAPPER.md:285`: *"Trap handling: ember traps cannot unwind unsafely across VST SDK frames (use setjmp checkpoint)."*

The implementation does **neither**: there is no checkpoint, no trap stub, and no bypass-on-trap. The plan's stated design (checkpoint + bypass) was not implemented. So the current posture is **not the designed posture** — it is a stricter-but-crashier default (any trap kills the DAW) that the plan explicitly said to avoid. This is a design-vs-implementation divergence, not merely an intentional opt-out.

### How it differs from the guarded CLI path

The CLI JIT run path (`examples/ember_cli.cpp:526-534`) opts **in**: `trap_stub = &ember_cli_trap`, `use_context_reg = true`, `emit_budget_checks = true`, `emit_depth_checks = true`, `fn_allowlist_base`/`fn_slot_count` set, `budget_remaining = 100000000`, `max_call_depth = 512`, and wraps the call in `__builtin_setjmp(ectx.checkpoint)` (`ember_cli.cpp:648,705`). A trap → stub → `__builtin_longjmp` → host recovery branch. The VST3/DSP-harness path has **none** of this: no budget, no depth, no call-target guard, no recoverable trap, no GC env. The two paths are the opposite ends of the opt-in spectrum the spec describes — the CLI is the sandboxed reference host; the VST3/DSP hosts are the trusted-internal-tool opt-out — except the VST3 host is a **plugin that loads third-party DSP scripts into a DAW**, which is closer to the untrusted-script end of the spec's threat model than the trusted-internal-tool end, especially when combined with the `PERM_FFI` grant and the raw `load_f32`/`store_f32` arbitrary-memory natives (F2 in `SECURITY_AUDIT_20COMMITS_2026-07-12.md`).

### Severity rationale

HIGH. A VST3 plugin hosting a third-party `.ember` script with `PERM_FFI` + no sandbox guards + no recoverable trap means: (a) a buggy script (OOB index, div-by-zero) crashes the DAW, not just the plugin; (b) a runaway script hangs the audio thread (no budget); (c) unbounded recursion overflows the audio thread's stack (no depth guard). The plan said this would be bypass-on-trap; it is process-death. The realtime-constraint argument against `longjmp` is real but does not justify `ud2` (crash) over a bypass path that the plan already specified — a bypass can be implemented as a pre-checked flag + output zeroing without a `longjmp` across SDK frames.

### Recommended fix (NOT applied — read-only audit)

1. **Implement the plan's checkpoint + bypass-on-trap.** Either: (a) compile the plugin's `process_f32`/`process_f64` with `trap_stub` pointing at a bypass stub that zeroes output + sets a `trapped` flag (no `longjmp` across SDK frames — the stub returns normally and the wrapper skips the crossfade/blends zero), and wrap the `process` call in a `setjmp` checkpoint that the stub `longjmp`s to; or (b) if `longjmp` is ruled out for realtime, at minimum compile with `trap_stub` = a stub that sets `trapped=true` + `emit_budget_checks=true` + `emit_depth_checks=true` so the JIT'd code traps to the stub (which returns) instead of `ud2`-crashing, and the wrapper observes `trapped` and bypasses. Either way, the `ud2`-crash default must not be the shipped posture for a DAW plugin.
2. **Document the posture explicitly** in `docs/guide/30-examples/` (VST3 guide) and `docs/spec/SAFETY_AND_SANDBOX.md`: state that the VST3/DSP harness is the trusted-`PERM_FFI`-realtime path with no budget/depth/recoverable-trap, that a trap kills the process, and that a host loading untrusted `.ember` plugins must compile with guards + a bypass stub.
3. Consider whether `PERM_FFI` should be opt-in for VST3 plugins loading third-party scripts (currently granted unconditionally at `vst3_ember_processor.cpp:222`).

---

## Guarantee-by-guarantee re-validation

### G1. Per-frame byte budget — charging at entry + every taken loop back-edge (incl. continue)

**Tree codegen — VERIFIED.**
- Entry charge: `src/codegen.cpp:4924` region (`emit_budget_check(block_cost(f.body), ...)` after frame/param setup). `block_cost` is reach-aware, floors at 1, **saturates at `INT32_MAX`** (`cost_add` at `:4760-4763`: `a >= cap-b ? cap : a+b`).
- `emit_budget_check` (`:399-423`): saturates `encoded_cost` at `INT32_MAX` (`:401-402`); **compare-before-subtract** (`cmp budget,0; jle trap; cmp budget,cost; jbe trap; sub`) so an exhausted negative budget cannot wrap positive.
- Loop back-edges all charge `block_cost(loop_body)`:
  - while: `loops.push_back({latch,...})` `:4130`; `latch` bound `:4134`; charge `:4135`. **continue → `latch`** (`ContinueStmt` `:4727-4737` → `loops.back().cont`=latch), so `while (true) { continue; }` charges every iteration. ✓
  - do-while: continue → `cond` `:4145`; charge `:4150`. ✓
  - for: continue → `step_l` `:4342` (`loops.push_back({step_l,...})`); charge at `step_l` `:4345`. ✓
  - for-each (array): continue → `latch` `:4223`; charge `:4230`. ✓
  - for-each (slice): continue → `latch` `:4262`; charge (back-edge). ✓

**ThinIR lowering — VERIFIED.** Mirrors tree. `cost_add` saturates at `INT32_MAX` (`thin_lower.cpp:609-611`). Entry charge `:1108`. while: `loops.push_back({latch,...})` `:2681`; latch charges `:2689`; continue → `latch` (`:2820-2825` `loops[i].cond_bb`=latch). ✓ do-while: continue → `cond_bb` `:2700`; back-edge charges `:2720`. ✓ for: continue → `step_bb` `:2757`; `step_bb` charges `:2766`. ✓

**ThinIR emission — VERIFIED.** `emit_budget_check` (`thin_emit.cpp:259-285`) saturates + compare-before-subtract. Entry-charge double-charge guard at `:860-873`.

**Status: VERIFIED in both paths. Opt-in (S1): `emit_budget_checks=false` default (`codegen.hpp:100`).**

### G2. Saturating/checked cost arithmetic — VERIFIED

`cost_add` saturates at `INT32_MAX` in both tree (`codegen.cpp:4760-4763`) and ThinIR (`thin_lower.cpp:609-611`). `emit_budget_check` clamps `encoded_cost` to `INT32_MAX` (tree `:401-402`; ThinIR `:260`). Depth check compares before incrementing (tree `:462-476`; ThinIR `:287-313`). No wrap-to-negative path found.

### G3. No bypass through new constructs — VERIFIED

- `defer`: cleanup exprs run at scope exit (return/break/continue/throw) via `emit_cleanups_to`; defer cost counted in `block_cost`/`stmt_cost`, so a defer inside a loop is charged as part of the body cost at the back-edge. No bypass.
- `match`/`switch`: compare chains counted in `block_cost` (`sw->cases.size()` + per-case bodies). Switch/match `continue` targets the nearest real loop (skips switch frames, `codegen.cpp:4727-4737`). No bypass.
- `pin` (loop-carried reg promotion): reloads once per iteration from a frame slot; does not skip the back-edge charge. No bypass.
- `regalloc` (`enable_regalloc`): assigns VRegs to callee-saved regs; value-preserving; does not remove `BudgetCheck`/`DepthCheck`/`CallTargetGuard`/`BoundsCheck` (classified `is_side_effecting`, `ext_opt.cpp:23-41`). Opt-in via `--passes`; default off.
- `bounds-elim` pass: removes a `BoundsCheck` only after proving the canonical `i=0; while(i<N){ a[i]; i=i+1 }` shape with fixed array + provable induction (`ext_opt.cpp:1330-1344`); conservative. Opt-in via `--passes`.
- `SimplifyCFG` v2: avoids merging loop-header blocks (`:608-611` comment). Safe.
- **DCE/CSE never remove side-effecting instrs** (`ext_opt.cpp:37-39,428`): `CallNative/CallScript/CallIndirect/CallCrossModule/StoreGlobal/StoreAddr/CopyBytes/StructLitInit/ArrayLitInit/StringDecrypt/BoundsCheck/DivOverflowCheck/DepthCheck/BudgetCheck/CallTargetGuard` are all `is_side_effecting=true` (`:23-41`). Verified.

**Caveat (pre-existing, from `OPTIMIZATION_PASSES_READ_ONLY_AUDIT_2026-07-12.md`):** DCE/ConstProp/CSE treat a producer with non-zero `meta.frame_off` as a removable pure instr (C3), and `StoreToLoadForwardPass` forwards through non-SSA redefinitions (C7), and `fold_int_binop` computes signed overflow UB (C9). These are opt-pass correctness defects exploitable via crafted v5 `.em` in dev mode; they are **not modified by the current window** and are documented in the OPT audit. They are out of scope for the sandbox-guarantee re-validation but noted as context: a host enabling `--passes` on untrusted `.em` inherits C3/C7/C9.

### G4. Combined call-depth checks — all paths, balanced dec/restore — VERIFIED (tree + ThinIR)

**Tree codegen:** `emit_depth_check` (`:453-481`): load depth+max; require `depth >= 0` and `depth < max-1` (compare before increment); increment only on valid path; trap `StackOverflow`. `emit_depth_leave` (`:489-497`) decrements after.
- Direct script: `:1551-1554` (expr) + `:3339-3342` (stmt). ✓
- Cross-module direct: `:1543-1545` + `:3290-3292`. ✓
- Indirect (incl. lambda): `:3310-3332`. ✓
- Native (incl. hidden struct-return helpers, overloads, string helpers, for-each natives, coroutine yield, GC alloc): all via `emit_counted_native_call` (`:507-514`) / `emit_counted_named_native` (`:515-518`). Sites: `:1540,2308,3287,3478,4200,4217,4700,4704,1908`. ✓
- **Re-entry**: native re-enters script via `ember_call_*` (raw thunk, no depth-check); the re-entered script's calls do depth-check; the native remains counted. Combined nesting composes per spec §4.
- **Thread/coroutine**: script-side `thread_spawn`/`coroutine_next`/`coroutine_start` is depth-counted (native call). Trampoline's internal `ember_call` does not depth-check; `save_state`/`restore_state` snapshots+restores `call_depth` on done/trap (`ext_coroutine.cpp:157-167`, `ext_thread.cpp:116-132`). ✓ on done/trap. **Caveat: across a coroutine yield, `call_depth` is NOT restored** (S4).
- **Exception (throw)**: `throw` restores `call_depth` from `catch_saved_call_depths[cd-1]` (`codegen.cpp:4638-4644`). ✓

**ThinIR lowering/emission — VERIFIED.** `emit_depth_check` (`thin_lower.cpp:732-735`) emits `DepthCheck` before each call; `emit_depth_leave` emitted in `thin_emit.cpp` after each call type (`emit_native_call:819`, `emit_script_call:826`, `emit_cross_module_call:835`, `emit_indirect_call:846`). All four call types covered: native `:2384`, cross-module `:2395`, indirect `:2399`, script `:2404`. ✓

**Status: VERIFIED in both paths. Opt-in (S1): `emit_depth_checks=false` default (`codegen.hpp:110`).**

### G5. Call-target provenance/range/bitset guards — every indirect + cross-module path

**Tree codegen — VERIFIED when configured.**
- `emit_call_target_guard` (`codegen.cpp:533-575`): range `cmp rax, slot_count; jae trap` + bitset `bt [allowlist + handle>>3], handle&7; jnc trap`; **cross-aware** (`:551-565`): `bt rax,63; jc cross_skip` — a cross-module handle skips the intra guard and routes to `emit_cross_module_indirect_dispatch` (`:630`) which validates against the **target** module's allowlist from the records table. Both indirect paths (lambda call `:2800`, function-ref call `:2880`/`:3170`, cross-module indirect `:3323`). `BadCallTarget` trap on mismatch. `function_refs_test` C1/C2 + `cross_module_handles_test` C/D verify (PASS, tree-walker path).
- **Direct cross-module call** (`emit_cross_module_call`, `:597-611`): uses `c.cross_module_id`/`c.cross_module_slot` — sema-set compile-time constants from the linked export table (trusted, not script-controlled). No runtime guard needed for the direct path; the indirect cross-module path has the guard above.

**ThinIR lowering/emission — GAP (N1/N3, still holds).**
- `emit_call_target_guard` (`thin_emit.cpp:329-352`): range + bitset, but **NO cross-aware bit-63 branch** (N3).
- `CallTargetGuard` ThinInstr emitted in `lower_call` for indirect calls (`thin_lower.cpp:2319`) AND re-emitted in `emit_call` (`thin_emit.cpp:1989`) — **double guard** (N2).
- **N1:** ThinIR `FnHandleExpr` handler (`thin_lower.cpp:1631-1637`) emits `ConstInt(h->slot)` and ignores `h->is_cross_module`/`cross_module_id`/`cross_module_slot`. For a cross-module handle, `h->slot = -1` (sema sets the cross-module fields, not `slot`), so ThinIR bakes `-1`. No `non_serializable`. Tree codegen (`codegen.cpp:2580-2588`) correctly builds `(1<<63)|(id<<32)|slot` + sets `non_serializable`.

**v5 IR `CallCrossModule` — HOLE (X1).** The v5 IR `CallCrossModule` instr's `meta.mod_id`/`meta.slot` come from the deserialized blob (attacker-controlled). The validator checks only `mod_id` and only when `registry_size > 0`; `slot` is never validated. `emit_cross_module_call` (`thin_emit.cpp:830-835`) indexes with the unchecked slot. See X1 for the full chain + PoC.

**Status: tree VERIFIED when configured; ThinIR has N1 (HIGH, cross-module handle miscompilation), N2 (LOW, double guard), N3 (LOW, no cross-aware guard); v5 IR `CallCrossModule` has X1 (HIGH, slot validation hole + no-registry fail-open). Opt-in (S6): guard no-op when `fn_slot_count==0` in both paths.**

### G6. Trap/checkpoint unwind — reset_for_call, try/catch, nested, coroutines, in-context threads

- `reset_for_call` (`context.hpp:135-152`): clears `call_depth`, `last_trap`, `last_error`, **`catch_depth`**, **`thrown_value`**; does NOT reset `budget_remaining` (host-owned). `try_catch_test` G4 verifies it clears abandoned depth+catch. ✓
- **try/catch state**: try saves regs+rsp+rip into `catch_bufs[catch_depth]` + `call_depth` into `catch_saved_call_depths[catch_depth]` after the **bounded-index guard** (`codegen.cpp:4521-4525`, the S3 fix); increments `catch_depth`; normal completion decrements; throw restores regs+rsp+rip + `call_depth` + longjmps to catch-entry rip (`:4610-4660`); `catch_depth==0` throw → `UnhandledThrow` trap. Nested try across frames: `catch_saved_call_depths` snapshots the catching frame's depth. ✓ (S3 fixed; G4 test passes.)
- **Nested calls**: `call_depth`/`catch_depth` are `context_t` fields shared across calls; `catch_depth` persists across script-to-script calls (only `reset_for_call` clears it). The S3 guard bounds it at every try-entry. ✓
- **Coroutines (S4 — STILL HOLDS, broader than the original report)**: trampoline (`ext_coroutine.cpp:209-227`) snapshots caller state, overwrites `ctx->checkpoint` with its setjmp, runs fn, restores on done/trap (`:227`). `n_coro_yield` (`:260-274`) + `n_coroutine_next` return control to the caller **without restoring** `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth`. A caller trap while a coroutine is suspended longjmps to the trampoline's setjmp (host loses the trap, caller time-warped; UAF after `coroutine_reset` `:441-456` `DeleteFiber`s a suspended coroutine). **Broader than the original S4: not just the checkpoint, the whole per-call state is clobbered across a yield.** No test covers this (all `tests/lang/valid_coroutine_*.ember` are basic yield/next/done functionality; none trigger a caller trap during suspension).
- **In-context threads (S7 — STILL HOLDS, documented)**: worker (`ext_thread.cpp:154-185`) locks `call_mutex`, saves state, runs, restores, unlocks. `thread_join` (`:280-300`) unlocks `call_mutex`, waits, reacquires. Caller is **blocked** on `call_mutex`/`cv.wait` for the whole clobber window; worker restores before caller resumes — the key contrast with S4. Contract: host must lock `call_mutex` around the outer `ember_call`; raw thunks do not enforce it. `in_context_threads_test` honors it (PASS); unenforced.
- **Trap recoverability (S5 — STILL HOLDS, documented)**: `emit_trap` → `ud2` when `trap_stub=null` (`codegen.cpp:358-376`); stub `abort()`s when `has_checkpoint=false` (`ember_cli.cpp:128-129`). Recoverable only when host sets both `trap_stub` + a `setjmp` checkpoint. `__builtin_longjmp` (not `std::longjmp`). Spec §2 is honest; risk is a host reading only §7's "one unwind primitive" assuming automatic recoverability. **H1 shows the VST3/DSP hosts hit this exact default — a trap kills the DAW.**

**Status: try/catch + reset_for_call VERIFIED (S3 fixed). Coroutine (S4), trap-default (S5), thread-contract (S7) still opt-in/documented. New hosts (H1) hit the trap-default crash posture.**

### G7. Opt-in/default-off posture + CLI/loader/new-host consistency

**Defaults (all OFF, `src/codegen.hpp`):** `trap_stub=null` (`:88`), `trap_ctx=null` (`:91`), `emit_budget_checks=false` (`:100`), `emit_depth_checks=false` (`:110`), `use_context_reg=false` (`:118`), `fn_allowlist_base=0`/`fn_slot_count=0` (`:134-135`), `use_gc_env=false` (`:230`), `enable_ir_backend=false` (`:205`). No `safe_defaults()` helper exists.

**CLI (`examples/ember_cli.cpp`) consistency:**
- JIT run path (`run_ember_file`, `:523-534`): opts IN — `trap_stub`, `use_context_reg`, `emit_budget_checks`, `emit_depth_checks`, `fn_allowlist_base`/`fn_slot_count`, all gated on `opts.emit_em_path.empty()`. `budget_remaining=100000000`, `max_call_depth=512`. `use_gc_env=opts.gc_env` (default false). Checkpoint via `__builtin_setjmp` (`:648,705`). **The guarded reference host.**
- `--emit-em` path: **all guards OFF** (C2) — the `emit_em_path.empty()` gating disables budget/depth/trap/call-target for emitted `.em`.
- `compile_static` (`:1061-1067`, used by `ember pipe`): guards ON unconditionally (not gated on emit-em).
- `ember pipe` (`:1230-1251`): per-call `budget_remaining=100000000`, `max_call_depth=512`, checkpoint + `call_depth=0` reset per stage. Consistent.
- `ember live` (`:1319,1360,1402`): each tick/reload call sets budget+depth+checkpoint. Consistent.
- `--tick` path (`:732`): own `tick_ctx` (B1 isolation). Consistent.
- **Inconsistent**: `compile_static` sets guards unconditionally while `run_ember_file` gates on emit-em — a `.em` emitted via the CLI has no guards (C2), but `ember pipe` (which uses `compile_static`) does.

**Loader (`src/em_loader.cpp`) — C1:** v5 IR re-emit `ictx` (`:668-676`) sets only `dispatch_base`/`globals_base`/`natives`/`script_slots`/`structs`/`enable_ir_backend`. Does NOT set `emit_budget_checks`/`emit_depth_checks`/`trap_stub`/`fn_allowlist_base`/`fn_slot_count`/`use_context_reg`. **A v5 `.em` re-emits with zero sandbox guards and `ud2` traps**, regardless of the source compile. `PERM_FFI` is the one guarantee the loader DOES enforce (3 gates). **X1 adds: the loader also does not fail closed on `CallCrossModule` when `!registry` — it crashes after allocation.**

**New hosts (H1):** `examples/vst3_wrapper/vst3_ember_processor.cpp:251-259` and `examples/vst_dsp_harness.cpp:118-123` construct default-off `CodeGenCtx`s; the VST3 processor invokes raw JIT pointers at `:802-803,814` with no `context_t`/checkpoint/trap stub. **The trusted/realtime opt-out end of the spectrum — but for a DAW plugin hosting third-party scripts with `PERM_FFI`, and without the plan's stated checkpoint+bypass-on-trap.** See H1.

---

## Prior closures re-verified (all still in place, tests pass)

- **FINDING A (v5 IR `frame_off` stack-smash):** FIXED — `src/thin_ir_ser.cpp:668-672` checks `frame_off` for every instr regardless of `frame_size`. `thin_ir_ser_test` "frame_off OOB" case PASSES.
- **`PERM_FFI` 3 gates:** sema `sema.cpp` (call-site check), v2-v4 bind `em_loader.cpp:435`, v5 IR rebind `em_loader.cpp:713-716`; default `module_permissions=0` (`em_loader.hpp:129`, `em_loader.cpp:567`). `em_v5_ir_test` "unknown native name -> rejected" + "natives==nullptr + CallNative -> rejected" PASS.
- **Raw-x86 default-reject:** `em_loader.hpp:130` (`allow_raw_x86=false`), `em_loader.cpp:581-585` rejects v1-v4 unless `allow_raw_x86=true`. `em_redteam_audit_test` (a)(b)(c)(d) PASS.
- **v5 IR validator completeness (other checks):** `thin_ir_ser.cpp:547-712` — duplicate block IDs (`:605`), negative lens for `ConstStringRef`/`StringDecrypt`/`BoundsCheck` (`:665-672`), `CallScript` slot range (`:677-682`), `CallCrossModule` **mod_id** range (`:683-688` — but NOT slot, see X1), `Cmp` predicate (`:690-694`), rodata OOB, `frame_off` (`:668-672`). `thin_ir_ser_test` "all validation edge cases rejected" PASSES — **but the test does not cover the `CallCrossModule` slot gap or the `registry_size=0` skip (X1); the "mod_id OOB" assertion in the test's summary line overstates coverage.**
- **`ext_array` element-bounds overflow:** `ext_array.cpp:70-75` bounds `i` against `bytes.size()/elem_size`. `ext_bounds_test` PASSES.

---

## Test Results (read-only execution of pre-built `build/` binaries, MinGW g++)

**Sandbox/security/ThinIR/GC/codegen suite (all PASS):**
`v0_4_hardening` · `try_catch` · `function_refs` · `cross_module_handles` · `em_redteam_audit` · `em_loader_hardening` · `em_signed` · `thread_safety` · `in_context_threads` · `thin_ir_ser` · `thin_ir` · `thin_ir_struct` · `em_v5_ir` · `em_v5_mixed` · `ext_bounds` · `gc_core` · `gc_integration` · `gc_full` · `ir_passes` · `regalloc` · `codegen_opt` · `ember_pass` · `fn_types` · `constexpr` · `type_stress` · `win64_abi` · `binding_abi`.

**Broader suite (all PASS):**
`aggregate_global` · `array_lit` · `call_raw` · `em_bytes` · `em_roundtrip` · `ext_lifecycle` · `ext_map` · `ext_registration` · `ext_runtime` · `ext_sync` · `for_each_array` · `import_roundtrip` · `pub_priv` · `v0_5_live_modules`.

**Language suite (`tests/run_lang_tests.sh ./build`): 463 passed, 0 failed, 0 skipped.**

**Key test confirmations:**
- `v0_4_hardening_test` H-M4-2: budget charging works when enabled (2000-term expr / 300 native calls / 8×8KiB aggregate copies trap under small budgets). M-M4-3: B1 per-context `max_call_depth`.
- `try_catch_test` G4: 257 nested active try blocks → 257th traps `StackOverflow` before `catch_bufs` OOB; `reset_for_call` clears abandoned `call_depth`+`catch_depth`. **S3 fix verified by test.**
- `function_refs_test` C1/C2: out-of-range forged handle (99999) and in-range-unregistered handle both trap `BadCallTarget` (call-target guard works when configured — tree-walker path).
- `cross_module_handles_test` A/B/C/D/E: all pass — via the **tree-walker** path (does not set `enable_ir_backend`; does not cover N1 or X1).
- `gc_full_test` / `gc_integration_test`: GC-heap env (`use_gc_env=true`) works. **S2 structural fix verified (opt-in).**
- `thread_safety_test`: B1 per-context isolation — thread B's tiny budget traps on its own context, no cross-thread longjmp corruption.
- `em_signed_test` C/D1/D2/E: Ed25519 content authentication.

**PoC results (NEW untracked, `tmp_edit/sandbox_poc/`):**
- `cross_module_slot_poc.exe`: 5/5 assertions — GAP-1a (negative slot accepted), GAP-1b (oversized slot accepted), GAP-2 (no-registry accepted), CONTROL (CallScript slot OOB rejected), CONTROL (CallCrossModule mod_id OOB rejected). **Confirms X1 at the validator level.**
- `cross_module_em_poc.exe B`: CASE B — `load_em_file` returns true, publishes 1 executable page with `slot=999999999`. **Confirms X1 fail-open end-to-end.**
- `cross_module_em_poc.exe A`: CASE A — **SEGFAULT (exit 139)** at `registry->base()` deref after `alloc_executable_rw`. **Confirms X1 no-registry crash end-to-end.**

**No regressions found.** The full tree is green on the executed suite. The X1 hole is a **validator incompleteness + missing fail-closed guard**, not a regression in existing behavior — the existing tests pass because they do not cover the gap.

---

## Newly discovered issues — ranked by severity

### X1 (HIGH) — v5 IR `CallCrossModule` slot validation hole + no-registry fail-open
**Evidence:** `src/thin_ir_ser.cpp:683-688` (mod_id-only, `registry_size>0`-gated, never `slot`); `src/thin_emit.cpp:830-835` (unchecked `in.meta.slot` at `:833`); `src/em_loader.cpp:745-746` (passes `registry ? registry->count() : 0u`); `src/em_loader.cpp:798` (alloc) → `:814-815` (null deref); no `!registry` guard for `CallCrossModule` (contrast `CallNative` at `:733-737`); v1-v4 has the guard at `:406-409` but v5 IR relocs bypass that loop. PoC: segfault (CASE A) + fail-open page publish (CASE B).
**Trigger:** a crafted v5 `.em` (unsigned, dev mode) with a `CallCrossModule` instr. With a registry: valid `mod_id` + oversized/negative `slot` → OOB read → wild call. Without a registry: any `CallCrossModule` → crash after executable allocation.
**Impact:** OOB read / wild call (memory corruption / controlled-call primitive) or DoS crash. Contradicts round-1's "v5 CallCrossModule range validation complete" + "every cross-module call path guarded."
**Fix shape (NOT applied):** (1) In `validate_thin_function`: reject any `CallCrossModule` when `registry_size == 0`; validate `meta.slot >= 0` always; range-check `slot` against the target module's `slot_count` (thread per-module counts into the validator). (2) In `em_loader.cpp` v5 re-emit path: add a `CallCrossModule`-when-`!registry` scan mirroring `:728-737`, rejecting before `emit_x64` / `alloc_executable_rw`. (3) Add tests (negative slot, oversized slot, no-registry `CallCrossModule`).

### H1 (HIGH) — New hosts (VST3/DSP harness) compile default-off + raw JIT invocation, no checkpoint/bypass (contradicts plan)
**Evidence:** `examples/vst3_wrapper/vst3_ember_processor.cpp:251-259` (default-off `CodeGenCtx`); `:802-803,814` (raw JIT ptr calls, no context/checkpoint/trap_stub); `examples/vst_dsp_harness.cpp:118-123` (default-off `CodeGenCtx`); `:222`/`:100` (`PERM_FFI` granted); `docs/planning/plan_VST3_EMBER_WRAPPER.md:72,285` (plan says checkpoint+bypass-on-trap; unimplemented). Zero `context_t`/`ember_call`/`checkpoint`/`setjmp`/`trap_stub` matches in either host.
**Impact:** a buggy/malicious `.ember` plugin script (OOB, div-by-zero, runaway loop, unbounded recursion) crashes/hangs the DAW process, not just the plugin. The plan's bypass-on-trap was not implemented.
**Fix shape (NOT applied):** implement the plan's checkpoint + bypass-on-trap (compile with `trap_stub` = bypass stub + `emit_budget_checks`/`emit_depth_checks`; wrap `process` in `setjmp` or use a stub-returns-normally + `trapped` flag); document the posture; reconsider unconditional `PERM_FFI` for third-party plugins.

### N1 (HIGH, round-1 — still holds) — ThinIR silently miscompiles cross-module function handles
**Evidence:** `src/thin_lower.cpp:1631-1637` (FnHandleExpr emits `ConstInt(h->slot)`; for `&mod::fn`, `h->slot=-1`); `src/thin_emit.cpp:329-352` (no bit-63 cross-aware branch); tree handles it at `src/codegen.cpp:2580-2588` + `:551-565`.
**Trigger:** `enable_ir_backend=true` + a module using `&mod::fn`.
**Fix shape (NOT applied):** set `non_serializable` for cross-module handles (force tree-walker fallback) OR emit the full handle + add cross-aware guard + cross-module indirect dispatch to ThinIR. Add a test that enables `enable_ir_backend` with `&mod::fn`.

### S2 (HIGH, prior — partially addressed) — lambda `env_ptr` escape
**Status:** structural fix (GC-heap env) opt-in/default-off; sema stopgap NOT added.
**Evidence:** GC path `src/codegen.cpp:1885-1934` (`use_gc_env`, default false `codegen.hpp:230`); sema guards `src/sema.cpp:2393,2407,2822,2012,2041` (is_slice only, no is_lambda).
**Fix shape (NOT applied):** add `is_lambda` (with `env_size>0`) to the three sema escape guards; consider defaulting `use_gc_env=true` for sandboxed compiles.

### S1 (HIGH, prior — still holds) — budget/depth/trap OFF by default
**Evidence:** `src/codegen.hpp:100,110,88,118`; no `safe_defaults()`. CLI opts in for JIT only. New hosts (H1) do not opt in.

### C1 (MEDIUM, round-1 — still holds) — Sandbox guards stripped on v5 `.em` load-time re-emit
**Evidence:** `src/em_loader.cpp:668-676` (re-emit `ictx` omits all guard flags).

### C2 (MEDIUM, round-1 — still holds) — CLI `--emit-em` compiles with all sandbox guards off
**Evidence:** `examples/ember_cli.cpp:526-534` (guards gated on `emit_em_path.empty()`).

### S4 (MEDIUM, prior — still holds, broader than reported, untested) — coroutine checkpoint + per-call state misrouting across yield
**Evidence:** `extensions/coroutine/ext_coroutine.cpp:209-227,260-274`; `restore_state` only on done/trap `:227`. `call_depth`/`budget_remaining`/`catch_depth` also clobbered across a yield (not just the checkpoint).

### S5 (MEDIUM, prior — still holds, documented) — trap = process death by default
**Evidence:** `src/codegen.cpp:358-376`; `examples/ember_cli.cpp:128-129`. **H1's VST3/DSP hosts hit this default.**

### S6 (MEDIUM, prior — still holds, both paths) — call-target guard no-op when unconfigured
**Evidence:** tree `src/codegen.cpp:533`; ThinIR `src/thin_emit.cpp:330`.

### S7 (LOW, prior — still holds, documented) — in-context thread `call_mutex` contract unenforced
**Evidence:** `extensions/thread/ext_thread.cpp:284`; `src/engine.cpp:222-247`.

### N2 (LOW, round-1 — still holds) — ThinIR double call-target guard for indirect calls
**Evidence:** `src/thin_lower.cpp:2319` + `src/thin_emit.cpp:1989`.

### N3 (LOW, round-1 — still holds) — ThinIR call-target guard lacks cross-aware bit-63 routing
**Evidence:** `src/thin_emit.cpp:329-352` vs `src/codegen.cpp:551-565`.

---

## Severity-ordered recommendations (no edits made — this audit is read-only)

1. **X1** — fix `validate_thin_function` to reject `CallCrossModule` when `registry_size==0` + validate `meta.slot` (negative always; range against target `slot_count` when possible); add a `!registry` `CallCrossModule` scan in the v5 re-emit path mirroring the `CallNative` scan. Add the three tests (negative slot, oversized slot, no-registry). **Fail closed before `alloc_executable_rw`.**
2. **H1** — implement the VST3 plan's checkpoint + bypass-on-trap; compile plugin `process_f32`/`process_f64` with `trap_stub` + budget/depth guards; document the posture; reconsider unconditional `PERM_FFI` for third-party plugins.
3. **N1** — fix the ThinIR `FnHandleExpr` handler (set `non_serializable` for cross-module handles, or emit the full handle + cross-aware guard). Add a `enable_ir_backend` + `&mod::fn` test.
4. **S2** — add `is_lambda` (with `env_size>0`) to the three sema escape guards; consider defaulting `use_gc_env=true` for sandboxed compiles.
5. **C1 + C2** — decide/document whether `.em` is the guard-free trusted-artifact path, or thread guard flags through `EmLoadPolicy` so a host can load a sandboxed `.em`. Add a guard-retention-across-serialization test.
6. **S4** — restore caller `checkpoint`/`call_depth`/`budget_remaining`/`catch_depth` across each yield, or give coroutines their own `context_t`. Add a caller-trap-during-suspension test.
7. **S1 + S5 + S6 + S7** — ship a `CodeGenCtx::safe_defaults()` / `ember_compile` / `ember_call_in_context` so the default integration path is the sandboxed one. One API addition closes four findings.
8. **N2** — remove the redundant `CallTargetGuard` ThinInstr in `lower_call` (keep only the `emit_call` site).

---

*End of re-validation (round 2). READ-ONLY; no tracked source files modified, no commits. Findings verified against `src/` at HEAD `f256ff9`. PoCs under gitignored `tmp_edit/sandbox_poc/` (untracked). All executed existing tests (27 sandbox/security/ThinIR/GC + 13 broader + 463 lang) PASS; PoCs confirm X1 (segfault + fail-open).*
