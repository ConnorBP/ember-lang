// module_layout.cpp — Red 3 GREEN: logical/physical module layout planning.
//
// Implementation of the planner/concept/registry declared in module_layout.hpp.
// Uses ONLY deterministic, implementation-independent operations: pinned
// FNV-1a 64-bit (NOT std::hash), canonical little-endian byte serialization
// (NOT native struct bytes), std::map ordering (NOT unordered iteration), and
// the ONE Red 1 authoritative permutation helper (keyed_dispatch_permute) for
// placement. No randomness, no expected-key comparison, no direct pointers.
//
// Design ref: docs/planning/plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §2, §5, §7, §9.2, §14.3.

#include "module_layout.hpp"
#include "context.hpp"          // context_t, TrapReason (padding trap stub)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <tuple>

namespace ember {

namespace {

// ─── Pinned FNV-1a 64-bit (the non-std::hash family dispatch_abi.cpp /
// seed_derivation.cpp use) ────────────────────────────────────────────────
inline constexpr uint64_t kFnv1aOffset = 0xcbf29ce484222325ULL;
inline constexpr uint64_t kFnv1aPrime  = 0x100000001b3ULL;

inline uint64_t fnv1a64(const uint8_t* data, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnv1aPrime;
    }
    return h;
}

inline void put_u32_le(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)));
}
inline void put_u64_le(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)));
}
inline void put_lpref_str(std::string& out, const std::string& s) {
    put_u32_le(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

// ─── Structured-error helpers ────────────────────────────────────────────
inline constexpr const char* kRegistry = "ember-keyed-dispatch";

inline ExtensionError err(std::string name, std::string message) {
    return ExtensionError{std::string(kRegistry), std::move(name), std::move(message)};
}

inline constexpr uint32_t kStrategyVersion = 1;

// A sentinel logical_slot value for padding entries (serves no callable).
inline constexpr uint32_t kPaddingLogicalSlot = 0xFFFFFFFFu;

// ─── Domain identity key (ordered tuple for std::map grouping) ───────────
// Ordering is by (module_id, visibility, calling_mode, abi_fingerprint,
// dispatch_domain). std::map iterates this in deterministic order; we then
// re-sort domains by min logical slot for stable base assignment (intuitive
// physical layout). Neither step uses std::hash or unordered iteration.
using DomainKey = std::tuple<std::string, uint8_t, uint8_t, uint64_t, std::string>;

DomainKey make_domain_key(const std::string& module_id, Visibility vis,
                          CallingMode mode, uint64_t fp, const std::string& label) {
    return std::make_tuple(module_id,
                           static_cast<uint8_t>(vis),
                           static_cast<uint8_t>(mode),
                           fp,
                           label);
}

// Builder state for one domain while grouping callables.
struct DomainBuilder {
    DomainKey key;
    std::vector<uint32_t> logical_slots;   // in encounter order (sorted later)
};

} // namespace

// ─── Build-key view validation ─────────────────────────────────────────
// Reject a malformed BuildKeyView BEFORE derive_route_word dereferences it.
// A null pointer with a NON-zero size is a structured error (the fold would
// read past a null base); a null pointer with size 0 is the degenerate empty
// key (folds to the FNV-1a offset basis, a valid non-zero route word); a non-
// null pointer with any size (including 0) is valid. This guard is the gate;
// derive_route_word is additionally defensive so a direct caller can never
// crash, but the structured rejection lives here at the plan entry point.
static ExtensionStatus validate_build_key_view(BuildKeyView key) {
    if (key.bytes == nullptr && key.size != 0) {
        return ExtensionStatus{err("",
            "BuildKeyView{nullptr, nonzero_size} is invalid (null bytes with size " +
            std::to_string(key.size) + ")")};
    }
    return ExtensionStatus{};
}

// ─── Deterministic derivations ───────────────────────────────────────────
uint64_t derive_route_word(BuildKeyView key) noexcept {
    // Defensive: a null base is treated as an empty span so a direct caller
    // can never dereference null. The structured rejection of
    // {nullptr, nonzero_size} happens in plan() via validate_build_key_view.
    if (key.bytes == nullptr) return kFnv1aOffset;
    return fnv1a64(key.bytes, key.size, kFnv1aOffset);
}

uint64_t derive_domain_salt(const std::string& module_id, Visibility visibility,
                            CallingMode calling_mode, uint64_t abi_fingerprint,
                            const std::string& dispatch_domain,
                            uint32_t strategy_version) noexcept {
    // Canonical byte serialization — field-by-field little-endian, length-
    // prefixed strings. NOT a native struct memcpy; nothing about a C++ object
    // layout participates.
    std::string b;
    b.reserve(module_id.size() + dispatch_domain.size() + 32);
    put_lpref_str(b, module_id);
    b.push_back(static_cast<char>(static_cast<uint8_t>(visibility)));
    b.push_back(static_cast<char>(static_cast<uint8_t>(calling_mode)));
    put_u64_le(b, abi_fingerprint);
    put_lpref_str(b, dispatch_domain);
    put_u32_le(b, strategy_version);
    return fnv1a64(reinterpret_cast<const uint8_t*>(b.data()), b.size(), kFnv1aOffset);
}

// ─── Manifest validation (rejects malformed inputs before any layout) ────
static ExtensionStatus validate_manifest(const ModuleManifest& m) {
    const uint32_t n = static_cast<uint32_t>(m.callables.size());
    // Logical slots must be exactly {0, 1, ..., n-1} (dense, no duplicates).
    std::set<uint32_t> seen;
    for (const auto& c : m.callables) {
        if (c.logical_slot >= n) {
            return ExtensionStatus{err(/*name=*/"",
                "manifest logical slot " + std::to_string(c.logical_slot) +
                " out of dense range [0," + std::to_string(n) + ")")};
        }
        auto [it, ok] = seen.insert(c.logical_slot);
        if (!ok) {
            return ExtensionStatus{err("",
                "duplicate logical slot " + std::to_string(c.logical_slot) +
                " in manifest")};
        }
    }
    if (static_cast<uint32_t>(seen.size()) != n || (!seen.empty() && *seen.begin() != 0)) {
        return ExtensionStatus{err("",
            "manifest logical slots are not a dense set starting at 0")};
    }
    return ExtensionStatus{};
}

ExtensionResult<ModuleLayoutPlan> ImplicitKeyedLayoutV1::plan(
    const ModuleManifest& manifest, BuildKeyView target_key) const {

    if (auto s = validate_manifest(manifest); !s) {
        return ExtensionResult<ModuleLayoutPlan>{std::move(s.error.value())};
    }
    // Reject a malformed BuildKeyView BEFORE derive_route_word dereferences it
    // (e.g. BuildKeyView{nullptr, nonzero_size} -> structured error, not a
    // null-pointer read).
    if (auto s = validate_build_key_view(target_key); !s) {
        return ExtensionResult<ModuleLayoutPlan>{std::move(s.error.value())};
    }

    const uint64_t route_word = derive_route_word(target_key);

    ModuleLayoutPlan p;
    p.module_id = manifest.module_id;
    p.keyed = true;
    p.logical_slot_count = static_cast<uint32_t>(manifest.callables.size());

    // Empty module -> a keyed layout has no dispatch domains and is not a
    // valid keyed plan (strict negative: an empty keyed module is rejected,
    // not silently turned into an empty plan). The identity planner accepts
    // an empty module (an empty identity table is valid — see IdentityLayout).
    if (p.logical_slot_count == 0) {
        return ExtensionResult<ModuleLayoutPlan>{err("",
            "keyed layout requires at least one callable (empty module)")};
    }

    // Group callables into domains by the identity key. Iterate callables in
    // logical-slot order (deterministic). std::map keeps a deterministic order.
    std::map<DomainKey, DomainBuilder> groups;
    for (const auto& c : manifest.callables) {
        DomainKey k = make_domain_key(manifest.module_id, c.visibility,
                                      c.calling_mode, c.abi_fingerprint,
                                      c.dispatch_domain);
        auto& b = groups[k];
        b.key = k;
        b.logical_slots.push_back(c.logical_slot);
    }

    // Materialize domains, sorted by min logical slot for stable base
    // assignment (intuitive, deterministic physical ordering — independent of
    // the std::map's key order).
    struct DomTmp {
        DispatchDomain d;
        uint32_t min_slot;
    };
    std::vector<DomTmp> tmp;
    tmp.reserve(groups.size());
    for (auto& [k, b] : groups) {
        std::sort(b.logical_slots.begin(), b.logical_slots.end());
        DispatchDomain d;
        d.module_id = manifest.module_id;
        d.visibility = static_cast<Visibility>(std::get<1>(k));
        d.calling_mode = static_cast<CallingMode>(std::get<2>(k));
        d.abi_fingerprint = std::get<3>(k);
        d.dispatch_domain = std::get<4>(k);
        d.strategy_version = kStrategyVersion;
        d.domain_salt = derive_domain_salt(d.module_id, d.visibility, d.calling_mode,
                                           d.abi_fingerprint, d.dispatch_domain,
                                           d.strategy_version);
        d.logical_count = static_cast<uint32_t>(b.logical_slots.size());
        d.padding_count = 1;                          // exactly one padding per keyed domain (§2.5)
        d.physical_count = d.logical_count + d.padding_count;
        d.padding_ordinal = d.logical_count;          // padding occupies the last ordinal
        d.logical_slots = b.logical_slots;            // copy (b still owns the sorted vector)
        uint32_t min_slot = b.logical_slots.empty() ? 0u : b.logical_slots.front();
        tmp.push_back({std::move(d), min_slot});
    }
    std::sort(tmp.begin(), tmp.end(),
              [](const DomTmp& a, const DomTmp& b){ return a.min_slot < b.min_slot; });

    // Validate domain sizes BEFORE assigning bases: physical_count must be in
    // [2, KEYED_DISPATCH_MAX_DOMAIN_SIZE]. A domain with logical_count L has
    // physical_count L+1, so L must be <= MAX-1.
    uint32_t base = 0;
    p.domains.reserve(tmp.size());
    for (auto& t : tmp) {
        if (t.d.physical_count < 2 ||
            t.d.physical_count > KEYED_DISPATCH_MAX_DOMAIN_SIZE) {
            return ExtensionResult<ModuleLayoutPlan>{err("",
                "domain physical_count " + std::to_string(t.d.physical_count) +
                " out of range [2," + std::to_string(KEYED_DISPATCH_MAX_DOMAIN_SIZE) +
                "] for ABI fingerprint 0x" +
                std::to_string(t.d.abi_fingerprint) + ")")};
        }
        t.d.physical_base = base;
        base += t.d.physical_count;
        p.domains.push_back(std::move(t.d));
    }
    p.physical_slot_count = base;

    // Build logical routes (indexed by logical_slot) and per-domain ordinal map.
    p.logical_routes.assign(p.logical_slot_count, LogicalRoute{});
    for (uint32_t di = 0; di < p.domains.size(); ++di) {
        auto& d = p.domains[di];
        for (uint32_t o = 0; o < d.logical_count; ++o) {
            uint32_t slot = d.logical_slots[o];
            LogicalRoute r;
            r.logical_slot = slot;
            r.domain_index = di;
            r.ordinal = o;
            r.abi_fingerprint = d.abi_fingerprint;
            r.visibility = d.visibility;
            r.calling_mode = d.calling_mode;
            r.dispatch_domain = d.dispatch_domain;
            p.logical_routes[slot] = r;
        }
    }

    // Place every ordinal (real + padding) via the Red 1 helper and build the
    // physical table. The permutation is a bijection on [0, physical_count),
    // so the real ordinals [0, logical_count) and the padding ordinal
    // (== logical_count) together cover every physical slot in the domain.
    // Index the manifest callables by logical_slot so a dense-but-shuffled
    // manifest still attaches the correct callable name to its physical entry.
    std::vector<std::string> name_by_slot(p.logical_slot_count);
    for (const auto& c : manifest.callables) {
        if (c.logical_slot < p.logical_slot_count) name_by_slot[c.logical_slot] = c.name;
    }
    p.physical_entries.assign(p.physical_slot_count, PhysicalEntry{});
    std::vector<bool> placed(p.physical_slot_count, false);
    p.build_physical_placement.assign(p.logical_slot_count, 0);
    for (uint32_t di = 0; di < p.domains.size(); ++di) {
        auto& d = p.domains[di];
        for (uint32_t o = 0; o < d.physical_count; ++o) {
            KeyedDispatchDomain kd{d.domain_salt, d.strategy_version, d.physical_count};
            auto pr = keyed_dispatch_permute(route_word, kd, o);
            if (!pr) {
                return ExtensionResult<ModuleLayoutPlan>{err("",
                    "keyed_dispatch_permute failed for domain " + std::to_string(di) +
                    " ordinal " + std::to_string(o))};
            }
            uint32_t slot = d.physical_base + *pr.value;
            if (slot >= p.physical_slot_count || placed[slot]) {
                return ExtensionResult<ModuleLayoutPlan>{err("",
                    "internal placement collision/OOB at physical slot " +
                    std::to_string(slot))};
            }
            placed[slot] = true;
            PhysicalEntry e;
            e.physical_slot = slot;
            e.abi_fingerprint = d.abi_fingerprint;
            e.visibility = d.visibility;
            e.calling_mode = d.calling_mode;
            e.dispatch_domain = d.dispatch_domain;
            e.domain_index = di;
            e.ordinal = o;
            if (o < d.logical_count) {
                e.is_padding = false;
                e.logical_slot = d.logical_slots[o];
                e.name = name_by_slot[e.logical_slot];
                p.build_physical_placement[e.logical_slot] = slot;
            } else {
                e.is_padding = true;
                e.logical_slot = kPaddingLogicalSlot;
                e.name.clear();
            }
            p.physical_entries[slot] = e;
        }
    }

    // Build one complete PaddingDescriptor per keyed domain (in domain order).
    p.padding_descriptors.clear();
    p.padding_descriptors.reserve(p.domains.size());
    for (uint32_t di = 0; di < p.domains.size(); ++di) {
        const auto& d = p.domains[di];
        PaddingDescriptor pd;
        pd.domain_index = di;
        pd.ordinal = d.padding_ordinal;
        pd.abi_fingerprint = d.abi_fingerprint;
        pd.visibility = d.visibility;
        pd.calling_mode = d.calling_mode;
        pd.dispatch_domain = d.dispatch_domain;
        // The padding physical slot: base + permute(padding_ordinal).
        KeyedDispatchDomain kd{d.domain_salt, d.strategy_version, d.physical_count};
        auto pr = keyed_dispatch_permute(route_word, kd, d.padding_ordinal);
        if (!pr) {
            return ExtensionResult<ModuleLayoutPlan>{err("",
                "keyed_dispatch_permute failed for padding in domain " + std::to_string(di))};
        }
        pd.physical_slot = d.physical_base + *pr.value;
        p.padding_descriptors.push_back(std::move(pd));
    }

    // Validate before returning (total routes + exactly-once complete physical
    // coverage + all structural invariants).
    if (auto v = validate(p); !v) {
        return ExtensionResult<ModuleLayoutPlan>{std::move(v.error.value())};
    }
    return ExtensionResult<ModuleLayoutPlan>{p};
}

ExtensionStatus ImplicitKeyedLayoutV1::validate(const ModuleLayoutPlan& p) const {
    // A keyed plan must be flagged keyed: an identity plan (keyed == false)
    // handed to the keyed validator is a malformed plan, not silently accepted.
    if (!p.keyed) {
        return ExtensionStatus{err("", "keyed plan must have keyed == true")};
    }
    // logical_routes count == logical_slot_count.
    if (p.logical_routes.size() != p.logical_slot_count) {
        return ExtensionStatus{err("", "logical_routes size != logical_slot_count")};
    }
    // build_physical_placement count == logical_slot_count.
    if (p.build_physical_placement.size() != p.logical_slot_count) {
        return ExtensionStatus{err("", "build_physical_placement size != logical_slot_count")};
    }
    // physical_entries count == physical_slot_count.
    if (p.physical_entries.size() != p.physical_slot_count) {
        return ExtensionStatus{err("", "physical_entries size != physical_slot_count")};
    }

    // Dense logical routes: route[i].logical_slot == i, no dup, no OOB,
    // and each route's metadata matches its domain identity (ABI fingerprint,
    // visibility, calling mode, dispatch-domain label). A mutated route that
    // disagrees with its domain is a malformed plan.
    {
        std::set<uint32_t> seen;
        for (uint32_t i = 0; i < p.logical_routes.size(); ++i) {
            const auto& r = p.logical_routes[i];
            if (r.logical_slot != i) {
                return ExtensionStatus{err("", "logical_routes[" + std::to_string(i) +
                    "].logical_slot != " + std::to_string(i))};
            }
            auto [it, ok] = seen.insert(r.logical_slot);
            if (!ok) {
                return ExtensionStatus{err("", "duplicate logical route slot " +
                    std::to_string(r.logical_slot))};
            }
            if (r.domain_index >= p.domains.size()) {
                return ExtensionStatus{err("", "route domain_index OOB")};
            }
            const auto& d = p.domains[r.domain_index];
            if (r.abi_fingerprint != d.abi_fingerprint) {
                return ExtensionStatus{err("", "route ABI fingerprint != domain ABI fingerprint for slot " +
                    std::to_string(i))};
            }
            if (r.visibility != d.visibility) {
                return ExtensionStatus{err("", "route visibility != domain visibility for slot " +
                    std::to_string(i))};
            }
            if (r.calling_mode != d.calling_mode) {
                return ExtensionStatus{err("", "route calling_mode != domain calling_mode for slot " +
                    std::to_string(i))};
            }
            if (r.dispatch_domain != d.dispatch_domain) {
                return ExtensionStatus{err("", "route dispatch_domain != domain dispatch_domain for slot " +
                    std::to_string(i))};
            }
            // The route's ordinal must be a real (non-padding) ordinal.
            if (r.ordinal >= d.logical_count) {
                return ExtensionStatus{err("", "route ordinal >= domain logical_count for slot " +
                    std::to_string(i))};
            }
            // The route's ordinal must index the domain's logical_slots to
            // exactly this route's logical slot: d.logical_slots[r.ordinal] ==
            // r.logical_slot. A plan where the ordinal<->logical-slot mapping
            // disagrees with the domain's logical_slots vector is malformed
            // (deterministic ordering / stable identity violation). Guard the
            // index against logical_slots.size() (caught earlier as a domain
            // invariant, but re-checked here for defense in depth).
            if (r.ordinal >= d.logical_slots.size() ||
                d.logical_slots[r.ordinal] != r.logical_slot) {
                return ExtensionStatus{err("", "route ordinal/logical_slots mismatch: d.logical_slots[" +
                    std::to_string(r.ordinal) + "] != route logical_slot " +
                    std::to_string(r.logical_slot) + " for slot " + std::to_string(i))};
            }
        }
    }

    // Domain invariants + base/size sanity.
    {
        std::set<uint32_t> covered;
        uint32_t domain_phys_total = 0;
        for (uint32_t di = 0; di < p.domains.size(); ++di) {
            const auto& d = p.domains[di];
            // Every domain's module_id must match the plan's module_id.
            if (d.module_id != p.module_id) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " module_id != plan module_id")};
            }
            if (d.padding_count != 1) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " padding_count != 1 (must be exactly one per keyed domain)")};
            }
            if (d.physical_count != d.logical_count + d.padding_count) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " physical_count != logical_count + padding_count")};
            }
            if (d.logical_slots.size() != d.logical_count) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " logical_slots size != logical_count")};
            }
            if (d.padding_ordinal != d.logical_count) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " padding_ordinal != logical_count")};
            }
            if (d.physical_count < 2 ||
                d.physical_count > KEYED_DISPATCH_MAX_DOMAIN_SIZE) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " physical_count out of range [2, MAX]")};
            }
            // Domain physical range must be in-bounds and non-overlapping.
            if (d.physical_base >= p.physical_slot_count &&
                p.physical_slot_count > 0) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " physical_base OOB")};
            }
            if (uint64_t(d.physical_base) + uint64_t(d.physical_count) >
                uint64_t(p.physical_slot_count)) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " physical range exceeds physical_slot_count")};
            }
            for (uint32_t s = d.physical_base; s < d.physical_base + d.physical_count; ++s) {
                auto [it, ok] = covered.insert(s);
                if (!ok) {
                    return ExtensionStatus{err("", "overlapping physical domain range at slot " +
                        std::to_string(s))};
                }
            }
            domain_phys_total += d.physical_count;
        }
        if (domain_phys_total != p.physical_slot_count) {
            return ExtensionStatus{err("", "sum of domain physical_counts != physical_slot_count")};
        }
    }

    // Exactly-once complete physical coverage: every entry indexed by slot,
    // inside its domain, matching domain metadata, no padding-only domain.
    {
        std::set<uint32_t> seen_slots;
        for (uint32_t i = 0; i < p.physical_entries.size(); ++i) {
            const auto& e = p.physical_entries[i];
            if (e.physical_slot != i) {
                return ExtensionStatus{err("", "physical_entries[" + std::to_string(i) +
                    "].physical_slot != index")};
            }
            if (e.domain_index >= p.domains.size()) {
                return ExtensionStatus{err("", "physical entry domain_index OOB")};
            }
            const auto& d = p.domains[e.domain_index];
            if (e.physical_slot < d.physical_base ||
                e.physical_slot >= d.physical_base + d.physical_count) {
                return ExtensionStatus{err("", "physical entry outside its domain range")};
            }
            // Cross-ABI / cross-visibility / cross-mode / cross-label membership.
            if (e.abi_fingerprint != d.abi_fingerprint) {
                return ExtensionStatus{err("", "cross-ABI membership: entry fingerprint != domain fingerprint")};
            }
            if (e.visibility != d.visibility) {
                return ExtensionStatus{err("", "cross-visibility membership: entry visibility != domain visibility")};
            }
            if (e.calling_mode != d.calling_mode) {
                return ExtensionStatus{err("", "cross-calling-mode membership")};
            }
            if (e.dispatch_domain != d.dispatch_domain) {
                return ExtensionStatus{err("", "cross-label membership: entry label != domain label")};
            }
            auto [it, ok] = seen_slots.insert(e.physical_slot);
            if (!ok) {
                return ExtensionStatus{err("", "duplicate physical slot " +
                    std::to_string(e.physical_slot))};
            }
            // A padding-only domain (logical_count 0) is invalid: every keyed
            // domain must have at least one real callable.
            if (d.logical_count == 0) {
                return ExtensionStatus{err("", "padding-only domain (logical_count 0)")};
            }
            // Entry ordinal <-> domain consistency. A REAL entry at ordinal o
            // must serve the logical slot the domain assigns to that ordinal:
            // d.logical_slots[e.ordinal] == e.logical_slot, and e.ordinal must
            // be a real (< logical_count) ordinal. A PADDING entry must sit at
            // the padding ordinal (e.ordinal == d.padding_ordinal) and carry
            // the padding logical-slot sentinel. A plan where an entry's
            // ordinal disagrees with the domain's logical_slots / padding_ordinal
            // is malformed (deterministic ordering violation).
            if (e.is_padding) {
                if (e.ordinal != d.padding_ordinal) {
                    return ExtensionStatus{err("", "padding entry ordinal != domain padding_ordinal for slot " +
                        std::to_string(i))};
                }
                if (e.logical_slot != kPaddingLogicalSlot) {
                    return ExtensionStatus{err("", "padding entry logical_slot != padding sentinel for slot " +
                        std::to_string(i))};
                }
            } else {
                if (e.ordinal >= d.logical_count) {
                    return ExtensionStatus{err("", "real entry ordinal >= domain logical_count for slot " +
                        std::to_string(i))};
                }
                if (e.ordinal >= d.logical_slots.size() ||
                    d.logical_slots[e.ordinal] != e.logical_slot) {
                    return ExtensionStatus{err("", "entry ordinal/logical_slots mismatch: d.logical_slots[" +
                        std::to_string(e.ordinal) + "] != entry logical_slot " +
                        std::to_string(e.logical_slot) + " for slot " + std::to_string(i))};
                }
            }
        }
        if (seen_slots.size() != p.physical_slot_count) {
            return ExtensionStatus{err("", "physical coverage gap: covered != physical_slot_count")};
        }
    }

    // Each keyed domain has exactly one padding entry; padding metadata matches.
    {
        for (uint32_t di = 0; di < p.domains.size(); ++di) {
            const auto& d = p.domains[di];
            uint32_t pad_count = 0;
            for (const auto& e : p.physical_entries) {
                if (e.domain_index == di && e.is_padding) {
                    ++pad_count;
                    if (e.ordinal != d.padding_ordinal) {
                        return ExtensionStatus{err("", "padding ordinal mismatch in domain " +
                            std::to_string(di))};
                    }
                }
            }
            if (pad_count != d.padding_count) {
                return ExtensionStatus{err("", "domain " + std::to_string(di) +
                    " has " + std::to_string(pad_count) + " padding entries (expected " +
                    std::to_string(d.padding_count) + ")")};
            }
        }
    }

    // Padding descriptors: one per keyed domain, in domain order, each
    // matching its domain identity and the placed padding entry at its slot.
    {
        if (p.padding_descriptors.size() != p.domains.size()) {
            return ExtensionStatus{err("", "padding_descriptors size != domain count")};
        }
        for (uint32_t di = 0; di < p.domains.size(); ++di) {
            const auto& d = p.domains[di];
            const auto& pd = p.padding_descriptors[di];
            if (pd.domain_index != di) {
                return ExtensionStatus{err("", "padding descriptor domain_index mismatch for domain " +
                    std::to_string(di))};
            }
            if (pd.ordinal != d.padding_ordinal) {
                return ExtensionStatus{err("", "padding descriptor ordinal mismatch for domain " +
                    std::to_string(di))};
            }
            if (pd.abi_fingerprint != d.abi_fingerprint ||
                pd.visibility != d.visibility ||
                pd.calling_mode != d.calling_mode ||
                pd.dispatch_domain != d.dispatch_domain) {
                return ExtensionStatus{err("", "padding descriptor metadata != domain identity for domain " +
                    std::to_string(di))};
            }
            if (pd.physical_slot < d.physical_base ||
                pd.physical_slot >= d.physical_base + d.physical_count) {
                return ExtensionStatus{err("", "padding descriptor physical_slot outside domain range for domain " +
                    std::to_string(di))};
            }
            if (pd.physical_slot >= p.physical_entries.size()) {
                return ExtensionStatus{err("", "padding descriptor physical_slot OOB for domain " +
                    std::to_string(di))};
            }
            const auto& e = p.physical_entries[pd.physical_slot];
            if (!e.is_padding || e.domain_index != di || e.ordinal != pd.ordinal) {
                return ExtensionStatus{err("", "padding descriptor physical_slot != placed padding entry for domain " +
                    std::to_string(di))};
            }
        }
    }

    // build_physical_placement: in-bounds and consistent with routes/entries.
    {
        std::set<uint32_t> placed;
        for (uint32_t s = 0; s < p.logical_slot_count; ++s) {
            uint32_t phys = p.build_physical_placement[s];
            if (phys >= p.physical_slot_count) {
                return ExtensionStatus{err("", "OOB build placement for logical slot " +
                    std::to_string(s))};
            }
            const auto& e = p.physical_entries[phys];
            if (e.is_padding || e.logical_slot != s) {
                return ExtensionStatus{err("", "build placement for logical slot " +
                    std::to_string(s) + " does not serve that slot")};
            }
            const auto& r = p.logical_routes[s];
            if (e.domain_index != r.domain_index || e.ordinal != r.ordinal) {
                return ExtensionStatus{err("", "build placement domain/ordinal mismatch for slot " +
                    std::to_string(s))};
            }
            // Placements must be distinct (one physical slot per real callable).
            auto [it, ok] = placed.insert(phys);
            if (!ok) {
                return ExtensionStatus{err("", "duplicate build placement at physical slot " +
                    std::to_string(phys))};
            }
        }
    }

    return ExtensionStatus{};
}

