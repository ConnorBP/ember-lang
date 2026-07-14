// ext_obf.hpp — Stage C Step 5: IR-level obfuscation passes.
//
// Obfuscation passes are IR→IR transforms that INCREASE code complexity (more
// instructions, harder to reverse-engineer) while preserving the same result.
// They have is_required = true (bypass skip gates — you don't want a JIT budget
// gate accidentally disabling security passes).
//
// Passes:
// - SubstitutionPass ("subst"): MBA (Mixed Boolean-Arithmetic) instruction
//   substitution. Replaces simple integer arithmetic with equivalent MBA
//   expressions (e.g. a + b → (a ^ b) + 2*(a & b)).
// - MBAExpansionPass ("mba_expand"): seed-selects integer Add/Sub/Mul-by-two
//   sites and expands them into equivalent mixed boolean/arithmetic forms.
// - ConstantEncodingPass ("const_encode"): replaces nontrivial integer
//   constants with seeded arithmetic/bitwise computations of the same value.
// - OpaquePredicatesPass ("opaque_pred"): splits one block and guards its
//   continuation with a deterministic, mathematically fixed predicate.
// - DeadCodeInjectionPass ("deadcode"): injects a pure computation chain whose
//   result feeds a same-target branch, making it semantically inert but live to
//   ordinary dead-code elimination.
// - StringEncryptionPass ("str_encrypt"): encrypts ConstStringRef rodata bytes
//   in place and replaces each reference with the first-class StringDecrypt op.
// - BlockSplittingPass ("block_split"): deterministically splits long basic
//   blocks and connects each prefix to its continuation with an explicit jump.
//
// Red 6 (plan_POLYMORPHIC_CODE_ENGINE.md §4, §9.3): every pass has a CONFIGURED
// constructor taking an immutable PolymorphicPassOptions. The configured
// `run` derives INDEPENDENT per-site streams through src/seed_derivation.hpp
// (stable original block IDs + instruction ordinals + separate purposes:
// select | variant | constant | truth | junk-count), and mutates through the
// shared ThinIRMutation (reserve_site / allocate_* / split_block /
// canonicalize_block_ids / commit).
//
// COMPATIBILITY (Red 6 feedback: preserve the prior `reg.add<T>()` behavior):
// a DEFAULT-constructed pass captures `legacy_defaults(pass_name)` — a
// DETERMINISTIC fixed-root deriver (seed 0) + the pass's PRIOR per-pass
// eligibility density (subst / str_encrypt / block_split = 100%; mba_expand /
// const_encode = 50%; opaque_pred / deadcode = 100% with at-most-one-site
// selection). So a bare `reg.add<SubstitutionPass>("subst")` is a FUNCTIONING
// pass that transforms every eligible Add (NOT a zero-density no-op), and the
// existing `reg.add<T>()` users keep working with the prior eligibility
// semantics. `register_passes(reg)` (the compatibility wrapper) registers
// every pass through its default constructor (the `reg.add<T>()` path) so the
// pipeline names and the prior per-pass eligibility behavior are retained.
//
// `str_encrypt` is registered through the configured factory with its full
// Red 7 migration: per-site `site_selected` density gating, per-site seed-
// derived nonzero byte key, distinct data_temp_off / frame_off frame regions
// via ThinIRMutation, an atomic private-encrypted-region-per-selected-site
// rodata rebuild (so same-key overlaps never double-XOR/cancel and different-
// key overlaps never cross-corrupt), original-plaintext scrubbing for plaintext
// absence, and no double encryption (a StringDecrypt is never a candidate).
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §8 Step 5, §11;
// docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4–§6, §9.3 Red 6.

#pragma once

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/polymorphic_options.hpp"  // PolymorphicPassOptions, legacy_defaults
#include "../src/thin_ir.hpp"

