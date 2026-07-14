// pipeline_profile.hpp — Red 8: ordinary named pipeline profiles.
//
// A PipelineProfile is an ORDINARY NAMED RECIPE: a profile name + a comma-
// separated pass recipe string + a flag marking it experimental + an optional
// immutable PolymorphicPassOptions carried for the obfuscation factories. A
// profile is NEVER a hidden pass-manager mode: it is expanded through the
// existing EmberPassRegistry + build_pipeline_from_string into a FRESH
// EmberPassManager, exactly as an explicit `--passes` recipe is. There is no
// profile-specific run path, no profile-only fixed-point loop, and no
// instrumentation the profile silently alters. Selecting a profile and typing
// the equivalent `--passes <recipe>` are the same operation.
//
// PipelineProfileRegistry is a collision-aware, DETERMINISTIC registry built
// on the existing ExtensionStatus / ExtensionError foundation (the same
// contract EmberPassRegistry uses). Strict registration:
//   - empty names are rejected (not stored);
//   - empty recipe strings are rejected (not stored) — a profile must name at
//     least one pass;
//   - duplicate names are rejected without replacing the first registration;
//   - names() is deterministic (sorted lexicographically).
//
// The three built-in profiles (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 8):
//   light     — simplifying opts + light diversification, no experimental pass.
//   balanced  — full simplifying pass set + the full diversification set.
//   heavy     — an EXPLICITLY EXPERIMENTAL ordinary recipe/options variant with
//               BOUNDED higher density (a higher site_probability_ppm and
//               tighter PassGrowthLimits), NOT an uncontrolled fixed point. It
//               is still a one-shot pipeline expanded through
//               build_pipeline_from_string; nothing runs to a fixed point and
//               no growth ceiling is raised above the hard caps.
//
// In every built-in recipe the SIMPLIFYING optimization passes precede the
// one-shot diversification passes (constprop / simplifycfg / forward / copyprop
// / instcombine / cse / dce / dse come before const_encode / mba_expand /
// str_encrypt / block_split / opaque_pred / deadcode). regalloc still runs
// exactly once after the whole pass pipeline (the existing single allocation
// stage in compile_func); no profile adds a second regalloc.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 8.

#pragma once

#include "ember_pass.hpp"            // EmberPassManager
#include "ember_pass_pipeline.hpp"   // build_pipeline_from_string
#include "ember_pass_registry.hpp"   // EmberPassRegistry, make_pass_concept
#include "extension_registry.hpp"    // ExtensionStatus, ExtensionError
#include "polymorphic_options.hpp"   // PolymorphicPassOptions, make_polymorphic_options
#include "seed_derivation.hpp"       // FixedRootSeedDeriver, u64_to_root
#include "thin_ir_mutation.hpp"       // PassGrowthLimits

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ember {

// The registry identity carried in ExtensionError.registry for diagnostics
// produced by the pipeline-profile service.
inline constexpr const char* kPipelineProfileRegistryId = "ember-pipeline-profile";

// A named pipeline profile. `recipe` is an ordinary comma-separated pass
// string expanded through EmberPassRegistry + build_pipeline_from_string.
// `is_experimental` marks profiles whose behavior is explicitly experimental
// (heavy) so a host may warn / require an opt-in. `options` is the immutable
// PolymorphicPassOptions carried for the obfuscation factories; for opt-only
// profiles it may carry a null deriver + zero density (the factories are
// registered with the profile's options so the recipe resolves to configured,
// fresh instances). `seed` is the 64-bit fixed root seed the options were
// constructed from (recorded for diagnostics + reproducibility; the deriver
// inside `options` is the canonical source of randomness).
struct PipelineProfile {
    std::string name;
    std::string recipe;                 // comma-separated pass names
    bool is_experimental = false;
    PolymorphicPassOptions options;     // configured obf factory options
    uint64_t seed = 0;                  // the root seed the options derive from

    PipelineProfile() = default;
    PipelineProfile(std::string n, std::string r, bool exp,
                    PolymorphicPassOptions o, uint64_t s)
        : name(std::move(n)), recipe(std::move(r)), is_experimental(exp),
          options(std::move(o)), seed(s) {}
};

// Collision-aware, deterministic pipeline-profile registry. Strict registration
// on the existing ExtensionStatus / ExtensionError foundation (mirrors
// EmberPassRegistry's policy). names() is sorted lexicographically.
class PipelineProfileRegistry {
public:
    // Register a profile. Strict: rejects empty names, empty recipes, and
    // duplicate names (the first registration is retained, not replaced).
    ExtensionStatus add(PipelineProfile profile) {
        if (profile.name.empty()) {
            return make_extension_error(kPipelineProfileRegistryId, "",
                                        "profile name must not be empty");
        }
        if (profile.recipe.empty()) {
            return make_extension_error(kPipelineProfileRegistryId, profile.name,
                                        "profile recipe must not be empty");
        }
        if (profiles_.find(profile.name) != profiles_.end()) {
            return make_extension_error(kPipelineProfileRegistryId, profile.name,
                                        "profile name already registered");
        }
        profiles_.emplace(profile.name, std::move(profile));
        return make_extension_ok();
    }

    // Is a profile registered under this name?
    bool has(const std::string& name) const {
        return profiles_.find(name) != profiles_.end();
    }

    // Look up a profile by name. Returns nullptr if not found.
    const PipelineProfile* get(const std::string& name) const {
        auto it = profiles_.find(name);
        if (it == profiles_.end()) return nullptr;
        return &it->second;
    }

    // List all registered profile names (deterministic: sorted lexicographically
    // so output does not depend on unordered_map iteration order).
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(profiles_.size());
        for (const auto& [k, _] : profiles_) out.push_back(k);
        std::sort(out.begin(), out.end());
        return out;
    }

    // Number of registered profiles.
    std::size_t size() const { return profiles_.size(); }

private:
    std::unordered_map<std::string, PipelineProfile> profiles_;
};