std::string ImplicitKeyedLayoutV1::name() const { return "implicit-keyed-v1"; }

// ─── Identity layout planner (disabled / unkeyed mode) ───────────────────
ExtensionResult<ModuleLayoutPlan> IdentityLayout::plan(
    const ModuleManifest& manifest, BuildKeyView /*target_key*/) const {

    if (auto s = validate_manifest(manifest); !s) {
        return ExtensionResult<ModuleLayoutPlan>{std::move(s.error.value())};
    }

    ModuleLayoutPlan p;
    p.module_id = manifest.module_id;
    p.keyed = false;
    p.logical_slot_count = static_cast<uint32_t>(manifest.callables.size());
    p.physical_slot_count = p.logical_slot_count;   // no padding
    p.logical_routes.assign(p.logical_slot_count, LogicalRoute{});
    p.physical_entries.assign(p.physical_slot_count, PhysicalEntry{});
    p.build_physical_placement.assign(p.logical_slot_count, 0);
    // Index callables by logical_slot (NOT vector position): a dense-but-shuffled
    // manifest must attach the correct callable metadata to physical slot i.
    std::vector<const ModuleCallable*> by_slot(p.logical_slot_count, nullptr);
    for (const auto& c : manifest.callables) {
        if (c.logical_slot < p.logical_slot_count) by_slot[c.logical_slot] = &c;
    }
    for (uint32_t i = 0; i < p.logical_slot_count; ++i) {
        const ModuleCallable* c = by_slot[i];
        if (!c) {
            return ExtensionResult<ModuleLayoutPlan>{err("",
                "identity layout: no callable for logical slot " + std::to_string(i))};
        }
        // Identity route (domain_index 0 sentinel unused; no keyed domains).
        LogicalRoute r;
        r.logical_slot = i;
        r.domain_index = 0;
        r.ordinal = i;
        r.abi_fingerprint = c->abi_fingerprint;
        r.visibility = c->visibility;
        r.calling_mode = c->calling_mode;
        r.dispatch_domain = c->dispatch_domain;
        p.logical_routes[i] = r;
        // Physical entry: slot i serves logical slot i, no padding.
        PhysicalEntry e;
        e.physical_slot = i;
        e.abi_fingerprint = c->abi_fingerprint;
        e.visibility = c->visibility;
        e.calling_mode = c->calling_mode;
        e.dispatch_domain = c->dispatch_domain;
        e.name = c->name;
        e.is_padding = false;
        e.logical_slot = i;
        e.domain_index = 0;
        e.ordinal = i;
        p.physical_entries[i] = e;
        p.build_physical_placement[i] = i;
    }
    return ExtensionResult<ModuleLayoutPlan>{p};
}

