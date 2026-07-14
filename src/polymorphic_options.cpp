// polymorphic_options.cpp — Red 6: validation + builders for PolymorphicPassOptions.
//
// The pure validation logic + the two construction paths (the strict
// `make_polymorphic_options` builder and the deterministic `legacy_defaults`
// builder) for the immutable option record. Lives in ember_frontend alongside
// seed_derivation.cpp (a compile-time pass concern) and depends only on the
// C++ standard library + the header-only option record. Never prints or
// throws; failures travel in the structured result.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.2, §9.3 Red 6.

#include "polymorphic_options.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace ember {

namespace {

// The hard IR instruction ceiling the growth-ratio overflow check scales
// against. Matches EmberPassManager::hard_max_ir_instructions so the ratio is
// guaranteed not to overflow when multiplied by the largest initial count a
// pass pipeline could legitimately start from.
inline constexpr std::uint64_t kHardIrInstructions = 100000ull;

// Check that limits.growth_numerator / limits.growth_denominator does not
// overflow uint64 when scaled by kHardIrInstructions. The growth ratio bounds
// final_count <= max(1, initial) * (num/den); we cap the multiplier by the
// hard instruction ceiling so the bound stays representable.
ExtensionStatus check_growth_ratio(const PassGrowthLimits& limits) {
    if (limits.growth_denominator == 0) {
        return make_extension_error(kPolymorphicOptionsRegistryId, std::string{},
                                    "growth_denominator must not be zero");
    }
    const std::uint64_t num = limits.growth_numerator;
    if (num != 0 && num > std::numeric_limits<std::uint64_t>::max() / kHardIrInstructions) {
        return make_extension_error(kPolymorphicOptionsRegistryId, std::string{},
                                    "growth ratio overflows the instruction ceiling");
    }
    return make_extension_ok();
}

// The legacy per-pass eligibility density (parts-per-million) for a pass name.
// Mirrors the per-pass gating that existed BEFORE Red 6 so a default-
// constructed pass (the bare `reg.add<T>()` path) preserves the prior
// eligibility semantics, not a zero-density no-op.
uint32_t legacy_density_ppm(const char* pass_name) {
    if (!pass_name) return 500'000u;
    // subst / str_encrypt / block_split had NO per-site gating before Red 6 —
    // they transformed EVERY eligible site. Preserve that at 100% density.
    if (std::strcmp(pass_name, "subst") == 0)        return 1'000'000u;
    if (std::strcmp(pass_name, "str_encrypt") == 0) return 1'000'000u;
    if (std::strcmp(pass_name, "block_split") == 0) return 1'000'000u;
    // opaque_pred / deadcode picked ONE site before Red 6. They run at 100%
    // density but the run selects AT MOST ONE site (the first accepted site),
    // preserving the "pick one site" eligibility. See ext_obf.cpp.
    if (std::strcmp(pass_name, "opaque_pred") == 0) return 1'000'000u;
    if (std::strcmp(pass_name, "deadcode") == 0)    return 1'000'000u;
    // mba_expand / const_encode used `(rng.next() & 1U) == 0` -> ~50%.
    return 500'000u;
}

} // namespace

ExtensionStatus validate_polymorphic_options(const PolymorphicPassOptions& opts) {
    // A configured factory must derive: a null deriver is the no-op sentinel
    // (PolymorphicPassOptions{}) only, and is never a functioning pass's
    // configuration. Reject it here so configured registration cannot silently
    // accept a no-op sentinel as a "configured" record.
    if (!opts.seed_deriver_) {
        return make_extension_error(kPolymorphicOptionsRegistryId, std::string{},
                                    "seed_deriver must not be null");
    }
    if (opts.site_probability_ppm_ > 1000000u) {
        return make_extension_error(kPolymorphicOptionsRegistryId,
                                    std::string{},
                                    "site_probability_ppm must not exceed 1000000");
    }
    if (auto st = check_growth_ratio(opts.limits_); !st) {
        return st;
    }
    return make_extension_ok();
}

ExtensionResult<PolymorphicPassOptions>
make_polymorphic_options(std::shared_ptr<const SeedDeriver> seed_deriver,
                         uint32_t algorithm_version,
                         std::string engine_version,
                         std::string module_id,
                         std::string build_profile_id,
                         uint32_t site_probability_ppm,
                         PassGrowthLimits limits) {
    // Build the record via the private full constructor (this function is a
    // friend), then validate. On validation failure return the error (no
    // value). The private constructor sets every field in one shot; the
    // fields are immutable thereafter.
    PolymorphicPassOptions opts(std::move(seed_deriver), algorithm_version,
                                std::move(engine_version), std::move(module_id),
                                std::move(build_profile_id), site_probability_ppm,
                                std::move(limits));
    if (auto st = validate_polymorphic_options(opts); !st) {
        return ExtensionResult<PolymorphicPassOptions>{std::move(st.error.value())};
    }
    return ExtensionResult<PolymorphicPassOptions>{std::move(opts)};
}

PolymorphicPassOptions legacy_defaults(const char* pass_name) {
    // A deterministic fixed-root deriver (seed 0): fully reproducible,
    // thread-safe, immutable. This is the configuration the obf passes'
    // DEFAULT constructors capture, so a bare `reg.add<T>()` is a functioning
    // pass with the prior eligibility semantics.
    auto deriver = std::make_shared<FixedRootSeedDeriver>(u64_to_root(0));
    // legacy_defaults is always valid (non-null deriver + a density <= 1M +
    // default nonzero denominator), so build directly via the private
    // constructor (this function is a friend).
    return PolymorphicPassOptions(std::move(deriver), /*algorithm_version=*/1u,
                                  /*engine_version=*/"ember",
                                  /*module_id=*/"default",
                                  /*build_profile_id=*/"default",
                                  legacy_density_ppm(pass_name),
                                  PassGrowthLimits{});
}

} // namespace ember
