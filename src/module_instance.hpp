// module_instance.hpp — Red 5 + Red 8
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §10.3): the host-owned object
// that keeps all of a module's lifetimes together.
//
// This is the GREEN side of the §10.3 ModuleInstance contract (Red 5 laid the
// field set + the safe-API consult pattern; Red 8 migrates the per-runtime
// extension state off the file-static globals onto this container and routes
// keyed resolution through the immutable ModuleDispatchRecord). A
// ModuleInstance owns (or borrows with a documented lifetime) the per-runtime
// state the keyed outer thunk + the keyed call APIs consult:
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
//   - the per-runtime extension state (lifecycle routines, thread registry,
//     coroutine fibers) scoped to THIS runtime — §10.3 mandates these move off
//     the file-static globals and onto the per-runtime container. Red 8 lands
//     that migration: the instance owns a shared_ptr<RuntimeExtensionState>
//     (lifecycle / thread / coroutine sub-state) plus the immutable
//     ModuleDispatchRecord + its storage the keyed host boundary resolves
//     through, and an optional hot-reload domain for the generation guard.
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
#include "module_layout.hpp"         // DispatchMode, ModuleDispatchRecord, RecordBuilderStorage
#include "runtime_extension_state.hpp"  // Red 8: per-runtime lifecycle/thread/coroutine state

namespace ember {

// Forward decl (src/hot_reload_domain.hpp) — the instance may borrow a reload
// domain so the safe keyed API can hold an ExecutionGuard from resolution
// through return/trap (Red 8 §9.8 / §12.4). Forward-declared (not included)
// here so this header does not pull jit_memory.hpp / dispatch_table internals
// beyond what it already includes; engine.cpp includes hot_reload_domain.hpp.
class HotReloadDomain;

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
    // the record). Empty = the safe API cannot resolve by name (the host
    // must populate it or use a by-slot overload / lower-level resolver).
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

    // ─── Red 8: per-runtime extension state (§10.3, replaces the Red 5
    // future_runtime_state opaque pointer) ─────────────────────────────
    // The ownership-safe per-runtime container for lifecycle / thread /
    // coroutine state. The §4.10 / §10.3-listed file-static extension globals
    // are a process-global singleton hazard; Red 8 migrates each extension's
    // mutable state off those globals and onto this per-runtime container.
    // Shared ownership so an extension native running under the keyed host
    // boundary (and a spawned OS thread) can hold a reference to the current
    // runtime's state without a process-global lookup. Null = the host has
    // not allocated per-runtime state (the legacy identity wrappers + the
    // file-static default store serve that case). The container stores NO
    // route material (§6.4, §10.3, §14.6): no route word, no expected key, no
    // fingerprint, no digest/verifier — only the extensions' mutable
    // operational state + per-runtime context/dispatch-base/slot-count
    // handles + back-pointers for re-resolution.
    std::shared_ptr<RuntimeExtensionState> ext_state;

    // ─── Red 8: the immutable dispatch record + its storage (§9.8, §10.1,
    // §10.3) ─────────────────────────────────────────────────────────────
    // The keyed host boundary resolves a logical callable through the
    // instance's CURRENT immutable ModuleDispatchRecord (its logical routes /
    // domains / allowlist + the borrowed physical slot storage) and the
    // transient provider-derived route word — NOT raw entry_table[logical_slot]
    // (§9.8). The instance owns the record's storage (routes / domains /
    // descriptors / allowlist — key-independent metadata, §10.1) and carries a
    // borrowed view (`record`) over it. The host (or assemble_identity_
    // dispatch_record) populates these; the safe keyed resolvers consult
    // `record`. The storage + record carry NO route material (§3.3, §14.6):
    // the abi_fingerprint in routes/domains is the canonical calling-convention
    // classifier (Red 2), not a machine fingerprint; the route word is a
    // per-call transient, never stored here.
    RecordBuilderStorage record_storage;
    ModuleDispatchRecord record{};

    // ─── Red 8: the applicable hot-reload/generation guard (§9.8, §12.4) ───
    // When non-null, the safe keyed API holds an ExecutionGuard on this domain
    // from resolution through return or trap, so a reload that retires the
    // replaced page cannot free code still in execution. Borrowed (the host
    // owns the domain); null = no reload domain (the guard is a no-op). The
    // guard is held across the call tree (nested script calls share it); the
    // safe API manually leaves it on both normal return and trapped longjmp
    // recovery (longjmp does not run C++ destructors, so the guard cannot be
    // RAII-only across a trap).
    HotReloadDomain* reload_domain = nullptr;
};

// ─── Red 8: assemble an identity ModuleDispatchRecord on a ModuleInstance ──
// (§9.8, §10.1). Builds a minimal identity record over the instance's borrowed
// entry_table + counts: logical routes where route[i].logical_slot == i (each
// in a single identity domain), an all-set logical allowlist, and the physical
// slot storage pointing at the instance's entry_table. The keyed resolver then
// resolves a logical slot through the record (identity mode ->
// physical_slots[logical_slot]) instead of raw entry_table[logical_slot]. For
// a keyed module the host builds the record via build_module_dispatch_record
// (Red 4) from a layout plan and stores it on the instance. Returns true on
// success, false on a missing entry_table or zero counts. The record carries
// NO route material (§3.3, §14.6).
bool assemble_identity_dispatch_record(ModuleInstance& inst);

// ─── Red 8: the current keyed runtime TLS (§6.6, §10.3) ─────────────────────
// The keyed host boundary sets this thread-local to identify the current
// keyed runtime on entry and clears it on every normal/trapped exit. It
// identifies a RUNTIME (a ModuleInstance*); it carries NO route material
// (a bare pointer, §6.4). Extension natives consult it to find the current
// runtime's RuntimeExtensionState. Returns null when no keyed runtime is
// active on this thread (the legacy identity wrappers serve that case via the
// file-static default store).
struct ModuleInstance* ember_current_keyed_runtime() noexcept;

// ─── Red 8: the logical callable identity for the keyed host boundary ──────
// (§2.1, §9.8). A lightweight logical identity: the logical slot within the
// module (the module is implied by the ModuleInstance the resolver is called
// on). The abi_fingerprint is optional (0 = the resolver validates the route
// against its domain's fingerprint, which is the canonical classifier, not a
// machine fingerprint — §7.1, §14.6). A physical dispatch index is NEVER
// stored here (§2.3, §4.6).
struct LogicalCallableId {
    uint32_t logical_slot = 0;
    uint64_t abi_fingerprint = 0;  // optional; 0 = let the route's domain own it
};

} // namespace ember
