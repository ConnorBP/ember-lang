// key_provider.hpp — Red 5 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §6.1, §6.3, §9.8, §10.3): the immutable provider boundary + the dispatch key
// adapter.
//
// This is the GREEN side of the §6.1 provider-boundary contract. It defines
// the three types the keyed outer thunk consumes:
//
//   - DerivationRequest:           a domain-tagged, C++17-compatible byte view
//                                  (const uint8_t* + size_t — NOT std::span, so
//                                  the header compiles on a C++17 toolchain
//                                  without the span header) over public context
//                                  bytes the provider MAY fold into its derived
//                                  material. The adapter constructs one per
//                                  route_word request.
//   - DerivedMaterialProvider:     an immutable, thread-safe (const derive)
//                                  interface that returns 32 bytes of derived
//                                  material through a structured ExtensionResult.
//                                  Required keyed mode never falls back silently
//                                  (§6.1): a provider that cannot derive returns a
//                                  structured ExtensionError, and the safe keyed
//                                  API reports a structured CallResult failure
//                                  without entering the thunk.
//   - DispatchKeyAdapter:          domain-separates `ember/dispatch` (and the
//                                  companion `ember/layout`, `ember/passes`
//                                  domains), folds ONE runtime route word for a
//                                  stable module ID + strategy version. The
//                                  root machine material stays INSIDE the
//                                  provider; the adapter only folds the
//                                  derived 32 bytes into a 64-bit route word
//                                  (§6.1: "folds route output only after
//                                  deriving 256-bit material").
//
// Constraints (§3.3, §6.1, §14.6): no expected key is stored, compared, hashed,
// or sealed anywhere in this header. The route word is a TRANSIENT, per-call
// value derived at the outer boundary (§6.3) and installed in r15 for the call
// tree; it is never stored in context_t or a module record (§6.4: "Do not add
// the key to context_t"). The provider is immutable shared ownership (a
// std::shared_ptr<const ...>) held by the build configuration or runtime for
// the full operation lifetime (§6.1).
//
// This header is self-contained: it touches only the shared
// ExtensionResult/ExtensionError vocabulary (src/extension_registry.hpp) and
// fixed-width integer types, so it lives in the CORE `ember` lib with no
// ember_frontend dependency (one-way link direction), matching keyed_dispatch.hpp.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "extension_registry.hpp"    // ExtensionResult / ExtensionError

namespace ember {

// ─── A stable module identity (§2.1, §6.1) ──────────────────────────────
// Plain data. `name` is the stable module identity that participates in
// domain grouping; `version` is the module's own layout version (distinct
// from the dispatch strategy version — a module may rev its layout without
// revving the permutation contract). The adapter folds both into the route
// word so a layout-revved module derives a different route word under the same
// provider.
struct ModuleId {
    std::string name;
    uint32_t version = 1;
};

// ─── A derivation request (§6.1) ──────────────────────────────────────────
// A domain-tagged, C++17-compatible byte view over the public context bytes the
// provider MAY fold into its derived material. `domain` is the domain-
// separation tag (`ember/dispatch`, `ember/layout`, `ember/passes`, ...); the
// provider SHOULD fold it so two requests differing only in domain derive
// independent material (§6.1). `public_context` is optional host-supplied
// context (e.g. a build id); empty is permitted.
//
// C++17 note: this uses `const uint8_t*` + `size_t` rather than `std::span`
// so the header compiles on a C++17 toolchain without `<span>`. A `std::span`
// would be the natural C++20 shape; the project toolchain is C++17-compatible
// (MinGW g++), so the byte views here and in BuildKeyView (module_layout.hpp)
// use the same pointer+size idiom.
struct DerivationRequest {
    std::string_view domain;
    const uint8_t* public_context = nullptr;
    size_t public_context_size = 0;
};

// ─── The immutable provider boundary (§6.1) ──────────────────────────────
// An immutable, thread-safe interface: `derive` is `const` and mutates no
// state, so one `const DerivedMaterialProvider&` (or shared_ptr<const ...>)
// may be shared across worker threads freely. A provider returns 32 bytes of
// derived material through a structured ExtensionResult; required keyed mode
// never falls back silently — a provider that cannot derive returns a
// structured ExtensionError, and the safe keyed API reports a structured
// CallResult failure without entering the thunk (§6.1).
//
// The root machine material stays INSIDE the provider (§6.1). The adapter only
// folds the derived 32 bytes into a 64-bit route word; the provider's internal
// state (a sealed secret, a keystore handle, an envlock binding) never crosses
// the boundary. This is the §6.1 "composable, module-scoped seam": the host
// supplies a provider, not an expected key.
class DerivedMaterialProvider {
public:
    virtual ~DerivedMaterialProvider() = default;
    // Derive 256 bits (32 bytes) of material for `req`. Returns a structured
    // ExtensionResult: a value on success, an ExtensionError on failure. Never
    // falls back silently (§6.1). The returned material is transient to the
    // derivation; the adapter folds it into a route word and the material is
    // not retained beyond the route_word call.
    virtual ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest& req) const = 0;
};

