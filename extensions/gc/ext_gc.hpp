// ext_gc.hpp - ember extension: tracing GC runtime for lambda/coroutine heap.
//
// An ember *extension* (see ember/extensions/README.md): a reusable, non-
// cheat-specific addon. This wires the standalone ember::gc::GcHeap core
// (src/gc.{hpp,cpp}) into the engine as a host-owned, process-wide managed
// heap for lambda closure environments (#20) and (future) coroutine suspended
// frames (#21). It mirrors ext_coroutine's host-setup shape: a thread-local
// host-owned store behind opaque i64 handles, register_natives/init/reset.
//
// === WHY THIS EXISTS ======================================================
//
// Before this extension, a lambda's closure env was a stack-frame-local temp
// (codegen: __envtmp$N at [rbp+env_off]), so a lambda could NOT outlive the
// frame that created it (the env_ptr became a dangling stack address the
// moment the frame returned). That is fine for the v1 lambda tests (the
// lambda is created and called within one frame), but it blocks the #20/#21
// roadmap items where a lambda/coroutine must ESCAPE its creating frame
// (return a lambda, store it in a global, hand it to a coroutine that resumes
// later). This extension provides the heap those escaping envs live on, with
// a tracing mark-sweep collector that reclaims envs nothing reaches anymore.
//
// === THE HOST-OWNED GcHeap + HANDLE-BASED ROOTS ==========================
//
// The GcHeap core (src/gc.hpp) does precise mark-sweep: collect() traces from
// a set of registered root SLOTS (void** whose pointed-to value is a GC
// object's user-byte pointer) and sweeps everything unreachable. The engine's
// JIT'd code cannot, in this v1, register/unregister those root slots at
// frame entry/exit (that needs JIT'd root-epilogue calls -- a follow-up). So
// this extension uses the GcHeap's OWN precise root mechanism through a
// host-side PIN layer:
//
//   gc_alloc_env(size) -> ptr   alloc size user bytes on the GcHeap AND pins
//                               the env (registers a stable root slot holding
//                               the ptr). A pinned env survives every
//                               auto-collect until gc_unroot_env(ptr).
//   gc_root_env(ptr)            pins an already-allocated env (idempotent).
//   gc_unroot_env(ptr)          unpins an env (it becomes collectable: a
//                               subsequent collect sweeps it if nothing else
//                               reaches it).
//   gc_collect() -> freed       runs mark-sweep; returns the freed count.
//                               Pinned envs survive; unpinned ones are swept.
//
// Because every freshly-allocated env is pinned, the auto-collect that fires
// when the heap reaches its threshold is SAFE: it never sweeps an env the
// JIT'd code is still holding. An env is only reaped once the host/script
// explicitly unpins it (the v1 escape hatch for "this env is dead now"). Full
// JIT'd root tracking (unpin at the owning frame's epilogue, so escaping
// lambdas stay pinned and frame-local ones are reaped automatically) is the
// documented remaining step -- see WHAT REMAINS below.
//
// === COLLECTION TRIGGER ===================================================
//
// After every gc_alloc_env, if live_objects >= gc_threshold, gc_collect runs
// automatically (the threshold is host-set via gc_set_threshold; default
// 1024). Auto-collect is safe because the just-allocated env is already
// pinned before the threshold check. The host may also call gc_collect
// explicitly at safe points (between runs, at coroutine suspension, etc.).
//
// === NATIVES (so JIT'd code can allocate/collect) =========================
//
//   __ember_gc_alloc_env(i64 size) -> i64 ptr   allocate + pin a lambda env
//   __ember_gc_collect()            -> i64 freed run a collection cycle
//   __ember_gc_live()               -> i64 count live object count
//
//   --- script-visible new/delete (full GC integration) ---
//   gc_new(i64 size)     -> i64 ptr   allocate + pin a raw GC heap object
//   gc_delete(i64 ptr)   -> i64 ok    unroot (mark collectable); 1 if was pinned
//   gc_collect()         -> i64 freed run a collection cycle (alias of the above)
//   gc_live()            -> i64 count live object count (alias of the above)
//
// The __ember_* natives are engine-internal (codegen's lambda-env path calls
// __ember_gc_alloc_env when CodeGenCtx::use_gc_env is set). The gc_* natives
// are the USER-FACING surface: a script can allocate raw GC heap objects
// (gc_new), release them for collection (gc_delete), and observe the heap
// (gc_collect/gc_live). This is the v1 new/delete — a raw i64 handle the
// script holds + releases, with the tracing GC reclaiming unreachable
// (unpinned) objects. `new Type{...}` syntax with typed field access needs a
// pointer-type system (ember structs are value types today); these natives
// are the heap-management substrate that follow-up would build on.
//
// codegen's lambda-env case (src/codegen.cpp) calls __ember_gc_alloc_env when
// CodeGenCtx::use_gc_env is set, instead of stack-allocating the env. The
// natives receive/return the env pointer as an i64 (Win64: a pointer is an
// i64), the same opaque-handle convention ext_array/ext_string use.
//
// === HOST SETUP ===========================================================
//
// The host calls gc_init() once before any GC-managed allocation (allocates
// the thread-local GcHeap + sets the default threshold) and gc_reset() between
// independent runs (clears the heap + roots, mirroring coroutine_reset). The
// host registers the natives via register_natives into its native table so
// codegen can resolve __ember_gc_alloc_env by name.
//
// === WHAT REMAINS (beyond this v1 wiring) =================================
//
// 0. DONE (full GC integration session): by-reference capture + new/delete.
//    * By-ref capture: `fn[&x, &y]` capture-list syntax (parser + sema +
//      codegen). A by-ref capture's env slot holds a POINTER to the captured
//      variable's storage (not a copy), so post-capture mutations are visible
//      + body writes mutate the original. Works on BOTH the GC heap env path
//      (use_gc_env) and the default stack-env path. Nested/transitive by-ref
//      captures copy the pointer through the enclosing env. See
//      tests/lang/valid_gc_by_ref{,_write}.ember + gc_full_test Part (a).
//    * new/delete: script-visible gc_new(size)/gc_delete(ptr)/gc_collect()/
//      gc_live() natives. gc_new alloc+pins; gc_delete unroots (collectable);
//      gc_collect reaps unreachable; gc_live reports. See gc_full_test Parts
//      (b)/(c). `new Type{...}` syntax with typed field access needs a pointer
//      type system (ember structs are value types today) — the natives are the
//      heap-management substrate that follow-up would build on.
//    * Root scanning (v1): the existing pin layer IS the root mechanism —
//      collect() traces from the registered root slots (every gc_alloc_env /
//      gc_new pins). This is the task's allowed v1 approach (explicit root
//      registration). The precise frame-layout approach is item 1 below.
//
// 1. JIT'd root tracking: emit __ember_gc_unroot_env calls at the owning
//    frame's epilogue for frame-local envs, so they are reaped automatically
//    on frame return (today they stay pinned until gc_reset). Escaping envs
//      (a lambda returned/stored to a global) must stay pinned -- the codegen
//    needs an escape analysis to distinguish the two. This is the step that
//    makes the GC fully automatic for the JIT'd path.
// 2. RefMap precision: today a lambda env's captures are scalars (i64/f64)
//    OR pointers to frame slots (by-ref captures). A by-ref capture's env
//    slot holds a pointer to a STACK frame slot, NOT a GC object, so it is
//    correctly NOT in the RefMap (the tracer would find is_live()==false and
//    skip it; a dangling frame-slot pointer after the frame dies is safe
//    because refmap_none means the tracer never follows it). When a capture
//    can itself be a GC OBJECT (a captured lambda/coroutine whose env is on
//    this heap), the env's RefMap must list those capture byte offsets so the
//    tracer follows them. Sema knows each capture's type; codegen would build
//    the RefMap at alloc time. (By-ref of a GC object would point into the
//    heap — that WOULD be a RefMap entry; today by-ref is scalar-frame-slot
//    only, so refmap_none stays correct.)
// 3. Coroutine suspended frames (#21): a suspended coroutine's frame holds
//    live locals (possibly GC pointers); wiring those onto this heap (with the
//    frame rooted while suspended) is the coroutine half of this task. The
//    heap + pin layer here are the foundation; the coroutine integration is a
//    follow-up because coroutines currently run on Windows-fiber stacks (the
//    frame is the fiber stack, not a heap object) and converting that needs a
//    heap-allocated frame design.
// 4. Per-context heaps: the GcHeap is thread-local today (one per OS thread),
//    mirroring context_t's per-thread model. A shared multi-context heap with
//    the GcHeap's own mutex (the core notes this as a follow-up) is a later
//    refinement for the in-context-threads addon.

