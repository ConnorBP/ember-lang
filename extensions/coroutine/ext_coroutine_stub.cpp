// ext_coroutine_stub.cpp — non-Windows stub for the coroutine extension.
// Red 8 (§6.7): provides the keyed APIs with typed unsupported-mode behavior.
#include "binding_builder.hpp"
#include "ext_coroutine.hpp"
#include "context.hpp"
#include "module_instance.hpp"
#include "runtime_extension_state.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace ember::ext_coroutine {

static int64_t n_coroutine_start(int64_t, int64_t) { return 0; }
static int64_t n_coroutine_next(int64_t) { return 0; }
static int64_t n_coroutine_done(int64_t) { return 1; }

bool coroutine_init(ember::context_t*, void*, int64_t) { return false; }

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type T = type_i64();
    Type fn_param = type_i64();
    fn_param.is_fn_handle = true;
    b.add("coroutine_start", T, {fn_param, type_i64()}, (void*)&n_coroutine_start);
    b.add("coroutine_next",  type_i64(), {T},           (void*)&n_coroutine_next);
    b.add("coroutine_done",  type_bool(), {T},           (void*)&n_coroutine_done);
    b.add("__ember_coro_yield", type_i64(), {type_i64()}, (void*)&n_coroutine_start);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

void coroutine_reset() {}

bool coroutine_init_keyed(ember::ModuleInstance& inst) {
    if (!inst.ext_state) return false;
    auto& s = inst.ext_state->coroutine;
    std::lock_guard<std::recursive_mutex> guard(s.setup_mutex);
    s.last_start_status.ok = false;
    s.last_start_status.unsupported_mode = true;
    s.last_start_status.reason =
        "coroutine start unsupported on this platform (no fiber support; "
        "§6.7 fail-closed / non-Windows typed unsupported behavior)";
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
