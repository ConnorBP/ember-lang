# ember - roadmap (v2+ deferrals)

Every feature deliberately not in v1, with the **re-entry trigger**
(the concrete signal that says "now build this") and the **dependency**
(what else must exist first). Ordered roughly by when each is likely
to become worth building. Nothing here is scheduled; this exists so a
deferral is a tracked decision, not a forgotten one.

## Shipped ahead of plan (between v0.1 and v0.2)

The `.em` binary bundling format shipped between v0.1 and v0.2 (user-requested, see `BUNDLING_AND_EM_MODULES.md`, `MODULES.md`). It is a single-module pre-compile format: a JIT'd function's post-resolve code + rodata + relocs serialize to a `.em` file, and `load_em_file` repoints the relocs at the loaded module's own dispatch table/globals block and runs the loaded code identically to the JIT'd version (one execution path, proven by `CODEGEN_SPEC.md` Section 15 test 7). The `em_loader` reloc and name-table infrastructure it established became the foundation for the live-modules feature that shipped in **v0.5** (Tier 6, below — bidirectional script↔`.em` cross-module linking via `link "foo.em" as foo;` + `foo::bar()`).

## Shipped v1.0 (the concurrency + Tier 2 batch, commit e5d1814 + follow-ons)

Four features shipped in the v1.0 concurrency + Tier 2 language batch,
three of them pulled forward off this roadmap (each was a tracked
deferral below; each now carries a ✓ shipped marker at its tier entry):

- **Tier 1 `enum`** — script-side `enum E { A, B, C }` + `E::A` qualified
  access; sema rewrites `EnumAccessExpr` to an `IntLit`, no codegen change.
  See Tier 1 below.
- **Tier 2 first-class function references** — `&fn` / `handle(args)` /
  `fn` type keyword + the REDSHELL guard #6 call-target-provenance
  invariant. Two open items documented at the Tier 2 entry (the bare-`fn`
  signature hole, cross-module handles → v2+). See Tier 2 below.
- **Sync queue primitives** (`extensions/sync/`) — atomics, swap buffer,
  SPSC/MPSC/MPMC queues behind i64 handles, internally synchronized host
  storage; the script side stays single-threaded per context (U2).
  Documented at the Tier 5 "Threads" entry (this is the concurrency piece
  short of in-context threading).
- **Context thread-safety** (Option D + B1) — per-thread `context_t`,
  `r14` context-register indirection so one compiled body serves N contexts.
  Documented at the Tier 5 "Threads" entry; the keystone two-thread proof is
  `examples/thread_safety_test.cpp`.

Three follow-on commits then built on the batch: dynamic routine
registration (`extensions/lifecycle/`, `register_routine`/`unregister_routine` —
the Tier 2 dynamic-registration entry below, now ✓ shipped), the CLI `--tick`
wiring to the B1 model (the `--tick` thread-safety bug, fixed), and a demo
script (`examples/scripts/dynamic_registration.ember`). See
`docs/v1.0_INTEGRATION_NOTES.md` §5.

The current tree configures 20 CTest tests when the AngelScript SDK is present
and 19 when it is absent (only the benchmark is conditional). The four
`plan_*.md` files in `docs/` are historical design records; shipped contracts
in the main docs take precedence where those plans describe earlier states.

## Tier 0 - standard addon set (ships with v1.0, host C++ side)

These are not language features - they're `NativeFn` addons using the
stable v1 binding API, delivered as part of a v1.0 release so mods are
actually writable (`GAP_ANALYSIS.md` Section 3):

- **`array`** ✓ limited v1 API — opaque i64 buffer handle with new/length/
  resize, typed u8/f32/i64 get/set, and `array_push_u8`. Generic push,
  pop/remove/clear are deferred.
- **`map<K,V>`** — deferred; no map extension exists.
- **`string`** ✓ limited v1 API — opaque nominal handle with construction,
  from-slice/scalar conversion, length/character access, identity, concat and
  equality. find/substr/general format natives are deferred. F-strings lower
  through `__fstring_to_string` to `string_from_*`/identity; no `__fmt` exists.
- **`math`** ✓ limited v1 API — f32 `sqrt`/`sin`/`cos`/`tan`; broader f32/f64
  math is deferred.