// ─── The dispatch key adapter (§6.1) ──────────────────────────────────────
// Folds the provider's 32-byte derived material into ONE 64-bit runtime route
// word for a stable module ID + strategy version, domain-separating
// `ember/dispatch` (and the companion `ember/layout`, `ember/passes` domains).
// The adapter owns its provider by shared_ptr (immutable shared ownership,
// §6.1); the provider is held for the full operation lifetime.
//
// The route word is a TRANSIENT, per-call value (§6.3): the safe keyed API
// derives it once at the outer boundary and installs it in r15 for the call
// tree. It is never stored in context_t or a module record (§6.4). A
// provider failure (ExtensionError) propagates as a structured ExtensionError
// from route_word, and the safe API reports a structured CallResult failure
// without entering the thunk.
//
// The fold is a pinned FNV-1a 64-bit over a canonical little-endian byte
// serialization of (domain, module_id.name, module_id.version, strategy_version,
// the 32 derived bytes) — NOT std::hash, NOT a native struct memcpy. Different
// (domain, module, version) tuples fold to independent route words; the
// strategy version folds in so a v1 and a v2 strategy derive different words
// under the same provider (§6.1: "folds route output only after deriving
// 256-bit material").
class DispatchKeyAdapter {
public:
    explicit DispatchKeyAdapter(std::shared_ptr<const DerivedMaterialProvider> provider)
        : provider_(std::move(provider)) {}

    // Derive the runtime route word for (module, strategy_version, purpose).
    // `purpose` is a domain-separation tag (e.g. "ember/dispatch"). Returns a
    // structured ExtensionResult: a 64-bit route word on success, an
    // ExtensionError on provider failure. The route word is transient; the
    // caller (the safe keyed API) installs it in r15 for the call tree and
    // discards it at the outer-call boundary (§6.3).
    ExtensionResult<uint64_t> route_word(const ModuleId& module, uint32_t strategy_version,
                                         std::string_view purpose) const;

    // The held provider (immutable shared ownership). Exposed so a host can
    // share the same provider across an adapter + a polymorphic seed deriver
    // (§13: the polymorphic engine consumes a separate ember/passes domain).
    const std::shared_ptr<const DerivedMaterialProvider>& provider() const { return provider_; }

private:
    std::shared_ptr<const DerivedMaterialProvider> provider_;
};

// ─── A fixed-material provider (for tests + reproducible builds) ──────────
// Returns a fixed 32-byte material for every request. The root machine
// material is the fixed bytes themselves; the adapter folds them into a route
// word. Used by tests to pin a deterministic route word and by hosts that
// derive their material out-of-band and supply it as a fixed root.
class FixedMaterialProvider : public DerivedMaterialProvider {
public:
    explicit FixedMaterialProvider(std::array<uint8_t, 32> material) : material_(std::move(material)) {}
    ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest&) const override {
        return make_extension_result_ok(material_);
    }
private:
    std::array<uint8_t, 32> material_;
};

} // namespace ember
