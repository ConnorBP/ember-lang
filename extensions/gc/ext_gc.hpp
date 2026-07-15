// ext_gc.hpp - ember extension: tracing GC runtime for lambda/coroutine heap.
//
// An ember *extension* (see ember/extensions/README.md): a reusable, non-
// cheat-specific addon. This wires the standalone ember::gc::GcHeap core
// (src/gc.{hpp,cpp}) into the engine as a context-owned, shareable managed
// heap for lambda closure environments (#20) and (future) coroutine suspended
// frames (#21). It mirrors ext_coroutine's host-setup shape: a host-owned
// store behind opaque i64 handles, register_natives/init/reset.
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
// Because a freshly-allocated env is NO LONGER permanently pinned (the
// precise root-scanning model replaced the permanent-pin workaround), the
// auto-collect that fires when the heap reaches its threshold is SAFE because
// the threshold check runs BEFORE the alloc (collect-then-alloc), so the new
// env is the newest object + the next collect is at the next alloc, by which
// time the env is stored into a rooted frame slot (the frame map lists it) or
// a typed global (the global-root descriptor lists it) or an array/map entry
// (the extension trace callback reports it). An env is reaped once none of
// the frame chain / global-root descriptor / extension callbacks / explicit
// pin reaches it. The explicit pin layer (gc_root_env) is still used by the
// script-visible gc_new/gc_delete surface (a script-held i64 handle is typed
// i64 to the compiler, so the frame map cannot track it). See WHAT REMAINS
// below for the precise root-scanning integration (DONE).
//
// === COLLECTION TRIGGER ===================================================
//
// Before every gc_alloc_env, if live_objects >= gc_threshold, gc_collect runs
// automatically (the threshold is host-set via gc_set_threshold; default
// 1024). Collect-then-alloc (the check runs before the alloc, never after)
// closes the auto-collect window between the alloc and the first rooted store
// — the new env is never the subject of the collect that fires at its own
// allocation. The host may also call gc_collect explicitly at safe points
// (between runs, at coroutine suspension, etc.).
//
// === NATIVES (so JIT'd code can allocate/collect) =========================
//
//   __ember_gc_alloc_env(i64 size) -> i64 ptr   allocate a lambda env (NOT
//                                              pinned; rooted via the frame
//                                              chain / global-root descriptor)
//   __ember_gc_collect()            -> i64 freed run a collection cycle
//   __ember_gc_live()               -> i64 count live object count
//   __ember_gc_write_barrier(o,c)   -> i64 0     record a GC-to-GC edge (the
//                                              stop-the-world write barrier;
//                                              no-op for null/non-live pairs)
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
//   --- deterministic new/delete substrate (language `new`/`delete` natives) ---
//   __ember_gc_alloc_object(i64 size)  -> i64 ptr   allocate an UNPINNED raw
//                                                  GC heap object (the `new`
//                                                  substrate; same thread-local
//                                                  heap + collect-before-alloc
//                                                  threshold safety as
//                                                  __ember_gc_alloc_env)
//   __ember_gc_delete_object(i64 ptr) -> i64 ok    DETERMINISTIC immediate free
//                                                  via GcHeap::free_object (the
//                                                  `delete` substrate; frees
//                                                  NOW, not just unpin). 1 on
//                                                  success, 0 if null / not
//                                                  live / already freed.
//
// The __ember_gc_alloc_object / __ember_gc_delete_object natives are the
// STABLE CONTRACT the compiler will lower the future language `new`/`delete`
// operators to. They are distinct from the legacy gc_new/gc_delete: gc_new
// allocs + PINS (the script-held handle is typed i64, so the frame map cannot
// track it) and gc_delete UNPINS (marks collectable for a future collect);
// __ember_gc_alloc_object allocs UNPINNED and __ember_gc_delete_object frees
// IMMEDIATELY (deterministic destruction, not unpinning). The legacy surface
// is preserved so current tests do not regress.
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
// === WHAT REMAINS (beyond the shipped precise root-scanning integration) ===
//
// 0. DONE (full GC integration + precise root scanning):
//    * By-ref capture: `fn[&x, &y]` capture-list syntax (parser + sema +
//      codegen). A by-ref capture's env slot holds a POINTER to the captured
//      variable's storage (not a copy), so post-capture mutations are visible
//      + body writes mutate the original. Works on BOTH the GC heap env path
//      (use_gc_env) and the default stack-env path. Nested/transitive by-ref
//      captures copy the pointer through the enclosing env. See
//      tests/lang/valid_gc_by_ref{,_write}.ember + gc_full_test Part (a).
//    * new/delete: script-visible gc_new(size)/gc_delete(ptr)/gc_collect()/
//      gc_live() natives. gc_new alloc+pins (a script-held i64 handle is typed
//      i64 to the compiler, so the frame map cannot track it — the legacy pin
//      layer roots it); gc_delete unroots (collectable); gc_collect reaps
//      unreachable; gc_live reports. See gc_full_test Parts (b)/(c). `new
//      Type{...}` syntax with typed field access needs a pointer type system
//      (ember structs are value types today) — the natives are the heap-
//      management substrate that follow-up would build on.
//    * Deterministic new/delete substrate (DONE): GcHeap::free_object frees a
//      single live object IMMEDIATELY (bypassing the collector) — the runtime
//      primitive for the future language `delete` operator. Exposed as the
//      internal natives __ember_gc_alloc_object (unpinned alloc, the `new`
//      substrate) + __ember_gc_delete_object (immediate free, the `delete`
//      substrate) — the stable contract the compiler will lower `new`/`delete`
//      to. These are DISTINCT from the legacy gc_new/gc_delete: gc_delete only
//      UNPINS (marks collectable for a future collect), while
//      __ember_gc_delete_object frees NOW (deterministic destruction). The
//      language frontend (`new`/`delete` syntax + typed pointer handling) is a
//      follow-up; the runtime substrate + native contract ship here. See
//      gc_test [14]-[19] (free_object direct) + gc_integration_test Part H
//      (extension host API).
//    * Precise root scanning (DONE): gc_alloc_env NO LONGER pins. Reachability
//      is determined by the JIT'd active-frame chain (the shadow stack: each
//      active frame links a GcFrameRecord onto context_t::gc_frame_head; the
//      collector walks the chain + reports each frame's mapped GC-pointer
//      slots) + the typed-global root descriptor (context_t::gc_global_roots:
//      the env_ptr half of every lambda-typed global) + extension trace
//      callbacks (array/map report their i64 slots/entries) + the explicit
//      pin layer (gc_root_env, for script-held gc_new handles). An env is
//      reaped once none of those reach it. The frame-chain + global-root
//      trace callback is registered by gc_attach_context. See gc_full_test
//      Part X (the cross-layer one-collection-sequence proof) +
//      gc_integration_test Parts D/E/F (precise scanning, IR backend, try/
//      catch safety) + G (extension trace callbacks). This SUPERSEDES the
//      old "permanent pin at alloc" workaround — item 1 below is DONE.
//
// 1. DONE: JIT'd root tracking. Both backends (tree-walker codegen + Thin IR
//    thin_lower/thin_emit) emit the frame-record prologue/epilogue maintenance
//    (link/unlink onto gc_frame_head) + build the GcFrameMap from the precise
//    GC-pointer frame slots. A frame-local env is reaped automatically when
//    its frame returns (the record is unlinked); an escaping env (returned /
//    stored to a global / held in an array/map) stays reachable via the
//    global-root descriptor / extension callback. No escape analysis needed
//    — the frame chain IS the root set, and escaping envs are rooted through
//    the global/extension path. The catch entry restores the head to the
//    catching frame's record after a cross-frame throw (stale records
//    unlinked); reset_for_call() clears the head after a host trap longjmp.
//
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
// 4. DONE (concurrent-entry integration): the GcHeap is now context-OWNED +
//    SHARABLE. gc_attach_context attaches a context to a shared GcRuntime (a
//    GcHeap + pin layer + execution-participant registry) whose lifetime is
//    held by shared_ptr across every attached thread; a spawned worker joins
//    the SAME runtime via gc_thread_enter (capturing the host context's opaque
//    gc_runtime back-pointer) so all workers + the host allocate into ONE heap
//    visible from any participant after the workers join. The cooperative
//    stop-the-world collector (gc_collect) blocks new entries, waits for every
//    other active participant to park at a GC safepoint or exit, scans every
//    participant's per-thread gc_frame_head plus the shared immutable global
//    roots, collects under the synchronized GcHeap, then resumes the parked
//    participants. Callers WITHOUT an attached context still get a private
//    thread-local runtime (the inert fallback), so non-GC + single-threaded
//    hosts are byte-identical to the pre-shared-heap behavior.

