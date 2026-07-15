# ember - memory & ownership spec

Detail doc for ../planning/DESIGN.md Section 7. Frame ownership, host-object references,
string/array representation, and explicitly deferred arena/GC designs.

## 1. Ownership taxonomy (every byte a script touches falls into
exactly one of these)

1. **JIT frame-local** - locals, spill slots, struct-value temporaries
   inside a currently-executing function's stack frame
   (CODEGEN_SPEC.md Section 2). Lifetime = the function call. No script
   construct can make one of these outlive its frame (no
   address-of-local, no returning a slice into a local - see Section 3).
2. **Host-owned, script-referenced** - memory the host allocated and
   handed a `slice`/struct pointer into via a native call return or a
   host-mapped struct field (TYPE_SYSTEM.md Section 4). Lifetime is entirely
   the host's responsibility; ember never frees it, never tracks it.
   **Extension-owned handles** (`array`/`string`/`map`/`vec`/`quat`/`mat`/
   `sync`/`lifecycle`) are also in this category: an opaque i64 handle indexes
   into a host-side vector (e.g. `std::string` for the `string` extension), the
   host owns the storage, and `reset()` clears the store between runs. The
   owned `string` handle (Section 6) is the canonical example.
3. **Module-global storage** - supported scalar/handle `global` variables,
   one fixed block per compiled/loaded module. It is independent of individual
   function pages and is unchanged by the shipped single-function reload.
4. **Arena storage** (v1: unused unless a future feature needs
   script-owned dynamic allocation - see Section 5; documented now so the
   plan has an answer ready, not because v0.x milestones need it).

