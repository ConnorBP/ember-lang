// ext_thread.cpp - ember extension: in-context threads (Tier 4).
// See ext_thread.hpp for the scope statement + correctness model.
//
// CONCURRENT ENTRY (replaces the legacy save/restore + call_mutex
// serialization + the keyed worker's disconnected private context_t). Both the
// legacy + keyed workers now enter JIT'd code CONCURRENTLY through the one
// shared dispatch context active at thread_spawn, each with its OWN per-call
// context_t seeded from the host context's settings (budget, max_call_depth,
// the shared typed-global root descriptor, the shared GC runtime pointer).
// There is NO call_mutex + NO save/restore: the host's context_t is untouched
// while workers run concurrently, so true parallel execution is possible
// (>= 2 workers inside JIT at once) and catch stacks / call depth / trap
// checkpoints do NOT cross threads.
//
//   * Legacy worker: seeds from the context supplied to thread_init (the
//     host's context) + resolves its entry from the registered dispatch table
//     (atomic acquire read). Enters via ember_call_i64 with its own context.
//   * Keyed worker (§6.5, §6.6, §9.8, §10.3, §12.4): captures a
//     shared_ptr<ModuleInstance> (shared lifetime ownership) so the instance
//     + its borrowed dispatch record / provider / entry table stay alive for
//     the worker's whole execution; captures the current shared context from
//     the keyed host boundary (ember_current_keyed_context) as the seed
//     source; re-resolves its logical entry at EXECUTION time through the
//     GUARDED CORE keyed-call API (ember_call_keyed_i64_by_slot) — the core
//     API establishes the current-runtime TLS, the recoverable checkpoint, the
//     reload/generation guard, and cleans up on every normal AND trapped exit.
//     The raw ember_keyed_reentry_i64 thunk does NONE of those and MUST NOT be
//     used here (task constraint).
//
// KEYED FAIL-CLOSED (task constraint): when the TLS current-keyed-runtime is
// active but ext_state is null, the keyed store selector does NOT fall back to
// the file-static singleton — it fails closed (spawn returns 0). Only when NO
// keyed runtime is active does the legacy identity path target the file-static
// default store.
//
// JOIN LOCKING (task constraint): thread_join + thread_join_keyed wait ONLY on
// slot synchronization (done + the OS thread join). They do NOT unlock/relock a
// shared call_mutex (there is none), so nested spawn/join is deadlock-free.
//
// GC INTEGRATION: when the seed context carries a shared GC runtime
// (ctx.gc_runtime), the worker joins it (ext_gc::gc_thread_enter) before
// entering JIT + leaves it (ext_gc::gc_thread_exit) on every normal + trapped
// exit, so the worker shares the ONE context-owned heap + its shadow-stack head
// is scanned by the cooperative stop-the-world collector, and a trap cannot
// leave an abandoned participant record.
//
// Layout-safety: both stores use the SAME ThreadSlot type
// (ThreadRuntimeState::ThreadSlot), no reinterpret_cast aliasing.
#include "ext_thread.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include "engine.hpp"
#include "module_instance.hpp"
#include "key_provider.hpp"
#include "runtime_extension_state.hpp"
#include "../gc/ext_gc.hpp"     // gc_thread_enter / gc_thread_exit (shared heap)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ember;