#pragma once
#include "sema.hpp"        // NativeSig
#include "context.hpp"     // context_t (precise root scanning: gc_attach_context)
#include "gc.hpp"          // ember::gc::GcHeap, RefMap
#include "gc_roots.hpp"    // ember::gc::GcFrameRecord, GcGlobalRoots
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
// captures are scalars; see WHAT REMAINS #2). Does NOT pin: reachability is
// determined by the JIT'd active-frame chain (the shadow stack) / the typed-
// global root descriptor / extension trace callbacks / an explicit pin
// (gc_root_env). The caller's generated code stores the returned ptr into a
// rooted frame slot IMMEDIATELY after the alloc returns (before any other
// allocation), so the env is reachable by the next collect. Returns the env
// pointer (as an i64 the JIT'd code stores in the lambda value's env_ptr
// half). Returns 0 on allocation failure.
int64_t gc_alloc_env(int64_t size);

// Pin an already-allocated env (idempotent). Returns true if the env is pinned
// after the call (false if ptr is not a live GC object).
bool gc_root_env(int64_t ptr);

// Unpin an env (it becomes collectable). Returns true if the env was pinned
// and is now unpinned (false if it was not pinned / not live).
bool gc_unroot_env(int64_t ptr);

// Run a mark-sweep collection cycle. Roots come from the frame chain + the
// global-root descriptor + extension trace callbacks + explicit pin slots;
// everything unreachable is swept. Returns the number of objects freed this
// cycle.
int64_t gc_collect();

