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
// canonicalize_block_ids / commit). A default-constructed pass carries a
// null-deriver, zero-density PolymorphicPassOptions and is a deterministic
// NO-OP (the bare `add<T>()` legacy path); `register_passes(reg)` (the
// compatibility wrapper) routes through `register_passes(reg, options)` with
// DETERMINISTIC DEFAULTS so existing pipeline names + eligibility behavior are
// retained.
//
// `str_encrypt` is registered through the configured factory with its full
// Red 7 migration: per-site seed-derived nonzero byte key, distinct data/
// slice frame regions via ThinIRMutation, and an atomic rodata rebuild when
// overlapping references require different keys.
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §8 Step 5, §11;
// docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4–§6, §9.3 Red 6.

#pragma once

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/polymorphic_options.hpp"  // PolymorphicPassOptions
#include "../src/thin_ir.hpp"

namespace ember::ext_obf {

// ─── Configured obfuscation passes ───
// Each pass has a default constructor (the bare `add<T>()` legacy path →
// no-op: null deriver + zero density) and a configured constructor that
// captures an immutable PolymorphicPassOptions by value. The configured `run`
// derives per-site, per-purpose streams through the shared SeedDeriver and
// mutates through the shared ThinIRMutation.

struct SubstitutionPass : EmberPassInfoMixin<SubstitutionPass> {
    static constexpr const char* pass_name = "subst";
    static constexpr bool is_required = true;  // obfuscation bypasses skip gates
    PolymorphicPassOptions options;
    SubstitutionPass() = default;
    explicit SubstitutionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct MBAExpansionPass : EmberPassInfoMixin<MBAExpansionPass> {
    static constexpr const char* pass_name = "mba_expand";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    MBAExpansionPass() = default;
    explicit MBAExpansionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct ConstantEncodingPass : EmberPassInfoMixin<ConstantEncodingPass> {
    static constexpr const char* pass_name = "const_encode";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    ConstantEncodingPass() = default;
    explicit ConstantEncodingPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct OpaquePredicatesPass : EmberPassInfoMixin<OpaquePredicatesPass> {
    static constexpr const char* pass_name = "opaque_pred";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    OpaquePredicatesPass() = default;
    explicit OpaquePredicatesPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct DeadCodeInjectionPass : EmberPassInfoMixin<DeadCodeInjectionPass> {
    static constexpr const char* pass_name = "deadcode";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    DeadCodeInjectionPass() = default;
    explicit DeadCodeInjectionPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct StringEncryptionPass : EmberPassInfoMixin<StringEncryptionPass> {
    static constexpr const char* pass_name = "str_encrypt";
    static constexpr bool is_required = true;
    PolymorphicPassOptions options;
    StringEncryptionPass() = default;
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
    BlockSplittingPass() = default;
    explicit BlockSplittingPass(PolymorphicPassOptions o) : options(std::move(o)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Register all obfuscation passes by name with DETERMINISTIC DEFAULT options
// (a fixed-root seed deriver + the existing eligibility density). Preserves
// the existing pipeline names and eligibility behavior. The compatibility
// wrapper — existing `register_passes(reg)` callers keep working unchanged.
void register_passes(EmberPassRegistry& reg);

// Register all obfuscation passes by name with the given immutable
// PolymorphicPassOptions. Each pass is registered through a CONFIGURED factory
// that captures `options` by value and returns a fresh PassConcept on every
// `create()`. Strict registration: rejects empty names, null factories, and
// duplicate names without replacing the first registration.
void register_passes(EmberPassRegistry& reg, const PolymorphicPassOptions& options);

} // namespace ember::ext_obf
