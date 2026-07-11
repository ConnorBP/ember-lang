# ember - roadmap

Every feature not yet shipped, ordered roughly by dependency and value.
As of 2026-07-11, **all items below are active TODOs** — the trigger-gated
"deferral" phase is over; we are working through the list. Each item carries
its **dependency** (what else must exist first) and its **status** (TODO,
in-progress, or blocked). ember is a **C-style procedural language**: structs
+ free functions, not classes/inheritance/vtables (OOP is a hard non-goal —
see the bottom). The long-term north star is **self-hosting**: an ember
compiler written in ember (work started — `demo/compiler/` is a µ-language
lexer/parser/evaluator all in ember source).

## Shipped ahead of plan (between v0.1 and v0.2)

The `.em` binary bundling format shipped between v0.1 and v0.2 (user-requested, see `BUNDLING_AND_EM_MODULES.md`, `MODULES.md`). It is a single-module pre-compile format: a JIT'd function's post-resolve code + rodata + relocs serialize to a `.em` file, and `load_em_file` repoints the relocs at the loaded module's own dispatch table/globals block and runs the loaded code identically to the JIT'd version (one execution path, proven by `spec/CODEGEN_SPEC.md` Section 15 test 7). The `em_loader` reloc and name-table infrastructure it established became the foundation for the live-modules feature that shipped in **v0.5** (Tier 6, below — bidirectional script↔`.em` cross-module linking via `link "foo.em" as foo;` + `foo::bar()`).

## Shipped v1.0 (the concurrency + Tier 2 batch, commit e5d1814 + follow-ons)

Four features shipped in the v1.0 concurrency + Tier 2 language batch,
three of them pulled forward off this roadmap (each was a tracked
deferral below; each now carries a ✓ shipped marker at its tier entry):

- **Tier 1 `enum`** — script-side `enum E { A, B, C }` + `E::A` qualified
  access; sema rewrites `EnumAccessExpr` to an `IntLit`, no codegen change.
  See Tier 1 below.
- **Tier 2 first-class function references** — `&fn` / `handle(args)` /
  `fn` type keyword + the call-target-provenance guard
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
`planning/v1.0_INTEGRATION_NOTES.md` §5.

The current tree configures 42 CTest targets total (40 excluding the two
bench targets `bench_codegen_paths` and `bench_ember_vs_as`; the latter is
only configured when the AngelScript SDK is present, so a no-SDK build
configures 41). The four `plan_*.md` files in `docs/` are historical design
records; shipped contracts in the main docs take precedence where those plans
describe earlier states.

## Self-hosting (the north star — work started)

**Status: IN PROGRESS (foundation laid).** The long-term goal is an ember
compiler written in ember — the language hosting itself. This is NOT a
non-goal (corrected from an earlier roadmap revision that listed it there);
it is the direction the language is growing toward.

**Work already done:** `demo/compiler/` is a complete µ-language
lexer/parser/evaluator written entirely in ember source (`lex.ember`,
`parse.ember`, `eval.ember`, `main.ember`). It stresses the pure-compute
language surface — enum, struct, switch, slices, recursion, fn-ref handles,
fixed arrays, import — and proves ember can host a compiler. The demo parses
`let a = 1 + 2 * 3; a + 10` → 17 with correct precedence, left-associativity,
parens, and divide-by-zero detection.

**The path to full self-hosting** (each step is a TODO below or in the tiers;
steps marked ✓ are done):
1. **`constexpr` function evaluation** (Tier 1) ✓ shipped 2026-07-11 —
   compile-time fn eval is the foundation a self-hosting compiler needs
   (macro-like transforms, constant folding of compiler data structures).
2. **String + I/O maturity** (io extension shipped; string extension shipped)
   — a compiler reads/writes files and manipulates strings heavily.
3. **Map extension** (shipped) — symbol tables, AST node pools.
4. **Lambdas with capture** (Tier 3/GC) — a real compiler uses closures for
   visitor patterns, scope chains, error accumulators.
5. **The ember-written ember lexer/parser/sema** — port the C++ frontend to
   ember, one stage at a time, validating each against the C++ reference.
6. **The ember-written ember codegen** — emit x86-64 (or ThinFunction IR)
   from ember. This needs the JIT/memory natives (shipped as host extensions)
   and is the final self-hosting milestone.

This is a multi-month effort, not a single feature. Every language feature
shipped (constexpr, lambdas, GC, the standard addons) is a step toward it.

## Tier 0 - standard addon set (ships with v1.0, host C++ side)

These are not language features - they're `NativeFn` addons using the
stable v1 binding API, delivered as part of a v1.0 release so mods are
actually writable (`planning/GAP_ANALYSIS.md` Section 3):

- **`array`** ✓ limited v1 API — opaque i64 buffer handle with new/length/
  resize, typed u8/f32/i64 get/set, push_u8/f32/i64, pop_u8/f32/i64, clear,
  remove. ✓ full v1 API shipped (push/pop/clear/remove added 2026-07-11).
- **`map<K,V>`** ✓ shipped 2026-07-11 — opaque i64 handle backed by a host-
  side `unordered_map<i64,i64>`. API: map_new, map_set, map_get, map_contains,
  map_length, map_remove, map_clear. K and V are i64 (v1 convention; typed
  keys/values are a v2 concern).
- **`string`** ✓ limited v1 API — opaque nominal handle with construction,
  from-slice/scalar conversion, length/character access, identity, concat,
  equality, find, substr. ✓ find/substr shipped 2026-07-11; general format
  natives are a TODO. F-strings lower
  through `__fstring_to_string` to `string_from_*`/identity; no `__fmt` exists.
- **`math`** ✓ limited v1 API — f32 `sqrt`/`sin`/`cos`/`tan`; f64 `sqrt_f64`/
  `sin_f64`/`cos_f64`/`tan_f64`/`floor_f64`/`ceil_f64`/`abs_f64`/`pow_f64` +
  `abs_i64` shipped 2026-07-11; broader math still deferred.
