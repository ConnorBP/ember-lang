// ext_lifecycle.cpp - ember extension: dynamic routine registration
// (docs/planning/plan_FUNCTION_REFS.md §6). See ext_lifecycle.hpp for the scope statement.
//
// Red 8 (§6.5, §10.3, §12.4): the routine table is DUAL-HOMED. The legacy
// identity path keeps a file-static store; the keyed path targets a SPECIFIC
// ModuleInstance's per-runtime state (inst.ext_state->lifecycle) so two
// runtimes carry independent routine tables (§10.3). The natives select the
// store via the current-keyed-runtime TLS. No keyed path consults the
// file-static singleton.
//
// KEYED FAIL-CLOSED (task constraint): when the TLS current-keyed-runtime is
// active (non-null) but the runtime's ext_state is null, the keyed store
// selector does NOT fall back to the file-static singleton — it returns a
// null StoreView so the native fails closed (returns 0 / no-op). Only when NO
// keyed runtime is active on this thread does the legacy path target the
// file-static default store. This prevents a keyed native from silently
// selecting legacy globals.
//
// lifecycle_tick_keyed re-resolves each handle through the GUARDED CORE
// keyed-call API (ember_call_keyed_i64_by_slot) AT INVOCATION (§12.4), so a
// replaced entry is observed. The core API establishes the current-runtime
// TLS, the recoverable checkpoint, the reload/generation guard, and cleans up
// on every normal AND trapped exit — the raw ember_keyed_reentry_i64 thunk
// does NONE of those, so it MUST NOT be used here (task constraint: use the
// guarded core keyed-call API, not an unguarded raw re-entry).
//
// Layout-safety: both stores use the SAME Slot type
// (LifecycleRuntimeState::Slot), removing unsafe reinterpret_cast aliasing.
#include "ext_lifecycle.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include "engine.hpp"            // Red 8: ember_call_keyed_i64_by_slot
#include "module_instance.hpp"   // Red 8: ModuleInstance, ember_current_keyed_runtime
#include "key_provider.hpp"      // Red 8: DispatchKeyAdapter
#include "runtime_extension_state.hpp" // Red 8: LifecycleRuntimeState
#include <vector>
#include <mutex>
#include <new>
#include <stdexcept>

using namespace ember;

