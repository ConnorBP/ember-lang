// thin_ir.hpp — Stage A c1: the thin three-address IR data structures.
//
// This is the CONTRACT chunk for the IL-.em Stage A: it pins the serializable-
// ready thin three-address IR that the lowering (c2: AST -> ThinFunction) and
// the emit (c3: ThinFunction -> x86 bytes) will independently implement
// against. c1 ships ONLY the data structures + the debug pretty-printer +
// a struct-invariants unit test. No lowering, no emit (LAZY MODE).
//
// Design refs:
//   docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.3 (the hybrid thin-IR option)
//     and §4.6 (the ThinFunction/ThinBlock/ThinInstr shape this pins).
//   docs/spec/COMPILER_PIPELINE.md §5 (the IrFunction target this is the
//     stepping stone toward) and §6 (the lowering rules c2 will implement).
//
// The thin IR is "three-address but NOT single-assignment" (naive TAC): each
// instr is `dst = op src1 src2`, a VReg may be reassigned. This is cheaper to
// build than full SSA-lite (no dominance, no phi, no rename) but gives passes
// a stable op vocabulary (ThinOp, not x86 bytes) so CSE/DCE/const-prop/peephole
// are IR passes, not byte-pattern passes. The Stage-3 upgrade to SSA-lite is
// additive (rename + slot-back + liveness on top of this instruction set),
// never a rewrite — so the ThinOp enum is the stable spine.
//
// ─── SERIALIZATION BOUNDARY (read this before writing the Stage B serializer) ───
//
// ThinOp is a STABLE uint16_t enum. Stage B serializes these IDs VERBATIM. Do
// NOT renumber, reorder, or insert into the existing enumerator list after
// this chunk lands — append new ops at the END only. (The numeric IDs are the
// on-disk identity; renumbering breaks every serialized .em.)
//
// The ONLY raw pointer fields in the IR are:
//   1. ThinInstr::native_fn      — JIT-time only (a host native fn ptr baked
//      at compile time). NOT serialized. Stage B DROPS it; the symbolic
//      binding is carried by ThinMeta::native_name (recover the ptr at load
//      time by name from the host native table, never by reverse-mapping).
//   2. ThinMeta::type, ThinInstr::ret_type, ThinInstr::arg_types — `const Type*`
//      into the compile-time type store. NOT serialized as pointers. Stage B
//      replaces each with a STABLE TYPE ID (the same canonical type encoding
//      em_writer/em_loader already use for .em: Prim + struct_name + is_slice +
//      array_len + elem chain). At Stage A the pointer is kept so c2/c3 share
//      the live Type objects with the rest of the frontend; Stage B swaps the
//      representation without touching the lowering/emit logic.
//
// No AST pointers cross this boundary: the IR is AST-free after lowering. The
// only source-level metadata carried forward is `Loc` (line/col) for debug /
// runtime error attribution. Frame references use ABSOLUTE rbp-negative offsets
// in meta.frame_off (the same offsets compile_func allocates), so emit does not
// recompute layout — see the representation conventions below.
//
// ─── REPRESENTATION CONVENTIONS (c2 produces, c3 consumes — pin these) ───
//
// • Scalar int/bool        = ONE VReg. The IR is register-agnostic; emit maps a
//   used scalar to rax (or the Stage-1 holding reg) at the use site, exactly as
//   the tree-walker does today. Float (f32/f64) = ONE VReg (mapped to xmm0 /
//   xmm1 at use). meta.is_f32 distinguishes the float width.
//
// • Slice                  = TWO consecutive VRegs (ptr at v, len at v+1). This
//   matches the tree-walker's slice ABI {rax=ptr, rdx=len} at every yield site
//   (Ident slice load, StringLit, ViewExpr, slice ArrayLit, slice arg, slice
//   return). c2 emits the pair together; c3 materializes them into rax/rdx.
//
// • Struct / fixed-array   = NOT a VReg. A struct/array temp is a frame slot
//   and is represented by its ABSOLUTE rbp-negative offset in meta.frame_off.
//   Struct/array temps are NEVER register candidates (the same rule as
//   compile_func's struct-temp allocation + the SSA-lite spec's "struct
//   temporaries are never register-candidates"). c2 lowers StructLit /
//   ArrayLit / aggregate copies to StructLitInit / ArrayLitInit / CopyBytes
//   operating on these frame offsets; c3 emits the byte-level stores/copies.
//
// • Frame references       = ABSOLUTE rbp-negative offsets in meta.frame_off
//   (the offsets compile_func allocates via next_local_off). c3 uses them
//   verbatim — it does NOT recompute the frame layout. The ThinFramePlan
//   carries the pre-computed frame_size / rbx_save_offset / struct_ret_ptr_
//   offset / arg_temps_base / next_local_off so c3 can emit an identical
//   prologue/epilogue to compile_func.
//
// • Calls                  = ONE CallNative / CallScript / CallIndirect /
//   CallCrossModule instr with args[] = the arg VRegs IN ORDER. A slice arg
//   contributes TWO consecutive vregs (ptr, len) — the two-word slice ABI. A
//   struct-by-value arg contributes its frame-slot offset via a SENTINEL:
//   vreg = 0 (invalid) + meta.frame_off = the slot's absolute rbp offset, set
//   on the corresponding args[] entry's... (carried per-arg: see ThinInstr::
//   arg_frame_offs, parallel to args, indexed by arg position; -1 = not a
//   frame-offset arg). c3 recognizes vreg==0 && arg_frame_offs[i] != -1 as a
//   struct-by-value word-0 source and copies the bytes from that slot.
//
// • Operator-overload BinExpr / implicit string_from_slice = CallNative (sema
//   already resolved the symbolic native binding; c2 stamps native_name, never
//   reverse-maps a ptr). Compile-time-folded assert_eq_* calls (CallExpr::
//   elided) emit NO instr at all.

