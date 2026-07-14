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