namespace ember::ext_lifecycle {

using Slot = LifecycleRuntimeState::Slot;

static std::vector<Slot> g_routines;
static std::vector<int64_t>     g_free;
static std::mutex               g_mutex;

// Returns the per-runtime keyed state when a keyed runtime is active on this
// thread. Returns nullptr when NO keyed runtime is active (the legacy identity
// path serves that case via the file-static default store). When a keyed
// runtime IS active but its ext_state is null, returns a sentinel non-null
// pointer so select_store can fail closed instead of falling back to the
// file-static singleton (task constraint).
static LifecycleRuntimeState* g_fail_closed_sentinel = nullptr;
static LifecycleRuntimeState* fail_closed_sentinel() {
    if (!g_fail_closed_sentinel)
        g_fail_closed_sentinel = reinterpret_cast<LifecycleRuntimeState*>(uintptr_t(0x1));
    return g_fail_closed_sentinel;
}

static LifecycleRuntimeState* current_keyed_state() {
    ModuleInstance* rt = ember_current_keyed_runtime();
    if (!rt) return nullptr;  // no keyed runtime active -> legacy path
    if (!rt->ext_state) return fail_closed_sentinel();  // keyed active but no state -> fail closed
    return &rt->ext_state->lifecycle;
}

struct StoreView {
    std::vector<Slot>*   routines;
    std::vector<int64_t>*     free_ids;
    std::mutex*               mutex;
    bool valid;
};

static StoreView select_store() {
    LifecycleRuntimeState* s = current_keyed_state();
    if (s == fail_closed_sentinel()) {
        // Keyed TLS active but ext_state is null: fail closed (do NOT fall
        // back to the file-static singleton).
        return StoreView{nullptr, nullptr, nullptr, /*valid=*/false};
    }
    if (s) {
        return StoreView{&s->routines, &s->free_ids, &s->mutex, /*valid=*/true};
    }
    // No keyed runtime active: legacy identity path -> file-static default store.
    return StoreView{&g_routines, &g_free, &g_mutex, /*valid=*/true};
}

extern "C" {
    static int64_t n_register_routine(int64_t handle, int64_t data) {
        try {
            StoreView st = select_store();
            if (!st.valid) return 0;  // keyed fail-closed
            std::lock_guard<std::mutex> guard(*st.mutex);
            if (!st.free_ids->empty()) {
                int64_t id = st.free_ids->back(); st.free_ids->pop_back();
                (*st.routines)[size_t(id - 1)] = {true, handle, data};
                return id;
            }
            st.routines->push_back({true, handle, data});
            return int64_t(st.routines->size());
        } catch (const std::bad_alloc&) { return 0; }
          catch (const std::length_error&) { return 0; }
    }
    static int64_t n_unregister_routine(int64_t id) {
        StoreView st = select_store();
        if (!st.valid) return 0;  // keyed fail-closed
        std::lock_guard<std::mutex> guard(*st.mutex);
        if (id < 1 || id > int64_t(st.routines->size())) return 0;
        auto& s = (*st.routines)[size_t(id - 1)];
        if (!s.active) return 0;
        s.active = false;
        st.free_ids->push_back(id);
        return 1;
    }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type fn_handle = type_i64(); fn_handle.is_fn_handle = true;
    b.add("register_routine",   type_i64(), {fn_handle, type_i64()}, (void*)&n_register_routine);
    b.add("unregister_routine", type_i64(), {type_i64()},            (void*)&n_unregister_routine);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

void reset() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_routines.clear();
    g_free.clear();
}

std::vector<Routine> host_routines() {
    std::lock_guard<std::mutex> guard(g_mutex);
    std::vector<Routine> out;
    for (size_t i = 0; i < g_routines.size(); ++i) {
        const auto& s = g_routines[i];
        if (s.active) out.push_back({s.slot, s.data});
    }
    return out;
}

int64_t host_count() {
    std::lock_guard<std::mutex> guard(g_mutex);
    int64_t c = 0;
    for (const auto& s : g_routines) if (s.active) ++c;
    return c;
}

bool lifecycle_init_keyed(ModuleInstance& inst) {
    if (!inst.ext_state) return false;
    auto& s = inst.ext_state->lifecycle;
    std::lock_guard<std::mutex> guard(s.mutex);
    s.routines.clear();
    s.free_ids.clear();
    return true;
}

std::vector<Routine> host_routines_keyed(ModuleInstance& inst) {
    std::vector<Routine> out;
    if (!inst.ext_state) return out;
    auto& s = inst.ext_state->lifecycle;
    std::lock_guard<std::mutex> guard(s.mutex);
    for (const auto& r : s.routines)
        if (r.active) out.push_back({r.slot, r.data});
    return out;
}

// lifecycle_tick_keyed re-resolves each registered routine's logical handle
// through the GUARDED CORE keyed-call API (ember_call_keyed_i64_by_slot) at
// INVOCATION time (§12.4), so a replaced entry is observed. The core API:
//   - derives the route word from the provider via the adapter (transient);
//   - resolves the logical slot through the instance's CURRENT immutable
//     ModuleDispatchRecord (NOT raw entry_table[logical_slot]);
//   - sets the current-keyed-runtime TLS on entry + clears on every exit;
//   - acquires the reload/generation guard from resolution through return/trap;
//   - establishes a setjmp checkpoint (when inst.trap_stub is set) and returns
//     a structured CallResult{trapped=true} on a trap;
//   - cleans all runtime/TLS state on every normal AND trapped exit.
// The raw ember_keyed_reentry_i64 thunk does NONE of these — it is a bare
// assembly re-entry that only installs r14/r15 — so it MUST NOT be used here
// (task constraint). Each routine's logical identity (the slot from &fn) is
// retained; the entry is resolved at invocation, not cached at registration.
int64_t lifecycle_tick_keyed(ModuleInstance& inst, context_t& ctx,
                             const DispatchKeyAdapter& adapter) {
    if (!inst.ext_state) return 0;
    if (!inst.provider) return 0;
    auto routines = host_routines_keyed(inst);
    int64_t sum = 0;
    for (const auto& r : routines) {
        // Re-resolve at invocation through the guarded core API (§12.4).
        // The logical slot r.slot is the routine's retained logical identity;
        // the core API derives the route word + resolves through the record +
        // establishes TLS/guard/checkpoint. r.data is the opaque arg.
        auto cr = ember_call_keyed_i64_by_slot(inst, uint32_t(r.slot), ctx, r.data, adapter);
        if (cr.ok) {
            sum += cr.value;
        }
        // On a trapped routine (cr.trapped), the core API already cleaned up
        // TLS/guard/checkpoint and reset ctx via reset_for_call. We skip the
        // trapped routine's contribution (value 0) and continue ticking the
        // remaining routines — a single routine's trap does not abort the
        // whole tick (the structured CallResult lets us recover per-routine).
    }
    return sum;
}

} // namespace ember::ext_lifecycle