namespace ember::ext_thread {

static constexpr int64_t TRAP_SENTINEL = INT64_MIN;

using ThreadSlot = ThreadRuntimeState::ThreadSlot;

static std::recursive_mutex g_setup_mutex;
static context_t*           g_ctx           = nullptr;
static void*                g_dispatch_base = nullptr;
static int64_t              g_slot_count    = 0;
static std::vector<std::unique_ptr<ThreadSlot>> g_threads;
static std::vector<int64_t>                     g_threads_free;

// Returns the per-runtime keyed state when a keyed runtime is active on this
// thread. nullptr when no keyed runtime is active (legacy path). A sentinel
// when keyed TLS is active but ext_state is null (fail-closed).
static ThreadRuntimeState* g_fail_closed_sentinel = nullptr;
static ThreadRuntimeState* fail_closed_sentinel() {
    if (!g_fail_closed_sentinel)
        g_fail_closed_sentinel = reinterpret_cast<ThreadRuntimeState*>(uintptr_t(0x1));
    return g_fail_closed_sentinel;
}

static ThreadRuntimeState* current_keyed_state() {
    ModuleInstance* rt = ember_current_keyed_runtime();
    if (!rt) return nullptr;  // no keyed runtime active -> legacy path
    if (!rt->ext_state) return fail_closed_sentinel();  // keyed active, no state -> fail closed
    if (!rt->ext_state->thread.keyed_mode) return fail_closed_sentinel();  // not keyed-init -> fail closed
    return &rt->ext_state->thread;
}

struct StoreView {
    std::vector<std::unique_ptr<ThreadSlot>>* threads;
    std::vector<int64_t>*                     free_ids;
    std::recursive_mutex*                     setup_mutex;
    context_t**                               ctx_pp;
    void**                                    dispatch_base_pp;
    int64_t*                                  slot_count_p;
    ModuleInstance*                           instance;
    bool keyed;
    bool valid;
};

static StoreView select_store() {
    ThreadRuntimeState* s = current_keyed_state();
    if (s == fail_closed_sentinel()) {
        // Keyed TLS active but ext_state null or keyed_mode off: fail closed.
        return StoreView{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, /*keyed=*/true, /*valid=*/false};
    }
    if (s) {
        return StoreView{&s->threads, &s->free_ids, &s->setup_mutex,
                         &s->ctx, &s->dispatch_base, &s->slot_count,
                         s->instance, /*keyed=*/true, /*valid=*/true};
    }
    // No keyed runtime active: legacy identity path -> file-static default store.
    return StoreView{&g_threads, &g_threads_free, &g_setup_mutex,
                     &g_ctx, &g_dispatch_base, &g_slot_count,
                     nullptr, /*keyed=*/false, /*valid=*/true};
}

static ThreadSlot* raw_slot(StoreView& st, int64_t h) {
    if (h < 1 || h > int64_t(st.threads->size())) return nullptr;
    auto& s = (*st.threads)[size_t(h - 1)];
    return (s && s->in_use && !s->joined) ? s.get() : nullptr;
}

// Like raw_slot but also allows already-joined slots (for thread_trap_reason,
// which the script calls AFTER thread_join — the trap reason must remain
// accessible after the join retires the slot).
static ThreadSlot* raw_slot_or_joined(StoreView& st, int64_t h) {
    if (h < 1 || h > int64_t(st.threads->size())) return nullptr;
    auto& s = (*st.threads)[size_t(h - 1)];
    return (s && s->in_use) ? s.get() : nullptr;
}

static void* resolve_entry_legacy(int64_t handle, void* dispatch_base, int64_t slot_count) {
    if (handle < 0) return nullptr;
    if (slot_count <= 0 || handle >= slot_count) return nullptr;
    if (!dispatch_base) return nullptr;
    auto* slots = static_cast<std::atomic<void*>*>(dispatch_base);
    return slots[size_t(handle)].load(std::memory_order_acquire);
}

// The worker seed settings, captured BY VALUE at thread_spawn time (Fix: the
// previous version passed a raw host context_t* to the worker, which then read
// budget/max_call_depth/gc fields from it LATER -- by which time the host could
// have mutated budget/state between spawn and the worker's seed read, a data
// race on the host context). Capturing the seed settings by value at spawn +
// copying them into the std::thread's argument makes the worker's seed a
// stable snapshot of the host context's settings at the spawn instant. The
// gc_global_roots descriptor + the gc_runtime back-pointer are shared
// (immutable / owned by the runtime) so copying the pointers is correct.
struct SeedSettings {
    int64_t budget_remaining = 2'000'000'000LL;
    int32_t max_call_depth   = DEFAULT_MAX_CALL_DEPTH;
    gc::GcGlobalRoots* gc_global_roots = nullptr;  // shared immutable descriptor
    void*   gc_runtime = nullptr;                  // shared runtime back-pointer (or null)
    bool    valid = false;                          // a seed context was captured
};

// Capture the seed settings BY VALUE from a host context (at spawn time).
static SeedSettings capture_seed(const context_t* seed) {
    SeedSettings s;
    if (seed) {
        s.budget_remaining = seed->budget_remaining;
        s.max_call_depth   = seed->max_call_depth;
        s.gc_global_roots  = seed->gc_global_roots;  // shared immutable descriptor
        s.gc_runtime       = seed->gc_runtime;       // shared runtime back-pointer
        s.valid = true;
    }
    return s;
}

// Seed a worker's OWN per-call context_t from the captured seed settings. The
// worker gets private checkpoint / budget / call depth / catch stack /
// gc_frame_head (so concurrent entry + trap isolation + catch isolation hold),
// while sharing the host's max_call_depth, the shared typed-global root
// descriptor, and the shared GC runtime back-pointer. The trap stub is baked
// into the JIT'd code (a direct call address), so the worker's per-call context
// does NOT carry it -- the stub receives the worker's context via r14 and
// longjmps to THIS context's checkpoint (per-worker trap isolation). Returns
// the shared GC runtime opaque pointer (or nullptr) so the worker can join the
// shared heap.
static void* seed_worker_context(context_t& wctx, const SeedSettings& seed) {
    wctx.budget_remaining = seed.budget_remaining;
    wctx.max_call_depth   = seed.max_call_depth;
    wctx.gc_global_roots  = seed.gc_global_roots;  // shared immutable descriptor
    return seed.gc_runtime;                        // shared runtime back-pointer (or null)
}

// ── Legacy worker: concurrent entry through the shared dispatch table ───────
// Owns its OWN per-call context_t (seeded from the SeedSettings captured BY
// VALUE at thread_spawn) so it runs CONCURRENTLY with the host + siblings -- no
// call_mutex, no save/restore. Shares the immutable dispatch table (entry
// resolved via an atomic acquire read) + the context-owned GC heap (joined via
// gc_thread_enter when the seed carries a shared runtime). The trap stub
// longjmps to THIS worker's checkpoint (setjmp below), so a trap unwinds to the
// worker, never the host.
static void thread_worker_legacy(void* entry, SeedSettings seed, int64_t arg,
                                 ThreadSlot* slot) {
    context_t wctx;                         // the worker's OWN per-call context
    void* gc_runtime = seed_worker_context(wctx, seed);

    bool joined_gc = false;
    if (gc_runtime) {
        // Join the shared heap BEFORE entering JIT so the worker's allocations
        // land in the ONE context-owned heap + its shadow-stack head is scanned.
        joined_gc = ext_gc::gc_thread_enter(gc_runtime, &wctx);
    }

    int64_t result = 0;
    bool trapped = false;
    int  reason  = 0;

    wctx.has_checkpoint = true;
    // EMBER_SETJMP (not raw setjmp): the JIT'd trap stub longjmps via
    // EMBER_LONGJMP (__builtin_longjmp on MinGW), which expects a
    // __builtin_setjmp-format buffer. Raw setjmp saves fewer registers and
    // uses an incompatible buffer layout, so mixing setjmp + __builtin_longjmp
    // corrupts callee-saved state across the trap unwind and segfaults the
    // worker. The macros in context.hpp resolve to the matching primitive.
    if (EMBER_SETJMP(wctx.checkpoint)) {
        // Trap unwound to THIS worker's checkpoint. The host's context is
        // untouched (it has its own checkpoint). Record the trap + clean up.
        trapped = true;
        reason  = int(wctx.last_trap);
        wctx.has_checkpoint = false;
        wctx.reset_for_call();  // clear the abandoned shadow-stack head
    } else {
        result = ember_call_i64(entry, &wctx, arg);
        wctx.has_checkpoint = false;
    }

    // Leave the shared heap on EVERY exit (normal + trapped) so no abandoned
    // participant record remains registered after a trap longjmp.
    if (joined_gc) ext_gc::gc_thread_exit(&wctx);

    {
        std::lock_guard<std::mutex> g(slot->done_lock);
        slot->result      = result;
        slot->trapped     = trapped;
        slot->trap_reason = reason;
        slot->done        = true;
    }
    slot->done_cv.notify_all();
}

// ── Keyed worker (§6.5, §6.6, §9.8, §10.3, §12.4) ──────────────────────────
// Captures shared_ptr<ModuleInstance> (shared lifetime ownership) so the
// instance + its borrowed dispatch record / provider / entry table stay alive
// for the worker's whole execution. Seeds its OWN per-call context_t from the
// SeedSettings captured BY VALUE at the keyed host boundary at spawn (so the
// host cannot mutate the worker's seed budget/state between spawn + the
// worker's seed read). Re-resolves + invokes through the GUARDED core keyed-
// call API (ember_call_keyed_i64_by_slot) — the core API sets the TLS current-
// keyed-runtime on this worker thread (so any keyed native invoked by the
// worker's script finds THIS runtime's per-runtime state), derives the route
// word from the provider, resolves the logical slot through the instance's
// immutable ModuleDispatchRecord, acquires the generation guard, establishes
// the checkpoint, and cleans everything up on normal AND trapped exit. The raw
// thunk would skip all of this and let keyed natives select legacy globals.
//
// The ThreadSlot* is valid for the worker's lifetime: the shared_ptr<ModuleInstance>
// keeps ext_state (and thus the ThreadRuntimeState + the threads vector + this
// slot) alive. The host retires the slot only AFTER joining (which waits for
// done), so the slot is never reset while the worker is still executing.
static void thread_worker_keyed(std::shared_ptr<ModuleInstance> inst,
                                int64_t logical_handle, int64_t arg,
                                SeedSettings seed,
                                ThreadRuntimeState::WorkerGate* gate,
                                ThreadSlot* slot) {
    // If a test gate is installed, block here BEFORE resolving the entry — so
    // a test can deterministically publish a replacement between spawn and
    // the worker's execution-time resolution (§12.4).
    if (gate && gate->enabled) {
        std::unique_lock<std::mutex> lk(gate->mtx);
        gate->cv.wait(lk, [&] { return gate->open.load(); });
    }

    // The worker owns its own per-call context (§6.6), seeded from the
    // SeedSettings captured BY VALUE at the keyed host boundary at spawn (a
    // stable snapshot -- the host cannot mutate the worker's seed between spawn
    // + this read). No shared call_mutex.
    context_t wctx;
    void* gc_runtime = seed_worker_context(wctx, seed);

    bool joined_gc = false;
    if (gc_runtime) {
        joined_gc = ext_gc::gc_thread_enter(gc_runtime, &wctx);
    }

    int64_t result  = 0;
    bool    trapped = false;
    int     reason  = 0;

    // Re-resolve + invoke through the guarded core keyed-call API (§9.8, §12.4).
    // The core API sets the TLS current-keyed-runtime on this worker thread
    // (so any keyed native invoked by the worker's script finds THIS runtime's
    // per-runtime state), derives the route word from the provider, resolves
    // the logical slot through the instance's immutable ModuleDispatchRecord,
    // acquires the generation guard, establishes the checkpoint, and cleans
    // everything up on normal AND trapped exit. The raw thunk would skip all
    // of this and let keyed natives select legacy globals (the bug).
    DispatchKeyAdapter adapter(inst->provider);
    auto cr = ember_call_keyed_i64_by_slot(*inst, uint32_t(logical_handle),
                                           wctx, arg, adapter);
    if (cr.ok) {
        result = cr.value;
    } else if (cr.trapped) {
        trapped = true;
        // The core API already reset wctx via reset_for_call on the trapped
        // exit, which CLEARS wctx.last_trap -- so reading wctx.last_trap here
        // would yield TrapReason::None (0), the bug where the keyed worker
        // reported reason 0. The numeric reason is captured in cr.trap_reason
        // (set inside keyed_call_core BEFORE reset_for_call clears ctx.last_trap);
        // the structured cr.reason string is the authoritative diagnostic.
        reason = int(cr.trap_reason);
    }
    // On a pre-entry failure (provider unavailable, resolve failed), cr.ok is
    // false and cr.trapped is false: result stays 0, trapped stays false. The
    // structured cr.reason carries the diagnostic (not stored on the slot,
    // but the join returns 0 / a failure indicator).

    // Leave the shared heap on every exit (the keyed core already cleaned the
    // TLS + guard + checkpoint on its normal/trapped exit; this cleans the GC
    // participant record so a trap cannot leave it registered).
    if (joined_gc) ext_gc::gc_thread_exit(&wctx);

    {
        std::lock_guard<std::mutex> g(slot->done_lock);
        slot->result      = trapped ? TRAP_SENTINEL : result;
        slot->trapped     = trapped;
        slot->trap_reason = reason;
        slot->done        = true;
    }
    slot->done_cv.notify_all();
    // The shared_ptr<ModuleInstance> is released when this function returns
    // (the thread's internal copy is destroyed after the function returns).
    // If this was the last reference, ModuleInstance is destroyed, which
    // releases ext_state and triggers ThreadRuntimeState's destructor — which
    // detaches this thread (it cannot join itself). The worker has already
    // published done=true, so the host's join (if any) completed or will
    // complete via the destructor's join of non-current threads.
}

extern "C" {

static int64_t n_thread_spawn(int64_t handle, int64_t arg) {
    StoreView st = select_store();
    if (!st.valid) return 0;  // keyed fail-closed

    // For the keyed path, lock the weak_ptr NOW (before acquiring the setup
    // mutex) so the worker's shared_ptr<ModuleInstance> is established before
    // the slot is allocated. The weak_ptr lives on the TLS runtime's
    // ThreadRuntimeState; st does not carry it. Also capture the worker seed
    // settings BY VALUE from the current shared context NOW (at spawn time) so
    // the worker's seed is a stable snapshot -- the host cannot mutate the
    // worker's budget/state between spawn + the worker's seed read.
    std::shared_ptr<ModuleInstance> inst_sp;
    SeedSettings seed;
    if (st.keyed) {
        ModuleInstance* rt = ember_current_keyed_runtime();
        if (!rt || !rt->ext_state) return 0;
        inst_sp = rt->ext_state->thread.instance_wp.lock();
        if (!inst_sp) return 0;  // fail-closed: no shared ownership available
        if (!inst_sp->provider) return 0;
        if (handle < 0 || uint32_t(handle) >= inst_sp->logical_slot_count) return 0;
        // Capture the seed settings from the shared host context active at the
        // keyed host boundary (§6.6 concurrent-entry integration). A null
        // boundary yields a default-seeded context (capture_seed handles it).
        seed = capture_seed(ember_current_keyed_context());
    } else {
        // Legacy path: resolve the entry now (the legacy worker caches it).
        // The seed context is the host context registered via thread_init.
        if (!*st.ctx_pp || !*st.dispatch_base_pp || *st.slot_count_p <= 0) return 0;
        void* entry = resolve_entry_legacy(handle, *st.dispatch_base_pp, *st.slot_count_p);
        if (!entry) return 0;
        seed = capture_seed(*st.ctx_pp);
    }

    std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);

    int64_t idx;
    ThreadSlot* raw;
    if (!st.free_ids->empty()) {
        idx = st.free_ids->back();
        st.free_ids->pop_back();
        (*st.threads)[size_t(idx - 1)] = std::make_unique<ThreadSlot>();
        raw = (*st.threads)[size_t(idx - 1)].get();
    } else {
        if (st.threads->size() >= (size_t(1) << 20)) return 0;
        st.threads->push_back(std::make_unique<ThreadSlot>());
        idx = int64_t(st.threads->size());
        raw = st.threads->back().get();
    }
    raw->in_use  = true;
    raw->joined  = false;
    raw->done    = false;
    raw->trapped = false;
    raw->trap_reason = 0;
    raw->result  = 0;
    raw->logical_handle = handle;
    raw->arg     = arg;

    try {
        if (st.keyed) {
            // Pass the test gate (if installed) so the worker blocks before
            // resolving — enables deterministic delayed-replacement tests.
            ThreadRuntimeState::WorkerGate* gate = nullptr;
            ModuleInstance* rt = ember_current_keyed_runtime();
            if (rt && rt->ext_state) gate = rt->ext_state->thread.worker_gate.get();
            raw->th = std::thread(thread_worker_keyed, inst_sp, handle, arg,
                                  seed, gate, raw);
        } else {
            void* entry = resolve_entry_legacy(handle, *st.dispatch_base_pp, *st.slot_count_p);
            raw->th = std::thread(thread_worker_legacy, entry, seed, arg, raw);
        }
    } catch (const std::exception&) {
        raw->in_use = false;
        (*st.threads)[size_t(idx - 1)].reset();
        st.free_ids->push_back(idx);
        return 0;
    }
    return idx;
}

static int64_t n_thread_join(int64_t handle) {
    StoreView st = select_store();
    if (!st.valid) return 0;  // keyed fail-closed
    ThreadSlot* s;
    {
        std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
        if (!*st.ctx_pp) return 0;
        s = raw_slot(st, handle);
    }
    if (!s) return 0;

    // CONCURRENT-ENTRY JOIN: wait ONLY on slot synchronization (the done flag
    // + the OS thread join). There is NO shared call_mutex to release/relock
    // (the worker owns its own per-call context + never serialized on the
    // host's), so join never unlocks a mutex it did not lock. This keeps
    // nested spawn/join deadlock-free (a worker that spawns + joins a
    // grandchild does not contend on the outer context's lock).
    //
    // GC COOPERATION: the wait is a bounded-poll loop (done_cv.wait_for with a
    // short interval) that calls ext_gc::gc_park() each iteration. A
    // participant blocked in thread_join is NOT executing JIT'd code, so its
    // gc_frame_head is stable -- it is a valid GC safepoint. Parking here lets
    // a concurrent gc_collect's unbounded wait observe the joiner parked +
    // proceed, instead of waiting forever for a joiner that is itself waiting
    // for the collector's worker to finish (a deadlock). The poll interval is
    // short so the collector's wait completes in bounded wall time.
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(s->done_lock);
        while (!s->done) {
            ext_gc::gc_park();  // park if a GC collect is in progress
            s->done_cv.wait_for(dlk, std::chrono::milliseconds(1),
                                [&] { return s->done; });
        }
        trapped = s->trapped;
        result  = s->result;
    }
    if (s->th.joinable()) s->th.join();
    // Mark the slot as joined (the std::thread is now non-joinable). Keep
    // in_use=true so thread_trap_reason (called by the script AFTER join) can
    // still read the slot's trap_reason. The slot is NOT recycled to the free
    // list yet — the trap_reason must remain accessible. The slot is cleaned
    // up by thread_reset or the ThreadRuntimeState destructor.
    {
        std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
        s->joined = true;
    }
    return trapped ? TRAP_SENTINEL : result;
}

