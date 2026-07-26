// ext_coroutine.cpp - ember extension: coroutines with yield (#21).
// See ext_coroutine.hpp for the scope statement + correctness model.
//
// Red 8 (§6.7, §10.3): DUAL-HOMED coroutine state. The keyed path targets a
// SPECIFIC ModuleInstance's per-runtime state (inst.ext_state->coroutine) so
// two runtimes carry independent fiber registries (§10.3). §6.7: a suspended
// coroutine can retain machine registers in a fiber context; the keyed
// r15/generation invariant across a fiber yield/resume cannot yet be
// guaranteed, so coroutine_start in KEYED mode returns a TYPED unsupported-mode
// failure (§6.7 fail-closed). Layout-safety: the coroutine slot type is
// CoroutineRuntimeState::Coroutine, shared by both stores — no reinterpret_cast.
#include "ext_coroutine.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include "engine.hpp"
#include "context.hpp"
#include "module_instance.hpp"
#include "runtime_extension_state.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/mman.h>       // mmap/munmap for the coroutine stack
#include <unistd.h>         // sysconf
#include <cstddef>
#endif

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

using namespace ember;

namespace ember::ext_coroutine {

static constexpr int64_t TRAP_SENTINEL = INT64_MIN;

using Coroutine = CoroutineRuntimeState::Coroutine;

static std::recursive_mutex g_setup_mutex;
static context_t*           g_ctx           = nullptr;
static void*                g_dispatch_base = nullptr;
static int64_t              g_slot_count    = 0;
#if defined(_WIN32)
static void*                g_main_fiber    = nullptr;
#elif defined(__APPLE__)
// Phase 8: no fiber conversion on Darwin — the main thread's context is just
// the callee-saved regs saved on the first resume. g_initialized gates the
// natives (coroutine_init sets it). Default coroutine stack size (bytes).
static bool                 g_initialized   = false;
static constexpr size_t     CORO_STACK_SIZE = 1 << 20;  // 1 MiB
#endif

static thread_local Coroutine* g_current_coro = nullptr;

static void* resolve_entry(int64_t handle) {
    if (handle < 0) return nullptr;
    if (g_slot_count <= 0 || handle >= g_slot_count) return nullptr;
    if (!g_dispatch_base) return nullptr;
    auto* slots = static_cast<std::atomic<void*>*>(g_dispatch_base);
    return slots[size_t(handle)].load(std::memory_order_acquire);
}

static CoroutineRuntimeState* current_keyed_state() {
    ModuleInstance* rt = ember_current_keyed_runtime();
    if (!rt || !rt->ext_state) return nullptr;
    return &rt->ext_state->coroutine;
}

static std::vector<std::unique_ptr<Coroutine>> g_coros;
static std::vector<int64_t>                     g_coros_free;

static Coroutine* raw_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_coros.size())) return nullptr;
    auto& c = g_coros[size_t(h - 1)];
    return (c && c->in_use) ? c.get() : nullptr;
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

// ─── Phase 8: Darwin ARM64 cooperative context switch (declaration) ────────
// Defined in src/darwin_arm64_ctx_switch.S. Saves `from`'s callee-saved GP
// regs + SP + LR, loads `to`'s, and `ret`s into `to`'s saved LR (the resume
// PC). See CoroCtx in runtime_extension_state.hpp for the layout.
#if defined(__APPLE__)
extern "C" void ember_ctx_switch(CoroCtx* from, CoroCtx* to) noexcept;

// The first ember_ctx_switch into a fresh coroutine `ret`s into this C
// trampoline (its LR was set to this address at create time). It runs the
// JIT'd entry on the coroutine's private stack; on return it marks the
// coroutine done and switches back to whoever resumed it. A trap inside the
// entry longjmps to the setjmp checkpoint here (mirrors the Windows fiber
// trampoline). `co` is reached via the thread-local g_current_coro (set by
// n_coroutine_next before the switch) — the switch itself passes no args.
extern "C" void coro_trampoline_darwin() {
    Coroutine* co = g_current_coro;
    context_t* ctx = co->ctx;

    SavedState saved;
    save_state(ctx, saved);

    int64_t result  = 0;
    bool    trapped = false;
    int     reason  = 0;

    ctx->has_checkpoint = true;
    // EMBER_SETJMP resolves to setjmp on Darwin (context.hpp). The JIT'd trap
    // stub longjmps here on a recoverable trap; the checkpoint frame lives on
    // the coroutine's private stack, so the longjmp restores this frame's SP.
    if (EMBER_SETJMP(ctx->checkpoint)) {
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        result = ember_call_i64(co->entry, ctx, co->arg);
        ctx->has_checkpoint = false;
    }

    restore_state(ctx, saved);

    co->yield_value  = trapped ? TRAP_SENTINEL : result;
    co->trapped      = trapped;
    co->trap_reason  = reason;
    co->done         = true;

    // Switch back to whoever resumed us (the main thread or an outer coroutine).
    // caller_ctx was saved by the resume's ember_ctx_switch; restoring it lands
    // back in n_coroutine_next right after its switch. After this we never
    // resume the trampoline (co->done), so std::terminate guards the tail.
    g_current_coro = co->caller_coro;
    ember_ctx_switch(&co->coro_ctx, &co->caller_ctx);
    std::terminate();
}
#endif