#pragma once
#include "sema.hpp"        // NativeSig
#include "gc.hpp"          // ember::gc::GcHeap, RefMap
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_gc {

// Register the GC natives (__ember_gc_alloc_env/collect/live) into the host's
// native table. Called once at host startup, like every other extension's
// register_natives.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Host setup: allocate the thread-local GcHeap + set the default collection
// threshold. Idempotent (a no-op if the heap already exists for this thread).
// Must be called before any gc_alloc_env. Returns false only on allocation
// failure (the GcHeap itself is trivially constructible, so this effectively
// always returns true).
bool gc_init();

// Clear the heap + all roots between independent runs (mirrors
// ext_coroutine::coroutine_reset / ext_thread::thread_reset). Safe to call
// when the heap is empty. Does NOT destroy the thread-local GcHeap itself
// (it is reused across runs; only its contents are cleared).
void gc_reset();

// --- the host-side managed-allocation API (also exposed as natives) ---

// Allocate `size` user bytes on the GcHeap as a lambda env (refmap_none -- v1
// captures are scalars; see WHAT REMAINS #2). Pins the env (registers a stable
// root slot) so it survives auto-collects. Returns the env pointer (as an i64
// the JIT'd code stores in the lambda value's env_ptr half). Returns 0 on
// allocation failure.
int64_t gc_alloc_env(int64_t size);

// Pin an already-allocated env (idempotent). Returns true if the env is pinned
// after the call (false if ptr is not a live GC object).
bool gc_root_env(int64_t ptr);

// Unpin an env (it becomes collectable). Returns true if the env was pinned
// and is now unpinned (false if it was not pinned / not live).
bool gc_unroot_env(int64_t ptr);

// Run a mark-sweep collection cycle. Pinned envs survive; unpinned unreachable
// envs are swept. Returns the number of objects freed this cycle.
int64_t gc_collect();

// Live object count (the GcHeap's stats().live_objects).
int64_t gc_live_count();

// Set the auto-collect threshold (live_objects at which an alloc triggers a
// collect). 0 disables auto-collect. Default 1024.
void gc_set_threshold(int64_t n);

// True iff ptr points at a currently-live GC object (env) on this thread's
// heap. Test helper.
bool gc_is_live(int64_t ptr);

// Cumulative freed count (stats().freed_objects). Test helper.
int64_t gc_freed_count();

} // namespace ember::ext_gc