ExtensionStatus IdentityLayout::validate(const ModuleLayoutPlan& p) const {
    // Identity invariants: counts match, slot i -> physical i, no padding,
    // no keyed domains.
    if (p.keyed) {
        return ExtensionStatus{err("", "identity plan must not be keyed")};
    }
    if (p.physical_slot_count != p.logical_slot_count) {
        return ExtensionStatus{err("", "identity plan: physical_slot_count != logical_slot_count")};
    }
    if (p.logical_routes.size() != p.logical_slot_count) {
        return ExtensionStatus{err("", "identity plan: logical_routes size mismatch")};
    }
    if (p.physical_entries.size() != p.physical_slot_count) {
        return ExtensionStatus{err("", "identity plan: physical_entries size mismatch")};
    }
    if (p.build_physical_placement.size() != p.logical_slot_count) {
        return ExtensionStatus{err("", "identity plan: build_physical_placement size mismatch")};
    }
    if (!p.domains.empty()) {
        return ExtensionStatus{err("", "identity plan must have no keyed domains")};
    }
    for (uint32_t i = 0; i < p.logical_slot_count; ++i) {
        if (p.build_physical_placement[i] != i) {
            return ExtensionStatus{err("", "identity plan: build_physical_placement[i] != i")};
        }
        if (p.logical_routes[i].logical_slot != i) {
            return ExtensionStatus{err("", "identity plan: route[i].logical_slot != i")};
        }
        if (p.physical_entries[i].physical_slot != i || p.physical_entries[i].is_padding ||
            p.physical_entries[i].logical_slot != i) {
            return ExtensionStatus{err("", "identity plan: physical entry[i] not identity (slot i, real, serves i)")};
        }
    }
    return ExtensionStatus{};
}

