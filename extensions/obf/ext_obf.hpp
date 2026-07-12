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
// - OpaquePredicatesPass ("opaque_pred"): splits one block and guards its
//   continuation with a deterministic, mathematically fixed predicate.
// - DeadCodeInjectionPass ("deadcode"): injects a pure computation chain whose
//   result feeds a same-target branch, making it semantically inert but live to
//   ordinary dead-code elimination.
//
// Design ref: docs/spec/PASS_SYSTEM_DESIGN.md §8 Step 5, §11 (obfuscation pass
// structure from Pluto).
#pragma once

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/thin_ir.hpp"

namespace ember::ext_obf {

struct SubstitutionPass : EmberPassInfoMixin<SubstitutionPass> {
    static constexpr const char* pass_name = "subst";
    static constexpr bool is_required = true;  // obfuscation bypasses skip gates
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct OpaquePredicatesPass : EmberPassInfoMixin<OpaquePredicatesPass> {
    static constexpr const char* pass_name = "opaque_pred";
    static constexpr bool is_required = true;
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct DeadCodeInjectionPass : EmberPassInfoMixin<DeadCodeInjectionPass> {
    static constexpr const char* pass_name = "deadcode";
    static constexpr bool is_required = true;
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Register all obfuscation passes by name.
void register_passes(EmberPassRegistry& reg);

} // namespace ember::ext_obf
