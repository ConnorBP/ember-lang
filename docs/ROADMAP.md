# ember - roadmap (v2+ deferrals)

Every feature deliberately not in v1, with the **re-entry trigger**
(the concrete signal that says "now build this") and the **dependency**
(what else must exist first). Ordered roughly by when each is likely
to become worth building. Nothing here is scheduled; this exists so a
deferral is a tracked decision, not a forgotten one.

## Shipped ahead of plan (between v0.1 and v0.2)

The `.em` binary bundling format shipped between v0.1 and v0.2 (user-requested, see `BUNDLING_AND_EM_MODULES.md`, `MODULES.md`). It is a single-module pre-compile format: a JIT'd function's post-resolve code + rodata + relocs serialize to a `.em` file, and `load_em_file` repoints the relocs at the loaded module's own dispatch table/globals block and runs the loaded code identically to the JIT'd version (one execution path, proven by `CODEGEN_SPEC.md` Section 15 test 7). This is not a Tier 6 feature - single-module, no live multi-module linking - but `em_loader`'s reloc and name-table infrastructure is a foundation for the eventual live-`import` path below.

## Tier 0 - standard addon set (ships with v1.0, host C++ side)

These are not language features - they're `NativeFn` addons using the
stable v1 binding API, delivered as part of a v1.0 release so mods are
actually writable (`GAP_ANALYSIS.md` Section 3):

- **`array<T>`** - host-allocated growable buffer behind an i64 handle.
  `push`/`pop`/`get`/`set`/`len`/`remove`/`clear`. Internal bounds
  checks in every indexing native.
- **`map<K,V>`** - hash map behind an i64 handle.
  `get`/`set`/`contains`/`remove`/`len`/iter-via-`slice` of keys.
- **`string` (mutable)** - `array<u8>` specialization + format/concat/
  find/substr natives. `f"..."` interpolation lowers to `__fmt`.
- **`math`** - sin/cos/sqrt/floor/ceil/abs/min/max/pow (f32 + f64).
- **`vec2/vec3/vec4`, `quat`, `mat4`** - registered as host-mapped
  structs with operator overloads (`TypeBuilder.bin_add` etc.), the
  one place v1's operator-overload API earns its keep for game work.

**Trigger to build:** v1.0 milestone. No JIT/type-system changes
needed - pure host C++ against the v1 `NativeFn`/`TypeBuilder` API.

## Tier 1 - small language extensions (post-v1.0, low cost)

- **`enum`** - script-side `enum E { A, B, C }` declaring named i32
  constants. Grammar + `EnumBuilder` (already noted as deferred in
  `BINDING_API.md` Section 5). Trigger: a real script wants more than ~5
  related constants and the global-flat hurts readability. Dep: none.
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

- **Function references** (`cast(fn)` → i64 handle, passable to
  natives like `register_routine`). This is the v2 answer to the surveyed native-JIT language's
  `register_routine(cast(my_draw), data)` dynamic-registration model
  (`GAP_ANALYSIS.md` Section 6). v1 uses static annotation discovery
  (`LIFECYCLE.md`); v2 adds dynamic registration for cases where the
  *script* decides at runtime what to hook (rare in game scripting,
  common in tooling). Trigger: a real mod needs runtime-decided
  callbacks. Dep: a stable "function handle" type (just an i64 = slot
  index, already exists internally; expose as a script-visible
  `fn` type). **No GC needed** - handles are slot indices into the
  existing dispatch table, not heap objects.

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

## Tier 6 - language ecosystem (never strictly required)

- **Modules / `import`** - multi-file linking. Trigger: a mod is big
  enough that one file is unmaintainable. Dep: a linker stage
  (resolve cross-module names → cross-module slot indices; the
  dispatch table is already per-module, so cross-module calls need a
  module-level dispatch indirection - moderate). The `em_loader`
  reloc/name-table infrastructure shipped in v0.1 (see "Shipped ahead of
  plan" above) is a foundation for the linker stage; the remaining work
  is cross-module name resolution, not the load/serialize mechanics.
- **Namespaces** - name scoping within a module. Trigger: module
  size makes flat scope crowded. Dep: modules, or usable standalone.
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

This roadmap is revisited **after the v0.5 benchmark milestone**
(`DESIGN.md` Section 9) - that's the first point where we have real
performance data and real usage to tell which deferrals are actually
blocking real scripts, vs. speculative. Adding features before v0.5
data exists is YAGNI; adding them after, driven by measured need, is
the whole point of the staged plan.