There is deliberately **no** category for "script-owned heap object
with unknown lifetime" - that category is what a GC would exist to
manage, and ../planning/DESIGN.md Section 1 excludes a **script-visible** GC
heap from v1. A tracing GC **core** has shipped (`src/gc.{hpp,cpp}`) and is
**wired into the engine** for lambda closure environments (#20): precise
stack-frame root maps (the shadow stack) + precise typed-global root maps +
extension trace-callback root providers + a stop-the-world write barrier
(see Section 8). The GC manages lambda env lifetimes (an env that escapes
its frame — returned / stored to a global / held in an `array`/`map` — is
traced from the frame chain / global-root descriptor / extension callback
and reaped when no root reaches it). The v1.0 language-level `new T` /
`delete expr` operators add a **managed-pointer** ownership category (see
Section 8): a `new T` yields a one-word opaque GC pointer (not a raw
pointer — no arithmetic/dereference), rooted via the frame map / global-root
descriptor while its owning slot is live, reaped when the root is gone, and
freed immediately by `delete`. Every category above
has a lifetime that's either lexically scoped (1), externally owned
(2), or engine-object-scoped (3/4) - no case requires tracing.

## 2. Stack frame ownership detail

Exact byte ranges already specified in CODEGEN_SPEC.md Section 2/Section 6. This
section states the ownership/aliasing rules on top of that layout:
- A struct-value local's frame slot is exclusively owned by that
  local for the local's entire declared scope - no other local ever
  aliases the same slot (regalloc, CODEGEN_SPEC.md Section 5, never reuses a
  slot across two simultaneously-in-scope locals; slot reuse across
  *disjoint* scopes, e.g. two different `if` branches' locals sharing
  a slot since only one branch executes, is allowed and is a normal
  frame-size optimization - safe precisely because the scopes are
  disjoint, never simultaneously live).
- Callee-saved register spill slots (CODEGEN_SPEC.md Section 6) are
  compiler-internal, never script-addressable - no language construct
  can name or take a reference to one, so there's no ownership
  question script code could even ask about them.

## 3. Why "no address-of-local" and its consequence for slices

TYPE_SYSTEM.md Section 4 already states the only way to get a slice is (1)
native call return, (2) whole-fixed-array view `arr[..]`, (3)
host-mapped struct field. Case (2) - taking a slice-view of a
*local* fixed array - is the one case where a slice's `ptr` points
into the current frame. This is safe **only** because:
- The grammar has no way to store a slice value into a location that
  outlives the frame it was taken in - slices can be passed *down*
  into calls made from the current frame (perfectly fine, callee's
  activation is nested inside the caller's lifetime) but a script
  function **cannot return a slice that views one of its own local
  fixed arrays** - this is a dedicated sema check (COMPILER_PIPELINE.md
  Section 4): if a `return` expression's static type is `slice<T>` and its
  value was constructed via a local `arr[..]` view within *this same
  function body* (tracked via a simple "provenance" tag carried on
  the typed AST during sema - not full borrow checking, just "was
  this slice value's ptr derived from a local array view with no
  intervening native call," which is decidable locally per-function),
  it's a **compile error** ("cannot return a slice viewing a local
  array - its memory does not outlive this function").
- Storing such a slice into a **global** is likewise rejected by the
  same provenance check (assigning to a `global` slice-typed variable
  from a local-array-view value inside a function body is a compile
  error, same rationale).
- Passing such a slice as an argument to a call made *from* the
  current frame is fine **for a callee that does not retain the ptr past
  the frame** (callee's frame is nested strictly inside the caller's - the
  local array is still alive for the callee's entire synchronous
  execution). The provenance tag blocks the **three Stage-1 escape routes**
  - C1 (return), C2a (global-store), and C2b (global-rooted `FieldExpr`/
  `IndexExpr` store) - for both the `ViewExpr`-over-fixed-array class and
  the `StringLit`-derived-`slice<u8>` class (Stage 1, commit `8062195`, see
  `../ROADMAP.md` "Slice-of-stack-local escape safety — STAGE 1 DONE,
  STAGE 2 DEFERRED"). **C3** (a stack-backed slice passed to a native that
  may retain the ptr) and **C5** (a stack-backed slice passed to a script
  fn / fn-handle / cross-module call that may retain it) are an **open
  hole** that is **not yet guarded** at the call-arg sites (Stage 2
  deferred): a retaining callee dangles the ptr. Closing C3/C5 needs a real
  borrow/escape analysis (propagate the localview bit through a call's
  return value, reject only at the actual escape point, and add a
  `borrows`/`retains` annotation to `NativeSig` so C3 can distinguish
  copying natives like `string_from_slice` from retaining ones). No
  shipped native retains a slice ptr today, so C3 is "accidentally safe";
  C5 (a retaining script fn) is the residual live hole. See
  `../../demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md` for the 5-category
  escape matrix.
- This is **not** general borrow-checking (TYPE_SYSTEM.md Section 4 already
  disclaims that) - it's one narrow, syntactically-decidable check
  covering the one case ember itself can introduce a dangling slice
  (`arr[..]`); slices originating from native calls (case 1/3) are
  entirely a trust boundary as already documented, no static check
  possible or attempted there.

## 4. Module-global storage

- v1 global storage supports scalar/handle/struct/fixed-array/slice globals.
  Aggregate globals (struct / fixed-array / slice) shipped 2026-07-10
  (commit `9e90cf8`): the globals block is now a **typed block** with a
  per-global `offset` and `size`, 8-byte alignment per slot. A scalar global
  occupies 8 bytes at its offset; a **slice** global occupies 16 bytes
  (the `{ptr, len}` pair); a **struct** global occupies its tightly-packed
  Ember size; a **fixed-array** global occupies `N * sizeof(T)`. Global
  load/store addresses the slot at `[globals_base + offset]` (the typed
  per-global offset), not the old flat `[globals_base + i*8]` index form.
  See `TYPE_SYSTEM.md` §12.2 for the type-system / layout contract and
  `CODEGEN_SPEC.md` §16.4 for the codegen (typed global table, load/store
  addressing, `.em` round-trip with slice relative-ptr relocation).
- Access from JIT'd code: each global's frame offset within this block
  is a compile-time constant, so a global read/write compiles to
  `mov reg, [globals_base_imm64 + offset]` - `globals_base` is an
  absolute address baked as a 64-bit immediate exactly like the
  dispatch-table base (CODEGEN_SPEC.md Section 7), same rationale (the
  block's address is stable for the `module_t`'s lifetime, never
  moves, so baking it in is safe and avoids an extra indirection a
  RIP-relative or table-lookup scheme would cost).
- **Reload and globals**: the shipped single-function reload changes only one
  dispatch slot/page and cannot change declarations, so global slots remain
  untouched. Whole-module declaration/layout migration is deferred.
- **Global initializer**: folded at **load time** by the host's
  `eval_global_initializers` (the const-initializer-folding rule,
  `TYPE_SYSTEM.md` §12.2): a struct initializer folds field-by-field, a
  fixed-array initializer element-by-element, a slice initializer
  materializes its backing store and folds `{ptr, len}`. A field/element
  initializer that is not a literal or constexpr-foldable expression folds
  to **zero** for that field/element; the rest fold normally (the same
  const-fold restriction v1 scalar globals already enforced, extended to
  aggregate fields/elements). No "run arbitrary script code at module-load
  time" concept is needed - this is the host folding const initializers into
  the typed block, not a JIT'd "init function" that has to be invoked.

## 5. Arena allocator (reserved design, not built until needed)

If/when a feature needs script-owned dynamic allocation (e.g. a
future growable-array builtin), the answer is a **bump allocator**,
not a GC:
```cpp
struct Arena {
    uint8_t* base;
    size_t   capacity;
    size_t   offset;   // bump pointer
};
void* arena_alloc(Arena*, size_t size, size_t align); // bump offset up, aligned; OOM -> runtime_error (SAFETY_AND_SANDBOX.md Section 7), not silent null
void  arena_reset(Arena*, size_t mark = 0);            // rewind offset, invalidates everything allocated after `mark`
```
- One arena per `context_t` (matching checkpoint/depth-counter scoping in
  SAFETY_AND_SANDBOX.md Section 2/Section 4; hot-reload epochs live separately
  in `HotReloadDomain`) is the proposed natural per-execution-session
  scope for all of these).
- **Reset point**: an explicit `arena_reset` called by the host between
  logical units of work (e.g. once per game frame, or once per
  `ember_call` if the host wants per-call scoping) - never automatic/
  implicit, since implicit reset timing would need the engine to know
  when script-side references are "done," which is exactly the
  problem a GC solves and arenas deliberately don't try to.
- **Dangling-after-reset risk**: identical class of trust boundary as
  Section 3's slice-lifetime discussion - a script holding an
  arena-allocated reference across a host-triggered reset is a
  use-after-free, same category as any manual-memory-management
  system (C/C++ included). If this ever becomes a real feature, the
  mitigation is a debug-mode "poison the reset range" (write a
  recognizable byte pattern into `[mark, offset)` on reset in debug
  builds only, so a stale read reliably produces garbage that's easy
  to spot rather than accidentally-plausible leftover data)  - 
  matches the existing debug/release safety-check split pattern
  (SAFETY_AND_SANDBOX.md Section 3/Section 6, CODEGEN_SPEC.md Section 10).
- This section is written now so ../planning/DESIGN.md Section 7's deferral has a
  concrete answer ready, **not** an instruction to build it as part
  of the current spec-only pass - no milestone in ../planning/DESIGN.md Section 9
  requires it yet.

## 6. String representation

There are **two** string representations in v1:

1. **`slice<u8>` (the literal / view form).** A string literal in script
   source (`"hello"` or a raw `r"""..."""`) is a `slice<u8>` — a two-word
   `{ptr, len}` value (TYPE_SYSTEM.md Section 1/Section 4). No distinct
   compile-time `string` type for this form; it IS `slice<u8>`. String literals
   are emitted as a `RipFixup`-referenced rodata blob (CODEGEN_SPEC.md Section 4)
   embedded in the *literal's containing function's* compiled code - i.e. string
   literals are function-local rodata, valid for exactly as long as that
   function's code page is alive (which, per ../HOT_RELOAD.md Section 5's guarded
   epoch retirement is at least as long as any in-flight outer call that could
   execute that function version - so a literal's lifetime is never shorter than
   any code that could reference it). A slice value derived from a literal
   follows the same "cannot escape via return/global-store from a *different*
   code page than the one holding the literal" concern as Section 3 - but since
   the literal's rodata lives in the *same* code page as the function using it,
   and returning that slice value from the function is fine (it's not escaping
   the code page, just the frame - the underlying bytes live exactly as long as
   the code that returns them), a literal-derived slice is **not** subject to
   Section 3's dangling-check on the unencrypted path; only locally-computed
   `arr[..]` views of stack data are.

2. **The owned `string` handle (the extension form).** The `string` extension
   (`extensions/string/`, `ember_ext_string`) ships a **mutable, owned `string`
   host-store type** — an **opaque i64 handle** backed by a host-side
   `std::string` (1-indexed slot into a process-wide vector of strings, same
   pattern as `ext_array`/`ext_map`). This IS a distinct type from `slice<u8>`:
   it owns its storage (the host owns the `std::string`, the handle is the
   reference), it survives across calls/frames (the host store is
   process-scoped, not frame-scoped), and it supports mutation + concatenation
   (`Add`/`Eq` operator overloads) + the `from_slice`/`from_i64`/`from_f32`/
   `from_f64`/`from_bool`/`identity`/`length`/`char_at`/`find`/`substr` natives.
   The f-string pipeline (TYPE_SYSTEM.md §11.7) lowers through
   `__fstring_to_string` to these `string_from_*` natives and produces an owned
   `string` handle as the result. A `string` handle is in ownership category 2
   (host-owned, script-referenced) — the host allocates and owns the
   `std::string`; script holds the i64 handle. The handle is not a GC object
   (the host store is a vector indexed by handle; `reset()` clears it between
   runs for determinism). See `extensions/README.md` + `extensions/string/` +
   `TYPE_SYSTEM.md` §11.7.

The earlier spec claim "no distinct `string` type, no owned/heap string type in
v1" is **superseded**: the owned `string` handle ships as an extension type
(category 2, host-owned). The `slice<u8>` literal/view form (category 1 or
rodata-derived) still exists and is the compile-time representation of string
literals; the two are siblings, not the same thing. (A raw `slice<u8>` literal
that needs to outlive the expression is copied into an owned `string` handle
via `string_from_slice` — the handle owns the only persistent copy.)

There are now **two** string-literal lowering paths for the `slice<u8>` form
(the 2026-07-10 inline-stack-XOR string-encryption lowering, commit `e98dc87`;
see `../ROADMAP.md` "Runtime string encryption — DONE"):
- **Unencrypted path (`Program::string_xor_key == 0`):** the literal's
  bytes are function-local rodata as described above - a raw rodata
  pointer, lifetime = the code page. A `slice<u8>` derived from it is
  exempt from Section 3's dangling-check (the bytes outlive any
  frame).
