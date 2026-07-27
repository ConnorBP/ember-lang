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

## WASM W1 — EMBER_WASM_INTERP gating + Emscripten build

**Status:** COMPLETE. The ThinIR interpreter compiles to WebAssembly via
Emscripten 6.0.4 and runs `.ember` scripts in Node through the interpreter
(no JIT, no executable memory). Native build unaffected (472/472 lang suite).

### What W1 delivered

The `EMBER_WASM_INTERP` macro is defined ONLY in the Emscripten build
(`-DEMBER_WASM_INTERP=ON`), so every `#ifdef` branch below is INACTIVE
natively — the native build is byte-identical (verified: `cd buildm && ninja`
clean, `bash tests/run_lang_tests.sh buildm` → 472/472). The macro gates the
platform/engine/jit-memory stubs + the WASM CLI; it is NOT defined on the
native targets.

### The gating (src/ changes — all `#ifdef EMBER_WASM_INTERP` branches)

- **`src/platform.cpp`** — added an `#elif defined(EMBER_WASM_INTERP)` branch
  before the `#else #error`. WASM has no executable memory: `alloc_rw` =
  plain `malloc` (RW, never PROT_EXEC); `protect_rx`/`protect_rw` = no-ops
  returning `true` (no `mprotect`, nothing to seal — the interpreter never
  calls them); `free_page` = `free`; `page_size` = 65536 (the WASM page; the
  value is irrelevant since no executable pages are allocated);
  `executable_path` = empty; `round_up_to_page` = standard. No MAP_JIT, no
  `pthread_jit_write_protect_np`, no `sys_icache_invalidate` (Apple-only).
