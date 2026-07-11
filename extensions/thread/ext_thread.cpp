// ext_thread.cpp - ember extension: in-context threads (Tier 4).
// See ext_thread.hpp for the scope statement + correctness model.
//
// A Tier-4 extension mirroring ext_array/ext_sync (one TU per the
// extensions/README.md purity rule; depends only on ember public headers +
// stdlib). Host-owned thread registry behind 1-based i64 handles. The spawned
// threads call ember back into the SAME context_t, serialized by
// context_t::call_mutex with save/restore of the per-call state.
//
// Memory-ordering / sync notes:
//   - `done` + `result` + `trapped` + `trap_reason` are written by the spawned
//     thread BEFORE it unlocks call_mutex + notifies the cv, and read by the
//     joining thread AFTER it reacquires call_mutex. The mutex pair
//     (unlock-with-notify on producer, lock-after-wait on consumer) is a
//     release/acquire pair, so the joining thread sees the final values without
//     extra atomics. The cv's wait/notify is the阻塞 mechanism; the mutex is
//     the happens-before edge.
//   - The fn-handle -> entry resolution reads the dispatch table slot under
//     std::memory_order_acquire (the dispatch table stores under release in
//     DispatchTable::set), so a handle baked at compile resolves to the fn the
//     host published, not a stale null.
#include "ext_thread.hpp"
#include "ast.hpp"            // type_i64, Prim, Type
#include "binding_builder.hpp" // BindingBuilder
#include "engine.hpp"          // ember_call_i64
#include <atomic>
#include <condition_variable>
#include <cstring>             // memcpy (jmp_buf save/restore)
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ember;

namespace ember::ext_thread {

// Sentinel returned by thread_join when the joined thread trapped (so a script
// can distinguish "the fn returned INT64_MIN" from "the fn trapped" — the
// former is a valid return value, the latter is a control-flow signal). A
// script that expects its worker to possibly return INT64_MIN should also call
// thread_trap_reason to disambiguate. Mirrors ext_sync's EMPTY_SENTINEL shape.
static constexpr int64_t TRAP_SENTINEL = INT64_MIN;

// ---- Host setup state (set by thread_init, read by thread_spawn) ----
// One store-management mutex serializes lookup/init/reset across contexts.
// The dispatch base + slot count + context pointer are the three things a
// native (which only gets i64 args) cannot recover on its own.
static std::recursive_mutex g_setup_mutex;
static context_t*           g_ctx           = nullptr;
static void*                g_dispatch_base = nullptr;   // atomic<void*>[] base
static int64_t              g_slot_count    = 0;

// ---- Thread registry (1-based handles, mirrors ext_sync's slot shape) ----
struct ThreadSlot {
    std::thread        th;            // the OS thread (default-constructed if free)
    int64_t            result   = 0;  // the fn's i64 return (valid when done)
    bool               done     = false;
    bool               trapped  = false;
    int                trap_reason = 0;  // TrapReason as int (0 = None)
    std::mutex         done_lock;     // guards done/result/trapped + the cv
    std::condition_variable done_cv;  // join waits on this
    bool               in_use = false;
};
static std::vector<std::unique_ptr<ThreadSlot>> g_threads;
static std::vector<int64_t>                     g_threads_free;

// Raw observing lookup (the worker + join hold a ThreadSlot* that is stable for
// the thread's lifetime — reset() never erases an in_use slot, it only clears
// the registry when all slots are free). 1-based handle, returns nullptr for
// out-of-range or not-in-use slots (mirrors ext_sync's slot() shape).
static ThreadSlot* raw_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (h < 1 || h > int64_t(g_threads.size())) return nullptr;
    auto& s = g_threads[size_t(h - 1)];
    return (s && s->in_use) ? s.get() : nullptr;
}

// Resolve a fn_handle (bare dispatch slot) to the JIT entry. Returns nullptr
// for out-of-range / cross-module (bit 63 set) / not-yet-published slots.
// The dispatch table is an array of std::atomic<void*>; read under acquire so
// the entry published by DispatchTable::set (release) is visible.
static void* resolve_entry(int64_t handle) {
    if (handle < 0) return nullptr;           // negative == bit 63 set (cross-module)
    if (g_slot_count <= 0 || handle >= g_slot_count) return nullptr;
    if (!g_dispatch_base) return nullptr;
    auto* slots = static_cast<std::atomic<void*>*>(g_dispatch_base);
    return slots[size_t(handle)].load(std::memory_order_acquire);
}

// ---- The spawned-thread worker ----
// Locks call_mutex, saves the caller's per-call state, runs a complete
// ember_call on the shared context, restores, unlocks, marks done + notifies.
//
// The save/restore is the crux: the caller is blocked on call_mutex at a
// native call boundary (thread_join released it for us), so its in-progress
// budget/depth/catch/checkpoint are frozen. We must not clobber them
// permanently — reset_for_call zeroes depth + clears catch, and setjmp
// overwrites checkpoint. So we snapshot, run, restore. On resumption the
// caller sees its exact pre-spawn state.
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

