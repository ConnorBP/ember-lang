// gc.hpp — tracing mark-sweep garbage collector core.
//
// Foundational infrastructure for lambdas-with-capture (#20) and coroutines
// (#21). Manages a GC heap of objects with known reference layouts (NOT
// classes — OOP is a non-goal). Stop-the-world mark-sweep.
//
// CONCURRENCY MODEL (standalone collector core):
//   All mutable GcHeap state (the live set, roots, trace-callback registry,
//   barrier-observer registry, token counters, statistics, the RSS throttle,
//   and the mark/sweep phases) is guarded by a single coarse recursive mutex
//   `m_lock`. Every public mutating/querying method acquires `m_lock` and
//   then performs its work through `*_locked` helpers that assume the lock is
//   already held. The recursive mutex lets a trace callback / barrier observer
//   re-enter register/unregister (or any other public API) from WITHIN its own
//   invocation without self-deadlock.
//
//   CALLBACK / OBSERVER RE-ENTRY (deliberate, self-deadlock-free):
//     collect() and write_barrier() do NOT range-iterate the live registries
//     while invoking user code. They SNAPSHOT the callback / observer records
//     into a local vector UNDER the lock, then invoke from the snapshot. A
//     callback / observer that registers or unregisters ITSELF (or another
//     entry) during invocation mutates the LIVE registry (under the recursive
//     lock — no deadlock) but NOT the snapshot — so the active iteration is
//     never invalidated (no same-thread iterator/reference invalidation, the
//     bug a bare recursive mutex does NOT prevent).
//     SNAPSHOT SEMANTICS (documented): a callback / observer registered DURING
//     a collect() / write_barrier() invocation is NOT invoked in that same
//     call (the snapshot was already taken); it IS invoked on the NEXT call.
//     A callback / observer that unregisters itself DURING invocation may
//     still be invoked in that same call (it was already in the snapshot) but
//     is not invoked thereafter. This is the stable contract callers can rely
//     on.
//     A re-entrant collect() (a callback that calls collect() from within its
//     own invocation) is gracefully short-circuited via the `m_collecting`
//     guard: it returns a stats snapshot WITHOUT running a nested mark/sweep
//     (a nested sweep would corrupt / double-free), so collection can never
//     self-deadlock or self-corrupt.
//
//   STATISTICS SNAPSHOT API: stats() and collect() return GcStats BY VALUE (a
//     locked copy), never a reference to the mutable m_stats. Exposing a
//     reference would let another thread mutate the counters while a caller
//     reads them (a torn / inconsistent read). The by-value snapshot is the
//     safe API; callers that held `const GcStats&` bind to the returned
//     temporary (lifetime-extended) and see a coherent point-in-time copy.
//
//   OBJECT-PAYLOAD ACCESS: the core lock protects HEAP ALLOCATION and COLLECTOR
//   METADATA (the live set, headers, roots, registries, stats). It does NOT
//   serialize direct reads/writes of object USER payloads (the bytes returned
//   by alloc). Direct payload access is protected by the HIGHER-LEVEL
//   EXECUTION STOP-THE-WORLD protocol implemented during integration (the
//   engine's per-call mutex / the thread extension's call gate), which ensures
//   no JIT'd code touches an object's payload while the collector is examining
//   or freeing it. The core lock + the integration stop-the-world protocol are
//   two distinct layers; the core lock alone is not sufficient for safe
//   concurrent payload mutation.
//
// No ast.hpp/sema.hpp/codegen.hpp dependency — the `ember` core lib must stay
// frontend-free (one-way link direction per CMakeLists.txt).
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <initializer_list>

