// ext_thread.cpp - ember extension: in-context threads (Tier 4).
// See ext_thread.hpp for the scope statement + correctness model.
//
// Red 8 (§6.6, §10.3, §12.4): DUAL-HOMED thread registry. The keyed path
// targets a SPECIFIC ModuleInstance's per-runtime state; the worker:
//   - captures a shared_ptr<ModuleInstance> (shared lifetime ownership, §6.6,
//     §10.3) so the instance (dispatch record + provider + entry table) stays
//     alive for the worker's whole execution — a host destroying the runtime
//     while a worker is in-flight does not dangle the worker's borrowed refs;
//   - owns its OWN context_t (§6.6: "Each independently entered OS thread owns
//     its own context_t under the normal model") — no shared call_mutex, no
//     host-context contention;
//   - re-resolves its logical entry at EXECUTION time through the guarded
//     core keyed-call API (ember_call_keyed_i64_by_slot, §9.8/§6.5) — NOT a
//     raw ember_keyed_reentry_i64 thunk. The core API establishes the
//     current-runtime TLS, the recoverable checkpoint, the reload/generation
//     guard, and cleans up on every normal AND trapped exit. The raw thunk
//     does NONE of those, so it MUST NOT be used here (task constraint).
//
// KEYED FAIL-CLOSED (task constraint): when the TLS current-keyed-runtime is
// active but ext_state is null, the keyed store selector does NOT fall back to
// the file-static singleton — it fails closed (spawn returns 0). Only when NO
// keyed runtime is active does the legacy identity path target the file-static
// default store.
//
// JOIN LOCKING (task constraint): thread_join_keyed does NOT unconditionally
// unlock a mutex it never locked. The keyed worker owns its own context (no
// shared call_mutex), so join is a simple wait-for-done + thread-join + slot-
// retire. The legacy n_thread_join still uses the shared-context call_mutex
// model (lock/unlock ownership-explicit: it unlocks only what it locked).
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
#include <atomic>
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

struct SavedState {
    int64_t   budget_remaining;
    int32_t   call_depth;
    int32_t   max_call_depth;
    TrapReason last_trap;
    int32_t   catch_depth;
    int32_t   catch_pad;
    int64_t   thrown_value;
    bool      has_checkpoint;
    jmp_buf   checkpoint;
    std::string last_error;
};

static void save_state(context_t* ctx, SavedState& s) {
    s.budget_remaining = ctx->budget_remaining;
    s.call_depth       = ctx->call_depth;
    s.max_call_depth   = ctx->max_call_depth;
    s.last_trap        = ctx->last_trap;
    s.catch_depth      = ctx->catch_depth;
    s.catch_pad        = ctx->_catch_pad;
    s.thrown_value     = ctx->thrown_value;
    s.has_checkpoint   = ctx->has_checkpoint;
    std::memcpy(s.checkpoint, ctx->checkpoint, sizeof(jmp_buf));
    s.last_error       = ctx->last_error;
}

static void restore_state(context_t* ctx, const SavedState& s) {
    ctx->budget_remaining = s.budget_remaining;
    ctx->call_depth       = s.call_depth;
    ctx->max_call_depth   = s.max_call_depth;
    ctx->last_trap        = s.last_trap;
    ctx->catch_depth      = s.catch_depth;
    ctx->_catch_pad       = s.catch_pad;
    ctx->thrown_value     = s.thrown_value;
    ctx->has_checkpoint   = s.has_checkpoint;
    std::memcpy(ctx->checkpoint, s.checkpoint, sizeof(jmp_buf));
    ctx->last_error       = s.last_error;
}

static void thread_worker_legacy(void* entry, context_t* ctx, int64_t arg,
                                 ThreadSlot* slot) {
    SavedState saved;
    int64_t result = 0;
    bool trapped = false;
    int  reason  = 0;

    ctx->call_mutex.lock();
    save_state(ctx, saved);
    ctx->reset_for_call();
    ctx->has_checkpoint = true;
    // EMBER_SETJMP (not raw setjmp): the JIT'd trap stub longjmps via
    // EMBER_LONGJMP (__builtin_longjmp on MinGW), which expects a
    // __builtin_setjmp-format buffer. Raw setjmp saves fewer registers and
    // uses an incompatible buffer layout, so mixing setjmp + __builtin_longjmp
    // corrupts callee-saved state across the trap unwind and segfaults the
    // worker. The macros in context.hpp resolve to the matching primitive.
    if (EMBER_SETJMP(ctx->checkpoint)) {
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        result = ember_call_i64(entry, ctx, arg);
        ctx->has_checkpoint = false;
    }
    restore_state(ctx, saved);
    {
        std::lock_guard<std::mutex> g(slot->done_lock);
        slot->result      = result;
        slot->trapped     = trapped;
        slot->trap_reason = reason;
        slot->done        = true;
    }
    slot->done_cv.notify_all();
    ctx->call_mutex.unlock();
}

