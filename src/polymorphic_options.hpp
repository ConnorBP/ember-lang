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
// Immutability (Red 6 feedback: enforce the intended valid state):
//
// `PolymorphicPassOptions` is a CLASS with PRIVATE fields and public CONST
// accessors. The fields cannot be mutated after construction: there is no
// public field assignment and no public constructor that takes the individual
// fields unchecked. The only construction paths are:
//   - `make_polymorphic_options(...)` — the STRICT builder. Validates and
//     returns an `ExtensionResult<PolymorphicPassOptions>` (an
//     `ExtensionError` on a null deriver, ppm over 1_000_000, a zero growth
//     denominator, or a growth ratio that overflows uint64). This is the path
//     configured factories use.
//   - `legacy_defaults(pass_name)` — the DETERMINISTIC legacy-defaults builder
//     (a fixed-root deriver seed 0 + the pass's prior per-pass eligibility
//     density). This is the path the obf passes' DEFAULT constructors use so a
//     bare `reg.add<SubstitutionPass>("subst")` is a FUNCTIONING pass (not a
//     zero-density no-op) with the prior eligibility semantics preserved.
//   - the public default constructor `PolymorphicPassOptions{}` — produces a
//     NULL-deriver + zero-density sentinel (a no-op). This exists ONLY for
//     whole-object default initialization in unrelated aggregate holders
//     (e.g. `PipelineProfile bad{..., PolymorphicPassOptions{}, ...}`) and is
//     immediately overwritten or never derived; it is never the configuration
//     a functioning pass captures (the passes' default constructors call
//     `legacy_defaults`, not this sentinel).
//
// Copy/move construction and copy/move assignment remain public because
// unrelated aggregate holders (PipelineProfile, the CLI's `obf_options` local)
// rely on whole-object copy/move. Whole-object assignment replaces the entire
// record with another already-constructed (validated) record; it does not
// expose the individual fields for mutation, so the encapsulated-valid-state
// contract is preserved.
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
// Validation:
//
// `validate_polymorphic_options(opts)` returns an `ExtensionStatus` that is an
// `ExtensionError` (registry "ember-polymorphic-options") on:
//   - a null `seed_deriver` (a configured factory must derive; the only null-
//     deriver record is the `PolymorphicPassOptions{}` sentinel, which is a
//     no-op and never the configuration a functioning pass captures);
//   - `site_probability_ppm > 1_000_000` (above 100%);
//   - `growth_denominator == 0` or a growth ratio that overflows uint64.
// Neither `validate_polymorphic_options` nor `make_polymorphic_options` prints
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
// captures by value. See the file header for the immutability contract. The
// fields are PRIVATE; use the const accessors below. Construction is gated by
// `make_polymorphic_options` (strict, validating) or `legacy_defaults`
// (deterministic per-pass legacy eligibility). A public default constructor
// produces a null-deriver + zero-density sentinel (a no-op) for whole-object
// default initialization in unrelated aggregate holders only.
class PolymorphicPassOptions {
public:
    // Public default constructor: the no-op sentinel. Produces a null deriver
    // + zero density. Exists ONLY for whole-object default initialization in
    // unrelated aggregate holders (e.g.
    // `PipelineProfile bad{..., PolymorphicPassOptions{}, ...}`). A
    // functioning pass never captures this sentinel: the obf passes' default
    // constructors call `legacy_defaults(pass_name)`, and configured factories
    // receive a `make_polymorphic_options`-built record.
    PolymorphicPassOptions() = default;

    // Copy / move: public for whole-object copy/move in aggregate holders.
    // Whole-object copy/move replaces the entire record with another
    // already-constructed (validated) record; the individual fields stay
    // encapsulated and are never exposed for mutation.
    PolymorphicPassOptions(const PolymorphicPassOptions&) = default;
    PolymorphicPassOptions(PolymorphicPassOptions&&) noexcept = default;
    PolymorphicPassOptions& operator=(const PolymorphicPassOptions&) = default;
    PolymorphicPassOptions& operator=(PolymorphicPassOptions&&) noexcept = default;

    // Public CONST accessors (the fields are immutable after construction).
    const std::shared_ptr<const SeedDeriver>& seed_deriver() const { return seed_deriver_; }
    uint32_t algorithm_version() const { return algorithm_version_; }
    const std::string& engine_version() const { return engine_version_; }
    const std::string& module_id() const { return module_id_; }
    const std::string& build_profile_id() const { return build_profile_id_; }
    uint32_t site_probability_ppm() const { return site_probability_ppm_; }
    const PassGrowthLimits& limits() const { return limits_; }

