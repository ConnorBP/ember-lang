// ext_thread.hpp - ember extension: in-context threads (Tier 4).
//
// CONCURRENT-ENTRY MODEL (replaces the legacy save/restore + call_mutex
// serialization). A spawned worker enters JIT'd code CONCURRENTLY with the
// host + sibling workers through the ONE shared dispatch context active at
// thread_spawn. Because a context_t's per-call fields (checkpoint / budget /
// call depth / catch stack / gc_frame_head) are not concurrently-safe in a
// single context_t, each worker is given its OWN per-call context_t seeded
// from the shared host context's settings (budget, max_call_depth, the shared
// typed-global root descriptor, the shared GC runtime pointer). All workers +
// the host share the ONE immutable dispatch table (atomic acquire reads) + the
// ONE context-owned GC heap. There is NO call_mutex serialization + NO
// save/restore of the host's per-call fields: the host's context_t is left
// untouched while the workers run concurrently (true parallel execution — the
// test gate observes >= 2 workers inside JIT at once).
//
//   * Legacy workers use the context supplied to thread_init (the host's
//     context) as the seed source + the dispatch table it registered.
//   * Keyed workers capture the current shared context from the keyed host
//     boundary (ember_current_keyed_context) as the seed source, while
//     retaining the existing shared_ptr<ModuleInstance> (shared lifetime
//     ownership), the generation guard, execution-time dispatch resolution
//     through ember_call_keyed_i64_by_slot, and the fail-closed behavior.
//
// JOIN (task constraint): thread_join + thread_join_keyed wait ONLY on slot
// synchronization (the slot's done flag + the OS thread join). They do NOT
// unlock/relock a shared call_mutex (there is none), so nested spawn/join is
// deadlock-free (a worker that itself spawns + joins a grandchild never
// contends on the outer context's lock).
//
// GC INTEGRATION: when the seed context carries a shared GC runtime
// (ctx.gc_runtime, set by ext_gc::gc_attach_context), each worker joins it via
// ext_gc::gc_thread_enter before entering JIT + leaves it via
// ext_gc::gc_thread_exit on every normal + trapped exit, so the worker
// allocates into the SAME shared heap + its per-thread shadow-stack head is
// scanned by the cooperative stop-the-world collector, and a trap cannot leave
// an abandoned participant record registered.
//
// PRESERVED: atomic acquire dispatch reads, worker lifetime guarantees (the
// shared_ptr<ModuleInstance> for keyed workers; the slot's done flag for
// legacy), the trap sentinel (INT64_MIN) + trap_reason behavior, and the
// immutable finalized JIT code every worker enters.
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
// thread_join_keyed: the keyed worker seeds its own per-call context_t from the
// shared host context captured at the keyed host boundary (no shared
// call_mutex). The ctx + adapter parameters are retained for API compatibility
// but are NOT used for locking (the worker re-derives its own adapter from the
// instance's provider). Join is a simple wait-for-done + thread-join + slot-
// retire (ownership-explicit: locks only slot->done_lock, never a mutex the
// caller did not lock).
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
