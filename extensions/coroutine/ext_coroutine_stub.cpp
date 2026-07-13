// ext_coroutine_stub.cpp — non-Windows stub for the coroutine extension.
//
// The real coroutine extension (ext_coroutine.cpp) uses Windows fibers
// (CreateFiber/SwitchToFiber). On non-Windows platforms a ucontext port is a
// TODO (ROADMAP). This stub provides the same register_natives symbol so the
// extension links, but the natives return error values (coroutine_start
// returns 0 = invalid handle, coroutine_next returns 0, coroutine_done
// returns true). Scripts using coroutines on non-Windows will get a clean
// runtime error rather than a link failure.
#include "binding_builder.hpp"
#include "ext_coroutine.hpp"

namespace ember::ext_coroutine {

static int64_t n_coroutine_start(int64_t, int64_t) { return 0; }  // 0 = invalid
static int64_t n_coroutine_next(int64_t) { return 0; }
static int64_t n_coroutine_done(int64_t) { return 1; }  // true = done (nothing to run)

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b(m);
    auto T = type_i64();
    b.add("coroutine_start", T, {T, type_i64()}, (void*)&n_coroutine_start);
    b.add("coroutine_next",  T, {T},             (void*)&n_coroutine_next);
    b.add("coroutine_done",  type_bool(), {T},   (void*)&n_coroutine_done);
}

void coroutine_reset() {}

} // namespace ember::ext_coroutine