std::string IdentityLayout::name() const { return "identity-layout"; }

ExtensionResult<ModuleLayoutPlan> plan_identity_layout(const ModuleManifest& manifest) {
    IdentityLayout id;
    return id.plan(manifest, BuildKeyView{});
}

// ─── KeyedDispatchRegistry ───────────────────────────────────────────────
ExtensionStatus KeyedDispatchRegistry::add_factory(const std::string& name,
                                                   StrategyFactory factory) {
    if (name.empty()) {
        return ExtensionStatus{err("", "empty strategy name")};
    }
    if (!factory) {   // std::function with no callable target
        return ExtensionStatus{err(name, "null strategy factory")};
    }
    auto [it, ok] = factories_.emplace(name, std::move(factory));
    if (!ok) {
        // Duplicate — reject WITHOUT replacing the existing factory.
        return ExtensionStatus{err(name, "duplicate strategy name (not replaced)")};
    }
    return ExtensionStatus{};
}

ExtensionResult<std::unique_ptr<ModuleLayoutConcept>>
KeyedDispatchRegistry::create(const std::string& name) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        return ExtensionResult<std::unique_ptr<ModuleLayoutConcept>>{
            err(name, "unknown strategy name")};
    }
    // Fresh configured instance each call.
    std::unique_ptr<ModuleLayoutConcept> inst = it->second();
    if (!inst) {
        return ExtensionResult<std::unique_ptr<ModuleLayoutConcept>>{
            err(name, "factory returned null instance")};
    }
    return ExtensionResult<std::unique_ptr<ModuleLayoutConcept>>{std::move(inst)};
}

