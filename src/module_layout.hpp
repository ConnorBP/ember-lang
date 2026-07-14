// module_layout.hpp — Red 3 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §5, §9.2, §14.3): logical/physical module layout planning.
//
// This is the GREEN side of the logical/physical layout contract. It takes a
// classified ModuleManifest (the output of the parse + sema + stable-logical-
// slot-assignment build step, §9.2) and produces a ModuleLayoutPlan: the
// deterministic mapping from stable logical identity to key-dependent physical
// dispatch storage position, with one ABI-compatible padding target per keyed
// domain.
//
// The design in one paragraph (§2.2, §2.5, §5.3): every logical callable
// belongs to exactly one ABI domain, grouped by (module id, public/private
// visibility, calling mode, exact ABI fingerprint, optional dispatch-domain
// label). Within a domain, callables get stable ordinals; the domain gets a
// deterministic public salt (NOT a key, NOT a verifier — §8.4). Build derives a
// transient target route word K from a BuildKeyView and places each callable at
// physical_slot = domain.physical_base + keyed_dispatch_permute(K, salt,
// version, ordinal, physical_count). Every keyed domain also gets exactly one
// ABI-compatible padding ordinal (§2.5), so a singleton real function forms a
// two-entry permutation. A disabled/unkeyed module uses identity layout
// (slot i -> physical slot i, no padding), preserving existing DispatchTable
// behavior.
//
// Constraints (§3, §7.1, §8.4, §14.6): this header/impl uses NO std::hash, NO
// native struct bytes, NO unordered-container iteration for ordering decisions,
// NO randomness, NO expected-key comparison, and NO direct pointers. Public
// domain salts and route words derive from deterministic canonical byte
// serializations folded through a pinned FNV-1a 64-bit (the same non-std::hash
// family dispatch_abi.cpp / seed_derivation.cpp use). Placement uses the ONE
// Red 1 authoritative helper (keyed_dispatch_permute) so the mathematics cannot
// drift. Malformed manifests and plans are rejected through structured
// ExtensionResult/ExtensionError diagnostics (src/extension_registry.hpp).
//
// Scope (§14.3 Red 3): layout metadata/planning ONLY. No JIT call lowering,
// V6 artifact, hot reload, threads, or the Red 4 runtime resolver. This file
// does not touch dispatch_table.hpp / codegen.* / thin_emit.* / context.hpp /
// module_registry.* — existing dispatch behavior is unchanged, and the identity
// planner reproduces the current slot-i->physical-i invariant.
//
// Dependency direction: module_layout.cpp links into ember_frontend (a
// compile-time/layout concern). It depends on keyed_dispatch.* (core ember,
// the Red 1 helper — reached via ember_frontend -> ember) and dispatch_abi.hpp
// (header-only enum types Visibility/CallingMode from Red 2). No cycle: the
// core lib never depends on ember_frontend.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dispatch_abi.hpp"          // Visibility, CallingMode (Red 2 enum types)
#include "extension_registry.hpp"    // ExtensionResult / ExtensionStatus / ExtensionError
#include "keyed_dispatch.hpp"        // KEYED_DISPATCH_MAX_DOMAIN_SIZE, keyed_dispatch_permute (Red 1)

