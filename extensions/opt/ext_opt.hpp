// ext_opt.hpp — Stage C: the IR optimization passes extension.
//
// Three IR→IR optimization passes over ThinFunction, registered by name via
// register_passes (the extension-style discovery pattern, mirroring
// register_natives). The host wires it:
//   EmberPassRegistry reg;
//   ext_opt::register_passes(reg);
//   EmberPassManager pm;
//   pm.add_pass_concept(reg.create("constprop"));
//
// Passes (docs/spec/PASS_SYSTEM_DESIGN.md §8):
// - ConstPropPass ("constprop"): constant folding + frame-slot const-prop +
//   dead-constant elimination. Folds both-constant binary ops to ConstInt;
//   substitutes single-constant operands with the immediate form; then sweeps
//   dead ConstInt/LoadFrame defs and dead StoreFrame (slot never read).
// - DeadCodeElimPass ("dce"): removes dead pure instrs (dst VReg unused) +
//   dead local stores (StoreFrame to a slot never read). Iterates to fixpoint.
// - CSEPass ("cse"): local common-subexpression elimination within a block.
//   Coalesces redundant pure instrs (same op + operands + meta) by remapping
//   uses of the second's dst to the first's dst, then removing the second.
//
// All three are VALUE-PRESERVING: after the pass, emit_x64 produces the same
// i64 result. They return EmberPreserved::none() if they changed the IR,
// Preserved::all() if they did nothing.
#pragma once

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/thin_ir.hpp"

namespace ember::ext_opt {

struct ConstPropPass : EmberPassInfoMixin<ConstPropPass> {
    static constexpr const char* pass_name = "constprop";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct DeadCodeElimPass : EmberPassInfoMixin<DeadCodeElimPass> {
    static constexpr const char* pass_name = "dce";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct CSEPass : EmberPassInfoMixin<CSEPass> {
    static constexpr const char* pass_name = "cse";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Register all three passes by name. Mirrors register_natives(NativeTable&).
void register_passes(EmberPassRegistry& reg);

} // namespace ember::ext_opt
