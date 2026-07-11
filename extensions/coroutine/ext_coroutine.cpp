// ext_coroutine.cpp - ember extension: coroutines with yield (#21).
// See ext_coroutine.hpp for the scope statement + correctness model.
//
// A #21 extension mirroring ext_thread (one TU per the extensions/README.md
// purity rule; depends only on ember public headers + stdlib + Win32 fibers).
// Host-owned coroutine registry behind 1-based i64 handles. Each coroutine is
// a Windows fiber with its own stack; the fn runs on that stack via
// ember_call_i64 (which installs r14 = ctx, the caller's context_t*). yield
// (lowered by codegen to a call to __ember_coro_yield) stores the value +
// SwitchToFiber back to the caller's fiber; coroutine_next SwitchToFibers to
// the coroutine's fiber to resume it.
//
// Why fibers (not a hand-rolled setjmp/longjmp + separate stack): fibers are
// the OS-provided cooperative-coroutine primitive on Windows — CreateFiberEx
// allocates + sets up the stack, SwitchToFiber does a full register + stack
// switch (preserving ALL registers including the JIT's r14 context reg + the
// xmm regs), DeleteFiber frees the stack. Rolling our own would duplicate
// exactly that with inline asm + a manually-allocated stack, and would have
// to get the Win64 stack alignment + TEB fiber-chain bookkeeping right. Fibers
// are the simplest CORRECT approach on Windows, which is why the design uses
// them.
//
// FIBER_FLAG_FLOAT_SWITCH is passed to CreateFiberEx + ConvertThreadToFiberEx
// so the fiber preserves the FPU/SSE control word + xmm state — the JIT'd code
// uses xmm registers for f32/f64, so a fiber switch that did NOT preserve
// them (the legacy default) would corrupt floating-point state across a
// yield/next boundary. This flag is the modern default for any fiber that runs
// floating-point code.
#include "ext_coroutine.hpp"
#include "ast.hpp"            // type_i64, Prim, Type
#include "binding_builder.hpp" // BindingBuilder
#include "engine.hpp"          // ember_call_i64
#include "context.hpp"         // context_t, TrapReason

#include <windows.h>           // CreateFiberEx, SwitchToFiber, DeleteFiber, ...

#include <atomic>
#include <cstring>             // memcpy (jmp_buf save/restore)
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

using namespace ember;

