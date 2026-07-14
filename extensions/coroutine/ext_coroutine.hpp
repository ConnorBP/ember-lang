// ext_coroutine.hpp - ember extension: coroutines with yield (#21).
// See the full scope statement in the original header. Red 8 adds the keyed
// per-runtime APIs (§6.7, §10.3) with fail-closed behavior.
#pragma once
#include "ast.hpp"
#include "binding_builder.hpp"
#include "context.hpp"
#include <cstdint>
#include <string>

namespace ember { struct ModuleInstance; }

namespace ember::ext_coroutine {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
bool coroutine_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count);
void coroutine_reset();

// ─── Red 8 (§6.7, §10.3): per-runtime keyed coroutines (fail-closed) ────────
bool coroutine_init_keyed(ember::ModuleInstance& inst);

struct CoroutineStartStatus {
    bool ok               = false;
    bool unsupported_mode = false;
    std::string reason;
};
CoroutineStartStatus coroutine_last_start_status_keyed(ember::ModuleInstance& inst);

} // namespace ember::ext_coroutine
