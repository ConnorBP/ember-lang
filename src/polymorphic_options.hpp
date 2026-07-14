// polymorphic_options.hpp — Red 6: the immutable, shared configuration every
// configured obfuscation pass factory captures.
//
// This is the concrete, serialization-independent option record the
// plan_POLYMORPHIC_CODE_ENGINE.md §4.2 design calls `PolymorphicPassOptions`.
// It lives in `src/` (a concrete shared path reachable from every extension
// lib via the one-way `ember_frontend` link) so the obfuscation extension's
// configured factories capture it by value without depending on a higher
// serialization layer.
//
// What it carries:
//   - a shared `const SeedDeriver` (immutable, thread-safe; owned jointly for
//     the whole registry/pipeline lifetime so a fresh per-worker pipeline may
//     share one deriver);
//   - the algorithm version (independent per pass — bumping one transform does
//     not renumber every other stream);
//   - the stable module/profile identities a `SeedRequest` needs
//     (engine_version, module_id, build_profile_id) so the per-site streams
//     are reproducible and serialization-independent;
//   - `site_probability_ppm` (parts-per-million site selection density);
//   - a `PassGrowthLimits` soft-ceiling record.
//
// Validation: `make_polymorphic_options(...)` returns an
// `ExtensionResult<PolymorphicPassOptions>` that is an `ExtensionError`
// (registry "ember-polymorphic-options") on:
//   - a null `seed_deriver`;
//   - `site_probability_ppm > 1_000_000` (above 100%);
//   - `growth_denominator == 0` or a growth ratio that overflows uint64.
// `validate_polymorphic_options(opts)` performs the same checks on an already-
// constructed record and returns an `ExtensionStatus`. Neither function prints
// or throws; failures travel in the structured result.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.2, §5, §9.3 Red 6.

#pragma once

#include "extension_registry.hpp"   // ExtensionResult, ExtensionStatus, ...
#include "seed_derivation.hpp"      // SeedDeriver (shared const ownership)
#include "thin_ir_mutation.hpp"     // PassGrowthLimits

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ember {

// The registry identity carried in ExtensionError.registry for diagnostics
// produced by the polymorphic-options service.
inline constexpr const char* kPolymorphicOptionsRegistryId =
    "ember-polymorphic-options";

// The immutable option record every configured obfuscation pass factory
// captures by value. `seed_deriver` is shared immutable ownership (the
// complete registry/pipeline lifetime). Every other field is a value.
struct PolymorphicPassOptions {
    // Shared, immutable, thread-safe seed-derivation service. May be null only
    // in the no-op compatibility wrapper (which never derives); a configured
    // factory rejects a null deriver at construction time.
    std::shared_ptr<const SeedDeriver> seed_deriver;

    // Per-pass algorithm version. Independent across passes so bumping one
    // transform's algorithm version does not renumber every other stream.
    uint32_t algorithm_version = 1;

    // The stable module/profile identities a SeedRequest needs. These are
    // serialization-independent canonical identities (not raw machine
    // identifiers), shared by every configured factory in a registry.
    std::string engine_version;
    std::string module_id;
    std::string build_profile_id;

    // Site selection density in parts-per-million. 0 = no sites are selected
    // (every configured pass is a no-op). 1_000_000 = every eligible site is
    // selected. Values above 1_000_000 are rejected as invalid (above 100%).
    uint32_t site_probability_ppm = 0;

    // Soft per-pass growth ceilings + the growth ratio. `growth_denominator`
    // must be nonzero; the ratio (numerator/denominator) must not overflow
    // uint64 when multiplied by a representative initial instruction count.
    PassGrowthLimits limits{};
};

// Validate a PolymorphicPassOptions record. Returns an ExtensionStatus: ok on
// success, an ExtensionError (registry == kPolymorphicOptionsRegistryId) on
// failure. Never prints or throws. Checks:
//   - site_probability_ppm <= 1_000_000;
//   - limits.growth_denominator != 0;
//   - the growth ratio (numerator/denominator) does not overflow uint64 when
//     scaled by the hard IR instruction ceiling.
// A null seed_deriver is NOT rejected here (the no-op compatibility wrapper
// legitimately carries a null deriver); a CONFIGURED factory that would derive
// rejects a null deriver at construction time.
ExtensionStatus validate_polymorphic_options(const PolymorphicPassOptions& opts);

// Construct + validate a PolymorphicPassOptions in one step. Returns an
// ExtensionResult<PolymorphicPassOptions>: a value on success, an ExtensionError
// (registry == kPolymorphicOptionsRegistryId) on failure. Never prints or
// throws. This is the strict path configured factories should use.
ExtensionResult<PolymorphicPassOptions>
make_polymorphic_options(std::shared_ptr<const SeedDeriver> seed_deriver,
                         uint32_t algorithm_version,
                         std::string engine_version,
                         std::string module_id,
                         std::string build_profile_id,
                         uint32_t site_probability_ppm,
                         PassGrowthLimits limits);

} // namespace ember