namespace ember {

// ─── Build-time transient target key view (§5.3) ─────────────────────────
// A borrowed byte span over the transient target key material. The planner
// folds it into a deterministic route word (derive_route_word); it never
// stores the bytes and never compares an expected key. Empty is permitted
// (folds to the FNV-1a offset basis, a non-zero route word) so a degenerate
// key still yields a valid, fully-determined layout.
struct BuildKeyView {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

// ─── A classified callable in a module manifest (§5.3) ───────────────────
// Plain data. `abi_fingerprint` is produced by the canonical ABI classifier
// (dispatch_abi.hpp classify_callable / abi_fingerprint) — NEVER a hand-
// written arbitrary hash. `dispatch_domain` is the optional explicit label
// from a `@dispatch_domain("...")` annotation (§2.6); "" means the default
// (unlabelled) domain for this (module, visibility, calling-mode, ABI) tuple.
struct ModuleCallable {
    std::string name;
    uint32_t logical_slot = 0;
    uint64_t abi_fingerprint = 0;          // canonical classifier output (Red 2)
    Visibility visibility = Visibility::Public;
    CallingMode calling_mode = CallingMode::LegacyContext;
    std::string dispatch_domain;           // optional explicit label; "" = default
};

// ─── Module manifest (the planner's input) ───────────────────────────────
// Plain data. Stands in for the output of the parse + sema + stable-logical-
// slot-assignment build step (§9.2). `module_id` is the stable module
// identity that participates in domain grouping.
struct ModuleManifest {
    std::string module_id;
    std::vector<ModuleCallable> callables;
};

// ─── A physical dispatch-table entry (real callable or padding) ──────────
// Plain data. `is_padding` distinguishes a real callable entry from an
// ABI-compatible padding/trap-stub entry. For a real entry, `logical_slot`
// is the logical slot it serves; for a padding entry, `logical_slot` is
// 0xFFFFFFFFu (sentinel: padding serves no logical callable). `domain_index`
// and `ordinal` locate the entry within its domain's permutation.
struct PhysicalEntry {
    uint32_t physical_slot = 0;            // index in the physical dispatch table
    uint64_t abi_fingerprint = 0;          // matches its domain's fingerprint
    Visibility visibility = Visibility::Public;
    CallingMode calling_mode = CallingMode::LegacyContext;
    std::string dispatch_domain;           // matches its domain's label
    std::string name;                      // real: callable name; padding: empty
    bool is_padding = false;
    uint32_t logical_slot = 0;             // real: served logical slot; padding: 0xFFFFFFFFu
    uint32_t domain_index = 0;             // index into ModuleLayoutPlan::domains
    uint32_t ordinal = 0;                  // ordinal within the domain
};

// ─── Padding target descriptor (§7.3) ────────────────────────────────────
// Plain data describing the one ABI-compatible padding target for a keyed
// domain. The padding target does not compare a key (§7.3); it simply
// occupies one physical ordinal selected by the build permutation and traps
// through Ember's recoverable mechanism (TrapReason::KeyedDispatchPadding,
// wired in a later Red — this struct carries the metadata only).
struct PaddingDescriptor {
    uint32_t domain_index = 0;            // index into ModuleLayoutPlan::domains
    uint32_t ordinal = 0;                  // ordinal within domain (== logical_count)
    uint32_t physical_slot = 0;            // placed physical slot
    uint64_t abi_fingerprint = 0;          // ABI-compatible with its domain
    Visibility visibility = Visibility::Public;
    CallingMode calling_mode = CallingMode::LegacyContext;
    std::string dispatch_domain;           // matches its domain's label
};

inline bool operator==(const PaddingDescriptor& a, const PaddingDescriptor& b) {
    return a.domain_index == b.domain_index && a.ordinal == b.ordinal &&
           a.physical_slot == b.physical_slot &&
           a.abi_fingerprint == b.abi_fingerprint &&
           a.visibility == b.visibility &&
           a.calling_mode == b.calling_mode &&
           a.dispatch_domain == b.dispatch_domain;
}

// ─── A dispatch domain (§2.2, §7) ────────────────────────────────────────
// Plain data. A same-(module, visibility, calling-mode, ABI, label) group of
// callables. `domain_salt` is a public, deterministic per-domain tweak
// (§8.4) — NOT a key, NOT a verifier; it MAY be stored in an artifact
// alongside the physical layout. `strategy_version` is the versioned
// permutation contract (1 = the Red 1 reference affine permutation).
// `physical_base` is the first physical slot; `physical_count` is n =
// logical_count + padding_count (>= 2 for every keyed domain because every
// domain gets exactly one padding ordinal, §2.5). `logical_slots[ordinal]`
// maps an ordinal to its served logical slot; `padding_ordinal` is the
// ordinal occupied by the padding target (== logical_count).
struct DispatchDomain {
    // identity key (grouping axes per §2.6 / §7.2)
    std::string module_id;
    Visibility visibility = Visibility::Public;
    CallingMode calling_mode = CallingMode::LegacyContext;
    uint64_t abi_fingerprint = 0;
    std::string dispatch_domain;           // optional explicit label; "" = default

