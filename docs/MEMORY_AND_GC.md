# ember - memory & ownership spec

Detail doc for DESIGN.md Section 7. Frame ownership, host-object references,
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
3. **Module-global storage** - supported scalar/handle `global` variables,
   one fixed block per compiled/loaded module. It is independent of individual
   function pages and is unchanged by the shipped single-function reload.
4. **Arena storage** (v1: unused unless a future feature needs
   script-owned dynamic allocation - see Section 5; documented now so the
   plan has an answer ready, not because v0.x milestones need it).

There is deliberately **no** category for "script-owned heap object
with unknown lifetime" - that category is what a GC would exist to
manage, and DESIGN.md Section 1 excludes a GC from v1. Every category above
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
  current frame is fine (callee's frame is nested strictly inside the
  caller's - the local array is still alive for the callee's entire
  execution) - the provenance tag only blocks the two escape routes
  (return, global-store) that could outlive the frame, not ordinary
  downward parameter passing.
- This is **not** general borrow-checking (TYPE_SYSTEM.md Section 4 already
  disclaims that) - it's one narrow, syntactically-decidable check
  covering the one case ember itself can introduce a dangling slice
  (`arr[..]`); slices originating from native calls (case 1/3) are
  entirely a trust boundary as already documented, no static check
  possible or attempted there.

## 4. Module-global storage

- v1 global storage supports scalar/handle globals only: one eight-byte slot
  per declaration. Sema rejects slice, fixed-array, and by-value struct globals;
  a typed aggregate global layout remains deferred.
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
- **Global initializer**: evaluated once at `ember_compile` time (for
  compile-time-constant initializers - the only kind allowed in v1,
  matching `auto`'s restriction to no-solver-needed cases, TYPE_SYSTEM.md
  Section 9) - no "run script code at module-load time" concept needed for
  this, keeps global init trivial (a memcpy of constant bytes into
  the block at compile time, not a JIT'd "init function" that has to
  be invoked).

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
- This section is written now so DESIGN.md Section 7's deferral has a
  concrete answer ready, **not** an instruction to build it as part
  of the current spec-only pass - no milestone in DESIGN.md Section 9
  requires it yet.

## 6. String representation

Strings are `slice<u8>` (TYPE_SYSTEM.md Section 1/Section 4) - no distinct `string`
type, no owned/heap string type in v1. A string literal in script
source (`"hello"`) is emitted as a `RipFixup`-referenced rodata blob
(CODEGEN_SPEC.md Section 4) embedded in the *literal's containing function's*
compiled code - i.e. string literals are function-local rodata, valid
for exactly as long as that function's code page is alive (which, per
HOT_RELOAD.md Section 5's guarded epoch retirement is at least as long as any
in-flight outer call that could execute that function version - so a literal's lifetime
is never shorter than any code that could reference it). A slice
value derived from a literal follows the same "cannot escape via
return/global-store from a *different* code page than the one holding
the literal" concern as Section 3 - but since the literal's rodata lives in
the *same* code page as the function using it, and returning that
slice value from the function is fine (it's not escaping the code
page, just the frame - the underlying bytes live exactly as long as
the code that returns them), literal-derived slices are **not**
subject to Section 3's dangling-check; only locally-computed `arr[..]`
views of stack data are.

## 7. Array representations recap (cross-reference, no new rules)

Already fully specified in TYPE_SYSTEM.md Section 3/Section 4 - restated here only
to confirm the ownership story is consistent: fixed arrays are
frame-local (category 1) or embedded fields of a host/global struct
(category 2/3, following whatever that struct's own category is);
slices never own memory (category 2 pointer, or category-1-derived
via the checked `arr[..]` path in Section 3, or category-6 rodata-derived).
No array representation in this language ever implies heap ownership
 -  consistent with "no script-managed heap" (DESIGN.md Section 7) holding all
the way down.

## 8. v2 GC deferral - explicit rationale (not just "later")

Recorded here so the reasoning isn't lost: a tracing GC would be
needed only if script code could create heap references with
lifetimes that (a) outlive the frame that created them, (b) aren't
owned by the host, and (c) have statically-unknowable extent (so an
arena reset point can't be chosen safely). No feature in the v1
language surface (TYPE_SYSTEM.md, DESIGN.md Section 2) creates that
situation - every reference type is either host-owned, frame-scoped,
or global/module-scoped. A GC becomes necessary only if a future
feature explicitly wants script-side heap-allocated, reference-
counted-or-traced object graphs (e.g. script-defined linked
structures, closures capturing heap state) - at which point it's a
deliberate, scoped addition with its own design pass, not a
retrofit onto the current model. Until then, building GC
infrastructure would be speculative complexity with no consumer
(YAGNI, DESIGN.md Section 10).
