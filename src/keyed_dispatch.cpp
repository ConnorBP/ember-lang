// keyed_dispatch.cpp — versioned reference affine dispatch permutation impl.
//
// Green side of Red 1 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §14.3).
// See keyed_dispatch.hpp for the full contract. This file is the ONE
// implementation of the mathematics; the build/runtime wrappers delegate to
// the authoritative `keyed_dispatch_permute` so build and runtime cannot drift.
//
// The algorithm is specified bit-for-bit and cross-checked against an
// INDEPENDENT reimplementation in examples/keyed_dispatch_math_test.cpp. The
// two must agree for every tested input. If you change anything in this file,
// re-run that test — a mismatch is an algorithm drift bug.
//
// Self-contained: only unsigned integer arithmetic + ExtensionResult/ExtensionError
// (src/extension_registry.hpp). No ember_frontend dependency, so it stays in the
// core `ember` lib. NO std::hash, NO native struct bytes (the mix serializes
// fields field-by-field in little-endian, not a struct memcpy), NO randomness,
// NO expected-key comparison, NO direct pointers.

#include "keyed_dispatch.hpp"

#include <cstdint>

namespace ember {

// ─── Internal: splitmix64-style mixer ────────────────────────────────────
// A specified 64-bit unsigned mixer used inside the canonical mix and the
// a-derivation. 64-bit wraparound is well-defined unsigned overflow. These are
// the standard splitmix64 constants (splitmix64, Sebastiano Vigna); they are
// NOT a secret and NOT a key — they are fixed mixing constants that make the
// stream well-distributed. The route word (the only secret-ish input) is fed
// in by the caller; nothing here is derived from a machine identifier.
static uint64_t splitmix64_step(uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// ─── Internal: binary gcd (Euclid) ───────────────────────────────────────
// Used to test gcd(a, n) == 1 for the coprime search. Pure unsigned arithmetic.
static uint64_t binary_gcd(uint64_t a, uint64_t b) noexcept {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// ─── Internal: canonical little-endian mix ───────────────────────────────
// Serializes (route_word, domain_salt, strategy_version, domain_size) into a
// 24-byte buffer FIELD-BY-FIELD in little-endian order (NOT a native struct
// memcpy — each field is shifted out byte-by-byte so the byte order is
// explicit and independent of host endianness or struct padding), then folds
// the buffer through the mixer with specified 64-bit unsigned overflow.
//
// Every input participates: changing any one of (route_word, salt, version,
// n) changes the mixed state and therefore changes a and b (barring
// astronomically unlikely collisions, which the bijection property makes
// irrelevant — even a colliding b still yields a valid bijection because a is
// always coprime with n).
static uint64_t canonical_mix(uint64_t route_word, uint64_t domain_salt,
                              uint32_t strategy_version, uint32_t domain_size) noexcept {
    uint8_t buf[24];
    for (int i = 0; i < 8; ++i)
        buf[i]      = uint8_t((route_word       >> (8 * i)) & 0xFFu);
    for (int i = 0; i < 8; ++i)
        buf[8 + i]  = uint8_t((domain_salt      >> (8 * i)) & 0xFFu);
    for (int i = 0; i < 4; ++i)
        buf[16 + i] = uint8_t((strategy_version >> (8 * i)) & 0xFFu);
    for (int i = 0; i < 4; ++i)
        buf[20 + i] = uint8_t((domain_size      >> (8 * i)) & 0xFFu);

    // Golden-ratio constant seed (0x9E3779B97F4A7C15). Not a key; a fixed
    // non-zero IV so an all-zero input does not produce an all-zero stream.
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 24; i += 8) {
        uint64_t chunk = 0;
        for (int j = 0; j < 8; ++j)
            chunk |= uint64_t(buf[i + j]) << (8 * j);
        state += chunk;                 // specified unsigned overflow
        state = splitmix64_step(state); // fold each 8-byte chunk
    }
    return state;
}

// ─── Internal: derive affine coefficients a, b ───────────────────────────
// b = mixed mod n  (any value in [0, n) is valid for the affine shift).
// a  = derived from a second splitmix64 of the mixed state (domain-separated
//      from b by the XOR tweak), reduced mod n, then incremented — skipping 0
//      — until gcd(a, n) == 1. Because a = 1 is coprime with every n >= 2,
//      the search always terminates in at most n-1 steps.
//
// a is never 0 (gcd(0, n) = n != 1 for n >= 2), so the permutation is never
// the degenerate constant map P(x) = b.
static void derive_affine(uint64_t mixed, uint32_t n,
                          uint64_t& a_out, uint64_t& b_out) noexcept {
    b_out = mixed % n;

    // Domain-separate the a-seed from b so b and a are not trivially linked.
    uint64_t a_state = splitmix64_step(mixed ^ 0xD1B54A32D192ED03ULL);
    uint64_t a = a_state % n;
    if (a == 0) a = 1;                  // never start at 0
    while (binary_gcd(a, n) != 1) {
        a = (a + 1) % n;
        if (a == 0) a = 1;              // skip 0 (gcd(0,n)=n != 1 for n>=2)
    }
    a_out = a;
}

// ─── Internal: one authoritative evaluation ──────────────────────────────
// Validates, derives a/b, and evaluates P(x) = (a*x + b) mod n. Returns the
// index in [0, n) on success, or sets the out-error on failure. The actual
// ExtensionResult is assembled by the public functions below.
static bool eval_permute(uint64_t route_word, const KeyedDispatchDomain& domain,
                         uint32_t ordinal, uint32_t& out_index,
                         ExtensionError& out_err) noexcept {
    const uint32_t n = domain.domain_size;

    // Reject invalid domain sizes BEFORE any arithmetic.
    if (n < 2) {
        out_err = ExtensionError{"ember-keyed-dispatch", "", "domain size < 2"};
        return false;
    }
    if (n > KEYED_DISPATCH_MAX_DOMAIN_SIZE) {
        out_err = ExtensionError{"ember-keyed-dispatch", "",
            "domain size exceeds KEYED_DISPATCH_MAX_DOMAIN_SIZE"};
        return false;
    }
    // Reject invalid ordinals BEFORE any arithmetic.
    if (ordinal >= n) {
        out_err = ExtensionError{"ember-keyed-dispatch", "",
            "ordinal >= domain size"};
        return false;
    }

    const uint64_t mixed = canonical_mix(route_word, domain.domain_salt,
                                         domain.strategy_version, n);
    uint64_t a = 0, b = 0;
    derive_affine(mixed, n, a, b);

    // Widened unsigned modular multiplication. a, b, ordinal are all < n <=
    // KEYED_DISPATCH_MAX_DOMAIN_SIZE, so a*ordinal is at most (MAX-1)*(MAX-1)
    // and a*ordinal + b fits in uint64_t with no overflow. The final mod n
    // yields a value in [0, n). This is the overflow-safe widening the plan
    // requires (§8.1): the logical domain is u32, the evaluation is u64.
    const uint64_t r = (a * uint64_t(ordinal) + b) % n;
    out_index = uint32_t(r);
    return true;
}

// ─── Public: one authoritative helper ────────────────────────────────────
ExtensionResult<uint32_t> keyed_dispatch_permute(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept {
    uint32_t idx = 0;
    ExtensionError err{};
    if (eval_permute(route_word, domain, ordinal, idx, err)) {
        return ExtensionResult<uint32_t>{idx};
    }
    return ExtensionResult<uint32_t>{err};
}

// ─── Public: thin build-time reference wrapper ───────────────────────────
// Distinct API, identical mathematics — delegates to the authoritative helper.
ExtensionResult<uint32_t> keyed_dispatch_permute_build(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept {
    return keyed_dispatch_permute(route_word, domain, ordinal);
}

// ─── Public: thin runtime reference wrapper ──────────────────────────────
// Distinct API, identical mathematics — delegates to the authoritative helper.
ExtensionResult<uint32_t> keyed_dispatch_permute_runtime(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept {
    return keyed_dispatch_permute(route_word, domain, ordinal);
}

// ─── Public: domain validation ───────────────────────────────────────────
ExtensionStatus keyed_dispatch_validate_domain(
    const KeyedDispatchDomain& domain) noexcept {
    const uint32_t n = domain.domain_size;
    if (n < 2) {
        return ExtensionStatus{ExtensionError{"ember-keyed-dispatch", "",
            "domain size < 2"}};
    }
    if (n > KEYED_DISPATCH_MAX_DOMAIN_SIZE) {
        return ExtensionStatus{ExtensionError{"ember-keyed-dispatch", "",
            "domain size exceeds KEYED_DISPATCH_MAX_DOMAIN_SIZE"}};
    }
    // Strategy version 1 is the only currently-supported version. A future
    // hardened strategy (§8.3 implicit-keyed-v2) registers a separate version
    // and would be accepted here once it exists.
    if (domain.strategy_version != 1) {
        return ExtensionStatus{ExtensionError{"ember-keyed-dispatch", "",
            "unsupported strategy version"}};
    }
    return ExtensionStatus{};
}

} // namespace ember
