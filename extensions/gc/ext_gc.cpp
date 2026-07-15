// ext_gc.cpp - ember extension: tracing GC runtime for lambda/coroutine heap.
// See ext_gc.hpp for the scope statement + design (context-owned shareable
// GcHeap behind a pin layer, auto-collect threshold, natives for JIT'd
// allocation, cooperative stop-the-world collection across execution
// participants).
//
// A one-TU extension (extensions/README.md purity rule): depends only on ember
// public headers (sema.hpp/ast.hpp/gc.hpp/binding_builder.hpp/context.hpp) +
// the stdlib. The GcHeap itself is in the core ember lib (src/gc.cpp); this
// extension wraps a context-owned, SHARABLE instance of it + exposes the
// pin/collect API + the __ember_gc_* natives the codegen calls.
//
// === THE CONTEXT-OWNED SHARED RUNTIME + EXECUTION PARTICIPANTS ==============
//
// A GcRuntime (a GcHeap + pin layer + execution-participant registry) is
// created on demand by gc_attach_context and owned by std::shared_ptr across
// every attached thread (a thread holds a shared_ptr for as long as it is the
// active runtime, so the runtime outlives any single thread). The runtime's
// raw pointer is recorded on the host context_t::gc_runtime (an opaque void*)
// so a spawned worker (created by the thread extension) can capture it at
// thread_spawn time + hand it to gc_thread_enter, joining the SAME shared heap.
// Every attached context — the host's AND each worker's own per-call context_t
// — registers as a PARTICIPANT on the runtime, contributing its per-thread
// gc_frame_head (the shadow-stack head) to the collector.
//
// A caller that never attaches a context (no gc_attach_context / gc_thread_enter)
// still gets a PRIVATE thread-local runtime via the rt() lazy-create fallback,
// so single-threaded + non-GC hosts are byte-identical to the pre-shared-heap
// behavior: no participants, no shared state, the inert thread-local heap.
//
// === COOPERATIVE STOP-THE-WORLD COLLECTION =================================
//
// gc_collect (and the auto-collect at the alloc threshold) is a COOPERATIVE
// stop-the-world cycle:
//   1. Elect a single collector (atomic CAS on collect_owner) + set
//      collect_requested (under part_mtx). A concurrent gc_collect loser
//      parks as a participant (or waits) + runs its OWN cycle after the winner
//      finishes, so two collectors never race scan_snapshot / collection state.
//      New allocs + new participant entries (gc_thread_enter) observe
//      collect_requested at their safepoint (checked UNDER part_mtx) + PARK
//      instead of proceeding -- so new entries are blocked for the cycle.
//   2. Wait (UNBOUNDED -- part_cv.wait, never a timeout) until every OTHER
//      active participant has parked at a GC safepoint or exited. A
//      participant parks at the GC safepoints: alloc, collect, write_barrier,
//      gc_live, gc_delete, gc_thread_enter, gc_thread_exit, AND thread_join
//      (a participant blocked in thread_join polls gc_park each iteration, so
//      a joiner waiting for the collector's worker to finish still parks + lets
//      the collector proceed -- no deadlock). The collector NEVER proceeds
//      after a timeout + NEVER scans a non-parked participant's gc_frame_head
//      (a raw non-atomic pointer that may be mutating under JIT prologues/
//      epilogues -- scanning it would be a data race that corrupts root
//      scanning). The safepoints cover every GC-mutating operation + the join
//      wait, so every participant either parks or exits in bounded wall time;
//      the unbounded wait therefore completes.
//   3. Snapshot ONLY the parked (or self) participants' per-thread gc_frame_head
//      (stable while parked: the participant is inside a native, not executing
//      JIT) + the shared immutable global roots, and feed them to the GcHeap's
//      trace callback.
//   4. Perform the mark-sweep under the GcHeap's own synchronized m_lock.
//   5. Clear collect_requested + collect_owner, bump collect_generation, wake
//      every parked participant so they resume.
//
// Allocation remains concurrency-safe under the heap lock (GcHeap::alloc takes
// m_lock). Allocation, collection, write-barrier natives, thread_join, and the
// worker's outer-call exit (gc_thread_exit) all cooperate with safepoints, so a
// collection never races a participant's JIT root-chain mutation or object-
// payload writes. The write barrier PARKS (does not skip): a participant
// recording a GC edge first parks at the barrier safepoint if a collect is in
// progress, so the collector never scans a mid-write state; the array/map
// mutations release their store mutex BEFORE the barrier so parking cannot
// AB-BA deadlock the trace callbacks. Trap/longjmp cleanup (gc_thread_exit on
// the worker's trapped-exit path) guarantees an abandoned participant record +
// its shadow-stack head cannot remain registered after a trap unwinds the
// worker's call. The shared pin map + threshold are guarded by pinned_mtx.
#include "ext_gc.hpp"
#include "ast.hpp"            // type_i64, NativeSig, NativeTable
#include "binding_builder.hpp" // BindingBuilder
#include "gc.hpp"             // ember::gc::GcHeap, RefMap, refmap_none

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace ember;

