// module_instance.hpp — Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §10.3): the host-owned object that keeps all of a module's lifetimes together.
//
// This is the GREEN side of the §10.3 ModuleInstance contract. A ModuleInstance
// owns (or borrows with a documented lifetime) the per-runtime state the keyed
// outer thunk + the keyed call APIs consult:
//
//   - the stable module identity (§2.1, §6.1);
//   - the dispatch mode + strategy version;
//   - the immutable ModuleDispatchRecord generation (the dispatch-record fields
//     a keyed resolver needs: physical/logical counts, the entry table base);
//   - the provider + strategy ownership references (§6.1: shared_ptr<const
//     DerivedMaterialProvider>, held for the full operation lifetime);
//   - the executable pages + globals (borrowed from the host's CompiledFns /
//     LoadedModule; the instance does not own the JIT pages itself in this
//     phase — it holds the dispatch_base + entry_table pointers the safe APIs
//     resolve through);
//   - the host-provided trap stub (the safe API establishes a checkpoint and
//     the trap stub longjmps to it; the instance carries the stub so a host
//     wires it once per runtime, not per call);
//   - future runtime state (lifecycle routines, thread registry, coroutine
//     fibers) scoped to THIS runtime — §10.3 mandates these move off the file-
//     static globals and onto the per-runtime container. This phase does NOT
//     migrate the extensions (that is Red 8); the ModuleInstance is the
//     designated owner the migration will target, so the field is reserved here
//     as a documented future slot.
//
// Constraints (§6.4, §10.3, §14.6): NO route material is stored in the
// ModuleInstance or in any module record. The route word is a per-call
// transient derived at the outer boundary (§6.3) and installed in r15 for the
// call tree; it never lives on the instance. NO expected key, machine
// fingerprint, key digest, or verifier is stored (§3.3, §14.6). The instance
// holds a provider REFERENCE (shared_ptr), not a key; the provider's root
// machine material stays inside the provider (§6.1).
//
// Lifetime: a ModuleInstance is a plain struct a host constructs per runtime
// (§6.6: "Each independently entered OS thread ... invokes the provider at its
// outer keyed thunk"). Two ModuleInstances in one process carry independent
// dispatch records + provider references; the keyed call APIs consult the
// instance passed to each call, never a process-global. This is the §10.3
// precondition the keyed feature builds on (the file-static extension globals
// are a separate, §4.10/§10.3-listed hazard that Red 8 migrates onto this
// container).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "context.hpp"               // TrapStub (the trap-stub function-pointer type)
#include "dispatch_table.hpp"        // DispatchTable (borrowed entry table)
#include "extension_registry.hpp"    // shared ExtensionResult vocabulary
#include "key_provider.hpp"          // DerivedMaterialProvider (shared ownership)
#include "module_layout.hpp"         // DispatchMode, ModuleDispatchRecord

namespace ember {

// ─── ModuleInstance (§10.3) ──────────────────────────────────────────────
// A host-owned, per-runtime container. Plain data so a host constructs one
// without a factory; the safe keyed APIs (ember_call_keyed_*) take a reference
// and consult its fields. The instance borrows the entry table + dispatch
// storage from the host's CompiledFns / LoadedModule (the host guarantees they
// outlive the instance); the instance OWNS the provider reference (shared_ptr)
// and the strategy version. The dispatch record's immutable fields are
// assembled here from the compiled module's counts + the entry table base; a
// later phase (Red 7) publishes the record through one atomic pointer swap
// (§10.2). In this phase the record is assembled per-call by the safe API
// from the instance's fields (the §10.1 shape), so a half-published record is
// impossible — the safe API validates before publication.
//
// Field set (§10.3 + the task's "stable module ID, dispatch-record
// generation/storage, provider/strategy references, executable pages, globals,
// and future runtime state"):
//   - module_id, strategy_version:    the stable identity + versioned contract.
//   - mode:                           identity or keyed (§10.1).
//   - physical/logical_slot_count:    the dispatch-record counts (§2.3, §9.7).
//   - dispatch_base, entry_table:     the executable pages + dispatch storage
//                                     (borrowed; the host owns the CompiledFns).
//   - provider:                       the immutable provider reference (§6.1).
//   - trap_stub:                       the host-provided trap stub (§9.8: the
//                                     safe API establishes a checkpoint; the
//                                     stub longjmps to it). Null = no recoverable
//                                     trap (the JIT'd code's trap sites emit ud2,
//                                     the legacy default — the safe API does not
//                                     install a checkpoint in that case).
//   - globals_base:                   the globals block base (borrowed; the host
//                                     owns the GlobalsBlock).
//
// The instance stores NO field for: an expected key, a machine fingerprint, a
// key digest/hash, a verifier/comparison constant, a route word, or a predecoded
// permutation (§3.3, §6.4, §14.6). The route word is supplied PER CALL to the
// safe API (derived from the provider via the adapter) and installed in r15
// for the call tree only.
struct ModuleInstance {
    std::string module_id;
    uint32_t strategy_version = 1;
    DispatchMode mode = DispatchMode::Identity;
    uint32_t physical_slot_count = 0;
    uint32_t logical_slot_count = 0;
    int64_t dispatch_base = 0;
    const DispatchTable* entry_table = nullptr;
    // The name -> logical-slot map for the safe keyed APIs (§9.8). A host
    // populates this from its compiled module's slots so the safe APIs can
    // resolve "main"/named exports to a logical slot (then to the entry via
    // entry_table). Empty = the safe API cannot resolve by name (the host
    // must populate it or use a lower-level entry resolver).
    std::unordered_map<std::string, uint32_t> named_entries;
    std::shared_ptr<const DerivedMaterialProvider> provider;
    // The host-provided trap stub (context.hpp TrapStub). When set, the safe
    // keyed API establishes a setjmp checkpoint on the supplied context_t
    // before entering the thunk, and any trap fires the stub (which longjmps
    // to that checkpoint). When null, traps emit ud2 (the legacy default) and
    // the safe API does NOT install a checkpoint — a trap is a hard fault.
    void* trap_stub = nullptr;
    // The globals block base (borrowed; the host owns the GlobalsBlock). Used
    // by hosts that seed globals; the keyed call APIs do not consult it
    // directly (the JIT'd code reads globals through the baked globals_base),
    // but it is part of the §10.3 "globals" ownership slot.
    int64_t globals_base = 0;

    // ─── Future runtime state (§10.3, reserved) ─────────────────────────
    // The §10.3-listed file-static extension globals (g_routines/g_free,
    // g_ctx/g_dispatch_base/g_slot_count/g_threads, the coroutine fiber store)
    // are a process-global singleton hazard. Red 8 migrates each extension's
    // mutable state off the file-scope globals and onto this per-runtime
    // container. This phase reserves the slot (a documented opaque pointer the
    // migration will populate) so the ModuleInstance is the designated owner
    // from the start. Null in this phase; a non-null value in a later phase
    // means the extensions have been migrated.
    void* future_runtime_state = nullptr;
};

} // namespace ember
