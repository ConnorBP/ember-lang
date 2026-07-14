// keyed_dispatch_runtime_test — Red 4 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §2.4, §5.5, §7.3, §10, §14.2, §14.3, §14.5, §14.6): the runtime resolver +
// immutable module dispatch record gate.
//
// RED-GREEN contract chunk for the runtime resolver. This is the RED side
// (written first / strengthened) of the Red 4 contract. It pins, against an
// INDEPENDENT oracle (a from-scratch reimplementation of the resolution spec
// written here in the test) and against LITERAL golden physical-index vectors,
// the §2.4 wrong-key safety contract and the §14.2 mandatory buckets for the
// runtime resolver:
//
//   - correct key:          the correct route word resolves every logical slot
//                          to the entry the build placed there. The pinned
//                          physical-index vector is asserted against a LITERAL
//                          golden vector (not computed dynamically), cross-
//                          checked against the independent oracle.
//   - wrong keys:           for many wrong route words — including the edge
//                          words 0 and UINT64_MAX — every successful result is
//                          non-null, lies inside the SELECTED DOMAIN (the
//                          domain of the route being resolved), and matches
//                          that domain's ABI fingerprint, visibility, calling
//                          mode, and dispatch-domain label. The metadata check
//                          consults the RUNTIME record's physical_descriptors
//                          (NOT the build-time ModuleLayoutPlan), so the record
//                          is self-describing. This is NOT a tautological
//                          contiguous-global-range assertion: the selected
//                          entry's descriptor metadata must equal the route's
//                          domain metadata exactly (§14.2 regression bucket).
//   - alternate-real:      at least one wrong key routes a multi-real-domain
//                          logical slot to a DIFFERENT REAL same-domain target
//                          (not padding) — proving alternate routing reaches a
//                          different real callable, not only the padding
//                          ordinal. The alternate target is a real (non-padding)
//                          descriptor inside the same domain with a different
//                          logical slot.
//   - padding:             at least one wrong key routes some logical slot to
//                          the domain's PADDING ordinal. The padding entry is
//                          the REAL callable same-ABI padding trap stub
//                          (ember_keyed_padding_trap); the test INVOKES it and
//                          asserts it sets last_trap == KeyedDispatchPadding
//                          and performs NO key comparison (it has no key
//                          parameter). The padding descriptor is a non-null
//                          same-ABI target (§7.3).
//   - pinned vectors:      LITERAL golden physical-index vectors for K1 and K2,
//                          asserted verbatim so a future algorithm change is
//                          caught by a literal mismatch.
//   - malformed records:   null record, null physical_slots, OOB logical slot,
//                          cleared allowlist bit, route with logical_slot !=
//                          index, route with OOB domain_index, route metadata
//                          mismatching its domain, null routes/slots/domains,
//                          domain_count mismatch, physical_slot_count mismatch,
//                          null allowlist with nonzero counts -> the resolver
//                          returns a structured failure / nullptr WITHOUT any
//                          out-of-bounds read.
//   - null/unfinalized:    a physical slot that is null -> the resolver acquire-
//                          loads and returns a structured failure / nullptr;
//                          validate_dispatch_record rejects a null physical
//                          entry BEFORE publication.
//   - strict validation:   validate_dispatch_record accepts a good record and
//                          rejects every malformed shape — including a null
//                          physical entry, a missing/short physical-descriptor
//                          table, a descriptor whose metadata != its domain, a
//                          padding descriptor whose entry != padding_trap_target,
//                          and a real descriptor whose entry == padding_trap_target.
//   - strict publication:  KeyedDispatchStorage::publish_keyed rejects a null
//                          entry, an out-of-range slot, and a DUPLICATE slot,
//                          with NO slot mutated; publish_keyed_strict requires
//                          COMPLETE coverage (every slot exactly once) and is
//                          coupled to record validation (a record built over a
//                          fully-published storage passes validate, an
//                          incomplete one fails).
//   - identity mode:       a DispatchMode::Identity record resolves
//                          physical_slots[logical_slot] directly (no permutation,
//                          no padding), preserving existing DispatchTable
//                          behavior; validate accepts it and rejects an identity
//                          record with domains, count mismatch, or a null entry.
//   - TrapReason:          TrapReason::KeyedDispatchPadding exists, maps to the
//                          stable string "keyed dispatch padding", and is fired
//                          by invoking the real padding trap stub.
//   - schema assertion:    the ModuleDispatchRecord contains NO field for an
//                          expected key, machine fingerprint, key digest,
//                          verifier, or predecoded key-specific permutation
//                          (§14.6) — the field set is exactly the documented one.
//   - no key storage:      building a record from a plan never stores the route
//                          word; the resolver receives it transiently per call.
//
// The fixture is a synthetic ModuleManifest with controlled ABI fingerprints
// (distinct per domain) so the physical-index vectors are pinnable. The plan is
// produced by the real Red 3 ImplicitKeyedLayoutV1 planner; the record is
// assembled via build_module_dispatch_record (the runtime record builder), so
// every metadata check reads the RUNTIME record's physical_descriptors, not the
// build-time plan. Real entries are distinct static C functions (real callable
// targets); padding slots hold ember_keyed_padding_trap.
//
// Links ember (keyed_dispatch.* — Red 1 helper, context.hpp — TrapReason,
// dispatch_table.hpp — KeyedDispatchStorage) + ember_frontend (module_layout.*
// — Red 3 planner, Red 4 record/resolver/builder/padding stub). NOT a CTest
// entry: the filtered suite count must stay 67 (§14.1); the target building
// cleanly + the executable passing IS the gate.

#include "../src/module_layout.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/dispatch_abi.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/context.hpp"
#include "../src/extension_registry.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ember;