- **`vec2/vec3/vec4`, `quat`, `mat4`** ✓ opaque nominal handles with
  constructors/accessors and registered overloads. `sync` ✓ provides atomics,
  swap buffers, and SPSC/MPSC/MPMC queues; `lifecycle` ✓ provides dynamic
  routine registration.

**Trigger to build:** v1.0 milestone. No JIT/type-system changes
needed - pure host C++ against the v1 `NativeFn`/`TypeBuilder` API.

## Tier 1 - small language extensions (post-v1.0, low cost)

- **`enum`** ✓ shipped v1.0 (Tier 1) - script-side `enum E { A, B, C }`
  declaring named constants, `E::A` qualified access. Top-level decl in
  `src/parser.cpp` (`parse_enum`); sema resolves each variant's value and
  rewrites the `EnumAccessExpr` to an `IntLit` **in place** (`src/sema.cpp`
  `lower_enum_access_expr`), so codegen sees an ordinary integer literal
  (no codegen change). Variants are `i32` values (auto-increment from 0,
  optional explicit `= constexpr_int_expr`, sign-extended into the `IntLit`'s
  i64 storage field); usable anywhere an `i64` is, exactly like an untyped
  integer literal. An enum name is *not* a type in v1 (`let x: Color = ...`
  is a clean sema error — the hook typed enums later flip). Duplicate
  variant names / duplicate explicit values within one enum are errors.
  Pinned by `tests/lang/{valid_enums,sema_valid_enums,
  sema_invalid_enum_unknown_enum,sema_invalid_enum_unknown_variant}.ember`.
  **No `EnumBuilder`** — the host-binding-side `EnumBuilder` sketched in
  `BINDING_API.md` Section 5 stays dropped: script enums need no host-side
  builder (the variants live entirely in the script). Typed enums (`enum E : i32`)
  and `enum`-from-expr remain a later refinement.
- **`for-each`** + `iterable()` TypeBuilder hook - `for (x in coll)`
  sugar over a length+index protocol. Trigger: `array<T>` addon use
  shows the manual index loop is a real friction point. Dep: `array<T>`
  addon exists.
- **`match` (pattern)** - richer than `switch` (struct destructure,
  guard). Trigger: `switch` plus nested `if` guards get unreadable in
  real handler code. Dep: `switch` (v1) proven.
- **`static_assert(cond, msg)`** - compile-time assertion. Trigger:
  binding code wants to verify struct layout assumptions at compile.
  Dep: `constexpr` evaluation broadened beyond literals (below).
- **`constexpr` function evaluation** - full compile-time fn eval
  (recursive `const` fns), enabling `static_assert`, complex array
  sizes, compile-time string building. Trigger: `static_assert` or
  `enum`-from-expr demand it. Dep: a const-eval interpreter (small,
  but a real subsystem).

## Tier 2 - function references + dynamic registration