#pragma once
#include "ast.hpp"          // Type, Loc, Prim (the value types the IR tags)
#include "x64_emitter.hpp"  // AbsFixup::Kind (reloc base kind carried in meta)

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// A virtual register. 0 = invalid / none (used for unused three-address
// fields and as the struct-by-value arg sentinel). NOT single-assignment: a
// VReg may be reused/reassigned (naive three-address TAC, per §4.3).
using VReg = uint32_t;

// STABLE enum — Stage B serializes these IDs verbatim. Do NOT renumber after
// land; append new ops at the END only. Ordered to cover every operation the
// tree-walker (src/codegen.cpp CG::eval / exec_stmt / compile_func) performs:
//   - literals        : IntLit/FloatLit/BoolLit/StringLit (FnHandleExpr is a
//                       compile-time slot -> ConstInt; Sizeof/Offsetof are
//                       resolved constants -> ConstInt)
//   - memory          : local/global load+store, reg-reg move, byte copies
//   - int arithmetic  : BinExpr int ops + UnaryExpr Neg/Not/BitNot
//                       (width in meta.width; result normalized to meta.width
//                       like the tree-walker's normalize_rax)
//   - float arithmetic: BinExpr float ops (f32/f64 in meta.is_f32)
//   - compare         : BinExpr compare (int + float) -> bool; predicate in
//                       meta.cmp (0=Eq..5=Ge, matching BinExpr::Op order)
//   - short-circuit   : LAnd/LOr (c2 may expand to Branch; kept as a first-
//                       class op so peephole can fold the shape back)
//   - cast            : CastExpr (int<->int width, int<->float, f32<->f64;
//                       target type in meta.type)
//   - calls           : native / script-slot / indirect(fn handle) / cross-mod
//   - aggregates      : field/index addr, bounds+div guards, slice materialize,
//                       struct/array literal init, string decrypt
//   - safety guards   : depth / budget / call-target (match emit_depth_check /
//                       emit_budget_check / emit_call_target_guard)
enum class ThinOp : uint16_t {
    // constants
    ConstInt, ConstFloat, ConstBool, ConstStringRef,
    // moves / memory
    Move, LoadFrame, StoreFrame, LoadGlobal, StoreGlobal, CopyBytes,
    // int arithmetic (width in meta.width; result normalized to meta.width)
    Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr, Neg, Not, BitNot,
    // float arithmetic (f32/f64 in meta.is_f32)
    FAdd, FSub, FMul, FDiv, FMod,
    // compare -> bool (predicate in meta.cmp)
    Cmp,
    // short-circuit logical (c2 may expand to Branch; peephole may fold back)
    LAnd, LOr,
    // cast (int<->int width, int<->float, f32<->f64; target type in meta.type)
    Cast,
    // calls
    CallNative, CallScript, CallIndirect, CallCrossModule,
    // addresses / aggregates
    FieldAddr, IndexAddr, BoundsCheck, DivOverflowCheck, MakeSlice,
    StructLitInit, ArrayLitInit, StringDecrypt,
    // guards (safety) — match emit_depth_check / emit_budget_check /
    // emit_call_target_guard. Emitted only when the host enabled the
    // corresponding CodeGenCtx flag; absent when off (byte-identical to the
    // tree-walker with flags off, the Stage-A gate).
    DepthCheck, BudgetCheck, CallTargetGuard,
    // Indirect memory store: [src2 + meta.frame_off] = src1. Appended to keep
    // the serialized ThinOp IDs above stable.
    StoreAddr,
    // Tier 4 try/catch/throw (in-language recoverable exceptions). Appended
    // AFTER StoreAddr to keep the serialized ThinOp IDs above stable.
    //   TryCatch     : inline setjmp — save callee-saved regs + rsp + the
    //                  catch-entry rip (block_labels[meta.slot]) into
    //                  context_t::catch_bufs[catch_depth], snapshot call_depth,
    //                  increment catch_depth. meta.slot = catch block id;
    //                  meta.frame_off = the catch_name i64 slot (allocated by
    //                  the lowerer; the catch entry loads thrown_value there).
    //                  An opaque barrier to every pass (classify_thin_effects'
    //                  default: WritesIndirect + aliases_unknown_memory).
    //   CatchCleanup : normal try-completion pop — decrement catch_depth.
    //                  Emitted at the end of the try body's final block before
    //                  the Jmp to the end block. Opaque barrier.
    //   CatchEntry   : catch-block prologue — load context_t::thrown_value into
    //                  the catch_name slot (meta.frame_off). The throw's longjmp
    //                  restored registers + rsp + rip to land here. Opaque.
    //   Throw        : src1 = the thrown i64 value. Store it into
    //                  context_t::thrown_value, then if catch_depth > 0 longjmp
    //                  to catch_bufs[catch_depth-1] (restore regs + rsp + jmp
    //                  to the saved catch-entry rip), else trap
    //                  (TrapReason::UnhandledThrow). Opaque barrier; src1 is a
    //                  use for liveness.
    TryCatch, CatchCleanup, CatchEntry, Throw,
};