namespace ember::ext_obf {

// ─── Configured obfuscation passes ───
// Each pass has a DEFAULT constructor that captures `legacy_defaults(pass_name)`
// (the bare `add<T>()` legacy path → a FUNCTIONING pass with the prior per-pass
// eligibility, NOT a no-op) and a CONFIGURED constructor that captures an
// immutable PolymorphicPassOptions by value. The configured `run` derives
// per-site, per-purpose streams through the shared SeedDeriver and mutates
// through the shared ThinIRMutation.

struct SubstitutionPass : EmberPassInfoMixin<SubstitutionPass> {
    static constexpr const char* pass_name = "subst";
    static constexpr bool is_required = true;  // obfuscation bypasses skip gates
    PolymorphicPassOptions options;
    SubstitutionPass() : options(legacy_defaults(pass_name)) {}
    explicit SubstitutionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct MBAExpansionPass : EmberPassInfoMixin<MBAExpansionPass> {
    static constexpr const char* pass_name = "mba_expand";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    MBAExpansionPass() : options(legacy_defaults(pass_name)) {}
    explicit MBAExpansionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct ConstantEncodingPass : EmberPassInfoMixin<ConstantEncodingPass> {
    static constexpr const char* pass_name = "const_encode";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    ConstantEncodingPass() : options(legacy_defaults(pass_name)) {}
    explicit ConstantEncodingPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct OpaquePredicatesPass : EmberPassInfoMixin<OpaquePredicatesPass> {
    static constexpr const char* pass_name = "opaque_pred";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    OpaquePredicatesPass() : options(legacy_defaults(pass_name)) {}
    explicit OpaquePredicatesPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct DeadCodeInjectionPass : EmberPassInfoMixin<DeadCodeInjectionPass> {
    static constexpr const char* pass_name = "deadcode";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    DeadCodeInjectionPass() : options(legacy_defaults(pass_name)) {}
    explicit DeadCodeInjectionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct StringEncryptionPass : EmberPassInfoMixin<StringEncryptionPass> {
    static constexpr const char* pass_name = "str_encrypt";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    StringEncryptionPass() : options(legacy_defaults(pass_name)) {}
    explicit StringEncryptionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    // Red 7: per-site seed-derived nonzero byte key, distinct data_temp_off /
    // frame_off frame regions via ThinIRMutation, atomic rodata rebuild for
    // overlapping references requiring different keys, plaintext absence.
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct BlockSplittingPass : EmberPassInfoMixin<BlockSplittingPass> {
    static constexpr const char* pass_name = "block_split";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    BlockSplittingPass() : options(legacy_defaults(pass_name)) {}
    explicit BlockSplittingPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Register all obfuscation passes by name with DETERMINISTIC LEGACY DEFAULTS
// (legacy_defaults per pass: a fixed-root seed-0 deriver + the prior per-pass
// eligibility density). Preserves the existing pipeline names and eligibility
// behavior. The compatibility wrapper — existing `register_passes(reg)`
// callers keep working unchanged, and the resulting passes are FUNCTIONING
// (not no-ops). Uses `reg.add<T>()` (the default-constructor path) so each
// pass captures its own `legacy_defaults(pass_name)`.
void register_passes(EmberPassRegistry& reg);

// Register all obfuscation passes by name with the given immutable
// PolymorphicPassOptions. Each pass is registered through a CONFIGURED factory
// that captures `options` by value and returns a fresh PassConcept on every
// `create()`. STRICT + VALIDATING: validates `options` first; on a validation
// failure (null deriver, ppm > 1_000_000, zero growth denominator, or overflow)
// NOTHING is registered and the structured ExtensionError is returned (so
// configured registration never silently accepts unvalidated options). On
// success, all 7 passes are registered (strict per-name: empty names, null
// factories, and duplicate names are rejected without replacing the first
// registration) and an ok status is returned. Never prints or throws.
ExtensionStatus register_passes(EmberPassRegistry& reg,
                                const PolymorphicPassOptions& options);

} // namespace ember::ext_obf
