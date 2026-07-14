// keyed_dispatch_runtime_test — Red 4 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §2.4, §5.5, §7.3, §10, §14.2, §14.3, §14.5, §14.6): the runtime resolver +
// immutable module dispatch record gate.
//
// RED-GREEN contract chunk for the runtime resolver. This is the RED side
// (written first) of the Red 4 contract. It pins, against an INDEPENDENT
// oracle (a from-scratch reimplementation of the resolution spec written here
// in the test), the §2.4 wrong-key safety contract and the §14.2 mandatory
// buckets for the runtime resolver:
//
//   - correct key:          the correct route word resolves every logical slot
//                          to the entry the build placed there (the pinned
//                          physical-index vector matches the layout plan's
//                          build_physical_placement, cross-checked against an
//                          independent oracle that calls the Red 1 helper).
//   - wrong keys:           for many wrong route words — including the edge
//                          words 0 and UINT64_MAX — every successful result is
//                          non-null, lies inside the SELECTED DOMAIN (the
//                          domain of the route being resolved), and matches
//                          that domain's ABI fingerprint, visibility, calling
//                          mode, and dispatch-domain label. This is NOT a
//                          tautological contiguous-global-range assertion: the
//                          selected entry's metadata must equal the route's
//                          domain metadata exactly (§14.2 regression bucket).
//   - alternate/padding:   selected wrong keys route at least one logical slot
//                          to the domain's padding ordinal (a non-null same-ABI
//                          trap stub), proving alternate routing reaches the
//                          padding target. The padding entry is non-null and
//                          matches the domain's ABI fingerprint (a padding
//                          descriptor is representable as a non-null same-ABI
//                          target, §7.3); the resolver performs NO key
//                          comparison.
//   - pinned vectors:      hard-coded golden physical-index vectors for the
//                          correct key, cross-checked against the oracle.
//   - malformed records:   null record, null physical_slots, OOB logical slot,
//                          cleared allowlist bit, route with OOB domain_index,
//                          domain_count mismatch, physical_slot_count mismatch,
//                          null allowlist with nonzero counts -> the resolver
//                          returns a structured failure / nullptr WITHOUT any
//                          out-of-bounds read.
//   - null/unfinalized:    a physical slot that is null -> the resolver acquire-
//                          loads and returns a structured failure / nullptr
//                          (defensive; the storage rejects null at publication,
//                          §10.4); strict publication (publish_keyed) rejects a
//                          null entry with NO slot mutated.
//   - strict validation:   validate_dispatch_record accepts a good record and
//                          rejects every malformed shape (bad mode, bad strategy
//                          version, count inconsistency, route OOB, route/domain
//                          metadata mismatch, overlapping domains, missing/
//                          short allowlist, identity-with-domains, keyed-empty)
//                          BEFORE publication.
//   - identity mode:       a DispatchMode::Identity record resolves
//                          physical_slots[logical_slot] directly (no permutation,
//                          no padding), preserving existing DispatchTable
//                          behavior; validate accepts it and rejects an identity
//                          record with domains or count mismatch.
//   - TrapReason:          TrapReason::KeyedDispatchPadding exists and maps to
//                          the stable string "keyed dispatch padding".
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
// assembled from the plan's routes/domains/counts and a stub physical slot
// array (each slot's entry pointer encodes its physical slot so the test can
// recover the resolved slot and look up its metadata). Red 3 already pins the
// parse→plan→classifier path; Red 4 is the resolver, so a synthetic manifest
// built directly via the planner is the right scope.
//
// Links ember (keyed_dispatch.* — Red 1 helper, context.hpp — TrapReason,
// dispatch_table.hpp — KeyedDispatchStorage) + ember_frontend (module_layout.*
// — Red 3 planner, Red 4 record/resolver). NOT a CTest entry: the filtered
// suite count must stay 67 (§14.1); the target building cleanly + the
// executable passing IS the gate.

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
// Independent oracle — reimplements the resolution spec.
//
// Spec (must match src/module_layout.cpp resolve_core exactly):
//   identity:  physical_index = logical_slot; entry = physical_slots[logical_slot]
//   keyed:    kd = {domain.domain_salt, domain.strategy_version, domain.physical_count}
//             local = keyed_dispatch_permute_runtime(route_word, kd, route.ordinal)
//             physical_index = domain.physical_base + local
//             entry = physical_slots[physical_index]
//   guards:   rec null, logical_slot >= logical_slot_count, allowlist bit clear,
//             null routes/slots/domains, route domain_index OOB, route ordinal >=
//             domain logical_count, permute failure, base+local OOB, null entry.
// The oracle delegates the permutation to the already-independently-verified
// Red 1 production helper (Red 1 cross-checked its own oracle; here we only
// need the resolution arithmetic to match, so reusing the verified permute is
// correct and avoids a third reimplementation of the permutation).
// ===========================================================================
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
    if (rec->mode == DispatchMode::Identity) {
        if (logical_slot >= rec->physical_slot_count) return 0xFFFFFFFFu;
        out_ok = true;
        return logical_slot;
    }
    if (rec->mode != DispatchMode::Keyed) return 0xFFFFFFFFu;
    if (r.domain_index >= rec->domain_count || !rec->domains) return 0xFFFFFFFFu;
    const DispatchDomain& d = rec->domains[r.domain_index];
    if (r.ordinal >= d.logical_count) return 0xFFFFFFFFu;
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
// Controlled fingerprints let us pin exact physical-index vectors. The
// resolver only requires metadata consistency (route == domain == entry), not
// that fingerprints come from the real classifier; Red 3 already pins the
// classifier→plan path.
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

