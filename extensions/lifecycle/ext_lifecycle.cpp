// ext_lifecycle.cpp - ember extension: dynamic routine registration
// (plan_FUNCTION_REFS.md §6). See ext_lifecycle.hpp for the scope statement.
//
// A Tier-0-shaped extension mirroring ext_array/ext_sync (1-based ids, bounds
// check, register_natives/reset, host accessor). One TU per extensions/README.md
// purity. The routine table is a std::vector<RoutineSlot>; register appends +
// returns the 1-based id; unregister tombstones (marks active=false, O(1)); reset
// clears. Not synchronized — register_routine/unregister are single-script-thread
// ops under the U2 contract (the script side is single-threaded per context); the
// HOST reads host_routines() on its own thread, which is safe because the host
// either (a) reads after the script's @entry completes (the common case — the
// script registers in @entry, the host ticks after), or (b) the host treats the
// read as approximate (a snapshot). A std::atomic flag or mutex would be added
// only if a real concurrent-register-while-ticking use case appears (YAGNI).
#include "ext_lifecycle.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons, make_prim

namespace ember::ext_lifecycle {

struct RoutineSlot {
    bool active = false;
    int64_t slot = 0;    // the fn handle (a dispatch-table slot from &fn)
    int64_t data = 0;    // the opaque data arg the script passed
};
static std::vector<RoutineSlot> g_routines;
static std::vector<int64_t>     g_free;   // free-list of freed ids (tombstones)

extern "C" {
    // register_routine(fn handle, i64 data) -> i64 routine_id (1-based).
    // The handle is the slot index (from &fn in the script). We store it + data.
    static int64_t n_register_routine(int64_t handle, int64_t data) {
        if (!g_free.empty()) {
            int64_t id = g_free.back(); g_free.pop_back();
            g_routines[size_t(id - 1)] = {true, handle, data};
            return id;
        }
        g_routines.push_back({true, handle, data});
        return int64_t(g_routines.size());   // 1-based id
    }
    // unregister_routine(i64 id) -> i64 (1 = removed, 0 = no such id).
    static int64_t n_unregister_routine(int64_t id) {
        if (id < 1 || id > int64_t(g_routines.size())) return 0;
        auto& s = g_routines[size_t(id - 1)];
        if (!s.active) return 0;
        s.active = false;
        g_free.push_back(id);
        return 1;
    }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    // The fn-handle param type: i64 with is_fn_handle=true (so sema tags it as
    // a fn handle, distinct from a plain i64 — a script can't pass a forged
    // i64 here; it must pass a &fn or a fn-typed var).
    Type fn_handle = type_i64(); fn_handle.is_fn_handle = true;
    b.add("register_routine",   type_i64(), {fn_handle, type_i64()}, (void*)&n_register_routine);
    b.add("unregister_routine", type_i64(), {type_i64()},            (void*)&n_unregister_routine);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

void reset() {
    g_routines.clear();
    g_free.clear();
}

std::vector<Routine> host_routines() {
    std::vector<Routine> out;
    for (size_t i = 0; i < g_routines.size(); ++i) {
        const auto& s = g_routines[i];
        if (s.active) out.push_back({s.slot, s.data});
    }
    return out;
}

int64_t host_count() {
    int64_t c = 0;
    for (const auto& s : g_routines) if (s.active) ++c;
    return c;
}

} // namespace ember::ext_lifecycle
