// ext_lifecycle.hpp - ember extension: dynamic routine registration
// (docs/planning/plan_FUNCTION_REFS.md §6). The Tier 2 function-ref feature enables this:
// a script passes `&fn` (a dispatch-table slot handle) to a host native that
// stores it keyed by a routine id; the host later calls the routine per frame
// via the dispatch table (the SAME call mechanism as the static @on_tick
// annotation path, just discovered by the script at runtime).
//
// An ember *extension* (see ember/extensions/README.md): reusable,
// non-cheat-specific. Host-owned routine table behind opaque i64 ids; reset()
// clears it. Mirrors ext_sync's shape (1-based ids, id(h) bounds check,
// register_natives/reset, public accessor for host-side reach-in).
//
// === SCOPE (docs/planning/plan_FUNCTION_REFS.md §6.2/§6.3) ===
// register_routine(handle, data) -> id: stores (slot=handle, data). The handle
//   is a fn-typed i64 whose provenance sema already validated at the &fn site
//   (plan §4.2) — so it's a registered slot by construction. The host trusts it
//   the same way it trusts any sema-resolved slot (§6.3).
// unregister_routine(id) -> i64 (1=removed, 0=no such id): drops the routine.
// The HOST calls a stored routine via the dispatch table: table.get(slot) with
//   arg = data. The guard (§5.2) is NOT duplicated on the host side — the host
//   is trusted (SAFETY §1) and the slot came from a &fn sema validated. The
//   host's own bounds check (slot < table.size && entry != null) is one line;
//   the host_routines() accessor below returns the (slot, data) pairs for the
//   host to iterate + call.
#pragma once
#include "sema.hpp"
#include "context.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember { struct ModuleInstance; class DispatchKeyAdapter; }

namespace ember::ext_lifecycle {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
void reset();
struct Routine { int64_t slot; int64_t data; };
std::vector<Routine> host_routines();
int64_t host_count();

// ─── Red 8 (§6.5, §10.3, §12.4): per-runtime keyed lifecycle ────────────────
bool lifecycle_init_keyed(ModuleInstance& inst);
std::vector<Routine> host_routines_keyed(ModuleInstance& inst);
int64_t lifecycle_tick_keyed(ModuleInstance& inst, context_t& ctx,
                             const DispatchKeyAdapter& adapter);

} // namespace ember::ext_lifecycle