static int64_t n_thread_trap_reason(int64_t handle) {
    StoreView st = select_store();
    if (!st.valid) return 0;  // keyed fail-closed
    std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
    ThreadSlot* s = raw_slot_or_joined(st, handle);
    if (!s) return 0;
    std::lock_guard<std::mutex> dlk(s->done_lock);
    return int64_t(s->trap_reason);
}

} // extern "C"

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type T = type_i64();
    Type fn_param = type_i64();
    fn_param.is_fn_handle = true;
    b.add("thread_spawn",        T, {fn_param, type_i64()},        (void*)&n_thread_spawn);
    b.add("thread_join",         type_i64(), {T},                  (void*)&n_thread_join);
    b.add("thread_trap_reason",  type_i64(), {T},                  (void*)&n_thread_trap_reason);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

bool thread_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count) {
    if (!ctx || !dispatch_base || slot_count <= 0) return false;
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    g_ctx           = ctx;
    g_dispatch_base = dispatch_base;
    g_slot_count    = slot_count;
    return true;
}

void thread_reset() {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    for (auto& s : g_threads) {
        if (s && s->in_use && s->th.joinable()) s->th.detach();
    }
    g_threads.clear();
    g_threads_free.clear();
    g_ctx           = nullptr;
    g_dispatch_base = nullptr;
    g_slot_count    = 0;
}