namespace ember::ext_gc {

// One execution participant: a thread that has entered JIT'd code on this
// shared runtime + contributes its per-thread gc_frame_head to the collector.
// The runtime OWNS the record (a unique_ptr in the participant vector); the
// owning thread holds a raw pointer to it (g_my_part, TLS) to set its parked
// flag at safepoints + to unregister on exit. `parked` is set by the
// participant when it reaches a GC safepoint while a collect is in progress
// (it then waits for collect_generation to advance before resuming). A parked
// participant is INSIDE a GC-cooperating native (alloc / collect / live /
// delete / write_barrier / thread_enter / thread_exit), NOT executing JIT'd
// code, so its gc_frame_head is stable + the collector may scan it safely.
struct Participant {
    context_t* ctx = nullptr;          // the participant's per-call context (has gc_frame_head)
    std::atomic<bool> parked{false};   // true == at a safepoint, not mutating the shadow stack
};

// The per-runtime GC state: the heap + pin layer + the participant registry +
// the cooperative-STW synchronization state + the shared global-root
// descriptor. Owned by shared_ptr across every attached thread (g_rt_hold on
// each thread); looked up by raw pointer via g_shared_runtimes (weak_ptr) so a
// worker can capture it from the host context's gc_runtime back-pointer.
struct GcRuntime {
    gc::GcHeap heap;
    // The pin layer (env_ptr -> heap-allocated root slot holding env_ptr) + the
    // auto-collect threshold. Guarded by pinned_mtx: pin/unpin mutate the map
    // + add/remove heap roots; gc_set_threshold/gc_collect read threshold;
    // these are reachable from multiple participants on the shared runtime, so
    // unsynchronized access would race the map + the threshold read.
    std::mutex pinned_mtx;
    std::unordered_map<void*, void**> pinned;
    int64_t threshold = 1024;                  // auto-collect when live >= threshold (0 = off)

    // Execution-participant registry (cooperative STW).
    std::mutex part_mtx;
    std::condition_variable part_cv;
    std::atomic<bool> collect_requested{false};
    std::atomic<uint64_t> collect_generation{0};
    std::vector<std::unique_ptr<Participant>> participants;

    // Collector election (Fix: the previous check-then-set on collect_requested
    // let two concurrent gc_collect callers both pass the check + both drive a
    // STW cycle, racing scan_snapshot + collection state). A collector atomically
    // claims ownership by CASing collect_owner from 0 to its thread id; the loser
    // parks as a participant (or, if it has no participant record, waits for the
    // in-progress cycle to finish) + then runs its OWN cycle. Only the owner
    // drives the STW + the scan snapshot. Cleared by the owner at cycle end.
    std::atomic<std::thread::id> collect_owner{};  // default-constructed == "no owner"

    // Snapshot of the participant contexts to scan for the in-flight collect.
    // Set under part_mtx in gc_collect BEFORE heap.collect() + read by the trace
    // callback DURING heap.collect() (under the heap's m_lock). ONLY parked (or
    // self) participants are snapshotted: a parked participant is inside a GC
    // native (not executing JIT), so its gc_frame_head is stable + the raw
    // non-atomic pointer read is race-free. A non-parked participant is NOT
    // scanned (its gc_frame_head may be mutating under JIT prologues/epilogues).
    // Stable for the cycle's duration (new entries are blocked + exiting
    // participants wait for collect_done before unregistering), so the callback
    // reads it WITHOUT part_mtx. Cleared after the cycle.
    std::vector<context_t*> scan_snapshot;
    gc::GcGlobalRoots* global_roots = nullptr;

