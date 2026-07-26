# WASM Progress — the ThinIR interpreter (plan_WASM.md)

Tracks the WASM port milestones. **W0 = the ThinIR interpreter, built + validated
natively (macOS ARM64) alongside the JIT.** Emscripten/wasi-sdk is W1 (a later
task). The interpreter is ADDITIVE — it does not touch the JIT path.

## WASM W0 — ThinIR interpreter

**Status: COMPLETE.** The interpreter builds natively (macOS ARM64, Apple Clang),
passes its end-to-end gate (`thin_interp_test`, 89 assertions), and the full
suite holds: lang_suite **472/472**, ctest **68/68** (no regression — the
interpreter is additive and does not modify any existing `src/` file).

### Files added (W0)
- `src/thin_interp.hpp` — the interpreter API: `interpret_thin_i64` /
  `interpret_thin_f64` / `interpret_thin_void` / `interpret_thin_i64_safe`
  + the `InterpTrap` struct + the `InterpDispatch` /
  `InterpCrossModuleTables` / `InterpHandleRecords` dispatch types.
- `src/thin_interp.cpp` — the interpreter loop (switch over `ThinOp`, frame
  buffer, recursive calls, trap/catch, GC linkage hook).
- `tests/thin_interp_test.cpp` — the validation gate (34 probes, 89
  assertions): lowers real `.ember` sources via `lower_function` then runs them
  through `interpret_thin`, asserting the correct values (matching `emit_arm64`).
- `CMakeLists.txt` — `thin_interp.cpp` added to `ember_frontend`;
  `thin_interp_test` added as an Apple-gated CTest target (`thin_interp`).
- (this doc) `docs/planning/WASM_PROGRESS.md`.

### The interpreter design

**VReg model — frame-buffer, mirroring `emit_arm64`.** The interpreter maintains
a `vregs[VReg] -> {frame_off, type}` map (built lazily as producing instrs
execute: a producing instr with `dst != 0` + `meta.frame_off != 0` records the
dst's home frame slot). Reading a src vreg reads from its frame slot
(width-normalized per its type); writing a dst vreg writes to its frame slot.
VRegs with `frame_off == 0` (emit_arm64's "trust x9" fallback — rare in
well-formed lowering) fall back to a `vreg -> {int|float}` value table. This is
`emit_arm64`'s `load_int_vreg` / `pin_int_dst` / `record_dst_x9` model
translated to C++. Validated: the model produces correct results for every
covered ThinOp.

**Frame — a `frame_size` byte buffer.** `LoadFrame`/`StoreFrame` access it at
`meta.frame_off` (a negative offset; `addr(off) = buf + frame_size + off`).
`CopyBytes` `memmove`s within it. Struct/array temps live in it at their frame
offsets. Scalar slots are 8 bytes (mirrors `emit_arm64`'s
`frame_load64`/`frame_store64`); narrow element loads/stores honor `meta.width`.
Int values are normalized to their type's width on read (sign/zero-extend, like
`normalize_x9`).

**Call mechanism — recursive `interpret_thin`.**
- `CallScript`: look up `dispatch[slot]` -> `ThinFunction*`, marshal args from
  the caller's vregs/frame into a flat `int64_t` word array (one word per param;
  slices = 2 words {ptr, len}; struct-by-value = 1 word = `const void*` to the
  bytes; floats = bit-cast), recurse, place the result. Handles
  `returns_struct_by_ptr` (the hidden return-ptr slot; the caller provides a
  dest buffer).
- `CallNative`: resolve by `meta.native_name` from `ctx.natives` (NEVER
  `native_fn`), marshal args, call the `fn_ptr` via a typed dispatcher that
  inspects the `NativeSig` (handles all-int, all-double, all-float, + the
  mixed int/float combos the array extension uses), return.
- `CallIndirect`: the handle is in `src1` (validated by the preceding
  `CallTargetGuard`). Bit 63 = cross-module -> `handle_records` table
  (mod_id + slot + allowlist); else intra -> `dispatch[handle]`.
- `CallCrossModule`: `cross_module_tables[mod_id][slot]` -> `ThinFunction*`.

**Traps — a C++ exception (`struct InterpTrap { TrapReason; detail; }`)** thrown
on a trap, caught at the top-level `interpret_thin` call (sets `ctx->last_trap`
+ returns 0) OR at `TryCatch` boundaries for in-language catch. The WASM build
(`-fno-exceptions`) will swap this to `setjmp`/`longjmp` or error-code return;
the trap policy is isolated behind `EMBER_INTERP_TRAP_THROW` so it can be swapped
without touching the loop. `interpret_thin_i64_safe` is the recoverable-trap
variant (catches `InterpTrap` + reports `trapped`).