std::vector<std::string> KeyedDispatchRegistry::list_names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    // std::map iteration is ordered by key -> deterministic sorted output.
    for (const auto& [n, f] : factories_) names.push_back(n);
    return names;
}

bool KeyedDispatchRegistry::has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

ExtensionStatus register_implicit_keyed_v1(KeyedDispatchRegistry& registry) {
    return registry.add_factory("implicit-keyed-v1", []{
        return std::unique_ptr<ModuleLayoutConcept>(
            std::make_unique<ImplicitKeyedLayoutV1>());
    });
}

// ============================================================================
// Red 4: runtime resolver + immutable module dispatch record (§10, §5.5,
// §14.3). The GREEN side of the runtime-resolver contract.
//
// validate_dispatch_record — strict whole-record validation BEFORE publication
// (§10.4). resolve_keyed_dispatch — structured C++ resolver wrapper.
// ember_resolve_keyed_dispatch — the stable C-ABI reference helper (§5.5).
//
// The resolver delegates to the ONE Red 1 authoritative runtime wrapper
// keyed_dispatch_permute_runtime (the mathematics cannot drift between build
// placement and runtime navigation). It validates the logical identity, adds
// the domain base safely with checked arithmetic, acquire-loads a non-null
// entry, and returns a structured failure / nullptr BEFORE any out-of-bounds
// physical-slot read. No expected-key comparison ever occurs (§3.3).
// ============================================================================

// Internal: is allowlist bit i set? Defensive against a null/short allowlist.
static bool allowlist_bit_set(const uint8_t* allowlist, uint32_t bytes,
                              uint32_t logical_slot) noexcept {
    if (!allowlist || bytes == 0) return false;
    uint32_t byte_idx = logical_slot >> 3;
    if (byte_idx >= bytes) return false;
    uint32_t bit_idx = logical_slot & 7u;
    return (allowlist[byte_idx] & (uint8_t(1) << bit_idx)) != 0;
}