namespace ember::gc {

// Which byte-offsets within an object's USER bytes hold GC pointers.
// 8-aligned byte offsets. Empty = leaf object (no outgoing refs).
struct RefMap {
    std::vector<uint32_t> ptr_byte_offsets;
    bool empty() const { return ptr_byte_offsets.empty(); }
};

RefMap refmap_none();
RefMap refmap_words(std::initializer_list<uint32_t> byte_offsets);

struct GcStats {
    size_t live_objects = 0;
    size_t live_bytes   = 0;
    size_t total_allocated = 0;   // cumulative alloc count
    size_t collections  = 0;     // collect() call count
    size_t freed_objects = 0;    // cumulative swept count
    size_t barrier_calls = 0;    // cumulative write_barrier() invocations that
                                 // observed a live owner+child pair (null/non-
                                 // live pairs do not bump this). See
                                 // write_barrier() for the rationale.
};

class GcHeap;

// ===========================================================================
// Trace-callback mechanism (external root providers).
//
// A trace callback is how an external root provider (an extension, a host root
// set, a coroutine suspended-frame registry, ...) supplies roots to the GC
// WITHOUT exposing its own internals or registering individual root slots. The
// provider registers one callback (with a piece of caller-owned user_data);
// during collect() the GC invokes every registered callback and hands each a
// GcTraceVisitor. The callback calls visitor.report(candidate_ptr) for each
// candidate GC object pointer it wants treated as a root.
//
// The GC validates each reported pointer: only pointers to CURRENTLY-LIVE GC
// objects are marked and fed into the SAME mark worklist used by explicit root
// slots (add_root) and by RefMap edge tracing. Null pointers, pointers into
// non-GC memory (e.g. stack slots), and pointers to already-freed objects are
// silently ignored — a provider can report its whole candidate set without
// having to pre-filter, and stale entries are safe.
//
// Registration returns a STABLE token (a non-zero integer) the caller keeps to
// remove the callback later. unregister_trace_callback(token) removes the
// callback; it does NOT touch user_data (the caller owns it, so the heap never
// retains a pointer to caller storage after unregister). clear() drops every
// registration without retaining any caller user_data — there are no dangling
// callback user data after clear(). After clear() (or after unregister) all
// previously-issued tokens for the dropped registrations are invalid; calling
// unregister on an invalid token returns false.
//
// The visitor's only public contract for callbacks is report(void*). The other
// members are set up by the heap during collect() and are not part of the
// stable callback API.
// ===========================================================================

struct GcTraceVisitor {
    // Called by a trace callback for each candidate GC object pointer. The GC
    // validates `candidate` (live?) and, if live + not yet marked, marks it and
    // pushes it onto the mark worklist (so its RefMap edges are traced next).
    // Safe to call with nullptr / non-GC / freed pointers — they are ignored.
    void report(void* candidate);

    // --- internals, set by the heap during collect(); do not use from a callback ---
    GcHeap* heap = nullptr;
    std::vector<void*>* worklist = nullptr;
};

using GcTraceFn = void (*)(void* user_data, GcTraceVisitor& visitor);

// Stable registration token. 0 == invalid / no registration.
using GcTraceToken = uint64_t;

// ===========================================================================
// Write barrier + barrier observer hook.
//
// write_barrier(owner, child) records that `owner` now references `child`
// (the program wrote `child` into one of owner's pointer fields). The current
// stop-the-world, non-generational collector does NOT need a remembered set,
// so the barrier is a behavioural no-op on collection itself — it does not
// change reachability and is not required for correctness today. It exists as
// the observability/extension surface a future generational or concurrent
// collector would build on:
//   - a registered barrier observer is notified of every valid (live owner +
//     live child) barrier event, so a future remembered-set or card-table
//     implementation can be layered on WITHOUT touching the core collector;
//   - stats().barrier_calls counts the valid events (useful for tests +
//     diagnostics).
//
// Null or non-live owner/child are SAFELY IGNORED: no observer is fired and
// barrier_calls is not bumped. This keeps the barrier safe to call
// unconditionally from generated write sites without the caller having to
// pre-check liveness.
// ===========================================================================

using GcBarrierObserver = void (*)(void* user_data, void* owner, void* child);
using GcBarrierToken = uint64_t;  // 0 == invalid / no registration.

class GcHeap {
public:
    GcHeap();
    ~GcHeap();

    // Allocate `size` user bytes with reference layout `rm`. Returns pointer to
    // USER bytes (a hidden header precedes it). nullptr on bad_alloc. Object is
    // unmarked; the next collect() frees it unless a root reaches it.
    //
    // SAFETY: every allocation checks process RSS against the global memory
    // limit (safety::check_memory_limit, throttled to every ~64 MiB of live-
    // byte growth). If RSS exceeds the limit, the process instant-fails
    // (abort) — this is the failsafe that stops an unbounded allocation loop
    // from freezing the host machine. nullptr is still possible on malloc
    // failure below the RSS limit.
    void* alloc(size_t size, const RefMap& rm);

    // Precise root registration: `addr` is the address of a slot (void**) that
    // holds a pointer to a GC object's user bytes. The slot must stay alive and
    // its pointed-to pointer must stay valid until remove_root. collect() reads
    // *addr to find the root object, then traces.
    void add_root(void** addr);
    void remove_root(void** addr);
    void clear_roots();

    // --- Trace-callback registration (external root providers) ---
    // Register a trace callback. `user_data` is caller-owned and passed back to
    // `fn` on each collect(); the heap does NOT copy or retain ownership of it
    // (it stores the raw pointer only for the lifetime of the registration).
    // Returns a stable non-zero token, or 0 on failure (fn == nullptr). The
    // same callback may be registered multiple times (each gets its own token).
    GcTraceToken register_trace_callback(void* user_data, GcTraceFn fn);
    // Remove the callback identified by `token`. Returns true if a callback was
    // removed, false if `token` is invalid / already removed. Does NOT touch
    // user_data (the caller owns it). After this, `token` is invalid.
    bool unregister_trace_callback(GcTraceToken token);

    // --- Write barrier + barrier observer hook ---
    // See the block comment above for the full rationale. Returns nothing; the
    // observable effects are observer notification + stats().barrier_calls.
    void write_barrier(void* owner, void* child);
    GcBarrierToken register_barrier_observer(void* user_data, GcBarrierObserver obs);
    bool unregister_barrier_observer(GcBarrierToken token);