// The worker runs in the spawned OS thread. It captures the entry + arg + the
// ThreadSlot* it reports into. The trap stub (baked at compile) longjmps to
// ctx->checkpoint, which we set here via __builtin_setjmp — so a trap unwinds
// to THIS worker, not the caller. We record the trap reason off the context
// (the trap stub set ctx->last_trap before the longjmp) + restore the caller's
// state before unlocking.
static void thread_worker(void* entry, context_t* ctx, int64_t arg, ThreadSlot* slot) {
    SavedState saved;
    int64_t result = 0;
    bool trapped = false;
    int  reason  = 0;

    ctx->call_mutex.lock();
    save_state(ctx, saved);
    ctx->reset_for_call();
    ctx->has_checkpoint = true;
    if (__builtin_setjmp(ctx->checkpoint)) {
        // Trap fired during our ember_call -> the trap stub longjmp'd here.
        // ctx->last_trap was set by the stub before the longjmp.
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        result = ember_call_i64(entry, ctx, arg);
        ctx->has_checkpoint = false;
    }
    // Restore the caller's per-call state exactly; our call's depth/catch
    // increments are discarded (mirrors reset_for_call after a longjmp).
    restore_state(ctx, saved);

    // Publish the result + done under done_lock, then notify the cv so a
    // thread_join blocked in cv.wait wakes. Release call_mutex AFTER the
    // publish so the happens-before edge through the mutex also orders the
    // result write for a joiner that reacquires call_mutex (belt + suspenders
    // with the cv's own edge).
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

// ---- Natives ----
extern "C" {

// thread_spawn(fn_handle, arg) -> thread_id
// fn_handle is a bare dispatch slot (an intra-module `&fn`). We resolve it to
// the JIT entry + validate the range (REDSHELL guard: a forged/out-of-range
// handle yields thread_id 0, never a wild call). A cross-module handle (bit 63
// set, i.e. a negative i64) is rejected for v1. Returns 0 on any setup error
// (thread_init not called, dispatch base null, slot out of range, entry null,
// or thread allocation failure).
static int64_t n_thread_spawn(int64_t handle, int64_t arg) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (!g_ctx || !g_dispatch_base || g_slot_count <= 0) return 0;
    void* entry = resolve_entry(handle);
    if (!entry) return 0;

    // Allocate a slot (reuse from the free-list if possible).
    int64_t idx;
    ThreadSlot* raw;
    if (!g_threads_free.empty()) {
        idx = g_threads_free.back();
        g_threads_free.pop_back();
        g_threads[size_t(idx - 1)] = std::make_unique<ThreadSlot>();
        raw = g_threads[size_t(idx - 1)].get();
    } else {
        if (g_threads.size() >= (size_t(1) << 20)) return 0;   // hard ceiling (mirrors ext_sync)
        g_threads.push_back(std::make_unique<ThreadSlot>());
        idx = int64_t(g_threads.size());
        raw = g_threads.back().get();
    }
    raw->in_use  = true;
    raw->done    = false;
    raw->trapped = false;
    raw->trap_reason = 0;
    raw->result  = 0;
    context_t* ctx  = g_ctx;

    // Launch the OS thread. We hand it the entry + ctx + arg + its slot.
    // std::thread constructs BEFORE we release g_setup_mutex; the worker
    // synchronizes on call_mutex (not g_setup_mutex), so releasing
    // g_setup_mutex here is safe.
    try {
        raw->th = std::thread(thread_worker, entry, ctx, arg, raw);
    } catch (const std::exception&) {
        // Thread creation failed (resource exhaustion). Free the slot.
        raw->in_use = false;
        g_threads[size_t(idx - 1)].reset();
        g_threads_free.push_back(idx);
        return 0;
    }
    return idx;
}

// thread_join(thread_id) -> i64
// Waits for the spawned thread to finish + returns its i64 result. If the
// thread trapped, returns TRAP_SENTINEL (INT64_MIN); the script can call
// thread_trap_reason to see which trap. Returns 0 on a bad handle.
//
// DEADLOCK-FREEDOM: the caller is inside an ember_call that holds call_mutex.
// The spawned worker needs call_mutex to run. So thread_join RELEASES
// call_mutex, waits on the slot's cv (which the worker notifies when done),
// then REACQUIRES call_mutex before returning to the JIT. The worker acquires
// call_mutex while join waits, runs to completion, unlocks + notifies, join
// wakes + reacquires. The lock is balanced (one unlock + one lock here), so
// the caller's ember_call still owns exactly one lock on return.
//
// g_setup_mutex is held ONLY for the slot lookup, then RELEASED before the
// cv wait. This is critical for nested spawn (test 7): the joined worker may
// itself call thread_spawn for a grandchild, and thread_spawn needs
// g_setup_mutex to allocate a slot. If join held g_setup_mutex across the
// wait, the worker's nested thread_spawn would block on it, the worker could
// never finish, and join's done_cv would never fire -> deadlock. The
// ThreadSlot* is stable for the thread's lifetime (reset() never erases an
// in_use slot), so it is safe to use after releasing g_setup_mutex.
//
// The slot is NOT freed here: the result/trap data must stay readable so a
// subsequent thread_trap_reason(handle) works (the script inspects it after
// join). The slot is reclaimed by thread_reset (the host's between-runs
// isolation gesture). The std::thread IS joined here (OS resources reclaimed);
// the slot just keeps the result fields + in_use=true.
static int64_t n_thread_join(int64_t handle) {
    // Look up the slot under g_setup_mutex, then release it. The slot pointer
    // is stable for the worker's lifetime (in_use slots are never erased), so
    // we can use it after unlocking the registry mutex.
    ThreadSlot* s;
    {
        std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
        if (!g_ctx) return 0;
        s = raw_slot(handle);
    }
    if (!s) return 0;

    // Release the context call_mutex so the worker can acquire it. The caller
    // (host's outer ember_call) holds it; we unlock it here + reacquire before
    // returning. If the worker already finished + unlocked, this is a no-op
    // lock-unlock-lock cycle (still correct).
    g_ctx->call_mutex.unlock();

    // Wait for the worker to publish done. cv.wait handles the
    // unlock-wait-reacquire of done_lock atomically. g_setup_mutex is NOT held
    // here, so a nested thread_spawn inside the worker can proceed.
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(s->done_lock);
        s->done_cv.wait(dlk, [&] { return s->done; });
        trapped = s->trapped;
        result  = s->result;
    }

    // Reacquire the context call_mutex before returning to the caller's JIT
    // frame (the caller's ember_call expects to still hold it).
    g_ctx->call_mutex.lock();

    // Join the std::thread (reclaim OS resources). The worker has finished
    // (done == true), so join returns immediately.
    if (s->th.joinable()) s->th.join();

    // NOTE: the slot stays in_use so thread_trap_reason(handle) can read the
    // trap reason after join. thread_reset reclaims it.

    return trapped ? TRAP_SENTINEL : result;
}

