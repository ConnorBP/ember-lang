// seed_derivation.cpp — Phase 2: versioned, stable seed/substream derivation.
//
// GREEN implementation of the pinned algorithm documented in
// seed_derivation.hpp. Uses ONLY implementation-independent integer operations
// (FNV-1a 64-bit and SplitMix64 mixing). No std::hash, no std::random_device,
// no standard distributions, no native struct bytes, no unordered-container
// iteration. The deriver holds no mutable/global state; StableRng is a local
// per-site object.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §5, §9.3 Red 3.

#include "seed_derivation.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace ember {

namespace {

// 64-bit mask (mod 2^64).
inline constexpr uint64_t kMask64 = 0xFFFFFFFFFFFFFFFFULL;

// FNV-1a 64-bit offset basis and prime (the pinned constants from the
// algorithm spec). These are the same constants em_file.hpp / ext_obf.cpp use;
// they are repeated here only so seed_derivation is self-contained and never
// depends on another translation unit's private helpers.
inline constexpr uint64_t kFnv1aOffset = 0xcbf29ce484222325ULL;
inline constexpr uint64_t kFnv1aPrime  = 0x100000001b3ULL;

// SplitMix64 constants (the pinned mixing function).
inline constexpr uint64_t kSplitMixGamma = 0x9E3779B97F4A7C15ULL;
inline constexpr uint64_t kSplitMixMul1 = 0xBF58476D1CE4E5B9ULL;
inline constexpr uint64_t kSplitMixMul2 = 0x94D049BB133111EBULL;

// The fixed 20-byte domain header (ASCII "Ember.SeedDeriver.v1", no NUL, no
// length prefix — it is a constant).
inline constexpr char kDomain[] = "Ember.SeedDeriver.v1";
inline constexpr size_t kDomainLen = 20;  // strlen, but constant & spelled out.

// One SplitMix64 mixing step over a 64-bit value (a bijective mix, not the
// advancing generator). Used to derive each independent lane from `base`.
inline uint64_t splitmix_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * kSplitMixMul1;
    z = (z ^ (z >> 27)) * kSplitMixMul2;
    return z ^ (z >> 31);
}

// FNV-1a 64-bit over a byte range. h starts at the offset basis; per byte
// h = (h ^ byte) * prime, all mod 2^64 (unsigned overflow is well-defined).
inline uint64_t fnv1a64(const uint8_t* data, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnv1aPrime;
    }
    return h;
}

// Append a uint32_t little-endian to the buffer.
inline void put_u32_le(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>(
            static_cast<uint8_t>((v >> (8 * i)) & 0xFF)));
}

// Append a length-prefixed string (u32_le length + raw bytes) to the buffer.
// std::string_view::size() is a byte count (no NUL); the view is not iterated
// through any unordered container.
inline void put_lpref_str(std::string& out, std::string_view s) {
    put_u32_le(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

// Build the canonical request byte stream B (domain header + length-prefixed
// strings + fixed-width little-endian integers), excluding the root. The root
// is folded in by the caller before the FNV-1a pass.
std::string encode_request(const SeedRequest& r) {
    std::string out;
    out.reserve(128);
    out.append(kDomain, kDomainLen);
    put_lpref_str(out, r.engine_version);
    put_lpref_str(out, r.module_id);
    put_lpref_str(out, r.build_profile_id);
    put_lpref_str(out, r.pass_name);
    put_u32_le(out, r.pass_algorithm_version);
    put_lpref_str(out, r.function_name);
    put_u32_le(out, r.logical_slot);
    put_u32_le(out, r.block_id);
    put_u32_le(out, r.instruction_ordinal);
    put_lpref_str(out, r.purpose);
    return out;
}

// Write a uint64_t little-endian into out[pos..pos+8).
inline void put_u64_le(std::array<uint8_t, 32>& out, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out[pos + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

} // namespace

std::array<uint8_t, 32> u64_to_root(uint64_t seed) {
    // Pinned SplitMix64 stream, initial state domain-separated from any other
    // use of the same seed. Four 64-bit draws, written little-endian.
    StableRng rng(seed ^ kSeedRootDomain);
    std::array<uint8_t, 32> root{};
    for (size_t i = 0; i < 4; ++i)
        put_u64_le(root, i * 8, rng.next());
    return root;
}

FixedRootSeedDeriver::FixedRootSeedDeriver(std::array<uint8_t, 32> root)
    : root_(std::move(root)) {}

ExtensionResult<std::array<uint8_t, 32>>
FixedRootSeedDeriver::derive(const SeedRequest& req) const {
    // Fold the 256-bit root first, then the canonical request bytes, through
    // FNV-1a 64-bit to get `base`.
    uint64_t base = fnv1a64(root_.data(), root_.size(), kFnv1aOffset);
    const std::string body = encode_request(req);
    base = fnv1a64(reinterpret_cast<const uint8_t*>(body.data()),
                   body.size(), base);

    // Four independent, domain-separated 64-bit lanes. Each lane folds `base`
    // with its own tag before a single SplitMix64 mix, so the lanes are
    // independent (no shared advancing state).
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 4; ++i) {
        uint64_t lane = splitmix_mix(base ^ kSeedLaneDomain[i]);
        put_u64_le(out, i * 8, lane);
    }
    return ExtensionResult<std::array<uint8_t, 32>>{out};
}

uint64_t StableRng::next() {
    // Pinned SplitMix64 advancing generator.
    state_ += kSplitMixGamma;
    return splitmix_mix(state_);
}

uint64_t StableRng::bounded(uint64_t n) {
    if (n == 0) return 0;
    // r = 2^64 mod n, computed without overflow as ((0 - n) mod 2^64) mod n.
    // (0 - n) in unsigned 64-bit is 2^64 - n, and (2^64 - n) mod n == 2^64 mod n
    // because -n ≡ 0 (mod n).
    const uint64_t r = (static_cast<uint64_t>(0) - n) % n;
    if (r == 0) {
        // n divides 2^64 (n is a power of two): no modular bias, no rejection.
        return next() % n;
    }
    // Reject the top `r` values so the accepted range is an exact multiple of n.
    const uint64_t threshold = static_cast<uint64_t>(0) - r;  // 2^64 - r
    uint64_t x;
    do {
        x = next();
    } while (x >= threshold);
    return x % n;
}

} // namespace ember