- **Encrypted path (`Program::string_xor_key != 0`, the CLI default is
  `0xA5`, ON by default):** the encrypted bytes still live in rodata,
  but the *plaintext* the slice points at is a **transient stack temp**
  - the encrypted literal is decrypted inline into a compiler-hidden
  temp frame slot at each use site (codegen's StringLit eval case /
  `alloc_str_temp` / `count_str_temps_block`), and the plaintext lives
  only on the caller's stack frame for the expression's lifetime and
  is reclaimed at frame teardown. A `slice<u8>` derived from such an
  encrypted literal IS subject to Section 3's dangling-check: the
  Stage-1 slice-escape fix (commit `8062195`) covers it via
  `is_local_array_view` returning true for a `StringLit` whose
  resolved type is `slice<u8>`, so C1 (return), C2a (global-store), and
  C2b (global-rooted field/element store) are all guarded. See
  `../../demo/STRING_ENCRYPTION_ANALYSIS.md` for the original analysis
  + the probe that demonstrated the old heap-residency leak.

## 7. Array representations recap (cross-reference, no new rules)

Already fully specified in TYPE_SYSTEM.md Section 3/Section 4 - restated here only
to confirm the ownership story is consistent: fixed arrays are
frame-local (category 1) or embedded fields of a host/global struct
(category 2/3, following whatever that struct's own category is);
slices never own memory (category 2 pointer, or category-1-derived
via the checked `arr[..]` path in Section 3, or category-6 rodata-derived).
No array representation in this language ever implies heap ownership
 -  consistent with "no script-managed heap" (../planning/DESIGN.md Section 7) holding all
the way down.

## 8. GC status - core shipped + precise root scanning wired into codegen

> **Status update (2026-07-15):** the tracing mark-sweep **GC core**
> (`src/gc.{hpp,cpp}`, `ember::gc::GcHeap` — `alloc`/`collect`/`add_root`/
> `remove_root`, precise root scanning via `RefMap`, mark-sweep, stats) is
> shipped AND **wired into the engine** with precise stack + global root
> scanning + extension trace-callback root providers + a stop-the-world write
> barrier. This is no longer "core shipped, integration deferred" — the
> integration is done. The pieces:
>
> **Precise stack root maps (the shadow stack).** Both backends (the
> tree-walker `src/codegen.cpp` + the Thin IR `src/thin_lower.cpp`/
> `src/thin_emit.cpp`) emit, when `CodeGenCtx::use_gc_env` is set, a
> per-function compile-time `GcFrameMap` (`src/gc_roots.hpp`) listing the
> rbp-relative byte offsets of every frame slot that holds a GC object
> pointer (a lambda env_ptr temp, the env_ptr half of a lambda-valued
> local/param at offset+8). At runtime each active JIT frame links a
> 24-byte `GcFrameRecord` (prev / frame_base / map) onto a singly-linked
> chain whose head lives in `context_t::gc_frame_head`; the prologue links,
> the epilogue unlinks, and a catch entry restores the head to the catching
> frame's record after a cross-frame throw (so abandoned frames' stale
> records are not walked). `context_t::reset_for_call()` clears the head
> after a host trap longjmp (the abandoned frames' epilogues never ran). The
> collector walks the chain + reports each mapped slot's value as a root.
> This is NOT conservative stack scanning — it is exact: only the mapped GC
> pointer slots are read, and their offsets are a compile-time fact.
>
> **Precise global root maps.** The typed globals block (`Section 4`) carries
> a host-built `GcGlobalRoots` descriptor (`src/gc_roots.hpp`) listing the
> byte offsets of every globals-block word that holds a GC object pointer
> (notably the env_ptr half of a lambda-typed global at offset+8). The host
> (CLI / harness) builds it from `GlobalsBlock` + attaches it to
> `context_t::gc_global_roots` before the call; the collector reports each
> `*(base + off)` as a root. Replacing/clearing the global removes the only
> root -> the old env is reclaimed on the next collect.
>
> **Extension root callbacks (external root providers).** The c1 trace-
> callback facade (`extensions/gc/ext_gc.{hpp,cpp}`) lets an extension
> register ONE callback that reports its owned GC-pointer-bearing storage as
> roots during `collect()` — without exposing its internals. The
> `array`/`map` host-store extensions register callbacks (idempotent:
> unregister-stale + register on the first pointer-capable mutation, only
> when the GC runtime is initialized on the thread) that walk their stores
> under their `g_store_mutex` + report each aligned `i64` slot / each map
> entry's key AND value as root candidates; the heap visitor validates each
> (live GC object?) + rejects ordinary integers, null, and stale addresses,
> so a plain-integer slot or a `u8` array NEVER creates a false root. The
> `vec`/`string` extensions register no-op LEAF callbacks (report nothing)
> so their float/char bytes are never reinterpreted as pointers. Each
> extension's `reset()` unregisters its callback; `gc_reset()` invalidates
> every token; the next mutation re-registers. The callback cannot outlive
> the store or the runtime.
>
> **Stop-the-world write-barrier semantics.** `gc_write_barrier(owner,
> child)` records that `owner` (a GC object) now references `child` (another
> GC object). The current **stop-the-world, non-generational** collector does
> NOT need a remembered set, so the barrier is a **behavioural no-op on
> collection itself** — it does not change reachability and is not required
> for correctness today. It exists as the **observability/extension surface**
> a future generational or concurrent collector would build on: a registered
> **barrier observer** is notified of every valid (live owner + live child)
> barrier event, and `stats().barrier_calls` counts them. Null / non-live
> owner or child are SAFELY IGNORED (no observer fire, no counter bump), so
> the barrier is safe to call unconditionally from generated write sites.
> Generated code (both backends) emits `__ember_gc_write_barrier` calls at
> the env capture-store sites when `use_gc_env`; the `array`/`map` extensions
> call the facade on `set_i64`/`map_set`. For today's scalar captures +
> non-GC extension owners those calls are no-ops (owner or child is not a
> live GC pair), zero-cost when GC is disabled (`gc_active()` false). The
> barrier is the surface, not a current correctness requirement.
>
> **Lifetime model.** A lambda env is NO LONGER permanently pinned at alloc
> (`gc_alloc_env` allocates without pinning; a collect-then-alloc closes the
> alloc-to-first-rooted-store window). Reachability is determined by the
> frame chain (while the owning frame is live) / the global-root descriptor
> (if stored to a typed global) / extension trace callbacks (if stored in
> `array`/`map`) / an explicit pin (`gc_root_env`, still used by the
> script-visible `gc_new`/`gc_delete` surface — a script-held `i64` handle is
> typed `i64` to the compiler so the frame map cannot track it). An env is
> reaped once none of those reach it. Pinned by `gc_core` (the core unit
> test), `gc_integration` (Parts A–G: host API, codegen, new/delete, precise
> root scanning, IR backend, try/catch safety, extension trace callbacks),
> and `gc_full` (by-reference capture + new/delete + the cross-layer
> one-collection-sequence covering all five behaviors on both backends).
>
> The residual deferral is now narrower: (1) the IR backend does not yet
> lower `LambdaExpr` (a lambda-creating function falls back to the tree-
> walker — the IR path's frame-record maintenance + frame-map emit are
> implemented + tested via the `gc_new`/`delete` surface, ready for when it
> does); (2) `new Type{...}` syntax with typed field access (a pointer-type
> system; ember structs are value types today) — the `gc_new`/`gc_delete`
> natives are the heap-management substrate it would build on; (3) coroutine
> suspended-frame rooting (#21); (4) per-context shared heaps. See
> `extensions/gc/ext_gc.hpp` (the design + WHAT REMAINS) + `../ROADMAP.md`
> Tier 3 (Tracing GC — ✓ core shipped + integration shipped).
>
> **Language-level `new`/`delete` managed pointers (v1.0).** The `new T`
> operator allocates zero-initialized GC memory for a sized type `T` (a
> scalar, a fixed array `T[N]`, or a registered by-value struct) and yields a
> **managed pointer** — a one-word opaque GC pointer, NOT a raw pointer.
> There is no pointer arithmetic, no dereference syntax, and no cast to/from
> `i64`; the managed pointer is a distinct compiler-recognized type
> (`Type::is_managed_ptr`) carrying its pointee. `delete expr` accepts ONLY a
> managed pointer and invokes **deterministic destruction** (the object is
> freed immediately via `__ember_gc_delete_object` -> `GcHeap::free_object`,
> not just unpinned) — `gc_live()` drops by one with no `gc_collect()` needed.
> Repeated `delete` of the same pointer is a safe no-op (the native returns 0
> for an already-freed / null / non-live pointer). The operators lower to the
> internal natives `__ember_gc_alloc_object(size)` / `__ember_gc_delete_object(ptr)`
> (distinct from the legacy pinned `gc_new`/`gc_delete` surface). Both the
> tree-walker (`src/codegen.cpp`) and the Thin IR backend (`src/thin_lower.cpp`/
> `src/thin_emit.cpp`) lower them through the existing native-call paths
> (CallNative ThinOp for the IR backend), so optimized programs using `new`/
> `delete` stay on the IR backend. Managed-pointer locals, parameters, hidden
> expression temporaries, return staging slots, and supported globals are
> listed in the precise GC root maps (a managed-pointer slot is a one-word root
> at offset+0); a rooted hidden temporary is reserved for each `new` so a
> later argument/allocation cannot collect a just-created pointer before its
> destination store. `new` and `delete` are forbidden in `@realtime` functions
> (GC/heap allocation category). Sema rejects `new void` / `new T[]` (unsized
> types) and `delete <non-managed-value>`. Run `new`/`delete` tests with
> `--gc-env` so the precise frame-root scanning is active (the GC heap is
> always initialized; `--gc-env` enables the frame-record + root-map emit so a
> `new`'d object survives in-frame `gc_collect()` while its managed-pointer
> local is live).

Recorded here so the reasoning isn't lost: a tracing GC would be
needed only if script code could create heap references with
lifetimes that (a) outlive the frame that created them, (b) aren't
owned by the host, and (c) have statically-unknowable extent (so an
arena reset point can't be chosen safely). No feature in the v1
language surface (TYPE_SYSTEM.md, ../planning/DESIGN.md Section 2) creates that
situation - every reference type is either host-owned, frame-scoped,
or global/module-scoped. A GC becomes necessary only if a future
feature explicitly wants script-side heap-allocated, reference-
counted-or-traced object graphs (e.g. script-defined linked
structures, closures capturing heap state) - at which point it's a
deliberate, scoped addition with its own design pass, not a
retrofit onto the current model. Until then, building GC
infrastructure would be speculative complexity with no consumer
(YAGNI, ../planning/DESIGN.md Section 10).
