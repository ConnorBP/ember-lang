// regalloc.hpp — Stage 3: SSA-lite linear-scan register allocation over the
// ThinFunction.
//
// run_regalloc(thf) computes the live interval of each register-candidate VReg
// (scalar int/bool — floats/slices/structs stay in frame slots for v1), sorts
// by start position, and assigns pool registers greedily (linear-scan). When
// all pool registers are in use, the farthest-reaching active interval is
// spilled to its existing frame slot. The result is stored in
// ThinFunction::ra (a RegAllocResult) and consumed by emit_x64.
//
// The IR is NOT SSA (VRegs may be reassigned). For v1 we compute the live
// interval as [first_def, last_use] — the conservative extent. This is correct
// because a reassignment overwrites the register with the new value, and the
// old value is dead before the reassignment (the lowering ensures this). Full
// SSA construction with phi nodes is NOT needed for linear-scan on this IR.
//
// Register pool (Win64 callee-saved, NOT used as scratch by emit_x64):
//   rbx, rsi, rdi, r12, r13, r15  (r14 = context reg, avoided)
// These survive calls (callee-saved). The regalloc records which it used so
// emit_x64 can save/restore them in the prologue/epilogue. rbx is already
// saved by the standard prologue.
//
// The regalloc runs AFTER the optimization passes (it is a final pass before
// emit). It does NOT change the IR instructions — it only produces the
// assignment map + extends the frame (frame_size / next_local_off) to add save
// slots for the callee-saved registers it uses. Value-preservation is
// CRITICAL: the regalloc MUST NOT change the result of any script.
//
// Design refs:
//   docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.3 (Stage-3 upgrade: SSA-lite
//     rename + linear-scan regalloc, additive on top of the ThinOp spine)
//   ROADMAP Stage 3 (cross-block + register-allocation improvement over the
//     intra-block store-to-load forwarding pass)

#pragma once
#include "thin_ir.hpp"    // ThinFunction, VReg, RegAllocResult

namespace ember {

// Run linear-scan register allocation over thf. Populates thf.ra (the
// RegAllocResult) and extends thf.frame (frame_size / next_local_off) to add
// save slots for the callee-saved registers used. When thf.ra.enabled is
// already false OR the function has no register candidates, this is a no-op
// (thf.ra.enabled stays false, emit_x64 uses the all-frame-slot path).
//
// num_regs: the number of pool registers to allow (0 = use the full pool of 6).
// Defaults to 0 (full pool) — the host may pass a smaller number for testing.
void run_regalloc(ThinFunction& thf, int32_t num_regs = 0);

} // namespace ember