- **`vec2/vec3/vec4`, `quat`, `mat4`** ✓ opaque nominal handles with
  constructors/accessors and registered overloads. `sync` ✓ provides atomics,
  swap buffers, and SPSC/MPSC/MPMC queues; `lifecycle` ✓ provides dynamic
  routine registration; `io` ✓ shipped 2026-07-11 (the CLI Family B trigger,
  below) — console I/O (`print`, `println`, `print_i64`, `print_f64`,
  `read_line`), file I/O (`file_read_bytes`, `file_write_bytes`,
  `file_exists`), and path ops (`path_exists`, `path_basename`,
  `path_dirname`). ALL `io` natives are `PERM_FFI`-gated — a module compiled
  without the FFI permission bit has every I/O call site rejected at sema
  time (the CLI grants the bit via `--ffi` / `--allow-io`; without it the
  natives are registered but not callable). See the Family B entry below for
  the full shipped contract.

**Trigger to build:** v1.0 milestone. No JIT/type-system changes
needed - pure host C++ against the v1 `NativeFn`/`TypeBuilder` API.

## Tier 1 - small language extensions (post-v1.0, low cost)

- **`enum`** ✓ shipped v1.0 (Tier 1) - script-side `enum E { A, B, C }`
  declaring named constants, `E::A` qualified access. Top-level decl in
  `src/parser.cpp` (`parse_enum`); sema resolves each variant's value and
  rewrites the `EnumAccessExpr` to an `IntLit` **in place** (`src/sema.cpp`
  `lower_enum_access_expr`), so codegen sees an ordinary integer literal
  (no codegen change). Variants are `i32` values (auto-increment from 0,
  optional explicit `= constexpr_int_expr` or `= constexpr_fn_call`, sign-
  extended into the `IntLit`'s i64 storage field); usable anywhere an `i64` is,
  exactly like an untyped integer literal. An **untyped** enum name is not a
  type (`let x: Color = ...` for an untyped `Color` is a clean sema error);
  the **typed** enum form (`enum E : T`, see the Typed enums entry below)
  makes the enum name a real type. Duplicate
  variant names / duplicate explicit values within one enum are errors.
  Pinned by `tests/lang/{valid_enums,sema_valid_enums,
  sema_invalid_enum_unknown_enum,sema_invalid_enum_unknown_variant}.ember`.
  **No `EnumBuilder`** — the host-binding-side `EnumBuilder` sketched in
  `spec/BINDING_API.md` Section 5 stays dropped: script enums need no host-side
  builder (the variants live entirely in the script). Typed enums (`enum E : T`)
  and enum-from-constexpr-expr **shipped 2026-07-11** (see the dedicated Tier 1
  entry below).
- **`for-each`** ✓ shipped 2026-07-11 (Tier 1) — `for (x in slice)` over a
  slice T[], **and** `for (x in array_handle)` over an array<T> handle (the
  iterable() hook, array case, shipped 2026-07-11). For a slice, the
  tree-walker evaluates the slice → {ptr, len} and emits a while loop with
  element indexing at [ptr + index*elem_size]. For an array<T> handle (an
  opaque i64 from `array_new`), sema infers the element type (u8/f32/i64)
  from the `array_new` elem_size and tags the handle; codegen lowers the
  for-each to `len = array_length(h); while i < len { x = array_get_*(h, i);
  ... }`, dispatching to the typed get native for the inferred element type.
  A bare i64 that isn't provably an array handle is rejected (so `for (x in
  42)` stays an error). The IR backend marks functions using for-each as
  non_serializable (falls back to the tree-walker). The general `iterable()`
  hook surface is documented below (only the array case is implemented now;
  map and host collections are the follow-on).
- **`match` (pattern)** ✓ shipped 2026-07-11 (Tier 1, v1 form) —
  `match (expr) { pattern => body, _ => default }`. Patterns: integer/bool
  literals + `_` wildcard. Each arm is a separate branch (no fallthrough, no
  break). Body can be a block or single statement. The IR backend marks
  functions using match as non_serializable (falls back to the tree-walker) —
  `src/thin_lower.cpp` treats `MatchStmt` the same as `ForEachStmt` for Stage A.
  Struct destructure + guards are a later refinement.
- **`static_assert(cond, msg)`** ✓ shipped 2026-07-11 (Tier 1) —
  `static_assert(cond, "msg");` is a compile-time assertion. The condition is
  folded at sema (it may be a literal integer/bool expression or a `constexpr
  fn` call — the constexpr-call pre-pass folds those first). A **false** result
  is a **compile error** carrying `msg`; a **true** result is **elided** (no
  runtime code is emitted for the assertion). A non-constant condition is a
  compile error ("static_assert condition must be a compile-time constant").
  Usable both at top level (a `static_assert_decl` on `Program`) and inside a
  function body (`StaticAssertStmt`); both positions apply the identical
  compile-time verdict via the shared `check_static_assert`. Parser:
  `parse_static_assert`; sema: `check_static_assert` (`src/sema.cpp`). Pinned
  by `examples/static_assert_test.cpp` (ctest `static_assert`) +
  `tests/lang/{valid_static_assert,valid_static_assert_constexpr,
  sema_invalid_static_assert_false,sema_invalid_static_assert_nonconst}.ember`.
  (Was blocked on constexpr; the two shipped together.)
- **`constexpr` function evaluation** ✓ shipped 2026-07-11 (Tier 1) — a fn
  declared `constexpr fn name(...) -> i64 { ... }` **can** be const-evaluated
  at sema time when called with all-constant args. A tree-walking interpreter
  (`eval_constexpr_fn` in `src/sema.cpp`) evaluates the call and the
  constexpr-call pre-pass (`lower_constexpr_calls_expr` / `try_fold_constexpr_call`)
  rewrites the call site to an `IntLit` carrying the folded result **before**
  `check_expr` runs — so codegen, the const-folder, the switch case-value
  check, the static_assert check, and the globals/enum initializer evaluators
  all see an ordinary integer literal. **Bounds:** max 100000 loop iterations
  per loop, max 256 recursion depth (nested constexpr calls); **i64 integer
  fns only** in this increment (float/bool/struct fns skip constexpr eval and
  fall back to a normal runtime call). A constexpr fn called with a
  non-constant (runtime) arg also falls back to a normal runtime call — a
  `constexpr` fn is one that **can** be const-evaluated, not one that **must**
  be. The `constexpr` modifier is only valid on a function declaration
  (`constexpr fn`); `priv` and `constexpr` may appear in either order before
  `fn`. Foundational — unblocks static_assert, typed enums, enum-from-constexpr.
  Pinned by `examples/constexpr_test.cpp` (ctest `constexpr`) +
  `tests/lang/{valid_constexpr,valid_constexpr_in_expr,valid_constexpr_recursive,
  valid_constexpr_runtime_fallback,invalid_constexpr_not_fn}.ember`.
- **Typed enums (`enum E : T`) + enum-from-constexpr-expr** ✓ shipped
  2026-07-11 (Tier 1) — a typed enum `enum E : T { ... }` (e.g. `enum Color :
  i32`) makes the enum name a **real type** backed by the integer `T`
  (`let c: Color = Color::Red;` works). The backing type is recorded in
  `typed_enum_backing` / `typed_enum_types` (sema Pass 1.4, `register_typed_enums`);
  `EnumAccessExpr` lowers to an `IntLit` carrying the variant's value typed as
  the enum type (not a plain i32). **Conversion rule:** enum→int implicit
  widening is allowed (a typed-enum value widens to its backing int, and on to
  i64, via a synthesized `CastExpr` — so `return c` from an i64 fn works);
  **int→enum is rejected** (a raw integer literal `let c: Color = 5` is a sema
  error — a typed-enum value must come from a `Color::Variant` or another
  `Color` binding). **Enum-from-constexpr-expr:** a variant's explicit value
  may be a `constexpr fn` call (`X = base()` where `base` is a `constexpr fn`);
  `resolve_enums` folds the call to an `IntLit` via the constexpr-call pre-pass
  before the const-check, so `X = base()` resolves. Untyped enums (`enum E {
  ... }`, i32 variants) are unchanged (backward compat). Pinned by
  `examples/typed_enum_test.cpp` (ctest `typed_enum`) + `tests/lang/{valid_typed_enum,
  valid_typed_enum_match,valid_enum_from_constexpr,sema_invalid_int_to_enum}.ember`.
- **`iterable()` hook** — **PARTIAL (array case shipped 2026-07-11).**
  General collection iteration for `for (x in collection)` beyond slices. The
  shipped for-each now covers two iterable kinds: slices T[] (ptr+len
  indexing) and array<T> handles (array_length + array_get_*). The hook
  surface is documented in `src/ast.hpp` (`ForEachStmt::array_elem_ty`),
  `src/sema.cpp` (the ForEachStmt check + the `infer_*_array_elem_ty`
  helpers), and `src/codegen.cpp` (the ForEachStmt array branch). Future
  iterables (map, host collections) register through the same surface: sema
  recognizes the iterable type and determines its element type, then codegen
  lowers the loop to that type's length + element-access primitives. Only the
  array case is implemented now; the rest is deferred. Dep: per-type length/
  element-access primitives (a map would need map_size + map_iter, etc.).
- **Struct destructure + guards in match** - **TODO.** A later refinement of
  the shipped `match`: let a pattern destructure a struct (`Point{ x, y } =>`)
  and carry a guard (`Point{ x, y } if x > 0 =>`). Dep: parser + sema work.

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
  handle (`fib` via `&fib`), and the call-target-provenance guard (out-of-range and
  in-range-unregistered handles both trap). **No GC needed** — handles
  are slot indices into the existing dispatch table, not heap objects,
  exactly as the original Tier 2 entry predicted. Spec/plan: `planning/plan_FUNCTION_REFS.md`.

  **Two documented open items** (from `planning/plan_FUNCTION_REFS.md` §9.3/§9.5):
  - **The bare-`fn` signature hole.** A `fn`-typed *parameter* (`fn h`,
    with no recorded signature) accepts any args at the call site — sema
    does not type-check them, the runtime guard still validates the handle
    is a registered slot before dispatch, and the called code obeys all
    budgets/bounds. This is a **type-soundness hole, not a sandbox
    violation** (a `let f: fn = &add; f(1);` call dispatches into `add`
    with `rcx=1, rdx=garbage` and returns a garbage result with no sema
    error). The fix is `fn(i64)->i64` parameterized function types (a
    real type-system expansion — signature equality, parameterized
    types) — **TODO**. The guard ensures the *call target* is always
    safe; the *args* are the caller's responsibility at the bare-`fn`
    type, same as a C function pointer with no prototype.
  - **Cross-module function handles — TODO.** `&mod::fn` /
    `mod::fn` returning a handle is not in scope: the allowlist is
    per-module (built from this module's `script_slots`), and a handle
    from another module is that module's slot index, meaningless against
    this module's dispatch table. Cross-module handles need either a
    global `(module_id, slot)` handle space or per-module allowlists with
    a guard that knows which module a handle is for. Tier 2 ships
    intra-module handles only.

- **`register_routine`-style dynamic registration** ✓ shipped v1.0 (follow-on
  commits) — the host-native half of the surveyed native-JIT language's
  `register_routine(cast(my_draw), data)` model (`planning/GAP_ANALYSIS.md` Section 6).
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
  `planning/v1.0_INTEGRATION_NOTES.md` §5. (This was the reference host native the
  original Tier 2 entry described as "trigger-gated on a real mod needing
  runtime-decided callbacks" — the v0.6 integration + the Tier 2 fn-refs
  batch together fired that trigger, so it shipped in-tree as an extension.)

## Tier 3 - heap + GC (was Tier 4; OOP Tier 3 moved to non-goals)

OOP/polymorphism (classes, interfaces, mixins, templates) was moved to the
hard non-goals (ember is C-style procedural, not OOP). GC stays — it unlocks
lambdas-with-capture and coroutines, not classes.

- **Tracing GC** - TODO. Unlocks lambdas with by-reference capture and
  coroutines (suspended-frame heap storage), NOT classes (OOP is a non-goal).
  `spec/MEMORY_AND_GC.md` Section 8 has the design rationale. Dep: a
  write-barrier or safepoint-based GC (mark-sweep or incremental), root
  scanning across JIT frames (needs frame-layout metadata retained per
  function - `spec/CODEGEN_SPEC.md` Section 2 already specifies the frame layout
  in a GC-friendly shape, so the metadata is derivable). Significant subsystem.
- **`new`/`delete` + lambdas with capture** - TODO, depends on GC. Lambdas
  that capture by value can be done without GC (capture is a struct copy);
  by-reference capture needs GC for the captured refs' lifetime. Dep: GC.

## Tier 4 - concurrency + exceptions (was Tier 5)

- **Coroutines / `yield`** - **TODO** (blocked on GC/heap). Needs suspended-
  frame storage (copy a frame off the native stack into a heap allocation on
  `yield`, restore on `next()`). A real game wants script-driven cutscenes/AI
  with sequential-looking code. Dep: heap/GC for the suspended frame storage
  (Tier 3). Moderate.
- **Exceptions `try`/`catch`/`throw`** - **TODO.** v1's `runtime_error`/
  `runtime_exception` host-signal + non-local unwind
  (`spec/SAFETY_AND_SANDBOX.md` Section 7) covers host→script abort. In-language
  `try`/`catch` is a different thing (script catches script-thrown errors at
  specific frames). Dep: per-frame catch-handler registration (extends the
  checkpoint stack in `spec/SAFETY_AND_SANDBOX.md` Section 2). Moderate, but
  complicates the unwind machinery.
- **In-context threads (`thread` addon, `aint*` atomics)** - **TODO** (largest,
  highest-risk). Multithreaded script execution inside one context. Dep: GC
  thread-safety, per-context arena, the whole memory model gets harder.
  Multi-context parallelism (shipped) covers most real cases without in-
  context threading; this is the residual case for compute-heavy mods that
  need parallelism within one script context.

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
    safety bug, fixed. Spec/plan: `planning/plan_CONTEXT_THREADSAFETY.md`,
    `planning/v1.0_INTEGRATION_NOTES.md` §1.
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
    Spec/plan: `planning/plan_SYNC_QUEUES.md`.

  These two pieces together cover the **multi-context parallelism** case
  the original entry names as covering "most real cases": a host runs one
  `context_t` per worker thread against shared compiled code, and the sync
  primitives let those workers (and host producer/consumer threads) coordinate
  on host-owned shared state. **In-context multithreaded script execution**
  (two ember-calling threads on one `context_t`) remains a non-goal and still
  races exactly as `planning/plan_CONTEXT_THREADSAFETY.md` S1.2 documents — the sync
  addon does not make a `context_t` safe for concurrent calls; the U2 contract
  is the discipline that keeps the queues correct.

## Tier 6 - language ecosystem (never strictly required)

- **Modules / live `link`** ✓ shipped in v0.5 — bidirectional script↔`.em` cross-module linking. `link "foo.em" as foo;` loads+registers a pre-compiled `.em` (or `link "foo" as foo;` links to an already-registered module); `foo::bar(args)` is the cross-module call. The runtime half (ModuleRegistry + kind-2 reloc) was built earlier; v0.5 added the source half (grammar, sema resolution, codegen emission, a linker/loader, an `--emit-em` pre-compile CLI mode). See `MODULES.md` (now implemented, not just a design sketch) + `examples/v0_5_live_modules_test.cpp`. The *textual* `import "path";` (parse-time source merge into one module) is unchanged and remains the one-file-bundle mechanism. Open: the `link` to an already-registered module (bare-name form) is host-driven in v0.5 (a host pre-registers); a future linker could resolve bare-name links against a module search path.
- **Namespaces** - **TODO.** Name scoping within a module. Dep: modules
  (now shipped), or usable standalone. Trigger: module size makes flat scope
  crowded.
- **Preprocessor** - deliberately never; `engine.define(name,value)`
  (`planning/DESIGN.md` Section 8) + `const`/`constexpr` cover the legitimate
  compile-time-conditional needs without C-preprocessor footguns.
  A `#include`-equivalent is just module `import`.

