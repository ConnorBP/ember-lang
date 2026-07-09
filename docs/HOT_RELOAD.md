# ember - hot reload spec

Detail doc for DESIGN.md Section 6. Dispatch table layout, reload protocol,

> **Implementation status: v0.1** - this is the v1.0 design spec. The
> current repo implements the JIT codegen proof (encoder, label/patch,
> exec-mem, `.em` format). See `README.md` for what's shipped; see
> `CODEGEN_SPEC.md` Section 12 + Section 15 for the acceptance suite. This doc's
> content is the target design, not a claim of current implementation.
reclamation, concurrency.

## 1. Dispatch table layout

```cpp
struct DispatchTable {
    std::atomic<void*>* slots;   // one per script function, index-stable
    uint32_t             count;
    uint32_t             capacity;
};
```

- One `DispatchTable` per `module_t`. `slots[i]` holds the current
  entry address of the function assigned slot `i`.
- **Slot assignment**: happens during the first sema pass that
  registers all function signatures before any body is lowered
  (COMPILER_PIPELINE.md Section 3) - slot index = registration order within
  that pass, for the module's *initial* compile. **Slot indices never
  change for the lifetime of the `module_t`**, including across
  reloads - this is the entire property that makes reload safe
  (CODEGEN_SPEC.md Section 7: callers bake in `slot*8` as part of their
  compiled code, so slot renumbering would require recompiling every
  caller, which defeats the purpose).
- **`std::atomic<void*>` not plain `void*`**: a slot write during
  reload (Section 3) must be observed atomically by any thread concurrently
  executing a `call [slot]` - relaxed-order atomic store is
  sufficient on x86-64 (aligned 8-byte stores are already atomic at
  the hardware level; the `std::atomic` wrapper is here for the C++
  memory model's sake and to stop the optimizer from doing anything
  unexpected, not because x86-64 needs extra instructions for it  - 
  `mov [slot], reg` compiles the same either way on this target).

## 2. New function added after initial compile

- Adding a brand-new function to a module (not present at initial
  compile) via reload gets the **next available slot index**
  (`count++`), growing the table if `count == capacity` (realloc to a
  new, larger `slots` array - see Section 5 for why growing is safe even
  with concurrent readers). Existing functions' slots are untouched.
- A function *removed* from source on reload: its slot is repointed
  to the shared `ember_trap_removed_function` stub (same non-returning
  trap pattern as SAFETY_AND_SANDBOX.md Section 2/Section 7) rather than freed/
  reused - slot indices are never recycled in v1 (simpler than
  tracking a free-list, and script modules realistically have at most
  a few hundred functions, so slot-table growth is not a real memory
  concern).

## 3. Reload protocol (single function)

`ember_reload_function(module_t*, const char* fn_name, const char*
new_source)`:
1. Parse+sema+lower+codegen the new function body in isolation,
   exactly like initial compile of one function (COMPILER_PIPELINE.md
   Section 3's per-function independence - this is *why* CODEGEN_SPEC.md Section 7
   mandates dispatch-table-only cross-function references: a function
   can be fully recompiled without touching any other function's
   bytes). If this fails (parse/type error in the new body), the
   reload **aborts before touching the dispatch table at all** - old
   code keeps running untouched, `ember_reload_function` returns
   `false` with error info, module is left in its previous fully-valid
   state (no partial/broken reload state is ever observable).
2. Allocate fresh exec memory for the new function body (JIT memory
   arena, MEMORY_AND_GC.md Section 5) - **never overwrite the old function's
   code bytes in place**, even though the old and new versions might
   be the same size; in-flight calls (Section 4) may still be executing the
   old bytes at the moment of the swap, so the old page must remain
   valid and unmodified until provably unreferenced.
3. Atomically store the new address into `slots[fn.slot_index]`
   (`std::memory_order_release` store; call sites load with implicit
   acquire-enough ordering since x86-64 loads are already
   acquire-ordered by the hardware - again the atomic wrapper is
   about correctness-on-paper for the C++ memory model rather than
   needing a fence instruction on this target).
4. Old code page is handed to the epoch reclamation scheme (Section 5)  - 
   not freed synchronously.
5. Host is notified via an optional reload callback (mirrors a typical
   native-JIT scripting language's `reload()` returning success/failure, DESIGN.md Section 8's
   `ember_reload_function` signature) - no separate "reload event"
   annotation mechanism needed, it's a direct return value plus an
   optional registered callback for logging/UI purposes.

## 4. In-flight calls during reload

- A call already inside the *old* version of a function when its slot
  is swapped **keeps running the old version to completion** - the
  swap only affects *future* `call [slot]` instructions, which read
  the slot fresh each time; a frame that already did its `call` and
  is executing inside the old function body has no reason to re-read
  the slot mid-execution, so it simply finishes normally using the old
  code, then returns to its caller as usual.