// Live object count (the GcHeap's stats().live_objects).
int64_t gc_live_count();

// Set the auto-collect threshold (live_objects at which an alloc triggers a
// collect-then-alloc). 0 disables auto-collect. Default 1024.
void gc_set_threshold(int64_t n);

// ===========================================================================
// Deterministic new/delete: unpinned object allocation + immediate destruction.
//
// These are the runtime substrate for the future language-level `new` and
// `delete` operators, exposed as the internal native bindings
// __ember_gc_alloc_object / __ember_gc_delete_object (the stable contract the
// compiler will lower `new`/`delete` to). They are SEPARATE from the legacy
// script-visible gc_new/gc_delete surface:
//
//   gc_new / gc_delete    — the COMPATIBILITY surface. gc_new allocs + PINS
//                           (a script-held i64 handle is typed i64, so the
//                           frame map cannot track it); gc_delete UNPINS
//                           (marks the object collectable for a future
//                           collect()). Preserved as-is so current tests do
//                           not regress.
//
//   gc_alloc_object       — UNPINNED allocation (the `new` substrate). Uses
//                           the same thread-local heap + collect-before-
//                           allocate threshold safety as gc_alloc_env. The
//                           object is NOT pinned; reachability is determined
//                           by the frame chain / global roots / extension
//                           callbacks / an explicit pin (gc_root_env).
//
//   gc_delete_object      — DETERMINISTIC immediate destruction (the `delete`
//                           substrate). Calls GcHeap::free_object: validates
//                           the pointer is an exact live GC object, removes +
//                           frees it IMMEDIATELY, decrements live counts/bytes,
//                           increments freed_objects. This is NOT unpinning —
//                           the object is gone right now, not just collectable.
//
// The distinction matters: unpinning (gc_delete) is a HINT to the collector
// ("you may reap this when nothing reaches it"); deterministic destruction
// (gc_delete_object) is an IMMEDIATE free ("free this NOW"). The future
// language `delete` operator uses __ember_gc_delete_object; a managed-only
// mode would use gc_delete (unpin + let the collector decide).
// ===========================================================================

// Allocate `size` user bytes on the GcHeap as an UNPINNED raw object (the `new`
// substrate). Same thread-local heap + collect-before-allocate threshold safety
// as gc_alloc_env. NOT pinned (reachability via the frame chain / global roots
// / extension callbacks / an explicit pin). Returns the object pointer as an
// i64 (0 on failure / bad size).
int64_t gc_alloc_object(int64_t size);

// Immediately + deterministically free a GC object (the `delete` substrate).
// Calls GcHeap::free_object: validates `ptr` is an exact live GC user pointer,
// removes + frees the allocation immediately, decrements live counts/bytes,
// increments freed_objects. Returns 1 on success, 0 if `ptr` is null / not a
// live GC object / already freed. Stale root/edge values referencing the freed
// object are safely ignored by the collector's liveness filter. DIFFERENT from
// gc_delete (which only unpins, leaving the object collectable for a future
// collect()).
int64_t gc_delete_object(int64_t ptr);

// True iff ptr points at a currently-live GC object (env) on this thread's
// heap. Test helper.
bool gc_is_live(int64_t ptr);

// Cumulative freed count (stats().freed_objects). Test helper.
int64_t gc_freed_count();