## Slated for removal (deprecated)

- **`auto x = expr;`** — deprecated (2026-07-10). A redundant spelling of
  `let x = expr;` type inference (both share the same `is_auto` inference
  path in the parser). `let x = expr;` is the canonical inference form;
  `let x: T = expr;` is the explicit form. `auto` adds a second way to spell
  the same thing with no semantic difference, so it's deprecated for
  removal: using it emits a non-fatal sema warning (the program still
  compiles and runs). Migrate any `auto x = e;` to `let x = e;`. Removed
  from the in-tree tests; remove from the language after a grace period.

## Investigation-backed candidate changes

Changes recommended by a completed investigation. The first item below is
now DONE; the rest remain not-yet-slated-for-a-specific-milestone, listed
here so the decision and its evidence have a tracked home.

- **Runtime string encryption — DONE (2026-07-10).** Replaced the
  `__str_decrypt` heap-ptr native contract with an inline-stack-XOR
  lowering: an encrypted string literal is now decrypted inline into a
  compiler-hidden temp frame slot at each use site (see codegen's StringLit
  eval case / `alloc_str_temp` / `count_str_temps_block`). The plaintext is
  TRANSIENT — it lives only on the caller's stack frame for the
  expression's lifetime and is reclaimed at frame teardown; no heap, no
  host native call, no leak. The `__str_decrypt` host contract was removed
  entirely (`str_decrypt_fn`/`str_decrypt_name` dropped from `CodeGenCtx`);
  encryption is now pure codegen, so a host turns it on just by setting
  `Program::string_xor_key != 0` before sema (the CLI default is now `0xA5`,
  ON by default for the first time — the old "no in-tree host registers the
  native" barrier is gone). The `string`-handle path reuses the same inline
  XOR + `string_from_slice` (the handle owns the only persistent copy).
  **Design note: the const/non-const literal classification the original
  analysis recommended was NOT implemented — every encrypted literal takes
  the same inline-XOR path.** This is safe because the `string`-handle path
  copies out of the stack temp immediately (the handle owns the only
  persistent copy), and a raw `slice<u8>` literal that escapes the frame
  (returned, stored to a global, captured across loop iterations) has the
  same pre-existing dangling-slice-of-stack-local limitation that
  `local_array_view` already has — not a regression introduced here.
  Verified: ctest 22/22, lang 245/0/0 with encryption on (incl. a 260-byte
  literal test that forces the runtime-loop XOR path), plus a probe
  (`tmp_edit/enc2/`) confirming 0 native calls, distinct stack temps per
  use, and frame-reclaimed plaintext after return. See
  `../demo/STRING_ENCRYPTION_ANALYSIS.md` for the original analysis + the
  probe that demonstrated the old heap-residency leak.

- **Slice-of-stack-local escape safety — STAGE 1 DONE (2026-07-10), STAGE 2
  TODO.** A stack-backed slice (a `ViewExpr` over a fixed array, or an
  encrypted `StringLit` temp) could escape its frame — returned, stored to a
  global, stored into a global struct's field — and dangle. `is_local_array_view`
  guarded only 2 of 5 escape categories, and `StringLit`-derived slices were
  invisible to it entirely (all 5 open). Stage 1 (this pass, sema-only, no
  codegen): (1) `is_local_array_view` now covers a `StringLit` whose resolved
  type is `slice<u8>` (closes C1 return + C2a global-store for StringLit via
  the existing guards); (2) a new `AssignExpr` guard chases a `FieldExpr`/
  `IndexExpr` target to its root base and rejects when the root is a global
  (closes C2b for BOTH classes, incl. nested `go.inner.data = a[..]`); (3)
  updated the C1/C2a error messages from "local fixed array" to "stack local".
  **Stage 2 (deferred): C3 (stack-backed slice passed to a native that may
  retain the ptr) and C5 (stack-backed slice passed to a script fn / fn-handle
  / cross-module call that may retain it) are NOT yet guarded** — a blanket
  reject there would break the legitimate synchronous pattern
  `return_slice_defer(return_values[..])` (a fn that returns its slice arg for
  the caller to read within the caller's own frame). Closing C3/C5 needs a real
  borrow/escape analysis: propagate the localview bit through a call's return
  value, reject only at the actual escape point (return/store of the
  propagated result), and add a `borrows`/`retains` annotation to `NativeSig`
  so C3 can distinguish copying natives (`string_from_slice`) from retaining
  ones. No shipped native retains a slice ptr today, so C3 is "accidentally
  safe"; C5 (a retaining script fn) is the residual live hole. See
  `../demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md` (the 5-category escape matrix,
  probe evidence, and the full fix design) and `../demo/STRING_CONST_MODE_
  INVESTIGATION.md` (which established that the const-mode classification is
  subsumed by this fix — the owned `string`-handle path is already correct, so
  no separate const-mode feature is needed). Verified: ctest 22/22, lang
  255/0/0 (incl. 5 new `sema_invalid_*` regression tests for the closed
  categories). See `docs/spec/SPEC_AUDIT_2026-07-10.md` for F4 (the spec
  side of this — MEMORY_AND_GC §3/COMPILER_PIPELINE §4 called call-arg
  passing "perfectly fine"; the audit + this fix correct that claim).

- **Codegen optimization (gated on benchmark evidence).** ember v1 is a
  baseline JIT: a tree-walker that lowers the AST directly to x86-64 with a
  stack-spilling value convention (every value goes through rax/memory, no
  register allocation across expressions). The SSA-lite IR + linear-scan
  regalloc (COMPILER_PIPELINE §5) is the documented target — was EXPLICITLY
  DEFERRED per `../planning/DESIGN.md` §9 ("no speculative optimization before
  the bench proves it matters"), but a benchmark system (`bench/`,
  `../spec/BENCHMARK_SYSTEM_DESIGN.md`) PROVED the need per path, so the
  optimization is no longer speculative. Stage A (thin-IR backend), Stage B
  (.em v5 IR serialization), and Stage C (8 IR optimization passes) all
  SHIPPED. The full design is
  `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` (LLVM pass survey × JIT-scripting
  relevance, per-path waste mapping with line numbers, three architecture
  options, a staged recommendation, pass interface, migration plan). The
  roadmap entries below are gated, not scheduled; each carries its benchmark
  trigger.

  **Benchmark evidence (the gate, from `bench/results_codegen_paths.md`):**
  ember vs g++ -O2 (median ns ratio, the real "optimizing native" baseline):
  int_div **1.00x** (YAGNI vindicated — straight-line integer arithmetic is
  already adequate); call_overhead **5.23x** (safety off) / 6.12x (on);
  loop_overhead **5.69x** / 6.11x; slice_bounds **5.60x** / 9.15x (the bounds
  check is +54% safety-on overhead); string_decrypt **5.58x** / 6.27x;
  struct_by_value **3.00x**. So the SSA-lite IR + linear-scan regalloc is
  benchmark-PROVEN warranted on 5 of 6 paths, not speculative.

  **Recommended architecture (staged, no flag-day rewrite):**
  - **Stage 1** — DONE (2026-07-10). A peephole + per-basic-block local register
    allocator LAYERED OVER the current tree-walker (kept; added a post-emit
    peephole pass over the emitted byte buffer + a BinExpr integer-path local
    regalloc using the volatile r10 holding register, gated on an
    `expr_clobbers_r10` check). Ships behind flags (`enable_peephole`/
    `enable_local_regalloc`, default off → byte-identical to today; the 26/26
    ctest gate + 268/0/0 lang gate hold with flags off AND on — the optimizations
    are correctness-preserving). `src/peephole.{hpp,cpp}` ship the SmartImmPass (W4)
    + the inert SetccMovzxPass (W10) placeholder; `examples/codegen_opt_test.cpp`
    pins each rewrite's value-equivalence. Measured (bench/bench_codegen_paths,
    safety-off median): call_overhead -14% (1225700→1058700 ns), loop_overhead
    -15% regalloc-only (9546300→8100000 ns), slice_bounds +8% regression (the
    `mov r10/rax` reg-reg dependency chain + 6 bytes is net slower than the hot-L1
    `push/pop` store-to-load forwarding for the simple `i+1` pattern — the
    microarchitectural finding Stage 2's cost-model regalloc addresses). See
    `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §8 for the full table. Gate (the
    5-9x call/loop/slice/string slowdowns are spill-bound): CONFIRMED; Stage 1
    ships the working subset (the loop/call wins; the slice regression motivates
    Stage 2).
  - **Stage 2** — **Stage A SHIPPED (2026-07-10)** as the landed Stage-2
    stepping stone: the thin three-address IR compile-time backend
    (`AST → ThinFunction → x64` via `lower_function` + `emit_x64`), behind
    `CodeGenCtx::enable_ir_backend` (default off → byte-identical tree-walker;
    on → value-equivalent, NOT byte-identical). Value-equivalent for scalar
    integer arithmetic + control flow (if/while/for/do-while/switch,
    break/continue) + recursion + division forms, gated by `thin_ir_test` +
    `thin_ir_struct` (ctest 27/27, lang 268/0/0; the CLI never sets the flag, so
    the default path is the unchanged tree-walker). Composes with
    `enable_peephole`; obf functions fall back to the tree-walker. KNOWN GAPS
    (documented as SKIP in `thin_ir_test`, Stage B/C work): slices (index +
    bounds), structs (by-value arg/return/field/reassign), strings (native +
    inline-XOR decrypt), defer-cleanup emission, fixed-array indexed store.
    `src/thin_ir.{hpp,cpp}` (the IR + stable `ThinOp` serialization boundary) +
    `src/thin_lower.{hpp,cpp}` + `src/thin_emit.{hpp,cpp}`. This was the
    foundation for Stage B (`.em` IR serialization — the security property) and
    Stage C (IR optimization passes over the `ThinFunction`). **Stage C has now
    shipped** — see the Stage C entry below (composable pass system + eight IR
    optimization passes + one obfuscation pass). The FULL Stage 2 (carrying the
    Stage 1 peephole/regalloc over as `ThinPass`es) and Stage 3 (full SSA-lite
    rename + linear-scan) remain the still-future upgrade path, gated on Stage
    A's insufficiency or cross-block evidence. See
    `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §8 (Stage A status).
  - **Stage 3** — **TODO.** Full SSA-lite rename + linear-scan regalloc
    (COMPILER_PIPELINE §5's target). Dep: Stage 2/Stage A insufficiency on a
    spill-heavy workload (CODEGEN_SPEC §5 acceptance criteria become the test
    surface). The intra-block passes (Stage C, shipped) cover most waste;
    Stage 3 is the cross-block + register-allocation upgrade.

  - **Stage C — SHIPPED (steps 1-5 + 4 additional passes, 2026-07-10/11).** The
    composable IR pass system over the `ThinFunction`, wired into `CodeGenCtx`
    and the CLI (`--passes <names>`, `EMBER_IR_PASS` bench env). Steps 1-5
    shipped 2026-07-10: step 1 the pass-system infrastructure + unit test; step
    2 the first three IR optimization passes (ConstProp, DCE, CSE); step 3 the
    pass-manager wiring into `CodeGenCtx` + CLI `--passes` + the `EMBER_IR_PASS`
    bench knob; step 4 LICM (loop-invariant code motion); step 5 the
    SubstitutionPass (MBA obfuscation, `extensions/obf/`). Four additional IR
    optimization passes then shipped 2026-07-11: store-to-load forwarding
    (`forward`), copy propagation (`copyprop`), instruction combining
    (`instcombine`), and dead-store elimination (`dse`). The tree now carries
    **eight IR optimization passes** (`constprop`, `dce`, `cse`, `licm`,
    `forward`, `copyprop`, `instcombine`, `dse` — `extensions/opt/ext_opt.cpp`)
    + **one obfuscation pass** (`subst` — `extensions/obf/ext_obf.cpp`) = nine
    total. The pass system infrastructure and all nine passes are shipped (no
    `partial` status remains — LICM and SubstitutionPass are complete). Pinned
    by `examples/ember_pass_test.cpp` (ctest target `ember_pass`) and
    `examples/ir_passes_test.cpp` (ctest target `ir_passes`). Spec/design:
    `../spec/PASS_SYSTEM_DESIGN.md`. The still-future upgrade path is Stage 3
    (full SSA-lite rename + linear-scan) and carrying the Stage 1 peephole/
    regalloc over as `ThinPass`es — gated on Stage A's insufficiency or
    cross-block evidence.

  **Ordered optimization entries (research prediction, confirmed/reordered by
  the benchmark; build first):**
  1. **R1 Register allocation** (per-block local → linear-scan) — fixes the
     BinExpr per-expression spill + the CallExpr arg stash; the most-executed
     spills. Gate: call_overhead/loop/slice/string are spill-bound (5-9x,
     confirmed). Stage 1 → 3.
  2. **R-loop LICM** (loop-invariant code motion) — fixes loop-invariant exprs
     re-evaluated per iteration (the string_decrypt re-XOR-per-use is exactly
     this). Gate: loop/string-decrypt workloads invariant-dominated (6x,
     confirmed for string_decrypt). Stage 1 → 2.
  3. **P1 Peephole** (redundant-guard trimming + `setcc;movzx`) — per-site byte
     savings on the div/overflow/bounds/call-target guards. Gate: slice_bounds
     safety-on is +54% (confirmed). Stage 1.
  4. **I1 Instruction selection** (smart immediate forms) — `IntLit`→10-byte
     `mov rax,imm64` always; cheaper forms for small imms. Stage 1.
  5. **N1 Inlining** (small leaf script fns) — fixes call dispatch overhead;
     hot-reload interaction (inline only `const`/`@noinline`-marked fns to keep
     reload correct). Stage 2.
  6. **T1 Tail-call optimization** — `fib`-style recursion stack/budget-bound.
     Stage 2.
  7. **C1/D1/B1/CSE1/R2/W9** — constant prop/folding (the missing folds: Div/
     Mod, float, comparisons), dead-code/dead-store elim, branch folding,
     block-local CSE, range propagation (bounds-check elision for induction
     vars), max-simultaneously-live temp sizing. Stage 1-2, lower predicted win.

  See `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §3 (per-path waste), §4
  (architecture), §5 (full prediction table) for the complete design; see
  `../spec/BENCHMARK_SYSTEM_DESIGN.md` + `bench/` for the harness + results.

## What will never be added (hard non-goals)

- **`goto`** - structured control only; complicates liveness, scope,
  and `defer` emission for no benefit over `break`/`continue`/`switch`.
- **C-preprocessor** - `define`/`ifdef`/`include`/`pragma`. Use
  `engine.define` + `const`/`constexpr` + modules.
- **OOP / polymorphism (classes, inheritance, vtables, interfaces,
  mixins, templates/monomorphization)** - ember is a **C-style procedural
  language**: structs + free functions, not classes/inheritance/vtables.
  This is a deliberate design decision (2026-07-11): the struct +
  free-function pattern is ergonomic for ember's embedded-scripting +
  game-modding use case, and OOP would bring managed-reference lifetime
  complexity (vtables, dynamic dispatch, single-inheritance object graphs)
  that the GC tier exists to serve for lambdas/coroutines, not for classes.
  `fn`-typed values (`&fn`) + the dispatch-table guard cover the indirect-
  call case; struct fields + free functions cover the data + behavior case.
  If a real workload needs polymorphic dispatch, a tagged-union + match
  pattern (which ember has) is the C-style answer, not virtual methods.
- **Multiple inheritance / diamond** - subsumed by the OOP non-goal above;
  no inheritance at all, single or multiple.
- **Self-hosting is NOT a non-goal** — it is the long-term north star (see
  the Self-Hosting section above). This entry existed in an earlier roadmap
  revision and is corrected here: the compiler staying C++ forever is NOT
  the plan. Work has started (`demo/compiler/` is a µ-language
  lexer/parser/evaluator written entirely in ember).

## Re-evaluation cadence

This roadmap was originally revisited after the v0.5 benchmark milestone
(`planning/DESIGN.md` Section 9) — the first point with real performance data and
real usage to tell which deferrals were actually blocking real scripts
vs. speculative. The v1.0 concurrency + Tier 2 batch (commit e5d1814)
followed exactly that re-evaluation: `enum`, first-class function refs, sync
queues, and context thread-safety all moved from tracked deferrals below
into shipped after the v0.6 integration surfaced concrete demand (a per-frame
tick loop with per-thread state, and the host↔script coordination pattern the
sync primitives serve). Going forward the same trigger-driven rule applies:
adding features driven by measured need, not speculative demand, is the
whole point of the staged plan.

## SHIPPED — first-class struct / aggregate (was HIGH PRIORITY; shipped 2026-07-10)

**Status: SHIPPED (2026-07-10).** The re-entry trigger had fired: the
2026-07-10 multi-file demo (`demo/`) and the game-sim/compiler/hot-reload/
concurrency demos surfaced this as the single biggest ergonomic blocker to
writing real ember programs — every non-trivial module fought it. The pass
closed all four concrete gaps in a single coordinated chunk (c1 struct-literal
return + struct-by-value-arg temps; c2 array literals; c3 aggregate globals),
then validated the result by marking each demo's workaround kinks RESOLVED,
documenting the four features in `spec/TYPE_SYSTEM.md` §12 and
`spec/CODEGEN_SPEC.md` §16, and re-running all four demos (game / compiler /
hot-reload / concurrency) green.

**Shipped (2026-07-10), four sub-features:**

1. **Struct-literal return** — a fn may now `return V3 { ... };` directly.
   Codegen materializes the struct literal into a compiler-hidden temp frame
   slot, then copies it through the Win64 hidden return pointer. Regression:
   `binding_abi_test` probe [2c] (`V3{1,2,3}.x+y+z==6.0`, host reads fields
   directly — non-circular). Docs: `spec/CODEGEN_SPEC.md` §16.
2. **Struct-by-value-arg temporaries** — a struct-by-value arg may now be a
   struct literal, a struct-returning fn call, or a bare local. Codegen
   materializes a general-expr struct arg into a distinct compiler-hidden
   temp frame slot (one distinct temp per arg, never reused within a call) and
   copies bytes into the arg-stash slot. `v3_dot(v3_up(), v3_up())` now works.
   Regression: `binding_abi_test` probes [2d] (`v3_dot(v3_up(),v3_up())==1.0`)
   and [2e] (nested `v3_shift(v3_up())==(1,1,0)`). Docs: `spec/CODEGEN_SPEC.md` §16.
3. **Array literals** — `[a, b, c]` is a first-class expression constructing a
   fixed array (`let arr: i64[3] = [10, 20, 30];`) or a slice (`let s: i64[] =
   [1, 2, 3];` — allocates a backing store, yields ptr/len). Declared type
   required (no inference); count and element type checked; empty `[]` rejected.
   Regression: `array_lit_test` probes [1]-[8] (fixed-array + slice construction,
   full-i64 storage pinned via the direct C read path). Docs:
   `spec/TYPE_SYSTEM.md` §12.1, `spec/CODEGEN_SPEC.md` §16.
4. **Aggregate globals** — `struct`/`array`/`slice` globals ship with typed
   per-global offsets/sizes (slices 16 bytes ptr+len, structs/arrays their full
   layout, 8-byte alignment), const-foldable struct/array/slice initializers
   baked at load, and correct `.em` round-trip (slice relative-ptr relocation).
   Regression: `aggregate_global_test` probes [1]-[8] (struct/array/slice global
   read + by-value arg/return + `.em` round-trip). Docs: `spec/TYPE_SYSTEM.md`
   §12.2, `spec/CODEGEN_SPEC.md` §16.

**Verification (the synthesizer's final gate):** full `ninja` rebuild clean
(c1+c2+c3 compile together); ctest 22/22 (was 20, +1 `array_lit`, +1
`aggregate_global`; `binding_abi` carries c1's [2c]/[2d]/[2e] probes); lang suite
241/0/0; all four demos green (game exit 1 / clean 20-tick run, compiler exit 0,
hot-reload v1→v2→v3 PASS, concurrency shape A + shape B PASS); the four headline
probes ([2c] `V3{1,2,3}`, [2d] `v3_dot(v3_up(),v3_up())`, c2 `[10,20,30]` sum,
c3 `cfg.name_id==42`) each confirmed non-circular (each fails with its fix
reverted).

---

### Historical record — what "first-class struct/aggregate" meant (the concrete gaps, now closed)

The re-entry-trigger narrative and the concrete-gaps text below are preserved
as the audit trail of what was fixed and why. Nothing here is still open; it is
the shipped contract described as it was when it was the open roadmap item.

**Priority (at time of deferral):** HIGH. The re-entry trigger had fired: the
2026-07-10 multi-file demo (`demo/`) and the game-sim/compiler/hot-reload/
concurrency demos surfaced this as the single biggest ergonomic blocker to
writing real ember programs. Every non-trivial module fights it.

**What "first-class struct/aggregate" means (the concrete gaps to close):**

1. **Aggregate global initializers** (audit M11, currently rejected in sema):
   `global cfg : Config = Config { name_id: 42, scale: 2.0f, ... };` and
   `global arr : i64[4] = [1, 2, 3, 4];`. Today globals accept only scalar
   initializers; aggregates must be initialized in a fn. Requires the
   global table to allocate typed offsets/sizes (slices need ptr+len;
   structs/arrays need their full layout) and complete init/load/store —
   the codegen work M11 flagged, not just a parser/sema relaxation.

2. **Array literals** (`[1, 2, 3]`) as expressions, for fixed arrays AND
   slice construction. Today arrays must be initialized element-by-element.

3. **Struct-literal return** — let a fn `return V3 { x:..., y:..., z:... };`
   directly. Today sema rejects this: "a return of struct must be a plain
   local variable or a call to a function with the same struct return type"
   (a Win64 hidden-pointer-ABI restriction). Workaround today: store the
   literal in a local first (`let r: V3 = V3{...}; return r;`).

4. **Struct-by-value argument temporaries** — let a struct literal or a
   struct-returning fn call be passed directly as a struct-by-value argument.
   Today sema requires "a plain local variable." Workaround: introduce a local.

   Items 3 & 4 are the Win64 hidden-pointer struct-return/arg ABI: the named
   slot IS the hidden pointer, so the compiler today requires a named local.
   The fix is to let codegen allocate a hidden temp for struct-literal returns
   and struct-by-value arg temporaries. This is a real codegen change to the
   struct ABI surface (M10-adjacent), not a parser tweak — but it's the change
   that turns ember from "verbose for any vector-math-style API" to ergonomic.

**Why HIGH (at time of deferral):** these four together are the difference
between a language where `v3_dot(v3_up(), v3_up())` and `global cfg =
Config{...}` work (every real program) and one where every struct must be
hand-shuttled through named locals. The demos proved the workarounds are
pervasive and error-prone.

---

---

## CLI tooling (Family A shipped; B shipped; C TODO)

### Family A — compute-engine CLI (no new natives; shipped/next)

- **`ember bench`** — SHIPPED (2026-07-10, commit pending). Microbenchmark
  the entry fn: warmup + N timed iterations, each under its own fresh
  checkpoint. Reports min/median/mean/p99/max/stddev/CV% + the return value +
  machine/compiler/date provenance. **Closes the 07-09 §6.1/§6.3
  benchmark-methodology gap** (was: one mean, no variance/CI, no machine
  metadata, report written as a test side-effect; bench writes to stdout on
  request only, never as a side-effect). Zero new natives.
- **`ember test`** — ✓ SHIPPED (2026-07-11). A native test runner over a
  directory of `.ember` files classified by expected outcome (mirroring the
  `tests/run_lang_tests.sh` convention). `ember test [dir]` (default
  `tests/lang/`) classifies each file: a `// expect: N` comment → RUN, expect
  exit N; `runtime_trap_*` → RUN, expect 70; `invalid_*` → parse-only,
  expect fail; `sema_invalid_*` → sema-only, expect fail; `sema_valid_*` →
  sema-only, expect OK; everything else → parse-only, expect OK. It reuses the
  per-file compile flow factored into a reusable `run_one_file` helper (with
  `sema_only`/`parse_only` modes), resets extension state between files, prints
  a `N/M passed` summary, exits non-zero on any failure, and is wired as the
  `ember_test_cli` ctest target. The bash `run_lang_tests.sh` stays as the
  `lang_suite` ctest fallback. Zero new natives.

### Family B — ember as a scripting language with real I/O (SHIPPED 2026-07-11)

✓ **SHIPPED.** The `io` extension (`extensions/io/`, `ext_io.cpp` +
`ext_io.hpp`) is registered in the CLI and provides the core I/O surface a
scripting language needs. API (11 natives): console output `print(s)`,
`println(s)`, `print_i64(n)`, `print_f64(n)`; console input `read_line() -> s`;
file I/O `file_read_bytes(path) -> array<u8> handle`, `file_write_bytes(path,
buf) -> i64`, `file_exists(path) -> i64`; path ops `path_exists(p) -> i64`,
`path_basename(p) -> s`, `path_dirname(p) -> s`. Text natives take/return
ember `string` handles; byte natives use `array<u8>` handles (ext_io calls
into ext_string + ext_array, one-directional coupling).

**Gating — ALL 11 natives are `PERM_FFI`-gated.** A module compiled without
the FFI permission bit has every I/O call site rejected at sema time (before
codegen — zero runtime cost, no bypass path; see `SAFETY_AND_SANDBOX.md` §6).
The CLI grants the bit via `--ffi` / `--allow-io`; without it the natives are
registered but not callable (security by default). This is the
deliberate-for-the-core-subset posture: the full plan ungates `print`/
`println` as output-only (no security surface), but the core subset gates
everything so a host that has not opted into I/O at all sees a uniform
permission wall. A host that wants ungated print can register its own ungated
print native (the extension is the menu, not the mandate). Two layers of
defense: (1) registration — a host that does not call `register_natives` has
NO I/O surface (a script calling `print` gets "unknown function"); (2)
permission — `PERM_FFI` even when registered. The extension provides RAW
CAPABILITY, NOT POLICY: it does not sandbox paths, restrict `read_line`
(blocking stdin), or rate-limit; a host that wants policy wraps the natives
or configures the process environment.

**Design rationale (preserved from the deferral):** this turns ember into a
usable scripting language and unlocks arbitrary CLI tools (data processing,
code-gen, templating, build helpers). It was deferred because it is a
language feature + a security decision, not just a tool — the re-entry
trigger was a demo or real use genuinely blocked on output beyond the exit
code, which fired (the `io` extension is now also listed in the Tier 0
standard addon set above). See `extensions/io/ext_io.hpp` for the full
scope/state notes and `docs/planning/plan_OS_IO_EXTENSIONS.md` for the
full-plan extension surface (directory listing + subprocess execution as
separate sub-registration functions, still future).

### Family C — ember as a unique compute module/pipeline tool (TODO)

- **`ember pipe`** — **TODO.** A dataflow pipeline runner: load several `.em`
  modules, wire their functions into a directed graph (`A.process -> B.reduce`),
  run a stream of i64 values through it, report the transformed result.
  Exercises the bundler + module linking + the array/sync extensions. "ember as
  a dataflow compute kernel" — distinct from a general script runner.
- **`ember live`** — **TODO.** A live-coding/reload runner: `--tick` +
  hot-reload watching a `.ember` file, recompiling on change, showing the tick
  output evolve. Turns the hot-reload demo into a tool.

### Standalone exe bundler — TODO

- **Standalone exe bundler** — **TODO.** Bundle a `.ember` script + the ember
  runtime + the JIT'd code into a single self-contained `.exe` that runs
  without a separate ember install. A full 793-line design exists at
  `docs/planning/plan_STANDALONE_BUNDLER.md` (the embed-the-VM + serialize-
  the-IR + stub-main approach). Unlocks ember as a distribution format for
  CLI tools and game mods. Dep: the `.em` v5 IR format (shipped) + a small
  stub `main` that loads + runs the embedded module.