// Immediate payload. ConstInt uses i; ConstFloat uses f (with meta.is_f32 for
// the width); ConstBool uses i (0/1); ConstStringRef uses i = the rodata
// addend (meta.base_kind = FunctionRodataBase) + meta.len = the byte length.
struct ThinImm {
    int64_t i = 0;
    double  f = 0.0;
};

// Per-instr metadata. All fields default to the "no-op / unspecified" value so
// a minimal three-address instr (just op + dst/src1/src2) is well-formed.
struct ThinMeta {
    int32_t frame_off = 0;      // rbp-relative ABSOLUTE offset (negative) for
                                // LoadFrame/StoreFrame/CopyBytes/FieldAddr and
                                // for struct-by-value arg frame slots.
                                // For StringDecrypt: the slice RESULT slot
                                // ({ptr,len}); data temp uses data_temp_off.
    int32_t width = 8;          // operand byte width (1/2/4/8) for int
                                // normalize + narrow element stores/loads
                                // (matches value_bytes / normalize_rax).
    int32_t len = 0;            // slice len / array count / copy byte count /
                                // ConstStringRef byte length.
    int32_t slot = -1;          // dispatch slot (CallScript) / cross-module
                                // target slot (CallCrossModule).
    int32_t mod_id = -1;        // CallCrossModule registry module id.
    int32_t field_off = 0;      // FieldAddr field byte offset within the base.
    AbsFixup::Kind base_kind = AbsFixup::DispatchTableBase; // LoadGlobal /
                                // StoreGlobal / CallScript / ConstStringRef
                                // base relocation kind (the same AbsFixup::Kind
                                // the tree-walker records via
                                // mov_reg_imm64_external).
    uint32_t addend = 0;        // rodata / globals addend for base_kind.
    std::string native_name;    // CallNative symbolic binding (serialize-ready;
                                // NEVER recover a ptr from native_fn — Stage B
                                // drops native_fn and rebinds by this name).
    const Type* type = nullptr; // Cast target type / value type. NOT serialized
                                // as a pointer: Stage B replaces with a stable
                                // type ID (the canonical Prim+struct_name+
                                // is_slice+array_len+elem encoding).
    // compare predicate, 0=Eq..5=Ge — matches BinExpr::Op order
    // (Eq,Neq,Lt,Le,Gt,Ge). Cmp sets this; emit selects the matching setcc.
    uint8_t cmp = 0;
    uint8_t is_unsigned = 0;    // Div/Mod/Shr unsigned (tree-walker's
                                // lt->is_uint() branch).
    uint8_t is_f32 = 0;         // float width: 1 = f32, 0 = f64 (ConstFloat /
                                // FAdd..FMod / Cast / float Cmp).
    uint8_t trap_reason = 0;    // Trap-site reason (TrapReason ordinal) for
                                // guards / BoundsCheck / DivOverflowCheck.
    int32_t data_temp_off = 0;  // StringDecrypt: rbp-relative offset (negative)
                                // for the decrypted-data temp buffer (separate
                                // from frame_off which is the slice result slot).
};

