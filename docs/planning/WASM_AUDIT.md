# Audit — WASM port plan (`docs/planning/plan_WASM.md`)

**Audited:** `docs/planning/plan_WASM.md` (the ThinIR-interpreter WASM target plan).
**Cross-referenced against:** `src/thin_ir.hpp`, `src/thin_emit.hpp`,
`src/thin_emit_arm64.cpp`, `src/thin_ir_ser.cpp`, `src/em_loader.cpp`,
`src/platform.cpp`, `src/jit_memory.cpp`, `src/engine.cpp`,
`src/context.hpp`, `src/codegen.cpp`, `extensions/coroutine/ext_coroutine.cpp`,
`extensions/io/ext_io.cpp`, `extensions/thread/ext_thread.cpp`,
`extensions/call_raw/ext_call_raw.cpp`, `extensions/gc/ext_gc.cpp`,
`extensions/obf/ext_obf.cpp`, `thirdparty/ed25519/ed25519_ember.hpp`,
`docs/planning/plan_MACOS_ARM64.md`, `docs/planning/MACOS_ARM64_PROGRESS.md`.
**macOS ARM64 baseline:** COMPLETE — lang_suite **471/471** (full language parity),
`emit_arm64` + AAPCS64 thunks working, `.em` v5 IR loader dispatching to
`emit_arm64` (MACOS_ARM64_PROGRESS.md Phase 8). This is the WASM port's
precondition.

---

## (a) Overall readiness verdict

**READY-WITH-FIXES.** The plan's core thesis is **sound and well-chosen**:
WASM has no user-allocated executable memory, so ember's native-JIT mechanism is
fundamentally inapplicable, and a **ThinIR interpreter** is the correct third
backend — it reuses the arch-neutral `ThinFunction` (the same seam the ARM64
port proved) without emitting machine code. The macOS ARM64 port has
**de-risked the hard part**: the ThinIR lowering + a second non-x86 backend
(`emit_arm64`) + the v5 IR re-emit dispatch + AAPCS64 ABI marshaling (HFA,
struct-by-value, slices) all work, so the interpreter is "emit_arm64 but in
C++" — strictly *easier* (no ABI marshaling, no register allocation, no
relocations). The plan correctly identifies the no-exceptions constraint, the
two deployment shapes (A: interpret-only; B: full-compiler-in-WASM), and the
major platform couplings.

However, the plan has **several gaps that must be fixed before W0 starts**:
(1) the `EMBER_WASM_INTERP` gating macro does **not yet exist** anywhere in the
tree — engine.cpp, platform.cpp, jit_memory.cpp, and the extensions gate on
`__x86_64__`/`__aarch64__`/`_WIN32`/`__APPLE__`, not on a WASM macro, so the
"compile out the native JIT" step is **not a flip of an existing pattern** but
**new gating** that must be authored and is larger than the plan implies (the
`#else` arm in engine.cpp currently services ARM64, not WASM); (2) the
interpreter must mirror emit_arm64's **frame-slot/VReg model precisely** (the
plan's "VReg map OR frame-buffer" hedge under-specifies the
struct-by-value/arg_frame_offs/HFA/GC-root cases — see Q1); (3) the plan
under-counts the **GC extension** coupling (the tracing collector walks the
JIT'd `gc_frame_head` chain via a registered trace callback — an interpreter
must synthesize its own root-scanning, and the plan doesn't mention it); (4)
the **`.em` v5 IR loader's re-emit path** is hardcoded to `emit_arm64`/`emit_x64`
(em_loader.cpp:1176-1181) and allocates executable pages — Shape A needs a
*deserialize-only* path that feeds the interpreter, not the re-emit path; (5)
`std::filesystem` is used in `ext_io.cpp` and `import.cpp` and is **not
available in wasi-sdk's wasi-libc by default** (it IS in Emscripten) — a
toolchain decision with consequences.

None of these are showstoppers; all are fixable within the plan's MEDIUM-LARGE
envelope (the ~1–2k-line interpreter estimate is realistic for the core, but
the gating + GC + loader-split + extension-stub work adds ~500–1k lines the
plan doesn't budget — see Q5). The plan is a good blueprint; this audit
sharpens it into a buildable spec.

---

## (b) Per-question findings + recommendations

### Q1 — Is a ThinIR interpreter the right approach? Is ThinIR sufficient? Which ThinOps are underspecified?

**Approach: correct.** The three alternatives:

1. **JIT-in-WASM via runtime module generation** (the wasm-jit proposal:
   generate a new WASM module at runtime, late-link via `call_indirect` + a
   growable function table + Wizer/instantiation). The plan correctly rejects
   this as "complex and runtime-dependent." I concur: it requires a WASM
   assembler/runtime in the module, depends on host instantiation support, and
   buys nothing over an interpreter for ember's use cases (see Q6 — ember is a
   scripting/embed language, not a perf-critical hot loop). **Do not pursue.**

2. **Compile the existing native JIT to WASM.** **Impossible.** The entire
   codegen (`x64_emitter.hpp`/`arm64_emitter.hpp`, `emit_x64`/`emit_arm64`,
   `engine.cpp` asm thunks, `jit_memory.cpp` mmap/VirtualAlloc, the W^X path)
   is built around emitting raw machine bytes into `mmap(PROT_EXEC)` pages and
   jumping to them. WASM is a Harvard architecture — code is not addressable at
   runtime; there is no `PROT_EXEC` memory. `call_raw` (extensions/call_raw,
   which casts an i64 to `int64_t(*)(int64_t)` and calls it) is the literal
   embodiment of the impossible mechanism. So the native JIT **cannot be the
   WASM backend**; only an interpreter (or the wasm-jit proposal) can. **The
   plan's choice is forced and correct.**

3. **ThinIR interpreter.** The natural seam: `ThinFunction` is already
   target-independent (thin_ir.hpp "STABLE enum… append only"), the lowering
   (`thin_lower.cpp`) produces it, and `emit_arm64` proves a second backend
   consumes it cleanly. An interpreter is `emit_arm64` with the ARM64 emitter
   swapped for C++ execution over a frame buffer — strictly simpler (no ABI
   marshaling, no register pressure, no relocations/fixups, no veneers).

**Is ThinIR sufficient? Mostly yes, with two caveats.** Inspecting
`thin_ir_ser.cpp` (the serializer that the `.em` v5 path uses) and
`thin_ir.hpp`, the serialized IR carries everything the interpreter needs:

- **Frame plan**: `frame.frame_size`, `frame.params` (name + type + offset +
  word0/nwords), `frame.gc_ptr_frame_offs`, `frame.gc_rec_*` offsets, and
  per-instr `meta.frame_off` (absolute rbp-negative). ✅ The interpreter can
  allocate `frame[frame_size]` and index `frame[frame_size + off]` exactly as
  emit_arm64 indexes `[x29 + off]`. The frame plan is **fully carried** — the
  plan's worry that the interpreter "needs the frame plan" is already
  satisfied; it's serialized.
- **Type info for struct element loads**: `meta.type`, `arg_types[]`,
  `ret_type`, `frame.params[].ty` are all serialized as **stable canonical
  type IDs** (thin_ir_ser.cpp:54-58, 146-160 — `parse_type` reconstructs `Type`
  objects into `thf.owned_types`). ✅ The interpreter can recover struct
  layouts/element widths from the reconstructed `Type` objects (same as
  emit_arm64 uses `ctx.structs`). **But** — see caveat below: for a
  *deserialized* ThinFunction, struct layouts come from the `.em` type codec,
  not the host `ctx.structs` registry; the interpreter must ensure the type
  store is populated (em_loader does this; a Shape-A-only interpreter reuses
  it).
- **Native bindings**: `meta.native_name` is the symbolic binding (serialized);
  `native_fn` (the raw ptr) is **NOT serialized** (thin_ir.hpp:252-254,
  thin_ir_ser drops it). ✅ The interpreter rebinds by name from a host native
  table — exactly the `.em` load-time rebind path (em_loader.cpp:1118-1123).
  This is the key property that makes Shape A work.

**Caveat 1 — struct element loads need the host type store, not just the IR.**
`FieldAddr` carries `meta.field_off` (the byte offset) and `meta.type`, so a
field *address* is computable from the IR alone. But a field **load/store** at
the correct width needs the element type's `value_bytes` (1/2/4/8) — carried as
`meta.width` on the load/store instr. ✅ Sufficient. The interpreter reads
`meta.width` and does a sized read/write (mirrors emit_arm64's
`frame_load64`/`load32`/`load_f32` dispatch). **No gap here** — the plan's
worry is addressed by `meta.width`, which is serialized.

**Caveat 2 — the `arg_frame_offs` struct-by-value convention.** A struct-by-
value call arg is encoded as `args[i]=0` (the VReg-0 sentinel) +
`arg_frame_offs[i]=<slot offset>` (thin_ir.hpp:256-262). The interpreter must
recognize this sentinel and copy bytes from that frame slot — **exactly what
emit_arm64 does** (thin_emit_arm64.cpp:1061-1077). The plan mentions
"StructLitInit/ArrayLitInit/StoreAddr → frame-buffer stores at field offsets"
but does **not** call out the `vreg==0 && arg_frame_offs[i]!=-1` sentinel. The
interpreter MUST handle it or struct-by-value calls silently pass garbage.
**Recommendation**: mirror emit_arm64's call-marshal path verbatim (the HFA
classification is NOT needed in an interpreter — structs are just byte buffers;
but the *arg source* (frame slot vs VReg) must be distinguished).