ExtensionStatus validate_dispatch_record(const ModuleDispatchRecord& rec) noexcept {
    // Mode must be a supported value.
    if (rec.mode != DispatchMode::Identity && rec.mode != DispatchMode::Keyed) {
        return ExtensionStatus{err("", "dispatch record: unsupported mode")};
    }
    // Strategy version 1 is the only currently-supported version. Identity
    // mode does not permute but still carries the field for shape uniformity.
    if (rec.strategy_version != 1) {
        return ExtensionStatus{err("", "dispatch record: unsupported strategy version")};
    }

    // An empty module (logical_slot_count 0) is valid only in Identity mode
    // (an empty identity table is valid — preserves existing DispatchTable
    // behavior). A keyed empty module has no dispatch domains and is rejected
    // (mirroring the Red 3 keyed planner's strict empty-module rejection).
    if (rec.logical_slot_count == 0) {
        if (rec.mode == DispatchMode::Keyed) {
            return ExtensionStatus{err("", "keyed dispatch record: empty module (logical_slot_count 0)")};
        }
        // Identity empty: physical_slot_count must also be 0 and no routes/domains.
        if (rec.physical_slot_count != 0) {
            return ExtensionStatus{err("", "identity empty record: physical_slot_count != 0")};
        }
        if (rec.domain_count != 0) {
            return ExtensionStatus{err("", "identity empty record: domain_count != 0")};
        }
        // An empty allowlist (null, 0 bytes) is the canonical empty shape.
        return ExtensionStatus{};
    }

    // Non-empty module: the allowlist must be non-null and large enough to
    // cover every logical slot.
    const uint32_t need_allowlist_bytes = (rec.logical_slot_count + 7u) >> 3;
    if (rec.logical_allowlist == nullptr) {
        return ExtensionStatus{err("", "dispatch record: null allowlist with nonzero logical_slot_count")};
    }
    if (rec.logical_allowlist_bytes < need_allowlist_bytes) {
        return ExtensionStatus{err("", "dispatch record: allowlist too short for logical_slot_count")};
    }

    // Routes pointer must be non-null and cover every logical slot.
    if (rec.logical_routes == nullptr) {
        return ExtensionStatus{err("", "dispatch record: null logical_routes")};
    }
    // Physical storage must be non-null for a non-empty module.
    if (rec.physical_slots == nullptr) {
        return ExtensionStatus{err("", "dispatch record: null physical_slots")};
    }

    if (rec.mode == DispatchMode::Identity) {
        // Identity invariants (§10.1 legacy path): physical == logical, no
        // keyed domains, the resolver indexes physical_slots[logical_slot]
        // directly. A domain_count > 0 in identity mode is malformed (keyed
        // domains are not part of the identity record shape).
        if (rec.physical_slot_count != rec.logical_slot_count) {
            return ExtensionStatus{err("", "identity record: physical_slot_count != logical_slot_count")};
        }
        if (rec.domain_count != 0) {
            return ExtensionStatus{err("", "identity record: domain_count != 0 (no keyed domains)")};
        }
        // Routes must be dense (route[i].logical_slot == i). No domain to
        // cross-check (domain_count 0), but the route's domain_index is
        // unused in identity mode — we do not require it to be 0 here; the
        // identity resolver ignores it.
        for (uint32_t i = 0; i < rec.logical_slot_count; ++i) {
            if (rec.logical_routes[i].logical_slot != i) {
                return ExtensionStatus{err("", "identity record: route[" + std::to_string(i) +
                    "].logical_slot != " + std::to_string(i))};
            }
            if (!allowlist_bit_set(rec.logical_allowlist, rec.logical_allowlist_bytes, i)) {
                return ExtensionStatus{err("", "identity record: allowlist bit clear for slot " +
                    std::to_string(i))};
            }
        }
        // Every identity physical slot must be non-null (§10.4 / §14.2).
        for (uint32_t s = 0; s < rec.physical_slot_count; ++s) {
            if (rec.physical_slots[s].load(std::memory_order_acquire) == nullptr) {
                return ExtensionStatus{err("", "identity record: null/unfinalized entry at slot " +
                    std::to_string(s))};
            }
        }
        // Physical descriptors are OPTIONAL in identity mode (the identity
        // resolver needs no per-entry metadata to validate the permutation),
        // but if present they must be sized == physical_slot_count and each
        // be a real (non-padding) descriptor serving its own slot.
        if (rec.physical_descriptors != nullptr) {
            if (rec.physical_descriptor_count != rec.physical_slot_count) {
                return ExtensionStatus{err("", "identity record: physical_descriptor_count != physical_slot_count")};
            }
            for (uint32_t s = 0; s < rec.physical_slot_count; ++s) {
                const auto& pd = rec.physical_descriptors[s];
                if (pd.physical_slot != s || pd.is_padding ||
                    pd.logical_slot != s) {
                    return ExtensionStatus{err("", "identity record: descriptor[" + std::to_string(s) +
                        "] not identity (slot s, real, serves s)")};
                }
            }
        }
        return ExtensionStatus{};
    }

    // ---- Keyed mode ----
    if (rec.domain_count == 0) {
        return ExtensionStatus{err("", "keyed record: domain_count == 0")};
    }
    if (rec.domains == nullptr) {
        return ExtensionStatus{err("", "keyed record: null domains")};
    }
    if (rec.physical_slot_count < rec.logical_slot_count) {
        return ExtensionStatus{err("", "keyed record: physical_slot_count < logical_slot_count")};
    }

    // Domain range/identity invariants + non-overlapping coverage.
    {
        std::set<uint32_t> covered;
        uint32_t domain_phys_total = 0;
        for (uint32_t di = 0; di < rec.domain_count; ++di) {
            const auto& d = rec.domains[di];
            if (d.padding_count != 1) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " padding_count != 1")};
            }
            if (d.physical_count != d.logical_count + d.padding_count) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " physical_count != logical_count + padding_count")};
            }
            if (d.physical_count < 2 || d.physical_count > KEYED_DISPATCH_MAX_DOMAIN_SIZE) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " physical_count out of range [2, MAX]")};
            }
            if (d.strategy_version != rec.strategy_version) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " strategy_version != record strategy_version")};
            }
            if (d.logical_slots.size() != d.logical_count) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " logical_slots size != logical_count")};
            }
            if (d.padding_ordinal != d.logical_count) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " padding_ordinal != logical_count")};
            }
            // Domain physical range must be in-bounds and non-overlapping.
            if (uint64_t(d.physical_base) + uint64_t(d.physical_count) >
                uint64_t(rec.physical_slot_count)) {
                return ExtensionStatus{err("", "keyed record: domain " + std::to_string(di) +
                    " physical range exceeds physical_slot_count")};
            }
            for (uint32_t s = d.physical_base; s < d.physical_base + d.physical_count; ++s) {
                auto [it, ok] = covered.insert(s);
                if (!ok) {
                    return ExtensionStatus{err("", "keyed record: overlapping physical domain range at slot " +
                        std::to_string(s))};
                }
            }
            domain_phys_total += d.physical_count;
        }
        if (domain_phys_total != rec.physical_slot_count) {
            return ExtensionStatus{err("", "keyed record: sum of domain physical_counts != physical_slot_count")};
        }
    }

    // Dense logical routes + route/domain metadata consistency + allowlist.
    {
        std::set<uint32_t> seen_slots;
        for (uint32_t i = 0; i < rec.logical_slot_count; ++i) {
            const auto& r = rec.logical_routes[i];
            if (r.logical_slot != i) {
                return ExtensionStatus{err("", "keyed record: route[" + std::to_string(i) +
                    "].logical_slot != " + std::to_string(i))};
            }
            auto [it, ok] = seen_slots.insert(r.logical_slot);
            if (!ok) {
                return ExtensionStatus{err("", "keyed record: duplicate logical route slot " +
                    std::to_string(r.logical_slot))};
            }
            if (r.domain_index >= rec.domain_count) {
                return ExtensionStatus{err("", "keyed record: route domain_index OOB for slot " +
                    std::to_string(i))};
            }
            const auto& d = rec.domains[r.domain_index];
            // Route metadata must match its domain identity (ABI fingerprint,
            // visibility, calling mode, dispatch-domain label). A mutated
            // route that disagrees with its domain is a malformed record.
            if (r.abi_fingerprint != d.abi_fingerprint) {
                return ExtensionStatus{err("", "keyed record: route ABI fingerprint != domain for slot " +
                    std::to_string(i))};
            }
            if (r.visibility != d.visibility) {
                return ExtensionStatus{err("", "keyed record: route visibility != domain for slot " +
                    std::to_string(i))};
            }
            if (r.calling_mode != d.calling_mode) {
                return ExtensionStatus{err("", "keyed record: route calling_mode != domain for slot " +
                    std::to_string(i))};
            }
            if (r.dispatch_domain != d.dispatch_domain) {
                return ExtensionStatus{err("", "keyed record: route dispatch_domain != domain for slot " +
                    std::to_string(i))};
            }
            // The route's ordinal must be a real (non-padding) ordinal, and the
            // domain must assign that ordinal to this logical slot.
            if (r.ordinal >= d.logical_count) {
                return ExtensionStatus{err("", "keyed record: route ordinal >= domain logical_count for slot " +
                    std::to_string(i))};
            }
            if (r.ordinal >= d.logical_slots.size() ||
                d.logical_slots[r.ordinal] != r.logical_slot) {
                return ExtensionStatus{err("", "keyed record: route ordinal/logical_slots mismatch for slot " +
                    std::to_string(i))};
            }
            // The allowlist bit for every present route must be set (a route
            // without an allowlist bit is a publication-inconsistent record).
            if (!allowlist_bit_set(rec.logical_allowlist, rec.logical_allowlist_bytes, i)) {
                return ExtensionStatus{err("", "keyed record: allowlist bit clear for present route slot " +
                    std::to_string(i))};
            }
        }
    }

    // ---- Physical-entry descriptors (Red 4, §10.1, §10.4, §14.2) ----
    // The descriptor table must be present and sized == physical_slot_count so
    // the validator can verify each stored target matches its domain's ABI
    // fingerprint/visibility/calling mode/dispatch-domain label and distinguish
    // a REAL callable from a PADDING/trap-stub entry. The test consults the
    // RUNTIME record's descriptors, not the build-time ModuleLayoutPlan.
    if (rec.physical_descriptors == nullptr) {
        return ExtensionStatus{err("", "keyed record: null physical_descriptors")};
    }
    if (rec.physical_descriptor_count != rec.physical_slot_count) {
        return ExtensionStatus{err("", "keyed record: physical_descriptor_count != physical_slot_count")};
    }
    const void* pad_target = rec.padding_trap_target;  // may be null
    {
        std::set<uint32_t> seen_desc_slots;
        for (uint32_t s = 0; s < rec.physical_slot_count; ++s) {
            const auto& pd = rec.physical_descriptors[s];
            if (pd.physical_slot != s) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "].physical_slot != index")};
            }
            auto [it, ok] = seen_desc_slots.insert(pd.physical_slot);
            if (!ok) {
                return ExtensionStatus{err("", "keyed record: duplicate descriptor physical slot " +
                    std::to_string(pd.physical_slot))};
            }
            if (pd.domain_index >= rec.domain_count) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] domain_index OOB")};
            }
            const auto& d = rec.domains[pd.domain_index];
            // Descriptor must lie inside its domain's physical range.
            if (pd.physical_slot < d.physical_base ||
                pd.physical_slot >= d.physical_base + d.physical_count) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] outside its domain range")};
            }
            // Cross-ABI / cross-visibility / cross-mode / cross-label membership:
            // each stored target's metadata must match its domain's identity.
            if (pd.abi_fingerprint != d.abi_fingerprint) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] ABI fingerprint != domain")};
            }
            if (pd.visibility != d.visibility) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] visibility != domain")};
            }
            if (pd.calling_mode != d.calling_mode) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] calling_mode != domain")};
            }
            if (pd.dispatch_domain != d.dispatch_domain) {
                return ExtensionStatus{err("", "keyed record: descriptor[" + std::to_string(s) +
                    "] dispatch_domain != domain")};
            }
            // Real vs padding consistency against the domain.
            if (pd.is_padding) {
                if (pd.ordinal != d.padding_ordinal) {
                    return ExtensionStatus{err("", "keyed record: padding descriptor[" + std::to_string(s) +
                        "] ordinal != domain padding_ordinal")};
                }
                if (pd.logical_slot != kPaddingLogicalSlotRuntime) {
                    return ExtensionStatus{err("", "keyed record: padding descriptor[" + std::to_string(s) +
                        "] logical_slot != padding sentinel")};
                }
            } else {
                if (pd.ordinal >= d.logical_count) {
                    return ExtensionStatus{err("", "keyed record: real descriptor[" + std::to_string(s) +
                        "] ordinal >= domain logical_count")};
                }
                if (pd.ordinal >= d.logical_slots.size() ||
                    d.logical_slots[pd.ordinal] != pd.logical_slot) {
                    return ExtensionStatus{err("", "keyed record: real descriptor[" + std::to_string(s) +
                        "] ordinal/logical_slots mismatch")};
                }
            }
            // Cross-check the stored entry pointer against the padding-trap
            // identity, when declared. A padding descriptor's entry must equal
            // the padding target; a real descriptor's entry must NOT.
            if (pad_target != nullptr) {
                void* entry = rec.physical_slots[s].load(std::memory_order_acquire);
                if (pd.is_padding) {
                    if (entry != const_cast<void*>(pad_target)) {
                        return ExtensionStatus{err("", "keyed record: padding descriptor[" + std::to_string(s) +
                            "] entry != padding_trap_target")};
                    }
                } else {
                    if (entry == const_cast<void*>(pad_target)) {
                        return ExtensionStatus{err("", "keyed record: real descriptor[" + std::to_string(s) +
                            "] entry == padding_trap_target (real entry must not be the padding stub)")};
                    }
                }
            }
        }
        if (seen_desc_slots.size() != rec.physical_slot_count) {
            return ExtensionStatus{err("", "keyed record: physical descriptor coverage gap")};
        }
    }

    // ---- Null / unfinalized physical entries (§10.4 / §14.2 should-fail) ----
    // Every physical slot must acquire-load a NON-NULL finalized entry. A null
    // slot is a publication-invariant violation: strict publication rejects a
    // null entry with no slot mutated, so a published record must be complete.
    // This is the runtime half of the strict-publication coupling.
    for (uint32_t s = 0; s < rec.physical_slot_count; ++s) {
        void* entry = rec.physical_slots[s].load(std::memory_order_acquire);
        if (entry == nullptr) {
            return ExtensionStatus{err("", "keyed record: null/unfinalized entry at physical slot " +
                std::to_string(s))};
        }
    }

    return ExtensionStatus{};
}