// Cumulative write-barrier call count (stats().barrier_calls) — valid (live
// owner + live child) barrier events. Test helper for verifying generated
// code emits barrier calls at GC-child-into-GC-object store sites.
int64_t gc_barrier_count();

// ===========================================================================
// Thread-runtime facade: trace-callback + write-barrier access for OTHER
// extensions.
//
// The GcRuntime (the per-thread GcHeap + pin layer) is an INTERNAL detail of
// this extension (defined in ext_gc.cpp, never exposed in the header). Other
// extensions (e.g. a coroutine extension that roots suspended frames, or a
// future host root-set provider) need to (a) register trace callbacks that
// report extension-owned roots into the collector and (b) invoke the write
// barrier when they install an inter-object edge — WITHOUT reaching into
// GcRuntime or holding a GcHeap pointer themselves. These facade functions
// forward to the current thread-local heap (the same one gc_alloc_env/gc_collect
// operate on), so a caller that has called gc_init() on its thread gets the
// right heap automatically.
//
// The types (gc::GcTraceFn / gc::GcTraceVisitor / gc::GcTraceToken /
// gc::GcBarrierToken) come from src/gc.hpp, which this header already includes,
// so callers get them transitively. The tokens are stable per-heap; keep them
// to unregister. gc_reset() (which calls GcHeap::clear()) invalidates every
// outstanding token for this thread's heap.
// ===========================================================================

// Register a trace callback on the current thread-local heap. The callback is
// invoked during gc_collect() (and auto-collects) and reports candidate GC
// object pointers via the visitor, which feeds valid live candidates into the
// same mark worklist as explicit roots + RefMap edges. Returns a stable token,
// or 0 if there is no thread-local heap yet / fn is null. The caller owns
// user_data; gc_reset() and unregister drop the registration without touching
// it (no dangling user data).
gc::GcTraceToken gc_register_trace_callback(void* user_data, gc::GcTraceFn fn);

// Unregister a trace callback on the current thread-local heap. Returns true
// if a callback was removed, false if the token is invalid / already removed /
// there is no thread-local heap.
bool gc_unregister_trace_callback(gc::GcTraceToken token);

// Invoke the write barrier on the current thread-local heap: records that
// `owner` (a GC object's user-byte pointer) now references `child` (another GC
// object's user-byte pointer). Safely ignores null / non-live values. The
// current non-generational collector does not need a remembered set, so this is
// observable only via a registered barrier observer + stats().barrier_calls —
// the extension surface a future generational/concurrent collector would use.
// No-op (returns) if there is no thread-local heap yet.
void gc_write_barrier(void* owner, void* child);

// Register / unregister a barrier observer on the current thread-local heap.
// The observer is fired for every valid (live owner + live child) write-barrier
// event. Returns a stable token, or 0 if there is no thread-local heap yet /
// obs is null.
gc::GcBarrierToken gc_register_barrier_observer(void* user_data,
                                                 gc::GcBarrierObserver obs);
bool gc_unregister_barrier_observer(gc::GcBarrierToken token);

// True iff a thread-local GC runtime (GcHeap) has been created on the calling
// thread -- i.e. gc_init() has run OR a prior facade call lazily created one.
// This is the NON-CREATING existence probe: unlike gc_register_trace_callback /
// gc_write_barrier (which lazily create the runtime via the internal rt()
// helper), this returns false WITHOUT creating one. Other extensions use it to
// skip ALL GC work (trace-callback registration + write barriers) when the host
// never initialized the GC on this thread, preserving byte-identical non-GC
// operation on threads that do not use the collector (the c1 integration
// contract: the host-store extensions are inert when the GC extension is
// inactive). Returns false when the GC extension is effectively absent on this
// thread.
bool gc_runtime_initialized();