**ThinOps the plan under-specifies for interpretation:**

| ThinOp | Plan's coverage | Gap / what the interpreter must do |
|---|---|---|
| **CopyBytes** | "memmove within the frame buffer" | **Under-specified re: overlap + direction.** emit_arm64 (thin_emit_arm64.cpp:1698, 308-311) uses a *forward* chunked copy and comments "array temps are non-overlapping; mirrors emit_x64's copy_bytes." The IR's `CopyBytes` is for aggregate temps (StructLit/ArrayLit/copy-assign) which the lowerer guarantees non-overlapping. **Use `std::memmove`** (handles overlap safely) keyed off `meta.len` (byte count) + src/dst frame offsets. Do NOT assume a direction; `memmove` is correct and cheap. The plan's "memmove" is right; just confirm `meta.len` is the byte count (it is — thin_ir.hpp:201). |
| **StringDecrypt** | "call the host decrypt" | **Under-specified re: the two-slot layout.** emit_arm64 (thin_emit_arm64.cpp:1347-1355) uses **two** offsets: `meta.data_temp_off` (the decrypted-data temp buffer) and `meta.frame_off` (the slice result slot {ptr,len}). The interpreter must: decrypt rodata into `frame[data_temp_off]`, then write the slice {ptr=&frame[data_temp_off], len=meta.len} into `frame[frame_off]` + `frame[frame_off+8]`. The plan only mentions "call the host decrypt" — it misses the slice-result materialization. **Both offsets are serialized** (data_temp_off is the v2 field, thin_ir_ser.cpp:297). Mirror emit_arm64. |
| **TryCatch / CatchEntry / CatchCleanup / Throw** | "longjmp to the catch block (no C++ try/catch)" | **The longjmp model is different in an interpreter.** In the JIT, Throw does a custom register-save/restore longjmp into `catch_bufs[catch_depth]` (context.hpp:121-129, thin_emit_arm64.cpp:1993+). In an interpreter there are **no registers to save** — a "longjmp to catch" is just **setting the interpreter's `pc` to the catch block** + restoring `call_depth` from `catch_saved_call_depths`. This is **simpler than the JIT path** (no register save/restore, no SP manipulation) but the plan's "longjmp to the catch block" wording suggests reusing libc `longjmp`, which is **wrong for intra-interpreter catch** (you don't need it; just set pc). Reserve libc `setjmp`/`longjmp` for the **host-level trap checkpoint** (the outermost call from host→interpreter), mirroring context.hpp's `EMBER_SETJMP(ctx.checkpoint)`. **Intra-interpreter throw/catch = pc-restore + call_depth-restore**, not longjmp. The interpreter must maintain its own catch stack (catch block index + call_depth snapshot) in `context_t` or interpreter state. |
| **CallIndirect** | listed under "calls" but not detailed | **Fine.** In the interpreter, CallIndirect = load the fn handle (src1, a dispatch-slot index, validated by the preceding CallTargetGuard), look up the ThinFunction by slot from `ctx.functions`, recursive `interpret_thin`. emit_arm64 (thin_emit_arm64.cpp, Phase 8) does `ldr dispatch_base + lsl #3 + ldr entry + blr`; the interpreter just indexes a `ThinFunction*` table. **Simpler than JIT.** The handle provenance guard (CallTargetGuard) is a bounds+allowlist check on the i64 — trivial in C++. |
| **HFA struct calls** | "the interpreter handles HFA/slices/structs trivially in C++ — no AAPCS64 register marshaling needed" | **Correct, but the plan understates the work.** HFA passing/return in the JIT (emit_arm64 + aapcs64_classify) is the *hardest* part of the ARM64 port (Phase 6c, a whole classifier module). In the interpreter it IS trivial: a struct is a byte buffer in a frame slot; passing it by value = `memcpy` the slot bytes into the callee's frame; returning it = `memcpy` back (or via the `returns_struct_by_ptr` hidden-ptr convention, which the interpreter handles as "write the result into the caller-provided slot"). **But** the interpreter must still honor `ThinFramePlan::returns_struct_by_ptr` + `struct_ret_ptr_offset` (the hidden return-ptr slot) and the `arg_frame_offs` sentinel — the frame-plan conventions the lowerer emits. The plan should say: "structs/slices/HFA: byte-buffer copy keyed off frame offsets + the frame plan; no ABI classifier needed, but the frame-plan struct-return-ptr convention MUST be honored." |
| **LAnd / LOr** | not mentioned | Short-circuit logical. The lowerer may expand to Branch (thin_ir.hpp:139-141). If kept as a first-class op, the interpreter must **short-circuit** (eval src1; if false for LAnd / true for LOr, skip src2). The plan omits this; trivial but must be implemented (not a plain `src1 && src2` if src2 has side effects — though in ThinIR src2 is a VReg read, already evaluated; so a plain logical op IS correct here since ThinIR is three-address, not AST). **Likely just `bool(src1) && bool(src2)` — but verify the lowerer doesn't rely on short-circuit side-effect skipping.** (It can't — ThinIR has no side-effectful operand eval; VRegs are frame reads.) ✅ Trivial. |
| **DepthCheck / BudgetCheck / CallTargetGuard** | "interpreter-side checks" | ✅ Correct + simple: read `ctx.call_depth`/`ctx.budget_remaining`/the allowlist; if violated, set `ctx.last_trap` + longjmp to the host checkpoint (or set a trapped status). Mirror emit_arm64's compare-before-subtract (no wrap-around). |

**Verdict on Q1:** ThinIR is **sufficient** for the interpreter — the frame
plan, type info, native names, and per-instr metadata (width, field_off,
data_temp_off, arg_frame_offs) are all serialized and carry what emit_arm64
consumes. The interpreter must **mirror emit_arm64's frame-slot/VReg model
exactly** (the plan's "VReg map OR frame-buffer" hedge should collapse to
**frame-buffer only** — that's what the IR's frame_off conventions assume, and
it's simpler). The under-specified ops (CopyBytes direction, StringDecrypt's
two-slot layout, try/catch as pc-restore-not-longjmp, the arg_frame_offs
sentinel, the struct-ret-ptr convention) are all **documented in emit_arm64**
— the interpreter author should read `thin_emit_arm64.cpp` as the reference
implementation, not the plan's prose.

