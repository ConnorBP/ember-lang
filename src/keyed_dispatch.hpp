// keyed_dispatch.hpp — versioned reference affine dispatch permutation.
//
// Red 1 (docs/planning/plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §14.3,
// §8.1, §8.2). This is the GREEN side of the permutation-property contract: a
// single authoritative core helper that implements the versioned reference
// affine permutation P(x) = (a*x + b) mod n, plus thin build/runtime reference
// wrappers that delegate to it so the mathematics cannot drift between the
// build-time target placement and the runtime-time navigation of the same
// topology.
//
// The design in one paragraph (§2.2): build derives a transient target route
// word K and places each logical callable at physical_slot = P(K, salt,
// version, ordinal, n); runtime independently derives a transient route word k
// and navigates physical_slot = P(k, salt, version, ordinal, n). If the
// machine matches the target, k == K and the intended topology is reached; if
// not, k != K and a different, still-in-range, still-exact-ABI topology is
// reached. No expected key is stored, compared, hashed, or sealed — the route
// word only participates in the routing arithmetic.
//
// Required properties (§8.1), all pinned by examples/keyed_dispatch_math_test:
//   - P is a total bijection on [0, n) for every supported n;
//   - runtime is bounded (one affine evaluation; a is chosen by a deterministic
//     coprime search that always terminates because a=1 is always coprime);
//   - no modulo/out-of-range result for a valid ordinal;
//   - build and runtime implementations are byte-for-byte specified (one helper);
//   - different domains use independent tweaks (salt + version fold into the
//     canonical mix);
//   - arithmetic overflow behavior is explicit unsigned modular arithmetic,
//     performed in widened 64-bit so a*x + b cannot overflow for n <= MAX;
//   - all n from 2 through the configured maximum are supported.
//
// Constraints (§3, §8.1, §14.6): this header/impl uses NO std::hash, NO native
// struct bytes (the mix serializes fields field-by-field in little-endian
// order, not a struct memcpy), NO randomness, NO expected-key comparison, and
// NO direct pointers. Invalid domain sizes and ordinals are rejected through
// structured ExtensionResult/ExtensionError diagnostics (src/extension_registry.hpp),
// never through unchecked arithmetic that could yield an out-of-range index.
//
// This file is self-contained: it touches only unsigned integer arithmetic and
// the shared ExtensionResult/ExtensionError vocabulary, so it lives in the
// CORE `ember` lib with no ember_frontend dependency (one-way link direction),
// matching thin_ir.cpp / thin_ir_ser.cpp / em_type_codec.cpp.

#pragma once

#include <cstdint>
#include "extension_registry.hpp"

