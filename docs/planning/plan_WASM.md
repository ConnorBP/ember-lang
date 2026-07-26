# Plan — WASM target (compile the ember compiler/interpreter to WebAssembly)

> ⚠️ **Audited in `WASM_AUDIT.md` (GO verdict — "READY-WITH-FIXES").**
> Before W0 starts, incorporate the audit's corrections. The headline
> deltas vs. this plan:
> - The interpreter is **~2.5–3.5k lines** (not 1–2k) — the ~1–2k core is
>   real, but the gating + GC + loader-split + extension-stub work adds
>   ~500–1k lines the plan doesn't budget.
> - `EMBER_WASM_INTERP` gating is **NEW**, not a flip of an existing pattern —
>   the `#else` arm in `engine.cpp` currently services ARM64, not WASM, so the
>   "compile out the native JIT" step must be **authored** and is larger than
>   implied.
> - The **GC shadow-stack linkage** (~100–150 lines, W2) is **required** and
>   NOT mentioned in this plan: an interpreter has no JIT'd prologue/epilogue,
>   so it must synthesize the `GcFrameRecord` linkage itself (on entry to a fn
>   with `frame.gc_ptr_frame_offs` non-empty, link a record; on exit, unlink) —
>   else `gc_collect()` during an interpreted lambda-using script misses stack
>   roots → use-after-free.
> - The **`.em` loader deserialize-only fork** (~200–400 lines, W3) is
>   **required**: the v5 IR loader's re-emit path is hardcoded to
>   `emit_arm64`/`emit_x64` + allocates executable pages (`em_loader.cpp:1176-
>   1181`); Shape A needs a *deserialize-only* path that feeds the interpreter,
>   not the re-emit path.
> - **`std::filesystem`** (in `ext_io.cpp` + `import.cpp`) is a **wasi-sdk
>   blocker** — not in wasi-libc by default (it IS in Emscripten). **Use
>   Emscripten first** (the audit's toolchain recommendation); under wasi-sdk,
>   gate the `std::filesystem` calls behind `#ifndef EMBER_WASM_INTERP`.
> - Interpreter coroutines are **possible + ~100–200 lines** (a coroutine = a
>   paused interpreter frame: `{ThinFunction*, pc, frame buffer copy,
>   call_depth}`; yield = save + return; resume = re-enter at saved pc) — the
>   standard interpreter coroutine pattern — but a NEW design not in the
>   estimate. Defer from the first milestone (stub, as this plan says) but note
>   the design exists; do NOT attempt the fiber/asm-switch path.
>
> See `WASM_AUDIT.md` for the full per-question findings (Q1–Q6) + the 11
> corrections. This plan is retained as the design blueprint; the audit
> sharpens it into a buildable spec.

**Status:** PLANNING. Implement AFTER the Apple Silicon (ARM64) port is complete
(plan_MACOS_ARM64.md) — **the ARM64 port is now COMPLETE (56/56 CTest, 471/471
lang_suite)**, so the WASM prerequisite is satisfied. This is a third "target"
alongside `x64-Win64` and `arm64-Darwin`: **`wasm-interp`** — ember compiled to
WebAssembly, running `.ember` scripts via a ThinIR interpreter (no JIT).

## 0. The dominant constraint — WASM has no JIT

WebAssembly is a **Harvard architecture**: code is not addressable at runtime.
A WASM module's functions are numbered; `call` takes the callee as an immediate,
`call_indirect` takes a table index. **There is no user-allocated executable
memory** — you cannot `mmap(PROT_EXEC)`, write machine bytes, and jump to them.
(Confirmed: wingolog "just-in-time code generation within webassembly", 2022;
the `wasm-jit` proposal work requires generating a *new* WASM module at runtime
+ late-linking it via `call_indirect` + a growable/exported function table +
Wizer or runtime instantiation — complex and runtime-dependent.)

**ember's core mechanism is JIT-to-native** (emit x86-64/ARM64 bytes → W^X page
→ call). This **cannot work in pure WASM.** So the WASM target does NOT use the
native codegen. Instead it uses ember's **arch-neutral ThinIR** as the seam:
the frontend lowers AST → `ThinFunction` (already target-independent), and a
new **ThinIR interpreter** executes the IR in pure C++ — no native code emitted,
no executable memory. This is the natural third backend:

| Target | ThinIR → ... | Runs via |
|---|---|---|
| `x64-Win64` | `emit_x64` → x86-64 bytes | W^X page + Win64 thunks |
| `arm64-Darwin` | `emit_arm64` → AArch64 bytes | MAP_JIT page + AAPCS64 thunks |
| **`wasm-interp`** | **`interpret_thin` (new)** — walks the IR | **C++ interpreter loop** |

This mirrors the advisor's ARM64 guidance ("ThinIR is the multi-target seam")
and is the same pattern many language WASM ports use (interpreter, not JIT).

## 1. Why this is MEDIUM-LARGE (not trivial)

- **New interpreter backend** (~1–2k lines + tests): a `switch` over the ~40
  `ThinOp`s that executes each in C++ over a frame byte-buffer. Script→script
  calls = recursive `interpret_thin`. Native calls = a host function table
  (WASM imports / a registered-natives table). Traps + try/catch/throw =
  longjmp/error-code discipline (NOT C++ exceptions — see §3). Bounds/budget/
  depth guards = interpreter-side checks. This is real work but bounded.
- **No C++ exceptions in WASI** (`-fno-exceptions`): ember's host-side compile
  errors + the trap-stub path use `throw`/`catch` + `setjmp`/`longjmp`. The
  interpreter must use a status-code / `longjmp` discipline instead of C++
  exceptions for traps. (The frontend's compile-error `throw`s would either need
  an error-code path OR the split in §4 — compile in host, run in WASM.)
- **Platform gating**: the native-JIT code (x64_emitter/arm64_emitter, emit_x64/
  emit_arm64, the asm thunks, jit_memory's mmap/VirtualAlloc, cpuid, Windows
  fibers, the W^X path) must compile OUT under a `EMBER_WASM_INTERP` target
  macro. ⚠️ **Per `WASM_AUDIT.md`, this gating is NEW (not a flip of the
  existing `__x86_64__`/`__aarch64__` pattern)** — `engine.cpp`'s `#else` arm
  currently services ARM64, not WASM, so the macro + the compile-out branches
  must be **authored** and are larger than a one-line flip. The
  interpreter replaces the emit step.
- **Build toolchain**: Emscripten (`em++`) or wasi-sdk (`clang --target=wasm32-
  wasi`). A CMake toolchain file + a `EMBER_WASM_INTERP` build flag. The ed25519
  thirdparty (C99) compiles to WASM. The frontend + ThinIR + interpreter +
  arch-neutral extensions compile; the native-only extensions get WASM stubs.
- **Deferred**: the self-hosted compiler (emits x86 via `call_raw`) does NOT run
  in WASM (same as ARM64 — `call_raw` can't execute native bytes; a self-hosted
  IR-emit/WASM target is future). Native-JIT-only features (keyed dispatch
  thunks, hot-reload of native pages, coroutine fibers) get interpreter
  equivalents or are deferred.

## 2. The interpreter design (`src/thin_interp.cpp` + `thin_interp.hpp`)

A function `InterpResult interpret_thin(const ThinFunction& thf, const InterpCtx& ctx)`:
- **Frame**: a byte buffer of `thf.frame.frame_size` bytes (stack-allocated or
  arena). Frame slots at the same `meta.frame_off` (rbp-negative) offsets — the
  interpreter indexes `frame[frame_size + off]` (the offsets are negative; the
  buffer base = the frame pointer). Params spilled into the buffer at entry.