// One three-address instruction: `dst = op src1 src2` (+ imm + meta + args).
// Unused three-address fields are 0. Calls carry their arg VRegs in args[].
struct ThinInstr {
    ThinOp op = ThinOp::ConstInt;
    VReg dst = 0, src1 = 0, src2 = 0;   // three-address; unused fields = 0
    ThinImm imm;
    ThinMeta meta;
    std::vector<VReg> args;          // call arg vregs IN ORDER (slice arg =
                                     // two consecutive vregs ptr,len).
    std::vector<int32_t> arg_frame_offs; // parallel to args: struct-by-value
                                     // arg i's frame-slot ABSOLUTE rbp offset,
                                     // or -1 = this arg is a plain vreg. A
                                     // struct-by-value arg is represented by
                                     // args[i] = 0 (invalid vreg sentinel) +
                                     // arg_frame_offs[i] = the slot offset.
    std::vector<const Type*> arg_types; // call arg types (parallel to args);
                                     // Stage B replaces with stable type IDs.
    const Type* ret_type = nullptr;  // call return type (Stage B: type ID).
    void* native_fn = nullptr;       // CallNative JIT-time ptr. NOT serialized
                                     // — Stage B drops it; rebind by
                                     // meta.native_name at load time.
    Loc loc{};                       // source location for debug / errors
                                     // (AST-free otherwise).
};

// Block terminator. Every ThinBlock has term.kind != TermKind::None (the
// struct test pins this). A Trap terminator has no successors (the non-local
// unwind means control never returns — matches the SSA-lite spec's Trap
// block treatment), so liveness/peephole treat it as a dead end.
enum class TermKind : uint8_t {
    None, Jmp, Branch, Return, Trap
};