    // The runtime's frame-chain + global-roots trace callback token (registered
    // once at runtime creation so every collect walks every participant).
    gc::GcTraceToken frame_cb_token = 0;
};

// The active runtime on this thread (the shared runtime this thread joined, or
// null if none is active — rt() then lazily creates a private thread-local
// fallback). g_rt_hold is the shared_ptr keeping a shared runtime alive on this
// thread; g_owned_rt is a private thread-local runtime for callers that never
// attached a context. g_my_part is this thread's participant record on its
// active runtime (null when not registered).
static thread_local GcRuntime* g_rt = nullptr;
static thread_local std::shared_ptr<GcRuntime> g_rt_hold;
static thread_local std::unique_ptr<GcRuntime> g_owned_rt;
static thread_local Participant* g_my_part = nullptr;

// Lookup table for shared runtimes by raw pointer (weak_ptr so the table does
// NOT keep a runtime alive — the threads' g_rt_hold shared_ptrs do). A worker
// captures the host context's gc_runtime raw pointer + locks it here to obtain
// its own shared_ptr (joining the shared heap). Guarded by g_shared_mtx.
static std::mutex g_shared_mtx;
static std::unordered_map<GcRuntime*, std::weak_ptr<GcRuntime>> g_shared_runtimes;

// Forward decl: the participant-aware trace callback (defined below) is
// registered on the heap at runtime creation (rt()), which precedes its
// definition.
static void context_roots_trace_cb(void* user_data, gc::GcTraceVisitor& visitor);

// Lazily create the active runtime on this thread if none is active. A thread
// that never attached a context gets a private thread-local runtime (the inert
// fallback); a thread that attached/entered a shared runtime has g_rt already
// set to it. Returns nullptr only on a genuine allocation failure.
static GcRuntime* rt() {
    if (g_rt) return g_rt;
    if (g_rt_hold) { g_rt = g_rt_hold.get(); return g_rt; }
    auto sp = std::make_shared<GcRuntime>();
    // Register the runtime's trace callback once (it walks the participant
    // snapshot + global roots on every collect — empty for the private
    // fallback with no participants, so it is a no-op there).
    sp->frame_cb_token = sp->heap.register_trace_callback(sp.get(), &context_roots_trace_cb);
    {
        std::lock_guard<std::mutex> lk(g_shared_mtx);
        g_shared_runtimes.emplace(sp.get(), std::weak_ptr<GcRuntime>(sp));
    }
    g_owned_rt = nullptr;  // a shared runtime is now active; clear any private one
    g_rt_hold = std::move(sp);
    g_rt = g_rt_hold.get();
    return g_rt;
}

// ---- the participant-aware trace callback (walks the snapshot) ----
// Registered on the heap at runtime creation with user_data = the GcRuntime.
// On every collect() the heap invokes this callback; it walks every
// participant's per-thread gc_frame_head (the snapshot taken in gc_collect) +
// the shared immutable global roots, reporting each mapped slot's value as a
// root candidate via the visitor. The GC validates each candidate (live?) and
// feeds valid ones into the mark worklist. Stale slots (a freed env still
// referenced by a not-yet-unlinked frame record after a trap longjmp) are
// safely ignored by the visitor's liveness check. The snapshot is stable for
// the cycle (new entries blocked + exiting participants wait), so this reads
// it WITHOUT part_mtx.
static void context_roots_trace_cb(void* user_data, gc::GcTraceVisitor& visitor) {
    auto* R = reinterpret_cast<GcRuntime*>(user_data);
    if (!R) return;
    // Walk every participant's shadow stack (each active frame's mapped GC
    // pointer slots). Each participant's gc_frame_head is per-thread (its own
    // context_t); the snapshot captured the set of participants to scan.
    for (context_t* ctx : R->scan_snapshot) {
        if (!ctx) continue;
        for (gc::GcFrameRecord* rec = ctx->gc_frame_head; rec != nullptr; rec = rec->prev) {
            if (!rec->map || rec->map->offs.empty()) continue;
            const uintptr_t base = reinterpret_cast<uintptr_t>(rec->frame_base);
            for (int32_t off : rec->map->offs) {
                void* const* slot = reinterpret_cast<void* const*>(base + intptr_t(off));
                visitor.report(*slot);
            }
        }
    }
    // Walk the shared immutable typed-global GC-pointer words (once — the
    // descriptor is shared across all participants of one module).
    if (R->global_roots && !R->global_roots->empty()) {
        const uint64_t gbase = R->global_roots->base;
        for (int32_t off : R->global_roots->offs) {
            void* const* slot = reinterpret_cast<void* const*>(uintptr_t(gbase) + intptr_t(off));
            visitor.report(*slot);
        }
    }
}

// ---- cooperative STW safepoint (called by every GC native on this thread) ----
// If a stop-the-world collect is in progress on ANOTHER thread (a different
// thread owns the cycle via collect_owner), park this participant (mark it
// safe — its shadow stack is not mutating while it is inside this native) +
// wait for the collect to finish (an UNBOUNDED wait: the collector never
// proceeds until every other participant has parked or exited, so parking
// here is what lets the collector complete). No-op if no collect is in
// progress, this thread IS the collector (it owns the cycle + does not park
// on its own safepoint), or this thread has no participant record (e.g. the
// private thread-local fallback runtime).
static void gc_safepoint() {
    GcRuntime* R = g_rt;
    Participant* me = g_my_part;
    if (!R || !me) return;  // no shared runtime / not a participant -> no STW
    if (!R->collect_requested.load(std::memory_order_acquire)) return;
    if (R->collect_owner.load(std::memory_order_acquire) == std::this_thread::get_id())
        return;  // this thread is the collector -> do not park on its own cycle
    // Park: mark this participant safe, then wait for the collect to finish.
    me->parked.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(R->part_mtx);
        // Notify the collector that we parked (it may be waiting for us).
        R->part_cv.notify_all();
        R->part_cv.wait(lk, [&] { return !R->collect_requested.load(std::memory_order_acquire); });
    }
    me->parked.store(false, std::memory_order_release);
}

// ---- host setup / reset ----
bool gc_init() {
    return rt() != nullptr;
}

