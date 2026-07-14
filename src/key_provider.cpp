// key_provider.cpp — Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §6.1): the DispatchKeyAdapter route-word fold.
//
// The adapter folds the provider's 32-byte derived material + the
// (domain, module, strategy_version) identity into ONE 64-bit route word via a
// pinned FNV-1a 64-bit over a canonical little-endian byte serialization. NOT
// std::hash, NOT a native struct memcpy, NOT unordered-container iteration.
// The root machine material stays inside the provider; the adapter only folds
// the derived 32 bytes (§6.1: "folds route output only after deriving 256-bit
// material").

#include "key_provider.hpp"

#include <cstdint>
#include <string>

namespace ember {

namespace {

// ─── Pinned FNV-1a 64-bit (the non-std::hash family dispatch_abi.cpp /
// module_layout.cpp / seed_derivation.cpp use) ────────────────────────────
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
inline void put_lpref_str(std::string& out, std::string_view s) {
    put_u32_le(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

} // namespace

ExtensionResult<uint64_t> DispatchKeyAdapter::route_word(const ModuleId& module,
                                                         uint32_t strategy_version,
                                                         std::string_view purpose) const {
    if (!provider_) {
        return make_extension_result_error<uint64_t>(
            "ember-keyed-dispatch", "adapter", "null provider");
    }
    // Derive 256-bit material for the (domain, public-context) request. The
    // public context is empty here — the (module, strategy_version, purpose)
    // identity is folded below; the provider's root machine material is the
    // load-bearing input. A provider failure propagates as a structured error.
    DerivationRequest req;
    req.domain = purpose;
    req.public_context = nullptr;
    req.public_context_size = 0;
    auto mat = provider_->derive(req);
    if (!mat) {
        return ExtensionResult<uint64_t>{std::move(mat.error.value())};
    }
    // Canonical byte serialization: domain || module.name || module.version ||
    // strategy_version || the 32 derived bytes. Field-by-field little-endian,
    // length-prefixed strings. NOT a native struct memcpy.
    std::string b;
    b.reserve(purpose.size() + module.name.size() + 32 + 16);
    put_lpref_str(b, purpose);
    put_lpref_str(b, module.name);
    put_u32_le(b, module.version);
    put_u32_le(b, strategy_version);
    b.append(reinterpret_cast<const char*>(mat.value->data()), 32);
    uint64_t word = fnv1a64(reinterpret_cast<const uint8_t*>(b.data()), b.size(),
                            kFnv1aOffset);
    return make_extension_result_ok(word);
}

} // namespace ember
