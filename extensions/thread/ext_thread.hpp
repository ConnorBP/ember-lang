// ext_thread.hpp - ember extension: in-context threads (Tier 4).
// See the full scope statement in the original header. Red 8 adds the keyed
// per-runtime APIs (§6.6, §10.3, §12.4).
#pragma once
#include "sema.hpp"
#include "context.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember { struct ModuleInstance; class DispatchKeyAdapter; }

namespace ember::ext_thread {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
bool thread_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count);
void thread_reset();

// ─── Red 8 (§6.6, §10.3, §12.4): per-runtime keyed threads ──────────────────
bool thread_init_keyed(ember::ModuleInstance& inst);
int64_t thread_join_keyed(ember::ModuleInstance& inst, int64_t handle,
                          ember::context_t& ctx,
                          const ember::DispatchKeyAdapter& adapter);

} // namespace ember::ext_thread