void gc_reset() {
    GcRuntime* R = g_rt;
    if (!R) return;
    // If this thread is a participant, unregister it first so the runtime's
    // participant list does not hold a dangling &ctx after reset.
    if (g_my_part) {
        std::lock_guard<std::mutex> lk(R->part_mtx);
        for (auto& p : R->participants) {
            if (p.get() == g_my_part) { p.reset(); break; }
        }
        g_my_part = nullptr;
    }
    // Unregister every pin slot from the heap, free the slot, then clear the
    // heap. clear() frees all objects + drops the heap's own root list, but we
    // free the pin slots ourselves first (the heap's root list holds raw
    // pointers into our map's slots; clear() drops the list without freeing
    // them, so a leak here would be real if we did not do it). Guard the pin
    // map under pinned_mtx (it is shared-across-participants state).
    {
        std::lock_guard<std::mutex> plk(R->pinned_mtx);
        for (auto& kv : R->pinned) {
            R->heap.remove_root(kv.second);  // kv.second is the void** slot
            delete kv.second;
        }
        R->pinned.clear();
    }
    // Clear the STW state + the participant list + the global-roots descriptor.
    R->global_roots = nullptr;
    R->scan_snapshot.clear();
    {
        std::lock_guard<std::mutex> lk(R->part_mtx);
        R->participants.clear();
        R->collect_requested.store(false);
        R->collect_owner.store(std::thread::id{});
        R->collect_generation.fetch_add(1);
    }
    R->part_cv.notify_all();
    R->heap.clear();
    // The trace callback registration is dropped by heap.clear(); re-register
    // it so subsequent collects on this runtime still walk participants.
    R->frame_cb_token = R->heap.register_trace_callback(R, &context_roots_trace_cb);
    // Clear this thread's context back-pointer if it pointed at this runtime.
    // (The host owns the context; we only clear the GC's record of it.)
}