// Internal: the core resolution, shared by the C++ wrapper and the C-ABI
// helper. Validates the logical identity, resolves through the permutation
// (keyed) or the identity index, adds the domain base safely, and acquire-
// loads a non-null entry. On failure sets `out_err` and returns false WITHOUT
// any out-of-bounds physical-slot read. On success returns true and sets
// `out_entry` to a non-null entry pointer.
static bool resolve_core(const ModuleDispatchRecord* rec, uint32_t logical_slot,
                         uint64_t transient_route_word, void*& out_entry,
                         ExtensionError& out_err) noexcept {
    out_entry = nullptr;
    if (!rec) {
        out_err = err("", "resolve: null dispatch record");
        return false;
    }
    // Logical identity range check BEFORE any array read.
    if (logical_slot >= rec->logical_slot_count) {
        out_err = err("", "resolve: logical_slot >= logical_slot_count");
        return false;
    }
    // Validate the logical handle with the allowlist FIRST (§9.6).
    if (!allowlist_bit_set(rec->logical_allowlist, rec->logical_allowlist_bytes,
                           logical_slot)) {
        out_err = err("", "resolve: logical slot not in allowlist");
        return false;
    }
    // Routes must be present (a validated record guarantees this; check
    // defensively so a malformed record never crashes the resolver).
    if (!rec->logical_routes) {
        out_err = err("", "resolve: null logical_routes");
        return false;
    }
    if (!rec->physical_slots) {
        out_err = err("", "resolve: null physical_slots");
        return false;
    }
    const LogicalRoute& r = rec->logical_routes[logical_slot];

    // Strengthened resolver guards (Red 4 feedback): validate the route's
    // logical identity and, for keyed mode, its metadata against its domain
    // BEFORE any permutation/physical read, so a malformed record resolves to
    // a structured failure instead of a wrong entry. A validated record passes
    // these trivially; the guards exist so an unvalidated/corrupted record
    // never silently resolves.
    if (r.logical_slot != logical_slot) {
        out_err = err("", "resolve: route logical_slot != requested logical_slot");
        return false;
    }

    uint32_t physical_index = 0;
    if (rec->mode == DispatchMode::Identity) {
        // Legacy path: physical_slots[logical_slot] directly. No permutation,
        // no padding. Preserve existing DispatchTable behavior (§4.4).
        physical_index = logical_slot;
        if (physical_index >= rec->physical_slot_count) {
            out_err = err("", "resolve: identity physical index OOB");
            return false;
        }
    } else if (rec->mode == DispatchMode::Keyed) {
        // Keyed path: validate the route's domain, then resolve through the
        // ONE Red 1 runtime permutation helper.
        if (r.domain_index >= rec->domain_count) {
            out_err = err("", "resolve: route domain_index OOB");
            return false;
        }
        if (!rec->domains) {
            out_err = err("", "resolve: null domains");
            return false;
        }
        const DispatchDomain& d = rec->domains[r.domain_index];
        // Route metadata must match its domain identity (ABI fingerprint,
        // visibility, calling mode, dispatch-domain label). A malformed record
        // whose route disagrees with its domain resolves to a structured
        // failure, never a wrong entry.
        if (r.abi_fingerprint != d.abi_fingerprint) {
            out_err = err("", "resolve: route ABI fingerprint != domain");
            return false;
        }
        if (r.visibility != d.visibility) {
            out_err = err("", "resolve: route visibility != domain");
            return false;
        }
        if (r.calling_mode != d.calling_mode) {
            out_err = err("", "resolve: route calling_mode != domain");
            return false;
        }
        if (r.dispatch_domain != d.dispatch_domain) {
            out_err = err("", "resolve: route dispatch_domain != domain");
            return false;
        }
        // The route's ordinal must be a real (non-padding) ordinal of the
        // domain, and the domain must assign that ordinal to this logical
        // slot. A validated record guarantees this; check defensively so a
        // malformed record never resolves to a wrong entry.
        if (r.ordinal >= d.logical_count) {
            out_err = err("", "resolve: route ordinal >= domain logical_count");
            return false;
        }
        if (r.ordinal >= d.logical_slots.size() ||
            d.logical_slots[r.ordinal] != r.logical_slot) {
            out_err = err("", "resolve: route ordinal/logical_slots mismatch");
            return false;
        }
        // The domain must publish the same strategy version as the record.
        if (d.strategy_version != rec->strategy_version) {
            out_err = err("", "resolve: domain strategy_version != record");
            return false;
        }
        KeyedDispatchDomain kd{d.domain_salt, d.strategy_version, d.physical_count};
        auto pr = keyed_dispatch_permute_runtime(transient_route_word, kd, r.ordinal);
        if (!pr) {
            out_err = err("", "resolve: keyed_dispatch_permute_runtime failed");
            return false;
        }
        const uint32_t local = *pr.value;
        if (local >= d.physical_count) {
            // Defensive — the Red 1 helper guarantees [0, n) for a valid
            // domain; this never fires for a validated record but guards a
            // corrupted domain.
            out_err = err("", "resolve: permute result out of domain range");
            return false;
        }
        // Add the domain base SAFELY with checked 64-bit arithmetic so a
        // corrupted domain base + local can never overflow into an OOB read.
        const uint64_t slot64 = uint64_t(d.physical_base) + uint64_t(local);
        if (slot64 >= uint64_t(rec->physical_slot_count)) {
            out_err = err("", "resolve: physical index (base + permute) OOB");
            return false;
        }
        physical_index = uint32_t(slot64);
    } else {
        out_err = err("", "resolve: unsupported dispatch mode");
        return false;
    }

    // Acquire-load the physical entry. A null/unfinalized entry is a structured
    // failure (§2.4 item 4) — the storage rejects null at publication (§10.4),
    // but the resolver is defensive so a half-published or corrupted table
    // yields a clean failure, never a raw null/wild call.
    void* entry = rec->physical_slots[physical_index].load(std::memory_order_acquire);
    if (!entry) {
        out_err = err("", "resolve: physical entry is null/unfinalized");
        return false;
    }
    out_entry = entry;
    return true;
}

