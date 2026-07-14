// seed_derivation.hpp — Phase 2: versioned, stable seed/substream derivation.
//
// The single shared service every polymorphic transform derives its per-site
// randomness from. A pass never reads a process-global mutable stream and never
// re-implements FNV-1a/SplitMix64 itself; it builds a SeedRequest, hands it to
// an immutable SeedDeriver, and consumes the 256-bit result (or one of its four
// 64-bit lanes) through a local StableRng.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §5 (Seed
// architecture) and §9.3 Red 3 (seed derivation vectors).
//
// ─── Contract (§5.1) ───
//
//   same source + canonical options + tool version + build seed
//       = identical transformed Thin IR
//   different selected build seeds
//       = different eligible structure, same semantics
//
// The seed controls layout/diversity choices only. It is NOT a cryptographic
// secret and must not be used as a substitute for environmental authorization.
//
// ─── Immutability / thread safety (§5.3) ───
//
// SeedDeriver::derive is `const` and every provided implementation holds its
// root by value and mutates nothing, so one `const SeedDeriver&` may be shared
// across worker threads freely (parallel compilation creates a fresh
// configured pipeline per worker but shares the immutable deriver). There is no
// mutable/global stream: a StableRng is a local object a pass constructs per
// site from a derived lane, so adding a draw for one constant never reshuffles
// every later site and compile order never advances a shared generator.
//
// ─── Canonical byte encoding (§5.4) ───
//
// One fully-specified, implementation-independent encoding turns a SeedRequest
// into a byte stream. Every multi-byte integer below is LITTLE-ENDIAN. The
// encoding is the single source of truth for golden vectors and is duplicated
// NOWHERE else (tests hold hard-coded expected bytes, not a re-run of this
// algorithm).
//
//   DOMAIN  = the 20 ASCII bytes of "Ember.SeedDeriver.v1"
//             (fixed, written verbatim, NO length prefix — it is a constant)
//
//   B := DOMAIN
//        || u32_le(len(engine_version))   || engine_version bytes
//        || u32_le(len(module_id))        || module_id bytes
//        || u32_le(len(build_profile_id)) || build_profile_id bytes
//        || u32_le(len(pass_name))        || pass_name bytes
//        || u32_le(pass_algorithm_version)
//        || u32_le(len(function_name))    || function_name bytes
//        || u32_le(logical_slot)
//        || u32_le(block_id)
//        || u32_le(instruction_ordinal)
//        || u32_le(len(purpose))          || purpose bytes
//
//   String lengths are uint32_t counts of raw UTF-8 bytes (no NUL terminator).
//   Integer fields are fixed-width uint32_t. Nothing about a native C++ struct
//   layout, std::hash, std::random_device, a standard distribution, or
//   unordered-container iteration order participates in the encoding.
//
// ─── Derivation (root + request → 32 bytes) ───
//
//   FNV-1a 64-bit:  h0 = 0xcbf29ce484222325
//                   h  = (h ^ byte) * 0x100000001b3   (per byte, mod 2^64)
//   base = FNV1a( root_32_bytes || B )     // fold the 256-bit root first,
//                                           // then the canonical request bytes
//
//   Four INDEPENDENT, domain-separated 64-bit lanes (no shared advancing
//   state — drawing lane 0 never advances or reshuffles lanes 1..3):
//
//     splitmix_mix(z) = let z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9 mod 2^64
//                           z = (z ^ (z>>27)) * 0x94D049BB133111EB mod 2^64
//                       in  z ^ (z>>31)
//
//     lane[i] = splitmix_mix( base ^ LANE_DOMAIN[i] )      for i in 0..3
//
//   LANE_DOMAIN = { 0x454D53440C000000, 0x454D53440C000001,
//                   0x454D53440C000002, 0x454D53440C000003 }
//
//   out[i*8 .. i*8+8) = u64_le(lane[i])   →  the returned std::array<uint8_t,32>
//
// ─── u64 → 256-bit root adapter (§5.2 Fixed mode) ───
//
//   A fixed 64-bit CLI seed is adapted ONCE into a 256-bit root with a pinned
//   SplitMix64 stream whose initial state is `seed ^ ROOT_DOMAIN`, so the root
//   expansion is domain-separated from any other SplitMix64 use of the same
//   seed. Four 64-bit draws are written little-endian into the 32-byte root:
//
//     ROOT_DOMAIN = 0x454D424552524F54
//     rng = StableRng(seed ^ ROOT_DOMAIN)
//     root[i*8 .. i*8+8) = u64_le(rng.next())   for i in 0..3
//
// ─── StableRng (§5.4) ───
//
//   Pinned SplitMix64 advancing generator + rejection-sampled bounded index:
//
//     next():  state += 0x9E3779B97F4A7C15 (mod 2^64)
//              return splitmix_mix(state)
//     bounded(n):
//       if n == 0: return 0
//       r = (2^64 mod n)               // computed as ((0 - n) mod 2^64) mod n
//       if r == 0: return next() % n   // n divides 2^64: no bias, no rejection
//       threshold = 2^64 - r            // largest multiple of n that fits in u64
//       do x = next() while x >= threshold
//       return x % n
//
//   Rejection sampling removes the modular bias a naive `next() % n` introduces
//   when n does not divide 2^64. StableRng is a LOCAL object; one per site,
//   seeded from a derived lane. It is never held in process-global state.

