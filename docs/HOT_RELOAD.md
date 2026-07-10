# ember - hot reload spec

Detail doc for DESIGN.md Section 6. v1 ships atomic single-function
replacement with signature validation and caller-managed, single-threaded
page retirement. Concurrent reclamation and broader reload workflows are
labeled deferred below.

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
- **`std::atomic<void*>` not plain `void*`**: slot publication uses a
  `std::memory_order_release` store and readers use
  `std::memory_order_acquire` loads. This is the actual `DispatchTable`
  contract and publishes finalized code before a caller observes its address.

## 2. Added/removed functions (deferred)

v1 `reload_function` replaces exactly one function already present in the
module. Adding functions, growing a live dispatch table, and repointing removed
functions to a trap stub are not implemented. Those operations belong to a
future whole-module reload transaction.

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
3. Atomically store the new address into `slots[fn.slot_index]` with a
   `std::memory_order_release` store. `DispatchTable::get` uses an explicit
   `std::memory_order_acquire` load.
4. Old code page is returned to the caller for retirement. The shipped path
   is single-threaded/caller-owned: free only when no call can be in flight.
   Concurrent epoch/quiescence reclamation is deferred (Section 5).
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

## 5. Old code page reclamation (deferred for concurrent hosts)

`reload_function` returns the old entry page without freeing it. In the shipped
single-threaded model, the caller owns retirement and frees that page only
after it knows no invocation can still execute it. There is no context epoch,
live-context registry, quiescence API, or background sweep in v1.

A concurrent host requires an explicit epoch/quiescence protocol before it can
reclaim old pages; implementing that protocol remains deferred. Such a host
must retain retired pages (safe but leaking) or establish its own stop-the-world
quiescent point. This section is a boundary statement, not a speculative epoch
architecture presented as shipped behavior.

## 6. Whole-module reload (deferred)

No whole-module reload API ships in v1. Consequently, transactional batch
replacement, added-function slot assignment, removed-function trap stubs, and
global-layout migration are deferred. The shipped single-function path does
not alter global declarations or storage.

## 7. Host-to-script calls through the dispatch table

- Shipped hosts resolve a stable slot index/name and invoke the current address
  from the dispatch table. The raw B1 thunks accept an entry pointer rather
  than a function name, so the host must fetch the slot again after reload and
  must not cache an old raw entry pointer.
- Annotation-discovered event handlers (`@on_tick`, `@event(...)`,
  DESIGN.md Section 2/Section 8) are looked up by name the same way - the host
  caches the *name* (or a resolved slot index if it wants to skip the
  hash lookup on a true hot path like a per-frame tick callback; a
  cached slot index remains valid across reload per the "slot indices
  never change" invariant, Section 1 - so caching the *index* is safe and
  fast, caching a raw pointer is not).
