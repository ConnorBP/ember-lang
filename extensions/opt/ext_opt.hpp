// ext_opt.hpp — Stage C: the IR optimization passes extension.
//
// Eight IR→IR optimization passes over ThinFunction, registered by name via
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
// - LICMPass ("licm"): loop-invariant code motion. Detects natural loops via
//   back-edges, finds the pre-header, and hoists invariant pure instrs
//   (ConstInt/ConstBool/ConstFloat, pure binops with invariant operands,
//   LoadFrame from a slot never written in the loop, Move of an invariant
//   vreg) to the end of the pre-header. Does NOT hoist stores. Works by
//   direct IR traversal (no EmberAnalysisManager needed yet).
// - StoreToLoadForwardPass ("forward"): intra-block store-to-load forwarding.
//   Replaces LoadFrame dst=vD off=X with Move dst=vD src1=vN when a
//   StoreFrame src1=vN off=X is the last writer to slot X (no intervening
//   store). Eliminates the frame-slot round-trip that makes the IR backend
//   1.2-1.9× slower than the tree-walker.
// - CopyPropPass ("copyprop"): intra-block copy propagation. After forwarding
//   creates Move instrs, replaces uses of the Move's dst with its src.
//   Pairs with forward + dce in the pipeline: constprop,forward,copyprop,dce.
// - InstCombinePass ("instcombine"): intra-block identity-fold of binary ops
//   where one operand is a known constant (x+0→x, x*1→x, x*0→0, x-x→0, x|x→x,
//   x&-1→x, x^x→0, x<<0→x, etc.). Replaces the BinOp with a Move (or ConstInt 0
//   for the self-annihilating cases), keeping meta.frame_off so emit still
//   treats the dst as frame-backed. Pairs with dce to remove the resulting
//   dead Moves.
// - DeadStoreElimPass ("dse"): intra-block dead store elimination. When a
//   second StoreFrame to the same frame_off appears with NO intervening
//   LoadFrame of that off, the FIRST StoreFrame was overwritten before being
//   read → remove it. Iterates to fixpoint within each block.
//
// All eight are VALUE-PRESERVING: after the pass, emit_x64 produces the same
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

struct LICMPass : EmberPassInfoMixin<LICMPass> {
    static constexpr const char* pass_name = "licm";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct StoreToLoadForwardPass : EmberPassInfoMixin<StoreToLoadForwardPass> {
    static constexpr const char* pass_name = "forward";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct CopyPropPass : EmberPassInfoMixin<CopyPropPass> {
    static constexpr const char* pass_name = "copyprop";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct InstCombinePass : EmberPassInfoMixin<InstCombinePass> {
    static constexpr const char* pass_name = "instcombine";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct DeadStoreElimPass : EmberPassInfoMixin<DeadStoreElimPass> {
    static constexpr const char* pass_name = "dse";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Register all passes by name. Mirrors register_natives(NativeTable&).
void register_passes(EmberPassRegistry& reg);

} // namespace ember::ext_opt