    // permutation configuration (public, deterministic)
    uint64_t domain_salt = 0;              // deterministic canonical derivation (not a key)
    uint32_t strategy_version = 1;         // versioned permutation contract

    // placement
    uint32_t physical_base = 0;            // first physical slot
    uint32_t physical_count = 0;           // n = logical_count + padding_count
    uint32_t logical_count = 0;            // real callables in this domain
    uint32_t padding_count = 0;            // 1 for keyed domains, 0 for identity
    uint32_t padding_ordinal = 0;          // == logical_count for keyed domains

    // ordinal -> logical slot (size == logical_count, ordered by logical slot)
    std::vector<uint32_t> logical_slots;
};

// ─── A logical -> domain/ordinal route ───────────────────────────────────
// Plain data. Indexed by logical_slot in ModuleLayoutPlan::logical_routes.
struct LogicalRoute {
    uint32_t logical_slot = 0;
    uint32_t domain_index = 0;             // index into ModuleLayoutPlan::domains
    uint32_t ordinal = 0;                  // ordinal within the domain
    uint64_t abi_fingerprint = 0;          // matches the domain's fingerprint
    Visibility visibility = Visibility::Public;
    CallingMode calling_mode = CallingMode::LegacyContext;
    std::string dispatch_domain;           // matches the domain's label
};

// ─── The layout plan (§5.3) ──────────────────────────────────────────────
// Plain data. `logical_routes` is indexed by logical_slot (size ==
// logical_slot_count). `physical_entries` is indexed by physical_slot (size ==
// physical_slot_count) and covers every physical slot exactly once.
// `padding_descriptors` carries one complete ABI-compatible padding record
// per keyed domain (in domain order, size == domains.size() for a keyed plan,
// empty for an identity plan); each is validated against its domain and the
// physical entry at its slot. `build_physical_placement[logical_slot]` is the build-time physical slot for
// that real callable (§5.3: "exists only during build/load planning ... does
// not contain the key"). `keyed` is true for the keyed planner, false for the
// identity planner.
struct ModuleLayoutPlan {
    std::string module_id;
    bool keyed = false;
    uint32_t logical_slot_count = 0;
    uint32_t physical_slot_count = 0;
    std::vector<DispatchDomain> domains;
    std::vector<LogicalRoute> logical_routes;          // indexed by logical_slot
    std::vector<PhysicalEntry> physical_entries;       // indexed by physical_slot
    std::vector<PaddingDescriptor> padding_descriptors; // one per keyed domain (indexed in domain order)
    std::vector<uint32_t> build_physical_placement;    // [logical_slot] -> physical_slot
};

// ─── Layout strategy concept (§5.3 ModuleLayoutConcept) ──────────────────
// A versioned, configured layout strategy. `plan` derives a plan from a
// manifest + transient target key; `validate` checks structural invariants
// (the planner calls validate internally before returning a plan, and tests
// call it directly on malformed plans).
class ModuleLayoutConcept {
public:
    virtual ~ModuleLayoutConcept() = default;
    virtual ExtensionResult<ModuleLayoutPlan> plan(const ModuleManifest& manifest,
                                                   BuildKeyView target_key) const = 0;
    virtual ExtensionStatus validate(const ModuleLayoutPlan& plan) const = 0;
    virtual std::string name() const = 0;
};

// ─── implicit-keyed-v1 layout planner ────────────────────────────────────
// The keyed layout planner. Groups callables into domains, derives
// deterministic public salts, assigns stable ordinals and contiguous bases,
// adds exactly one ABI-tagged padding ordinal per domain, uses the Red 1
// helper for placement, and validates total routes + exactly-once complete
// physical coverage before returning a plan.
class ImplicitKeyedLayoutV1 : public ModuleLayoutConcept {
public:
    ExtensionResult<ModuleLayoutPlan> plan(const ModuleManifest& manifest,
                                           BuildKeyView target_key) const override;
    ExtensionStatus validate(const ModuleLayoutPlan& plan) const override;
    std::string name() const override;
};

// ─── Identity layout planner (disabled / unkeyed mode) ───────────────────
// Logical slot i -> physical slot i, physical_count == logical_count, no
// padding, no keyed domains. Preserves existing DispatchTable behavior.
class IdentityLayout : public ModuleLayoutConcept {
public:
    ExtensionResult<ModuleLayoutPlan> plan(const ModuleManifest& manifest,
                                           BuildKeyView target_key) const override;
    ExtensionStatus validate(const ModuleLayoutPlan& plan) const override;
    std::string name() const override;
};

// Free-function form for identity layout (convenience; matches IdentityLayout).
ExtensionResult<ModuleLayoutPlan> plan_identity_layout(const ModuleManifest& manifest);

// ─── Red 4: runtime resolver + immutable module dispatch record (§10, §5.5,
//   §14.3) ────────────────────────────────────────────────────────────────
// The GREEN side of the runtime-resolver contract. An IMMUTABLE, host-published
// record describing a module's keyed dispatch topology at runtime, plus a
// stable C-ABI resolver that validates a logical identity and resolves it to
// a physical dispatch entry through the same versioned permutation the build
// placed it with (keyed_dispatch_permute_runtime — the ONE Red 1 helper).
//
// The record (§10.1) is the runtime shape of the §10.4 atomic immutable
// publication: build/validate the complete generation privately, then publish
// one coherent record. It carries separate logical and physical counts
// (§2.3, §9.7), the physical atomic slot storage pointer, the logical routes,
// the domains, the logical allowlist, and the strategy version. It carries NO
// expected key, machine fingerprint, key digest, verifier, or predecoded
// key-specific permutation (§3.3, §11.3, §14.6) — the route word is transient,
// supplied per resolution, and only participates in the routing arithmetic.
//
// The resolver implements the §2.4 wrong-key safety contract:
//   1. resolution terminates in bounded work (one permute evaluation);
//   2. the resulting physical index lies inside the callable's domain;
//   3. the selected entry shares the domain's canonical ABI fingerprint,
//      visibility, calling mode, and dispatch-domain label (these are
//      properties of the domain, so any in-domain entry — including a padding
//      entry — matches them by construction);
//   4. the entry is non-null finalized RX code (the storage rejects null at
//      publication, §10.4; the resolver acquire-loads and returns null on a
//      null/unfinalized entry);
//   5. malformed metadata resolves to a structured failure (C++ wrapper) or
//      null (C-ABI) BEFORE any physical-slot read — no OOB read;
//   6. NO expected-key comparison occurs (§3.3).
//
// A wrong route word may route a logical callable to a DIFFERENT same-domain
// physical entry (alternate selection) or to the domain's padding ordinal (a
// non-null same-ABI trap stub that fires TrapReason::KeyedDispatchPadding,
// §7.3). Not every wrong key traps; this is implicit keyed control-flow, not
// authentication (§1, §3.3). Every selected destination nevertheless remains
// inside the chosen domain and matches its ABI fingerprint/visibility/calling-
// mode/domain label, NOT merely inside the global physical table range (§14.2
// regression bucket).

// Dispatch mode of a module's dispatch record (§9.7, §11.3). Identity mode is
// the legacy/unkeyed path (logical slot == physical index, no permutation,
// no padding); Keyed mode permutes logical ordinals into physical slots.
enum class DispatchMode : uint8_t {
    Identity = 0,   // legacy/unkeyed: physical_slots[logical_slot] is the entry
    Keyed    = 1,   // keyed: P(route_word, domain, ordinal) selects the entry
};

// The immutable module dispatch record (§10.1). A borrowed, read-only view
// over host-owned storage. Lifetime: the host owns the physical slot storage,
// the routes, the domains, and the allowlist on the ModuleInstance (§10.3) and
// guarantees they outlive every record that references them. The record itself
// is plain data so it can be published through one atomic pointer swap (§10.2:
// std::atomic<const ModuleDispatchRecord*>).
//
// Field set (§10.1 + the task's "sufficient per-entry metadata for strict
// validation"): mode, strategy_version, physical_slots, physical_slot_count,
// logical_slot_count, logical_routes, domains, domain_count, logical_allowlist,
// logical_allowlist_bytes. The LogicalRoute and DispatchDomain types (defined
// above) carry the per-entry/per-domain metadata (ABI fingerprint, visibility,
// calling mode, dispatch-domain label, salt, strategy_version, base, counts,
// logical_slots) the strict whole-record validator and the resolver both use.
//
// The record contains NO field for: an expected target/runtime key, a machine
// fingerprint, a key digest/hash, a verifier/comparison constant, or a
// predecoded key-specific permutation map (§3.3, §11.3, §14.6). The route word
// is NEVER stored here — it is a per-resolution transient supplied to the
// resolver. The physical slot order itself is the build-time key's influence
// and is necessarily observable (§3.3); the record exposes that order, not the
// key.
struct ModuleDispatchRecord {
    DispatchMode mode = DispatchMode::Identity;
    uint32_t strategy_version = 1;          // versioned permutation contract
    const std::atomic<void*>* physical_slots = nullptr;   // physical slot storage (borrowed)
    uint32_t physical_slot_count = 0;       // keyed dispatch storage size (§9.7)
    uint32_t logical_slot_count = 0;        // stable logical identity range (§2.3)
    const LogicalRoute* logical_routes = nullptr;         // indexed by logical_slot (borrowed)
    const DispatchDomain* domains = nullptr;              // domain descriptors (borrowed)
    uint32_t domain_count = 0;
    // Logical allowlist bitset: bit i set iff logical slot i is a registered,
    // callable entry (the function-ref REDSHELL guard #6 shape, extended to
    // keyed mode — §4.6, §9.6 validate the logical handle with the allowlist
    // FIRST). logical_allowlist_bytes = ceil(logical_slot_count / 8). A null
    // allowlist with a nonzero logical_slot_count is malformed (rejected by
    // validation); an empty (0-count) module has a null/0-byte allowlist.
    const uint8_t* logical_allowlist = nullptr;
    uint32_t logical_allowlist_bytes = 0;
};

// Strict whole-record validation BEFORE publication (§10.4, §14.2 should-fail
// bucket). Rejects a malformed record through a structured ExtensionStatus so a
// bad generation is never published. Checks:
//   - mode is Identity or Keyed; strategy_version is supported;
//   - counts are consistent (physical >= logical for keyed; physical == logical
//     for identity with no domains);
//   - every logical route is in range, dense, and matches its domain's ABI
//     fingerprint, visibility, calling mode, and dispatch-domain label;
//   - every route's domain_index is in range and its ordinal is a real (<
//     logical_count) ordinal that the domain assigns to its logical slot;
//   - domains do not overlap and cover the physical range exactly for keyed;
//   - the allowlist is non-null when logical_slot_count > 0 and has the right
//     byte count, and every set bit indexes a present route;
//   - no expected-key / fingerprint / digest / verifier field exists (the
//     record's field set is the documented one — this is a structural shape
//     check, not a value check).
// Identity mode (§10.1 legacy path): a record with mode == Identity and zero
// domains is accepted if counts match and physical_slots is non-null (or the
// module is empty); the identity resolver indexes physical_slots[logical_slot]
// directly.
ExtensionStatus validate_dispatch_record(const ModuleDispatchRecord& rec) noexcept;

// Structured C++ resolver wrapper (for diagnostics). Validates the logical
// identity against the record, resolves through keyed_dispatch_permute_runtime
// (keyed mode) or the identity index (identity mode), adds the domain base
// safely, and acquire-loads a non-null entry. Returns the entry pointer on
// success or a structured ExtensionError on failure. NEVER performs an OOB
// read: a malformed record, an out-of-range logical slot, a cleared allowlist
// bit, a permute failure, an OOB base+ordinal, or a null/unfinalized entry all
// return a structured failure BEFORE any physical-slot read past the validated
// index.
//
// The route word is a transient, locally-derived value (§6.3); it is NEVER
// stored in the record and NEVER compared to an expected value. The resolver
// only feeds it to the permutation arithmetic (§3.3).
ExtensionResult<void*> resolve_keyed_dispatch(
    const ModuleDispatchRecord* rec, uint32_t logical_slot,
    uint64_t transient_route_word) noexcept;

// ─── Stable C-ABI reference resolver (§5.5 prototype path) ───────────────
// The planned C-ABI contract:
//   extern "C" void* ember_resolve_keyed_dispatch(const ModuleDispatchRecord*,
//                                                  uint32_t logical_slot,
//                                                  uint64_t transient_route_word);
//
// Returns a non-null finalized entry pointer on success, or nullptr on any
// failure (malformed record, OOB logical slot, cleared allowlist bit, permute
// failure, OOB physical index, or a null/unfinalized entry) — with NO out-of-
// bounds read. The C++ wrapper `resolve_keyed_dispatch` carries the structured
// diagnostic; this C helper is the no-exceptions, host-callable form that the
// generated resolver helper path and the §11.5 name resolver use.
extern "C" void* ember_resolve_keyed_dispatch(const ModuleDispatchRecord* rec,
                                              uint32_t logical_slot,
                                              uint64_t transient_route_word) noexcept;

// ─── Deterministic derivations (exposed for oracle cross-checking) ───────
// Fold a BuildKeyView into a deterministic route word (pinned FNV-1a 64-bit
// over the key bytes; NOT std::hash). Empty input folds to the FNV-1a offset
// basis (a non-zero route word).
uint64_t derive_route_word(BuildKeyView key) noexcept;

// Derive a public, deterministic per-domain salt from the domain identity
// (§8.4). Canonical byte serialization (length-prefixed LE fields, field-by-
// field — NOT a native struct memcpy) folded through pinned FNV-1a 64-bit.
// NOT std::hash, NOT a key, NOT a verifier.
uint64_t derive_domain_salt(const std::string& module_id, Visibility visibility,
                            CallingMode calling_mode, uint64_t abi_fingerprint,
                            const std::string& dispatch_domain,
                            uint32_t strategy_version) noexcept;

// ─── KeyedDispatchRegistry: configured immutable strategy factories ──────
// (§5.2). Holds named ModuleLayoutConcept factories. add_factory rejects
// empty names, null factories, and duplicate names WITHOUT replacement.
// create returns a FRESH configured instance each call. list_names returns
// names in deterministic (sorted) order. All errors are structured
// ExtensionError diagnostics.
class KeyedDispatchRegistry {
public:
    using StrategyFactory = std::function<std::unique_ptr<ModuleLayoutConcept>()>;

    // Register a configured factory under `name`. Rejects:
    //   - empty name          -> ExtensionError
    //   - null factory        -> ExtensionError
    //   - duplicate name      -> ExtensionError (WITHOUT replacing the existing
    //                            factory — "duplicates without replacement")
    ExtensionStatus add_factory(const std::string& name, StrategyFactory factory);

    // Create a fresh configured instance for `name`. Unknown name -> structured
    // ExtensionError. Each call returns a distinct instance.
    ExtensionResult<std::unique_ptr<ModuleLayoutConcept>> create(const std::string& name) const;

    // Names in deterministic (sorted) order. Not insertion/unordered order.
    std::vector<std::string> list_names() const;

    // Whether a name is registered.
    bool has(const std::string& name) const;

private:
    // std::map is ordered by key (NOT std::unordered_map), so iteration is
    // deterministic and list_names is sorted by construction.
    std::map<std::string, StrategyFactory> factories_;
};

// Register the implicit-keyed-v1 strategy package into `registry`. Returns the
// registration status (structured diagnostic on failure).
ExtensionStatus register_implicit_keyed_v1(KeyedDispatchRegistry& registry);

} // namespace ember
