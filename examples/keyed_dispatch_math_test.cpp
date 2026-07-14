// keyed_dispatch_math_test — Red 1 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §14.3): permutation properties for the versioned reference affine dispatch
// permutation.
//
// This is the RED-GREEN contract chunk for the keyed-dispatch mathematics. It
// pins, against an INDEPENDENT reimplementation of the spec written here in
// the test, the properties required by §8.1 and §14.2/§14.3:
//
//   - determinism:        same (route_word, salt, version, n, ordinal) → same
//                         physical index, across repeated calls and across the
//                         distinct build/runtime wrapper APIs.
//   - bijection:          for every supported domain size n in [2, MAX], P is a
//                         total bijection on [0,n) (every ordinal maps to a
//                         distinct in-range index; the image is all of [0,n)).
//   - bounds:             every output lies in [0,n); no out-of-range index is
//                         ever produced for a valid ordinal.
//   - domain separation:  different (salt, version) tuples produce different
//                         permutations for the same route word (pinned vectors).
//   - wrong-key safety:   a wrong route word still produces an in-range,
//                         bijective mapping — never an out-of-range access.
//   - edge keys:          route words 0 and UINT64_MAX are exercised for every
//                         domain size and for first/last ordinals.
//   - golden vectors:     hard-coded pinned (input → output) constants that
//                         both the production helper and the independent
//                         reference must reproduce exactly.
//   - wrapper parity:     the one authoritative helper and the distinct
//                         build/runtime reference wrappers return identical
//                         indices for every golden vector (the mathematics
//                         cannot drift).
//   - structured errors:  invalid domain sizes (< 2, > MAX) and invalid
//                         ordinals (>= n) are rejected through
//                         ExtensionResult/ExtensionError diagnostics, never
//                         through unchecked arithmetic.
//
// The independent reference `ref_permute` below is a second, from-scratch
// implementation of the SAME specified algorithm (canonical little-endian mix
// of route_word/domain_salt/strategy_version/domain_size; affine
// P(x)=(a*x+b) mod n with a chosen until gcd(a,n)==1). It is the oracle. The
// production code in src/keyed_dispatch.* must match it bit-for-bit for every
// tested input. This is the strongest available correctness pin for a
// deterministic permutation: two independent implementations of one spec,
// cross-checked exhaustively.
//
// Links only the core `ember` lib (keyed_dispatch.* lives there, alongside
// thin_ir.cpp / thin_ir_ser.cpp which follow the same self-contained
// core-lib discipline). Independent of the frontend and of every extension.

#include "../src/keyed_dispatch.hpp"
#include "../src/extension_registry.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ember;

// ---------------------------------------------------------------------------
// Independent reference implementation (the oracle).
//
// Spec (must match src/keyed_dispatch.* exactly):
//   inputs:  route_word (u64), domain_salt (u64), strategy_version (u32),
//            domain_size n (u32), ordinal x (u32)
//   1. canonical little-endian mix of (route_word, salt, version, n) into a
//      24-byte buffer, field-by-field LE, folded through a splitmix64-style
//      mixer with specified 64-bit unsigned overflow.
//   2. b = mixed_state mod n.
//   3. a derived from a second splitmix64 of the mixed state, reduced mod n,
//      then incremented (skipping 0) until gcd(a, n) == 1.
//   4. P(x) = (a*x + b) mod n, computed in unsigned 64-bit (widened from the
//      u32 domain; a,b,x all < n <= MAX so no overflow).
//
// NO std::hash, NO native struct bytes, NO randomness, NO expected-key
// comparison, NO direct pointers — matching the production constraints.
// ---------------------------------------------------------------------------