#if defined(_WIN32)
static void WINAPI coro_trampoline(PVOID lpParameter) {
    Coroutine* co = static_cast<Coroutine*>(lpParameter);
    context_t* ctx = co->ctx;

    SavedState saved;
    save_state(ctx, saved);

    int64_t result  = 0;
    bool    trapped = false;
    int     reason  = 0;

    ctx->has_checkpoint = true;
    // EMBER_SETJMP (not raw setjmp): the JIT'd trap stub longjmps via
    // EMBER_LONGJMP (__builtin_longjmp on MinGW), which expects a
    // __builtin_setjmp-format buffer. Raw setjmp + __builtin_longjmp is UB
    // and corrupts callee-saved state across the trap unwind (segfaults the
    // fiber). The macros in context.hpp resolve to the matching primitive.
    if (EMBER_SETJMP(ctx->checkpoint)) {
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        result = ember_call_i64(co->entry, ctx, co->arg);
        ctx->has_checkpoint = false;
    }

    restore_state(ctx, saved);

    co->yield_value  = trapped ? TRAP_SENTINEL : result;
    co->trapped      = trapped;
    co->trap_reason  = reason;
    co->done         = true;

    void* caller = co->caller_fiber;
    g_current_coro = co->caller_coro;
    SwitchToFiber(caller);
    std::terminate();
}
#endif // _WIN32

static void n_coro_yield(int64_t value) {
    Coroutine* co = g_current_coro;
    if (!co) return;
    co->yield_value = value;
    Coroutine* prev = co->caller_coro;
    g_current_coro = prev;
#if defined(_WIN32)
    SwitchToFiber(co->caller_fiber);
#elif defined(__APPLE__)
    // Save THIS coroutine's state into coro_ctx and restore whoever resumed us
    // (caller_ctx) — `ret` lands back in the caller's n_coroutine_next. The
    // saved LR in coro_ctx resumes right here on the next coroutine_next.
    ember_ctx_switch(&co->coro_ctx, &co->caller_ctx);
#endif
}

extern "C" {

static int64_t n_coroutine_start(int64_t handle, int64_t arg) {
    if (CoroutineRuntimeState* ks = current_keyed_state()) {
        std::lock_guard<std::recursive_mutex> g(ks->setup_mutex);
        ks->last_start_status.ok = false;
        ks->last_start_status.unsupported_mode = true;
        ks->last_start_status.reason =
            "keyed coroutine start unsupported: r15/generation invariant across "
            "fiber yield/resume not yet guaranteed (§6.7 fail-closed)";
        return 0;
    }
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (!g_ctx || !g_dispatch_base || g_slot_count <= 0) return 0;
#if defined(_WIN32)
    if (!g_main_fiber) return 0;
#elif defined(__APPLE__)
    if (!g_initialized) return 0;
#endif
    void* entry = resolve_entry(handle);
    if (!entry) return 0;

    int64_t idx;
    Coroutine* raw;
    if (!g_coros_free.empty()) {
        idx = g_coros_free.back();
        g_coros_free.pop_back();
        g_coros[size_t(idx - 1)] = std::make_unique<Coroutine>();
        raw = g_coros[size_t(idx - 1)].get();
    } else {
        if (g_coros.size() >= (size_t(1) << 20)) return 0;
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

#if defined(_WIN32)
    raw->fiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH,
                               coro_trampoline, raw);
    if (!raw->fiber) {
        raw->in_use = false;
        g_coros[size_t(idx - 1)].reset();
        g_coros_free.push_back(idx);
        return 0;
    }
#elif defined(__APPLE__)
    // Allocate a 16-byte-aligned private stack (mmap; a data stack, NOT MAP_JIT).
    // The initial SP is the 16-aligned TOP (stack grows down). The initial
    // ctx LR (regs[11]) = the trampoline, so the first ember_ctx_switch `ret`s
    // into coro_trampoline_darwin with SP = stack top. All other regs start
    // zero — the trampoline does not read callee-saved regs before it sets its
    // own frame. x18 is intentionally left untouched (Apple platform reg).
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    size_t sz = CORO_STACK_SIZE;
    if (sz % page) sz = (sz / page + 1) * page;   // round up to a page
    void* stk = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED || !stk) {
        raw->in_use = false;
        g_coros[size_t(idx - 1)].reset();
        g_coros_free.push_back(idx);
        return 0;
    }
    raw->stack      = stk;
    raw->stack_size = sz;
    int64_t top = int64_t(static_cast<char*>(stk) + sz);
    top &= ~int64_t(15);   // 16-byte align (AAPCS64 SP at the switch point)
    std::memset(&raw->coro_ctx, 0, sizeof(CoroCtx));
    std::memset(&raw->caller_ctx, 0, sizeof(CoroCtx));
    raw->coro_ctx.regs[11] = reinterpret_cast<int64_t>(&coro_trampoline_darwin);
    raw->coro_ctx.sp       = top;
#endif
    return idx;
}