// ===========================================================================
// Precise root scanning: context attachment (shadow stack + global roots) +
// the cooperative stop-the-world participant protocol.
//
// The JIT'd code maintains a linked active-frame record chain (the "shadow
// stack") whose head lives in context_t::gc_frame_head, and each frame's
// compile-time GC-slot map is baked into its prologue. For the collector to
// SEE those roots, a trace callback must walk the chain + the shared global-
// root descriptor and report each mapped slot's value.
//
// gc_attach_context attaches `ctx` to a shared GcRuntime (a GcHeap + pin layer
// + execution-participant registry), creating the runtime on demand, registers
// `ctx` as a PARTICIPANT on that runtime, installs the shared `global_roots`
// descriptor on the runtime (and on ctx.gc_global_roots), and records the
// runtime's opaque pointer on ctx.gc_runtime so a spawned worker can capture
// it. The runtime's trace callback (registered once at creation) walks every
// registered participant's per-thread gc_frame_head + the shared immutable
// global roots on each collect. gc_attach_context must be called BEFORE
// running JIT'd code that may trigger a collection. gc_detach_context
// unregisters the participant + clears ctx.gc_runtime so the runtime never
// holds a dangling &ctx. gc_reset() clears the active runtime's heap +
// participant list; calling detach after reset is a safe no-op.
//
// gc_thread_enter / gc_thread_exit are the WORKER-thread entry/exit pair: a
// spawned OS thread (created by the thread extension) calls gc_thread_enter
// with the host context's opaque gc_runtime back-pointer + the worker's OWN
// per-call context_t before entering JIT'd code, so the worker joins the SAME
// shared heap + registers its own shadow-stack head as a participant; it calls
// gc_thread_exit on every normal + trapped exit so no abandoned participant
// record remains registered. The collection natives obtain every participant's
// roots via the runtime trace callback (no per-context pointer is passed to
// them). A host that never attaches a context gets the inert thread-local
// fallback (explicit pin roots only); a host that attaches gets precise stack
// + global root scanning across every participant on every collect.
// ===========================================================================

// Attach a context (with its global-root descriptor) to a shared GcRuntime for
// precise root scanning + cooperative stop-the-world participation. Creates
// the runtime on demand, registers `ctx` as a participant, installs the shared
// `global_roots` descriptor, and records the runtime's opaque pointer on
// ctx.gc_runtime. `global_roots` may be nullptr (stack-only scanning).
// Idempotent re-attach on the SAME context updates the global-roots pointer
// without double-registering; re-attach on a DIFFERENT context detaches the
// previous one first. Returns true on success, false if `ctx` is null.
bool gc_attach_context(context_t* ctx, gc::GcGlobalRoots* global_roots);

// Detach the context from its shared GcRuntime (unregister the participant +
// clear ctx.gc_runtime). Safe to call when no context is attached (no-op).
// After this, the runtime's collector no longer scans this context's stack
// roots (only explicit pin roots + other participants' roots remain). Does NOT
// touch the context (the caller owns it).
void gc_detach_context(context_t* ctx);

// Worker-thread entry: join the shared GcRuntime identified by
// `runtime_opaque` (the host context's ctx.gc_runtime back-pointer, captured
// at thread_spawn) + register `ctx` as a participant on it, so the worker
// allocates into the SAME shared heap + its per-thread gc_frame_head is scanned
// by the cooperative stop-the-world collector. Installs the runtime's shared
// global-roots descriptor on ctx.gc_global_roots + the runtime pointer on
// ctx.gc_runtime. Sets the calling thread's active runtime to the shared one.
// Returns true on success, false if `runtime_opaque` is null/expired (the
// runtime was destroyed) or `ctx` is null — in the failure case the worker
// runs WITHOUT a shared heap (its allocations go to a private thread-local
// fallback heap, never visible from the host, so a shared-heap test would
// observe the loss; the worker still runs safely).
bool gc_thread_enter(void* runtime_opaque, context_t* ctx);

// Worker-thread exit: unregister `ctx` from its shared GcRuntime participant
// list + clear ctx.gc_runtime so no abandoned participant record remains.
// Called on EVERY worker exit (normal return AND trapped longjmp cleanup) so a
// trap cannot leave a dangling participant. If a cooperative stop-the-world
// collect is in progress on another thread, this marks the participant parked
// + waits for the collect to finish before unregistering, so the collector
// never scans a context that is being torn down. Safe to call when no
// participant is registered (no-op). Does NOT touch the context.
void gc_thread_exit(context_t* ctx);

// Cooperative-STW safepoint for a participant blocked in a NON-GC native
// (notably thread_join, which waits on a slot condition variable). A
// participant blocked in thread_join is NOT executing JIT'd code, so its
// gc_frame_head is stable -- it is a valid GC safepoint. thread_join calls
// this in its wait loop so a concurrent gc_collect's unbounded wait observes
// the joiner parked (within one poll interval) + can proceed, instead of
// waiting forever for a joiner that is itself waiting for the collector's
// worker to finish (a deadlock). No-op when no collect is in progress or this
// thread is the collector / has no participant record. This is the "another
// mechanism that guarantees all active participants quiesce": every
// participant is either at a GC-native safepoint, at this join safepoint, or
// exits -- so the collector's unbounded wait completes in bounded wall time.
void gc_park();

} // namespace ember::ext_gc