    // Mark from all registered roots AND all registered trace callbacks, trace
    // each reachable object's RefMap (recursively), then sweep (free) every
    // unmarked object. Clears marks on survivors. Returns a STATS SNAPSHOT (by
    // value — a locked copy of m_stats); see the concurrency-model note at the
    // top of this file for why stats are never returned by reference.
    GcStats collect();
    // Return a STATS SNAPSHOT (by value, locked copy) of the current counters.
    // Never a reference to mutable m_stats (another thread could mutate the
    // counters mid-read); the by-value copy is the safe, consistent API.
    GcStats stats() const;

    // True iff `p` points at the user bytes of a currently-live GC object.
    bool is_live(const void* p) const;

    // Deterministic immediate free — the runtime substrate for the future
    // language `delete` operator. Validates that `p` is an EXACT live GC user
    // pointer (returns false for null / non-GC / interior / already-freed
    // pointers). On success: removes `p` from the live set, frees its
    // allocation IMMEDIATELY (bypassing the mark-sweep collector), decrements
    // live_objects + live_bytes, and increments freed_objects.
    //
    // Does NOT touch root slots or RefMap edges that may still reference `p`
    // — those stale values are SAFELY IGNORED by the existing liveness filter:
    // the explicit-root loop calls is_live(*root) before marking; the trace
    // visitor calls is_live(candidate); RefMap edge tracing calls
    // is_live(child). A freed pointer fails is_live() in all three, so it is
    // skipped (not dereferenced, not resurrected). Clearing every possible
    // reference would require a full heap scan (the collector's job); leaving
    // stale values for the filter to ignore is the O(1) deterministic-delete
    // contract.
    //
    // This is DIFFERENT from unpinning (the extension's gc_unroot_env / the
    // gc_delete native): unpinning only removes a root so the object becomes
    // COLLECTABLE by a future collect(); free_object frees the object NOW,
    // deterministically. The caller must not use `p` after a true return
    // (use-after-free is the caller's contract, the same as free()).
    bool free_object(void* p);

    // Free ALL objects + clear roots + clear all trace-callback and barrier-
    // observer registrations (without touching any caller user_data). Test
    // teardown / full reset. After this, every previously-issued token is
    // invalid.
    void clear();

private:
    struct Header;
    struct TraceCb {
        GcTraceToken token;
        void* user_data;
        GcTraceFn fn;
    };
    struct BarrierObs {
        GcBarrierToken token;
        void* user_data;
        GcBarrierObserver fn;
    };
    std::unordered_set<void*> m_live;  // user-byte pointers
    std::vector<void**> m_roots;
    std::vector<TraceCb> m_trace_cbs;
    std::vector<BarrierObs> m_barrier_obs;
    GcStats m_stats;
    GcTraceToken m_next_trace_token = 1;     // 0 reserved for "invalid"
    GcBarrierToken m_next_barrier_token = 1; // 0 reserved for "invalid"
    // RSS-check throttle: only call safety::check_memory_limit() when live_bytes
    // has grown by >= 64 MiB since the last check. Keeps the ~1us syscall out of
    // the per-alloc hot path while still catching runaway growth within ~64 MiB.
    size_t m_last_rss_check_bytes = 0;

    // Coarse heap lock guarding ALL mutable state above (live set, roots,
    // registries, stats, token counters, RSS throttle) + the mark/sweep phases.
    // Recursive so a callback / observer can re-enter any public API from
    // within its own invocation without self-deadlock. `mutable` so the const
    // query methods (is_live, stats) can lock it.
    mutable std::recursive_mutex m_lock;
    // Re-entrancy guard for collect(): true while THIS thread is mid-collect.
    // A callback that re-enters collect() from within its own invocation would
    // otherwise run a NESTED mark/sweep (corrupting / double-freeing); the guard
    // short-circuits a re-entrant collect() to a stats snapshot. Guarded by
    // m_lock; only ever set/cleared by the collecting thread under the lock.
    bool m_collecting = false;

    // --- locked/unlocked helpers (assume m_lock is held) ---
    // True iff `p` points at the user bytes of a currently-live GC object.
    // Assumes m_lock is held (the public is_live() acquires it then calls here).
    bool is_live_locked(const void* p) const;
    // Mark `obj` (a live GC object's user bytes) + push onto `worklist` if not
    // already marked. Used by both the root loop and the trace-visitor. Assumes
    // m_lock is held (collect() acquires it; the visitor runs under collect()).
    void mark_and_push_locked(void* obj, std::vector<void*>& worklist);

    // GcTraceVisitor::report() calls is_live_locked + mark_and_push_locked on
    // validated candidates, so it needs access to these private helpers (the
    // visitor is the bridge between a callback's reported pointers and the
    // heap's mark worklist). report() always runs under collect()'s held lock.
    friend struct GcTraceVisitor;
};

} // namespace ember::gc