namespace ember::ext_coroutine {

// Sentinel coroutine_next returns when the coroutine's fn TRAPPED (budget /
// stack / bounds / illegal / unhandled-throw) instead of returning normally.
// Lets a script distinguish "the fn returned INT64_MIN" from "the fn trapped"
// (the former is a valid return value, the latter is a control-flow signal).
// Mirrors ext_thread's TRAP_SENTINEL. A script that expects its coroutine to
// possibly return INT64_MIN should treat a trap as fatal (the coroutine is
// done + yielded no clean value).
static constexpr int64_t TRAP_SENTINEL = INT64_MIN;

// ---- Host setup state (set by coroutine_init, read by the natives) ----
// One store-management mutex serializes lookup/init/reset. The dispatch base +
// slot count + context pointer are the three things a native (which only gets
// i64 args) cannot recover on its own — same shape as ext_thread's g_ctx/
// g_dispatch_base/g_slot_count. g_main_fiber is the caller's fiber (the thread
// converted to a fiber in coroutine_init); coroutine_next uses it as the
// caller fiber when a coroutine is started from the top level (no enclosing
// coroutine).
static std::recursive_mutex g_setup_mutex;
static context_t*           g_ctx           = nullptr;
static void*                g_dispatch_base = nullptr;   // atomic<void*>[] base
static int64_t              g_slot_count    = 0;
static void*                g_main_fiber    = nullptr;   // the thread-as-fiber

// Forward decl: g_current_coro (below) + the trampoline reference Coroutine
// before its struct definition. The full definition is further down.
struct Coroutine;

// The currently-running coroutine (thread-local because fibers are per-thread;
// coroutines are single-threaded but the thread-local is correct + free). null
// when running on the main fiber (no coroutine active). __ember_coro_yield
// reads this to find the coroutine to suspend; coroutine_next sets it to the
// coroutine being resumed. A coroutine that itself calls coroutine_next on
// another coroutine pushes the previous current onto the resumed coro's
// caller_coro (a linked list of suspended coroutines), so nesting works.
static thread_local Coroutine* g_current_coro = nullptr;

// Resolve a fn_handle (bare dispatch slot) to the JIT entry. Returns nullptr
// for out-of-range / cross-module (bit 63 set, i.e. a negative i64) /
// not-yet-published slots. Mirrors ext_thread::resolve_entry. The dispatch
// table is an array of std::atomic<void*>; read under acquire so the entry
// published by DispatchTable::set (release) is visible.
static void* resolve_entry(int64_t handle) {
    if (handle < 0) return nullptr;           // negative == bit 63 set (cross-module)
    if (g_slot_count <= 0 || handle >= g_slot_count) return nullptr;
    if (!g_dispatch_base) return nullptr;
    auto* slots = static_cast<std::atomic<void*>*>(g_dispatch_base);
    return slots[size_t(handle)].load(std::memory_order_acquire);
}

// ---- The coroutine slot ----
// One per coroutine_start. Holds the fiber (which owns the stack), the fn
// entry + arg, the last yielded value, the done/started/trapped flags, and the
// caller-fiber linkage used by SwitchToFiber. The trampoline writes
// yield_value/done/trapped/trap_reason; coroutine_next reads them after the
// SwitchToFiber returns to the caller.
struct Coroutine {
    void*     fiber        = nullptr;   // the Windows fiber (owns the stack)
    void*     entry        = nullptr;   // the JIT'd fn entry (dispatch slot)
    context_t* ctx         = nullptr;   // the context_t to call with (r14 = ctx)
    int64_t   arg          = 0;         // the fn's single i64 arg
    int64_t   yield_value  = 0;         // last yielded / returned value
    bool      done         = false;     // fn has returned (no more yields)
    bool      started      = false;     // first coroutine_next has run the fn
    bool      trapped      = false;     // fn trapped (budget/stack/etc.)
    int       trap_reason  = 0;         // TrapReason as int (0 = None)
    // caller-fiber linkage (set by coroutine_next before SwitchToFiber):
    void*     caller_fiber = nullptr;   // the fiber to SwitchToFiber back to on yield
    Coroutine* caller_coro = nullptr;   // the coroutine that was current (nesting)
    bool      in_use       = false;
};

// 1-based handle registry (mirrors ext_thread's slot shape). A free-list reuses
// slots so repeated coroutine_start/coroutine_reset cycles don't grow without
// bound across an `ember test` run.
static std::vector<std::unique_ptr<Coroutine>> g_coros;
static std::vector<int64_t>                     g_coros_free;

// Raw observing lookup (coroutine_next/done/yield hold a Coroutine* that is
// stable for the coroutine's lifetime — coroutine_reset never erases an in_use
// slot). 1-based handle, returns nullptr for out-of-range / not-in-use.
static Coroutine* raw_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_coros.size())) return nullptr;
    auto& c = g_coros[size_t(h - 1)];
    return (c && c->in_use) ? c.get() : nullptr;
}

// ---- Per-call state save/restore (mirrors ext_thread::SavedState) ----
// The coroutine's fn runs via its OWN ember_call_i64 on the shared context_t.
// It modifies budget_remaining (the fn's entry budget_check), call_depth (its
// internal calls' depth checks), catch_depth/thrown_value (if it uses
// try/catch), and the checkpoint (the trampoline sets its own). To keep the
// caller's in-progress state intact across the coroutine's WHOLE lifetime, the
// trampoline snapshots these at entry and restores them when the coroutine is
// done (fn returned OR trapped). While the coroutine is SUSPENDED at a yield
// the state is "in flight" — the caller must not throw/next into the same
// coroutine concurrently (coroutines are cooperative + single-threaded, so it
// can't). On done/trap, restoring the snapshot cleans up the caller's view.
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
}

