// thin_emit.hpp — Stage A c3: the ThinFunction -> x86-64 emit pass.
//
// emit_x64(thf, ctx) takes a lowered ThinFunction (produced by c2: the
// thin_lower.cpp AST->ThinFunction lowering) and emits x86-64 bytes whose
// JIT'd EXECUTION is value-equivalent to the tree-walker (compile_func) JIT'ing
// the same source function. The output is a CompiledFn with the SAME shape
// compile_func produces (bytes, abs_fixups, native_fixups, rodata,
// non_serializable_reason, name) so the Stage B (.em serializer) and the
// Stage 1 peephole compose with the IR-backend path identically.
//
// LAZY MODE (Stage A): the emit reproduces the tree-walker's byte sequences
// keyed off ThinOp. It does NOT optimize beyond what the tree-walker does
// (Stage C does regalloc). Byte-identity with the tree-walker is NOT required
// (the IR path may emit push/pop where the tree-walker used r10, etc.) — only
// value-equivalence. The peephole + AbsFixup/native-fixup lists MUST be correct
// so the .em serializer (Stage B) and the Stage 1 peephole still work on the
// IR path's output.
//
// Design refs:
//   docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.3 (the hybrid thin-IR option)
//   docs/spec/COMPILER_PIPELINE.md §5 (the IrFunction target) + §6 (lowering)
//   src/thin_ir.hpp (the CONTRACT: IR shapes + representation conventions)
//   src/codegen.cpp compile_func / eval (the tree-walker byte sequences this
//     reproduces — READ FIRST, do NOT modify).
//
// VReg materialization model (consumes the conventions c1 pins in thin_ir.hpp):
//   • Scalar int/bool  = ONE VReg. Emit materializes a used scalar into rax at
//     the use site (load from the VReg's frame slot, or use rax if the VReg is
//     the most-recently-produced value still live in rax). Producing instrs
//     leave the dst in rax; if the instr's meta.frame_off is set, the result is
//     also stored to that frame slot and the VReg is recorded as frame-backed.
//   • Float (f32/f64)  = ONE VReg. Emit materializes into xmm0; meta.is_f32
//     distinguishes the float width. Same frame-slot / in-xmm0 tracking.
//   • Slice            = TWO consecutive VRegs (ptr at v, len at v+1). Emit
//     materializes the pair into {rax=ptr, rdx=len} (the tree-walker's slice
//     ABI at every yield site). Frame-backed slices store {ptr,len} at
//     [rbp+off] and [rbp+off+8].
//   • Struct / fixed-array = NOT a VReg. Represented by meta.frame_off (an
//     absolute rbp-negative offset). StructLitInit / ArrayLitInit / CopyBytes /
//     FieldAddr operate on these frame offsets directly.
//   • Frame references  = ABSOLUTE rbp-negative offsets in meta.frame_off (the
//     offsets compile_func allocates). Emit uses them verbatim.
//   • Calls             = ONE CallNative/CallScript/CallIndirect/CallCrossModule
//     instr with args[] = the arg VRegs IN ORDER. A slice arg contributes TWO
//     consecutive vregs (ptr, len). A struct-by-value arg contributes ONE
//     args[] entry with vreg==0 (the sentinel) + arg_frame_offs[i] = the slot's
//     absolute rbp offset. Emit recognizes vreg==0 && arg_frame_offs[i] != -1
//     as a struct-by-value word-0 source and copies the bytes from that slot.
//
// Immediate operands: when src2 == 0 (the VReg 0 sentinel) and the op is a
// binary int op, the instr uses imm.i as the second operand (a c2 convenience
// that avoids a separate ConstInt + reg-reg op). Emit handles both the VReg
// form (push/pop across the rhs materialization, matching the tree-walker's
// BinExpr sequence) and the immediate form (a cheaper op rax, imm / mov rcx,
// imm; op rax, rcx — value-equivalent, not byte-identical).
//
// Param VReg convention: the lowering assigns VRegs to incoming params starting
// at VReg 1, in param order. A scalar/float param consumes 1 VReg; a slice
// param consumes 2 consecutive VRegs (ptr, len); a struct param consumes 0
// VRegs (it is a frame slot, accessed via FieldAddr/CopyBytes). The emit builds
// the initial VReg->frame_off map from ThinFramePlan::params using this
// convention, so the entry block's instrs can reference param VRegs directly
// (without preceding LoadFrame instrs). The c5 gate validates this against the
// real c2 lowering output.

#pragma once
#include "thin_ir.hpp"    // ThinFunction (the IR this consumes)
#include "codegen.hpp"    // CodeGenCtx (the host context: dispatch/globals/
                          //   registry bases, natives, structs, safety ptrs,
                          //   peephole + regalloc flags)
#include "engine.hpp"     // CompiledFn (the output shape)

namespace ember {

// Emit a ThinFunction to x86-64 bytes. Returns a CompiledFn whose JIT'd
// behavior is value-equivalent to the tree-walker compiling the same source.
// The CompiledFn carries the same abs_fixups / native_fixups / rodata /
// non_serializable_reason shape compile_func produces, so the .em serializer
// (Stage B) and the Stage 1 peephole compose with the IR path identically.
CompiledFn emit_x64(const ThinFunction& thf, const CodeGenCtx& ctx);

} // namespace ember
