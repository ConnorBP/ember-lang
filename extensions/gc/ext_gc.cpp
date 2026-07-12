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
struct GcRuntime {
    gc::GcHeap heap;
    std::unordered_map<void*, void**> pinned;  // env_ptr -> root slot (a heap-allocated void* holding env_ptr)
    int64_t threshold = 1024;                  // auto-collect when live >= threshold (0 = off)
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
int64_t gc_alloc_env(int64_t size) {
    GcRuntime* r = rt();
    if (!r || size <= 0) return 0;
    // v1: lambda captures are scalars (i64/f64), so an env has no outgoing GC
    // pointers -> refmap_none. See ext_gc.hpp WHAT REMAINS #2 for the
    // capture-is-a-GC-object follow-up (sema-typed RefMap at alloc time).
    void* env = r->heap.alloc(size_t(size), gc::refmap_none());
    if (!env) return 0;
    // Pin BEFORE the threshold check so the just-allocated env survives the
    // auto-collect (it is reachable from the JIT'd code that asked for it).
    pin_env(env);
    // Collection trigger: if the heap has reached the threshold, collect now.
    // Pinned envs (including this one) survive; only previously-unpinned envs
    // are swept. threshold == 0 disables auto-collect.
    if (r->threshold > 0 && int64_t(r->heap.stats().live_objects) >= r->threshold) {
        r->heap.collect();
    }
    return int64_t(reinterpret_cast<uintptr_t>(env));
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
static int64_t n_gc_new(int64_t size) {
    return gc_alloc_env(size);  // alloc + pin (survives auto-collect)
}

static int64_t n_gc_delete(int64_t ptr) {
    return gc_unroot_env(ptr) ? 1 : 0;  // unpin -> collectable; 1 if it was pinned
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
    // Script-visible new/delete (full GC integration): gc_new(size)->ptr
    // (alloc+pin), gc_delete(ptr)->i64 (unroot), gc_collect()->freed,
    // gc_live()->count. User-facing names (no __ember_ prefix) so a script
    // calls them directly; they share the same host heap as lambda envs.
    b.add("gc_new",     T, {T},  (void*)&n_gc_new);
    b.add("gc_delete",  T, {T},  (void*)&n_gc_delete);
    b.add("gc_collect", T, {},   (void*)&n_gc_collect);
    b.add("gc_live",    T, {},   (void*)&n_gc_live);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_gc