- **Function references** ✓ shipped v1.0 — first-class `&fn` takes a
  handle, `handle(args)` calls indirectly, `fn` is the handle type
  keyword. Parser: prefix `&` in `parse_unary` builds a `FnHandleExpr`;
  a relaxed call target (`<expr>(args)` where `<expr>` is none of the
  named forms) builds an indirect `CallExpr`; `parse_type` treats bare
  `fn` as i64-with-`is_fn_handle`. Sema: `FnHandleExpr` bakes the slot
  as an i64 literal (and records the source fn's signature on the type
  for arg checking); the indirect `CallExpr` path type-checks args
  against the recorded signature when present; a local fn-typed var
  called like a named fn is promoted to the indirect path; **i64 ↔ fn
  assignment is forbidden either direction** (closes V3 forging at the
  type level). Codegen: `emit_call_target_guard` validates the runtime
  i64 against a host-built bitset allowlist (range + `bt` bit test)
  before indexing the dispatch table, dispatches via
  `lea_reg_mem_sib(r11,r11,rax,3)`; a bad handle traps `BadCallTarget`
  (longjmp), never a raw `call rax` of a script value. `build_fn_allowlist`
  is the host helper that derives the bitset from `script_slots`.
  Pinned by `examples/function_refs_test.cpp` (ctest target
  `function_refs`): handle creation, multi-arg dispatch, recursion via
  handle (`fib` via `&fib`), and the REDSHELL guard #6 (out-of-range and
  in-range-unregistered handles both trap). **No GC needed** — handles
  are slot indices into the existing dispatch table, not heap objects,
  exactly as the original Tier 2 entry predicted. Spec/plan: `docs/plan_FUNCTION_REFS.md`.

  **Two documented open items** (from `plan_FUNCTION_REFS.md` §9.3/§9.5):
  - **The bare-`fn` signature hole.** A `fn`-typed *parameter* (`fn h`,
    with no recorded signature) accepts any args at the call site — sema
    does not type-check them, the runtime guard still validates the handle
    is a registered slot before dispatch, and the called code obeys all
    budgets/bounds. This is a **type-soundness hole, not a sandbox
    violation** (a `let f: fn = &add; f(1);` call dispatches into `add`
    with `rcx=1, rdx=garbage` and returns a garbage result with no sema
    error). The fix is `fn(i64)->i64` parameterized function types (a
    real type-system expansion — signature equality, parameterized
    types); deferred to v2+. The guard ensures the *call target* is always
    safe; the *args* are the caller's responsibility at the bare-`fn`
    type, same as a C function pointer with no prototype.
  - **Cross-module function handles — deferred to v2+.** `&mod::fn` /
    `mod::fn` returning a handle is not in scope: the allowlist is
    per-module (built from this module's `script_slots`), and a handle
    from another module is that module's slot index, meaningless against
    this module's dispatch table. Cross-module handles need either a
    global `(module_id, slot)` handle space or per-module allowlists with
    a guard that knows which module a handle is for. Tier 2 ships
    intra-module handles only.

- **`register_routine`-style dynamic registration** ✓ shipped v1.0 (follow-on
  commits) — the host-native half of the surveyed native-JIT language's
  `register_routine(cast(my_draw), data)` model (`GAP_ANALYSIS.md` Section 6).
  v1 uses static annotation discovery (`LIFECYCLE.md`); the first-class `&fn`
  handle shipped above is the script-side prerequisite a host native needs to
  accept a handle argument. The dynamic-registration extension
  `extensions/lifecycle/` ships the `register_routine(fn h, i64 data) -> id` /
  `unregister_routine(id)` natives + the `host_routines()` accessor (the host
  iterates `(slot, data)` pairs and calls each via the dispatch table — the
  SAME call mechanism as the static `@on_tick` path, discovered by the script
  at runtime). The `fn` param is typed so sema rejects a forged plain i64;
  the slot's provenance was validated at the `&fn` site. Pinned by
  `examples/ext_lifecycle_test.cpp` (ctest target `ext_lifecycle`); demo
  `examples/scripts/dynamic_registration.ember`; see `LIFECYCLE.md` §2 and
  `docs/v1.0_INTEGRATION_NOTES.md` §5. (This was the reference host native the
  original Tier 2 entry described as "trigger-gated on a real mod needing
  runtime-decided callbacks" — the v0.6 integration + the Tier 2 fn-refs
  batch together fired that trigger, so it shipped in-tree as an extension.)

## Tier 3 - OOP / polymorphism

- **Classes (reference types) + single inheritance + vtables.**
  Trigger: a real game has >1 entity types sharing a base interface
  and the struct+free-function pattern gets unwieldy. Dep: **heap/GC**
  (Tier 4) - class instances are reference types, need managed
  lifetime; can't ship classes without a GC.
- **Interfaces** - abstract method contracts, dispatch via vtable.
  Trigger: with classes, multiple types share a contract. Dep: classes.
- **Mixins** - add methods to an existing type externally. Trigger:
  cross-cutting concerns (logging, serialization) across many types.
  Dep: classes.
- **Templates / monomorphization** - `fn foo<T>(x: T)`. Trigger:
  container addons (`array<T>`) want to be type-safe per-element
  without per-type native registration. Dep: monomorphization pass in
  the compiler (duplicate IR per instantiation - moderate complexity).

## Tier 4 - heap + GC

- **Tracing GC** - the v2 feature that unlocks classes, lambdas,
  dynamic object graphs. `MEMORY_AND_GC.md` Section 8 has the deferral
  rationale. Trigger: Tier 3 (classes) lands, requiring managed
  reference-object lifetimes. Dep: a write-barrier or
  safepoint-based GC (mark-sweep or incremental), root scanning
  across JIT frames (needs frame-layout metadata retained per
  function - `CODEGEN_SPEC.md` Section 2 already specifies the frame layout
  in a GC-friendly shape, so the metadata is derivable). Significant
  subsystem; only build when Tier 3 forces it.
- **`new`/`delete` + lambdas with capture** - depend on GC. Lambdas
  that capture by value can be done without GC (capture is a struct
  copy); by-reference capture needs GC for the captured refs' lifetime.
  Trigger: closure-style callbacks in real demand. Dep: GC.

## Tier 5 - concurrency + exceptions (largest, least certain)

- **Coroutines / `yield`** - needs suspended-frame storage (copy a
  frame off the native stack into a heap allocation on `yield`,
  restore on `next()`). Trigger: a real game wants script-driven
  cutscenes/AI with sequential-looking code. Dep: heap/GC for the
  suspended frame storage (Tier 4). Moderate.
- **Exceptions `try`/`catch`/`throw`** - v1's `runtime_error`/
  `runtime_exception` host-signal + non-local unwind
  (`SAFETY_AND_SANDBOX.md` Section 7) covers host→script abort. In-language
  `try`/`catch` is a different thing (script catches script-thrown
  errors at specific frames). Trigger: real script wants
  local recovery rather than whole-call abort. Dep: per-frame
  catch-handler registration (extends the checkpoint stack in
  `SAFETY_AND_SANDBOX.md` Section 2). Moderate, but complicates the
  unwind machinery - only if there's real demand.
- **Threads (`thread` addon, `aint*` atomics)** - multithreaded
  script execution inside one context is a v1 non-goal
  (`SAFETY_AND_SANDBOX.md` Section 8). Trigger: a compute-heavy mod needs
  parallelism beyond running multiple `context_t`s. Dep: GC
  thread-safety, per-context arena, the whole memory model gets
  harder. Large; defer as long as possible - multi-context
  parallelism covers most real cases without in-context threading.

  **Shipped v1.0 (the two pieces short of in-context threading):**
  - **Context thread-safety (Option D + B1).** A `context_t` is per-thread
    (private checkpoint/budget/depth); the dispatch table, JIT'd code, and
    module registry are shared and read-only after compile. With
    `CodeGenCtx::use_context_reg = true`, the budget/depth/trap emits read
    `context_t` fields through `r14` (the per-call context register,
    callee-saved so preserved across script-to-script calls) instead of
    baked imm64 pointers — **one compiled body serves N per-thread
    contexts**, no per-context recompile. The host sets `r14 = ctx` at entry
    via `ember_call_void` / `ember_call_i64` (`src/engine.{hpp,cpp}`).
    Pinned by `examples/thread_safety_test.cpp` (ctest target `thread_safety`):
    the keystone two-thread proof (per-thread context, no cross-thread
    longjmp corruption — the generalized `--tick` bug). The legacy
    `g_globals_for_codegen` process-wide pointer is superseded by
    `ctx.globals_index` / `ctx.globals_types` (parallel-compile-safe; the
    legacy pointer stays as a null-by-default backward-compat fallback so
    existing hosts keep working). The CLI `--tick` path is the first host to
    consume B1 (commit `c6457cd`): the CLI compiles with `use_context_reg =
    true`, calls `@entry` via `ember_call_void(entry, &ectx)`, and runs the tick
    thread on its own `context_t` (`tick_ctx`) via `ember_call_void(f,
    &tick_ctx)`, fully isolated from the main thread — the `--tick` thread-
    safety bug, fixed. Spec/plan: `docs/plan_CONTEXT_THREADSAFETY.md`,
    `docs/v1.0_INTEGRATION_NOTES.md` §1.
  - **Sync queue primitives (`extensions/sync/`).** Atomics
    (`aint8/16/32/64`: load/store/fetch_add/cas/swap with width masking),
    a swap buffer (double-buffer + atomic flip), and SPSC/MPSC/MPMC queues
    — all behind opaque i64 handles, with internally-synchronized host
    storage (`std::atomic` / lock-free ring / host-internal `std::mutex` for
    MPMC held only across a short ring-index critical section, never across
    an ember call). The script side stays single-threaded per context (the
    **U2 contract** in `ext_sync.hpp`'s S0 scope statement: the host threads
    that produce/consume touch only the queue HOST storage via the `_host`
    accessors, never the context). All queue natives are **non-blocking**
    (push returns 0 on full, `try_pop` returns `INT64_MIN` on empty); a
    script cannot deadlock on any primitive here. Pinned by
    `examples/ext_sync_test.cpp` (ctest target `ext_sync`): 16 tests incl.
    multi-thread stress (10k SPSC, MPMC contention, no lost/dup).
    Spec/plan: `docs/plan_SYNC_QUEUES.md`.

  These two pieces together cover the **multi-context parallelism** case
  the original entry names as covering "most real cases": a host runs one
  `context_t` per worker thread against shared compiled code, and the sync
  primitives let those workers (and host producer/consumer threads) coordinate
  on host-owned shared state. **In-context multithreaded script execution**
  (two ember-calling threads on one `context_t`) remains a non-goal and still
  races exactly as `plan_CONTEXT_THREADSAFETY.md` S1.2 documents — the sync
  addon does not make a `context_t` safe for concurrent calls; the U2 contract
  is the discipline that keeps the queues correct.

## Tier 6 - language ecosystem (never strictly required)

- **Modules / live `link`** ✓ shipped in v0.5 — bidirectional script↔`.em` cross-module linking. `link "foo.em" as foo;` loads+registers a pre-compiled `.em` (or `link "foo" as foo;` links to an already-registered module); `foo::bar(args)` is the cross-module call. The runtime half (ModuleRegistry + kind-2 reloc) was built earlier; v0.5 added the source half (grammar, sema resolution, codegen emission, a linker/loader, an `--emit-em` pre-compile CLI mode). See `MODULES.md` (now implemented, not just a design sketch) + `examples/v0_5_live_modules_test.cpp`. The *textual* `import "path";` (parse-time source merge into one module) is unchanged and remains the one-file-bundle mechanism. Open: the `link` to an already-registered module (bare-name form) is host-driven in v0.5 (a host pre-registers); a future linker could resolve bare-name links against a module search path.
- **Namespaces** - name scoping within a module. Trigger: module
  size makes flat scope crowded. Dep: modules (now shipped), or usable standalone.
- **Preprocessor** - deliberately never; `engine.define(name,value)`
  (`DESIGN.md` Section 8) + `const`/`constexpr` cover the legitimate
  compile-time-conditional needs without C-preprocessor footguns.
  A `#include`-equivalent is just module `import`.

## What will never be added (hard non-goals)

- **`goto`** - structured control only; complicates liveness, scope,
  and `defer` emission for no benefit over `break`/`continue`/`switch`.
- **C-preprocessor** - `define`/`ifdef`/`include`/`pragma`. Use
  `engine.define` + `const`/`constexpr` + modules.
- **Multiple inheritance / diamond** - even the surveyed native-JIT language disallows diamonds;
  single inheritance (Tier 3) is the ceiling.
- **Self-hosting** - the compiler stays C++; no benefit to making
  ember its own implementation language for an embedded scripting
  use case.

## Re-evaluation cadence

This roadmap was originally revisited after the v0.5 benchmark milestone
(`DESIGN.md` Section 9) — the first point with real performance data and
real usage to tell which deferrals were actually blocking real scripts
vs. speculative. The v1.0 concurrency + Tier 2 batch (commit e5d1814)
followed exactly that re-evaluation: `enum`, first-class function refs, sync
queues, and context thread-safety all moved from tracked deferrals below
into shipped after the v0.6 integration surfaced concrete demand (a per-frame
tick loop with per-thread state, and the host↔script coordination pattern the
sync primitives serve). Going forward the same trigger-driven rule applies:
adding features driven by measured need, not speculative demand, is the
whole point of the staged plan.