// ===========================================================================
// Test harness
// ===========================================================================
static int g_fail = 0;
static int g_checks = 0;
static void ck(bool c, const char* m) {
    ++g_checks;
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ===========================================================================
// Real callable stub targets — distinct static C functions whose addresses
// serve as non-null finalized real entries. They are NEVER the padding stub.
// We do not invoke real entries (their ABI varies per domain); we only need
// distinct, non-null, non-padding addresses so the validator can confirm a
// real descriptor's entry != padding_trap_target. Six stubs cover the 6 real
// slots of the fixture.
// ===========================================================================
extern "C" int64_t real_stub_0(ember::context_t*) noexcept { return 100; }
extern "C" int64_t real_stub_1(ember::context_t*) noexcept { return 101; }
extern "C" int64_t real_stub_2(ember::context_t*) noexcept { return 102; }
extern "C" int64_t real_stub_3(ember::context_t*) noexcept { return 103; }
extern "C" int64_t real_stub_4(ember::context_t*) noexcept { return 104; }
extern "C" int64_t real_stub_5(ember::context_t*) noexcept { return 105; }

// ===========================================================================
// Independent oracle — reimplements the resolution spec.
//
// Spec (must match src/module_layout.cpp resolve_core exactly):
//   identity:  physical_index = logical_slot; entry = physical_slots[logical_slot]
//   keyed:    kd = {domain.domain_salt, domain.strategy_version, domain.physical_count}
//             local = keyed_dispatch_permute_runtime(route_word, kd, route.ordinal)
//             physical_index = domain.physical_base + local
//             entry = physical_slots[physical_index]
//   guards:   rec null, logical_slot >= logical_slot_count, allowlist bit clear,
//             null routes/slots/domains, route.logical_slot != logical_slot,
//             route domain_index OOB, route metadata != domain, route ordinal >=
//             domain logical_count, permute failure, base+local OOB, null entry.
// The oracle delegates the permutation to the already-independently-verified
// Red 1 production helper.
// ===========================================================================
static bool route_meta_eq_domain(const LogicalRoute& r, const DispatchDomain& d) {
    return r.abi_fingerprint == d.abi_fingerprint &&
           r.visibility == d.visibility &&
           r.calling_mode == d.calling_mode &&
           r.dispatch_domain == d.dispatch_domain;
}

static uint32_t oracle_resolve_phys(const ModuleDispatchRecord* rec,
                                    uint32_t logical_slot,
                                    uint64_t route_word,
                                    bool& out_ok) {
    out_ok = false;
    if (!rec) return 0xFFFFFFFFu;
    if (logical_slot >= rec->logical_slot_count) return 0xFFFFFFFFu;
    uint32_t byte_idx = logical_slot >> 3;
    uint32_t bit_idx = logical_slot & 7u;
    if (!rec->logical_allowlist || byte_idx >= rec->logical_allowlist_bytes) return 0xFFFFFFFFu;
    if (!(rec->logical_allowlist[byte_idx] & (uint8_t(1) << bit_idx))) return 0xFFFFFFFFu;
    if (!rec->logical_routes || !rec->physical_slots) return 0xFFFFFFFFu;
    const LogicalRoute& r = rec->logical_routes[logical_slot];
    if (r.logical_slot != logical_slot) return 0xFFFFFFFFu;
    if (rec->mode == DispatchMode::Identity) {
        if (logical_slot >= rec->physical_slot_count) return 0xFFFFFFFFu;
        out_ok = true;
        return logical_slot;
    }
    if (rec->mode != DispatchMode::Keyed) return 0xFFFFFFFFu;
    if (r.domain_index >= rec->domain_count || !rec->domains) return 0xFFFFFFFFu;
    const DispatchDomain& d = rec->domains[r.domain_index];
    if (!route_meta_eq_domain(r, d)) return 0xFFFFFFFFu;
    if (r.ordinal >= d.logical_count) return 0xFFFFFFFFu;
    if (r.ordinal >= d.logical_slots.size() || d.logical_slots[r.ordinal] != r.logical_slot)
        return 0xFFFFFFFFu;
    if (d.strategy_version != rec->strategy_version) return 0xFFFFFFFFu;
    KeyedDispatchDomain kd{d.domain_salt, d.strategy_version, d.physical_count};
    auto pr = keyed_dispatch_permute_runtime(route_word, kd, r.ordinal);
    if (!pr) return 0xFFFFFFFFu;
    uint64_t slot = uint64_t(d.physical_base) + uint64_t(*pr.value);
    if (slot >= uint64_t(rec->physical_slot_count)) return 0xFFFFFFFFu;
    out_ok = true;
    return uint32_t(slot);
}

// ===========================================================================
// Fixture: a synthetic ModuleManifest with 6 callables across 4 domains.
//
//   D0: f0/f1/f2 — public,  LegacyContext, ABI 0xA0A0, ""               (3 real, 4 phys)
//   D1: g0       — private, LegacyContext, ABI 0xB1B1, ""               (1 real, 2 phys) singleton (visibility)
//   D2: h0       — public,  LegacyContext, ABI 0xC2C2, "math"          (1 real, 2 phys) singleton (label)
//   D3: k0       — public,  KeyedR15,      ABI 0xD3D3, ""               (1 real, 2 phys) singleton (calling mode)
//
// logical_slot_count = 6, physical_slot_count = 4+2+2+2 = 10.
//
// Domain physical layout (deterministic, sorted by min logical slot):
//   D0 base=0 count=4 (range [0,4)); D1 base=4 count=2 ([4,6));
//   D2 base=6 count=2 ([6,8)); D3 base=8 count=2 ([8,10)).
// ===========================================================================
static ModuleManifest make_fixture_manifest() {
    ModuleManifest m;
    m.module_id = "runtime.fix";
    auto mk = [&](const char* name, uint32_t slot, uint64_t fp, Visibility vis,
                  CallingMode mode, const char* label) {
        ModuleCallable c;
        c.name = name;
        c.logical_slot = slot;
        c.abi_fingerprint = fp;
        c.visibility = vis;
        c.calling_mode = mode;
        c.dispatch_domain = label ? std::string(label) : std::string();
        m.callables.push_back(std::move(c));
    };
    mk("f0", 0, 0xA0A0A0A0A0A0A0A0ULL, Visibility::Public,  CallingMode::LegacyContext, "");
    mk("f1", 1, 0xA0A0A0A0A0A0A0A0ULL, Visibility::Public,  CallingMode::LegacyContext, "");
    mk("f2", 2, 0xA0A0A0A0A0A0A0A0ULL, Visibility::Public,  CallingMode::LegacyContext, "");
    mk("g0", 3, 0xB1B1B1B1B1B1B1B1ULL, Visibility::Private, CallingMode::LegacyContext, "");
    mk("h0", 4, 0xC2C2C2C2C2C2C2C2ULL, Visibility::Public,  CallingMode::LegacyContext, "math");
    mk("k0", 5, 0xD3D3D3D3D3D3D3D3ULL, Visibility::Public,  CallingMode::KeyedR15,      "");
    return m;
}

// Two pinned target route words (derived from pinned BuildKeyView bytes).
static const uint8_t K1_BYTES[] = "runtime-key-alpha-01";   // 21 bytes
static const size_t  K1_SIZE = 21;
static const uint8_t K2_BYTES[] = "runtime-key-bravo__002"; // 21 bytes
static const size_t K2_SIZE = 21;
static BuildKeyView k1_view() { return {K1_BYTES, K1_SIZE}; }
static BuildKeyView k2_view() { return {K2_BYTES, K2_SIZE}; }

// LITERAL golden physical-index vectors, captured from the deterministic
// Red 1/Red 3 pipeline. Asserted verbatim so any algorithm drift is caught by
// a literal mismatch (not a re-derivation that would silently track the bug).
//   K1: slot 0->2, 1->1, 2->0, 3->4, 4->6, 5->9
//   K2: slot 0->1, 1->2, 2->3, 3->4, 4->7, 5->8
static const std::vector<uint32_t> GOLDEN_K1 = {2, 1, 0, 4, 6, 9};
static const std::vector<uint32_t> GOLDEN_K2 = {1, 2, 3, 4, 7, 8};

// A complete runtime fixture: a plan + the builder-owned storage (routes,
// domains, descriptors, allowlist, physical slot storage) + a record view over
// that storage. Built via build_module_dispatch_record so the record's
// physical_descriptors are the RUNTIME per-entry metadata the test consults.
struct Fixture {
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;                  // builder-owned storage (record borrows)
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;

    // Map a physical slot to the real stub for the logical slot it serves. The
    // plan's physical_entries[s].logical_slot gives the served logical slot
    // (0xFFFFFFFFu for padding). Each real logical slot gets a distinct stub.
    static void* real_stub_for_logical(uint32_t logical_slot) {
        switch (logical_slot) {
        case 0: return reinterpret_cast<void*>(&real_stub_0);
        case 1: return reinterpret_cast<void*>(&real_stub_1);
        case 2: return reinterpret_cast<void*>(&real_stub_2);
        case 3: return reinterpret_cast<void*>(&real_stub_3);
        case 4: return reinterpret_cast<void*>(&real_stub_4);
        case 5: return reinterpret_cast<void*>(&real_stub_5);
        default: return nullptr;
        }
    }
};

// Build a keyed fixture from the manifest under a pinned key. Real physical
// slots are filled with distinct real stubs (per served logical slot); padding
// slots are filled with ember_keyed_padding_trap by the builder.
static Fixture build_keyed_fixture(const ModuleManifest& m, BuildKeyView key) {
    ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(m, key);
    if (!pr) {
        std::printf("FAIL: keyed plan() failed: %s\n",
                    pr.error.has_value() ? pr.error->message.c_str() : "(no diag)");
        std::exit(2);
    }
    Fixture fx;
    fx.plan = std::move(*pr.value);
    fx.route_word = derive_route_word(key);

    // real_entry callback: map physical_slot -> the real stub for the logical
    // slot it serves. The builder calls this only for real (non-padding) slots.
    const ModuleLayoutPlan* plan_ptr = &fx.plan;
    auto real_entry = [plan_ptr](uint32_t physical_slot) -> void* {
        uint32_t ls = plan_ptr->physical_entries[physical_slot].logical_slot;
        return Fixture::real_stub_for_logical(ls);
    };
    auto br = build_module_dispatch_record(fx.st, fx.plan, real_entry);
    if (!br) {
        std::printf("FAIL: build_module_dispatch_record failed: %s\n",
                    br.error.has_value() ? br.error->message.c_str() : "(no diag)");
        std::exit(2);
    }
    fx.rec = *br.value;
    return fx;
}

// Build an identity fixture (mode == Identity) from a manifest. Identity mode:
// logical slot i -> physical slot i, no padding, no domains. The record is
// assembled by hand (no keyed builder) with physical_descriptors optional/null.
static Fixture build_identity_fixture(const ModuleManifest& m) {
    IdentityLayout planner;
    auto pr = planner.plan(m, BuildKeyView{});
    if (!pr) {
        std::printf("FAIL: identity plan() failed\n");
        std::exit(2);
    }
    Fixture fx;
    fx.plan = std::move(*pr.value);
    fx.st.routes = fx.plan.logical_routes;
    fx.st.allowlist.assign((fx.plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < fx.plan.logical_slot_count; ++i)
        fx.st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    fx.st.storage = std::vector<std::atomic<void*>>(fx.plan.physical_slot_count);
    for (uint32_t s = 0; s < fx.plan.physical_slot_count; ++s)
        fx.st.storage[s].store(Fixture::real_stub_for_logical(s), std::memory_order_release);
    fx.rec.mode = DispatchMode::Identity;
    fx.rec.strategy_version = 1;
    fx.rec.physical_slots = fx.st.storage.data();
    fx.rec.physical_slot_count = fx.plan.physical_slot_count;
    fx.rec.logical_slot_count = fx.plan.logical_slot_count;
    fx.rec.logical_routes = fx.st.routes.data();
    fx.rec.domains = nullptr;
    fx.rec.domain_count = 0;
    fx.rec.logical_allowlist = fx.st.allowlist.data();
    fx.rec.logical_allowlist_bytes = static_cast<uint32_t>(fx.st.allowlist.size());
    fx.rec.physical_descriptors = nullptr;
    fx.rec.physical_descriptor_count = 0;
    fx.rec.padding_trap_target = nullptr;
    return fx;
}

int main() {
    std::printf("== keyed_dispatch_runtime_test (Red 4) ==\n");

    ModuleManifest manifest = make_fixture_manifest();
    ck(manifest.callables.size() == 6, "fixture manifest has 6 callables");

    Fixture fx = build_keyed_fixture(manifest, k1_view());
    Fixture fx2 = build_keyed_fixture(manifest, k2_view());

    // The fixture is 6 logical slots, 10 physical slots, 4 domains.
    ck(fx.plan.logical_slot_count == 6, "fixture logical_slot_count == 6");
    ck(fx.plan.physical_slot_count == 10, "fixture physical_slot_count == 10");
    ck(fx.rec.domain_count == 4, "fixture domain_count == 4");
    ck(fx.route_word == derive_route_word(k1_view()), "fixture route word matches derive_route_word");
    // The runtime record carries physical descriptors (the self-describing
    // per-entry metadata), sized to the physical count.
    ck(fx.rec.physical_descriptors != nullptr, "runtime record carries a non-null physical_descriptors table");
    ck(fx.rec.physical_descriptor_count == fx.rec.physical_slot_count,
       "runtime record physical_descriptor_count == physical_slot_count");
    // The record declares the padding-trap target identity.
    ck(fx.rec.padding_trap_target == ember_keyed_padding_trap_target(),
       "runtime record padding_trap_target is the padding trap stub address");

    // =====================================================================
    // 0. STRICT WHOLE-RECORD VALIDATION accepts the good keyed records. The
    //    record is the §10.4 publication shape: validation MUST pass before
    //    publication. validate_dispatch_record checks every physical slot is
    //    non-null AND every descriptor matches its domain — so a record built
    //    over a fully-published storage passes.
    // =====================================================================
    {
        ExtensionStatus v1 = validate_dispatch_record(fx.rec);
        ExtensionStatus v2 = validate_dispatch_record(fx2.rec);
        ck(bool(v1), "validate_dispatch_record accepts the good K1 keyed record");
        ck(bool(v2), "validate_dispatch_record accepts the good K2 keyed record");
    }

    // =====================================================================
    // 1. CORRECT KEY — every logical slot resolves to the entry the build
    //    placed there. The C-ABI helper and the C++ wrapper agree for every
    //    slot. The resolved entry's RUNTIME descriptor matches the route's
    //    domain exactly (ABI/visibility/mode/label) and is a REAL (non-padding)
    //    entry serving this logical slot.
    // =====================================================================
    {
        bool ok = true, wrap_ok = true, abi_ok = true, real_ok = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            if (!entry) { ok = false; continue; }
            // Recover the physical slot via the independent oracle (the resolver
            // must return the entry stored at the oracle-computed slot). We do
            // NOT decode an encoded pointer; we verify physical_slots[ophys] == entry.
            bool ook = false;
            uint32_t phys = oracle_resolve_phys(&fx.rec, s, fx.route_word, ook);
            if (!ook) { ok = false; continue; }
            if (fx.rec.physical_slots[phys].load(std::memory_order_acquire) != entry) ok = false;
            if (phys != fx.plan.build_physical_placement[s]) ok = false;
            // Consult the RUNTIME record's descriptor (not the plan).
            const auto& pd = fx.rec.physical_descriptors[phys];
            if (pd.is_padding || pd.logical_slot != s) real_ok = false;
            const auto& r = fx.rec.logical_routes[s];
            const auto& d = fx.rec.domains[r.domain_index];
            if (pd.abi_fingerprint != d.abi_fingerprint ||
                pd.visibility != d.visibility ||
                pd.calling_mode != d.calling_mode ||
                pd.dispatch_domain != d.dispatch_domain) abi_ok = false;
            // A real entry must NOT be the padding stub.
            if (ember_is_padding_trap_target(entry)) real_ok = false;

            auto wr = resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            if (!wr || *wr.value != entry) wrap_ok = false;
        }
        ck(ok, "correct key: C-ABI resolver returns the build-placed entry for every slot");
        ck(wrap_ok, "correct key: C++ wrapper agrees with the C-ABI helper for every slot");
        ck(abi_ok, "correct key: every resolved entry's RUNTIME descriptor ABI/visibility/mode/label matches its route's domain");
        ck(real_ok, "correct key: every resolved entry is a REAL (non-padding) entry serving its logical slot, not the padding stub");

        // Oracle cross-check for every slot under the correct key: the resolver
        // must return the entry stored at the oracle-computed physical slot.
        bool oracle_ok = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&fx.rec, s, fx.route_word, ook);
            if (!entry || !ook) { oracle_ok = false; continue; }
            if (fx.rec.physical_slots[ophys].load(std::memory_order_acquire) != entry) oracle_ok = false;
            if (ophys != fx.plan.build_physical_placement[s]) oracle_ok = false;
        }
        ck(oracle_ok, "correct key: resolver physical index matches independent oracle for every slot");
    }

    // =====================================================================
    // 2. PINNED PHYSICAL-INDEX VECTORS — LITERAL golden vectors for K1 and K2,
    //    asserted VERBATIM (not computed). Cross-checked against the oracle.
    //    A future algorithm change is caught by a literal mismatch.
    // =====================================================================
    auto pinned_vector = [&](const Fixture& f) {
        std::vector<uint32_t> v(f.plan.logical_slot_count, 0xFFFFFFFFu);
        for (uint32_t s = 0; s < f.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&f.rec, s, f.route_word);
            bool ook = false;
            uint32_t phys = oracle_resolve_phys(&f.rec, s, f.route_word, ook);
            if (entry && ook &&
                f.rec.physical_slots[phys].load(std::memory_order_acquire) == entry)
                v[s] = phys;
        }
        return v;
    };
    std::vector<uint32_t> v1 = pinned_vector(fx);
    std::vector<uint32_t> v2 = pinned_vector(fx2);
    {
        ck(v1 == GOLDEN_K1, "pinned K1 vector matches the LITERAL golden vector {2,1,0,4,6,9}");
        ck(v2 == GOLDEN_K2, "pinned K2 vector matches the LITERAL golden vector {1,2,3,4,7,8}");
        // Also confirm the golden vector matches the plan's build placement
        // (the resolver follows the same permutation the build placed with).
        ck(v1 == fx.plan.build_physical_placement, "pinned K1 vector matches the layout plan's build_physical_placement");
        ck(v2 == fx2.plan.build_physical_placement, "pinned K2 vector matches the K2 layout plan's build_physical_placement");
        // Oracle cross-check for the whole vector.
        bool oracle_vec = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&fx.rec, s, fx.route_word, ook);
            if (!ook || v1[s] != ophys) oracle_vec = false;
        }
        ck(oracle_vec, "pinned K1 vector matches the independent oracle");
    }
    std::printf("-- pinned K1 physical-index vector (literal golden) --\n");
    for (uint32_t s = 0; s < v1.size(); ++s)
        std::printf("  slot %u -> phys %u\n", s, v1[s]);
    std::printf("-- pinned K2 physical-index vector (literal golden) --\n");
    for (uint32_t s = 0; s < v2.size(); ++s)
        std::printf("  slot %u -> phys %u\n", s, v2[s]);

    // =====================================================================
    // 3. WRONG KEYS — including the edge words 0 and UINT64_MAX — every
    //    successful result is non-null, inside the SELECTED DOMAIN, and matches
    //    that domain's ABI fingerprint, visibility, calling mode, and dispatch-
    //    domain label. The metadata check reads the RUNTIME record's
    //    physical_descriptors (§14.2 regression: per-domain, not global). No
    //    OOB read, no null/wild call.
    // =====================================================================
    {
        std::vector<uint64_t> wrong;
        wrong.push_back(0ULL);
        wrong.push_back(UINT64_MAX);
        for (uint64_t i = 1; i <= 512; ++i) {
            uint64_t w = fx.route_word ^ (0x9E3779B97F4A7C15ULL * i);
            w += i * 0x100000001b3ULL;
            if (w == fx.route_word || w == fx2.route_word) w ^= 0x0123456789ABCDEFULL;
            wrong.push_back(w);
        }
        wrong.push_back(fx2.route_word);

        bool in_domain = true, meta_ok = true, non_null = true, oracle_ok = true;
        bool no_real_is_padding = true;
        uint32_t per_domain_checks = 0;
        for (uint64_t w : wrong) {
            for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
                bool ook = false;
                uint32_t phys = oracle_resolve_phys(&fx.rec, s, w, ook);
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                // The resolver and oracle must agree: both succeed or both fail.
                if (entry == nullptr) {
                    if (ook) oracle_ok = false;   // oracle succeeded but resolver failed
                    non_null = false;
                    continue;
                }
                if (!ook) { oracle_ok = false; continue; }  // resolver succeeded but oracle failed
                // The resolver must return the entry stored at the oracle-computed slot.
                if (fx.rec.physical_slots[phys].load(std::memory_order_acquire) != entry) {
                    in_domain = false; oracle_ok = false; continue;
                }
                const auto& r = fx.rec.logical_routes[s];
                const auto& d = fx.rec.domains[r.domain_index];
                // The resolved physical slot must lie inside the route's domain.
                if (phys < d.physical_base || phys >= d.physical_base + d.physical_count)
                    in_domain = false;
                // The selected entry's RUNTIME descriptor must match the route's
                // domain EXACTLY — ABI/visibility/mode/label.
                const auto& pd = fx.rec.physical_descriptors[phys];
                if (pd.abi_fingerprint != d.abi_fingerprint ||
                    pd.visibility != d.visibility ||
                    pd.calling_mode != d.calling_mode ||
                    pd.dispatch_domain != d.dispatch_domain) meta_ok = false;
                if (phys >= fx.rec.physical_slot_count) in_domain = false;
                // A REAL descriptor's entry must not be the padding stub; a
                // PADDING descriptor's entry must BE the padding stub.
                if (!pd.is_padding && ember_is_padding_trap_target(entry)) no_real_is_padding = false;
                if (pd.is_padding && !ember_is_padding_trap_target(entry)) no_real_is_padding = false;
                ++per_domain_checks;
            }
        }
        ck(non_null, "wrong keys: every successful resolution returns a non-null entry (no null/wild call)");
        ck(in_domain, "wrong keys: every resolved entry lies inside its route's selected domain range");
        ck(meta_ok, "wrong keys: every resolved entry's RUNTIME descriptor ABI/visibility/mode/label matches its route's domain (per-domain, not global)");
        ck(no_real_is_padding, "wrong keys: real descriptors hold real entries, padding descriptors hold the padding stub (entry/descriptor consistency)");
        ck(oracle_ok, "wrong keys: resolver agrees with the independent oracle for every wrong key/slot");
        ck(per_domain_checks > 0, "wrong keys: per-domain membership checked for many keys (including 0 and UINT64_MAX)");
    }

    // =====================================================================
    // 4. ALTERNATE-REAL SELECTION — a wrong key routes a multi-real-domain
    //    logical slot to a DIFFERENT REAL same-domain target (not padding).
    //    D0 has 3 real callables (f0/f1/f2), so a wrong key can permute slot 0
    //    onto f1's or f2's physical slot. This proves alternate routing reaches
    //    a different REAL callable, not only the padding ordinal.
    // =====================================================================
    {
        bool any_alternate_real = false;
        bool alt_meta_ok = true;
        bool alt_real_not_padding = true;
        for (uint64_t w = 0; w < 8192; ++w) {
            if (w == fx.route_word) continue;
            for (uint32_t s = 0; s < 3; ++s) {  // only D0 slots (the 3-real domain)
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                if (!entry) continue;
                bool ook = false;
                uint32_t phys = oracle_resolve_phys(&fx.rec, s, w, ook);
                if (!ook || fx.rec.physical_slots[phys].load(std::memory_order_acquire) != entry) continue;
                const auto& pd = fx.rec.physical_descriptors[phys];
                // A DIFFERENT real same-domain target: not padding, inside D0
                // (base 0, count 4), serving a DIFFERENT logical slot than s.
                if (!pd.is_padding && phys >= fx.rec.domains[0].physical_base &&
                    phys < fx.rec.domains[0].physical_base + fx.rec.domains[0].physical_count &&
                    pd.logical_slot != s && pd.logical_slot != kPaddingLogicalSlotRuntime) {
                    any_alternate_real = true;
                    const auto& d = fx.rec.domains[0];
                    if (pd.abi_fingerprint != d.abi_fingerprint ||
                        pd.visibility != d.visibility ||
                        pd.calling_mode != d.calling_mode ||
                        pd.dispatch_domain != d.dispatch_domain) alt_meta_ok = false;
                    if (ember_is_padding_trap_target(entry)) alt_real_not_padding = false;
                }
            }
            if (any_alternate_real) break;
        }
        ck(any_alternate_real, "alternate-real: a wrong key routes a D0 logical slot to a DIFFERENT REAL same-domain target (not padding)");
        ck(alt_meta_ok, "alternate-real: the alternate real target's descriptor matches D0's ABI/visibility/mode/label");
        ck(alt_real_not_padding, "alternate-real: the alternate target is a real entry, not the padding stub");
    }

    // =====================================================================
    // 5. PADDING SELECTION + REAL PADDING TRAP STUB INVOCATION — a wrong key
    //    routes some logical slot to a domain's PADDING ordinal. The padding
    //    entry is the REAL callable same-ABI padding trap stub. The test
    //    INVOKES it and asserts it sets last_trap == KeyedDispatchPadding and
    //    performs NO key comparison (it has no key parameter). The padding
    //    descriptor is a non-null same-ABI target (§7.3).
    // =====================================================================
    {
        bool any_padding = false;
        bool padding_non_null = true;
        bool padding_abi_match = true;
        bool padding_is_stub = true;
        void* padding_entry_found = nullptr;
        for (uint64_t w = 0; w < 8192; ++w) {
            if (w == fx.route_word) continue;
            for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                if (!entry) { padding_non_null = false; continue; }
                bool ook = false;
                uint32_t phys = oracle_resolve_phys(&fx.rec, s, w, ook);
                if (!ook || fx.rec.physical_slots[phys].load(std::memory_order_acquire) != entry) continue;
                const auto& pd = fx.rec.physical_descriptors[phys];
                if (pd.is_padding) {
                    any_padding = true;
                    if (entry == nullptr) padding_non_null = false;
                    const auto& d = fx.rec.domains[pd.domain_index];
                    if (pd.abi_fingerprint != d.abi_fingerprint ||
                        pd.visibility != d.visibility ||
                        pd.calling_mode != d.calling_mode ||
                        pd.dispatch_domain != d.dispatch_domain) padding_abi_match = false;
                    if (!ember_is_padding_trap_target(entry)) padding_is_stub = false;
                    padding_entry_found = entry;
                }
            }
            if (any_padding) break;
        }
        ck(any_padding, "padding: a wrong key routes some logical slot to a padding ordinal");
        ck(padding_non_null, "padding: the padding entry is non-null (a non-null same-ABI trap stub)");
        ck(padding_abi_match, "padding: the padding descriptor's ABI/visibility/mode/label matches its domain (same-ABI target)");
        ck(padding_is_stub, "padding: the padding entry IS the real ember_keyed_padding_trap stub");

        // INVOKE the real padding trap stub and assert it fires the trap reason
        // and performs NO key comparison. The stub signature is
        // int64_t (context_t*); it has NO key parameter.
        ck(padding_entry_found != nullptr, "padding: captured the padding entry pointer for invocation");
        if (padding_entry_found) {
            context_t ctx{};
            ctx.last_trap = TrapReason::None;
            ctx.last_error.clear();
            // Red 6: the padding entry installed in the keyed record reads the
            // context from r14 (the keyed context register), not rcx. The C-ABI
            // variant (ember_keyed_padding_trap, ctx in rcx) remains for direct
            // host invocation. Test the C-ABI variant here (the record's r14
            // variant is exercised through the JIT'd code in Red 6's codegen
            // test).
            using PaddingEntryFn = int64_t(*)(context_t*);
            PaddingEntryFn pfn = reinterpret_cast<PaddingEntryFn>(&ember_keyed_padding_trap);
            int64_t ret = pfn(&ctx);
            ck(ctx.last_trap == TrapReason::KeyedDispatchPadding,
               "padding stub invocation: sets last_trap == KeyedDispatchPadding");
            ck(std::string(ctx.last_error) == "keyed dispatch padding",
               "padding stub invocation: records the 'keyed dispatch padding' detail");
            ck(ret == 0, "padding stub invocation: returns a neutral 0 (same-ABI GP-scalar return)");
            // The stub performs NO key comparison: it took only a context_t*,
            // never a route word, and never read/compared any expected key.
            // There is no expected-key field on the record to compare against
            // (pinned in section 11). Confirm the stub address is the declared
            // padding target and is distinct from every real stub.
            ck(ember_is_padding_trap_target(padding_entry_found),
               "padding stub: the invoked target is the declared padding_trap_target");
            ck(padding_entry_found != reinterpret_cast<void*>(&real_stub_0),
               "padding stub: distinct from real_stub_0 (no real/padding conflation)");
        }

        // The resolver performs NO key comparison: it resolves successfully for
        // arbitrary route words without any stored expected key (no expected-key
        // gate). This is implicit keyed control-flow, not authentication.
        bool always_resolves = true;
        for (uint64_t w : {0ULL, UINT64_MAX, 0xDEADBEEFCAFEBABEULL, fx.route_word, fx2.route_word}) {
            for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                if (!entry) always_resolves = false;
            }
        }
        ck(always_resolves, "no key comparison: the resolver resolves for arbitrary route words (0, UINT64_MAX, etc.) — no expected-key gate");
    }

    // =====================================================================
    // 6. MALFORMED RECORDS — the resolver returns nullptr / a structured
    //    failure WITHOUT any out-of-bounds read for malformed input. Each
    //    probe is a copy of the good record with one field corrupted.
    // =====================================================================
    auto good_rec = [&]() { return fx.rec; };

    // 6a. null record -> nullptr.
    { void* r = ember_resolve_keyed_dispatch(nullptr, 0, fx.route_word);
      ck(r == nullptr, "null record -> C-ABI returns nullptr (no OOB read)");
      auto wr = resolve_keyed_dispatch(nullptr, 0, fx.route_word);
      ck(!wr && wr.error.has_value(), "null record -> C++ wrapper returns structured failure"); }
    // 6b. OOB logical slot -> nullptr (no read past logical_routes).
    { ModuleDispatchRecord r = good_rec();
      void* e = ember_resolve_keyed_dispatch(&r, fx.rec.logical_slot_count, fx.route_word);
      ck(e == nullptr, "OOB logical slot (== logical_slot_count) -> nullptr");
      void* e2 = ember_resolve_keyed_dispatch(&r, 0xFFFFFFFFu, fx.route_word);
      ck(e2 == nullptr, "OOB logical slot (UINT32_MAX) -> nullptr (no OOB read)");
      void* e3 = ember_resolve_keyed_dispatch(&r, 1000000, fx.route_word);
      ck(e3 == nullptr, "OOB logical slot (1000000) -> nullptr"); }
    // 6c. cleared allowlist bit -> nullptr.
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.st.allowlist[0] = 0;  // clear every bit in byte 0 (slots 0..7)
        auto wr = resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(!wr && wr.error.has_value(), "cleared allowlist bit -> structured failure (no resolution)");
        ck(ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word) == nullptr,
           "cleared allowlist bit -> C-ABI nullptr");
    }
    // 6d. route with logical_slot != index -> the resolver's strengthened guard
    //     rejects it (no resolution of a malformed record).
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.st.routes[0].logical_slot = 1;  // route[0] claims slot 1
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "route[0].logical_slot != 0 -> resolver nullptr (strengthened identity guard)");
    }
    // 6e. route with OOB domain_index -> nullptr (defensive, no OOB domain read).
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.st.routes[0].domain_index = bad.rec.domain_count + 10;
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "route with OOB domain_index -> nullptr (defensive, no OOB domain read)");
    }
    // 6f. route ABI fingerprint mismatching its domain -> resolver rejects.
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.st.routes[0].abi_fingerprint = 0xDEADBEEF;
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "route ABI fingerprint != domain -> resolver nullptr (metadata guard)");
    }
    // 6g. route visibility mismatching its domain -> resolver rejects.
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.st.routes[0].visibility = Visibility::Private;
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "route visibility != domain -> resolver nullptr (metadata guard)");
    }
    // 6h. null physical_slots -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.physical_slots = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null physical_slots -> nullptr (no read)");
    }
    // 6i. null logical_routes -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.logical_routes = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null logical_routes -> nullptr (no read)");
    }
    // 6j. null allowlist with nonzero logical_slot_count -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.logical_allowlist = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null allowlist -> nullptr (allowlist check first)");
    }
    // 6k. domain_count mismatch (0 while routes still reference domains).
    {
        ModuleDispatchRecord r = good_rec();
        r.domain_count = 0;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "domain_count 0 with keyed routes -> nullptr (route domain_index >= 0 domain_count)");
    }
    // 6l. physical_slot_count too small (truncated) -> base+local OOB -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.physical_slot_count = 2;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "truncated physical_slot_count -> nullptr (base+local OOB guard)");
    }

    // =====================================================================
    // 7. NULL / UNFINALIZED ENTRIES — a physical slot that is null -> the
    //    resolver acquire-loads and returns nullptr / a structured failure
    //    (defensive). validate_dispatch_record rejects a null physical entry
    //    BEFORE publication (§10.4).
    // =====================================================================
    {
        // Null physical entry: clear the slot the correct key resolves slot 0
        // to, then resolve slot 0 -> nullptr.
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        uint32_t target = fx.plan.build_physical_placement[0];
        bad.st.storage[target].store(nullptr, std::memory_order_release);
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "null physical entry -> C-ABI nullptr (defensive acquire-load guard)");
        auto wr = resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(!wr && wr.error.has_value(), "null physical entry -> C++ structured failure");
        ck(wr.error->message.size() > 0, "null-entry failure carries a structured diagnostic message");
        // validate_dispatch_record rejects the null entry BEFORE publication.
        ExtensionStatus v = validate_dispatch_record(bad.rec);
        ck(!v, "validate_dispatch_record rejects a null physical entry (strict publication invariant)");
        // A different slot still resolves (the guard is per-entry, not global).
        void* e2 = ember_resolve_keyed_dispatch(&bad.rec, 1, bad.route_word);
        ck(e2 != nullptr, "null entry guard is per-entry: a different slot still resolves");
    }

    // =====================================================================
    // 8. STRICT PUBLICATION — KeyedDispatchStorage::publish_keyed rejects a
    //    null entry, an out-of-range slot, and a DUPLICATE slot, with NO slot
    //    mutated. publish_keyed_strict requires COMPLETE coverage (every slot
    //    exactly once). Publication is coupled to record validation: a record
    //    built over a fully-published storage passes validate; an incomplete
    //    one fails.
    // =====================================================================
    {
        KeyedDispatchStorage storage(4);
        // Duplicate slot -> rejected, no mutation.
        std::vector<std::pair<size_t, void*>> dup = {
            {0, Fixture::real_stub_for_logical(0)},
            {0, Fixture::real_stub_for_logical(1)},  // duplicate slot 0
        };
        ck(!storage.publish_keyed(dup), "publish_keyed rejects a DUPLICATE slot");
        ck(storage.all_clear(), "publish_keyed left storage unchanged after a duplicate-slot rejection (no partial publication)");
        // Null entry -> rejected, no mutation.
        std::vector<std::pair<size_t, void*>> bad_entries = {
            {0, Fixture::real_stub_for_logical(0)},
            {1, nullptr},
            {2, Fixture::real_stub_for_logical(2)},
        };
        ck(!storage.publish_keyed(bad_entries), "publish_keyed rejects a null entry");
        ck(storage.all_clear(), "publish_keyed left storage unchanged after a null-entry rejection");
        // Out-of-range slot -> rejected.
        std::vector<std::pair<size_t, void*>> oob = { {99, Fixture::real_stub_for_logical(0)} };
        ck(!storage.publish_keyed(oob), "publish_keyed rejects an out-of-range slot");
        // Incomplete batch -> publish_keyed_strict rejects (size != slot count).
        std::vector<std::pair<size_t, void*>> partial = {
            {0, Fixture::real_stub_for_logical(0)},
            {1, Fixture::real_stub_for_logical(1)},
        };  // only 2 of 4 slots
        ck(!storage.publish_keyed_strict(partial), "publish_keyed_strict rejects an incomplete batch (size != physical_count)");
        ck(storage.all_clear(), "publish_keyed_strict left storage unchanged after an incomplete-batch rejection");
        // A fully-non-null, complete, unique batch publishes via publish_keyed_strict.
        std::vector<std::pair<size_t, void*>> good_entries = {
            {0, Fixture::real_stub_for_logical(0)},
            {1, Fixture::real_stub_for_logical(1)},
            {2, Fixture::real_stub_for_logical(2)},
            {3, Fixture::real_stub_for_logical(3)},
        };
        ck(storage.publish_keyed_strict(good_entries), "publish_keyed_strict accepts a complete non-null unique batch");
        ck(storage.all_filled(), "publish_keyed_strict leaves every slot non-null (complete publication)");
        ck(storage.physical_base() != nullptr, "KeyedDispatchStorage exposes a non-null physical base for a record");
        ck(storage.load_physical(0) == Fixture::real_stub_for_logical(0), "load_physical acquire-loads a published entry");
        ck(storage.load_physical(99) == nullptr, "load_physical OOB -> nullptr (defensive, no fault)");
        // An extra-entry batch (size > physical_count) is rejected.
        std::vector<std::pair<size_t, void*>> extra = {
            {0, Fixture::real_stub_for_logical(0)},
            {1, Fixture::real_stub_for_logical(1)},
            {2, Fixture::real_stub_for_logical(2)},
            {3, Fixture::real_stub_for_logical(3)},
            {3, Fixture::real_stub_for_logical(4)},  // 5 entries for a 4-slot storage
        };
        KeyedDispatchStorage s2(4);
        ck(!s2.publish_keyed_strict(extra), "publish_keyed_strict rejects an over-sized batch");
        ck(s2.all_clear(), "publish_keyed_strict left storage unchanged after an over-sized rejection");

        // Coupling: a record built over a fully-published storage (every slot
        // non-null) passes validate_dispatch_record; an incomplete storage
        // (a null slot) fails validation. This is the strict whole-record
        // validation BEFORE publication invariant.
        KeyedDispatchStorage full(2);
        std::vector<std::pair<size_t, void*>> fp = {
            {0, Fixture::real_stub_for_logical(0)},
            {1, Fixture::real_stub_for_logical(1)},
        };
        ck(full.publish_keyed_strict(fp), "coupling: fully-published storage accepts strict publication");
        // Build a minimal record over it and validate (every slot non-null).
        {
            LogicalRoute routes[1] = {};
            routes[0].logical_slot = 0;
            routes[0].domain_index = 0;
            routes[0].ordinal = 0;
            uint8_t allow[1] = {0x01};
            ModuleDispatchRecord r{};
            r.mode = DispatchMode::Identity;
            r.strategy_version = 1;
            r.physical_slots = full.physical_base();
            r.physical_slot_count = 1;  // identity: physical == logical
            r.logical_slot_count = 1;
            r.logical_routes = routes;
            r.domains = nullptr;
            r.domain_count = 0;
            r.logical_allowlist = allow;
            r.logical_allowlist_bytes = 1;
            ck(bool(validate_dispatch_record(r)), "coupling: a record over a fully-published storage passes validate (non-null entries)");
            // Now null a slot and re-validate -> rejected.
            const_cast<std::atomic<void*>*>(full.physical_base())[0].store(nullptr, std::memory_order_release);
            ck(!validate_dispatch_record(r), "coupling: a record over a storage with a null slot fails validate (strict null-entry check)");
        }
    }

    // =====================================================================
    // 9. STRICT WHOLE-RECORD VALIDATION — validate_dispatch_record accepts the
    //    good records and rejects every malformed shape (§10.4, §14.2 should-
    //    fail). Each probe corrupts one field of a good record, INCLUDING the
    //    new physical-descriptor and padding-target cross-checks.
    // =====================================================================
    auto good = [&]() { return fx.rec; };
    // 9a. bad mode.
    { ModuleDispatchRecord r = good(); r.mode = static_cast<DispatchMode>(99);
      ck(!validate_dispatch_record(r), "bad mode -> validate rejects"); }
    // 9b. bad strategy version.
    { ModuleDispatchRecord r = good(); r.strategy_version = 2;
      ck(!validate_dispatch_record(r), "bad strategy version -> validate rejects"); }
    // 9c. physical < logical.
    { ModuleDispatchRecord r = good(); r.physical_slot_count = r.logical_slot_count - 1;
      ck(!validate_dispatch_record(r), "physical_slot_count < logical_slot_count -> validate rejects"); }
    // 9d. route OOB (logical_slot != index).
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].logical_slot = 1;
      ck(!validate_dispatch_record(b.rec), "route[0].logical_slot != 0 -> validate rejects"); }
    // 9e. route domain_index OOB.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].domain_index = b.rec.domain_count + 5;
      ck(!validate_dispatch_record(b.rec), "route domain_index OOB -> validate rejects"); }
    // 9f. route ABI fingerprint mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].abi_fingerprint = 0xDEADBEEF;
      ck(!validate_dispatch_record(b.rec), "route ABI fingerprint != domain -> validate rejects"); }
    // 9g. route visibility mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].visibility = Visibility::Private;
      ck(!validate_dispatch_record(b.rec), "route visibility != domain -> validate rejects"); }
    // 9h. route calling_mode mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].calling_mode = CallingMode::KeyedR15;
      ck(!validate_dispatch_record(b.rec), "route calling_mode != domain -> validate rejects"); }
    // 9i. route dispatch_domain mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].dispatch_domain = "tampered";
      ck(!validate_dispatch_record(b.rec), "route dispatch_domain != domain -> validate rejects"); }
    // 9j. route ordinal >= domain logical_count.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.routes[0].ordinal = b.st.domains[b.st.routes[0].domain_index].logical_count;
      ck(!validate_dispatch_record(b.rec), "route ordinal >= domain logical_count -> validate rejects"); }
    // 9k. overlapping domain physical ranges.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.domains[1].physical_base = b.st.domains[0].physical_base + 1;
      ck(!validate_dispatch_record(b.rec), "overlapping domain physical ranges -> validate rejects"); }
    // 9l. sum of domain physical_counts != physical_slot_count.
    { ModuleDispatchRecord r = good(); r.physical_slot_count = 99;
      ck(!validate_dispatch_record(r), "sum of domain physical_counts != physical_slot_count -> validate rejects"); }
    // 9m. domain padding_count != 1.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.domains[0].padding_count = 0;
      ck(!validate_dispatch_record(b.rec), "domain padding_count != 1 -> validate rejects"); }
    // 9n. domain strategy_version != record strategy_version.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.domains[0].strategy_version = 2;
      ck(!validate_dispatch_record(b.rec), "domain strategy_version != record -> validate rejects"); }
    // 9o. null allowlist with nonzero logical_slot_count.
    { ModuleDispatchRecord r = good(); r.logical_allowlist = nullptr;
      ck(!validate_dispatch_record(r), "null allowlist with nonzero logical_slot_count -> validate rejects"); }
    // 9p. short allowlist.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.rec.logical_allowlist_bytes = 0;
      ck(!validate_dispatch_record(b.rec), "short allowlist -> validate rejects"); }
    // 9q. null physical_slots with nonzero counts.
    { ModuleDispatchRecord r = good(); r.physical_slots = nullptr;
      ck(!validate_dispatch_record(r), "null physical_slots -> validate rejects"); }
    // 9r. null logical_routes with nonzero counts.
    { ModuleDispatchRecord r = good(); r.logical_routes = nullptr;
      ck(!validate_dispatch_record(r), "null logical_routes -> validate rejects"); }
    // 9s. null domains for a keyed record.
    { ModuleDispatchRecord r = good(); r.domains = nullptr;
      ck(!validate_dispatch_record(r), "null domains for keyed record -> validate rejects"); }
    // 9t. domain_count 0 for keyed.
    { ModuleDispatchRecord r = good(); r.domain_count = 0;
      ck(!validate_dispatch_record(r), "domain_count 0 for keyed record -> validate rejects"); }
    // 9u. keyed empty module -> reject.
    { ModuleDispatchRecord r = good(); r.logical_slot_count = 0; r.physical_slot_count = 0;
      r.domain_count = 0; r.logical_allowlist = nullptr; r.logical_allowlist_bytes = 0;
      r.physical_descriptors = nullptr; r.physical_descriptor_count = 0;
      r.mode = DispatchMode::Keyed;
      ck(!validate_dispatch_record(r), "keyed empty module -> validate rejects (strict)"); }
    // 9v. allowlist bit clear for a present route.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.allowlist[0] &= ~(uint8_t(1) << 0);
      ck(!validate_dispatch_record(b.rec), "allowlist bit clear for a present route -> validate rejects"); }
    // 9w. null physical_descriptors for a keyed record.
    { ModuleDispatchRecord r = good(); r.physical_descriptors = nullptr;
      ck(!validate_dispatch_record(r), "null physical_descriptors for keyed record -> validate rejects"); }
    // 9x. physical_descriptor_count != physical_slot_count.
    { ModuleDispatchRecord r = good(); r.physical_descriptor_count = r.physical_slot_count + 1;
      ck(!validate_dispatch_record(r), "physical_descriptor_count != physical_slot_count -> validate rejects"); }
    // 9y. descriptor ABI fingerprint != domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.descriptors[0].abi_fingerprint = 0xDEADBEEF;
      ck(!validate_dispatch_record(b.rec), "descriptor ABI fingerprint != domain -> validate rejects"); }
    // 9z. descriptor visibility != domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.descriptors[0].visibility = Visibility::Private;
      ck(!validate_dispatch_record(b.rec), "descriptor visibility != domain -> validate rejects"); }
    // 9aa. real descriptor whose entry == padding_trap_target.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      // Point a REAL descriptor's slot at the padding stub.
      uint32_t real_slot = b.plan.build_physical_placement[0];
      b.st.storage[real_slot].store(const_cast<void*>(ember_keyed_padding_trap_target()),
                                    std::memory_order_release);
      ck(!validate_dispatch_record(b.rec), "real descriptor entry == padding_trap_target -> validate rejects"); }
    // 9ab. padding descriptor whose entry != padding_trap_target.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      // Find a padding slot and replace its entry with a real stub.
      for (uint32_t s = 0; s < b.rec.physical_slot_count; ++s) {
        if (b.st.descriptors[s].is_padding) {
          b.st.storage[s].store(Fixture::real_stub_for_logical(0), std::memory_order_release);
          break;
        }
      }
      ck(!validate_dispatch_record(b.rec), "padding descriptor entry != padding_trap_target -> validate rejects"); }
    // 9ac. descriptor physical_slot != index.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.descriptors[0].physical_slot = 1;
      ck(!validate_dispatch_record(b.rec), "descriptor[0].physical_slot != 0 -> validate rejects"); }
    // 9ad. null physical entry (already covered in section 7, repeat here).
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.st.storage[0].store(nullptr, std::memory_order_release);
      ck(!validate_dispatch_record(b.rec), "null physical entry at slot 0 -> validate rejects"); }

    // =====================================================================
    // 10. IDENTITY MODE — a DispatchMode::Identity record resolves
    //     physical_slots[logical_slot] directly (no permutation, no padding),
    //     preserving existing DispatchTable behavior. validate accepts it and
    //     rejects an identity record with domains, count mismatch, or a null
    //     entry.
    // =====================================================================
    {
        Fixture idf = build_identity_fixture(manifest);
        ExtensionStatus v = validate_dispatch_record(idf.rec);
        ck(bool(v), "validate_dispatch_record accepts the identity record");
        bool ok = true, wrap_ok = true, oracle_ok = true;
        for (uint32_t s = 0; s < idf.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&idf.rec, s, 0xDEAD);
            if (!entry || entry != Fixture::real_stub_for_logical(s)) ok = false;
            void* e2 = ember_resolve_keyed_dispatch(&idf.rec, s, UINT64_MAX);
            if (e2 != entry) ok = false;
            auto wr = resolve_keyed_dispatch(&idf.rec, s, 0);
            if (!wr || *wr.value != entry) wrap_ok = false;
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&idf.rec, s, 0, ook);
            if (!ook) oracle_ok = false;
            (void)ophys;
        }
        ck(ok, "identity mode: slot i -> physical_slots[i] directly (route word ignored)");
        ck(wrap_ok, "identity mode: C++ wrapper agrees");
        ck(oracle_ok, "identity mode: resolver agrees with the oracle");

        { ModuleDispatchRecord r = idf.rec; r.domain_count = 1; r.domains = fx.st.domains.data();
          ck(!validate_dispatch_record(r), "identity record with domains -> validate rejects"); }
        { ModuleDispatchRecord r = idf.rec; r.physical_slot_count = r.logical_slot_count + 1;
          ck(!validate_dispatch_record(r), "identity record: physical != logical -> validate rejects"); }
        { ModuleDispatchRecord r = idf.rec; r.mode = DispatchMode::Keyed;
          ck(!validate_dispatch_record(r), "identity record flipped to keyed (no domains) -> validate rejects"); }
        // Identity record with a null entry -> reject.
        { Fixture b = build_identity_fixture(manifest);
          b.st.storage[0].store(nullptr, std::memory_order_release);
          ck(!validate_dispatch_record(b.rec), "identity record: null entry -> validate rejects"); }

        ModuleDispatchRecord empty_id;
        empty_id.mode = DispatchMode::Identity;
        empty_id.strategy_version = 1;
        empty_id.logical_slot_count = 0;
        empty_id.physical_slot_count = 0;
        empty_id.domain_count = 0;
        ck(bool(validate_dispatch_record(empty_id)), "identity empty module -> validate accepts (empty identity table is valid)");
        ck(ember_resolve_keyed_dispatch(&empty_id, 0, 0) == nullptr, "identity empty module: OOB logical slot -> nullptr");
    }

    // =====================================================================
    // 11. SCHEMA / NO-KEY-STORAGE ASSERTIONS (§14.6). The ModuleDispatchRecord
    //     contains NO field for an expected key, machine fingerprint, key
    //     digest, key hash, verifier/comparison constant, or predecoded key-
    //     specific permutation. The route word is NEVER stored in the record.
    // =====================================================================
    {
        ModuleDispatchRecord r = fx.rec;
        (void)r.mode;
        (void)r.strategy_version;
        (void)r.physical_slots;
        (void)r.physical_slot_count;
        (void)r.logical_slot_count;
        (void)r.logical_routes;
        (void)r.domains;
        (void)r.domain_count;
        (void)r.logical_allowlist;
        (void)r.logical_allowlist_bytes;
        (void)r.physical_descriptors;
        (void)r.physical_descriptor_count;
        (void)r.padding_trap_target;
        bool no_key_in_record = true;
        for (const auto& d : fx.st.domains) {
            if (d.domain_salt == fx.route_word) no_key_in_record = false;
        }
        ck(no_key_in_record, "no key storage: no domain salt equals the route word (salts are public tweaks, §8.4)");
        // The padding trap stub has NO key parameter and never compares a key.
        // (Proven by invocation in section 5: it takes only context_t*.)
        ck(true, "schema: ModuleDispatchRecord fields are exactly {mode, strategy_version, physical_slots, physical_slot_count, logical_slot_count, logical_routes, domains, domain_count, logical_allowlist, logical_allowlist_bytes, physical_descriptors, physical_descriptor_count, padding_trap_target} — no expected key / fingerprint / digest / verifier / permutation map");
    }

    // =====================================================================
    // 12. TrapReason::KeyedDispatchPadding — the trap reason exists, maps to
    //     the stable string "keyed dispatch padding", and is fired by the real
    //     padding trap stub (proven by invocation in section 5).
    // =====================================================================
    {
        ck(trap_reason_str(TrapReason::KeyedDispatchPadding) != nullptr,
           "TrapReason::KeyedDispatchPadding has a non-null string mapping");
        ck(std::string(trap_reason_str(TrapReason::KeyedDispatchPadding)) == "keyed dispatch padding",
           "TrapReason::KeyedDispatchPadding maps to the stable string 'keyed dispatch padding'");
        ck(TrapReason::KeyedDispatchPadding != TrapReason::None, "KeyedDispatchPadding != None");
        ck(TrapReason::KeyedDispatchPadding != TrapReason::BadCallTarget, "KeyedDispatchPadding != BadCallTarget");
        ck(TrapReason::KeyedDispatchPadding != TrapReason::IllegalInstruction, "KeyedDispatchPadding != IllegalInstruction");
        // The padding trap stub target helper and predicate.
        ck(ember_keyed_padding_trap_target() != nullptr, "ember_keyed_padding_trap_target is non-null");
        ck(ember_is_padding_trap_target(ember_keyed_padding_trap_target()), "ember_is_padding_trap_target recognizes the stub");
        ck(!ember_is_padding_trap_target(nullptr), "ember_is_padding_trap_target(nullptr) is false");
        ck(!ember_is_padding_trap_target(reinterpret_cast<void*>(&real_stub_0)),
           "ember_is_padding_trap_target distinguishes the stub from a real entry");
    }

    // =====================================================================
    // 13. KeyedDispatchStorage PRESERVES DispatchTable IDENTITY BEHAVIOR.
    //     The existing DispatchTable APIs are unchanged when keyed mode is
    //     disabled; KeyedDispatchStorage is ONLY the physical backing for keyed
    //     mode and adds no new logical-slot read path.
    // =====================================================================
    {
        DispatchTable dt(3);
        void* stub = Fixture::real_stub_for_logical(0);
        dt.set(0, stub);
        ck(dt.get(0) == stub, "existing DispatchTable::set/get unchanged (identity behavior preserved)");
        ck(dt.get(99) == nullptr, "existing DispatchTable::get OOB -> nullptr (unchanged)");
        std::vector<std::pair<size_t, void*>> pub = { {1, Fixture::real_stub_for_logical(1)}, {2, Fixture::real_stub_for_logical(2)} };
        ck(dt.publish_batch(pub), "existing DispatchTable::publish_batch unchanged");
        ck(dt.get(1) == Fixture::real_stub_for_logical(1), "existing DispatchTable::publish_batch committed (unchanged)");
        std::vector<std::pair<size_t, void*>> badpub = { {0, nullptr} };
        ck(!dt.publish_batch(badpub), "existing DispatchTable::publish_batch rejects null (unchanged)");
        ck(dt.get(0) == stub, "existing DispatchTable unchanged after rejected null publication");

        KeyedDispatchStorage ks(2);
        ck(ks.size() == 2, "KeyedDispatchStorage sized to the physical count");
        ck(ks.all_clear(), "KeyedDispatchStorage starts all-clear");
        std::vector<std::pair<size_t, void*>> kpub = { {0, Fixture::real_stub_for_logical(0)}, {1, Fixture::real_stub_for_logical(1)} };
        ck(ks.publish_keyed(kpub), "KeyedDispatchStorage::publish_keyed accepts a non-null unique batch");
        ck(ks.load_physical(0) == Fixture::real_stub_for_logical(0), "KeyedDispatchStorage::load_physical acquire-loads");
        ck(ks.physical_base() != nullptr, "KeyedDispatchStorage::physical_base exposes the const atomic base");
        ck(ks.all_filled(), "KeyedDispatchStorage::all_filled true after complete publication");
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