// ---- The fiber trampoline ----
// Runs ON the coroutine's stack the first time coroutine_next SwitchToFibers
// to it. Sets up an isolated checkpoint (so a trap in the fn unwinds HERE, not
// to the caller's checkpoint — the coroutine is isolated, mirroring
// ext_thread's worker), calls the fn via ember_call_i64 (installs r14 = ctx),
// then on normal return OR trap: restores the caller's per-call state, marks
// the coroutine done, and SwitchToFibers back to the caller. The SwitchToFiber
// never returns (the coroutine is finished); DeleteFiber reclaims the stack
// later from coroutine_reset (or, defensively, right here — see below).
//
// The fiber param (lpParameter) is the Coroutine* (passed to CreateFiberEx).
static void WINAPI coro_trampoline(PVOID lpParameter) {
    Coroutine* co = static_cast<Coroutine*>(lpParameter);
    context_t* ctx = co->ctx;

    // Snapshot the caller's per-call state so the coroutine's fn consumption
    // (budget/depth/catch) is isolated — restored below on done/trap.
    SavedState saved;
    save_state(ctx, saved);

    int64_t result  = 0;
    bool    trapped = false;
    int     reason  = 0;

    // Isolated checkpoint: a trap in the fn (budget/stack/bounds/illegal/
    // unhandled-throw) longjmps HERE, not to the caller's checkpoint. The trap
    // stub (baked at compile) does __builtin_longjmp(ctx->checkpoint, 1); we
    // own ctx->checkpoint for the duration of the fn, so the longjmp lands here.
    ctx->has_checkpoint = true;
    if (__builtin_setjmp(ctx->checkpoint)) {
        // Trap fired during the fn -> the trap stub longjmp'd here. ctx->last_trap
        // was set by the stub before the longjmp.
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        // Run the fn. ember_call_i64 installs r14 = ctx + calls the entry with
        // arg in rcx. The fn runs until it yields (SwitchToFiber to the caller
        // — this trampoline frame is frozen on the coro stack) or returns. On
        // return, ember_call_i64 yields the fn's i64 return value.
        result = ember_call_i64(co->entry, ctx, co->arg);
        ctx->has_checkpoint = false;
    }

    // Restore the caller's per-call state exactly (the coroutine's budget/depth
    // consumption + catch state are discarded — the coroutine is done).
    restore_state(ctx, saved);

    // Publish the final value + done flag. yield_value is what coroutine_next
    // (waiting on the caller fiber) returns. On a trap, yield the sentinel so a
    // script can detect it (mirrors ext_thread's thread_join returning TRAP_SENTINEL).
    co->yield_value  = trapped ? TRAP_SENTINEL : result;
    co->trapped      = trapped;
    co->trap_reason  = reason;
    co->done         = true;

    // Switch back to the caller's fiber (coroutine_next, frozen right after its
    // SwitchToFiber to us). This never returns — the coroutine is finished.
    // We must NOT read `co` after the SwitchToFiber (the caller may immediately
    // coroutine_reset + DeleteFiber this fiber, invalidating the stack we're
    // on); capture caller_fiber first.
    void* caller = co->caller_fiber;
    g_current_coro = co->caller_coro;   // caller's current resumes (usually null)
    SwitchToFiber(caller);
    // Unreachable: a finished coroutine never resumes. If it somehow did
    // (caller bug), terminate rather than run on a freed stack.
    std::terminate();
}

// ---- The internal yield native ----
// `yield value;` (lowered by codegen to a call here) stores the value into the
// current coroutine + SwitchToFibers back to the caller's fiber. When the
// caller resumes the coroutine (coroutine_next -> SwitchToFiber to this coro's
// fiber), SwitchToFiber returns here + the fn continues after the yield.
//
// The value is in rcx (Win64 first arg). Returns void (the yield value is
// stashed on the coroutine, not returned through rax — codegen ignores the
// return). g_current_coro is the running coroutine (set by coroutine_next
// before it SwitchToFiber'd to us).
static void n_coro_yield(int64_t value) {
    Coroutine* co = g_current_coro;
    if (!co) {
        // yield outside a coroutine: codegen/sema should prevent this, but
        // defend at runtime too (a stray __ember_coro_yield call). Switching
        // with no caller fiber would fault; just drop the value.
        return;
    }
    co->yield_value = value;
    void* caller = co->caller_fiber;
    Coroutine* prev = co->caller_coro;
    g_current_coro = prev;          // the caller will be current after the switch
    SwitchToFiber(caller);          // returns to coroutine_next (frozen mid-SwitchToFiber)
    // Resumed: coroutine_next set g_current_coro = co again before switching
    // back to us, so g_current_coro is correct for the next yield. Nothing to
    // do here — just return + the fn continues after the yield statement.
}

