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

#include <windows.h>

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
static void*                g_main_fiber    = nullptr;

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

static void n_coro_yield(int64_t value) {
    Coroutine* co = g_current_coro;
    if (!co) return;
    co->yield_value = value;
    void* caller = co->caller_fiber;
    Coroutine* prev = co->caller_coro;
    g_current_coro = prev;
    SwitchToFiber(caller);
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
    if (!g_main_fiber) return 0;
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

    raw->fiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH,
                               coro_trampoline, raw);
    if (!raw->fiber) {
        raw->in_use = false;
        g_coros[size_t(idx - 1)].reset();
        g_coros_free.push_back(idx);
        return 0;
    }
    return idx;
}

static int64_t n_coroutine_next(int64_t handle) {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    if (!g_ctx || !g_main_fiber) return 0;
    Coroutine* co = raw_slot(handle);
    if (!co) return 0;
    if (co->done) return co->yield_value;

    co->caller_fiber = GetCurrentFiber();
    co->caller_coro  = g_current_coro;
    g_current_coro   = co;
    co->started      = true;

    SwitchToFiber(co->fiber);

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
    if (!IsThreadAFiber()) {
        g_main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    } else {
        g_main_fiber = GetCurrentFiber();
    }
    return true;
}

void coroutine_reset() {
    std::lock_guard<std::recursive_mutex> guard(g_setup_mutex);
    for (auto& c : g_coros) {
        if (c && c->in_use && c->fiber) {
            DeleteFiber(c->fiber);
            c->fiber = nullptr;
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
    if (!IsThreadAFiber()) {
        s.main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    } else {
        s.main_fiber = GetCurrentFiber();
    }
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