ExtensionResult<void*> resolve_keyed_dispatch(
    const ModuleDispatchRecord* rec, uint32_t logical_slot,
    uint64_t transient_route_word) noexcept {
    void* entry = nullptr;
    ExtensionError e{};
    if (resolve_core(rec, logical_slot, transient_route_word, entry, e)) {
        return ExtensionResult<void*>{entry};
    }
    return ExtensionResult<void*>{std::move(e)};
}

// Stable C-ABI reference resolver (§5.5). extern "C" linkage, no exceptions.
// Returns a non-null entry pointer on success or nullptr on any failure, with
// NO out-of-bounds read. Delegates to the same core resolution as the C++
// wrapper. Defined inside namespace ember (the header declaration is already
// extern "C"; extern "C" inside a namespace gives the function C linkage — an
// unmangled symbol name — while keeping it a namespace member for lookup). The
// emitted symbol is `ember_resolve_keyed_dispatch`, the planned §5.5 name.
extern "C" void* ember_resolve_keyed_dispatch(const ModuleDispatchRecord* rec,
                                              uint32_t logical_slot,
                                              uint64_t transient_route_word) noexcept {
    void* entry = nullptr;
    ExtensionError e{};
    if (resolve_core(rec, logical_slot, transient_route_word, entry, e)) {
        return entry;
    }
    return nullptr;
}

// ============================================================================
// Red 4: runtime ABI-compatible padding/trap target (§7.3) + record builder.
// ============================================================================

// The universal padding-trap entry point. Its entry ABI safely ignores every
// incoming argument class (Win64 args live in rcx/rdx/r8/r9/xmm/stack which
// this stub never reads); it reads only the context pointer (passed in r14 by
// the keyed outer thunk, or here directly as the first Win64 GP arg) and
// records TrapReason::KeyedDispatchPadding on it. Returns 0 (a neutral i64
// for any GP-scalar-returning domain). This is a REAL callable same-ABI target:
// a wrong route word that lands on a padding ordinal calls into finalized RX
// code, fires the recoverable trap reason, and NEVER compares a key. There is
// no key parameter and no expected-value comparison anywhere in this stub —
// proving it is a one-line read of the context's last_trap field.
extern "C" int64_t ember_keyed_padding_trap(ember::context_t* ctx) noexcept {
    if (ctx) {
        ctx->last_trap = TrapReason::KeyedDispatchPadding;
        ctx->last_error = "keyed dispatch padding";
    }
    return 0;
}

const void* ember_keyed_padding_trap_target() noexcept {
    return reinterpret_cast<const void*>(&ember_keyed_padding_trap);
}

bool ember_is_padding_trap_target(const void* entry) noexcept {
    if (entry == nullptr) return false;
    return entry == ember_keyed_padding_trap_target();
}

// Build a runtime record view over host-owned storage from a Red 3 plan. Fills
// st's vectors (storage/routes/domains/descriptors/allowlist) from the plan and
// returns a ModuleDispatchRecord borrowing them. Real physical slots are
// populated via real_entry(physical_slot); padding slots are filled with the
// padding-trap stub. The record's padding_trap_target is set so the validator
// can cross-check padding entries against the stub identity. The record NEVER
// stores the route word.
ExtensionResult<ModuleDispatchRecord> build_module_dispatch_record(
    RecordBuilderStorage& st,
    const ModuleLayoutPlan& plan,
    const std::function<void*(uint32_t physical_slot)>& real_entry) noexcept {
    if (!plan.keyed) {
        return ExtensionResult<ModuleDispatchRecord>{err("",
            "build_module_dispatch_record: plan is not keyed")};
    }
    if (plan.logical_slot_count == 0) {
        return ExtensionResult<ModuleDispatchRecord>{err("",
            "build_module_dispatch_record: empty keyed plan")};
    }
    if (!real_entry) {
        return ExtensionResult<ModuleDispatchRecord>{err("",
            "build_module_dispatch_record: null real_entry callback")};
    }

    st.routes = plan.logical_routes;
    st.domains = plan.domains;
    st.allowlist.assign((plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < plan.logical_slot_count; ++i)
        st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));

    // Physical descriptors: one per physical slot, transcribed from the plan's
    // build-time PhysicalEntry into the runtime PhysicalEntryDescriptor.
    st.descriptors.assign(plan.physical_slot_count, PhysicalEntryDescriptor{});
    for (uint32_t s = 0; s < plan.physical_slot_count; ++s) {
        const auto& pe = plan.physical_entries[s];
        PhysicalEntryDescriptor& pd = st.descriptors[s];
        pd.physical_slot = pe.physical_slot;
        pd.abi_fingerprint = pe.abi_fingerprint;
        pd.visibility = pe.visibility;
        pd.calling_mode = pe.calling_mode;
        pd.dispatch_domain = pe.dispatch_domain;
        pd.is_padding = pe.is_padding;
        pd.logical_slot = pe.logical_slot;
        pd.domain_index = pe.domain_index;
        pd.ordinal = pe.ordinal;
    }

    // Physical storage: real entries via the callback, padding slots via the
    // padding-trap stub. Reject a null real_entry callback result up front so
    // the storage never holds a null slot.
    st.storage = std::vector<std::atomic<void*>>(plan.physical_slot_count);
    const void* pad_stub = ember_keyed_padding_trap_target();
    for (uint32_t s = 0; s < plan.physical_slot_count; ++s) {
        const auto& pe = plan.physical_entries[s];
        void* entry = nullptr;
        if (pe.is_padding) {
            entry = const_cast<void*>(pad_stub);
        } else {
            entry = real_entry(s);
            if (entry == nullptr) {
                return ExtensionResult<ModuleDispatchRecord>{err("",
                    "build_module_dispatch_record: real_entry returned null for physical slot " +
                    std::to_string(s))};
            }
        }
        st.storage[s].store(entry, std::memory_order_release);
    }

    ModuleDispatchRecord rec{};
    rec.mode = DispatchMode::Keyed;
    rec.strategy_version = 1;
    rec.physical_slots = st.storage.data();
    rec.physical_slot_count = plan.physical_slot_count;
    rec.logical_slot_count = plan.logical_slot_count;
    rec.logical_routes = st.routes.data();
    rec.domains = st.domains.data();
    rec.domain_count = static_cast<uint32_t>(st.domains.size());
    rec.logical_allowlist = st.allowlist.data();
    rec.logical_allowlist_bytes = static_cast<uint32_t>(st.allowlist.size());
    rec.physical_descriptors = st.descriptors.data();
    rec.physical_descriptor_count = static_cast<uint32_t>(st.descriptors.size());
    rec.padding_trap_target = pad_stub;
    return ExtensionResult<ModuleDispatchRecord>{rec};
}

} // namespace ember
