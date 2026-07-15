// ext_gc.cpp - ember extension: tracing GC runtime for lambda/coroutine heap.
// See ext_gc.hpp for the scope statement + design (host-owned GcHeap behind a
// pin layer, auto-collect threshold, natives for JIT'd allocation).
//
// A one-TU extension (extensions/README.md purity rule): depends only on ember
// public headers (sema.hpp/ast.hpp/gc.hpp/binding_builder.hpp) + the stdlib.
// The GcHeap itself is in the core ember lib (src/gc.cpp); this extension
// wraps a thread-local instance of it + exposes the pin/collect API + the
// __ember_gc_* natives the codegen calls.
#include "ext_gc.hpp"
#include "ast.hpp"            // type_i64, NativeSig, NativeTable
#include "binding_builder.hpp" // BindingBuilder
#include "gc.hpp"             // ember::gc::GcHeap, RefMap, refmap_none

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace ember;

namespace ember::ext_gc {

// The per-thread GcHeap + its pin layer. Thread-local so each OS thread that
// runs ember code gets its own heap (mirrors context_t's per-thread model +
// ext_coroutine's thread-local current-coroutine pointer). A pinned env is a
// heap-allocated root SLOT (a void* holding the env ptr) registered with the
// GcHeap via add_root; the map keyed by env ptr lets gc_unroot_env find + drop
// it. The slot is separately heap-allocated (not inside a vector) so its
// address is stable across other pin/unpin ops (the GcHeap root API holds the
// slot address raw).
//
// PRECISE ROOT SCANNING: in addition to the legacy pin layer (still used by
// the script-visible gc_new/gc_delete), a context can be ATTACHED via
// gc_attach_context, which registers a trace callback that walks the JIT'd
// active-frame chain (context_t::gc_frame_head) + the context's global-root
// descriptor (context_t::gc_global_roots) and reports each mapped slot's
// value as a root. This is the precise path for lambda environments: they are
// NOT pinned at alloc (see gc_alloc_env) and reach the collector only via the
// frame chain (while the owning frame is live) or the global-root descriptor
// (if stored to a typed global). One attached context per thread at a time
// (mirrors context_t's per-thread model); re-attach on a different context
// detaches the previous one.
struct GcRuntime {
    gc::GcHeap heap;
    std::unordered_map<void*, void**> pinned;  // env_ptr -> root slot (a heap-allocated void* holding env_ptr)
    int64_t threshold = 1024;                  // auto-collect when live >= threshold (0 = off)
    // Precise root scanning: the currently-attached context (its trace callback
    // is registered on `heap`, capturing &ctx as user_data). nullptr = no
    // context attached (legacy pin-only behavior). The token lets detach
    // unregister the callback.
    context_t* attached_ctx = nullptr;
    gc::GcTraceToken frame_cb_token = 0;
};

static thread_local std::unique_ptr<GcRuntime> g_rt;

// Lazily create the thread-local runtime (so a thread that never calls gc_init
// but does call gc_alloc_env still works -- gc_init just pre-creates it + sets
// the threshold). Returns nullptr only on a genuine allocation failure.
static GcRuntime* rt() {
    if (!g_rt) g_rt = std::make_unique<GcRuntime>();
    return g_rt.get();
}

// ---- host setup / reset ----
bool gc_init() {
    return rt() != nullptr;
}

void gc_reset() {
    if (!g_rt) return;
    // Unregister every pin slot from the heap, free the slot, then clear the
    // heap. clear() frees all objects + drops the heap's own root list, but we
    // free the pin slots ourselves first (the heap's root list holds raw
    // pointers into our map's slots; clear() drops the list without freeing
    // them, so a leak here would be real if we did not do it).
    for (auto& kv : g_rt->pinned) {
        g_rt->heap.remove_root(kv.second);  // kv.second is the void** slot
        delete kv.second;
    }
    g_rt->pinned.clear();
    // If a context is still attached (the host called gc_reset without
    // detaching first), clear its global-roots pointer so it does not dangle
    // after the heap's registrations are dropped. The trace callback is gone
    // after heap.clear() below, so no collect would dereference it anyway, but
    // a clean pointer is safer for a host that inspects the context post-reset.
    if (g_rt->attached_ctx) g_rt->attached_ctx->gc_global_roots = nullptr;
    g_rt->attached_ctx = nullptr;
    g_rt->frame_cb_token = 0;
    g_rt->heap.clear();
}

// ---- pin / unpin ----
static bool pin_env(void* env) {
    GcRuntime* r = rt();
    if (!r || !r->heap.is_live(env)) return false;
    if (r->pinned.count(env)) return true;  // already pinned (idempotent)
    // slot is a heap-allocated void* (a void**) holding env; the GcHeap root
    // API takes the slot's address (void**) + reads *slot to find the env.
    void** slot = new (std::nothrow) void*;
    if (!slot) return false;
    *slot = env;
    r->heap.add_root(slot);
    r->pinned[env] = slot;
    return true;
}

static bool unpin_env(void* env) {
    GcRuntime* r = rt();
    if (!r) return false;
    auto it = r->pinned.find(env);
    if (it == r->pinned.end()) return false;
    r->heap.remove_root(it->second);  // it->second is the void** slot
    delete it->second;
    r->pinned.erase(it);
    return true;
}

bool gc_root_env(int64_t ptr) {
    return pin_env(reinterpret_cast<void*>(ptr));
}

bool gc_unroot_env(int64_t ptr) {
    return unpin_env(reinterpret_cast<void*>(ptr));
}

// ---- the managed-allocation API ----
// PRECISE ROOT SCANNING (lambda env path): a freshly-allocated env is NOT
// permanently pinned. Instead, the env reaches the collector via the JIT'd
// active-frame chain (the env_ptr is stored into a frame slot the frame's
// GcFrameMap lists, so while the owning frame is live the collector marks it)
// or, if the lambda escapes to a typed global, via the global-root descriptor.
// The caller's generated code stores the returned ptr into the rooted frame
// slot IMMEDIATELY after the alloc returns, before any other allocation, so the
// env is reachable by the next collect.
//
// AUTO-COLLECT WINDOW: to avoid a collect firing between the alloc and the
// first rooted store (which would reap an unrooted env), the threshold check
// runs BEFORE the alloc (collect-then-alloc), never after. A collect-then-alloc
// means the new env is the newest object and the next collect is at the NEXT
// alloc (or an explicit gc_collect), by which time the env is stored in a frame
// slot and reachable via the chain. This closes the allocation-to-first-store
// window without a pin.
//
// SCRIPT-VISIBLE gc_new keeps the legacy pin path (alloc + pin): a script-held
// raw i64 handle is typed i64 to the compiler, so the frame map cannot track it
// (the slot is not a known GC pointer). gc_delete unpins. This is the user-
// facing new/delete and is separate from the precise lambda-env tracking.
int64_t gc_alloc_env(int64_t size) {
    GcRuntime* r = rt();
    if (!r || size <= 0) return 0;
    // v1: lambda captures are scalars (i64/f64), so an env has no outgoing GC
    // pointers -> refmap_none. See ext_gc.hpp WHAT REMAINS #2 for the
    // capture-is-a-GC-object follow-up (sema-typed RefMap at alloc time).
    // Collect-then-alloc (see the comment above): close the auto-collect window
    // between alloc and the first rooted store. threshold == 0 disables auto.
    if (r->threshold > 0 && int64_t(r->heap.stats().live_objects) >= r->threshold) {
        r->heap.collect();
    }
    void* env = r->heap.alloc(size_t(size), gc::refmap_none());
    if (!env) return 0;
    // NOT pinned: reachability is determined by the frame chain / global-root
    // descriptor / explicit pin (gc_root_env). Returning here is safe because
    // the next collect is at the next alloc, by which point the env is stored.
    return int64_t(reinterpret_cast<uintptr_t>(env));
}

// ---- deterministic new/delete: unpinned object allocation + immediate ----
// ---- destruction (the language `new`/`delete` substrate).              ----
// gc_alloc_object: UNPINNED allocation on the same thread-local heap as
// gc_alloc_env, with the SAME collect-before-allocate threshold safety (the
// check runs before the alloc, never after, closing the alloc-to-first-store
// window). NOT pinned — reachability is determined by the frame chain / global
// roots / extension callbacks / an explicit pin (gc_root_env). This is the
// allocation native the future language `new` operator lowers to
// (__ember_gc_alloc_object). Distinct from gc_new (which allocs + PINS for the
// legacy script-visible surface) and from gc_alloc_env (the lambda-env path).
int64_t gc_alloc_object(int64_t size) {
    GcRuntime* r = rt();
    if (!r || size <= 0) return 0;
    // Collect-before-allocate (same safety as gc_alloc_env): if live >=
    // threshold, collect FIRST so the new object is the newest + the next
    // collect is at the next alloc, by which time the object is stored into a
    // rooted slot. threshold == 0 disables auto-collect.
    if (r->threshold > 0 && int64_t(r->heap.stats().live_objects) >= r->threshold) {
        r->heap.collect();
    }
    void* obj = r->heap.alloc(size_t(size), gc::refmap_none());
    if (!obj) return 0;
    // NOT pinned: reachability is determined by the frame chain / global-root
    // descriptor / extension callbacks / explicit pin (gc_root_env).
    return int64_t(reinterpret_cast<uintptr_t>(obj));
}

// gc_delete_object: DETERMINISTIC immediate destruction (the `delete`
// substrate). Calls GcHeap::free_object — validates the pointer is an exact
// live GC object, removes + frees it NOW (not just unpin), decrements live
// counts/bytes, increments freed_objects. This is DIFFERENT from gc_delete
// (the compatibility native), which only UNPINS an object (marks it
// collectable for a future collect). The future language `delete` operator
// lowers to __ember_gc_delete_object -> gc_delete_object.
int64_t gc_delete_object(int64_t ptr) {
    GcRuntime* r = rt();
    if (!r) return 0;
    return r->heap.free_object(reinterpret_cast<void*>(ptr)) ? 1 : 0;
}

int64_t gc_collect() {
    GcRuntime* r = rt();
    if (!r) return 0;
    size_t before = r->heap.stats().freed_objects;
    r->heap.collect();
    size_t after = r->heap.stats().freed_objects;
    // The freed count for THIS cycle = the delta (cumulative freed grew).
    return int64_t(after - before);
}

int64_t gc_live_count() {
    GcRuntime* r = rt();
    return r ? int64_t(r->heap.stats().live_objects) : 0;
}

void gc_set_threshold(int64_t n) {
    GcRuntime* r = rt();
    if (r) r->threshold = n;
}

bool gc_is_live(int64_t ptr) {
    GcRuntime* r = rt();
    if (!r) return false;
    return r->heap.is_live(reinterpret_cast<void*>(ptr));
}

int64_t gc_freed_count() {
    GcRuntime* r = rt();
    return r ? int64_t(r->heap.stats().freed_objects) : 0;
}

// Cumulative write-barrier call count (stats().barrier_calls) — the count of
// valid (live owner + live child) barrier events. Test helper for verifying
// generated code emits barrier calls at GC-child-into-GC-object store sites.
int64_t gc_barrier_count() {
    GcRuntime* r = rt();
    return r ? int64_t(r->heap.stats().barrier_calls) : 0;
}

// ---- thread-runtime facade: trace-callback + write-barrier access for ----
// ---- other extensions (operates on the current thread-local heap).    ----
// These forward to r->heap without exposing GcRuntime to the caller (rt() is
// file-static; the header only declares these functions + the gc:: types).
// gc_reset() calls heap.clear(), which invalidates every outstanding token.

gc::GcTraceToken gc_register_trace_callback(void* user_data, gc::GcTraceFn fn) {
    GcRuntime* r = rt();
    if (!r) return 0;
    return r->heap.register_trace_callback(user_data, fn);
}

bool gc_unregister_trace_callback(gc::GcTraceToken token) {
    GcRuntime* r = rt();
    if (!r) return false;
    return r->heap.unregister_trace_callback(token);
}

void gc_write_barrier(void* owner, void* child) {
    GcRuntime* r = rt();
    if (!r) return;  // no thread-local heap yet -> safe no-op
    r->heap.write_barrier(owner, child);
}

gc::GcBarrierToken gc_register_barrier_observer(void* user_data,
                                                 gc::GcBarrierObserver obs) {
    GcRuntime* r = rt();
    if (!r) return 0;
    return r->heap.register_barrier_observer(user_data, obs);
}

bool gc_unregister_barrier_observer(gc::GcBarrierToken token) {
    GcRuntime* r = rt();
    if (!r) return false;
    return r->heap.unregister_barrier_observer(token);
}

// Non-creating existence probe: returns whether the thread-local GC runtime
// exists WITHOUT lazily creating it (unlike rt(), which every other facade
// function calls). Other extensions gate their GC work (trace-callback
// registration + write barriers) on this so a thread that never called gc_init
// stays in pure non-GC mode -- no inert GcHeap is ever materialized there.
bool gc_runtime_initialized() {
    return g_rt != nullptr;
}

// ---- precise root scanning: the frame-chain + global-roots trace callback ----
// Registered on the heap by gc_attach_context with user_data = &ctx. On every
// collect() the heap invokes this callback; it walks the JIT'd active-frame
// chain (ctx->gc_frame_head) and the context's global-root descriptor
// (ctx->gc_global_roots) and reports each mapped slot's value as a root
// candidate via the visitor. The GC validates each candidate (live?) and feeds
// valid ones into the mark worklist (the c1 trace-callback contract). Stale
// slots (a freed env still referenced by a not-yet-unlinked frame record after a
// trap longjmp) are safely ignored by the visitor's liveness check.
static void context_roots_trace_cb(void* user_data, gc::GcTraceVisitor& visitor) {
    auto* ctx = reinterpret_cast<context_t*>(user_data);
    if (!ctx) return;
    // Walk the shadow stack: each active frame's mapped GC-pointer slots.
    for (gc::GcFrameRecord* rec = ctx->gc_frame_head; rec != nullptr; rec = rec->prev) {
        if (!rec->map || rec->map->offs.empty()) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(rec->frame_base);
        for (int32_t off : rec->map->offs) {
            // The slot address is base + off (off is rbp-relative, negative).
            void* const* slot = reinterpret_cast<void* const*>(base + intptr_t(off));
            visitor.report(*slot);
        }
    }
    // Walk the typed-global GC-pointer words.
    if (ctx->gc_global_roots && !ctx->gc_global_roots->empty()) {
        const uint64_t gbase = ctx->gc_global_roots->base;
        for (int32_t off : ctx->gc_global_roots->offs) {
            void* const* slot = reinterpret_cast<void* const*>(uintptr_t(gbase) + intptr_t(off));
            visitor.report(*slot);
        }
    }
}

bool gc_attach_context(context_t* ctx, gc::GcGlobalRoots* global_roots) {
    GcRuntime* r = rt();
    if (!r || !ctx) return false;
    // Re-attach on a DIFFERENT context detaches the previous one first (one
    // attached context per thread). Re-attach on the SAME context just updates
    // the global-roots pointer (idempotent, no double-registration).
    if (r->attached_ctx && r->attached_ctx != ctx && r->frame_cb_token != 0) {
        r->heap.unregister_trace_callback(r->frame_cb_token);
        r->frame_cb_token = 0;
    }
    if (r->frame_cb_token == 0) {
        r->frame_cb_token = r->heap.register_trace_callback(ctx, &context_roots_trace_cb);
    }
    r->attached_ctx = ctx;
    ctx->gc_global_roots = global_roots;
    return r->frame_cb_token != 0;
}

void gc_detach_context(context_t* ctx) {
    GcRuntime* r = g_rt.get();  // no lazy create: detach is a teardown path
    if (!r) return;
    if (r->frame_cb_token != 0) {
        r->heap.unregister_trace_callback(r->frame_cb_token);
        r->frame_cb_token = 0;
    }
    if (ctx) ctx->gc_global_roots = nullptr;
    r->attached_ctx = nullptr;
}

// ---- the natives (JIT'd code calls these by name) ----
extern "C" {

// __ember_gc_alloc_env(i64 size) -> i64 ptr
// Allocate + pin a lambda env of `size` user bytes. Returns the env pointer as
// an i64 (0 on failure). codegen's lambda-env case calls this when
// CodeGenCtx::use_gc_env is set, then copies each capture into [ptr + offset]
// and stores ptr as the lambda value's env_ptr half.
static int64_t n_gc_alloc_env(int64_t size) {
    return gc_alloc_env(size);
}

// __ember_gc_collect() -> i64 freed
// Run a collection cycle. Returns the number of objects freed this cycle.
// Pinned envs survive; unpinned unreachable envs are swept.
static int64_t n_gc_collect() {
    return gc_collect();
}

// __ember_gc_live() -> i64 count
// Live object count on this thread's heap. Test/diagnostic native.
static int64_t n_gc_live() {
    return gc_live_count();
}

// ---- script-visible new/delete natives (full GC integration task) ----
// These are the user-facing surface for script-visible heap objects: a script
// can `gc_new(size)` to allocate a raw GC heap object (pinned, so it survives
// auto-collects), `gc_delete(ptr)` to unroot it (mark it collectable), and
// `gc_collect()`/`gc_live()` to run a collection cycle + observe the live
// count. This is the v1 new/delete: a raw handle (i64) the script can hold +
// release, with the tracing GC reclaiming unreachable (unpinned) objects.
// `new Type{...}` syntax with typed field access needs a pointer type system
// (ember's structs are value types today) — that is the documented follow-up;
// these natives are the heap-management substrate it would build on.
//
// NOTE: a gc_new'd object is NOT tracked by the precise frame-chain scanner (its
// script-held handle is typed i64 to the compiler, so the frame map does not
// list it as a GC pointer). It uses the LEGACY pin layer: alloc + pin, gc_delete
// unpins. This is separate from the lambda-env precise path (which does NOT pin
// and relies on the frame chain / global roots). gc_alloc_env itself no longer
// pins (the lambda-env path), so gc_new pins explicitly here.
static int64_t n_gc_new(int64_t size) {
    int64_t p = gc_alloc_env(size);  // alloc (NOT pinned by gc_alloc_env anymore)
    if (p != 0) gc_root_env(p);     // legacy pin: script-held i64 handles need it
    return p;
}

static int64_t n_gc_delete(int64_t ptr) {
    // UNPIN (not destroy): gc_delete is the COMPATIBILITY unpinning native. It
    // removes the legacy pin so the object becomes COLLECTABLE by a future
    // collect() — it does NOT free the object immediately. This is distinct
    // from __ember_gc_delete_object (n_gc_delete_object below), which calls
    // GcHeap::free_object for DETERMINISTIC immediate destruction (the language
    // `delete` substrate). Preserved as-is so current gc_new/gc_delete tests do
    // not regress.
    return gc_unroot_env(ptr) ? 1 : 0;  // unpin -> collectable; 1 if it was pinned
}

// ---- deterministic new/delete natives (the language `new`/`delete` ----
// ---- substrate; stable contract for the compiler).                  ----
// __ember_gc_alloc_object(i64 size) -> i64 ptr
// Allocate an UNPINNED raw GC heap object of `size` user bytes (the `new`
// substrate). Same thread-local heap + collect-before-allocate threshold
// safety as __ember_gc_alloc_env. NOT pinned (reachability via the frame
// chain / global roots / extension callbacks / an explicit pin). Returns the
// object pointer as an i64 (0 on failure). The future language `new` operator
// lowers to this native. Distinct from gc_new (alloc+pin) and
// __ember_gc_alloc_env (lambda-env path).
static int64_t n_gc_alloc_object(int64_t size) {
    return gc_alloc_object(size);
}

// __ember_gc_delete_object(i64 ptr) -> i64 success
// Immediately + deterministically free a GC object (the `delete` substrate).
// Calls GcHeap::free_object: validates `ptr` is an exact live GC user pointer,
// removes + frees it NOW, decrements live counts/bytes, increments
// freed_objects. Returns 1 on success, 0 if `ptr` is null / not a live GC
// object / already freed. DIFFERENT from gc_delete (which only unpins — see
// n_gc_delete above). The future language `delete` operator lowers to this
// native. Stale root/edge values referencing the freed object are safely
// ignored by the collector's liveness filter (no dangling deref, no
// resurrection).
static int64_t n_gc_delete_object(int64_t ptr) {
    return gc_delete_object(ptr);
}

// __ember_gc_write_barrier(i64 owner, i64 child) -> i64 (0)
// The c1 write barrier, called by generated code whenever it stores a GC child
// into a GC-managed object. Safely ignores null / non-live owner or child (the
// c1 contract), so it is safe to call unconditionally from generated write
// sites. The current non-generational collector does not need a remembered set,
// so the observable effect is only a registered barrier observer +
// stats().barrier_calls — the extension surface a future generational collector
// would build on. Returns 0 (the i64 placeholder so the native has a stable
// i64(i64,i64) Win64 binding the codegen can resolve by name).
static int64_t n_gc_write_barrier(int64_t owner, int64_t child) {
    gc_write_barrier(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(child));
    return 0;
}

} // extern "C"

// ---- registration ----
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type T = type_i64();
    // The alloc native takes an i64 size and returns an i64 pointer (the env
    // handle). collect/live take no args and return i64 counts. All three are
    // internal (__ember_ prefix, mirroring __ember_coro_yield) so they do not
    // collide with user fns + are clearly engine-internal.
    b.add("__ember_gc_alloc_env", T, {T}, (void*)&n_gc_alloc_env);
    b.add("__ember_gc_collect",    T, {}, (void*)&n_gc_collect);
    b.add("__ember_gc_live",       T, {}, (void*)&n_gc_live);
    // Write barrier native (c1): generated code calls this when storing a GC
    // child into a GC-managed object. i64(i64,i64) Win64 binding.
    b.add("__ember_gc_write_barrier", T, {T, T}, (void*)&n_gc_write_barrier);
    // Script-visible new/delete (full GC integration): gc_new(size)->ptr
    // (alloc+pin), gc_delete(ptr)->i64 (unroot), gc_collect()->freed,
    // gc_live()->count. User-facing names (no __ember_ prefix) so a script
    // calls them directly; they share the same host heap as lambda envs.
    b.add("gc_new",     T, {T},  (void*)&n_gc_new);
    b.add("gc_delete",  T, {T},  (void*)&n_gc_delete);
    b.add("gc_collect", T, {},   (void*)&n_gc_collect);
    b.add("gc_live",    T, {},   (void*)&n_gc_live);
    // Deterministic new/delete substrate (the language `new`/`delete` internal
    // natives): __ember_gc_alloc_object (unpinned alloc) + __ember_gc_delete_object
    // (immediate free via GcHeap::free_object). Stable contract for the compiler.
    // Separate from the legacy gc_new/gc_delete (pin/unpin) compatibility surface
    // above: gc_new pins + gc_delete unpins (collectable later); these alloc
    // unpinned + free NOW (deterministic destruction). The future language `new`
    // operator lowers to __ember_gc_alloc_object; `delete` to
    // __ember_gc_delete_object.
    b.add("__ember_gc_alloc_object",  T, {T}, (void*)&n_gc_alloc_object);
    b.add("__ember_gc_delete_object", T, {T}, (void*)&n_gc_delete_object);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_gc