- **VRegs**: a `vreg_value[]` map (int64 / double / {ptr,len} for slices) OR
  read/write directly to the frame buffer (mirror the emit's frame-slot model —
  simpler + matches the IR's frame_off conventions). Prefer the frame-buffer
  model: every VReg read = read its frame slot; every write = write its frame
  slot. (Exactly what emit_arm64 does, but in C++ instead of ARM64 instructions.)
- **Block loop**: `pc = block index + instr index`; execute instrs; on the
  terminator (Jmp/Branch/Return/Trap) set the next block / return / trap.
- **ThinOps**: ConstInt/Float/Bool/StringRef → write the constant to the dst
  frame slot. Move/Load/Store/LoadGlobal/StoreGlobal → frame/globals buffer
  read/write. Add/.../Cmp/Cast → read operands, compute, write dst. Calls →
  marshal args from frame slots, invoke (native fn ptr from `ctx.natives` by
  name, or script fn by dispatch slot → recursive `interpret_thin`), write
  result. BoundsCheck/DepthCheck/BudgetCheck → check + trap. TryCatch/Catch/
  Throw → a C++ `setjmp`/`longjmp` (or a status discipline) over an interpreter
  catch-stack (the catch_bufs analogue, but in interpreter state — no register
  save needed, just the catch block + call_depth snapshot). FieldAddr/IndexAddr
  → address arithmetic into a frame slot (the address is an i64). CopyBytes →
  memmove within the frame buffer. StructLitInit/ArrayLitInit/StoreAddr →
  frame-buffer stores at field offsets. StringDecrypt → call the host decrypt.
- **Native calls**: a native fn ptr from `ctx.natives` (the host registers
  natives by name, like the JIT path's `ctx.natives`). For a WASM-in-browser
  deployment, host natives can be WASM imports (the JS side provides print,
  math, etc.) OR C++ natives compiled into the WASM (the ember extensions).
- **Output**: the interpreter returns the i64/double/slice result + a trap
  status (reason + detail). No `CompiledFn` (no bytes/exec) — the interpreter IS
  the executor.

## 3. The no-exceptions constraint (WASI)

WASI compiles with `-fno-exceptions` (the wingolog demo: `-fno-exceptions`
because "WASI doesn't support exceptions currently"). ember uses C++ exceptions
for: (a) compile errors (sema/codegen throw `runtime_error`), (b) the trap stub
path (the host `EMBER_SETJMP`/`longjmp` checkpoint + the JIT throw-to-catch).
For the WASM interpreter:
- **Traps**: use `setjmp`/`longjmp` (WASI supports setjmp/longjmp even without
  C++ exceptions) OR a return-status discipline (`InterpResult.trapped`). The
  interpreter's trap = set `ctx.last_trap` + `longjmp` to the host checkpoint (a
  plain C `setjmp` buf, not C++ exceptions). try/catch within the interpreter =
  an interpreter-managed catch stack + `longjmp` to the catch block (no C++
  try/catch needed).
- **Compile errors**: if the WASM build compiles AND runs in one process, the
  frontend's `throw` needs an error-code path (or compile with
  `-fno-exceptions` + `-DEMBER_NO_EXCEPTIONS` that converts throws to a status
  return + abort). SIMPLEST split (§4): **compile in the host (native), ship the
  lowered ThinFunction (or a `.em` IR module) to the WASM runtime, interpret
  there** — so the WASM side only interprets (no compile, no compile-error
  throws). This also keeps the WASM module small (no lexer/parser/sema). This is
  the recommended deployment shape.

## 4. Two deployment shapes

**Shape A — interpret-only WASM (recommended first):** the native host (x64/arm64)
compiles `.ember` → ThinFunction (or serializes it to a v5 IR `.em`), ships the
IR to the WASM runtime, the WASM module `interpret_thin`s it. The WASM module
contains ONLY the interpreter + arch-neutral extensions + the IR deserializer —
no lexer/parser/sema, no JIT, no exceptions-for-compile-errors. Small, fast to
load, no `-fno-exceptions` compile-error problem. This is the clean first
milestone: **a `.em` (v5 IR) loaded + interpreted in WASM.**

**Shape B — full compiler-in-WASM:** the entire frontend (lexer/parser/sema/
lower) + interpreter compiles to WASM, so the WASM module compiles + runs `.ember`
source directly. Needs the `-fno-exceptions` compile-error path. Larger module.
Do this AFTER Shape A proves the interpreter.

## 5. Build (Emscripten or wasi-sdk)

- Install: `brew install emscripten` (provides `em++`) OR wasi-sdk
  (`clang --target=wasm32-wasi`). Emscripten is easier for browser/Node (has
  libc + a virtual FS); wasi-sdk for pure WASI.
- CMake: a toolchain file (`cmake/wasm.cmake`) setting `CMAKE_CXX_COMPILER=em++`
  + `-DEMBER_WASM_INTERP=1` + `-fno-exceptions` + the reactor exec model
  (`-mexec-model=reactor` for a multi-entry library) + exported memory/table as
  needed.