// ─── Built-in profile recipes ───
//
// The exact pass sequences (Red 8). Simplifying optimization precedes one-shot
// diversification in every recipe; regalloc runs once after the whole pipeline
// (no profile adds a second allocation stage). These are plain string literals
// so a test can pin exact recipe expansion + order without re-deriving them.

inline constexpr const char* kProfileLightRecipe =
    "constprop,forward,copyprop,instcombine,dce,dse,const_encode,mba_expand";

inline constexpr const char* kProfileBalancedRecipe =
    "simplifycfg,constprop,forward,copyprop,instcombine,cse,dce,dse,"
    "str_encrypt,const_encode,block_split,opaque_pred,deadcode,mba_expand";

// heavy is the SAME diversification-bearing recipe as balanced but is
// explicitly experimental and carries BOUNDED higher density options. It is
// NOT an uncontrolled fixed point: it is still a one-shot pipeline expanded
// through build_pipeline_from_string, the growth ceilings are TIGHTER than the
// defaults (never raised above the hard caps), and the higher density is a
// bounded site_probability_ppm. Nothing runs to a fixed point.
inline constexpr const char* kProfileHeavyRecipe =
    "simplifycfg,constprop,forward,copyprop,instcombine,cse,dce,dse,"
    "str_encrypt,const_encode,block_split,opaque_pred,deadcode,mba_expand";

// Built-in profile names.
inline constexpr const char* kProfileLightName = "light";
inline constexpr const char* kProfileBalancedName = "balanced";
inline constexpr const char* kProfileHeavyName = "heavy";

// Build a PolymorphicPassOptions for a built-in profile from a 64-bit fixed
// seed. The deriver is a FixedRootSeedDeriver (reproducible). density_ppm is the
// site selection density; limits is the soft growth ceiling record. Returns
// the validated options (always succeeds for the built-in densities/limits
// because make_polymorphic_options validates). Never prints or throws.
inline PolymorphicPassOptions make_profile_options(uint64_t seed,
                                                   uint32_t density_ppm,
                                                   PassGrowthLimits limits,
                                                   std::string profile_id) {
    auto deriver = std::make_shared<FixedRootSeedDeriver>(u64_to_root(seed));
    auto r = make_polymorphic_options(deriver, /*algorithm_version=*/1,
                                      /*engine_version=*/"ember",
                                      /*module_id=*/"default",
                                      /*build_profile_id=*/std::move(profile_id),
                                      density_ppm, limits);
    // The built-in densities (<= 1_000_000 ppm) + nonzero denominators always
    // validate, so the value is present. If a future caller passes an invalid
    // record, fall back to a zero-density no-op so this never throws.
    return r.value ? std::move(*r.value) : PolymorphicPassOptions{};
}