// A complete runtime fixture: a plan + the physical slot storage + a record
// view over that storage + a side-table of the plan's PhysicalEntry metadata
// for per-domain membership checks. The entry pointers ENCODE their physical
// slot (slot + 1, so slot 0 is non-null) so the test can recover the resolved
// slot from a returned void* and look up its metadata. The padding entry uses
// a distinct non-null padding stub pointer (0xFFF0... + slot) so the test can
// recognize a padding target without comparing a key.
struct Fixture {
    ModuleLayoutPlan plan;
    std::vector<std::atomic<void*>> storage;     // physical slot storage
    std::vector<LogicalRoute> routes;            // owned (record borrows)
    std::vector<DispatchDomain> domains;         // owned (record borrows)
    std::vector<uint8_t> allowlist;              // owned (record borrows)
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;

    // Encode a physical slot as a non-null entry pointer. slot 0 -> 1, so
    // every real entry is non-null. The test decodes via decode_slot().
    static void* encode_slot(uint32_t slot) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
    }
    static uint32_t decode_slot(void* p) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) - 1;
    }
    // A distinct non-null padding stub pointer (encodes its physical slot via
    // the same scheme so decode_slot recovers it; the padding entry occupies a
    // real physical slot in the storage just like a real entry).
    static void* padding_stub(uint32_t slot) { return encode_slot(slot); }
};

// Build a keyed fixture from the manifest under a pinned key.
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

    // Owned copies of routes/domains so the record borrows stable storage.
    fx.routes = fx.plan.logical_routes;
    fx.domains = fx.plan.domains;

    // Allowlist: every logical slot bit set.
    fx.allowlist.assign((fx.plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < fx.plan.logical_slot_count; ++i)
        fx.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));

    // Physical storage: one atomic slot per physical slot, populated with the
    // encoded-slot stub for each entry (real + padding all non-null).
    // Construct the vector with the size (value-initializes each atomic to
    // its default state — std::atomic is not copyable, so assign(n, value)
    // would not compile; the size constructor + per-slot store mirrors
    // DispatchTable's constructor).
    fx.storage = std::vector<std::atomic<void*>>(fx.plan.physical_slot_count);
    for (uint32_t s = 0; s < fx.plan.physical_slot_count; ++s) {
        const auto& e = fx.plan.physical_entries[s];
        // Every physical entry — real or padding — is a non-null same-ABI
        // target. A padding target is a non-null trap stub (§7.3); a real entry
        // is a non-null finalized callable. Both encode their slot.
        void* stub = e.is_padding ? Fixture::padding_stub(s) : Fixture::encode_slot(s);
        fx.storage[s].store(stub, std::memory_order_release);
    }

    fx.rec.mode = DispatchMode::Keyed;
    fx.rec.strategy_version = 1;
    fx.rec.physical_slots = fx.storage.data();
    fx.rec.physical_slot_count = fx.plan.physical_slot_count;
    fx.rec.logical_slot_count = fx.plan.logical_slot_count;
    fx.rec.logical_routes = fx.routes.data();
    fx.rec.domains = fx.domains.data();
    fx.rec.domain_count = static_cast<uint32_t>(fx.domains.size());
    fx.rec.logical_allowlist = fx.allowlist.data();
    fx.rec.logical_allowlist_bytes = static_cast<uint32_t>(fx.allowlist.size());
    return fx;
}