- Source selection: under `EMBER_WASM_INTERP`, the `ember`/`ember_frontend` libs
  include the interpreter + arch-neutral sources; the native emit (emit_x64/
  emit_arm64) + the asm thunks + jit_memory's native path + cpuid + fibers are
  compiled out (the `#if !defined(EMBER_WASM_INTERP)` gating). The compile
  pipeline's `compile_impl_` dispatches to `interpret_thin` (no emit step) under
  `EMBER_WASM_INTERP`.

## 6. Phased plan

1. **Phase W0 — interpreter skeleton + build**: `src/thin_interp.{hpp,cpp}` with
   the integer/control-flow subset (ConstInt, Move, Load/StoreFrame, Add/.../Cmp,
   Cast int, Branch/Jmp/Return/Trap, CallScript/CallNative scalar, guards). An
   Emscripten CMake toolchain + a `EMBER_WASM_INTERP` build that compiles the
   frontend + interpreter, gating out the native JIT. A standalone test: lower a
   fib `.ember` → `interpret_thin` → assert 6765 (run natively first via a
   `#ifndef EMBER_WASM_INTERP` test harness, then in WASM).
2. **Phase W1 — WASM build + run**: build the interpreter module with em++ (or
   wasi-sdk), run the integer subset test in Node/browser/WASI. Prove
   `interpret_thin` works in WASM.
3. **Phase W2 — full ThinOp coverage**: floats, slices, structs (the interpreter
   handles HFA/slices/structs trivially in C++ — no AAPCS64 register marshaling
   needed, just frame-buffer reads/writes — so this is EASIER than the native
   emit), strings, for-each, match, try/catch/throw (longjmp-based), bounds/
   budget/depth traps.
4. **Phase W3 — `.em` IR interpret-only deployment (Shape A)**: load a v5 IR
   `.em` in WASM (em_loader's deserialize path, which already exists + is
   arch-neutral) + interpret. The recommended deployment.
5. **Phase W4 — full compiler-in-WASM (Shape B)**: frontend compiles to WASM;
   the `-fno-exceptions` compile-error path. Larger; after Shape A is proven.
6. **Deferred**: self-hosted compiler in WASM (needs a self-hosted IR-emit/WASM
   target — the self-hosted codegen emits x86 today); native-JIT-only features
   (keyed thunks, hot-reload of native pages, coroutine fibers) — interpreter
   equivalents or defer.

## 7. Difficulty verdict

**MEDIUM-LARGE, very doable.** The interpreter is the main new work — ⚠️ **per
`WASM_AUDIT.md`, budget ~2.5–3.5k lines, not 1–2k**: the ~1–2k-line core
interpreter (a `switch` over the ~40 `ThinOp`s over a frame byte-buffer) is
realistic, but the gating + GC shadow-stack linkage (~100–150 lines, W2) + the
`.em` deserialize-only loader fork (~200–400 lines, W3) + extension stubs add
~500–1k lines this plan didn't budget (structs/slices are EASIER in an
interpreter than in native emit — no ABI marshaling). The build is moderate
(Emscripten + CMake + gating; **use Emscripten first** — wasi-sdk blocks on
`std::filesystem`). The no-exceptions constraint is the trickiest but solved by
the Shape A split (compile in host, interpret in WASM) OR a setjmp/longjmp/
status discipline (Emscripten `-fexceptions` is the lower-effort Shape-B path).
It is NOT a small task, but it is a clean, well-scoped third backend that
reuses ember's existing arch-neutral ThinIR (the ARM64 port de-risked the
hard part). **The ARM64 port is complete (56/56, 471/471), so begin with
Phase W0 (interpreter skeleton) and validate natively before the WASM build.**

## 8. What is NOT in scope (for the first WASM milestone)

- JIT-in-WASM via runtime module generation (the wasm-jit proposal late-linking
  path) — complex, runtime-dependent; the interpreter makes it unnecessary.
- The self-hosted compiler running in WASM (emits x86 via call_raw — needs a
  self-hosted WASM/IR-emit target, future).
- Native-JIT-only features (keyed dispatch native thunks, hot-reload of native
  pages, Windows-fiber coroutines) — interpreter equivalents or deferred.
- Intel macOS / Windows ARM64 / 32-bit (out of scope generally, per the ARM64 plan).