- A **recursive** function reloaded while it's mid-recursion on the
  call stack: each recursive call is a fresh `call [slot]`
  (CODEGEN_SPEC.md Section 7 - recursion always goes through the table, never
  a direct self-call), so a reload mid-recursion means **outer frames
  finish in the old version, but any new recursive call made after the
  swap point picks up the new version** - i.e. recursion can observe a
  version change partway through its own call tree. This is
  documented as expected/acceptable behavior (same category of
  behavior as Mun's and most hot-reload systems' "function identity
  is per-call, not per-call-tree" semantics) rather than something
  ember tries to prevent (preventing it would require either freezing
  reloads during any recursion anywhere in the module - too coarse  - 
  or version-pinning per call-tree, which needs per-frame bookkeeping
  we have no other reason to carry). If a host needs stronger
  atomicity guarantees for a specific function, the host-level
  mitigation is "don't reload while you know that function is
  probably on the stack" (a host-level policy, not an ember mechanism)
  - documented as a known, deliberate limitation, not an oversight.

## 5. Old code page reclamation (epoch scheme)

```cpp
struct RetiredCodePage {
    void*    ptr;
    size_t   size;
    uint64_t retired_epoch;
};
```
- `context_t` (not `module_t` - depth counters and checkpoints already
  live per-context, SAFETY_AND_SANDBOX.md Section 2/Section 4, so epoch tracking
  fits the same place) increments a per-context `current_epoch`
  counter each time `ember_call` is entered at the *outermost* level
  (not on nested calls - one bump per top-level host->script
  invocation is enough granularity; this is deliberately coarse, we
  are not trying to reclaim memory within microseconds of a reload,
  just "eventually, safely, without a tracing GC").
- On reload (Section 3 step 4), the retired old page is stamped with the
  module's **maximum currently-observed epoch across all live
  contexts at the moment of the swap** (module keeps a registry of
  its live `context_t*`s for exactly this read - cheap, reload is a
  rare/manual operation, not a hot path, so an O(live_contexts) scan
  here is fine).
- A background/periodic sweep (called by the host, or invoked
  automatically on the next reload - either is acceptable, v1 just
  needs *a* trigger, not necessarily automatic-on-a-timer) frees any
  `RetiredCodePage` whose `retired_epoch` is less than the current
  minimum epoch across all live contexts (standard epoch-based
  reclamation - a page retired at epoch E is safe to free once every
  context has advanced past E, since that means no context could
  still be inside a call that started before the swap).
- **Why not refcount the code page instead**: refcounting would need
  an increment on every `call [slot]` and decrement on every return  - 
  a cost on the hot path for something that only matters during the
  rare reload event. Epoch counting only costs one increment per
  *top-level* `ember_call`, which is already the coarse granularity
  the rest of the safety model (checkpoints, Section 2's non-local unwind
  scope) operates at. Consistent with "pay for reload-safety on the
  reload path, not on every call."
- **v1 simplification**: if a host never calls the sweep, retired
  pages simply accumulate (a documented, bounded-by-reload-frequency
  leak, acceptable since reload is a development-time/manual-ops
  action, not something happening thousands of times per second in
  production) - a real production deployment is expected to call the
  sweep periodically (e.g. once per game-server tick, cheap
  epoch-min computation) or on each reload; this is a host-integration
  detail documented here, not a hidden gotcha.

## 6. Whole-module reload

- `reload(module_t*, source, len, filename)` (DESIGN.md Section 8, mirrors
  a typical native-JIT scripting language's `reload`) recompiles every function in the new source in one
  batch, matching each by name against the module's existing slot
  table:
  - Name exists in both old and new source: slot repointed as in Section 3
    (per-function, but batched - all-or-nothing at the *parse/sema*
    stage: if *any* function in the new source fails to compile, the
    whole `reload()` call fails and **no slots are touched at all**,
    same "never leave a partially-broken module" guarantee as Section 3 step
    1, just scoped to the whole batch instead of one function).
  - Name exists only in old: repointed to the removed-function trap
    stub (Section 2).
  - Name exists only in new: assigned a fresh slot (Section 2).
- Globals (`set_global`/`get_global`, DESIGN.md Section 8) are **preserved
  across reload** - a reload only touches function slots and their
  associated code pages, never the module's global-variable storage
  (a separate, stable block, MEMORY_AND_GC.md Section 4) - this matches the
  "hot reload changes behavior, not state" expectation from Mun-style
  hot-reloading workflows (RESEARCH_NOTES.md).

## 7. Host-to-script calls through the dispatch table

- Host code invoking a script function by name (`ember_call`) looks
  up the slot once (by name -> index, a hash map maintained
  alongside the table, rebuilt/updated whenever a function is
  added/removed) and issues an indirect call through it - same
  mechanism as script-to-script calls (CODEGEN_SPEC.md Section 7), just
  initiated from native code instead of JIT'd code. This means
  host->script calls are automatically reload-safe with **zero extra
  work**: there's no separate "host caches a raw function pointer"
  API that could go stale - `ember_call` always re-resolves through
  the table by name on every invocation (a hash lookup per call, cheap
  relative to actually running a script function, and avoids an
  entire class of "host held a stale pointer across a reload" bugs by
  construction).
- Annotation-discovered event handlers (`@on_tick`, `@event(...)`,
  DESIGN.md Section 2/Section 8) are looked up by name the same way - the host
  caches the *name* (or a resolved slot index if it wants to skip the
  hash lookup on a true hot path like a per-frame tick callback; a
  cached slot index remains valid across reload per the "slot indices
  never change" invariant, Section 1 - so caching the *index* is safe and
  fast, caching a raw pointer is not).