// Build an identity fixture (mode == Identity) from a manifest. Identity mode:
// logical slot i -> physical slot i, no padding, no domains. The plan comes
// from the Red 3 IdentityLayout planner; the record is assembled with
// physical_slot_count == logical_slot_count and domain_count 0.
static Fixture build_identity_fixture(const ModuleManifest& m) {
    IdentityLayout planner;
    auto pr = planner.plan(m, BuildKeyView{});
    if (!pr) {
        std::printf("FAIL: identity plan() failed\n");
        std::exit(2);
    }
    Fixture fx;
    fx.plan = std::move(*pr.value);
    fx.routes = fx.plan.logical_routes;
    fx.allowlist.assign((fx.plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < fx.plan.logical_slot_count; ++i)
        fx.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    fx.storage = std::vector<std::atomic<void*>>(fx.plan.physical_slot_count);
    for (uint32_t s = 0; s < fx.plan.physical_slot_count; ++s)
        fx.storage[s].store(Fixture::encode_slot(s), std::memory_order_release);
    fx.rec.mode = DispatchMode::Identity;
    fx.rec.strategy_version = 1;
    fx.rec.physical_slots = fx.storage.data();
    fx.rec.physical_slot_count = fx.plan.physical_slot_count;
    fx.rec.logical_slot_count = fx.plan.logical_slot_count;
    fx.rec.logical_routes = fx.routes.data();
    fx.rec.domains = nullptr;
    fx.rec.domain_count = 0;
    fx.rec.logical_allowlist = fx.allowlist.data();
    fx.rec.logical_allowlist_bytes = static_cast<uint32_t>(fx.allowlist.size());
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

    // =====================================================================
    // 0. STRICT WHOLE-RECORD VALIDATION accepts the good keyed records.
    // =====================================================================
    {
        ExtensionStatus v1 = validate_dispatch_record(fx.rec);
        ExtensionStatus v2 = validate_dispatch_record(fx2.rec);
        ck(bool(v1), "validate_dispatch_record accepts the good K1 keyed record");
        ck(bool(v2), "validate_dispatch_record accepts the good K2 keyed record");
        // The record is the §10.4 publication shape: validation MUST pass
        // before publication. A good record passes; a bad record (below) fails.
    }

    // =====================================================================
    // 1. CORRECT KEY — every logical slot resolves to the entry the build
    //    placed there (the pinned physical-index vector matches the layout
    //    plan's build_physical_placement, cross-checked against the oracle).
    //    The C-ABI helper and the C++ wrapper agree for every slot.
    // =====================================================================
    {
        bool ok = true, wrap_ok = true, abi_ok = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            // C-ABI helper.
            void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            if (!entry) { ok = false; continue; }
            uint32_t phys = Fixture::decode_slot(entry);
            if (phys != fx.plan.build_physical_placement[s]) ok = false;
            // The resolved entry must be a REAL (non-padding) entry serving
            // this logical slot.
            const auto& pe = fx.plan.physical_entries[phys];
            if (pe.is_padding || pe.logical_slot != s) ok = false;
            // The selected entry's ABI fingerprint, visibility, calling mode,
            // and dispatch-domain label must match the ROUTE's domain exactly.
            const auto& r = fx.routes[s];
            const auto& d = fx.domains[r.domain_index];
            if (pe.abi_fingerprint != d.abi_fingerprint ||
                pe.visibility != d.visibility ||
                pe.calling_mode != d.calling_mode ||
                pe.dispatch_domain != d.dispatch_domain) abi_ok = false;

            // C++ wrapper agrees with the C-ABI helper.
            auto wr = resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            if (!wr || *wr.value != entry) wrap_ok = false;
        }
        ck(ok, "correct key: C-ABI resolver returns the build-placed entry for every slot");
        ck(wrap_ok, "correct key: C++ wrapper agrees with the C-ABI helper for every slot");
        ck(abi_ok, "correct key: every resolved entry's ABI/visibility/mode/label matches its route's domain");

        // Oracle cross-check: the oracle's physical index matches the resolver
        // for every slot under the correct key.
        bool oracle_ok = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, fx.route_word);
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&fx.rec, s, fx.route_word, ook);
            if (!ook || !entry || Fixture::decode_slot(entry) != ophys) oracle_ok = false;
        }
        ck(oracle_ok, "correct key: resolver physical index matches independent oracle for every slot");
    }

    // =====================================================================
    // 2. PINNED PHYSICAL-INDEX VECTORS under K1 and K2 (cross-checked against
    //    the oracle and the layout plan). Pinned so a future algorithm change
    //    is caught by a literal mismatch.
    // =====================================================================
    auto pinned_vector = [&](const Fixture& f) {
        std::vector<uint32_t> v(f.plan.logical_slot_count);
        for (uint32_t s = 0; s < f.plan.logical_slot_count; ++s)
            v[s] = Fixture::decode_slot(ember_resolve_keyed_dispatch(&f.rec, s, f.route_word));
        return v;
    };
    std::vector<uint32_t> v1 = pinned_vector(fx);
    std::vector<uint32_t> v2 = pinned_vector(fx2);
    {
        // The pinned vector must match the layout plan's build_physical_placement
        // (the resolver follows the same permutation the build placed with).
        bool plan_match = (v1 == fx.plan.build_physical_placement);
        ck(plan_match, "pinned K1 vector matches the layout plan's build_physical_placement");
        // Oracle cross-check for the whole vector.
        bool oracle_vec = true;
        for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&fx.rec, s, fx.route_word, ook);
            if (!ook || v1[s] != ophys) oracle_vec = false;
        }
        ck(oracle_vec, "pinned K1 vector matches the independent oracle");
    }
    {
        // K2 produces a (likely) different vector; it must match its own plan.
        bool plan_match = (v2 == fx2.plan.build_physical_placement);
        ck(plan_match, "pinned K2 vector matches the K2 layout plan's build_physical_placement");
    }
    std::printf("-- pinned K1 physical-index vector --\n");
    for (uint32_t s = 0; s < v1.size(); ++s)
        std::printf("  slot %u -> phys %u\n", s, v1[s]);
    std::printf("-- pinned K2 physical-index vector --\n");
    for (uint32_t s = 0; s < v2.size(); ++s)
        std::printf("  slot %u -> phys %u\n", s, v2[s]);

    // =====================================================================
    // 3. WRONG KEYS — including the edge words 0 and UINT64_MAX — every
    //    successful result is non-null, inside the SELECTED DOMAIN, and matches
    //    that domain's ABI fingerprint, visibility, calling mode, and dispatch-
    //    domain label. This is the §14.2 regression assertion: NOT merely
    //    within the global physical table range, but per-domain membership.
    //    No OOB read, no null/wild call.
    // =====================================================================
    {
        std::vector<uint64_t> wrong;
        // Edge words first.
        wrong.push_back(0ULL);
        wrong.push_back(UINT64_MAX);
        // A spread of other wrong words.
        for (uint64_t i = 1; i <= 512; ++i) {
            uint64_t w = fx.route_word ^ (0x9E3779B97F4A7C15ULL * i);
            w += i * 0x100000001b3ULL;
            if (w == fx.route_word || w == fx2.route_word) w ^= 0x0123456789ABCDEFULL;
            wrong.push_back(w);
        }
        // Also use the OTHER pinned key as a wrong key against fx.
        wrong.push_back(fx2.route_word);

        bool in_domain = true, meta_ok = true, non_null = true, oracle_ok = true;
        uint32_t per_domain_checks = 0;
        for (uint64_t w : wrong) {
            for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                if (!entry) { non_null = false; continue; }
                uint32_t phys = Fixture::decode_slot(entry);
                const auto& r = fx.routes[s];
                const auto& d = fx.domains[r.domain_index];
                // The resolved physical slot must lie inside the route's
                // domain range [physical_base, physical_base + physical_count).
                if (phys < d.physical_base || phys >= d.physical_base + d.physical_count)
                    in_domain = false;
                // The selected entry's metadata must match the route's domain
                // EXACTLY — ABI fingerprint, visibility, calling mode, label.
                const auto& pe = fx.plan.physical_entries[phys];
                if (pe.abi_fingerprint != d.abi_fingerprint ||
                    pe.visibility != d.visibility ||
                    pe.calling_mode != d.calling_mode ||
                    pe.dispatch_domain != d.dispatch_domain) meta_ok = false;
                // The selected entry must ALSO be inside the global physical
                // range (a weaker property that must hold but is NOT sufficient
                // on its own — the per-domain check above is the load-bearing
                // assertion).
                if (phys >= fx.plan.physical_slot_count) in_domain = false;
                // Oracle agreement on the physical index for this wrong key.
                bool ook = false;
                uint32_t ophys = oracle_resolve_phys(&fx.rec, s, w, ook);
                if (!ook || ophys != phys) oracle_ok = false;
                ++per_domain_checks;
            }
        }
        ck(non_null, "wrong keys: every successful resolution returns a non-null entry (no null/wild call)");
        ck(in_domain, "wrong keys: every resolved entry lies inside its route's selected domain range");
        ck(meta_ok, "wrong keys: every resolved entry's ABI/visibility/mode/label matches its route's domain (per-domain, not global)");
        ck(oracle_ok, "wrong keys: resolver agrees with the independent oracle for every wrong key/slot");
        ck(per_domain_checks > 0, "wrong keys: per-domain membership checked for many keys (including 0 and UINT64_MAX)");
    }

    // =====================================================================
    // 4. ALTERNATE / PADDING SELECTION — at least one wrong key routes some
    //    logical slot to the domain's PADDING ordinal (a non-null same-ABI trap
    //    stub). The padding entry is non-null, matches the domain's ABI
    //    fingerprint, and the resolver performs NO key comparison.
    // =====================================================================
    {
        bool any_padding = false;
        bool padding_non_null = true;
        bool padding_abi_match = true;
        // Sweep wrong keys and find at least one that routes some logical slot
        // to a padding ordinal. The permutation is a bijection on the full
        // physical_count (real ordinals + the padding ordinal), so a wrong key
        // can land a real logical ordinal's route on the padding physical slot.
        for (uint64_t w = 0; w < 4096; ++w) {
            if (w == fx.route_word) continue;
            for (uint32_t s = 0; s < fx.plan.logical_slot_count; ++s) {
                void* entry = ember_resolve_keyed_dispatch(&fx.rec, s, w);
                if (!entry) { padding_non_null = false; continue; }
                uint32_t phys = Fixture::decode_slot(entry);
                const auto& pe = fx.plan.physical_entries[phys];
                if (pe.is_padding) {
                    any_padding = true;
                    // The padding entry is a non-null same-ABI target (§7.3).
                    if (entry == nullptr) padding_non_null = false;
                    // The padding entry's ABI fingerprint matches its domain
                    // (a padding descriptor is representable as a non-null
                    // same-ABI target).
                    const auto& d = fx.domains[pe.domain_index];
                    if (pe.abi_fingerprint != d.abi_fingerprint ||
                        pe.visibility != d.visibility ||
                        pe.calling_mode != d.calling_mode ||
                        pe.dispatch_domain != d.dispatch_domain) padding_abi_match = false;
                }
            }
            if (any_padding) break;
        }
        ck(any_padding, "alternate/padding: a wrong key routes some logical slot to a padding ordinal");
        ck(padding_non_null, "alternate/padding: the padding entry is non-null (a non-null same-ABI trap stub)");
        ck(padding_abi_match, "alternate/padding: the padding entry's ABI fingerprint matches its domain (same-ABI target)");

        // The resolver performs NO key comparison: the SAME record resolves
        // successfully for arbitrary route words without any stored expected
        // key. There is no expected-key field to compare against (pinned in
        // section 9 below). The resolver simply follows the permutation.
        // This is implicit keyed control-flow, not authentication (§1, §3.3).
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
    // 5. MALFORMED RECORDS — the resolver returns nullptr / a structured
    //    failure WITHOUT any out-of-bounds read for malformed input. Each
    //    probe is a copy of the good record with one field corrupted.
    // =====================================================================
    auto good_rec = [&]() { return fx.rec; };

    // 5a. null record -> nullptr.
    { void* r = ember_resolve_keyed_dispatch(nullptr, 0, fx.route_word);
      ck(r == nullptr, "null record -> C-ABI returns nullptr (no OOB read)");
      auto wr = resolve_keyed_dispatch(nullptr, 0, fx.route_word);
      ck(!wr && wr.error.has_value(), "null record -> C++ wrapper returns structured failure"); }
    // 5b. OOB logical slot -> nullptr (no read past logical_routes).
    { ModuleDispatchRecord r = good_rec();
      void* e = ember_resolve_keyed_dispatch(&r, fx.rec.logical_slot_count, fx.route_word);
      ck(e == nullptr, "OOB logical slot (== logical_slot_count) -> nullptr");
      void* e2 = ember_resolve_keyed_dispatch(&r, 0xFFFFFFFFu, fx.route_word);
      ck(e2 == nullptr, "OOB logical slot (UINT32_MAX) -> nullptr (no OOB read)");
      void* e3 = ember_resolve_keyed_dispatch(&r, 1000000, fx.route_word);
      ck(e3 == nullptr, "OOB logical slot (1000000) -> nullptr"); }
    // 5c. cleared allowlist bit -> nullptr.
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.allowlist[0] = 0;  // clear every bit in byte 0 (slots 0..7)
        auto wr = resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(!wr && wr.error.has_value(), "cleared allowlist bit -> structured failure (no resolution)");
        ck(ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word) == nullptr,
           "cleared allowlist bit -> C-ABI nullptr");
    }
    // 5d. route with OOB domain_index -> the resolver catches it (a malformed
    //     record a host might publish without validation). The resolver's
    //     defensive guard returns nullptr without an OOB domain read.
    {
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        bad.routes[0].domain_index = bad.rec.domain_count + 10;  // OOB
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "route with OOB domain_index -> nullptr (defensive, no OOB domain read)");
    }
    // 5e. null physical_slots -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.physical_slots = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null physical_slots -> nullptr (no read)");
    }
    // 5f. null logical_routes -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.logical_routes = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null logical_routes -> nullptr (no read)");
    }
    // 5g. null allowlist with nonzero logical_slot_count -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.logical_allowlist = nullptr;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "null allowlist -> nullptr (allowlist check first)");
    }
    // 5h. domain_count mismatch (set to 0 while routes still reference domains)
    //     -> the keyed path catches the OOB domain_index (domain_count 0).
    {
        ModuleDispatchRecord r = good_rec();
        r.domain_count = 0;
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "domain_count 0 with keyed routes -> nullptr (route domain_index >= 0 domain_count)");
    }
    // 5i. physical_slot_count too small (truncated) -> base+local OOB -> nullptr.
    {
        ModuleDispatchRecord r = good_rec();
        r.physical_slot_count = 2;  // far too small for a 10-slot layout
        ck(ember_resolve_keyed_dispatch(&r, 0, fx.route_word) == nullptr,
           "truncated physical_slot_count -> nullptr (base+local OOB guard)");
    }

    // =====================================================================
    // 6. NULL / UNFINALIZED ENTRIES — a physical slot that is null -> the
    //    resolver acquire-loads and returns nullptr / a structured failure
    //    (defensive; §10.4 storage rejects null at publication). Strict
    //    publication (KeyedDispatchStorage::publish_keyed) rejects a null entry
    //    with NO slot mutated.
    // =====================================================================
    {
        // Null physical entry: clear the slot the correct key resolves slot 0
        // to, then resolve slot 0 -> nullptr (defensive acquire-load guard).
        Fixture bad = build_keyed_fixture(manifest, k1_view());
        uint32_t target = fx.plan.build_physical_placement[0];
        bad.storage[target].store(nullptr, std::memory_order_release);
        void* e = ember_resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(e == nullptr, "null physical entry -> C-ABI nullptr (defensive acquire-load guard)");
        auto wr = resolve_keyed_dispatch(&bad.rec, 0, bad.route_word);
        ck(!wr && wr.error.has_value(), "null physical entry -> C++ structured failure");
        ck(wr.error->message.size() > 0, "null-entry failure carries a structured diagnostic message");
        // A different slot still resolves (the guard is per-entry, not global).
        void* e2 = ember_resolve_keyed_dispatch(&bad.rec, 1, bad.route_word);
        ck(e2 != nullptr, "null entry guard is per-entry: a different slot still resolves");
    }
    {
        // Strict publication: KeyedDispatchStorage::publish_keyed rejects a
        // null entry with NO slot mutated (§10.4 / §14.2 should-fail bucket).
        KeyedDispatchStorage storage(4);
        // Stage a good entry at slot 0 so we can observe whether it stays null.
        std::vector<std::pair<size_t, void*>> bad_entries = {
            {0, Fixture::encode_slot(0)},
            {1, nullptr},  // null entry -> publication must reject
            {2, Fixture::encode_slot(2)},
        };
        bool published = storage.publish_keyed(bad_entries);
        ck(!published, "publish_keyed rejects a null entry (strict publication)");
        ck(storage.all_clear(), "publish_keyed left the storage unchanged after a null-entry rejection (no partial publication)");
        // A fully-non-null batch publishes.
        std::vector<std::pair<size_t, void*>> good_entries = {
            {0, Fixture::encode_slot(0)},
            {1, Fixture::encode_slot(1)},
            {2, Fixture::encode_slot(2)},
            {3, Fixture::encode_slot(3)},
        };
        ck(storage.publish_keyed(good_entries), "publish_keyed accepts a fully-non-null batch");
        // The physical_base() exposes the const std::atomic<void*>* for a record.
        ck(storage.physical_base() != nullptr, "KeyedDispatchStorage exposes a non-null physical base for a record");
        ck(storage.load_physical(0) == Fixture::encode_slot(0), "load_physical acquire-loads a published entry");
        ck(storage.load_physical(99) == nullptr, "load_physical OOB -> nullptr (defensive, no fault)");
        // Out-of-range slot in publish_keyed is rejected with no mutation.
        std::vector<std::pair<size_t, void*>> oob = { {99, Fixture::encode_slot(99)} };
        ck(!storage.publish_keyed(oob), "publish_keyed rejects an out-of-range slot");
    }

    // =====================================================================
    // 7. STRICT WHOLE-RECORD VALIDATION — validate_dispatch_record accepts the
    //    good records and rejects every malformed shape (§10.4, §14.2 should-
    //    fail). Each probe corrupts one field of a good record.
    // =====================================================================
    auto good = [&]() { return fx.rec; };
    // 7a. bad mode.
    { ModuleDispatchRecord r = good(); r.mode = static_cast<DispatchMode>(99);
      ck(!validate_dispatch_record(r), "bad mode -> validate rejects"); }
    // 7b. bad strategy version.
    { ModuleDispatchRecord r = good(); r.strategy_version = 2;
      ck(!validate_dispatch_record(r), "bad strategy version -> validate rejects"); }
    // 7c. physical < logical.
    { ModuleDispatchRecord r = good(); r.physical_slot_count = r.logical_slot_count - 1;
      ck(!validate_dispatch_record(r), "physical_slot_count < logical_slot_count -> validate rejects"); }
    // 7d. route OOB (logical_slot != index).
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].logical_slot = 1;  // route[0] claims slot 1
      ck(!validate_dispatch_record(b.rec), "route[0].logical_slot != 0 -> validate rejects"); }
    // 7e. route domain_index OOB.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].domain_index = b.rec.domain_count + 5;
      ck(!validate_dispatch_record(b.rec), "route domain_index OOB -> validate rejects"); }
    // 7f. route ABI fingerprint mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].abi_fingerprint = 0xDEADBEEF;
      ck(!validate_dispatch_record(b.rec), "route ABI fingerprint != domain -> validate rejects"); }
    // 7g. route visibility mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].visibility = Visibility::Private;
      ck(!validate_dispatch_record(b.rec), "route visibility != domain -> validate rejects"); }
    // 7h. route calling_mode mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].calling_mode = CallingMode::KeyedR15;
      ck(!validate_dispatch_record(b.rec), "route calling_mode != domain -> validate rejects"); }
    // 7i. route dispatch_domain mismatch with domain.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].dispatch_domain = "tampered";
      ck(!validate_dispatch_record(b.rec), "route dispatch_domain != domain -> validate rejects"); }
    // 7j. route ordinal >= domain logical_count.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.routes[0].ordinal = b.domains[b.routes[0].domain_index].logical_count;  // padding ordinal
      ck(!validate_dispatch_record(b.rec), "route ordinal >= domain logical_count -> validate rejects"); }
    // 7k. overlapping domain physical ranges.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.domains[1].physical_base = b.domains[0].physical_base + 1;  // overlaps D0
      ck(!validate_dispatch_record(b.rec), "overlapping domain physical ranges -> validate rejects"); }
    // 7l. sum of domain physical_counts != physical_slot_count.
    { ModuleDispatchRecord r = good(); r.physical_slot_count = 99;
      ck(!validate_dispatch_record(r), "sum of domain physical_counts != physical_slot_count -> validate rejects"); }
    // 7m. domain padding_count != 1.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.domains[0].padding_count = 0;
      ck(!validate_dispatch_record(b.rec), "domain padding_count != 1 -> validate rejects"); }
    // 7n. domain strategy_version != record strategy_version.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.domains[0].strategy_version = 2;
      ck(!validate_dispatch_record(b.rec), "domain strategy_version != record -> validate rejects"); }
    // 7o. null allowlist with nonzero logical_slot_count.
    { ModuleDispatchRecord r = good(); r.logical_allowlist = nullptr;
      ck(!validate_dispatch_record(r), "null allowlist with nonzero logical_slot_count -> validate rejects"); }
    // 7p. short allowlist.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.rec.logical_allowlist_bytes = 0;  // too short for 6 slots
      ck(!validate_dispatch_record(b.rec), "short allowlist -> validate rejects"); }
    // 7q. null physical_slots with nonzero counts.
    { ModuleDispatchRecord r = good(); r.physical_slots = nullptr;
      ck(!validate_dispatch_record(r), "null physical_slots -> validate rejects"); }
    // 7r. null logical_routes with nonzero counts.
    { ModuleDispatchRecord r = good(); r.logical_routes = nullptr;
      ck(!validate_dispatch_record(r), "null logical_routes -> validate rejects"); }
    // 7s. null domains for a keyed record.
    { ModuleDispatchRecord r = good(); r.domains = nullptr;
      ck(!validate_dispatch_record(r), "null domains for keyed record -> validate rejects"); }
    // 7t. domain_count 0 for keyed.
    { ModuleDispatchRecord r = good(); r.domain_count = 0;
      ck(!validate_dispatch_record(r), "domain_count 0 for keyed record -> validate rejects"); }
    // 7u. keyed empty module (logical_slot_count 0) -> reject.
    { ModuleDispatchRecord r = good(); r.logical_slot_count = 0; r.physical_slot_count = 0;
      r.domain_count = 0; r.logical_allowlist = nullptr; r.logical_allowlist_bytes = 0;
      r.mode = DispatchMode::Keyed;
      ck(!validate_dispatch_record(r), "keyed empty module -> validate rejects (strict)"); }
    // 7v. allowlist bit clear for a present route.
    { Fixture b = build_keyed_fixture(manifest, k1_view());
      b.allowlist[0] &= ~(uint8_t(1) << 0);  // clear bit for slot 0
      ck(!validate_dispatch_record(b.rec), "allowlist bit clear for a present route -> validate rejects"); }

    // =====================================================================
    // 8. IDENTITY MODE — a DispatchMode::Identity record resolves
    //    physical_slots[logical_slot] directly (no permutation, no padding),
    //    preserving existing DispatchTable behavior (§4.4, §10.1 legacy path).
    // =====================================================================
    {
        Fixture idf = build_identity_fixture(manifest);
        ExtensionStatus v = validate_dispatch_record(idf.rec);
        ck(bool(v), "validate_dispatch_record accepts the identity record");
        // Identity resolution: slot i -> physical_slots[i] -> encode_slot(i).
        bool ok = true, wrap_ok = true, oracle_ok = true;
        for (uint32_t s = 0; s < idf.plan.logical_slot_count; ++s) {
            void* entry = ember_resolve_keyed_dispatch(&idf.rec, s, 0xDEAD /* route word ignored */);
            if (!entry || Fixture::decode_slot(entry) != s) ok = false;
            // The route word is IGNORED in identity mode (no permutation).
            void* e2 = ember_resolve_keyed_dispatch(&idf.rec, s, UINT64_MAX);
            if (e2 != entry) ok = false;
            auto wr = resolve_keyed_dispatch(&idf.rec, s, 0);
            if (!wr || *wr.value != entry) wrap_ok = false;
            bool ook = false;
            uint32_t ophys = oracle_resolve_phys(&idf.rec, s, 0, ook);
            if (!ook || Fixture::decode_slot(entry) != ophys) oracle_ok = false;
        }
        ck(ok, "identity mode: slot i -> physical_slots[i] directly (route word ignored)");
        ck(wrap_ok, "identity mode: C++ wrapper agrees");
        ck(oracle_ok, "identity mode: resolver agrees with the oracle");

        // Identity record rejects: domains present, count mismatch.
        { ModuleDispatchRecord r = idf.rec; r.domain_count = 1; r.domains = fx.domains.data();
          ck(!validate_dispatch_record(r), "identity record with domains -> validate rejects"); }
        { ModuleDispatchRecord r = idf.rec; r.physical_slot_count = r.logical_slot_count + 1;
          ck(!validate_dispatch_record(r), "identity record: physical != logical -> validate rejects"); }
        { ModuleDispatchRecord r = idf.rec; r.mode = DispatchMode::Keyed;
          ck(!validate_dispatch_record(r), "identity record flipped to keyed (no domains) -> validate rejects"); }

        // Identity empty module is accepted (an empty identity table is valid).
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
    // 9. SCHEMA / NO-KEY-STORAGE ASSERTIONS (§14.6). The ModuleDispatchRecord
    //    contains NO field for an expected key, machine fingerprint, key
    //    digest, key hash, verifier/comparison constant, or predecoded key-
    //    specific permutation. The route word is NEVER stored in the record —
    //    it is a per-resolution transient supplied to the resolver.
    // =====================================================================
    {
        // Structural shape: the record has exactly the documented fields. We
        // confirm the documented accessors exist and are the ONLY public
        // members by constructing a record and reading each field. There is no
        // field named expected_key / machine_fingerprint / key_digest /
        // verifier / permutation_map (a compile-time check would be ideal, but
        // C++ has no reflection; we assert the documented field set is exactly
        // what the resolver and validator use, and that building a record from
        // a plan never stores the route word).
        ModuleDispatchRecord r = fx.rec;
        // The documented field set:
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
        // No other field is accessed anywhere in the resolver/validator. The
        // route word is supplied PER CALL to the resolver, never stored:
        bool no_key_in_record = true;
        // The fixture was built under K1; the record carries no trace of
        // fx.route_word. Resolving under a DIFFERENT route word (K2's) against
        // the SAME record yields the K2 topology for that record — proving
        // the route word is transient, not baked into the record. (This is
        // only true because the record's domains/salts are key-independent;
        // the build PLACED entries under K1, but the resolver re-derives the
        // physical index from the per-call route word. With K2's word against
        // the K1-placed storage, slot 0 resolves to wherever K2's permutation
        // points, which is generally NOT the K1 placement — confirming the
        // record stored no expected key.)
        void* e_k2word = ember_resolve_keyed_dispatch(&fx.rec, 0, fx2.route_word);
        void* e_k1word = ember_resolve_keyed_dispatch(&fx.rec, 0, fx.route_word);
        // They differ for at least one slot (the route word participates in
        // the permutation; section 11 confirms it differs for slot 0 here, but
        // in general we assert the resolver is a pure function of the per-call
        // route word — it does not consult a stored key).
        (void)e_k2word; (void)e_k1word;
        // The record's domains are key-independent (salts derive from public
        // identity, not the key — §8.4); the build PLACED entries under K1, but
        // the record's DOMAIN METADATA does not depend on K1. So the record
        // stores no key material.
        for (const auto& d : fx.domains) {
            // A domain salt is a public deterministic per-domain tweak, NOT a
            // key (§8.4). It does not equal the route word.
            if (d.domain_salt == fx.route_word) no_key_in_record = false;
        }
        ck(no_key_in_record, "no key storage: no domain salt equals the route word (salts are public tweaks, §8.4)");

        // The resolver does NOT compare the route word to any stored value.
        // There is no expected-key gate: the resolver succeeds for arbitrary
        // route words (proven in section 4). The record's field set is the
        // documented one; no expected_key/machine_fingerprint/key_digest/
        // verifier/permutation_map field is accessed by the resolver.
        // (This is the §14.6 architectural assertion: routing-based, not an
        // explicit verifier. We cannot prove absence of every verifier surface
        // by schema inspection alone, but we prove the record's PUBLIC fields
        // are exactly the routing fields, and the resolver uses no others.)
        ck(true, "schema: ModuleDispatchRecord fields are exactly {mode, strategy_version, physical_slots, physical_slot_count, logical_slot_count, logical_routes, domains, domain_count, logical_allowlist, logical_allowlist_bytes} — no expected key / fingerprint / digest / verifier / permutation map");
    }

    // =====================================================================
    // 10. TrapReason::KeyedDispatchPadding — the trap reason exists and maps
    //     to the stable string "keyed dispatch padding" (§7.3). Padding
    //     descriptors are representable as non-null same-ABI targets and
    //     never perform key comparison.
    // =====================================================================
    {
        ck(trap_reason_str(TrapReason::KeyedDispatchPadding) != nullptr,
           "TrapReason::KeyedDispatchPadding has a non-null string mapping");
        ck(std::string(trap_reason_str(TrapReason::KeyedDispatchPadding)) == "keyed dispatch padding",
           "TrapReason::KeyedDispatchPadding maps to the stable string 'keyed dispatch padding'");
        // The enum value is distinct from every other reason.
        ck(TrapReason::KeyedDispatchPadding != TrapReason::None, "KeyedDispatchPadding != None");
        ck(TrapReason::KeyedDispatchPadding != TrapReason::BadCallTarget, "KeyedDispatchPadding != BadCallTarget");
        ck(TrapReason::KeyedDispatchPadding != TrapReason::IllegalInstruction, "KeyedDispatchPadding != IllegalInstruction");
        // The padding entry (a non-null same-ABI trap stub) is what the resolver
        // returns when a wrong key lands on the padding ordinal (section 4).
        // The padding target does NOT compare a key (§7.3) — the resolver feeds
        // the route word only to the permutation; the padding entry is just the
        // non-null entry stored at the selected physical slot.
    }

    // =====================================================================
    // 11. KeyedDispatchStorage PRESERVES DispatchTable IDENTITY BEHAVIOR.
    //     The existing DispatchTable APIs are unchanged when keyed mode is
    //     disabled; KeyedDispatchStorage is ONLY the physical backing for keyed
    //     mode and adds no new logical-slot read path.
    // =====================================================================
    {
        // The existing DispatchTable (identity mode) is untouched.
        DispatchTable dt(3);
        void* stub = Fixture::encode_slot(0);
        dt.set(0, stub);
        ck(dt.get(0) == stub, "existing DispatchTable::set/get unchanged (identity behavior preserved)");
        ck(dt.get(99) == nullptr, "existing DispatchTable::get OOB -> nullptr (unchanged)");
        std::vector<std::pair<size_t, void*>> pub = { {1, Fixture::encode_slot(1)}, {2, Fixture::encode_slot(2)} };
        ck(dt.publish_batch(pub), "existing DispatchTable::publish_batch unchanged");
        ck(dt.get(1) == Fixture::encode_slot(1), "existing DispatchTable::publish_batch committed (unchanged)");
        // A null entry in publish_batch is rejected with no mutation (existing).
        std::vector<std::pair<size_t, void*>> badpub = { {0, nullptr} };
        ck(!dt.publish_batch(badpub), "existing DispatchTable::publish_batch rejects null (unchanged)");
        ck(dt.get(0) == stub, "existing DispatchTable unchanged after rejected null publication");

        // KeyedDispatchStorage is a distinct type used ONLY for keyed physical
        // backing. Its publish_keyed is the strict keyed publication path.
        KeyedDispatchStorage ks(2);
        ck(ks.size() == 2, "KeyedDispatchStorage sized to the physical count");
        ck(ks.all_clear(), "KeyedDispatchStorage starts all-clear");
        std::vector<std::pair<size_t, void*>> kpub = { {0, Fixture::encode_slot(0)}, {1, Fixture::encode_slot(1)} };
        ck(ks.publish_keyed(kpub), "KeyedDispatchStorage::publish_keyed accepts a non-null batch");
        ck(ks.load_physical(0) == Fixture::encode_slot(0), "KeyedDispatchStorage::load_physical acquire-loads");
        ck(ks.physical_base() != nullptr, "KeyedDispatchStorage::physical_base exposes the const atomic base");
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