namespace ember {

// ─── Configured maximum ─────────────────────────────────────────────────
// The documented configured maximum domain size. Exhaustive bijection testing
// for every n in [2, MAX] is O(MAX^2) work, which stays well under a second on
// modern hardware (MAX=256 -> ~33k iterations per key/salt/version tuple). It
// is enforced consistently: domain sizes above this maximum are rejected with
// a structured ExtensionError at permute time and at validate_domain time. A
// real ABI domain with hundreds of same-signature functions is far beyond
// ordinary Ember modules, so this cap keeps exhaustive testing practical
// without constraining any realistic deployment.
inline constexpr uint32_t KEYED_DISPATCH_MAX_DOMAIN_SIZE = 256;

// ─── Dispatch domain ─────────────────────────────────────────────────────
// A dispatch domain is a same-ABI, same-visibility group of callables
// (§2.2, §7). For Red 1 (permutation properties only) the domain is identified
// by three public, non-key values:
//
//   - domain_salt:      a public per-domain tweak. It MAY derive from stable
//                       public module/domain/version metadata and optional
//                       build randomness. It is NOT a key and NOT a verifier
//                       (§8.4). It is the only domain value that may be stored
//                       in an artifact alongside the physical layout.
//   - strategy_version: the versioned permutation contract. Version 1 is the
//                       reference affine permutation defined here. A future
//                       hardened strategy registers a separate version
//                       (§8.3: "Do not silently change algorithm v1; register
//                       implicit-keyed-v2"). The version folds into the
//                       canonical mix so two versions produce independent
//                       permutations even with identical (salt, n).
//   - domain_size (n):  the physical slot count (logical function count plus
//                       padding count, §2.5). Must be in [2, MAX]. A domain
//                       always has at least one padding ordinal (§2.5), so n
//                       is at least 2 even for a singleton real function.
//
// The route word is NOT part of this struct: it is transient key-derived
// material supplied per resolution and is never stored in the domain.
struct KeyedDispatchDomain {
    uint64_t domain_salt = 0;       // public per-domain tweak (not a key)
    uint32_t strategy_version = 1;  // versioned permutation contract (v1=1)
    uint32_t domain_size = 0;       // n: physical slot count, must be in [2, MAX]
};

// ─── One authoritative helper ────────────────────────────────────────────
// The versioned reference affine permutation:
//
//   P(x) = (a * x + b) mod n
//
// where a and b derive deterministically from a canonical little-endian mix of
// (route_word, domain_salt, strategy_version, domain_size), and a is chosen
// deterministically (incrementing, skipping 0) until gcd(a, n) == 1. Because
// a=1 is coprime with every n >= 2, the search always terminates.
//
// Overflow behavior: a, b, and the ordinal x are all reduced to [0, n) with
// n <= KEYED_DISPATCH_MAX_DOMAIN_SIZE before the multiply, so a*x is at most
// (MAX-1)*(MAX-1) and a*x + b fits in uint64_t with no overflow. The final
// reduction mod n yields a value in [0, n). All arithmetic is unsigned with
// well-defined wraparound semantics (the mix uses specified 64-bit overflow).
//
// Returns a structured ExtensionResult<uint32_t>: success carries the physical
// index in [0, n); failure carries an ExtensionError for:
//   - domain_size < 2 or > KEYED_DISPATCH_MAX_DOMAIN_SIZE;
//   - ordinal >= domain_size.
// A failure never produces an index via unchecked arithmetic.
//
// This is the ONE implementation of the mathematics. The build/runtime
// wrappers below delegate to it.
ExtensionResult<uint32_t> keyed_dispatch_permute(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept;

// ─── Thin build-time reference wrapper ───────────────────────────────────
// Build uses the target-derived route word K to compute physical placement.
// Identical mathematics to the authoritative helper (it calls it), exposed as
// a DISTINCT API so build sites and runtime sites can be grepped/audited
// separately and so a future inline emitter can be cross-checked against this
// reference without conflating the two call roles (§5.5: "Both modes must
// produce identical physical indices for golden vectors").
ExtensionResult<uint32_t> keyed_dispatch_permute_build(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept;

// ─── Thin runtime reference wrapper ──────────────────────────────────────
// Runtime uses the locally-derived route word k to navigate the same topology.
// Identical mathematics to the authoritative helper (it calls it), exposed as
// a DISTINCT API for the same audit/parity reasons as the build wrapper.
ExtensionResult<uint32_t> keyed_dispatch_permute_runtime(
    uint64_t route_word,
    const KeyedDispatchDomain& domain,
    uint32_t ordinal) noexcept;

// ─── Domain validation ───────────────────────────────────────────────────
// Reject an invalid domain configuration through structured diagnostics BEFORE
// any arithmetic: domain_size must be in [2, KEYED_DISPATCH_MAX_DOMAIN_SIZE],
// and strategy_version must be a supported version (currently 1). Build and
// runtime both call this to reject malformed configuration early rather than
// reaching unchecked arithmetic. Returns ExtensionStatus (success carries no
// value; failure carries an ExtensionError).
ExtensionStatus keyed_dispatch_validate_domain(
    const KeyedDispatchDomain& domain) noexcept;

} // namespace ember