**Recommendation:** Add to the plan a sentence: *"The interpreter mirrors
`emit_arm64`'s frame-slot model verbatim (frame-buffer, not a VReg map);
`src/thin_emit_arm64.cpp` is the reference for CopyBytes/StringDecrypt/try-
catch/call-marshal/struct-ret-ptr semantics. Intra-interpreter throw/catch is
pc-restore + call_depth-restore (NOT libc longjmp); libc setjmp/longjmp is
reserved for the host-level trap checkpoint only."*

---

### Q2 — No-exceptions constraint: is the claim correct? Is setjmp/longjmp + status sound? Does the frontend's throw block Shape B? Is Shape A a real solution?

**The `-fno-exceptions` claim is correct.** WASI's wasi-libc is built with
`-fno-exceptions` and does not provide the C++ EH runtime; Emscripten's WASM
build similarly defaults to no-exceptions unless `-fexceptions` is explicitly
passed (and even then, EH-in-WASM is a separate feature/proposal, not
universally supported). Compiling ember with `-fno-exceptions` means **every
`throw`/`catch` in the codebase is a compile error** unless replaced.

**Is setjmp/longjmp + status-discipline sound? Yes — and the infrastructure
already exists.** `context.hpp` already defines `EMBER_SETJMP`/`EMBER_LONGJMP`
with a portable fallback (`setjmp`/`longjmp` on non-MinGW, which covers
Emscripten/wasi-sdk's clang). WASI libc **does** provide `setjmp`/`longjmp`
(they're C, not C++ EH). The host-level trap checkpoint
(`ctx.checkpoint` + `EMBER_SETJMP`, engine.cpp:552/765) is **already
exception-free** and works under `-fno-exceptions`. So:

- **Traps** (bounds/budget/depth/bad-call-target/unhandled-throw): the trap
  stub sets `ctx.last_trap` + `EMBER_LONGJMP(ctx.checkpoint)` — **already
  exception-free**, works in WASM. ✅
- **Intra-interpreter try/catch/throw**: as noted in Q1, this is **pc-restore +
  call_depth-restore** in the interpreter — no C++ exceptions, no libc longjmp
  needed (the JIT's custom-register longjmp is JIT-specific; the interpreter
  just sets its pc). ✅
- **Compile errors** (the real problem): see below.

**Does the frontend use C++ exceptions for compile errors in a way that blocks
Shape B? YES — extensively.** `sema.cpp` alone has ~62 `throw` statements
(`throw SemaError{...}`, `throw std::runtime_error(...)`); `parser.cpp`,
`lexer.cpp`, `thin_lower.cpp` throw similarly. These are **compile-time
diagnostics** (type errors, parse errors, recursion-depth limits). Under
`-fno-exceptions`, **every one of these is a compile error**. Shape B (full
compiler-in-WASM) therefore requires either:

1. **A `-DEMBER_NO_EXCEPTIONS` refactor** converting every `throw` to a
   status-return + early-out (a large, invasive change across
   lexer/parser/sema/lower — hundreds of throw sites, and the control flow
   assumes throw-unwinds-to-CompileError-catch at the driver level), OR
2. **Compiling with `-fexceptions`** (Emscripten supports it via
   `-fexceptions -s EXCEPTION_CATCHING_ALLOWED=[...]`; wasi-sdk with
   `-fwasm-exceptions`), accepting the larger module + the EH-in-WASM runtime
   dependency.

**Is Shape A (compile in host, interpret IR-`.em` in WASM) a real solution?
YES — and it's the clean one.** Shape A ships a **serialized ThinFunction**
(the v5 IR `.em`) to the WASM runtime, which **only deserializes +
interprets** — no lexer/parser/sema, no compile-error throws. The WASM module
contains: the interpreter + `thin_ir_ser`'s deserializer + arch-neutral
extensions. This:

- Eliminates the `-fno-exceptions` compile-error problem entirely (the
  frontend, the only thrower, stays on the host).
- Keeps the WASM module small (no ~15k-line frontend).
- Matches the existing `.em` v5 IR path (em_loader already deserializes +
  re-emits; Shape A replaces "re-emit to native" with "feed to interpreter").

**The plan's recommendation of Shape A first is correct.** Shape B is feasible
but should use **Emscripten `-fexceptions`** (the lower-effort path — no
frontend refactor) rather than a `-DEMBER_NO_EXCEPTIONS` status-discipline
conversion (high-effort, high-risk — the frontend's throw-based control flow
is pervasive). If Shape B must use wasi-sdk (no EH), the status-discipline
refactor is the only option and is a **separate, large sub-project** the plan
should call out explicitly (it's not in the ~1–2k-line estimate).

**Recommendation:** The plan's §3/§4 are sound. Add: *"Shape B on wasi-sdk
requires a `-DEMBER_NO_EXCEPTIONS` refactor of the frontend's ~62+ throw sites
(sema/parser/lexer/lower) to status-returns — a large sub-project NOT in the
interpreter line estimate. Shape B on Emscripten can use `-fexceptions`
instead (lower effort, larger module). Shape A avoids both."*

---

### Q3 — Platform couplings the plan may have missed

The plan lists threads, coroutines, file I/O, ed25519, call_raw, @obf_keyed,
self-hosted. Audit against the actual tree:

| Coupling | Plan says | Reality (from the tree) | Verdict |
|---|---|---|---|
| **Threads (`ext_thread`)** | "WASM has no native threads" | `ext_thread.cpp` uses `std::thread` (lines 348/352) + a shared `context_t` + `call_mutex` for in-context threads. **No core feature needs threads** — grep of `src/` for `std::thread`/`pthread` returns nothing outside extensions/tests. | ✅ Plan correct. **Stub `ext_thread` in WASM** (register-natives that return 0 / a typed unsupported-mode failure, mirroring the keyed fail-closed pattern). No core dependency. WASM threads (shared memory + atomics) are opt-in and not needed for v1. |
| **Coroutines (`ext_coroutine`)** | "coroutine fibers — interpreter equivalents or deferred" | Windows fibers (`CreateFiber`/`SwitchToFiber`) + Darwin ARM64 asm context-switch (`ember_ctx_switch` in `darwin_arm64_ctx_switch.S`, mmap'd private stacks). **Both are impossible in WASM** (no fiber API, no asm context switch, no mmap'd stack you can `ret` into). BUT — **an interpreter CAN do cooperative coroutines**: a coroutine = a paused interpreter frame (save the `ThinFunction` + pc + frame buffer + call_depth); `yield` = save + return to the resumer; `resume` = re-enter `interpret_thin` at the saved pc. This is the **standard interpreter coroutine pattern** (generators in Python/JS VMs). | ⚠️ Plan under-sells this. "Interpreter equivalents" is **possible and not hard** (~100-200 lines: a coroutine = {ThinFunction*, pc, frame buffer copy, call_depth}), but it's a **new design** not in the ~1–2k estimate. **Recommendation**: defer coroutines from the first WASM milestone (stub them, as the plan says), but note that an interpreter-native cooperative-coroutine design exists for a later phase — do NOT attempt the fiber/asm-switch path. |
| **File I/O (`ext_io`)** | "WASI virtual FS" | `ext_io.cpp` uses `std::FILE*` (fopen/fread/fwrite) + `std::filesystem` (is_regular_file/exists) + stdout/stdin. **WASI provides a virtual FS** (path_open/read/write via WASI imports) but **`std::filesystem` is NOT in wasi-libc by default** (it's in Emscripten's libc). `std::FILE*` works under both (Emscripten libc + wasi-libc support stdio backed by WASI fd). | ⚠️ **`std::filesystem` is a wasi-sdk blocker** for `ext_io.cpp` + `import.cpp`. **Two fixes**: (a) use Emscripten (has `std::filesystem`), or (b) under wasi-sdk, gate `std::filesystem` calls behind `#ifndef EMBER_WASM_INTERP` and use raw `fopen`/`stat` (WASI libc has `fopen` via fd). The plan should call this out. |
| **ed25519 (`.em` signature verify)** | "compiles to WASM" | `thirdparty/ed25519` is pure C99 (orlp/ed25519, public domain); `ED25519_NO_SEED` excludes the wincrypt-pulling seed.c. **Confirmed portable.** Compiles to WASM under both Emscripten and wasi-sdk (C99, no OS deps, no浮点 weirdness — uses SHA-512 internally). | ✅ Plan correct. No issue. Signature *verification* (the loader path) runs in WASM; *signing* (the writer path) stays on the host. |
| **`call_raw`** | "executes native bytes — impossible in WASM" | `ext_call_raw.cpp`: `n_call_raw` does `reinterpret_cast<int64_t(*)(int64_t)>(fn_ptr)(arg)`; `n_make_executable` does `alloc_executable` (W^X page). **Both impossible in WASM** (no executable memory, no function-pointer-to-native-bytes). | ✅ Plan correct. **Stub the whole `ext_call_raw` extension** in WASM (register-natives that return INT64_MIN/0). This also means the **self-hosted compiler cannot run its output** in WASM — see below. |
| **`@obf_keyed` (cpuid)** | "cpuid — N/A" | `codegen.cpp:43-48` `current_cpuid_signature()` is **already gated** to `__x86_64__`/`__i386__`/`_M_X64` (returns 0 on non-x86); `@obf_keyed` is diagnosed unsupported on arm64 (MACOS_ARM64_PROGRESS Phase 0). The obf extension (`ext_obf.cpp`) has MBA/opaque-pred/string-encrypt passes that are **IR-level** (operate on ThinFunction) — those **do work** in an interpreter context (they transform the IR before interpretation). | ✅ Plan correct. **`@obf_keyed` is N/A** (no CPUID; route through host key-provider, as ARM64 does). The other obf passes (MBA/opaque/str-encrypt) are arch-neutral IR passes and **work in WASM** (they run at compile time on the host in Shape A; in Shape B they run in-WASM on the IR before interpretation). StringDecrypt at runtime calls the host decrypt native — fine. |
| **Self-hosted compiler** | "emits x86 via call_raw — can't run in WASM" | `self_hosted/codegen.ember` emits **raw x86-64 bytes** (cg_rex/cg_byte/cg_mov_reg_imm64 — lines 174-287) and `full_pipeline.ember` uses `make_executable`+`call_raw` to execute them. **Confirmed: the self-hosted codegen is x86-only by design** (Stage 4 of plan_SELF_HOSTING). | ✅ Plan correct. The self-hosted compiler **cannot run in WASM** (its codegen emits x86, and call_raw can't execute it). A self-hosted **WASM/IR-emit** target is a future effort (the plan defers it correctly). Note: the self-hosted *lexer/parser/sema* (layers 1-3) are arch-neutral ember code and **could** run in WASM via the interpreter (Shape A interpreting emberc.ember's front layers) — but the codegen layer is x86-locked. |
| **GC extension (tracing GC)** | **NOT MENTIONED in the plan** | `ext_gc.cpp` registers a trace callback (`context_roots_trace_cb`, line 256) that walks `ctx->gc_frame_head` (the JIT'd shadow-stack frame chain) + `ctx->gc_global_roots`. The JIT'd prologue links a `GcFrameRecord` (frame_base + map of GC-ptr offsets) onto `gc_frame_head`; the epilogue unlinks it. **An interpreter has no JIT'd prologue/epilogue** — it must **synthesize the GcFrameRecord linkage itself**: on entry to a function with `frame.gc_ptr_frame_offs` non-empty, link a record (frame_base = the interpreter's frame buffer, map = the gc_ptr_frame_offs); on exit, unlink. | ❌ **Plan GAP.** This is real work (~100-150 lines) and **not in the ~1–2k estimate**. Without it, `gc_collect()` during an interpreted lambda-using script would miss stack roots → free reachable envs → use-after-free. **Recommendation**: the interpreter must manage `gc_frame_head` exactly as the JIT prologue/epilogue does, using `thf.frame.gc_ptr_frame_offs` (serialized) as the root map. Add this to the plan's interpreter design + the W2/W3 scope. |

**Couplings the plan correctly identifies + I confirm:** threads (stub),
coroutines (stub/defer; interpreter-native possible later), call_raw (stub),
@obf_keyed (N/A), self-hosted (defer).

**Couplings the plan MISSES:** (1) the **GC extension's shadow-stack
linkage** (above); (2) the **`.em` v5 loader's re-emit path is hardcoded to
emit_arm64/emit_x64 + alloc_executable** (em_loader.cpp:1176-1181, 1039+) —
Shape A needs a **deserialize-only path** that feeds the interpreter without
allocating executable pages (see Q4/build + the corrections); (3)
`std::filesystem` in ext_io/import (wasi-sdk blocker — above).

---

### Q4 — Build: Emscripten vs wasi-sdk? CMake toolchain sound? C++17 support? Does `EMBER_WASM_INTERP` gating cover ALL native-JIT code?

**Emscripten vs wasi-sdk:**

| | Emscripten (`em++`) | wasi-sdk (`clang --target=wasm32-wasi`) |
|---|---|---|
| **libc** | full libc (incl. `std::filesystem`, stdio, pthreads opt-in) | wasi-libc (no `std::filesystem` by default; stdio via WASI fd; no threads by default) |
| **C++ EH** | `-fexceptions` supported (Emscripten JS glue) | `-fwasm-exceptions` (newer; not universal) |
| **Browser/Node** | first-class (virtual FS, canvas, etc.) | pure WASI (needs a WASI runtime: wasmtime/wasmer) |
| **`setjmp`/`longjmp`** | ✅ | ✅ |
| **ed25519 (C99)** | ✅ | ✅ |

**Recommendation: Emscripten first.** Reasons: (1) `std::filesystem` works
(unblocks ext_io/import without gating); (2) `-fexceptions` is available if
Shape B is pursued (avoids the frontend refactor); (3) the virtual FS +
browser/Node target matches ember's embed/scripting use case; (4) easier
CMake integration (`emcmake cmake`). wasi-sdk is viable for Shape A
(interpret-only, no `std::filesystem` needed if ext_io is stubbed/gated) but
requires the `std::filesystem` gating fix and gives up the easy Shape-B EH
path. **The plan's "Emscripten OR wasi-sdk" is fine; pick Emscripten for W0.**

**CMake toolchain: sound.** A `cmake/wasm.cmake` toolchain file setting
`CMAKE_CXX_COMPILER=em++` + `-DEMBER_WASM_INTERP=1` + `-fno-exceptions` (Shape
A) + the reactor exec model (`-s EXPORTED_RUNTIME_METHODS=...,
EXPORT_NAME=...` + `-mexec-model=reactor` for a multi-entry library) is the
standard Emscripten-CMake pattern. The plan's approach is correct. One note:
`enable_language(ASM)` in CMakeLists.txt (line 22, for the Darwin ARM64
thunks) is harmless under Emscripten (no `.S` files are selected for the WASM
build), but the toolchain file should ensure the ARM64/x86 asm sources are
**not** in the WASM source list (see gating below).

**C++17 features ember uses — Emscripten/wasi-sdk support:**

- `std::optional` (8+ files), `std::string_view` (8 files), `std::variant`?,
  `if constexpr`, structured bindings, `std::shared_ptr`/`std::unique_ptr`
  (pervasive). **All supported** by Emscripten (clang-based, full C++17) and
  wasi-sdk (clang-based, C++17; `std::optional`/`string_view` in libc++).
- `std::filesystem` (ext_io.cpp, import.cpp): **Emscripten ✅, wasi-sdk ❌
  (by default)**. See Q3.
- `std::thread` (ext_thread): Emscripten needs `-pthread` + SharedArrayBuffer
  (opt-in); wasi-sdk needs `-pthread` + a threads-enabled wasi-libc. **Stub
  the extension instead** (no core need).
- `std::mutex` (context.hpp `call_mutex`, ext_coroutine/thread): available in
  both (libc++); under `-pthread` for actual locking. With ext_thread/coroutine
  stubbed, `call_mutex` is uncontended and compiles fine (std::mutex is
  available without -pthread in Emscripten's default build; it's a no-op-ish
  stub or a real one — verify, but it links).

**No C++17 feature ember uses is unsupported** by Emscripten; wasi-sdk only
loses `std::filesystem` (fixable by gating). ✅

**Does `EMBER_WASM_INTERP` gating cover ALL native-JIT code? NO — the macro
does not exist yet, and the existing gating is arch-keyed, not WASM-keyed.**
Auditing the native-JIT code paths:

| File | Current gating | WASM action needed |
|---|---|---|
| `engine.cpp` (asm thunks) | `#if defined(__GNUC__) && defined(__x86_64__)` (x86 asm) → `#else` (ARM64 `.S` thunks under `__APPLE__&&__aarch64__`, else throw-stubs) | **Add a WASM branch**: under `EMBER_WASM_INTERP`, `ember_call_*` don't exist (no JIT'd entry to call) — the interpreter is called directly. The whole `ember_call_*`/keyed-thunk surface is **N/A** in WASM (the host calls `interpret_thin`, not a thunk). Gate the entire thunk block out. |
| `platform.cpp` (mmap/VirtualAlloc/MAP_JIT) | `#if _WIN32` / `#elif __APPLE__` / `#elif __linux__` / `#else #error` | **Add `#elif defined(EMBER_WASM_INTERP)`**: `alloc_rw`/`protect_rx`/`free_page` are **unused** in WASM (no executable pages); provide no-op stubs or compile out. The `#else #error` would **break the WASM build** — must be gated. `executable_path()` → return empty or a host-provided path. |
| `jit_memory.cpp` (alloc_executable etc.) | none (delegates to platform) | If platform stubs are no-ops, jit_memory compiles but is **never called** (the interpreter doesn't allocate executable pages). OK. |
| `codegen.cpp` (tree-walker, x86-only) | `#if __x86_64__`/`_M_X64` for cpuid; the tree-walker itself is x86-only but **not gated** (MACOS_ARM64_PROGRESS forces IR on ARM64 instead of gating) | Under `EMBER_WASM_INTERP`, the tree-walker + `emit_x64` are **not called** (compile_impl_ dispatches to `interpret_thin`). They may still **compile** (they're C++ that emits bytes into a vector — no OS calls), but linking is cleaner if gated out. **Recommendation**: gate `emit_x64`/tree-walker out under `EMBER_WASM_INTERP` (they reference `X64Emitter` which is fine C++, but unused → smaller module). |
| `thin_emit_arm64.cpp` / `arm64_emitter.hpp` | `__aarch64__`/`_M_ARM64` | **Compile out under `EMBER_WASM_INTERP`** (not an ARM64 build). Already arch-gated, so just ensure the WASM build doesn't define `__aarch64__`. ✅ |
| `darwin_arm64_thunks.S` / `darwin_arm64_ctx_switch.S` | Apple ARM64 only (CMake Apple-only target) | **Not in the WASM source list** (CMake selects them only on Apple ARM64). ✅ |
| `extensions/coroutine` | `_WIN32` fibers / `__APPLE__` asm switch / else stub (`ext_coroutine_stub.cpp`) | **Use the stub** (the `else` path already exists). Add `EMBER_WASM_INTERP` to the `else` condition, OR a WASM-specific stub. ✅ (stub already exists) |
| `extensions/thread` | none (uses `std::thread`) | **Gate out / stub under `EMBER_WASM_INTERP`** (no `std::thread` without -pthread; no core need). New stub needed. |
| `extensions/call_raw` | none (uses `alloc_executable` + fn-ptr cast) | **Gate out / stub under `EMBER_WASM_INTERP`** (impossible in WASM). New stub needed. |
| `extensions/io` | none (stdio + `std::filesystem`) | **Keep under Emscripten** (works); **gate `std::filesystem` under wasi-sdk** or stub. |
| `extensions/gc` | none (trace callback walks `gc_frame_head`) | **Keep** — the interpreter synthesizes `gc_frame_head` linkage (Q3). No gating; the GC heap + collector are pure C++. |

**Verdict on Q4:** The `EMBER_WASM_INTERP` gating pattern is the **right
model** (mirrors the ARM64 port's `__aarch64__` gating), but **it does not
exist yet and is larger than the plan implies** — the plan says "the
`EMBER_WASM_INTERP` gating pattern cover[s] ALL the native-JIT code that must
compile out" as if it's a flip; in reality it's **new `#elif`/`#if` branches
in engine.cpp, platform.cpp, + new stubs for thread/call_raw/coroutine**.
The ARM64 port's stubs (engine.cpp `#else` arm) are a **template** but service
ARM64, not WASM — WASM needs its own branch (the interpreter is called
directly, not via a thunk). **Recommendation**: budget ~200-400 lines of
gating + stubs (not in the ~1–2k interpreter estimate); model the stubs on
the ARM64 `arm64_exec_unimplemented` stubs + the coroutine `ext_coroutine_stub.cpp`.

---

### Q5 — Phasing (W0–W4): order right? W0 correct? Hidden dependencies? Is ~1–2k lines realistic?

**Phasing order: mostly right, with two adjustments.**

- **W0 (interpreter skeleton + build, validate natively): ✅ correct.**
  Building + validating the interpreter **natively first** (a `#ifndef
  EMBER_WASM_INTERP` test harness lowering fib.ember → `interpret_thin` →
  assert 6765) is exactly the right de-risking step — it separates
  interpreter-correctness from WASM-build-correctness. **Hidden dependency**:
  W0 needs the `EMBER_WASM_INTERP` gating to at least partially exist (to
  compile the interpreter without the native JIT), OR the native-validation
  harness links the interpreter alongside the native JIT (both can coexist —
  the interpreter is just another consumer of ThinFunction). **Recommend the
  latter for W0**: build the interpreter natively *alongside* the JIT (no
  gating needed yet), validate it, THEN add the gating + Emscripten build in
  W1. This avoids coupling W0 to the gating work.

- **W1 (WASM build + run): ✅ correct** — but this is where the gating +
  toolchain work lands (Q4). **Hidden dependency**: W1 needs the
  `EMBER_WASM_INTERP` gating in engine.cpp/platform.cpp + the thread/call_raw
  stubs + the `std::filesystem` decision. Budget this in W1, not W0.

- **W2 (full ThinOp coverage): ✅ correct** — but **add the GC shadow-stack
  linkage** here (Q3 gap): the interpreter must link/unlink `GcFrameRecord`s
  for functions with `gc_ptr_frame_offs` before `gc_collect` can be safe.
  Floats/slices/structs are indeed **easier** in the interpreter (no ABI
  marshaling) — the plan is right. **Hidden dependency**: try/catch must be
  **pc-restore + call_depth-restore**, not libc longjmp (Q1) — the plan's
  "longjmp-based" wording is misleading; clarify in W2.

- **W3 (`.em` IR interpret-only deployment, Shape A): ✅ correct as a
  milestone** — but **the biggest hidden dependency is here**: the v5 IR
  loader's re-emit path (em_loader.cpp:1039-1188) is hardcoded to
  `emit_arm64`/`emit_x64` + `alloc_executable_rw`. Shape A needs a
  **deserialize-only path**: `deserialize_thin_function` →
  `validate_thin_function` → **feed to `interpret_thin`** (no re-emit, no
  executable page). This is a **fork in the loader**, not a config flag — the
  current loader's contract is "re-emit to native + publish a dispatch table
  of native entry ptrs"; the interpreter path's contract is "publish a
  dispatch table of `ThinFunction*` (or interpreter slots)." **Recommendation**:
  in W3, add a `load_em_bytes_interp` path (or an `EMBER_WASM_INTERP` branch in
  `load_em_bytes_impl`) that skips the re-emit loop + builds a `ThinFunction`
  dispatch table instead of a native-ptr dispatch table. **~200-400 lines, not
  in the estimate.**

- **W4 (full compiler-in-WASM, Shape B): ✅ correctly last.** Gated on the
  `-fno-exceptions` decision (Q2). If Emscripten `-fexceptions` is used, W4 is
  "compile the frontend to WASM + wire it to the interpreter" (moderate); if
  wasi-sdk, W4 requires the frontend status-discipline refactor (large,
  separate sub-project). **Recommendation**: do W4 on Emscripten with
  `-fexceptions`; defer wasi-sdk Shape B indefinitely (or budget the refactor
  explicitly).

**Is the ~1–2k-line estimate realistic? For the interpreter core, yes; for
the total WASM port, NO — it under-counts ~4 areas:**

| Work item | Lines (est.) | In plan's 1–2k? |
|---|---|---|
| Interpreter core (`thin_interp.cpp`, ~40 ThinOps, frame-buffer model, call dispatch, guards) | ~1.5–2k | ✅ yes |
| `EMBER_WASM_INTERP` gating (engine.cpp, platform.cpp branches) + thread/call_raw/coroutine stubs | ~300–500 | ❌ no |
| GC shadow-stack linkage in the interpreter (link/unlink GcFrameRecord, gc_ptr_frame_offs map) | ~100–150 | ❌ no |
| `.em` v5 loader interp path (deserialize-only, ThinFunction dispatch table) | ~200–400 | ❌ no |
| Emscripten CMake toolchain + build debugging | ~100–200 | partial |
| **Total** | **~2.2–3.3k** | plan says 1–2k |

**Recommendation:** revise the estimate to **~2.5–3.5k lines** (MEDIUM-LARGE,
upper end). The interpreter core is bounded (the plan is right about that);
the gating + GC + loader-split + stubs are the under-counted work. Still very
doable; still well-scoped; just not 1–2k.

---

### Q6 — Risks/gaps the plan doesn't mention

1. **Performance (interpreter ~10–100x slower than JIT).** The plan doesn't
   mention perf at all. An IR interpreter dispatching ~40 ThinOps via a
   `switch` over a frame buffer is **~10–50x slower** than native JIT for
   compute-heavy loops (each ThinOp = a switch case + frame read/write +
   bounds-free arithmetic; vs. 1-3 native instructions). **Is this
   acceptable?** For ember's use cases (scripting, embedding, config DSL,
   VST3 audio parameter logic, game scripting, obfuscated dispatch) — **yes,
   mostly**: ember scripts are typically short, event-driven, or
   control-flow-heavy, not tight numeric kernels. The VST3 audio extension
   (realtime audio) is the **one case where 10–50x is unacceptable** — but
   realtime ember functions are `@realtime`-gated and today run JIT'd; in WASM
   they'd need either the wasm-jit proposal (deferred) or acceptance that
   realtime audio in WASM-ember is interpret-speed. **Recommendation**: the
   plan should state the perf expectation explicitly (~10–50x slower) + name
   realtime audio as the known casualty (defer `@realtime` in WASM v1, or
   document the perf cliff). A threaded-interpreter / direct-threaded-dispatch
   optimization (computed-goto switch, ~2-3x faster) is a **future option** if
   perf matters — note it but don't build it in W0.

2. **Hot-reload of native pages — meaningless in WASM.** The plan mentions
   "hot-reload of native pages" as deferred. Correct — WASM has no native
   pages to hot-reload. **But** there's an adjacent feature: ember's
   **module hot-reload** (HotReloadDomain, engine.cpp:451+, the
   `ExecutionGuard` generation-guard) is about **swapping a module's dispatch
   table at runtime** (re-compile + repoint entries), NOT native-page patching.
   In an interpreter, hot-reload = **swap the `ThinFunction*` dispatch table**
   (re-compile on the host in Shape A → re-serialize → re-load → repoint).
   This is **possible + meaningful** in WASM (no native pages needed — just
   swap IR function ptrs). **Gap**: the plan lumps "hot-reload" into
   "native-JIT-only features" and defers it; but **IR-level hot-reload is
   feasible** in the interpreter and may be desirable. **Recommendation**:
   distinguish "native-page hot-reload" (N/A) from "module-dispatch hot-reload"
   (feasible in WASM via IR-table swap); defer the latter to a post-W3 phase
   but don't mark it impossible.

3. **The `.em` v5 IR loader works without native re-emit? NO — today it
   re-emits.** The plan's Shape-A description ("em_loader's deserialize path,
   which already exists + is arch-neutral") is **half-right**: the
   *deserializer* (`deserialize_thin_function`) is arch-neutral, but the
   *loader driver* (`load_em_bytes_impl`) **re-emits to native +
   alloc_executable** (em_loader.cpp:1039-1188). Shape A needs the
   **deserialize-only fork** (Q5). The plan's wording suggests the loader
   already supports interpret-only; it does not. **This is the most
   consequential gap** — it's the difference between "wire up an existing
   path" and "fork the loader." See Corrections.

4. **Determinism.** The plan doesn't mention determinism. The JIT's
   `alloc_executable` returns a **runtime address** (ASLR'd, non-deterministic)
   baked into abs_fixups; the `.em` re-emit resolves these at load time. An
   interpreter has **no runtime addresses** (no native entries — the
   "dispatch table" is `ThinFunction*` pointers, which are still
   runtime-address-dependent but **not serialized**). **Good news**: the
   interpreter is **more deterministic** than the JIT — the IR is fully
   serialized (stable ThinOp IDs, stable type IDs, stable frame offsets), and
   interpretation is a pure function of (IR + input). The only
   non-determinism is `ThinFunction*` pointer values in the dispatch table
   (process-internal, not observable). **No action needed**, but the plan
   should note that WASM-ember is **deterministic by construction** (a
   reproducibility win over the JIT).

5. **The `non_serializable` flag + obf fallback.** `ThinFunction` has a
   `non_serializable` flag (thin_ir.hpp:313) — obf functions fall back to the
   tree-walker (x86-only). In WASM, the tree-walker doesn't exist → an obf
   function marked `non_serializable` **can't be interpreted** (the fallback
   is x86-only). The plan doesn't address this. **Recommendation**: in Shape A,
   the host compiler must **lower obf functions to ThinIR** (not fall back to
   the tree-walker) before serializing — i.e., the `non_serializable` flag
   must be false for all functions in a Shape-A `.em`. The MBA/opaque/str-
   encrypt obf passes are **IR passes** (ext_obf.cpp) and produce serializable
   ThinIR; only `@obf_keyed` (cpuid) is non-serializable (and it's N/A in
   WASM). So **Shape-A `.em` modules must not contain `@obf_keyed` functions**
   — diagnose at serialize time. Add to the plan.

6. **Tail calls (`is_tail_call`).** ThinInstr has `is_tail_call` (thin_ir.hpp
   266-289), a JIT-time hint for emit_x64's tail emission. It's **NOT
   serialized** (thin_ir_ser never reads/writes it). In an interpreter, a
   tail-call-annotated CallScript is just a **normal recursive
   `interpret_thin`** — the interpreter doesn't need the annotation (no stack
   growth optimization unless the interpreter does explicit tail-call
   trampolining). **Recommendation**: ignore `is_tail_call` in the interpreter
   (it's a JIT-only hint); BUT — deep recursion (fib(40)+) may **overflow the
   C++ call stack** via recursive `interpret_thin`. The interpreter should
   either (a) rely on `DepthCheck` to bound recursion before C++ stack
   overflow (the existing safety mechanism — ✅), or (b) implement an
   explicit tail-call trampoline for marked calls (future optimization).
   **Default to (a)**; document the C++-stack-depth limit.

---

## (c) Ranked risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | **The `.em` v5 loader re-emit path is hardcoded to native emit; Shape A needs a deserialize-only fork the plan doesn't budget.** | High (it's a fact — em_loader.cpp:1176-1181) | High (blocks Shape A, the recommended first milestone) | Fork `load_em_bytes_impl` under `EMBER_WASM_INTERP`: skip re-emit + `alloc_executable`; build a `ThinFunction*` dispatch table; feed interpreter. Budget ~200-400 lines in W3. |
| **R2** | **GC shadow-stack linkage not handled in the interpreter → `gc_collect` frees reachable lambda envs → use-after-free.** | Medium (only affects lambda + GC-using scripts; lang_suite has GC tests) | High (silent memory corruption) | Interpreter links/unlinks `GcFrameRecord` on function entry/exit using `thf.frame.gc_ptr_frame_offs` (serialized). Budget ~100-150 lines in W2. Test with gc_core / gc_full lang_suite tests. |
| **R3** | **`EMBER_WASM_INTERP` gating doesn't exist; the `#else #error` in platform.cpp breaks the WASM build; engine.cpp's `#else` services ARM64 not WASM.** | High (it's a fact — no WASM gating in tree) | Medium (build-blocking but mechanical) | Author `EMBER_WASM_INTERP` branches in platform.cpp (before the `#else #error`), engine.cpp (skip thunks — interpreter is called directly), + stubs for thread/call_raw. Model on the ARM64 stubs. Budget ~300-500 lines in W1. |
| **R4** | **`std::filesystem` in ext_io.cpp + import.cpp breaks the wasi-sdk build.** | High under wasi-sdk; None under Emscripten | Medium (build-blocking under wasi-sdk) | Use Emscripten for W0/W1 (has `std::filesystem`). If wasi-sdk is needed later, gate `std::filesystem` behind `#ifndef EMBER_WASM_INTERP` + use raw `fopen`/`stat`. |
| **R5** | **Try/catch implemented as libc `longjmp` (per the plan's wording) instead of pc-restore → wrong semantics / UB across interpreter frames.** | Medium (depends on implementation reading) | High (correctness) | Clarify: intra-interpreter throw/catch = **set interpreter pc to catch block + restore call_depth from `catch_saved_call_depths`**; libc `setjmp`/`longjmp` is ONLY for the host-level trap checkpoint. Mirror the JIT's catch-stack in interpreter state (no register save needed). |
| **R6** | **Performance (~10–50x slower than JIT) makes `@realtime`/audio ember unusable in WASM.** | High (interpreter perf is inherent) | Medium (limits the use-case surface) | Document the perf cliff; defer `@realtime` in WASM v1; note direct-threaded-dispatch as a future ~2-3x optimization. Acceptable for non-realtime scripting/embedding. |
| **R7** | **Recursive `interpret_thin` overflows the C++ stack on deep recursion (fib(40)+, or malicious input).** | Medium | Medium (crash, but `DepthCheck` mitigates) | Rely on `DepthCheck` (the existing safety guard) to bound script recursion before C++ stack overflow; tune `max_call_depth` for the interpreter's larger per-frame C++ stack usage. Optional: explicit tail-call trampoline for `is_tail_call`-marked calls (future). |
| **R8** | **`non_serializable` obf functions (tree-walker fallback) can't be interpreted in WASM.** | Low (only `@obf_keyed`, which is N/A in WASM) | Low (diagnose at serialize time) | Shape-A `.em` modules must not contain `non_serializable` functions; the host compiler lowers obf (MBA/opaque/str-encrypt — all IR passes) to ThinIR before serializing. Diagnose `@obf_keyed` at serialize time as unsupported-in-WASM. |
| **R9** | **Shape B on wasi-sdk requires a ~62+-site frontend `throw`→status refactor (not budgeted, high-risk).** | Medium (only if Shape B + wasi-sdk is pursued) | High (large invasive refactor) | Do Shape B on Emscripten with `-fexceptions` (avoid the refactor). Only attempt the wasi-sdk Shape B refactor if there's a hard requirement for pure-WASI full-compiler-in-WASM; budget it as a separate sub-project. |
| **R10** | **Emscripten build quirks (ASM selection, ed25519 C linkage, reactor vs command model, exported functions) eat W1 time.** | Medium | Low-Medium (toolchain friction) | Use `emcmake cmake`; ensure the CMake toolchain excludes `.S` files + Windows-only targets; test ed25519 C linkage early in W1; use `-mexec-model=reactor` for a library (not a command). |

---

## (d) Corrections to the plan

1. **MISSED COUPLING — the GC extension.** The plan does not mention the GC
   extension's shadow-stack (`gc_frame_head`) linkage. **Fix**: add to §2
   (interpreter design): *"On entry to a function with non-empty
   `thf.frame.gc_ptr_frame_offs`, the interpreter links a `GcFrameRecord`
   {frame_base = the interpreter frame buffer, map = gc_ptr_frame_offs} onto
   `ctx.gc_frame_head`; on exit, unlinks. This mirrors the JIT prologue/
   epilogue's GcFrameRecord linkage (ext_gc.cpp's `context_roots_trace_cb`
   walks this chain). Without it, `gc_collect` during an interpreted lambda-
   using script misses stack roots + frees reachable envs."* Add to W2 scope.

2. **MISSED COUPLING — the `.em` v5 loader re-emit fork.** The plan (§4 Shape
   A) says "em_loader's deserialize path, which already exists + is
   arch-neutral." **This is inaccurate**: the deserializer is arch-neutral,
   but the **loader driver re-emits to native + allocates executable pages**
   (em_loader.cpp:1039-1188, hardcoded `emit_arm64`/`emit_x64` at 1176-1181).
   **Fix**: add to §4/§6 (W3): *"Shape A requires a **deserialize-only loader
   path**: under `EMBER_WASM_INTERP`, `load_em_bytes_impl` skips the re-emit
   loop + `alloc_executable_rw`, instead building a dispatch table of
   `ThinFunction*` (deserialized + validated) for the interpreter. This is a
   fork of the existing re-emit path, not a config flag — ~200-400 lines."*

3. **MISSED COUPLING — `std::filesystem` (wasi-sdk).** The plan doesn't note
   that `ext_io.cpp` + `import.cpp` use `std::filesystem`, which wasi-libc
   lacks by default. **Fix**: add to §5: *"Emscripten provides
   `std::filesystem`; wasi-sdk does not (by default). Under wasi-sdk, gate
   `std::filesystem` behind `#ifndef EMBER_WASM_INTERP` + use raw
   `fopen`/`stat`, or stub ext_io. Prefer Emscripten for the first build."*

4. **UNDER-SPECIFIED — try/catch as pc-restore, not longjmp.** The plan (§2)
   says try/catch/throw = "a C++ `setjmp`/`longjmp` (or a status discipline)
   over an interpreter catch-stack." **Fix**: clarify — *"Intra-interpreter
   throw/catch = **set the interpreter pc to the catch block + restore
   call_depth from `catch_saved_call_depths`** (no register save needed — the
   interpreter has no registers; no libc longjmp needed). Libc
   `setjmp`/`longjmp` is reserved for the **host-level trap checkpoint**
   (the outermost host→interpreter call), mirroring context.hpp's
   `EMBER_SETJMP(ctx.checkpoint)`."*

5. **UNDER-SPECIFIED — the `arg_frame_offs` struct-by-value sentinel +
   struct-ret-ptr convention.** The plan (§2) mentions "StructLitInit/
   ArrayLitInit/StoreAddr → frame-buffer stores at field offsets" but doesn't
   mention the `vreg==0 && arg_frame_offs[i]!=-1` sentinel for struct-by-value
   call args, nor `ThinFramePlan::returns_struct_by_ptr` +
   `struct_ret_ptr_offset`. **Fix**: add — *"The interpreter mirrors
   emit_arm64's call-marshal: a struct-by-value arg is `args[i]==0` (the VReg-0
   sentinel) + `arg_frame_offs[i]` = the source frame slot → `memcpy` into the
   callee frame. A struct-by-ptr return honors `returns_struct_by_ptr` +
   `struct_ret_ptr_offset` (write the result into the caller-provided slot).
   No AAPCS64/HFA classifier is needed (structs are byte buffers), but the
   frame-plan conventions MUST be honored."*

6. **UNDER-SPECIFIED — StringDecrypt's two-slot layout.** The plan (§2) says
   "StringDecrypt → call the host decrypt." **Fix**: add — *"StringDecrypt
   uses **two** frame offsets: `meta.data_temp_off` (the decrypted-data temp
   buffer) + `meta.frame_off` (the slice result slot {ptr,len}). Decrypt rodata
   into `frame[data_temp_off]`, then write the slice {ptr, len=meta.len} into
   `frame[frame_off]` + `frame[frame_off+8]`. Mirror emit_arm64
   (thin_emit_arm64.cpp:1347-1355)."*

7. **UNDERESTIMATED WORK — ~1–2k lines → ~2.5–3.5k lines.** The interpreter
   core is ~1.5–2k (bounded, as the plan says), but the gating + GC linkage +
   loader fork + extension stubs add ~600–1.2k. **Fix**: revise §1/§7 to
   "~2.5–3.5k lines total (interpreter core ~1.5–2k + gating/stubs ~400–500 +
   GC linkage ~100–150 + loader fork ~200–400 + toolchain ~100–200). Still
   MEDIUM-LARGE, very doable."

8. **UNDER-SPECIFIED — `EMBER_WASM_INTERP` gating is new, not a flip.** The
   plan (§1) says "the native-JIT code… must compile OUT under a
   `EMBER_WASM_INTERP` target macro (like the `__x86_64__`/`__aarch64__`
   gating already added)." **Fix**: clarify — *"The `EMBER_WASM_INTERP` macro
   does **not yet exist**; the existing gating is arch-keyed
   (`__x86_64__`/`__aarch64__`/`_WIN32`/`__APPLE__`). Author new
   `#elif defined(EMBER_WASM_INTERP)` branches in platform.cpp (before the
   `#else #error`) + engine.cpp (the interpreter is called directly, no
   thunks) + new stubs for ext_thread/ext_call_raw. Model on the ARM64
   `arm64_exec_unimplemented` stubs + `ext_coroutine_stub.cpp`."*

9. **MISSING — performance expectation + realtime casualty.** The plan
   doesn't mention perf. **Fix**: add to §7 — *"The interpreter is ~10–50x
   slower than the JIT for compute-heavy loops (acceptable for ember's
   scripting/embedding/config use cases). `@realtime` audio is the known
   casualty — defer in WASM v1. Direct-threaded-dispatch is a future ~2-3x
   optimization."*

10. **MISSING — `non_serializable` obf functions in Shape A.** **Fix**: add
    to §4 (Shape A) — *"Shape-A `.em` modules must not contain
    `non_serializable` functions (the tree-walker fallback is x86-only). The
    host compiler lowers obf (MBA/opaque/str-encrypt — IR passes) to ThinIR
    before serializing; `@obf_keyed` is diagnosed unsupported-in-WASM at
    serialize time."*

11. **MISSING — module-dispatch hot-reload is feasible (not just native-page
    hot-reload).** **Fix**: in §8 (out of scope), distinguish — *"Native-page
    hot-reload is N/A in WASM. **Module-dispatch hot-reload** (swapping a
    module's `ThinFunction*` dispatch table at runtime) is **feasible** in the
    interpreter and is a post-W3 candidate, not impossible."*

---

## (e) Go/no-go recommendation + prerequisites before W0

**GO.** The plan is sound: the ThinIR-interpreter approach is the correct
(and effectively forced) WASM backend, the macOS ARM64 port has de-risked the
multi-backend ThinIR seam, and Shape A (interpret-only, compile on host) is a
clean first milestone that sidesteps the no-exceptions problem. The gaps are
real but fixable within a revised ~2.5–3.5k-line envelope; none are
architectural showstoppers.

**Prerequisites before starting W0:**

1. **Confirm Emscripten as the W0 toolchain** (not wasi-sdk) — avoids the
   `std::filesystem` + Shape-B-EH problems. Install Emscripten
   (`brew install emscripten` or the upstream `emsdk`); verify `em++` can
   compile a trivial C++17 program (std::optional, std::string_view,
   std::thread-less) to WASM.

2. **Read `src/thin_emit_arm64.cpp` as the reference implementation** for the
   interpreter's per-Op semantics (frame-slot model, CopyBytes, StringDecrypt
   two-slot, try/catch catch-stack, call-marshal with arg_frame_offs sentinel,
   struct-ret-ptr). The interpreter is "emit_arm64 in C++" — emit_arm64 is the
   spec.

3. **Decide the interpreter's frame model: frame-buffer-only** (not the
   plan's "VReg map OR frame-buffer" hedge). The IR's `meta.frame_off`
   conventions assume frame-buffer indexing; a VReg map is redundant +
   divergent. Confirm every VReg has a frame slot (the lowerer's spill-slot
   pass ensures this; `RegAllocResult` is JIT-only + ignored by the
   interpreter, as it is by emit_arm64 frame-only mode).

4. **Decide the catch mechanism: pc-restore + call_depth-restore** for intra-
   interpreter throw/catch; libc `setjmp`/`longjmp` ONLY for the host-level
   trap checkpoint. (Q1/Q2/R5.)

5. **Scope W0 to the interpreter core ONLY** (integer/control-flow subset,
   native validation alongside the JIT — no gating needed yet). Defer the
   `EMBER_WASM_INTERP` gating + Emscripten build to W1. This decouples
   interpreter-correctness from WASM-build-correctness. (Q5.)

6. **Budget the under-counted work explicitly**: gating/stubs (~400–500,
   W1), GC linkage (~100–150, W2), loader fork (~200–400, W3). Revise the
   estimate to ~2.5–3.5k lines total. (Q5/Correction 7.)

7. **Acknowledge the deferred/out-of-scope-for-v1 surface**: `@realtime`
   audio (perf), coroutines (stub; interpreter-native possible later),
   ext_thread (stub), ext_call_raw (stub), self-hosted codegen (x86-locked),
   `@obf_keyed` (N/A), wasi-sdk Shape B (the frontend refactor — only if
   required). (Q3/Q6/Corrections.)

**With these prerequisites + corrections incorporated into the plan, W0 can
proceed.** The plan is a good blueprint; this audit converts it from
"plausible" to "buildable."