#pragma once

#include "extension_registry.hpp"   // ExtensionResult, ExtensionError

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ember {

// The contextual request a pass builds to derive per-site seed material. Every
// field is a stable, canonical identity; none is a raw machine identifier or a
// provider secret (those stay behind a host-supplied SeedDeriver, never here).
struct SeedRequest {
    std::string_view engine_version;         // tool/engine version string
    std::string_view module_id;              // stable module identity
    std::string_view build_profile_id;       // profile/build identity
    std::string_view pass_name;              // registered pipeline name
    uint32_t          pass_algorithm_version = 0;  // independent per pass
    std::string_view function_name;          // function the site lives in
    uint32_t          logical_slot = 0;      // function logical slot
    uint32_t          block_id = 0;          // site block ID
    uint32_t          instruction_ordinal = 0; // original instruction ordinal
    std::string_view purpose;               // select|variant|constant|truth|...
};

// Immutable, thread-safe seed-derivation interface. `derive` is const and
// mutates no state, so one `const SeedDeriver&` may be shared across worker
// threads. A host may supply its own implementation (e.g. the companion plan's
// external-material adapter); the fixed-root implementation below covers
// ordinary reproducible builds.
class SeedDeriver {
public:
    virtual ~SeedDeriver() = default;
    // Derive 256 bits (four domain-separated 64-bit lanes) for `req`. Returns
    // a structured ExtensionResult: a value on success, an ExtensionError
    // (registry == "ember-seed-deriver") on failure. Never prints.
    virtual ExtensionResult<std::array<uint8_t, 32>>
    derive(const SeedRequest& req) const = 0;
};

// The registry identity carried in ExtensionError.registry for diagnostics
// produced by the seed-derivation service.
inline constexpr const char* kSeedDeriverRegistryId = "ember-seed-deriver";

// Fixed, distinct 64-bit lane domain-separation tags. Lane i folds `base` with
// tag[i] before a single SplitMix64 mix, so the four output lanes are
// independent (drawing lane 0 never advances lanes 1..3). Arbitrary pinned
// constants — see the canonical encoding above.
inline constexpr uint64_t kSeedLaneDomain[4] = {
    0x454D53440C000000ULL,
    0x454D53440C000001ULL,
    0x454D53440C000002ULL,
    0x454D53440C000003ULL,
};

// Domain-separation constant for the u64 → 256-bit root expansion.
inline constexpr uint64_t kSeedRootDomain = 0x454D424552524F54ULL;

// Expand a 64-bit fixed seed into a 256-bit root with a pinned SplitMix64
// stream (initial state `seed ^ kSeedRootDomain`), four little-endian draws.
// Used once at deriver construction to turn a CLI `--pass-seed <u64>` into the
// same 256-bit SeedDeriver interface an external provider would supply.
std::array<uint8_t, 32> u64_to_root(uint64_t seed);

// Immutable, thread-safe fixed-root implementation. Holds a 256-bit root by
// value; derive() is a pure function of (root, request) and mutates nothing, so
// one `const FixedRootSeedDeriver&` is safe to share across worker threads.
class FixedRootSeedDeriver : public SeedDeriver {
public:
    explicit FixedRootSeedDeriver(std::array<uint8_t, 32> root);

    ExtensionResult<std::array<uint8_t, 32>>
    derive(const SeedRequest& req) const override;

    const std::array<uint8_t, 32>& root() const { return root_; }

private:
    std::array<uint8_t, 32> root_;
};

// A local, pinned SplitMix64 generator with a rejection-sampled bounded-index
// helper. NOT thread-safe by design — it is a per-site local object a pass
// constructs from a derived lane. It holds no process-global state and never
// shares its advancing stream across sites, so draws for one site never
// reshuffle later sites.
class StableRng {
public:
    explicit StableRng(uint64_t seed) : state_(seed) {}

    // Advance the SplitMix64 state one step and return the mixed 64-bit value.
    uint64_t next();

    // Unbiased index in [0, n) via rejection sampling. n == 0 returns 0.
    uint64_t bounded(uint64_t n);

    uint64_t state() const { return state_; }

private:
    uint64_t state_;
};

} // namespace ember