static int64_t n_coroutine_next(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
#if defined(_WIN32)
    if (!g_ctx || !g_main_fiber) return 0;
#elif defined(__APPLE__)
    if (!g_ctx || !g_initialized) return 0;
#endif
    Coroutine* co = raw_slot(handle);
    if (!co) return 0;
    if (co->done) return co->yield_value;

    co->caller_coro  = g_current_coro;
    g_current_coro   = co;
    co->started      = true;

#if defined(_WIN32)
    co->caller_fiber = GetCurrentFiber();
    SwitchToFiber(co->fiber);
#elif defined(__APPLE__)
    // Save the CURRENT context (the main thread or an outer coroutine) into
    // co->caller_ctx and switch into co->coro_ctx. On the first resume this
    // `ret`s into coro_trampoline_darwin (its LR); on a later resume it
    // `ret`s to the instruction right after the matching yield's switch.
    ember_ctx_switch(&co->caller_ctx, &co->coro_ctx);
#endif

    g_current_coro = co->caller_coro;
    return co->yield_value;
}

static int64_t n_coroutine_done(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    Coroutine* co = raw_slot(handle);
    if (!co) return 0;
    return co->done ? 1 : 0;
}

} // extern "C"

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type T = type_i64();
    Type fn_param = type_i64();
    fn_param.is_fn_handle = true;
    b.add("coroutine_start", T, {fn_param, type_i64()}, (void*)&n_coroutine_start);
    b.add("coroutine_next",  type_i64(), {T},           (void*)&n_coroutine_next);
    b.add("coroutine_done",  type_bool(), {T},           (void*)&n_coroutine_done);
    b.add("__ember_coro_yield", type_i64(), {type_i64()}, (void*)&n_coro_yield);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

bool coroutine_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count) {
    if (!ctx || !dispatch_base || slot_count <= 0) return false;
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    g_ctx           = ctx;
    g_dispatch_base = dispatch_base;
    g_slot_count    = slot_count;
#if defined(_WIN32)
    if (!IsThreadAFiber()) {
        g_main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    } else {
        g_main_fiber = GetCurrentFiber();
    }
#elif defined(__APPLE__)
    // Phase 8: no fiber conversion on Darwin. The main thread's context is
    // simply the callee-saved regs saved on the first ember_ctx_switch in
    // n_coroutine_next (into co->caller_ctx). Just mark the store ready.
    g_initialized = true;
#endif
    return true;
}

void coroutine_reset() {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    for (auto& c : g_coros) {
        if (c && c->in_use) {
#if defined(_WIN32)
            if (c->fiber) {
                DeleteFiber(c->fiber);
                c->fiber = nullptr;
            }
#elif defined(__APPLE__)
            if (c->stack && c->stack_size) {
                munmap(c->stack, c->stack_size);
                c->stack = nullptr;
                c->stack_size = 0;
            }
#endif
        }
    }
    g_coros.clear();
    g_coros_free.clear();
    g_current_coro = nullptr;
}

bool coroutine_init_keyed(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return false;
    auto& s = inst.ext_state->coroutine;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    s.ctx           = nullptr;
    s.dispatch_base = const_cast<void*>(static_cast<const void*>(
        inst.entry_table ? inst.entry_table->slots.data() : nullptr));
    s.slot_count    = int64_t(inst.logical_slot_count);
#if defined(_WIN32)
    if (!IsThreadAFiber()) {
        s.main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    } else {
        s.main_fiber = GetCurrentFiber();
    }
#endif
    s.last_start_status.ok = false;
    s.last_start_status.unsupported_mode = false;
    s.last_start_status.reason.clear();
    return true;
}

CoroutineStartStatus coroutine_last_start_status_keyed(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return CoroutineStartStatus{};
    auto& s = inst.ext_state->coroutine;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    CoroutineStartStatus out;
    out.ok               = s.last_start_status.ok;
    out.unsupported_mode = s.last_start_status.unsupported_mode;
    out.reason           = s.last_start_status.reason;
    return out;
}

} // namespace ember::ext_coroutine