// ---- The public natives ----
extern "C" {

// coroutine_start(fn_handle, arg) -> coroutine_handle
// fn_handle is a bare dispatch slot (an intra-module `&fn`). Resolve it to the
// JIT entry + validate the range (REDSHELL guard: a forged/out-of-range handle
// yields handle 0, never a wild call). A cross-module handle (bit 63 set, i.e.
// a negative i64) is rejected for v1. Returns 0 on any setup error (init not
// called, dispatch base null, slot out of range, entry null, or fiber alloc
// failure).
static int64_t n_coroutine_start(int64_t handle, int64_t arg) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (!g_ctx || !g_dispatch_base || g_slot_count <= 0) return 0;
    if (!g_main_fiber) return 0;   // coroutine_init did not convert the thread
    void* entry = resolve_entry(handle);
    if (!entry) return 0;

    // Allocate a slot (reuse from the free-list if possible).
    int64_t idx;
    Coroutine* raw;
    if (!g_coros_free.empty()) {
        idx = g_coros_free.back();
        g_coros_free.pop_back();
        g_coros[size_t(idx - 1)] = std::make_unique<Coroutine>();
        raw = g_coros[size_t(idx - 1)].get();
    } else {
        if (g_coros.size() >= (size_t(1) << 20)) return 0;   // hard ceiling (mirrors ext_sync/ext_thread)
        g_coros.push_back(std::make_unique<Coroutine>());
        idx = int64_t(g_coros.size());
        raw = g_coros.back().get();
    }
    raw->in_use   = true;
    raw->done     = false;
    raw->started  = false;
    raw->trapped  = false;
    raw->trap_reason = 0;
    raw->yield_value = 0;
    raw->entry    = entry;
    raw->ctx      = g_ctx;
    raw->arg      = arg;
    raw->caller_fiber = nullptr;
    raw->caller_coro  = nullptr;

    // Create the fiber with its own stack. FIBER_FLAG_FLOAT_SWITCH preserves
    // xmm/FPU state across switches (the JIT uses xmm for f32/f64). A 0 stack
    // size lets the OS pick the default (1 MB committed as needed).
    raw->fiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH,
                               coro_trampoline, raw);
    if (!raw->fiber) {
        // Fiber allocation failed (resource exhaustion). Free the slot.
        raw->in_use = false;
        g_coros[size_t(idx - 1)].reset();
        g_coros_free.push_back(idx);
        return 0;
    }
    return idx;
}

// coroutine_next(handle) -> i64
// Resumes the coroutine: first next() starts the fn (runs to the first yield);
// later next()s resume after the last yield. Returns the yielded value. When
// the fn returned (done), returns the return value (and, on a trap, the
// TRAP_SENTINEL). Returns 0 on a bad handle. Calling next() on an already-done
// coroutine returns the last yield_value again (a finished coroutine yields no
// more — the script should gate on coroutine_done).
static int64_t n_coroutine_next(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (!g_ctx || !g_main_fiber) return 0;
    Coroutine* co = raw_slot(handle);
    if (!co) return 0;
    if (co->done) return co->yield_value;   // finished: return the last value

    // Record the caller-fiber linkage: the fiber to switch back to on yield +
    // the coroutine that was current (for nesting). GetCurrentFiber returns the
    // caller's fiber (the thread was converted to a fiber in coroutine_init, so
    // the main fiber is valid; a nested coroutine_next called from inside a
    // coroutine fn returns THAT coroutine's fiber, which is correct — yield
    // switches back to it).
    co->caller_fiber = GetCurrentFiber();
    co->caller_coro  = g_current_coro;
    g_current_coro   = co;
    co->started      = true;

    // Switch to the coroutine's fiber. On the first next() this calls the
    // trampoline (which runs the fn to the first yield); on later next()s it
    // resumes __ember_coro_yield right after its SwitchToFiber. Control returns
    // here when the coroutine yields (or finishes: the trampoline switches back
    // as its last act).
    SwitchToFiber(co->fiber);

    // We're back on the caller's fiber. Restore the current-coroutine pointer
    // (the yield/trampoline already set it to caller_coro, but be explicit +
    // correct against any path that didn't — e.g. the trampoline's terminal
    // SwitchToFiber sets it too).
    g_current_coro = co->caller_coro;

    return co->yield_value;
}

