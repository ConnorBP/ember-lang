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
// thread_init_keyed requires the instance to be std::make_shared-managed
// (enable_shared_from_this) so the keyed worker can capture a
// shared_ptr<ModuleInstance> for shared lifetime ownership. Returns false if
// the instance is not shared_ptr-managed, has no ext_state/provider/entry
// table, or has zero logical slots.
// thread_join_keyed: the keyed worker owns its own context_t (§6.6), so join
// does NOT manage a shared call_mutex. The ctx + adapter parameters are
// retained for API compatibility but are NOT used for locking (the worker
// re-derives its own adapter from the instance's provider). Join is a simple
// wait-for-done + thread-join + slot-retire (ownership-explicit: locks only
// slot->done_lock, never a mutex the caller did not lock).
bool thread_init_keyed(ember::ModuleInstance& inst);
int64_t thread_join_keyed(ember::ModuleInstance& inst, int64_t handle,
                          ember::context_t& ctx,
                          const ember::DispatchKeyAdapter& adapter);

// ─── Red 8 test gate (§12.4): install a gate on the runtime's thread state ─
// so a test can deterministically delay the keyed worker's execution-time
// entry resolution. install_worker_gate creates + enables the gate (closed).
// open_worker_gate opens it so blocked workers proceed + resolve. The gate is
// per-runtime; it carries NO route material (it is a test synchronization
// primitive, not a dispatch input).
void install_worker_gate(ember::ModuleInstance& inst);
void open_worker_gate(ember::ModuleInstance& inst);

} // namespace ember::ext_thread