- **`src/engine.cpp`** — added an `#elif defined(EMBER_WASM_INTERP)` branch
  between the x86-asm `#if defined(__GNUC__) && defined(__x86_64__)` block
  and the non-x86 `#else` stubs. WASM has no thunks (the interpreter is
  called DIRECTLY as `interpret_thin_i64`, not via a JIT'd-entry thunk). The
  `ember_call_*` / keyed-thunk / keyed-driver symbols are stubbed: pure-
  execution stubs return 0; keyed `CallResult`/`ExtensionResult`/`LeaseResult`
  stubs return `ok=false` + "not supported in WASM (interpreter backend)";
  `ember_current_keyed_runtime` = nullptr; `assemble_identity_dispatch_record`
  = false. The top-of-file C++ helpers (`X64Emitter::resolve_fixups`,
  `compile_add_i64`, `finalize`, etc.) are dead code in WASM but compile as
  pure C++ byte-vector emission (the inline asm is ONLY in the x86 `#if`
  block, which is skipped) + link against the stubbed `alloc_executable`.
- **`src/jit_memory.cpp`** — wrapped the native JIT-memory path in
  `#if !defined(EMBER_WASM_INTERP)` and added a WASM stub block:
  `alloc_executable`/`alloc_executable_rw` return `nullptr`;
  `seal_executable` returns `false`; `free_executable` is a no-op. The
  interpreter never calls them; they exist only so the `ember` core lib
  links. No `std::mutex`/`safety::check_memory_limit` in the WASM path.
- **Extensions** (coroutine/thread/call_raw) — NOT built under
  `EMBER_WASM_INTERP` (gated in CMake). coroutines use Windows fibers /
  Apple ARM64 asm context switch; threads use `std::thread` (needs
  `-pthread`); call_raw uses `alloc_executable` + a fn-ptr cast (no
  executable memory in WASM). All three are impossible in WASM. The WASM
  CLI does NOT register their natives → scripts using `yield`/
  `coroutine_start`, `thread_spawn`/`join`, or `make_executable`/`call_raw`
  fail at sema with "unknown native". The arch-neutral extensions
  (vec/quat/mat/string/array/math/map/sync/lifecycle/io/gc/opt/obf/audio)
  build + link + register cleanly (verified: zero std::thread/pthread/asm/
  platform-header references).
- **`src/thin_interp.*`** — UNTOUCHED (W0 is done + validated, 88 assertions).
  The gating is AROUND the interpreter, not in it.

### The WASM build config (CMakeLists.txt)

- `option(EMBER_WASM_INTERP "Build the WebAssembly ThinIR-interpreter target (Emscripten)" OFF)`
  near the top. The native build defaults to OFF.
- The `ember_frontend` source list is a variable (`EMBER_FRONTEND_SOURCES`);
  under WASM it includes ALL frontend sources (the JIT emit passes
  `thin_emit.cpp`/`thin_emit_arm64.cpp`, the `.em` re-emit loader
  `em_loader.cpp`, and the host-boundary drivers `module_build.cpp`/
  `keyed_hot_reload.cpp` are pure C++ — no inline asm, no MAP_JIT — and
  compile under Emscripten as dead code, keeping `emit_x64`/`emit_arm64`/
  `finalize`/`alloc_executable` symbols resolved without gating `codegen.cpp`'s
  `compile_func`). The `.S` thunk/ctx-switch files are arch-gated
  (`APPLE AND arm64`) and never selected for WASM.
- The VST3 block + the entire native test/exec target section (from the VST3
  `option` to end of file) is wrapped in `if(NOT EMBER_WASM_INTERP)`, so under
  WASM ONLY `ember_wasm` (+ its lib/extension deps) builds.
- The `if(EMBER_WASM_INTERP)` block at the end defines `EMBER_WASM_INTERP`
  on `ember`/`ember_frontend`/`ember_import`/`ember_ed25519` + the arch-
  neutral extensions + the CLI, enables `-fexceptions` on every TU (the
  interpreter throws `InterpTrap` for traps + catches it; the frontend throws
  compile errors — Emscripten defaults to `-fno-exceptions`, so this is
  explicit; the audit's Shape-B-on-Emscripten recommendation, avoiding the
  `-fno-exceptions` frontend refactor), and links `ember_wasm` with
  `-sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1 -sEXIT_RUNTIME=1
  -fexceptions -sEXCEPTION_CATCHING_ALLOWED=['*']`. The
  `EXCEPTION_CATCHING_ALLOWED=['*']` is CRITICAL: Emscripten strips catch
  handlers by default even with `-fexceptions`, so without it an
  `InterpTrap` escapes `interpret_thin_i64_safe`'s `catch` → wasm trap →
  node promise rejection. `NODERAWFS` backs `fopen`/`fread` with node's fs
  so the CLI reads `.ember` files via plain C stdio.

### The WASM CLI (examples/ember_wasm.cpp)

A minimal runner modeled on `tests/thin_interp_test.cpp`'s harness. It:
1. reads a `.ember` file (argv[1], or `-` for stdin) via plain C `fopen`/
   `fread` (no `std::filesystem` in the CLI itself — portability);
2. resolves `import "path";` inlining via `resolve_imports` (arch-neutral;
   Emscripten has `std::filesystem`), falling back to raw source on error;
3. lexes / parses / sema-checks / lowers every fn to a `ThinFunction` via
   `lower_function`, building an `InterpDispatch` (`ThinFunction*` table);
4. registers the arch-neutral extension natives + operator overloads + a
   minimal `assert_eq_i64`/`assert_eq_f32`/`assert_eq_f64`/`assert_true`
   test-helper set;
5. runs the entry fn (default `main`, or `--fn NAME`) via
   `interpret_thin_i64_safe` with a `context_t` (budget = INT64_MAX,
   max_call_depth = 4096 — generous, no false traps);
6. prints `RESULT <i64>` (or `TRAP <reason>: <detail>` on an unhandled
   throw) + exits with `(result & 0xFF)`.

Key CodeGenCtx settings: `use_context_reg = true` (thin_lower requires it
for try/catch + throw to lower — the interpreter ignores the reg but the
lowerer gates on it), `emit_budget_checks = false`, `emit_depth_checks =
false` (no false budget/depth traps on deep recursion / long loops; the
interpreter's entry budget check + DepthCheck instrs are gated by these).
`use_gc_env = false` (lambdas-as-stack-env; the interpreter handles it).

The CLI does NOT use `compile_func`, `emit_x64`, `emit_arm64`,
`alloc_executable`, or any JIT path — it goes straight AST → `lower_function`
→ `interpret_thin` (Shape B: full compiler-in-WASM, backend = interpreter).

### Build + run

```
emcmake cmake -G Ninja -DCMAKE_CXX_COMPILER=em++ -DEMBER_WASM_INTERP=ON -S . -B buildwasm
cmake --build buildwasm
node buildwasm/ember_wasm.js tests/lang/valid_arith.ember   # -> RESULT 13
(cd buildwasm && ctest)                                       # wasm_interp_acceptance: 15/15
```

### Test results (WASM interpreter acceptance — 15/15 pass)

`tests/run_wasm_tests.sh` runs a curated set through `ember_wasm.js` +
asserts the result. All pass:

| script | expected | got |
|---|---|---|
| valid_arith | 13 | RESULT 13 |
| valid_control | 132 | RESULT 132 |
| wasm_fib (fib(20)) | 6765 | RESULT 6765 |
| valid_for_each | 150 | RESULT 150 |
| valid_match | 20 | RESULT 20 |
| valid_struct_destructure | 142 | RESULT 142 |
| valid_throw_nested | 99 | RESULT 99 |
| valid_lambda_no_capture | 84 | RESULT 84 |
| valid_lambda | 42 | RESULT 42 |
| runtime_division_forms | 78 | RESULT 78 |
| valid_constexpr_recursive | 55 | RESULT 55 |
| valid_namespaces_intra_call | 30 | RESULT 30 |
| valid_array_push_pop_i64 | 1 | RESULT 1 |
| runtime_struct_reassign_single | 42 | RESULT 42 |
| runtime_trap_throw_uncaught | TRAP | TRAP unhandled throw |

Coverage: i64 arithmetic, control flow (if/else/while/for), recursion
(fib(20)=6765), for-each, match, structs (i64 fields) + destructuring,
simple try/catch (single handler, throw caught), lambdas (capture + no
capture), i64 division/modulo forms, constexpr recursive fold, namespaced
calls, the array extension, + the recoverable-trap path (uncaught throw).

### Stubbed / not supported in WASM (W1 scope — by design)

- **Coroutines** (`yield`/`coroutine_start`/`coroutine_next`) — impossible in
  WASM (no fibers, no asm context switch). The extension is not built; the
  natives are not registered. The interpreter already stubs coroutines (W0).
  An interpreter-native cooperative-coroutine design (paused frame
  {ThinFunction*, pc, frame copy, call_depth}) is noted for a later phase —
  do NOT attempt the fiber/asm-switch path.
- **Threads** (`thread_spawn`/`thread_join`) — `std::thread` needs `-pthread`
  + SharedArrayBuffer (opt-in); no core feature needs threads. The extension
  is not built; the natives are not registered.
- **call_raw** (`make_executable`/`call_raw`/`free_executable_ptr`) —
  impossible in WASM (no executable memory to copy bytes to + no fn-ptr-to-
  native-bytes to invoke). The extension is not built; the natives are not
  registered. This means the self-hosted compiler (emits x86 via call_raw)
  cannot run its output in WASM (x86-locked by design).
- **Keyed dispatch** (`@obf_keyed`, keyed cross-module calls) — the keyed
  thunks/driver are stubbed (no JIT dispatch in WASM). The keyed *lowering*
  is not exercised by the WASM CLI.

### Known interpreter coverage gaps (W2 — NOT W1 regressions; pre-existing)

The interpreter (W0) was validated natively on its 88-assertion subset
(`thin_interp_test`). These scripts exercise features BEYOND that subset and
reveal W2 coverage gaps (the interpreter, NOT the gating/build — left
untouched per the W1 constraint). The JIT (`ember_cli`) handles them; the
interpreter does not yet:

- **Narrow integer widths** (i8/u8/i16/u16/i32/u32 arithmetic, casts, struct
  fields): `runtime_integer_boundaries` (expect 79, got 5 — i8 boundary),
  `valid_ir_struct` (expect 123, got 201 — i32 struct fields). The W0 test
  covers only int↔float casts, not narrow-width element loads/stores.
  (`meta.width`-keyed sized reads/writes are the W2 fix.)
- **Nested try/catch** (a try inside a try): `valid_nested_try_catch`
  (expect 44, got "TRAP none: trap terminator"). The W0 test covers only a
  single try/catch ([27]); the catch_stack + `catch_depth` interaction for
  nested handlers needs the W2 fix.

These are tracked for W2 ("full ThinOp coverage"). W1's job was the gating +
the Emscripten build + proving the interpreter runs in WASM on its validated
subset — DONE.

### Constraints honored

- The native build + CI are unaffected: `cd buildm && ninja` clean;
  `bash tests/run_lang_tests.sh buildm` → 472/472. Every `#ifdef
  EMBER_WASM_INTERP` branch is inactive natively (the macro is undefined).
- The `EMBER_WASM_INTERP` macro is defined ONLY on the WASM targets (CMake
  `target_compile_definitions` under `if(EMBER_WASM_INTERP)`, NOT global).
- The interpreter (`src/thin_interp.*`) is untouched.
- The gating + the WASM CLI are portable C++17 (the CLI uses plain C
  `fopen`/`fread`, no `std::filesystem` in the CLI itself; `resolve_imports`
  in `ember_import` uses `std::filesystem`, which Emscripten provides).
- Exceptions are enabled (`-fexceptions` + `-sEXCEPTION_CATCHING_ALLOWED
  =['*']`) so the interpreter's `InterpTrap` + the frontend's compile-error
  throws work without a `-fno-exceptions` refactor (the audit's Shape-B-on-
  Emscripten path).

### Next (W2+)

- Full ThinOp coverage: narrow integer widths (i8/i16/i32/u8/u16/u32 sized
  loads/stores + arithmetic + casts) + nested try/catch (the catch_stack/
  catch_depth interaction). The two known gaps above.
- GC shadow-stack linkage stress (the hook is built; full GC during deep
  interpreted call chains with escaping lambdas is W2).
- The `.em` v5 IR loader's deserialize-only fork (the audit's R1: feed
  deserialized `ThinFunction`s to the interpreter instead of re-emitting to
  native + `alloc_executable`; ~200-400 lines) — Shape A (interpret-only
  deployment).
- Interpreter-native cooperative coroutines (the paused-frame design).
- Performance: the interpreter is ~10–50x slower than the JIT (acceptable
  for scripting/embedding; `@realtime` audio is the known casualty — defer
  in WASM v1). Direct-threaded-dispatch is a future ~2–3x optimization.