**Try/catch — pc-restore + call_depth-restore, NOT libc `longjmp`** (the audit's
CRITICAL correction). The interpreter saves `(block_idx, call_depth)` at
`TryCatch` (onto a per-invocation catch stack + the context's
`catch_depth`/`catch_saved_call_depths` for JIT compatibility); `Throw` restores
them + jumps to the catch entry (the block at `meta.slot`). `CatchCleanup` pops
the catch stack. `CatchEntry` loads `thrown_value` into the catch_name slot
(`meta.frame_off`). A throw with no enclosing catch -> trap (`UnhandledThrow`).
Cross-frame throws: the callee's `Throw` (no catch there) throws `InterpTrap`,
which propagates to the caller's `CallScript` handler; if the caller has a catch,
it restores + jumps to the catch entry (the thrown_value set by the callee's
`Throw` is preserved); else it re-throws to the next frame up.

**GC shadow-stack linkage (the HOOK — full GC testing is W2, but the hook is
built now).** On entry to a fn with `thf.frame.gc_ptr_frame_offs` non-empty (or
`gc_rec_off != 0`), the interpreter allocates a `gc::GcFrameRecord`, sets its
`map` to a `gc::GcFrameMap` built from `gc_ptr_frame_offs`, sets `frame_base` to
the interpreter frame buffer base, links it onto `ctx->gc_frame_head`; on exit
(RAII), unlinks. This lets `gc_collect()` during an interpreted lambda-using
script find the roots (the collector walks `gc_frame_head` via `ext_gc`'s
`context_roots_trace_cb`). Validated: the gc_full-style probes ([28] by-ref
capture, [29] write-through, [30] gc_new/delete/collect) pass with the GC
extension attached.

### ThinOps — fully implemented vs stubbed