// ── Keyed worker (§6.5, §6.6, §9.8, §10.3, §12.4) ──────────────────────────
// Captures shared_ptr<ModuleInstance> (shared lifetime ownership) so the
// instance + its borrowed dispatch record / provider / entry table stay alive
// for the worker's whole execution. Owns its OWN context_t (§6.6: each
// independently entered OS thread owns its own context). Re-resolves the
// logical handle at EXECUTION time through the GUARDED CORE keyed-call API
// (ember_call_keyed_i64_by_slot) — the core API establishes the current-
// runtime TLS, the recoverable checkpoint, the reload/generation guard, and
// cleans up on every normal AND trapped exit. The raw ember_keyed_reentry_i64
// thunk does NONE of those and MUST NOT be used here (task constraint).
//
// The ThreadSlot* is valid for the worker's lifetime: the shared_ptr<ModuleInstance>
// keeps ext_state (and thus the ThreadRuntimeState + the threads vector + this
// slot) alive. The host retires the slot only AFTER joining (which waits for
// done), so the slot is never reset while the worker is still executing.
static void thread_worker_keyed(std::shared_ptr<ModuleInstance> inst,
                                int64_t logical_handle, int64_t arg,
                                ThreadRuntimeState::WorkerGate* gate,
                                ThreadSlot* slot) {
    // If a test gate is installed, block here BEFORE resolving the entry — so
    // a test can deterministically publish a replacement between spawn and
    // the worker's execution-time resolution (§12.4).
    if (gate && gate->enabled) {
        std::unique_lock<std::mutex> lk(gate->mtx);
        gate->cv.wait(lk, [&] { return gate->open.load(); });
    }

    // The worker owns its own context (§6.6). No shared call_mutex.
    context_t ctx;
    ctx.budget_remaining = 1'000'000'000LL;
    ctx.max_call_depth   = 64;

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
                                           ctx, arg, adapter);
    if (cr.ok) {
        result = cr.value;
    } else if (cr.trapped) {
        trapped = true;
        // Extract the trap reason from the structured reason string; the core
        // API already reset ctx via reset_for_call. We record a generic reason
        // code (the structured reason string is the authoritative diagnostic).
        reason = int(ctx.last_trap);  // reset_for_call clears this, but the
        // structured cr.reason carries the trap name.
    }
    // On a pre-entry failure (provider unavailable, resolve failed), cr.ok is
    // false and cr.trapped is false: result stays 0, trapped stays false. The
    // structured cr.reason carries the diagnostic (not stored on the slot,
    // but the join returns 0 / a failure indicator).

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
    // ThreadRuntimeState; st does not carry it.
    std::shared_ptr<ModuleInstance> inst_sp;
    if (st.keyed) {
        ModuleInstance* rt = ember_current_keyed_runtime();
        if (!rt || !rt->ext_state) return 0;
        inst_sp = rt->ext_state->thread.instance_wp.lock();
        if (!inst_sp) return 0;  // fail-closed: no shared ownership available
        if (!inst_sp->provider) return 0;
        if (handle < 0 || uint32_t(handle) >= inst_sp->logical_slot_count) return 0;
    } else {
        // Legacy path: resolve the entry now (the legacy worker caches it).
        if (!*st.ctx_pp || !*st.dispatch_base_pp || *st.slot_count_p <= 0) return 0;
        void* entry = resolve_entry_legacy(handle, *st.dispatch_base_pp, *st.slot_count_p);
        if (!entry) return 0;
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
            raw->th = std::thread(thread_worker_keyed, inst_sp, handle, arg, gate, raw);
        } else {
            void* entry = resolve_entry_legacy(handle, *st.dispatch_base_pp, *st.slot_count_p);
            context_t* ctx  = *st.ctx_pp;
            raw->th = std::thread(thread_worker_legacy, entry, ctx, arg, raw);
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

    // Legacy in-context-thread model: the worker shares the host's context_t
    // and locks call_mutex around its ember_call. The caller of thread_join
    // (the script function) holds call_mutex (it was locked by the enclosing
    // ember_call). The worker blocks on call_mutex.lock() in
    // thread_worker_legacy. The join path must release call_mutex so the
    // worker can proceed, then re-acquire it after the worker finishes.
    // Ownership-explicit: we unlock what the caller (the enclosing ember_call
    // for the script function calling thread_join) locked, and re-lock before
    // returning so the caller's call_mutex invariant is preserved.
    context_t* ctx = *st.ctx_pp;
    ctx->call_mutex.unlock();
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(s->done_lock);
        s->done_cv.wait(dlk, [&] { return s->done; });
        trapped = s->trapped;
        result  = s->result;
    }
    if (s->th.joinable()) s->th.join();
    ctx->call_mutex.lock();
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
    s.ctx           = nullptr;  // keyed worker owns its own context
    s.dispatch_base = const_cast<void*>(static_cast<const void*>(inst.entry_table->slots.data()));
    s.slot_count    = int64_t(inst.logical_slot_count);
    s.instance      = &inst;
    s.instance_wp   = std::weak_ptr<ModuleInstance>(self);  // shared-ownership back-pointer
    s.keyed_mode    = true;
    if (inst.record.physical_slots == nullptr && inst.logical_slot_count > 0)
        assemble_identity_dispatch_record(inst);
    return true;
}

// thread_join_keyed (§6.6, §12.4): the keyed worker owns its own context (no
// shared call_mutex), so join is a simple wait-for-done + thread-join + slot-
// retire. It does NOT unconditionally unlock a mutex it never locked (the bug
// in the previous version: it called ctx.call_mutex.unlock() / .lock() around
// the wait, but the keyed caller never locked call_mutex — UB). The context
// parameter is retained for API compatibility but is NOT used for locking
// (the keyed worker has its own context); the adapter is retained for API
// compatibility (the worker re-derives its own adapter from the instance's
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
    // owns its own context (§6.6), so there is no shared-context lock to
    // manage. Ownership-explicit: we lock/unlock only slot->done_lock.
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(slot->done_lock);
        slot->done_cv.wait(dlk, [&] { return slot->done; });
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
