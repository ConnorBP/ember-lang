// ext_lifecycle.cpp - ember extension: dynamic routine registration
// (docs/planning/plan_FUNCTION_REFS.md §6). See ext_lifecycle.hpp for the scope statement.
//
// Red 8 (§6.5, §10.3, §12.4): the routine table is DUAL-HOMED. The legacy
// identity path keeps a file-static store; the keyed path targets a SPECIFIC
// ModuleInstance's per-runtime state (inst.ext_state->lifecycle) so two
// runtimes carry independent routine tables (§10.3). The natives select the
// store via the current-keyed-runtime TLS. No keyed path consults the
// file-static singleton. lifecycle_tick_keyed re-resolves each handle through
// the keyed resolver AT INVOCATION (§12.4), so a replaced entry is observed.
//
// Layout-safety: both stores use the SAME Slot type
// (LifecycleRuntimeState::Slot), removing unsafe reinterpret_cast aliasing.
#include "ext_lifecycle.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include "engine.hpp"            // Red 8: ember_keyed_reentry_i64
#include "module_instance.hpp"   // Red 8: ModuleInstance, ember_current_keyed_runtime
#include "key_provider.hpp"      // Red 8: DispatchKeyAdapter
#include "module_layout.hpp"     // Red 8: resolve_keyed_dispatch
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

static LifecycleRuntimeState* current_keyed_state() {
    ModuleInstance* rt = ember_current_keyed_runtime();
    if (!rt || !rt->ext_state) return nullptr;
    return &rt->ext_state->lifecycle;
}

struct StoreView {
    std::vector<Slot>*   routines;
    std::vector<int64_t>*     free_ids;
    std::mutex*               mutex;
};

static StoreView select_store() {
    if (LifecycleRuntimeState* s = current_keyed_state()) {
        return StoreView{&s->routines, &s->free_ids, &s->mutex};
    }
    return StoreView{&g_routines, &g_free, &g_mutex};
}

extern "C" {
    static int64_t n_register_routine(int64_t handle, int64_t data) {
        try {
            StoreView st = select_store();
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

int64_t lifecycle_tick_keyed(ModuleInstance& inst, context_t& ctx,
                             const DispatchKeyAdapter& adapter) {
    if (!inst.ext_state) return 0;
    auto routines = host_routines_keyed(inst);
    int64_t sum = 0;
    for (const auto& r : routines) {
        auto rw = adapter.route_word(ModuleId{inst.module_id, 1}, inst.strategy_version,
                                     "ember/dispatch");
        if (!rw) continue;
        auto er = resolve_keyed_dispatch(&inst.record, uint32_t(r.slot), *rw.value);
        if (!er || !*er.value) continue;
        int64_t got = ember_keyed_reentry_i64(*er.value, &ctx, r.data, *rw.value);
        sum += got;
    }
    return sum;
}

} // namespace ember::ext_lifecycle