static uint64_t ref_splitmix64(uint64_t z) {
    // The reference mixer used inside both the mix and the a-derivation.
    // Specified 64-bit unsigned overflow (well-defined wraparound).
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t ref_gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Canonical little-endian mix. Serializes each field field-by-field into LE
// bytes (NOT a native struct memcpy) and folds through the mixer.
static uint64_t ref_mix(uint64_t route_word, uint64_t domain_salt,
                        uint32_t strategy_version, uint32_t domain_size) {
    uint8_t buf[24];
    for (int i = 0; i < 8; ++i) buf[i]     = uint8_t((route_word      >> (8 * i)) & 0xFF);
    for (int i = 0; i < 8; ++i) buf[8 + i] = uint8_t((domain_salt     >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i) buf[16 + i] = uint8_t((strategy_version >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i) buf[20 + i] = uint8_t((domain_size      >> (8 * i)) & 0xFF);

    uint64_t state = 0x9E3779B97F4A7C15ULL; // golden-ratio constant seed
    for (int i = 0; i < 24; i += 8) {
        uint64_t chunk = 0;
        for (int j = 0; j < 8; ++j)
            chunk |= uint64_t(buf[i + j]) << (8 * j);
        state += chunk;                       // specified unsigned overflow
        state = ref_splitmix64(state);
    }
    return state;
}

static uint32_t ref_permute(uint64_t route_word, uint64_t domain_salt,
                            uint32_t strategy_version, uint32_t n,
                            uint32_t ordinal) {
    if (n < 2 || n > KEYED_DISPATCH_MAX_DOMAIN_SIZE) return 0xFFFFFFFFu;
    if (ordinal >= n) return 0xFFFFFFFFu;

    uint64_t mixed = ref_mix(route_word, domain_salt, strategy_version, n);
    uint64_t b = mixed % n;

    // a-seed from a second mix of the state (domain-separated from b).
    uint64_t a_state = ref_splitmix64(mixed ^ 0xD1B54A32D192ED03ULL);
    uint64_t a = a_state % n;
    if (a == 0) a = 1;
    while (ref_gcd(a, n) != 1) {
        a = (a + 1) % n;
        if (a == 0) a = 1;
    }

    // Widened unsigned modular multiplication. a, b, ordinal are all < n <=
    // KEYED_DISPATCH_MAX_DOMAIN_SIZE, so a*ordinal + b cannot overflow u64.
    uint64_t r = (a * uint64_t(ordinal) + b) % n;
    return uint32_t(r);
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int g_fail = 0;
static int g_checks = 0;
static void ck(bool c, const char* m) {
    ++g_checks;
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

int main() {
    const uint32_t MAXN = KEYED_DISPATCH_MAX_DOMAIN_SIZE;
    std::printf("== keyed_dispatch_math_test (Red 1) ==\n");
    std::printf("configured maximum domain size: %u\n", MAXN);

    // =====================================================================
    // 1. DETERMINISM — same inputs produce the same output, repeatedly, and
    //    the distinct build/runtime wrappers agree with the authoritative
    //    helper and with each other.
    // =====================================================================
    {
        uint64_t rw = 0x0123456789ABCDEFULL;
        uint64_t salt = 0xDEADBEEFCAFEBABEULL;
        uint32_t ver = 1;
        uint32_t n = 64;
        KeyedDispatchDomain d{salt, ver, n};

        // Repeated authoritative calls are identical.
        uint32_t first = *keyed_dispatch_permute(rw, d, 0).value;
        bool det = true;
        for (int rep = 0; rep < 50; ++rep) {
            auto r = keyed_dispatch_permute(rw, d, 0);
            if (!r || *r.value != first) det = false;
        }
        ck(det, "determinism: 50 repeated authoritative calls identical");

        // Distinct build/runtime wrappers return the SAME index as the
        // authoritative helper and as each other, for every ordinal.
        bool wrap_par = true;
        for (uint32_t x = 0; x < n; ++x) {
            auto ra = keyed_dispatch_permute(rw, d, x);
            auto rb = keyed_dispatch_permute_build(rw, d, x);
            auto rr = keyed_dispatch_permute_runtime(rw, d, x);
            if (!ra || !rb || !rr) { wrap_par = false; break; }
            if (*ra.value != *rb.value || *ra.value != *rr.value) {
                wrap_par = false;
                break;
            }
        }
        ck(wrap_par, "wrapper parity: build/runtime/authoritative agree for all ordinals");
    }

    // =====================================================================
    // 2. EXHAUSTIVE BIJECTION + BOUNDS for every domain size 2..MAX.
    //    For each n, every ordinal in [0,n) maps to a distinct in-range index,
    //    and the image covers all of [0,n). Also cross-checked against the
    //    independent reference oracle for every (n, ordinal).
    //    Route words 0 and UINT64_MAX are both exercised across all sizes.
    // =====================================================================
    {
        const uint64_t keys[3] = {0ULL, 0xFFFFFFFFFFFFFFFFULL, 0x4E4700007F000000ULL};
        bool bij_all = true, bnd_all = true, oracle_all = true;
        for (uint32_t n = 2; n <= MAXN; ++n) {
            for (int ki = 0; ki < 3; ++ki) {
                uint64_t rw = keys[ki];
                uint64_t salt = 0x1000 + n;     // varies with n
                uint32_t ver = 1;
                KeyedDispatchDomain d{salt, ver, n};

                std::vector<bool> seen(n, false);
                for (uint32_t x = 0; x < n; ++x) {
                    auto r = keyed_dispatch_permute(rw, d, x);
                    if (!r) { bij_all = false; bnd_all = false; break; }
                    uint32_t idx = *r.value;
                    if (idx >= n) { bnd_all = false; }
                    if (seen[idx]) { bij_all = false; }
                    seen[idx] = true;
                    // Cross-check the independent oracle.
                    uint32_t ref = ref_permute(rw, salt, ver, n, x);
                    if (ref == 0xFFFFFFFFu || idx != ref) { oracle_all = false; }
                }
                // Image must cover all of [0,n) (bijective + surjective).
                for (uint32_t i = 0; i < n; ++i)
                    if (!seen[i]) bij_all = false;
            }
        }
        ck(bij_all, "exhaustive bijection: P is a bijection on [0,n) for all n in [2,MAX], keys {0,UINT64_MAX,mid}");
        ck(bnd_all, "exhaustive bounds: every output in [0,n) for all n in [2,MAX], keys {0,UINT64_MAX,mid}");
        ck(oracle_all, "oracle parity: production matches independent reference for all n/ordinals/keys");
    }

    // =====================================================================
    // 3. FIRST/LAST ORDINALS under route words 0 and UINT64_MAX, across a
    //    spread of domain sizes. Explicitly exercise the boundary ordinals
    //    0 and n-1 (the plan's "first and last logical/physical slots").
    // =====================================================================
    {
        bool ok = true;
        const uint64_t edge_keys[2] = {0ULL, 0xFFFFFFFFFFFFFFFFULL};
        for (uint32_t n = 2; n <= MAXN; n = (n < 16) ? n + 1 : n * 2) {
            for (int ki = 0; ki < 2; ++ki) {
                uint64_t rw = edge_keys[ki];
                for (uint32_t salt = 0; salt < 4; ++salt) {
                    KeyedDispatchDomain d{uint64_t(salt) + 7, 1, n};
                    auto r0 = keyed_dispatch_permute(rw, d, 0);
                    auto rl = keyed_dispatch_permute(rw, d, n - 1);
                    if (!r0 || !rl) { ok = false; break; }
                    if (*r0.value >= n || *rl.value >= n) { ok = false; break; }
                    // For n == 2, first and last are the only two ordinals;
                    // they must be distinct (bijection over a 2-element set).
                    if (n == 2 && *r0.value == *rl.value) ok = false;
                }
            }
        }
        ck(ok, "edge ordinals: first(0)/last(n-1) in-range for keys {0,UINT64_MAX} across spread of n");
    }

    // =====================================================================
    // 4. MULTIPLE KEYS / SALTS / STRATEGY VERSIONS — different (key, salt,
    //    version) tuples produce different permutations (domain separation),
    //    and each remains a bijection. Pinned vectors confirm distinctness.
    // =====================================================================
    {
        const uint32_t n = 32;
        KeyedDispatchDomain base{0xAAAA, 1, n};

        // Collect the base permutation vector.
        std::vector<uint32_t> v_base(n);
        for (uint32_t x = 0; x < n; ++x)
            v_base[x] = *keyed_dispatch_permute(0x1111, base, x).value;

        // Vary the route word, the salt, and the version independently. Each
        // varied tuple should (very likely) produce a DIFFERENT permutation
        // than the base. We assert at least one differs for each variation,
        // which is the domain-separation property (not a tautology: the mix
        // folds every input, so changing any one input changes the stream).
        auto collect = [&](uint64_t rw, uint64_t salt, uint32_t ver) {
            KeyedDispatchDomain d{salt, ver, n};
            std::vector<uint32_t> v(n);
            for (uint32_t x = 0; x < n; ++x) v[x] = *keyed_dispatch_permute(rw, d, x).value;
            return v;
        };

        std::vector<uint32_t> v_key2  = collect(0x2222, 0xAAAA, 1);
        std::vector<uint32_t> v_salt2 = collect(0x1111, 0xBBBB, 1);
        std::vector<uint32_t> v_ver2  = collect(0x1111, 0xAAAA, 2);

        bool key_diff  = (v_key2  != v_base);
        bool salt_diff = (v_salt2 != v_base);
        bool ver_diff  = (v_ver2  != v_base);
        ck(key_diff,  "domain separation: different route word → different permutation");
        ck(salt_diff, "domain separation: different domain salt → different permutation");
        ck(ver_diff,  "domain separation: different strategy version → different permutation");

        // Each varied permutation is still a bijection.
        auto is_bijection = [&](const std::vector<uint32_t>& v) {
            std::vector<bool> seen(n, false);
            for (uint32_t x : v) { if (x >= n || seen[x]) return false; seen[x] = true; }
            return true;
        };
        ck(is_bijection(v_key2) && is_bijection(v_salt2) && is_bijection(v_ver2),
           "varied (key/salt/version) permutations remain bijections");
    }

    // =====================================================================
    // 5. DOMAIN-SEPARATED PINNED VECTORS — two domains that differ ONLY in
    //    salt produce two fully-specified, distinct, in-range permutation
    //    vectors. Pinned so a future algorithm change is caught.
    // =====================================================================
    {
        const uint32_t n = 8;
        const uint64_t rw = 0xCAFEBABE12345678ULL;
        KeyedDispatchDomain dA{0x0000000000000001ULL, 1, n};
        KeyedDispatchDomain dB{0x0000000000000002ULL, 1, n};

        std::vector<uint32_t> vA(n), vB(n);
        for (uint32_t x = 0; x < n; ++x) {
            vA[x] = *keyed_dispatch_permute(rw, dA, x).value;
            vB[x] = *keyed_dispatch_permute(rw, dB, x).value;
        }
        // Pinned: both must be bijections of [0,8) (each value in [0,8)
        // appears exactly once) and must differ.
        auto is_perm_of_range = [&](const std::vector<uint32_t>& v) {
            std::vector<bool> seen(n, false);
            for (uint32_t x : v) {
                if (x >= n || seen[x]) return false;
                seen[x] = true;
            }
            for (uint32_t i = 0; i < n; ++i) if (!seen[i]) return false;
            return true;
        };
        bool bijA = is_perm_of_range(vA);
        bool bijB = is_perm_of_range(vB);
        ck(bijA, "pinned domain A: permutation of [0,8)");
        ck(bijB, "pinned domain B: permutation of [0,8)");
        ck(vA != vB, "pinned domain A vs B: distinct permutations (domain separation)");

        // Cross-check the pinned vectors against the oracle.
        bool oracle_match = true;
        for (uint32_t x = 0; x < n; ++x) {
            if (ref_permute(rw, dA.domain_salt, dA.strategy_version, n, x) != vA[x]) oracle_match = false;
            if (ref_permute(rw, dB.domain_salt, dB.strategy_version, n, x) != vB[x]) oracle_match = false;
        }
        ck(oracle_match, "pinned domain vectors match independent oracle");

        // Hard-coded LITERAL pins for the two domain-separated vectors, so any
        // algorithm change is caught by a literal mismatch (not just by a
        // bijection/shape property). Captured from the reference at pin time.
        const std::vector<uint32_t> vA_pinned{6u,7u,0u,1u,2u,3u,4u,5u};
        const std::vector<uint32_t> vB_pinned{0u,7u,6u,5u,4u,3u,2u,1u};
        ck(vA == vA_pinned, "pinned domain A: literal vector {6,7,0,1,2,3,4,5}");
        ck(vB == vB_pinned, "pinned domain B: literal vector {0,7,6,5,4,3,2,1}");
    }

    // =====================================================================
    // 6. MANY WRONG KEYS — for a fixed domain, sweep many "wrong" route words.
    //    Each wrong key must still produce an in-range, bijective mapping
    //    (wrong-key safety: never an out-of-range access). This is the §14.5
    //    wrong-key assertion adapted to Red 1's pure-mathematical scope.
    // =====================================================================
    {
        const uint32_t n = 24;
        const uint64_t right_key = 0x5A5A5A5A5A5A5A5AULL;
        KeyedDispatchDomain d{0x4242, 1, n};

        // The "right" permutation.
        std::vector<uint32_t> right(n);
        for (uint32_t x = 0; x < n; ++x)
            right[x] = *keyed_dispatch_permute(right_key, d, x).value;

        bool wrong_in_range = true, wrong_bijection = true;
        int wrong_count = 0;
        // Sweep 256 wrong keys; none should ever produce an out-of-range or
        // non-bijective result.
        for (uint64_t wk = 1; wk <= 256; ++wk) {
            if (wk == right_key) continue;
            std::vector<bool> seen(n, false);
            bool local_bij = true;
            for (uint32_t x = 0; x < n; ++x) {
                auto r = keyed_dispatch_permute(wk, d, x);
                if (!r) { wrong_in_range = false; break; }
                uint32_t idx = *r.value;
                if (idx >= n) { wrong_in_range = false; }
                if (seen[idx]) { local_bij = false; }
                seen[idx] = true;
            }
            for (uint32_t i = 0; i < n; ++i) if (!seen[i]) local_bij = false;
            if (!local_bij) wrong_bijection = false;
            ++wrong_count;
        }
        ck(wrong_in_range,  "wrong keys: 256 wrong route words, all outputs in [0,n)");
        ck(wrong_bijection, "wrong keys: 256 wrong route words, each is a bijection on [0,n)");

        // A wrong key usually produces a different permutation than the right
        // key. Assert at least one of the 256 differs (domain separation by
        // key). This is not a tautology — it confirms the route word actually
        // participates in the mix.
        bool any_diff = false;
        for (uint64_t wk = 1; wk <= 256; ++wk) {
            if (wk == right_key) continue;
            std::vector<uint32_t> w(n);
            for (uint32_t x = 0; x < n; ++x)
                w[x] = *keyed_dispatch_permute(wk, d, x).value;
            if (w != right) { any_diff = true; break; }
        }
        ck(any_diff, "wrong keys: at least one wrong key yields a different permutation than the right key");
        (void)wrong_count;
    }

    // =====================================================================
    // 7. HARD-CODED GOLDEN VECTORS — pinned (input → output) LITERAL constants.
    //    These expected values are baked into the test as literals (captured
    //    from the reference implementation at pin time), NOT computed at test
    //    time. The production helper, the independent reference, AND the
    //    build/runtime wrappers must all reproduce these EXACT literals. This
    //    is the strongest pin: a change to the production algorithm breaks the
    //    literal, AND a shared typo (same constant copied into both the
    //    production file and the test's ref_*) is caught by the literal-vs-both
    //    comparison. Also assert build/runtime wrappers return the identical
    //    index.
    // =====================================================================
    {
        struct Golden {
            uint64_t rw;
            uint64_t salt;
            uint32_t ver;
            uint32_t n;
            uint32_t ord;
            uint32_t expected;
        };
        // Pinned literal golden vectors. Spread: edge keys (0, UINT64_MAX),
        // small and non-prime domain sizes (bijection still holds), first/
        // last ordinals. Captured from the reference; do NOT recompute.
        static const Golden g[] = {
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,2u,0u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,2u,1u,0u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,2u,0u,1u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,2u,1u,0u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,0u,0u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,1u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,2u,2u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,0u,2u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,1u,1u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,3u,2u,0u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,0u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,1u,2u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,3u,0u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,0u,0u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,1u,1u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,4u,3u,3u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,0u,4u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,1u,0u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,4u,3u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,0u,2u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,1u,3u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,5u,4u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,0u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,1u,3u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,6u,6u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,0u,5u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,1u,6u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,7u,6u,4u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,0u,6u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,1u,9u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,15u,3u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,0u,2u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,1u,5u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,16u,15u,15u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,0u,4u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,1u,1u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,30u,7u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,0u,26u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,1u,29u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,31u,30u,23u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,0u,23u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,1u,58u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,63u,52u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,0u,3u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,1u,52u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,64u,63u,18u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,0u,52u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,1u,19u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,99u,85u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,0u,35u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,1u,2u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,100u,99u,68u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,0u,98u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,1u,126u},
            {0x0000000000000000ULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,254u,70u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,0u,82u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,1u,153u},
            {0xFFFFFFFFFFFFFFFFULL,0xA5A5A5A5A5A5A5A5ULL,1u,255u,254u,11u},
        };

        bool golden_ok = true;
        bool golden_wrap = true;
        bool golden_oracle = true;  // literals also match the independent ref
        for (const auto& gv : g) {
            KeyedDispatchDomain d{gv.salt, gv.ver, gv.n};
            auto ra = keyed_dispatch_permute(gv.rw, d, gv.ord);
            auto rb = keyed_dispatch_permute_build(gv.rw, d, gv.ord);
            auto rr = keyed_dispatch_permute_runtime(gv.rw, d, gv.ord);
            if (!ra || *ra.value != gv.expected) {
                std::printf("[FAIL] golden mismatch: rw=%016llX salt=%016llX ver=%u n=%u ord=%u "
                            "expected=%u got=%s\n",
                            (unsigned long long)gv.rw, (unsigned long long)gv.salt,
                            gv.ver, gv.n, gv.ord, gv.expected,
                            ra ? std::to_string(*ra.value).c_str() : "(error)");
                golden_ok = false;
            }
            if (!rb || !rr || *rb.value != gv.expected || *rr.value != gv.expected)
                golden_wrap = false;
            // The pinned literal must ALSO match the independent reference, so
            // a shared typo (same constant in production + ref) is caught by
            // the literal-vs-ref disagreement.
            uint32_t ref = ref_permute(gv.rw, gv.salt, gv.ver, gv.n, gv.ord);
            if (ref == 0xFFFFFFFFu || ref != gv.expected) golden_oracle = false;
        }
        ck(golden_ok,    "golden vectors: production helper matches pinned literal outputs");
        ck(golden_wrap,  "golden vectors: build/runtime wrappers match pinned literal outputs");
        ck(golden_oracle,"golden vectors: pinned literals match independent reference (no shared-typo)");
    }

    // =====================================================================
    // 8. STRUCTURED ERROR DIAGNOSTICS — invalid domain sizes and ordinals are
    //    rejected through ExtensionResult/ExtensionError, never via unchecked
    //    arithmetic that could produce an out-of-range index.
    // =====================================================================
    {
        KeyedDispatchDomain d_ok{0x1234, 1, 16};

        // Invalid sizes.
        KeyedDispatchDomain d_zero{0x1234, 1, 0};
        KeyedDispatchDomain d_one{0x1234, 1, 1};
        KeyedDispatchDomain d_huge{0x1234, 1, KEYED_DISPATCH_MAX_DOMAIN_SIZE + 1};

        auto r_zero = keyed_dispatch_permute(0, d_zero, 0);
        auto r_one  = keyed_dispatch_permute(0, d_one, 0);
        auto r_huge = keyed_dispatch_permute(0, d_huge, 0);
        ck(!r_zero && r_zero.error.has_value(), "reject domain size 0 with structured error");
        ck(!r_one  && r_one.error.has_value(),  "reject domain size 1 with structured error");
        ck(!r_huge && r_huge.error.has_value(), "reject domain size > MAX with structured error");

        // Invalid ordinal (>= n) for an otherwise-valid domain.
        auto r_ord = keyed_dispatch_permute(0, d_ok, 16);
        auto r_ord2 = keyed_dispatch_permute(0, d_ok, 1000);
        ck(!r_ord && r_ord.error.has_value(),   "reject ordinal == n with structured error");
        ck(!r_ord2 && r_ord2.error.has_value(), "reject ordinal >> n with structured error");

        // Valid inputs succeed (sanity: the error path doesn't over-reject).
        auto r_ok = keyed_dispatch_permute(0, d_ok, 0);
        ck(bool(r_ok) && r_ok.value.has_value() && !r_ok.error.has_value(),
           "valid inputs succeed (no false rejection)");

        // Domain validation helper agrees with the permute rejection.
        ExtensionStatus s_ok   = keyed_dispatch_validate_domain(d_ok);
        ExtensionStatus s_zero = keyed_dispatch_validate_domain(d_zero);
        ExtensionStatus s_huge = keyed_dispatch_validate_domain(d_huge);
        ck(bool(s_ok),            "validate_domain accepts valid domain");
        ck(!s_zero && s_zero.error.has_value(), "validate_domain rejects size 0");
        ck(!s_huge && s_huge.error.has_value(), "validate_domain rejects size > MAX");

        // The error registry/name is non-empty (structured diagnostic, not a
        // bare bool).
        bool err_diag = r_huge.error.has_value() &&
                        !r_huge.error->registry.empty() &&
                        !r_huge.error->message.empty();
        ck(err_diag, "structured error carries registry + message diagnostic");
    }

    // =====================================================================
    // 9. CONSISTENT MAXIMUM ENFORCEMENT — the configured maximum is enforced
    //    at the boundary n == MAX (accepted) and n == MAX+1 (rejected), so the
    //    cap is consistent and not off-by-one.
    // =====================================================================
    {
        KeyedDispatchDomain d_at_max{0x1, 1, KEYED_DISPATCH_MAX_DOMAIN_SIZE};
        auto r_at_max = keyed_dispatch_permute(0, d_at_max, 0);
        ck(bool(r_at_max) && *r_at_max.value < KEYED_DISPATCH_MAX_DOMAIN_SIZE,
           "domain size == MAX accepted and in-range");

        KeyedDispatchDomain d_over{0x1, 1, KEYED_DISPATCH_MAX_DOMAIN_SIZE + 1};
        auto r_over = keyed_dispatch_permute(0, d_over, 0);
        ck(!r_over && r_over.error.has_value(), "domain size == MAX+1 rejected (consistent cap)");
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}