// coroutine_done(handle) -> bool
// true once the coroutine's fn has returned (finished + will yield no more).
// false while it is suspended at a yield (more values available), or on a bad
// handle (a bad handle is "done" in the sense that there's nothing to resume —
// returns false to match raw_slot's not-in-use semantics, so a script looping
// `while (!coroutine_done(h))` on a bad handle does not spin forever... it
// would, since next() also returns 0; but a bad handle is a script bug anyway).
static int64_t n_coroutine_done(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    Coroutine* co = raw_slot(handle);
    if (!co) return 0;
    return co->done ? 1 : 0;
}

} // extern "C"

// ---- Registration ----
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type T = type_i64();
    // A bare `fn` param: is_fn_handle=true, no recorded sig. Type::same accepts
    // a recorded-sig fn handle against a bare `fn` param (the one subtyping
    // direction), so `coroutine_start(&counter, 10)` type-checks for any fn —
    // mirroring thread_spawn. coroutine_start/next/done are the public surface.
    Type fn_param = type_i64();
    fn_param.is_fn_handle = true;

    b.add("coroutine_start", T, {fn_param, type_i64()}, (void*)&n_coroutine_start);
    b.add("coroutine_next",  type_i64(), {T},           (void*)&n_coroutine_next);
    b.add("coroutine_done",  type_bool(), {T},           (void*)&n_coroutine_done);
    // The internal yield native. Named with the __ember_ prefix so it does not
    // collide with any user fn + is clearly internal. codegen's YieldStmt
    // handler looks this name up in the natives table + emits a call to it.
    // takes one i64 (the yield value), returns i64 (ignored by codegen — the
    // value is stashed on the coroutine, not returned through rax). params =
    // {i64} so codegen's native-call marshalling (if it ever routes through
    // the symbolic path) sees the right signature.
    b.add("__ember_coro_yield", type_i64(), {type_i64()}, (void*)&n_coro_yield);

    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// ---- Host setup / reset ----
bool coroutine_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count) {
    if (!ctx || !dispatch_base || slot_count <= 0) return false;
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    g_ctx           = ctx;
    g_dispatch_base = dispatch_base;
    g_slot_count    = slot_count;
    // Convert the calling thread to a fiber (idempotent). Fibers require the
    // thread to be a fiber first so GetCurrentFiber() returns the caller's
    // fiber (coroutine_next uses it as the switch-back target on yield). If the
    // thread is already a fiber (a previous coroutine_init, or the host
    // converted it itself), IsThreadAFiber is true + we just record the
    // current fiber. FIBER_FLAG_FLOAT_SWITCH preserves xmm/FPU state.
    if (!IsThreadAFiber()) {
        g_main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    } else {
        g_main_fiber = GetCurrentFiber();
    }
    return true;
}

void coroutine_reset() {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    // Delete every coroutine's fiber (which frees its stack). A coroutine that
    // is still suspended (the script leaked it without running to done) has a
    // live fiber; DeleteFiber on a suspended fiber is safe + frees its stack
    // (the fiber is simply never resumed). The unique_ptr then drops the
    // Coroutine struct itself.
    for (auto& c : g_coros) {
        if (c && c->in_use && c->fiber) {
            DeleteFiber(c->fiber);
            c->fiber = nullptr;
        }
    }
    g_coros.clear();
    g_coros_free.clear();
    g_current_coro = nullptr;
    // NOTE: we do NOT ConvertFiberToThread here. The thread stays a fiber for
    // the process lifetime: `ember test` calls run_ember_file N times in one
    // process, each calling coroutine_init + coroutine_reset. Converting back
    // + re-converting on every iteration is fragile (ConvertThreadToFiber
    // fails if already a fiber, so the idempotency guard above handles it, but
    // ConvertFiberToThread + re-Convert round-trips the TEB fiber state
    // needlessly). Staying a fiber is correct + free — a fiber behaves
    // exactly like a thread except it can SwitchToFiber.
    // g_ctx/g_dispatch_base/g_slot_count are left as-is; the next
    // coroutine_init refreshes them. g_main_fiber stays valid (the thread is
    // still a fiber).
}

} // namespace ember::ext_coroutine