struct ThinTerm {
    TermKind kind = TermKind::None;
    VReg cond = 0;            // Branch condition vreg (a bool scalar).
    uint32_t target = 0;      // Jmp target block id / Branch true-target id.
    uint32_t false_target = 0;// Branch false-target id (0 if none for Jmp).
    VReg ret = 0;             // Return value vreg (0 = void return).
    uint8_t trap_reason = 0;  // Trap reason ordinal (TrapReason).
};

struct ThinBlock {
    uint32_t id = 0;
    std::vector<ThinInstr> instrs;
    ThinTerm term;
};

// The frame plan c2 computes (mirroring compile_func's layout pass) so c3
// emits an identical prologue/epilogue without recomputing anything. All
// offsets are ABSOLUTE rbp-negative (the same offsets compile_func allocates).
struct ThinFramePlan {
    int32_t frame_size = 0;                // round16 total; `sub rsp, frame_size`
    int32_t rbx_save_offset = -8;          // fixed -8 (Item E pinning slot)
    int32_t struct_ret_ptr_offset = 0;     // hidden return-ptr slot (0 = none)
    int32_t arg_temps_base = 0;            // arg-temp area base offset
    int32_t next_local_off = 0;            // next free local offset (grows down)
    bool returns_struct_by_ptr = false;    // hidden word-0 return ptr in use
    // One entry per spilled incoming parameter (matches compile_func's
    // per-word param spill). word0 = the first word index this param occupies;
    // nwords = words_for_type(ty) (slice=2, struct=ceil(size/8), else 1).
    struct ParamSpill {
        std::string name;
        const Type* ty = nullptr;   // Stage B: stable type ID.
        int32_t off = 0;            // absolute rbp offset of the param slot
        int32_t word0 = 0;          // first Win64 word this param occupies
        int32_t nwords = 1;         // word count (slice/struct > 1)
    };
    std::vector<ParamSpill> params;
    // Symbolic native-binding names recorded at emit time (one per CallNative
    // site), forwarded to CompiledFn::native_fixups so the .em serializer can
    // repoint them at load time. c3 populates this; c2 does not.
    std::vector<std::string> native_fixup_names;
};

// Stage 3: the result of linear-scan register allocation over the
// ThinFunction. Produced by run_regalloc() (src/regalloc.cpp) AFTER the
// optimization passes and BEFORE emit_x64. The emit reads this map to decide
// where each VReg lives: a VReg assigned to a register stays in that register
// (mov reg, rax after each def; mov rax, reg before each use) instead of being
// spilled to / reloaded from a frame slot. A VReg assigned to a frame slot
// uses the existing LoadFrame/StoreFrame path (the slot is the VReg's existing
// meta.frame_off from the lowering's spill-slot pass, so NO new spill slots
// are allocated by the regalloc).
//
// Register pool (Win64 callee-saved, NOT used as scratch by emit_x64):
//   rbx, rsi, rdi, r12, r13, r15  (r14 is the context reg; avoided)
// These survive calls (callee-saved ABI) so a VReg in a register is valid
// across CallNative/CallScript/etc. without caller-side save/restore. The
// regalloc records which pool registers it used so the emit can save them in
// the prologue (store to extended frame slots) and restore them in the
// epilogue. rbx is already saved by the standard prologue (rbx_save_offset);
// the regalloc does NOT double-save it.
//
// ADDITIVE: default-constructed {enabled=false, empty map} — untouched by
// lower_function / dump / the thin_ir_struct ctest. emit_x64 checks
// `ra.enabled` and falls back to the all-frame-slot path when false (the
// existing behavior, byte-identical). The serializer (Stage B) does NOT
// serialize this — regalloc is a JIT-time pass; a deserialized ThinFunction
// has ra.enabled=false and re-runs regalloc at load time if the host enables it.
struct RegAllocResult {
    bool enabled = false;
    // VReg -> assignment. Absent = the VReg is NOT a register candidate
    // (float/slice/struct/non-scalar) and uses its existing frame slot.
    struct Assign {
        bool in_reg = false;       // true = lives in a pool register; false = frame slot
        int32_t reg_id = -1;       // the Reg enum value (when in_reg)
        int32_t frame_off = 0;     // the spill frame slot (when !in_reg; the VReg's existing meta.frame_off)
    };
    std::unordered_map<VReg, Assign> map;
    // Promoted scalar frame slots. Ordinary LoadFrame/StoreFrame operations for
    // these offsets use the assigned callee-saved register instead of memory.
    // This keeps mutable loop locals and parameters resident across blocks and
    // calls while preserving the frame slot as their initial ABI home.
    std::unordered_map<int32_t, int32_t> frame_reg_map; // frame_off -> Reg enum
    // The pool registers actually used (for prologue save / epilogue restore).
    // Each entry is a Reg enum value. Parallel to save_offsets.
    std::vector<int32_t> used_reg_ids;
    // Frame offsets for saving each used register (parallel to used_reg_ids).
    // These are ABSOLUTE rbp-negative offsets allocated by the regalloc in the
    // extended frame. rbx is NOT included here (already saved at rbx_save_offset).
    std::vector<int32_t> save_offsets;
    // The number of pool registers the allocator is allowed to use (<= pool size).
    int32_t num_regs = 0;
};