// thread_trap_reason(thread_id) -> i64
// Returns the TrapReason (as int) the spawned thread recorded, or 0 (None) if
// it completed normally or the handle is bad. Inspect after thread_join
// returned TRAP_SENTINEL to see WHICH trap unwound the thread.
static int64_t n_thread_trap_reason(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    ThreadSlot* s = raw_slot(handle);
    if (!s) return 0;
    std::lock_guard<std::mutex> dlk(s->done_lock);
    return int64_t(s->trap_reason);
}

} // extern "C"

// ---- Registration ----
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    // thread_id is a plain i64 handle (1-based, 0 = null). v1 keeps it untagged
    // so a script can compare `tid == 0` + pass it to thread_join without an
    // i64<->handle conversion friction; the slot is still bounds-checked in
    // every native (raw_slot rejects out-of-range / not-in-use). A tagged
    // `thread` handle remains a future ergonomic refinement (it would need a
    // `thread` type keyword + a null literal to be ergonomic in script).
    Type T = type_i64();
    // A bare `fn` param: is_fn_handle=true, no recorded sig. Type::same accepts
    // a recorded-sig fn handle against a bare `fn` param (the one subtyping
    // direction), so `thread_spawn(&worker, arg)` type-checks for any worker.
    Type fn_param = type_i64();
    fn_param.is_fn_handle = true;

    b.add("thread_spawn",        T, {fn_param, type_i64()},        (void*)&n_thread_spawn);
    b.add("thread_join",         type_i64(), {T},                  (void*)&n_thread_join);
    b.add("thread_trap_reason",  type_i64(), {T},                  (void*)&n_thread_trap_reason);

    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// ---- Host setup / reset ----
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
    // Detach (not join) any still-running threads: a reset between runs is a
    // host isolation gesture, not a synchronization point, and joining would
    // require call_mutex (which the host may not be holding here). A detached
    // worker that is mid-ember_call will finish on its own; it references
    // g_ctx/g_dispatch_base which the host must keep alive past reset OR call
    // reset only when the context + table are quiescent. The test harness
    // joins every thread before reset, so this path is a safety net.
    for (auto& s : g_threads) {
        if (s && s->in_use && s->th.joinable()) s->th.detach();
    }
    g_threads.clear();
    g_threads_free.clear();
    g_ctx           = nullptr;
    g_dispatch_base = nullptr;
    g_slot_count    = 0;
}

} // namespace ember::ext_thread
