// polymorphic_options.cpp — Red 6: validation for PolymorphicPassOptions.
//
// The pure validation logic for the immutable option record. Lives in
// ember_frontend alongside seed_derivation.cpp (a compile-time pass concern)
// and depends only on the C++ standard library + the header-only option
// record. Never prints or throws; failures travel in the structured result.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.2, §9.3 Red 6.

#include "polymorphic_options.hpp"

#include <cstdint>
#include <limits>
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
    // growth_multiplier = ceil(numerator * kHardIrInstructions / denominator),
    // computed with overflow guards. numerator and kHardIrInstructions are
    // both <= 10^5-class values (well below 2^32), so the product cannot
    // overflow uint64 for any sane configuration; we guard anyway.
    const std::uint64_t num = limits.growth_numerator;
    const std::uint64_t den = limits.growth_denominator;
    if (num != 0 && num > std::numeric_limits<std::uint64_t>::max() / kHardIrInstructions) {
        return make_extension_error(kPolymorphicOptionsRegistryId, std::string{},
                                    "growth ratio overflows the instruction ceiling");
    }
    const std::uint64_t scaled = num * kHardIrInstructions;
    (void)den;   // the ratio is representable whenever `scaled` is; den <= num
                 // would grow the cap, but the soft ceiling + hard ceiling are
                 // enforced separately by ThinIRMutation at mutation time.
    (void)scaled;
    return make_extension_ok();
}

} // namespace

ExtensionStatus validate_polymorphic_options(const PolymorphicPassOptions& opts) {
    if (opts.site_probability_ppm > 1000000u) {
        return make_extension_error(kPolymorphicOptionsRegistryId,
                                    std::string{},
                                    "site_probability_ppm must not exceed 1000000");
    }
    if (auto st = check_growth_ratio(opts.limits); !st) {
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
    PolymorphicPassOptions opts;
    opts.seed_deriver = std::move(seed_deriver);
    opts.algorithm_version = algorithm_version;
    opts.engine_version = std::move(engine_version);
    opts.module_id = std::move(module_id);
    opts.build_profile_id = std::move(build_profile_id);
    opts.site_probability_ppm = site_probability_ppm;
    opts.limits = limits;
    if (auto st = validate_polymorphic_options(opts); !st) {
        return ExtensionResult<PolymorphicPassOptions>{std::move(st.error.value())};
    }
    return ExtensionResult<PolymorphicPassOptions>{std::move(opts)};
}

} // namespace ember
