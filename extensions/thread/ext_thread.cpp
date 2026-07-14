// ext_thread.cpp - ember extension: in-context threads (Tier 4).
// See ext_thread.hpp for the scope statement + correctness model.
//
// Red 8 (§6.6, §10.3, §12.4): DUAL-HOMED thread registry. The keyed path
// targets a SPECIFIC ModuleInstance's per-runtime state; the worker re-resolves
// its logical entry at EXECUTION time through the current immutable record
// (§12.4) and uses the core safe-call API (ember_keyed_reentry_i64, §6.5) —
// NOT an unguarded raw re-entry. Layout-safety: both stores use the SAME
// ThreadSlot type (ThreadRuntimeState::ThreadSlot), no reinterpret_cast aliasing.
#include "ext_thread.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include "engine.hpp"
#include "module_instance.hpp"
#include "key_provider.hpp"
#include "module_layout.hpp"
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

static ThreadRuntimeState* current_keyed_state() {
    ModuleInstance* rt = ember_current_keyed_runtime();
    if (!rt || !rt->ext_state) return nullptr;
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
};

static StoreView select_store() {
    if (ThreadRuntimeState* s = current_keyed_state()) {
        return StoreView{&s->threads, &s->free_ids, &s->setup_mutex,
                         &s->ctx, &s->dispatch_base, &s->slot_count,
                         s->instance, /*keyed=*/true};
    }
    return StoreView{&g_threads, &g_threads_free, &g_setup_mutex,
                     &g_ctx, &g_dispatch_base, &g_slot_count,
                     nullptr, /*keyed=*/false};
}

static ThreadSlot* raw_slot(StoreView& st, int64_t h) {
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
    // worker (the Red 8 trap/thread recovery regression). The macros in
    // context.hpp resolve to the matching primitive per platform.
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

static void thread_worker_keyed(ModuleInstance* inst, int64_t logical_handle,
                                context_t* ctx, int64_t arg,
                                ThreadSlot* slot) {
    SavedState saved;
    int64_t result = 0;
    bool trapped = false;
    int  reason  = 0;

    ctx->call_mutex.lock();
    save_state(ctx, saved);
    ctx->reset_for_call();
    ctx->has_checkpoint = true;
    // EMBER_SETJMP (see thread_worker_legacy): the keyed worker's trap stub
    // longjmps via EMBER_LONGJMP, so the checkpoint must be set with the
    // matching EMBER_SETJMP primitive (raw setjmp + __builtin_longjmp is UB
    // and segfaults — the Red 8 regression).
    if (EMBER_SETJMP(ctx->checkpoint)) {
        trapped = true;
        reason  = int(ctx->last_trap);
        ctx->has_checkpoint = false;
    } else {
        if (inst && inst->provider && logical_handle >= 0) {
            DispatchKeyAdapter adapter(inst->provider);
            auto rw = adapter.route_word(ModuleId{inst->module_id, 1},
                                         inst->strategy_version, "ember/dispatch");
            if (rw) {
                auto er = resolve_keyed_dispatch(&inst->record,
                                                 uint32_t(logical_handle), *rw.value);
                if (er && *er.value) {
                    result = ember_keyed_reentry_i64(*er.value, ctx, arg, *rw.value);
                }
            }
        }
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

extern "C" {

static int64_t n_thread_spawn(int64_t handle, int64_t arg) {
    StoreView st = select_store();
    std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
    if (!*st.ctx_pp || !*st.dispatch_base_pp || *st.slot_count_p <= 0) return 0;

    void* entry = nullptr;
    if (st.keyed) {
        if (handle < 0 || handle >= *st.slot_count_p) return 0;
    } else {
        entry = resolve_entry_legacy(handle, *st.dispatch_base_pp, *st.slot_count_p);
        if (!entry) return 0;
    }

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
    raw->done    = false;
    raw->trapped = false;
    raw->trap_reason = 0;
    raw->result  = 0;
    raw->logical_handle = handle;
    raw->arg     = arg;
    context_t* ctx  = *st.ctx_pp;

    try {
        if (st.keyed) {
            raw->th = std::thread(thread_worker_keyed, st.instance, handle, ctx, arg, raw);
        } else {
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
    ThreadSlot* s;
    {
        std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
        if (!*st.ctx_pp) return 0;
        s = raw_slot(st, handle);
    }
    if (!s) return 0;

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
    ctx->call_mutex.lock();
    if (s->th.joinable()) s->th.join();
    return trapped ? TRAP_SENTINEL : result;
}

static int64_t n_thread_trap_reason(int64_t handle) {
    StoreView st = select_store();
    std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
    ThreadSlot* s = raw_slot(st, handle);
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
    auto& s = inst.ext_state->thread;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    s.ctx           = nullptr;
    s.dispatch_base = const_cast<void*>(static_cast<const void*>(inst.entry_table->slots.data()));
    s.slot_count    = int64_t(inst.logical_slot_count);
    s.instance      = &inst;
    if (inst.record.physical_slots == nullptr && inst.logical_slot_count > 0)
        assemble_identity_dispatch_record(inst);
    return true;
}

int64_t thread_join_keyed(ember::ModuleInstance& inst, int64_t handle,
                          ember::context_t& ctx,
                          const ember::DispatchKeyAdapter& adapter) {
    if (!inst.ext_state) return 0;
    auto& s = inst.ext_state->thread;
    {
        std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
        s.ctx = &ctx;
    }
    StoreView st{&s.threads, &s.free_ids, &s.setup_mutex,
                 &s.ctx, &s.dispatch_base, &s.slot_count,
                 s.instance, /*keyed=*/true};
    ThreadSlot* slot;
    {
        std::lock_guard<std::recursive_mutex> guard(*st.setup_mutex);
        slot = raw_slot(st, handle);
    }
    if (!slot) return 0;
    (void)adapter;
    ctx.call_mutex.unlock();
    bool trapped = false;
    int64_t result = 0;
    {
        std::unique_lock<std::mutex> dlk(slot->done_lock);
        slot->done_cv.wait(dlk, [&] { return slot->done; });
        trapped = slot->trapped;
        result  = slot->result;
    }
    ctx.call_mutex.lock();
    if (slot->th.joinable()) slot->th.join();
    return trapped ? TRAP_SENTINEL : result;
}

} // namespace ember::ext_thread