// One lowered function. blocks[0] is the entry block. rodata holds the
// function-local string-literal bytes (forwarded to CompiledFn::rodata).
// abs_fixups is populated by the EMIT (c3), not the lowering (c2) — c3 records
// the same AbsFixup list the tree-walker records via mov_reg_imm64_external.
struct ThinFunction {
    std::string name;
    std::vector<ThinBlock> blocks;   // blocks[0] is entry
    ThinFramePlan frame;
    const Type* ret_type = nullptr;  // function return type (Stage B: type ID)
    std::vector<uint8_t> rodata;     // function-local rodata (StringLit bytes)
    std::vector<AbsFixup> abs_fixups;// populated by emit (c3), not lowering (c2)
    int slot = -1;                   // dispatch slot (matches FuncDecl::slot)
    bool non_serializable = false;   // obf fallback flag: obf functions fall
                                     // back to the tree-walker at Stage A
                                     // (emit_cpuid_gate / MBA / opaque junk are
                                     // emitter-level transforms with no ThinOp
                                     // representation), so c2 sets this + the
                                     // reason and the driver skips the thin
                                     // path for this fn.
    std::string non_serializable_reason;
    // Stage B: type objects reconstructed by deserialize_thin_function live
    // here so ret_type / arg_types / meta.type / frame.params[].ty const Type*
    // pointers remain valid for the ThinFunction's lifetime. Empty for JIT-
    // lowered ThinFunctions (their Type* point into the compile-time type
    // store). ADDITIVE: default-constructed empty, untouched by lower_function
    // / emit_x64 / dump — the thin_ir_struct ctest builds a ThinFunction by
    // hand and stays green unchanged.
    std::vector<std::shared_ptr<Type>> owned_types;
    // Stage B: the max VReg+1 declared in the ir_blob header. The deserializer
    // stores it here so validate_thin_function can check every VReg reference
    // against the DECLARED bound (not a recomputed one — recomputing from the
    // function's own VRegs is tautological). 0 for JIT-lowered ThinFunctions
    // (the lowering doesn't declare a bound; the validator recomputes for
    // those). ADDITIVE: default-constructed 0, untouched by lower_function /
    // emit_x64 / dump.
    uint32_t declared_max_vreg = 0;
    // Stage 3: the linear-scan register allocation result. Populated by
    // run_regalloc() after the optimization passes; consumed by emit_x64.
    // ADDITIVE: default-constructed {enabled=false} — emit_x64 checks
    // ra.enabled and falls back to the all-frame-slot path when false.
    RegAllocResult ra;
};

// Debug pretty-printer (src/thin_ir.cpp). Returns a human-readable dump of
// the function (blocks, instrs, terminators, frame plan). Used by tests and
// debug builds; NOT part of the serialization path.
std::string dump(const ThinFunction&);

} // namespace ember