bool thread_init_keyed(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return false;
    if (!inst.entry_table || inst.logical_slot_count == 0) return false;
    if (!inst.provider) return false;
    // thread_init_keyed requires the instance to be shared_ptr-managed so the
    // keyed worker can capture shared_ptr<ModuleInstance> (shared lifetime
    // ownership). shared_from_this() throws bad_weak_ptr if the instance was
    // not created via make_shared — catch and fail closed.
    std::shared_ptr<ModuleInstance> self;
    try {
        self = inst.shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return false;  // not make_shared-managed -> keyed threads unavailable
    }
    auto& s = inst.ext_state->thread;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    s.ctx           = nullptr;  // keyed worker seeds its own context from the host boundary
    s.dispatch_base = const_cast<void*>(static_cast<const void*>(inst.entry_table->slots.data()));
    s.slot_count    = int64_t(inst.logical_slot_count);
    s.instance      = &inst;
    s.instance_wp   = std::weak_ptr<ModuleInstance>(self);  // shared-ownership back-pointer
    s.keyed_mode    = true;
    if (inst.record.physical_slots == nullptr && inst.logical_slot_count > 0)
        assemble_identity_dispatch_record(inst);
    return true;
}

// thread_join_keyed (§6.6, §12.4): the keyed worker seeds its own per-call
// context from the shared host context captured at the keyed host boundary (no
// shared call_mutex), so join is a simple wait-for-done + thread-join + slot-
// retire. It does NOT unconditionally unlock a mutex it never locked. The
// context parameter is retained for API compatibility but is NOT used for
// locking (the keyed worker has its own context); the adapter is retained for
// API compatibility (the worker re-derives its own adapter from the instance's
// provider).
int64_t thread_join_keyed(ember::ModuleInstance& inst, int64_t handle,
                          ember::context_t& /*ctx*/,
                          const ember::DispatchKeyAdapter& /*adapter*/) {
    if (!inst.ext_state) return 0;
    auto& s = inst.ext_state->thread;
    ThreadSlot* slot;
    {
        std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
        // Direct slot lookup on this runtime's thread state (no StoreView
        // temporary needed: we have the state reference directly).
        if (handle < 1 || handle > int64_t(s.threads.size())) return 0;
        auto& sptr = s.threads[size_t(handle - 1)];
        slot = (sptr && sptr->in_use && !sptr->joined) ? sptr.get() : nullptr;
    }
    if (!slot) return 0;

    // Wait for the worker to publish done. No call_mutex — the keyed worker
    // owns its own per-call context (§6.6), so there is no shared-context lock
    // to manage. Ownership-explicit: we lock/unlock only slot->done_lock.
    // GC COOPERATION: a bounded-poll loop + gc_park() each iteration so a
    // concurrent gc_collect observes the joiner parked (see n_thread_join).
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(slot->done_lock);
        while (!slot->done) {
            ext_gc::gc_park();
            slot->done_cv.wait_for(dlk, std::chrono::milliseconds(1),
                                   [&] { return slot->done; });
        }
        trapped = slot->trapped;
        result  = slot->result;
    }
    // Join the OS thread (deterministic: the worker has published done, so
    // join returns immediately or after the worker's final cleanup).
    if (slot->th.joinable()) slot->th.join();
    // Mark the slot as joined (the std::thread is now non-joinable). Keep
    // in_use=true so any post-join query (e.g. a keyed trap_reason extension
    // if added later) can still read the slot. The slot is cleaned up by the
    // ThreadRuntimeState destructor (deterministic join/detach of all slots).
    {
        std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
        slot->joined = true;
    }
    return trapped ? TRAP_SENTINEL : result;
}

void install_worker_gate(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return;
    auto& s = inst.ext_state->thread;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    if (!s.worker_gate) s.worker_gate = std::make_unique<ThreadRuntimeState::WorkerGate>();
    s.worker_gate->enabled = true;
    s.worker_gate->open.store(false);
}

void open_worker_gate(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return;
    auto& s = inst.ext_state->thread;
    if (!s.worker_gate) return;
    {
        std::lock_guard<std::mutex> g(s.worker_gate->mtx);
        s.worker_gate->open.store(true);
    }
    s.worker_gate->cv.notify_all();
}

} // namespace ember::ext_thread
