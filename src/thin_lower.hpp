// thin_lower.hpp — Stage A c2: AST -> ThinFunction lowering (the IR-backend
// path that replaces the tree-walker's recursive eval/exec_block).
//
// lower_function walks a sema-checked FuncDecl and produces a ThinFunction
// (src/thin_ir.hpp) whose instructions are a mechanical, value-equivalent
// mirror of what CG::eval / CG::exec_block / CG::compile_func (src/codegen.cpp)
// would emit as x86 bytes. c3 (thin_emit) turns the ThinFunction into bytes;
// c5 compiles the same source both ways and asserts identical return values.
//
// LAZY MODE (Stage A): this is a mechanical lowering, NOT an optimizer. No
// regalloc, no peephole, no CSE/DCE/const-prop — those are Stage C IR passes
// over the ThinFunction AFTER c2 produces it. c2 just produces correct IR.
//
// ─── CORRECTNESS CONTRACT ───
//
// The lowering is VALUE-EQUIVALENT to the tree-walker for every function it
// lowers. It does NOT need to be byte-identical (c3's emit chooses the bytes).
// Every guard, every normalization (normalize width), every temp
// materialization, every slice two-word ABI, every struct byte-trim in the
// param-spill plan is preserved (encoded into the ThinFunction so c3 can emit
// it verbatim). The frame layout (ThinFramePlan) is computed to match
// compile_func's pre-passes EXACTLY so c3 emits an identical prologue/epilogue.
//
// ─── REPRESENTATION CONVENTIONS (c2 produces, c3 consumes — pin these) ───
//
// These extend the conventions pinned in thin_ir.hpp with the lowering-side
// specifics c3 needs. They are the contract between c2 and c3.
//
// • LoweredValue (internal to c2): every lower_expr returns
//     { kind: Scalar | Slice | Aggregate, vreg, frame_off, ty }
//   - Scalar / float : kind=Scalar, vreg = the value's VReg, frame_off unused.
//   - Slice           : kind=Slice, vreg = the PTR VReg; LEN is vreg+1 (the
//                       two consecutive-VRegs convention from thin_ir.hpp).
//   - Struct / array  : kind=Aggregate, vreg = 0, frame_off = the rbp-negative
//                       ABSOLUTE offset of the value's frame slot. The value
//                       has NO VReg (struct/array temps are never registers).
//
// • VReg types: c2 maintains a vreg->Type map and stamps meta.type on every
//   producing instr, so c3 can recover any VReg's type (width / int-vs-float /
//   signedness / slice-ness) without re-deriving it. c3 SHOULD rebuild the same
//   map by scanning producing instrs (meta.type is authoritative).
//
// • meta.width: set to value_bytes(ty) (1/2/4/8) for every int load/store/
//   arithmetic so c3 can emit normalize_rax at the right width. For float
//   instrs meta.is_f32 distinguishes f32/f64 (width is 4/8 respectively but
//   is_f32 is the discriminant c3 reads). For aggregates, width is unused.
//
// • Slice reads:
//   - LoadFrame of a slice LOCAL's ptr : loads an ABSOLUTE ptr (the local
//     stores absolute ptrs); c3 does NOT add a base. meta.type = slice type.
//   - LoadGlobal of a slice GLOBAL's ptr: the stored ptr is a RELATIVE offset
//     within the globals block; c3 loads it from [globals_base+addend] and
//     ADDS globals_base to make it absolute. c3 recognizes this by
//     meta.type->is_slice on a LoadGlobal. The len is a second LoadGlobal at
//     addend+8 with meta.type = i64 (plain word — no base add).
//
// • Calls:
//   - Scalar/float return : dst = result VReg (1).
//   - Slice return         : dst = ptr VReg, dst+1 = len VReg (consecutive).
//     c3 materializes {rax=ptr, rdx=len}.
//   - Struct return         : dst = 0 (no register result). The hidden word-0
//     destination pointer is carried ONE of two ways:
//       (a) args[0] = a real VReg (the loaded dest pointer) — used when the
//           dest is a runtime pointer (e.g. `return mk();` loads the incoming
//           hidden ptr from struct_ret_ptr_offset into a VReg first).
//       (b) args[0] = 0 (sentinel) + arg_frame_offs[0] = the dest's rbp offset
//           — used when the dest is a known frame slot (a let-init local or a
//           compiler temp). c3 LEAs [rbp+arg_frame_offs[0]] into the word-0
//           register.
//     ret_type = the struct type (c3 sees is_struct && dst==0 -> hidden-ptr
//     ABI, no register result). The real args start at word 1.
//   - Void return : dst = 0, ret_type = void.
//
// • Struct-by-value call argument: args[i] = 0 (sentinel) +
//   arg_frame_offs[i] = the source frame-slot rbp offset + arg_types[i] =
//   the struct type. c3 copies arg_types[i]->byte_size() bytes from
//   [rbp+arg_frame_offs[i]] into the word slots (true extent, not rounded —
//   matching the tree-walker's copy_bytes). A struct arg whose source is a
//   GLOBAL is first copied into a temp (CopyBytes) and the temp's offset is
//   used (value-equivalent; the tree-walker copies from the global directly).
//
// • CopyBytes: copies meta.len bytes.
//   - dst base: if `dst` VReg != 0 -> dst base = that VReg (a pointer held in
//     a register), meta.frame_off = offset within (usually 0). If `dst` == 0
//     -> dst base = rbp, meta.frame_off = rbp offset (OR globals_base if
//     meta.base_kind == GlobalsBase, meta.frame_off = global byte offset).
//   - src base: if `src1` VReg != 0 -> src base = that VReg, meta.field_off =
//     offset within. If `src1` == 0 -> src base = rbp, meta.field_off = rbp
//     offset (OR globals_base if meta.base_kind == GlobalsBase AND dst VReg
//     != 0 — i.e. base_kind marks the non-vreg side; when BOTH are non-vreg,
//     base_kind == GlobalsBase marks the DST as global).
//   c3 emits the same 8/4/2/1-byte chunked copy the tree-walker's copy_bytes
//   uses. Covers: local->local (aggregate cast), local->vptr (return local
//   struct through hidden ptr), global->vptr (return global struct), and
//   temp->global (struct-call result assigned to a global struct).
//
// • Guards: DepthCheck / BudgetCheck / CallTargetGuard / BoundsCheck /
//   DivOverflowCheck carry meta.trap_reason (the TrapReason ordinal) so c3
//   emits the matching trap (ud2 or trap-stub call). Gated on the SAME
//   CodeGenCtx flags the tree-walker gates on (emit_depth_checks /
//   emit_budget_checks / fn_allowlist_base+fn_slot_count); absent when off
//   (byte-identical to the tree-walker with flags off, the Stage-A gate).
//
// ─── OBFUSCATION FALLBACK (Stage A) ───
//
// If ctx.obf.mba / ctx.obf.opaque / ctx.obf.keyed is on OR the function has
// @obf / @obf_keyed annotations, lower_function sets ThinFunction::
// non_serializable = true + a reason string and returns an EMPTY blocks list.
// The obfuscation transforms (MBA arithmetic substitution, opaque-predicate
// junk, CPUID-keyed entry gate) operate directly on the X64Emitter with NO
// ThinOp representation, so c4's dispatch falls back to the tree-walker for
// these functions at Stage A. (A future stage may add ThinOps for them; until
// then the tree-walker is the only correct path.) All other functions —
// including ones that use safety guards, indirect calls, cross-module calls,
// trap stubs, and budget/depth pointers — ARE lowered (those have ThinOp
// representations); their process-local storage concerns are a Stage-B
// serialization matter, not a Stage-A lowering matter.

#pragma once
#include "thin_ir.hpp"   // ThinFunction, VReg, ThinOp, ... (the contract)
#include "codegen.hpp"   // CodeGenCtx

namespace ember {

struct FuncDecl;  // from ast.hpp (included via thin_ir.hpp)

// Lower one sema-checked function to a ThinFunction. The result's blocks[0]
// is the entry block; frame carries the pre-computed frame plan; rodata
// carries the function-local StringLit bytes. For obf functions, blocks is
// empty and non_serializable is set (see the fallback note above).
ThinFunction lower_function(const FuncDecl& f, const CodeGenCtx& ctx);

} // namespace ember