// ---- pin / unpin ----
// The pin map + the heap root slots are shared-across-participants state on
// a shared runtime, so every access is serialized under pinned_mtx. is_live +
// add_root/remove_root are themselves concurrency-safe under the heap's m_lock;
// pinned_mtx serializes the MAP mutation + the slot alloc/free so two
// concurrent pins of the same env cannot double-register + an unpin cannot
// delete a slot another thread is inserting.
static bool pin_env(void* env) {
    GcRuntime* r = rt();
    if (!r) return false;
    if (!r->heap.is_live(env)) return false;
    std::lock_guard<std::mutex> lk(r->pinned_mtx);
    if (r->pinned.count(env)) return true;  // already pinned (idempotent)
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
    std::lock_guard<std::mutex> lk(r->pinned_mtx);
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

// Read the auto-collect threshold under pinned_mtx (it is shared-across-
// participants state). Returns 0 (auto-collect disabled) if no runtime.
static int64_t current_threshold() {
    GcRuntime* r = g_rt;
    if (!r) return 0;
    std::lock_guard<std::mutex> lk(r->pinned_mtx);
    return r->threshold;
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
// CONCURRENT ENTRY: the alloc is a GC safepoint. If a stop-the-world collect is
// in progress on another thread, this participant parks until it finishes
// (gc_safepoint). If THIS alloc triggers the auto-collect (live >= threshold),
// this thread runs the cooperative STW gc_collect (waiting for the other
// participants to park) BEFORE the alloc. Allocation itself is concurrency-safe
// under the heap's m_lock (GcHeap::alloc).
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
    gc_safepoint();  // park if another thread's collect is in progress
    // Collect-then-alloc (see the comment above): close the auto-collect window
    // between alloc and the first rooted store. threshold == 0 disables auto.
    int64_t thr = current_threshold();
    if (thr > 0 && int64_t(r->heap.stats().live_objects) >= thr) {
        gc_collect();
    }
    void* env = r->heap.alloc(size_t(size), gc::refmap_none());
    if (!env) return 0;
    return int64_t(reinterpret_cast<uintptr_t>(env));
}

// ---- deterministic new/delete: unpinned object allocation + immediate ----
// ---- destruction (the language `new`/`delete` substrate).              ----
// gc_alloc_object: UNPINNED allocation on the active runtime, with the SAME
// collect-before-allocate threshold safety (the check runs before the alloc,
// never after, closing the alloc-to-first-store window). NOT pinned —
// reachability is determined by the frame chain / global roots / extension
// callbacks / an explicit pin (gc_root_env). This is the allocation native the
// future language `new` operator lowers to (__ember_gc_alloc_object). Distinct
// from gc_new (which allocs + PINS for the legacy script-visible surface) and
// from gc_alloc_env (the lambda-env path). Shares the safepoint + STW
// cooperation with gc_alloc_env.
int64_t gc_alloc_object(int64_t size) {
    GcRuntime* r = rt();
    if (!r || size <= 0) return 0;
    gc_safepoint();  // park if another thread's collect is in progress
    // Collect-before-allocate (same safety as gc_alloc_env): if live >=
    // threshold, collect FIRST so the new object is the newest + the next
    // collect is at the next alloc, by which time the object is stored into a
    // rooted slot. threshold == 0 disables auto-collect.
    int64_t thr = current_threshold();
    if (thr > 0 && int64_t(r->heap.stats().live_objects) >= thr) {
        gc_collect();
    }
    void* obj = r->heap.alloc(size_t(size), gc::refmap_none());
    if (!obj) return 0;
    return int64_t(reinterpret_cast<uintptr_t>(obj));
}

// gc_delete_object: DETERMINISTIC immediate destruction (the `delete`
// substrate). Calls GcHeap::free_object — validates the pointer is an exact
// live GC object, removes + frees it NOW (not just unpin), decrements live
// counts/bytes, increments freed_objects. This is DIFFERENT from gc_delete
// (the compatibility native), which only UNPINS an object (marks it
// collectable for a future collect). The future language `delete` operator
// lowers to __ember_gc_delete_object -> gc_delete_object. Concurrency-safe
// under the heap's m_lock (GcHeap::free_object).
int64_t gc_delete_object(int64_t ptr) {
    GcRuntime* r = rt();
    if (!r) return 0;
    return r->heap.free_object(reinterpret_cast<void*>(ptr)) ? 1 : 0;
}

// The cooperative stop-the-world collection cycle. See the file header for the
// full protocol. The CALLING thread is the collector; it waits for every OTHER
// active participant to park (or exit), snapshots their gc_frame_heads + the
// shared global roots, runs the mark-sweep under the heap's m_lock, then
// resumes the parked participants. Returns the number of objects freed this
// cycle (the delta in cumulative freed_objects).
//
// STRICT COOPERATIVE STW (the mandated guarantee): the collector waits an
// UNBOUNDED wait (part_cv.wait, never wait_for / never a timeout) until every
// OTHER active participant has either parked at a GC safepoint (parked==true:
// it is inside a GC-cooperating native, NOT executing JIT, so its gc_frame_head
// is stable + the raw non-atomic pointer read is race-free) or exited (removed
// from the participant list). It NEVER proceeds after a timeout + NEVER scans a
// non-parked participant's gc_frame_head (which could be mutating under JIT
// prologues/epilogues -- a data race that corrupts root scanning). The
// safepoints (alloc / collect / live / delete / write_barrier / thread_enter /
// thread_exit) cover every GC-mutating operation, so a participant that does
// ANY GC work parks; the unbounded wait therefore completes in bounded wall
// time whenever every participant either does GC work or exits.
//
// COLLECTOR ELECTION (Fix: the previous check-then-set on collect_requested
// let two concurrent gc_collect callers both pass the check + both drive a STW
// cycle, racing scan_snapshot + collection state). A caller atomically claims
// ownership by CASing collect_owner from the default (no-owner) thread id to
// its own; the LOSER does NOT drive a cycle -- it parks as a participant (if it
// has a participant record) or waits for the in-progress cycle to finish, then
// runs its OWN cycle (re-trying the election). Only the owner drives the STW +
// the scan snapshot, so scan_snapshot is never raced by two collectors.
int64_t gc_collect() {
    GcRuntime* r = rt();
    if (!r) return 0;
    Participant* me = g_my_part;  // may be null (private fallback / host not a participant)

    // ── Election: atomically claim collector ownership. ─────────────────
    // CAS collect_owner from the default (no-owner) id to this thread. The
    // default std::thread::id{} compares equal to any default-constructed id
    // (the "no owner" sentinel); a real thread id (from get_id()) is unique +
    // never compares equal to the default, so the CAS is a sound election.
    std::thread::id no_owner{};
    while (!r->collect_owner.compare_exchange_strong(no_owner, std::this_thread::get_id())) {
        // Lost the election: another thread owns the cycle. Cooperate with it
        // (park as a participant so the owner's unbounded wait can complete)
        // + wait for the cycle to finish, then retry the election to run our
        // OWN cycle. If this thread has no participant record (e.g. the host
        // on a private fallback runtime with no other participants), just
        // wait for collect_requested to clear.
        if (me) {
            me->parked.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lk(r->part_mtx);
            r->part_cv.notify_all();
            r->part_cv.wait(lk, [&] { return !r->collect_requested.load(std::memory_order_acquire); });
            me->parked.store(false, std::memory_order_release);
        } else {
            std::unique_lock<std::mutex> lk(r->part_mtx);
            r->part_cv.wait(lk, [&] { return !r->collect_requested.load(std::memory_order_acquire); });
        }
        no_owner = std::thread::id{};  // refresh for the next CAS attempt
    }

    // ── 1. Request the stop-the-world + block new entries. ──────────────
    // Set collect_requested under part_mtx so gc_thread_enter (which checks it
    // under the SAME lock) cannot miss it + register a new participant mid-
    // cycle. A new entry that observes collect_requested parks at the entry
    // safepoint until the cycle finishes (Fix: the previous enter checked
    // collect_requested OUTSIDE the registration lock, letting a new
    // participant register + run after collection began).
    {
        std::lock_guard<std::mutex> lk(r->part_mtx);
        r->collect_requested.store(true, std::memory_order_release);
    }

    // ── 2. Wait (UNBOUNDED) for every OTHER active participant to park. ──
    // A participant is "quiesced" when it is parked (parked==true: inside a
    // GC safepoint native, gc_frame_head stable) OR it is the collector
    // itself (me, which is inside this native). The wait is UNBOUNDED
    // (part_cv.wait, no timeout): the collector NEVER proceeds until every
    // other participant has parked or exited, so it never scans a
    // non-parked participant's gc_frame_head (the data race that corrupts
    // root scanning). The safepoints cover every GC-mutating op, so a
    // participant that does any GC work parks; the wait completes once every
    // other participant has either parked or exited.
    auto quiesced = [&](Participant* p) {
        return p == me || p->parked.load(std::memory_order_acquire);
    };
    {
        std::unique_lock<std::mutex> lk(r->part_mtx);
        r->part_cv.wait(lk, [&] {
            for (auto& p : r->participants)
                if (!quiesced(p.get())) return false;  // keep waiting (unbounded)
            return true;  // all others parked / only me
        });
    }

    // ── 3. Snapshot the participants to scan + the shared global roots. ──
    // ONLY parked (or self) participants are snapshotted: a parked participant
    // is inside a GC native (not executing JIT), so its gc_frame_head is stable
    // + the raw non-atomic pointer read by the trace callback is race-free. A
    // non-parked participant is NEVER scanned (it would be a data race on
    // gc_frame_head). The snapshot is stable for the cycle: new entries
    // (gc_thread_enter) block on collect_requested; exiting participants
    // (gc_thread_exit) mark parked + wait for collect_done before
    // unregistering. Built under part_mtx (read), read by the trace callback
    // WITHOUT part_mtx.
    {
        std::lock_guard<std::mutex> lk(r->part_mtx);
        r->scan_snapshot.clear();
        r->scan_snapshot.reserve(r->participants.size());
        for (auto& p : r->participants)
            if (p->ctx && quiesced(p.get())) r->scan_snapshot.push_back(p->ctx);
    }

    // ── 4. Run the mark-sweep under the heap's m_lock. The heap's trace ──
    //    callback (context_roots_trace_cb, registered at runtime creation)
    //    walks r->scan_snapshot + r->global_roots + the explicit pin roots.
    size_t before = r->heap.stats().freed_objects;
    r->heap.collect();
    size_t after = r->heap.stats().freed_objects;

    // ── 5. Clear the request + release ownership + resume participants. ─
    r->scan_snapshot.clear();
    {
        std::lock_guard<std::mutex> lk(r->part_mtx);
        r->collect_requested.store(false, std::memory_order_release);
        r->collect_owner.store(std::thread::id{}, std::memory_order_release);
        r->collect_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    r->part_cv.notify_all();

    return int64_t(after - before);
}

int64_t gc_live_count() {
    GcRuntime* r = rt();
    return r ? int64_t(r->heap.stats().live_objects) : 0;
}

void gc_set_threshold(int64_t n) {
    GcRuntime* r = rt();
    if (!r) return;
    std::lock_guard<std::mutex> lk(r->pinned_mtx);
    r->threshold = n;
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

int64_t gc_barrier_count() {
    GcRuntime* r = rt();
    return r ? int64_t(r->heap.stats().barrier_calls) : 0;
}

// ---- thread-runtime facade: trace-callback + write-barrier access for ----
// ---- other extensions (operates on the active runtime).              ----
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

// gc_write_barrier records that `owner` now references `child` + COOPERATES
// with the cooperative stop-the-world protocol (Fix: the previous version
// SKIPPED while a collect was in progress, contrary to the requirement that
// the barrier park/cooperate so collection never races a payload write). When
// a collect is in progress on another thread, this participant PARKS at this
// safepoint (it is inside the barrier native, NOT executing JIT, so its
// gc_frame_head is stable + any payload write that preceded this barrier call
// is complete) + waits for the collect to finish before recording the edge +
// returning. The collector's unbounded wait therefore observes this
// participant parked + never scans a mid-write state.
//
// DEADLOCK-AVOIDANCE: parking waits on part_cv (NOT the heap m_lock) + does
// NOT take m_lock while parked, so a caller that holds an EXTERNAL mutex (the
// array/map store mutex) across this call cannot AB-BA with the heap m_lock.
// The array/map mutations are structured to call this barrier OUTSIDE their
// store mutex (see ext_array.cpp / ext_map.cpp), so no caller holds an
// external mutex while parked + the trace callbacks (which take those store
// mutexes under the heap m_lock) cannot deadlock against a parked mutator. A
// caller that cannot release its external mutex before the barrier MUST use
// gc_write_barrier_nosafepoint below (which records without parking -- only
// safe when the caller guarantees no concurrent collect can scan its state).
void gc_write_barrier(void* owner, void* child) {
    GcRuntime* r = rt();
    if (!r) return;  // no runtime yet -> safe no-op
    gc_safepoint();  // park if a collect is in progress on another thread
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

// Non-creating existence probe: returns whether the active runtime exists
// WITHOUT lazily creating it (unlike rt(), which every other facade function
// calls). Other extensions gate their GC work (trace-callback registration +
// write barriers) on this so a thread that never called gc_init stays in pure
// non-GC mode — no inert GcHeap is ever materialized there.
bool gc_runtime_initialized() {
    return g_rt != nullptr;
}

// ---- participant registration helpers (shared by attach / thread_enter) ----
// Add a participant for `ctx` on `r` (under part_mtx). Idempotent: if `ctx` is
// already registered, reuses its record. Sets g_my_part (TLS) to the record +
// returns it. Records the runtime pointer on ctx.gc_runtime + the shared
// global-roots descriptor on ctx.gc_global_roots.
static Participant* register_participant(GcRuntime* r, context_t* ctx) {
    if (!r || !ctx) return nullptr;
    std::lock_guard<std::mutex> lk(r->part_mtx);
    for (auto& p : r->participants) {
        if (p->ctx == ctx) { g_my_part = p.get(); ctx->gc_runtime = r; return p.get(); }
    }
    auto rec = std::make_unique<Participant>();
    rec->ctx = ctx;
    Participant* raw = rec.get();
    r->participants.push_back(std::move(rec));
    g_my_part = raw;
    ctx->gc_runtime = r;
    ctx->gc_global_roots = r->global_roots;
    return raw;
}

// Remove this thread's participant from its runtime (under part_mtx). If a
// stop-the-world collect is in progress on another thread, mark the
// participant parked first + wait for collect_done so the collector never
// scans a context that is being torn down (the scan snapshot was taken under
// part_mtx; the exiting participant stays in the vector + parked until the
// cycle finishes, then is removed here). Clears g_my_part (TLS).
static void unregister_participant() {
    GcRuntime* r = g_rt;
    Participant* me = g_my_part;
    if (!r || !me) { g_my_part = nullptr; return; }
    // If a collect is in progress, park + wait for it to finish so the
    // collector does not scan our (about-to-be-torn-down) context after we
    // leave. This is the trap/longjmp cleanup path: an abandoned participant
    // record cannot remain registered + the collector cannot dereference a
    // freed context.
    if (r->collect_requested.load(std::memory_order_acquire)) {
        me->parked.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lk(r->part_mtx);
        r->part_cv.notify_all();
        r->part_cv.wait(lk, [&] { return !r->collect_requested.load(std::memory_order_acquire); });
        me->parked.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lk(r->part_mtx);
        for (auto it = r->participants.begin(); it != r->participants.end(); ++it) {
            if (it->get() == me) { r->participants.erase(it); break; }
        }
    }
    g_my_part = nullptr;
}

bool gc_attach_context(context_t* ctx, gc::GcGlobalRoots* global_roots) {
    if (!ctx) return false;
    GcRuntime* r = rt();  // creates a shared runtime on demand (registers the trace cb)
    if (!r) return false;
    // Install the shared global-roots descriptor on the runtime (idempotent —
    // every participant of one module shares the same descriptor).
    r->global_roots = global_roots;
    // Register this context as a participant (sets g_my_part + ctx.gc_runtime +
    // ctx.gc_global_roots). Idempotent re-attach reuses the record.
    register_participant(r, ctx);
    ctx->gc_global_roots = global_roots;
    return true;
}

void gc_detach_context(context_t* ctx) {
    GcRuntime* r = g_rt;
    if (!r) return;
    // If this thread is the participant for ctx (or for any context — a host
    // thread has one participant), unregister it so the runtime's collector no
    // longer scans this context's shadow stack. Clear the context's GC
    // back-pointer. Safe to call when no participant is registered (no-op).
    if (g_my_part) unregister_participant();
    if (ctx) ctx->gc_global_roots = nullptr;
    if (ctx && ctx->gc_runtime == r) ctx->gc_runtime = nullptr;
}

bool gc_thread_enter(void* runtime_opaque, context_t* ctx) {
    if (!runtime_opaque || !ctx) return false;
    // Look up the shared runtime by raw pointer (the host context's gc_runtime
    // back-pointer) + lock a shared_ptr so the worker keeps it alive for its
    // whole execution. If the runtime was destroyed (expired weak_ptr), fail
    // closed -- the worker runs on a private thread-local fallback heap.
    std::shared_ptr<GcRuntime> sp;
    {
        std::lock_guard<std::mutex> lk(g_shared_mtx);
        auto it = g_shared_runtimes.find(static_cast<GcRuntime*>(runtime_opaque));
        if (it != g_shared_runtimes.end()) sp = it->second.lock();
    }
    if (!sp) return false;  // runtime gone -- fail closed
    // Make this the active runtime on the worker thread + keep it alive.
    g_owned_rt = nullptr;  // a shared runtime is now active; clear any private one
    g_rt_hold = sp;
    g_rt = sp.get();
    // Register the worker's context as a participant (joins the shared heap +
    // contributes its per-thread gc_frame_head to the collector). Inherits the
    // shared global-roots descriptor. Fix: the previous version checked
    // collect_requested OUTSIDE the registration lock, so a new participant
    // could register + run AFTER a collection began (the check passed, then the
    // collect started between the check and the registration). Now the
    // registration + the collect-requested check happen ATOMICALLY under
    // part_mtx: if a collect is in progress (or begins between the lookup and
    // here), the new participant is registered FIRST (so the collector's
    // unbounded wait sees it) + then immediately parks at this entry safepoint
    // until the collect finishes, so it never mutates the heap / its shadow
    // stack mid-cycle.
    GcRuntime* R = sp.get();
    Participant* me = register_participant(R, ctx);
    ctx->gc_global_roots = R->global_roots;
    if (me) {
        me->parked.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lk(R->part_mtx);
        R->part_cv.notify_all();  // the collector may be waiting for us to park
        R->part_cv.wait(lk, [&] { return !R->collect_requested.load(std::memory_order_acquire); });
        me->parked.store(false, std::memory_order_release);
    }
    return true;
}

void gc_thread_exit(context_t* ctx) {
    // Unregister this thread's participant (waiting for any in-progress
    // collect first so the collector never scans a torn-down context). This is
    // the trap/longjmp cleanup path: called on EVERY worker exit (normal AND
    // trapped), so an abandoned participant record + shadow-stack head cannot
    // remain registered after a trap unwinds the worker's call.
    unregister_participant();
    if (ctx) ctx->gc_global_roots = nullptr;
    if (ctx && g_rt && ctx->gc_runtime == g_rt) ctx->gc_runtime = nullptr;
    // Drop this thread's hold on the shared runtime. rt() will lazily create a
    // private thread-local runtime if the thread allocates again (a worker
    // normally does not after exit).
    g_rt_hold.reset();
    g_rt = nullptr;
}

// Cooperative-STW safepoint for a participant blocked in a non-GC native
// (thread_join). Delegates to gc_safepoint: if a collect is in progress on
// another thread, park this participant (its gc_frame_head is stable while it
// is blocked in the native) + wait for the collect to finish. thread_join
// calls this in its wait loop so a concurrent gc_collect's unbounded wait
// observes the joiner parked + can proceed (see ext_gc.hpp). No-op when no
// collect is in progress, this thread is the collector, or there is no
// participant record.
void gc_park() {
    gc_safepoint();
}

// ---- the natives (JIT'd code calls these by name) ----
extern "C" {

// __ember_gc_alloc_env(i64 size) -> i64 ptr
// Allocate a lambda env of `size` user bytes. Returns the env pointer as an i64
// (0 on failure). codegen's lambda-env case calls this when CodeGenCtx::use_gc_env
// is set, then copies each capture into [ptr + offset] and stores ptr as the
// lambda value's env_ptr half. NOT pinned (precise root scanning via the frame
// chain / global roots / extension callbacks / explicit pin). Cooperates with
// the cooperative STW safepoint.
static int64_t n_gc_alloc_env(int64_t size) {
    return gc_alloc_env(size);
}

// __ember_gc_collect() -> i64 freed
// Run a cooperative stop-the-world collection cycle. Returns the number of
// objects freed this cycle. Pinned envs survive; unpinned unreachable envs are
// swept.
static int64_t n_gc_collect() {
    return gc_collect();
}

// __ember_gc_live() -> i64 count
// Live object count on the active runtime. Test/diagnostic native. Parks at the
// safepoint so a concurrent collect cannot race the stats read.
static int64_t n_gc_live() {
    gc_safepoint();
    return gc_live_count();
}

// ---- script-visible new/delete natives (full GC integration task) ----
// These are the user-facing surface for script-visible heap objects: a script
// can `gc_new(size)` to allocate a raw GC heap object (pinned, so it survives
// auto-collects), `gc_delete(ptr)` to unroot it (mark it collectable), and
// `gc_collect()`/`gc_live()` to run a collection cycle + observe the live
// count. This is the v1 new/delete: a raw handle (i64) the script can hold +
// release, with the tracing GC reclaiming unreachable (unpinned) objects.
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
    gc_safepoint();
    return gc_unroot_env(ptr) ? 1 : 0;  // unpin -> collectable; 1 if it was pinned
}

// ---- deterministic new/delete natives (the language `new`/`delete` ----
// ---- substrate; stable contract for the compiler).                  ----
// __ember_gc_alloc_object(i64 size) -> i64 ptr
// Allocate an UNPINNED raw GC heap object of `size` user bytes (the `new`
// substrate). Same runtime + collect-before-allocate threshold safety as
// __ember_gc_alloc_env. NOT pinned (reachability via the frame chain / global
// roots / extension callbacks / an explicit pin). Returns the object pointer as
// an i64 (0 on failure). The future language `new` operator lowers to this
// native. Distinct from gc_new (alloc+pin) and __ember_gc_alloc_env (lambda-env
// path). Cooperates with the cooperative STW safepoint.
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
// resurrection). Concurrency-safe under the heap's m_lock.
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
// would build on. Parks at the safepoint so a concurrent collect cannot race
// the barrier. Returns 0 (the i64 placeholder so the native has a stable
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
    b.add("__ember_gc_alloc_env", T, {T}, (void*)&n_gc_alloc_env);
    b.add("__ember_gc_collect",    T, {}, (void*)&n_gc_collect);
    b.add("__ember_gc_live",       T, {}, (void*)&n_gc_live);
    b.add("__ember_gc_write_barrier", T, {T, T}, (void*)&n_gc_write_barrier);
    b.add("gc_new",     T, {T},  (void*)&n_gc_new);
    b.add("gc_delete",  T, {T},  (void*)&n_gc_delete);
    b.add("gc_collect", T, {},   (void*)&n_gc_collect);
    b.add("gc_live",    T, {},   (void*)&n_gc_live);
    b.add("__ember_gc_alloc_object",  T, {T}, (void*)&n_gc_alloc_object);
    b.add("__ember_gc_delete_object", T, {T}, (void*)&n_gc_delete_object);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_gc