// The light profile's density: a small bounded fraction (50_000 ppm = 5%) so
// diversification is light but observable.
inline constexpr uint32_t kProfileLightDensityPpm = 50'000;
// The balanced profile's density: the legacy 50% eligibility.
inline constexpr uint32_t kProfileBalancedDensityPpm = 500'000;
// The heavy profile's density: a BOUNDED higher fraction (900_000 ppm = 90%).
// Above the legacy 50% but strictly below 100% and never above 1_000_000.
inline constexpr uint32_t kProfileHeavyDensityPpm = 900'000;

// Tighter growth limits for the heavy (experimental) profile. The ceilings are
// LOWER than the defaults, never raised above the hard caps, so the higher
// density cannot run to an uncontrolled fixed point.
inline PassGrowthLimits heavy_profile_limits() {
    PassGrowthLimits l;
    l.max_sites = 32;
    l.max_added_instructions = 2048;
    l.max_added_blocks = 128;
    l.max_added_vregs = 4096;
    l.max_added_frame_bytes = 32 * 1024;
    l.max_added_rodata_bytes = 2 * 1024 * 1024;
    l.growth_numerator = 2;
    l.growth_denominator = 1;
    return l;
}

// Register the three built-in profiles (light, balanced, heavy) into `reg`
// using `seed` for the fixed-root deriver. Strict: if any built-in name is
// already present, that registration is rejected (and the original retained)
// — the returned ExtensionStatus carries the first collision. On a fresh
// registry all three register successfully. Never prints or throws.
inline ExtensionStatus register_builtin_profiles(PipelineProfileRegistry& reg,
                                                  uint64_t seed) {
    // light: opt-only options are irrelevant (the recipe has no obf pass that
    // reads density, but the factories are still registered with the profile's
    // options so the recipe resolves to configured fresh instances). Carry the
    // light density so the diversification passes (const_encode, mba_expand)
    // select ~5% of eligible sites — light but observable.
    PolymorphicPassOptions light_opts =
        make_profile_options(seed, kProfileLightDensityPpm, PassGrowthLimits{},
                             kProfileLightName);
    ExtensionStatus s1 = reg.add(PipelineProfile{
        kProfileLightName, kProfileLightRecipe, /*is_experimental=*/false,
        light_opts, seed});
    if (!bool(s1)) return s1;

    PolymorphicPassOptions balanced_opts =
        make_profile_options(seed, kProfileBalancedDensityPpm, PassGrowthLimits{},
                             kProfileBalancedName);
    ExtensionStatus s2 = reg.add(PipelineProfile{
        kProfileBalancedName, kProfileBalancedRecipe, /*is_experimental=*/false,
        balanced_opts, seed});
    if (!bool(s2)) return s2;

    PolymorphicPassOptions heavy_opts =
        make_profile_options(seed, kProfileHeavyDensityPpm, heavy_profile_limits(),
                             kProfileHeavyName);
    ExtensionStatus s3 = reg.add(PipelineProfile{
        kProfileHeavyName, kProfileHeavyRecipe, /*is_experimental=*/true,
        heavy_opts, seed});
    if (!bool(s3)) return s3;

    return make_extension_ok();
}

// ─── Profile → fresh EmberPassManager expansion ───
//
// The ONLY way a profile becomes a runnable pipeline: resolve the recipe
// through an EmberPassRegistry into a FRESH EmberPassManager via
// build_pipeline_from_string. No hidden pass-manager modes, no profile-only
// fixed-point loop. The caller owns the returned manager. Returns false + *err
// on a recipe parse failure (unknown pass, empty element, etc.); the manager
// is left unchanged on failure (transactional, per build_pipeline_from_string).
inline bool expand_profile(const PipelineProfile& profile,
                           const EmberPassRegistry& pass_reg,
                           EmberPassManager& out,
                           std::string* err) {
    return build_pipeline_from_string(profile.recipe, pass_reg, out, err);
}

} // namespace ember
