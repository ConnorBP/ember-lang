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
//   expressions (e.g. a + b → (a ^ b) + 2*(a & b)). Increases instruction count
//   and makes the code harder to statically analyze.
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

// Register all obfuscation passes by name.
void register_passes(EmberPassRegistry& reg);

} // namespace ember::ext_obf