**Fully implemented (mirror `emit_arm64`'s semantics):**
- Constants: `ConstInt`, `ConstFloat`, `ConstBool`, `ConstStringRef`,
  `StringDecrypt` (the two-slot layout: `data_temp_off` + `frame_off`).
- Moves/memory: `Move`, `LoadFrame` (ordinary + computed-address), `StoreFrame`
  (ordinary + computed-address + the `field_off!=0` exact-width flag),
  `LoadGlobal`, `StoreGlobal` (incl. slice relative<->absolute), `CopyBytes`
  (frame/vreg/global, all directions), `StoreAddr`.
- Int arithmetic: `Add`, `Sub`, `Mul`, `Div`, `Mod` (signed/unsigned + div-by-
  zero + signed-overflow guards), `And`, `Or`, `Xor`, `Shl`, `Shr`, `Neg`,
  `Not`, `BitNot`.
- Float arithmetic: `FAdd`, `FSub`, `FMul`, `FDiv`, `FMod` (portable
  `std::fmod` overloaded form, NOT `std::fmodf`).
- Compare: `Cmp` (int signed/unsigned + float with NaN/unordered handling — `<`
  `<=` `>` `>=` `==` false on NaN, `!=` true, mirroring `emit_arm64`'s mi/ls).
- Short-circuit: `LAnd`, `LOr` (ThinIR is three-address — srcs are
  already-evaluated vregs, so a plain logical op is correct).
- Cast: `Cast` (int<->int width, int<->float, f32<->f64).
- Calls: `CallNative` (typed dispatcher), `CallScript` (recursive),
  `CallIndirect` (intra + cross-module bit-63), `CallCrossModule`.
- Addresses/aggregates: `FieldAddr`, `IndexAddr` (with the FRESH-meta pin so
  the address result does NOT overwrite the backing array — mirrors
  `emit_arm64`'s temporary-home pin), `BoundsCheck`, `DivOverflowCheck` (no-op
  — `Div`/`Mod` emit their own inline guards), `MakeSlice`, `StructLitInit`,
  `ArrayLitInit`.
- Guards: `DepthCheck`, `BudgetCheck`, `CallTargetGuard` (the allowlist bit-test
  with the bit-0 isolation fix + the cross-module bit-63 skip).
- Try/catch: `TryCatch`, `CatchCleanup`, `CatchEntry`, `Throw`.

**Stubbed / deferred:**
- **Coroutines** — STUB. The design (per the WASM audit): a coroutine = a
  paused interpreter frame `{ThinFunction*, pc, frame copy, call_depth}`;
  `yield` = save + return to the resumer; `resume` = re-enter
  `interpret_thin` at the saved pc. This is the standard interpreter
  cooperative-coroutine pattern (generators in Python/JS VMs). Full coroutine
  support is deferred to W2+ (~100-200 lines; the JIT's fiber/asm-context-
  switch path is impossible in WASM, but the interpreter-native design IS
  feasible). Noted in the test ([31] STUB).

### Test results (ACCEPTANCE — macOS ARM64)
- `cd buildm && ninja thin_interp_test && ./thin_interp_test` -> **PASS**
  (89 assertions across 34 probes: int arithmetic, control flow, calls +
  recursion, floats, casts, structs (by-value arg/return), slices, for-each,
  match, try/catch/throw, lambdas + GC, coroutines STUB, cross-module handles,
  indirect calls, large-frame).
- `bash tests/run_lang_tests.sh buildm` -> **472/472** (no regression — the
  interpreter is additive; the JIT path is untouched).
- `ctest --timeout 600 -j4` -> **68/68** (all existing tests + `thin_interp`).

### Semantics — verified vs guessed

**Verified against `emit_arm64` (read `src/thin_emit_arm64.cpp` for each op):**
- The VReg/frame model (`load_int_vreg`/`pin_int_dst`/`record_dst_x9`), the
  8-byte scalar slot width, the width-normalize-on-read.
- `IndexAddr`'s FRESH-meta pin (the backing-array-base `meta.frame_off` is NOT
  the dst's spill slot — `emit_arm64` uses a temporary `ThinMeta home{}`).
  **This was a real bug caught during validation** (the first version pinned
  `IndexAddr` to `in.meta`, overwriting the backing array -> garbage slice
  reads; fixed by mirroring `emit_arm64`'s temporary-home pin).
- `StoreFrame`'s `field_off != 0` exact-width flag (aggregate field stores).
- `MakeSlice` does NOT pin to `frame_off` (that would overwrite the backing
  array; the result stays in the value table until a following `StoreFrame`).
- `StringDecrypt`'s two-slot layout (`data_temp_off` + `frame_off`).
- `LoadGlobal`/`StoreGlobal` slice relative<->absolute (slice global ptr is
  stored relative; load adds `globals_base`, store subtracts it).
- `CopyBytes`'s dst-vreg / global / frame disambiguation (the `base_kind` +
  `dst`/`src1` sentinel logic).
- `Cmp`'s NaN/unordered mapping (mi/ls false on NaN).
- `Cast`'s normalize-from-then-to-width for int<->int.
- `CallTargetGuard`'s bit-0 isolation + cross-module bit-63 skip.
- Try/catch as pc-restore (the audit's CRITICAL correction) — NOT libc longjmp.
  **The propagated-catch `thrown_value` preservation was a real bug caught
  during validation** (the first version zeroed `thrown_value` in the
  `CallScript` catch handler, losing the thrown value; fixed to preserve the
  value the callee's `Throw` set).

**Guessed / best-effort (not directly verified, but correct by construction):**
- The native-call typed dispatcher's mixed-type combos (array_set_f32: I,I,F).
  The dispatcher handles the combos that appear in the standard extensions;
  an unhandled combo throws (none appear in the tests). The all-int / all-double
  / all-float paths are verified by probes [5], [6], [14], [10], [11].
- The `CallIndirect` cross-module bit-63 path (implemented per `emit_arm64`'s
  `emit_indirect_call` cross path, but only the intra-module path is exercised
  by test [33]; the cross-module handle path is implemented but not directly
  tested — the `handle_records` table is wired but no probe constructs a
  cross-module handle. Deferred to W1/W2 with the full cross-module test
  harness).
- `interpret_thin_slice_result` is a stub (the test reads slice returns via the
  `InterpResult` path in `interpret_thin_i64`, which returns the slice ptr;
  the len is not exposed via the public API for slice returns. A full
  slice-return accessor is a W1 follow-up if needed).
- The GC hook is built + the gc_full-style probes pass, but FULL GC stress
  (collection during a deep interpreted call chain with many escaping lambdas)
  is W2 — the hook is correct by construction (it mirrors `emit_arm64`'s
  `emit_gc_frame_record_prologue`/`epilogue`), but only lightly stressed.

### Constraints honored
- Built NATIVELY first (macOS ARM64, Apple Clang). NO Emscripten/wasi-sdk (W1).
- ADDITIVE — no existing `src/` file modified (only `thin_interp.hpp` +
  `thin_interp.cpp` + the test + CMake + this doc).
- Portable C++17 (no macOS-specific APIs; no `std::filesystem`; `std::fmod`
  overloaded form, not `std::fmodf`). Compiles under Emscripten/wasi-sdk later.
- Mirrors `emit_arm64`'s semantics exactly (read `thin_emit_arm64.cpp` for each
  op); where in doubt, the interpreter's result matches the JIT's.

### Next (W1)
- The `EMBER_WASM_INTERP` gating macro + Emscripten/wasi-sdk build (the audit's
  R3: new `#elif`/`#if` branches in `engine.cpp`/`platform.cpp` + stubs for
  `thread`/`call_raw`/`coroutine`; ~300-500 lines).
- Swap the trap policy from C++ exceptions to `setjmp`/`longjmp` or error-code
  return for `-fno-exceptions` (the `EMBER_INTERP_TRAP_THROW` macro is the
  swap point).
- The `.em` v5 IR loader's deserialize-only fork (the audit's R1: feed
  deserialized `ThinFunction`s to the interpreter instead of re-emitting to
  native + `alloc_executable`; ~200-400 lines).
- Full coroutine support (the paused-frame design).
- Full GC stress testing (W2).