    // Friends: the only construction paths that set the fields.
    friend ExtensionStatus validate_polymorphic_options(const PolymorphicPassOptions&);
    friend ExtensionResult<PolymorphicPassOptions>
        make_polymorphic_options(std::shared_ptr<const SeedDeriver>,
                                 uint32_t, std::string, std::string,
                                 std::string, uint32_t, PassGrowthLimits);
    friend PolymorphicPassOptions legacy_defaults(const char* pass_name);

private:
    // Private full constructor: only the friend builders call this. Sets every
    // field in one shot; the caller is responsible for validation (the friends
    // validate before / for construction).
    PolymorphicPassOptions(std::shared_ptr<const SeedDeriver> deriver,
                           uint32_t algo_ver,
                           std::string eng_ver,
                           std::string mod_id,
                           std::string prof_id,
                           uint32_t ppm,
                           PassGrowthLimits lim)
        : seed_deriver_(std::move(deriver)),
          algorithm_version_(algo_ver),
          engine_version_(std::move(eng_ver)),
          module_id_(std::move(mod_id)),
          build_profile_id_(std::move(prof_id)),
          site_probability_ppm_(ppm),
          limits_(std::move(lim)) {}

    std::shared_ptr<const SeedDeriver> seed_deriver_;
    uint32_t algorithm_version_ = 1;
    std::string engine_version_;
    std::string module_id_;
    std::string build_profile_id_;
    uint32_t site_probability_ppm_ = 0;
    PassGrowthLimits limits_{};
};

// Validate a PolymorphicPassOptions record. Returns an ExtensionStatus: ok on
// success, an ExtensionError (registry == kPolymorphicOptionsRegistryId) on
// failure. Never prints or throws. Checks:
//   - seed_deriver != null (a configured factory must derive; the only null-
//     deriver record is the no-op sentinel, which is never a functioning
//     pass's configuration);
//   - site_probability_ppm <= 1_000_000;
//   - limits.growth_denominator != 0;
//   - the growth ratio (numerator/denominator) does not overflow uint64 when
//     scaled by the hard IR instruction ceiling.
ExtensionStatus validate_polymorphic_options(const PolymorphicPassOptions& opts);

// Construct + validate a PolymorphicPassOptions in one step. Returns an
// ExtensionResult<PolymorphicPassOptions>: a value on success, an ExtensionError
// (registry == kPolymorphicOptionsRegistryId) on failure (null deriver, ppm >
// 1_000_000, zero growth denominator, or overflow). Never prints or throws.
// This is the STRICT path configured factories use; it is the only public way
// to build a non-sentinel, non-legacy record.
ExtensionResult<PolymorphicPassOptions>
make_polymorphic_options(std::shared_ptr<const SeedDeriver> seed_deriver,
                         uint32_t algorithm_version,
                         std::string engine_version,
                         std::string module_id,
                         std::string build_profile_id,
                         uint32_t site_probability_ppm,
                         PassGrowthLimits limits);

// Build the DETERMINISTIC legacy-defaults PolymorphicPassOptions for a given
// pass name. A fixed-root deriver (seed 0, fully deterministic + reproducible)
// + the pass's PRIOR per-pass eligibility density:
//   - subst / str_encrypt / block_split: 1_000_000 ppm (every eligible site —
//     these passes had NO per-site gating before Red 6);
//   - mba_expand / const_encode: 500_000 ppm (~50% — the legacy
//     `(rng.next() & 1U) == 0` eligibility);
//   - opaque_pred / deadcode: 1_000_000 ppm (the run selects at most ONE site,
//     preserving the prior "pick one site" eligibility);
//   - any unknown name: 500_000 ppm (a safe middle default).
// This is the configuration the obf passes' DEFAULT constructors capture, so
// a bare `reg.add<SubstitutionPass>("subst")` is a FUNCTIONING pass that
// transforms every eligible Add (not a zero-density no-op), preserving the
// prior `reg.add<T>()` behavior. Never prints or throws; always valid.
PolymorphicPassOptions legacy_defaults(const char* pass_name);

} // namespace ember
