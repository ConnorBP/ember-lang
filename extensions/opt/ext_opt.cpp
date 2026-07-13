// ext_opt.cpp — Stage C: the IR optimization passes.
// See ext_opt.hpp for the design. All passes are value-preserving and
// conservative — when in doubt, do not transform.

#include "ext_opt.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ember::ext_opt {

namespace {

// ─── Side-effect classification ───
// A pure instr's dst VReg may be removed by DCE if unused, and the instr may
// be CSE'd. A side-effecting instr must never be removed by DCE or coalesced
// by CSE, even if its dst is unused.

bool is_side_effecting(ThinOp op) {
    switch (op) {
    case ThinOp::CallNative:
    case ThinOp::CallScript:
    case ThinOp::CallIndirect:
    case ThinOp::CallCrossModule:
    case ThinOp::StoreGlobal:
    case ThinOp::StoreAddr:
    case ThinOp::CopyBytes:
    case ThinOp::StructLitInit:
    case ThinOp::ArrayLitInit:
    case ThinOp::StringDecrypt:
    case ThinOp::BoundsCheck:
    case ThinOp::DivOverflowCheck:
    case ThinOp::DepthCheck:
    case ThinOp::BudgetCheck:
    case ThinOp::CallTargetGuard:
        return true;
    default:
        return false;
    }
}

bool is_pure(ThinOp op) {
    // StoreFrame is not "pure" in the CSE sense (it's a memory write), but it
    // is removable by DCE if the slot is dead. Handle StoreFrame separately.
    return !is_side_effecting(op) && op != ThinOp::StoreFrame;
}

// Is this a foldable integer binary op?
bool is_foldable_int_binop(ThinOp op) {
    switch (op) {
    case ThinOp::Add: case ThinOp::Sub: case ThinOp::Mul:
    case ThinOp::And: case ThinOp::Or:  case ThinOp::Xor:
    case ThinOp::Shl: case ThinOp::Shr:
        return true;
    default:
        return false;
    }
}

// Fold a binary int op on two constant operands. Returns true + sets *result.
// Only handles the safe, well-defined cases. Does NOT fold Div/Mod (division
// by zero / INT64_MIN/-1 UB). Respects meta.width (truncates to width bytes,
// two's-complement) and meta.is_unsigned (for Shr). Shift amounts are masked
// to 0..63 to match x64 shl/shr semantics (the CPU masks CL to 0..63) and
// try_eval_const_i64's `r & 63` — without the mask, `a << b` for b >= 64 is
// C++ undefined behavior and would let ConstProp fold a `7 << 64` to an
// arbitrary value instead of the `7 << 0 = 7` the tree-walker produces.
bool fold_int_binop(ThinOp op, int64_t a, int64_t b, int32_t width,
                    bool is_unsigned, int64_t* result) {
    int64_t r = 0;
    switch (op) {
    case ThinOp::Add: r = a + b; break;
    case ThinOp::Sub: r = a - b; break;
    case ThinOp::Mul: r = a * b; break;
    case ThinOp::And: r = a & b; break;
    case ThinOp::Or:  r = a | b; break;
    case ThinOp::Xor: r = a ^ b; break;
    case ThinOp::Shl: r = static_cast<int64_t>(uint64_t(a) << (uint64_t(b) & 63u)); break;
    case ThinOp::Shr: {
        int sh = int(uint64_t(b) & 63u);
        if (is_unsigned) r = int64_t(uint64_t(a) >> sh);
        else {
            uint64_t ur = uint64_t(a) >> sh;
            if (sh != 0 && a < 0) ur |= ~((1ULL << (64 - sh)) - 1);
            r = static_cast<int64_t>(ur);
        }
        break;
    }
    default:
        return false;
    }
    // Normalize to width (two's-complement truncation).
    if (width == 1)       r = int64_t(int8_t(r));
    else if (width == 2)  r = int64_t(int16_t(r));
    else if (width == 4)  r = int64_t(int32_t(r));
    // width == 8 or 0: no truncation.
    *result = r;
    return true;
}

// Count total instrs in a function.
size_t count_instrs(const ThinFunction& f) {
    size_t n = 0;
    for (const auto& blk : f.blocks) n += blk.instrs.size();
    return n;
}

// Compute the set of VRegs that are USED anywhere in the function (as src1,
// src2, args, term.cond, term.ret). CopyBytes is special: its `dst` field holds
// a dest-pointer VReg that is READ (not produced) — copy_frame_vptr /
// copy_global_vptr set in.dst to the runtime dest ptr. Without this, DCE would
// remove the LoadFrame that produces the hidden return pointer, leaving
// CopyBytes reading an undefined VReg (a crash).
std::unordered_set<VReg> compute_used_vregs(const ThinFunction& f) {
    std::unordered_set<VReg> used;
    for (const auto& blk : f.blocks) {
        for (const auto& in : blk.instrs) {
            if (in.src1) used.insert(in.src1);
            if (in.src2) used.insert(in.src2);
            for (VReg a : in.args) if (a) used.insert(a);
            // CopyBytes reads its dst VReg as a dest pointer (copy_frame_vptr /
            // copy_global_vptr). The copy_frame_global sentinel lives in src1
            // (not dst), so dst is always a real VReg here when non-zero.
            if (in.op == ThinOp::CopyBytes && in.dst != 0) used.insert(in.dst);
        }
        if (blk.term.cond) used.insert(blk.term.cond);
        if (blk.term.ret)  used.insert(blk.term.ret);
    }
    return used;
}

// A struct-by-value call argument is encoded as a v0 sentinel plus its source
// frame offset. Calls read that storage even though no explicit LoadFrame is
// present in the IR. Struct Type nodes do not carry their layout byte size, so
// conservatively treat stores from the struct base toward rbp as reads. This
// may retain an unrelated earlier local store, but cannot remove a live one.
// args[0] of a struct-returning call is the hidden return DESTINATION and must
// not be treated as an input.
bool call_reads_frame_off(const ThinInstr& in, int32_t off) {
    switch (in.op) {
    case ThinOp::CallNative: case ThinOp::CallScript:
    case ThinOp::CallIndirect: case ThinOp::CallCrossModule:
        break;
    default:
        return false;
    }
    const size_t first = in.ret_type && !in.ret_type->struct_name.empty() ? 1 : 0;
    for (size_t i = first; i < in.args.size(); ++i) {
        const int32_t arg_off = i < in.arg_frame_offs.size() ? in.arg_frame_offs[i] : -1;
        const Type* arg_ty = i < in.arg_types.size() ? in.arg_types[i] : nullptr;
        if (in.args[i] == 0 && arg_off != -1 && arg_ty &&
            !arg_ty->struct_name.empty() && off >= arg_off)
            return true;
    }
    return false;
}

bool is_frame_alias_barrier(const ThinInstr& in);

// Compute the set of frame_off values that are READ by any LoadFrame,
// CopyBytes, FieldAddr, or struct-by-value call argument. CopyBytes reads a byte
// range, and packed struct fields can begin at non-word-aligned offsets (for
// example three i32 fields at base, base+4, base+8). Test the actual StoreFrame
// offsets against that range rather than stepping through it eight bytes at a
// time; the old word-stepping logic missed packed fields and let constprop/DCE
// delete their initializers before a struct return copy.
std::unordered_set<int32_t> compute_read_slots(const ThinFunction& f) {
    std::unordered_set<int32_t> stored;
    for (const auto& blk : f.blocks)
        for (const auto& in : blk.instrs)
            if (in.op == ThinOp::StoreFrame && in.src2 == 0)
                stored.insert(in.meta.frame_off);

    std::unordered_set<int32_t> read;
    for (const auto& blk : f.blocks) {
        for (const auto& in : blk.instrs) {
            if ((in.op == ThinOp::LoadFrame && in.src1 == 0) ||
                in.op == ThinOp::CopyBytes || in.op == ThinOp::FieldAddr)
                read.insert(in.meta.frame_off);
            if (is_frame_alias_barrier(in))
                read.insert(stored.begin(), stored.end());
            if (in.op == ThinOp::CopyBytes && in.meta.len > 0) {
                const int32_t start = in.meta.field_off;
                const int32_t end = start + in.meta.len;
                for (int32_t off : stored)
                    if (off >= start && off < end) read.insert(off);
            }
            for (int32_t off : stored)
                if (call_reads_frame_off(in, off)) read.insert(off);
        }
    }
    return read;
}

// Does `in` READ frame offset `off`? Used by DSE's intra-block dead-store
// scan to kill a pending dead store when a reader appears between two
// StoreFrames to the same slot. Mirrors compute_read_slots's conservative
// reader set: LoadFrame reads meta.frame_off; CopyBytes reads its SOURCE
// range; a struct-by-value call reads its source slot; and FieldAddr reads
// meta.frame_off (the struct base).
bool instr_reads_off(const ThinInstr& in, int32_t off) {
    if (is_frame_alias_barrier(in)) return true;
    if (in.op == ThinOp::LoadFrame)
        return in.meta.frame_off == off;
    if (in.op == ThinOp::FieldAddr)
        return in.meta.frame_off == off;
    if (in.op == ThinOp::CopyBytes) {
        // conservative: dest (meta.frame_off, when frame-relative) counts as a read
        if (in.meta.frame_off == off) return true;
        // source range [field_off, field_off + len): any StoreFrame whose slot
        // lies in this range feeds the copy.
        if (in.meta.len > 0 && off >= in.meta.field_off &&
            off < in.meta.field_off + in.meta.len)
            return true;
        return false;
    }
    return call_reads_frame_off(in, off);
}

// Does `in` WRITE frame offset `off` (other than via StoreFrame, which the
// callers handle directly)? Used by StoreToLoadForward to kill a pending
// store-to-load forward when an intervening instruction writes the slot
// (so a later LoadFrame must re-read the slot, not forward the stale
// StoreFrame's value). CopyBytes writes its DEST range
// [meta.frame_off, meta.frame_off + meta.len) when the dest is
// frame-relative (in.dst == 0 and not global-backed). StoreFrame is handled
// by the caller's own tracking. Other instrs don't write frame slots.
//
// The CopyBytes case is the one StoreToLoadForward previously MISSED: a
// StoreFrame to X, then a CopyBytes whose dest range covers X, then a
// LoadFrame of X — the forward wrongly delivered the StoreFrame's value
// instead of the bytes the CopyBytes wrote (hand-built IR repro confirmed
// the LoadFrame was rewritten to a Move of the stale StoreFrame src).
// Operations whose address/range cannot be represented by the exact-offset
// frame-slot maps used by ConstProp, Forward, and DSE. They conservatively may
// read or write every tracked frame slot. A StoreFrame with src2 != 0 is an
// indirect [src2 + frame_off] store, not a write to the local frame offset.
bool is_frame_alias_barrier(const ThinInstr& in) {
    if (in.op == ThinOp::StoreFrame && in.src2 != 0) return true;
    switch (in.op) {
    case ThinOp::CopyBytes:
    case ThinOp::CallNative:
    case ThinOp::CallScript:
    case ThinOp::CallIndirect:
    case ThinOp::CallCrossModule:
    case ThinOp::StoreAddr:
    case ThinOp::StructLitInit:
    case ThinOp::ArrayLitInit:
        return true;
    default:
        return false;
    }
}

bool instr_writes_off(const ThinInstr& in, int32_t off) {
    auto overlaps = [off](int32_t begin, int32_t bytes) {
        // slot_const tracks scalar slots without a separate width. Treat each
        // tracked slot as the backend's full eight-byte frame cell so partial,
        // packed, or unaligned writes invalidate it too.
        return bytes > 0 && begin < off + 8 && off < begin + bytes;
    };
    if (in.op == ThinOp::CopyBytes) {
        // Dest is frame-relative when in.dst == 0 (no dest vreg) and the dest
        // side is not global-backed. copy_frame_frame / copy_global_frame set
        // meta.frame_off = dst_off; copy_frame_vptr / copy_global_vptr set
        // in.dst to a vreg (dest is [vreg+0], not a frame slot).
        return in.dst == 0 && overlaps(in.meta.frame_off, in.meta.len);
    }
    if (in.op == ThinOp::StoreFrame)
        return in.src2 == 0 && overlaps(in.meta.frame_off,
                                        std::max(in.meta.width, 1));
    if (in.op == ThinOp::StructLitInit || in.op == ThinOp::ArrayLitInit)
        return overlaps(in.meta.frame_off + in.meta.field_off,
                        std::max(in.meta.width, 1));
    if (in.op == ThinOp::StringDecrypt)
        return overlaps(in.meta.data_temp_off, in.meta.len) ||
               overlaps(in.meta.frame_off, 16);
    // An indirect store can alias any address-taken frame slot.
    if (in.op == ThinOp::StoreAddr) return true;

    // Most value producers with a frame_off implicitly pin their result to
    // that frame slot. An ordinary LoadFrame instead READS frame_off; only a
    // computed-address load (src1 != 0) uses it as a result spill slot.
    if (in.dst != 0 && in.meta.frame_off != 0 &&
        (in.op != ThinOp::LoadFrame || in.src1 != 0))
        return overlaps(in.meta.frame_off, std::max(in.meta.width, 1));
    return false;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// ConstPropPass: constant folding + frame-slot const-prop + dead-constant elim
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved ConstPropPass::run(ThinFunction& f, EmberAnalysisManager&) {
    size_t before = count_instrs(f);
    bool changed = false;

    // Per-block constant tracking (local — no inter-block propagation, safe
    // for loops since the loop body block starts fresh).
    struct ConstVal { bool valid; int64_t i; };
    for (auto& blk : f.blocks) {
        std::unordered_map<VReg, ConstVal> vreg_const;     // VReg → known constant
        std::unordered_map<int32_t, ConstVal> slot_const;  // frame_off → known constant

        auto get_vreg_const = [&](VReg v) -> ConstVal {
            auto it = vreg_const.find(v);
            if (it != vreg_const.end()) return it->second;
            return {false, 0};
        };

        for (auto& in : blk.instrs) {
            // Kill constants before processing every write, not just explicit
            // StoreFrame. Thin lowering frame-backs many producer results via
            // meta.frame_off, and aggregate/indirect writes can alias a tracked
            // slot. Unknown calls, copies, and computed stores invalidate the
            // complete exact-offset model; known writes invalidate overlaps.
            if (is_frame_alias_barrier(in)) {
                slot_const.clear();
            } else {
                for (auto it = slot_const.begin(); it != slot_const.end(); ) {
                    if (instr_writes_off(in, it->first)) it = slot_const.erase(it);
                    else ++it;
                }
            }
            switch (in.op) {
            case ThinOp::ConstInt:
                vreg_const[in.dst] = {true, in.imm.i};
                // ConstInt also stores to its own frame slot (meta.frame_off).
                if (in.meta.frame_off != 0)
                    slot_const[in.meta.frame_off] = {true, in.imm.i};
                break;
            case ThinOp::ConstBool:
                vreg_const[in.dst] = {true, in.imm.i};
                if (in.meta.frame_off != 0)
                    slot_const[in.meta.frame_off] = {true, in.imm.i};
                break;
            case ThinOp::StoreFrame: {
                // src2==0 is an exact local slot. A computed store was handled
                // as a full alias barrier above and cannot establish a fact.
                if (in.src2 != 0) break;
                ConstVal v = get_vreg_const(in.src1);
                if (v.valid) slot_const[in.meta.frame_off] = v;
                else         slot_const.erase(in.meta.frame_off);
                break;
            }
            case ThinOp::LoadFrame: {
                // LoadFrame dst=vN off=X: if slot X is constant, mark vN.
                auto it = slot_const.find(in.meta.frame_off);
                if (it != slot_const.end() && it->second.valid) {
                    vreg_const[in.dst] = it->second;
                    if (in.meta.frame_off != 0)
                        slot_const[in.meta.frame_off] = it->second;
                } else {
                    vreg_const.erase(in.dst);
                }
                break;
            }
            case ThinOp::Move: {
                // Move dst=vN src1=vM: propagate constant.
                ConstVal v = get_vreg_const(in.src1);
                if (v.valid) vreg_const[in.dst] = v;
                else         vreg_const.erase(in.dst);
                break;
            }
            default:
                // Binary int ops: fold if both operands are constant.
                if (is_foldable_int_binop(in.op) && in.meta.width != 0) {
                    ConstVal a = get_vreg_const(in.src1);
                    // Operand b: if src2==0, it's the immediate form (imm.i).
                    ConstVal b = (in.src2 == 0)
                        ? ConstVal{true, in.imm.i}
                        : get_vreg_const(in.src2);
                    if (a.valid && b.valid) {
                        // Full fold: both operands constant → ConstInt.
                        int64_t result;
                        if (fold_int_binop(in.op, a.i, b.i, in.meta.width,
                                           in.meta.is_unsigned != 0, &result)) {
                            in.op = ThinOp::ConstInt;
                            in.imm.i = result;
                            in.src1 = 0;
                            in.src2 = 0;
                            vreg_const[in.dst] = {true, result};
                            if (in.meta.frame_off != 0)
                                slot_const[in.meta.frame_off] = {true, result};
                            changed = true;
                            break;
                        }
                    }
                    // Partial: if src2 is a constant VReg (not immediate form),
                    // convert to immediate form (src2=0, imm.i=constant). This
                    // makes the LoadFrame of the constant dead → removable.
                    if (in.src2 != 0 && b.valid && b.i >= -0x7FFFFFFFLL && b.i <= 0x7FFFFFFFLL) {
                        in.imm.i = b.i;
                        in.src2 = 0;
                        changed = true;
                    }
                    // The dst is not a known constant (unless we folded above).
                    if (in.op != ThinOp::ConstInt)
                        vreg_const.erase(in.dst);
                } else {
                    // Any other producing instr: the dst is not a known constant.
                    if (in.dst) vreg_const.erase(in.dst);
                }
                break;
            }
        }
    }

    // Dead-constant sweep: remove ConstInt/ConstBool/LoadFrame/Move whose dst
    // has zero uses, and dead StoreFrame (slot never read). Iterate to fixpoint.
    auto used = compute_used_vregs(f);
    auto read_slots = compute_read_slots(f);
    bool sweep_changed = true;
    while (sweep_changed) {
        sweep_changed = false;
        for (auto& blk : f.blocks) {
            auto& instrs = blk.instrs;
            for (auto it = instrs.begin(); it != instrs.end(); ) {
                ThinInstr& in = *it;
                bool remove = false;
                if (in.op == ThinOp::StoreFrame) {
                    // Only an exact local slot participates in slot liveness.
                    // Computed stores are indirect observable writes.
                    if (in.src2 == 0 &&
                        read_slots.find(in.meta.frame_off) == read_slots.end())
                        remove = true;
                } else if (is_pure(in.op) && in.dst != 0) {
                    // Dead pure def: dst VReg has no uses.
                    if (used.find(in.dst) == used.end())
                        remove = true;
                }
                if (remove) {
                    // If we remove a StoreFrame, its src1 may become dead.
                    // If we remove a pure def, nothing else changes.
                    it = instrs.erase(it);
                    sweep_changed = true;
                    changed = true;
                    // Recompute used + read_slots (conservative — could be
                    // incremental, but the functions are small).
                    used = compute_used_vregs(f);
                    read_slots = compute_read_slots(f);
                } else {
                    ++it;
                }
            }
        }
    }

    if (changed && count_instrs(f) != before)
        return EmberPreserved::none();
    if (changed)
        return EmberPreserved::none();
    return EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// DeadCodeElimPass: remove dead pure instrs + dead local stores
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved DeadCodeElimPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;

    auto read_slots = compute_read_slots(f);

    // Iterate to fixpoint: removing a dead def may make its source dead.
    bool iter_changed = true;
    while (iter_changed) {
        iter_changed = false;
        auto used = compute_used_vregs(f);
        read_slots = compute_read_slots(f);

        for (auto& blk : f.blocks) {
            auto& instrs = blk.instrs;
            for (auto it = instrs.begin(); it != instrs.end(); ) {
                ThinInstr& in = *it;
                bool remove = false;
                if (in.op == ThinOp::StoreFrame) {
                    // Computed StoreFrame is an indirect write and cannot be
                    // removed by exact local-slot liveness.
                    if (in.src2 == 0 &&
                        read_slots.find(in.meta.frame_off) == read_slots.end())
                        remove = true;
                } else if (is_pure(in.op) && in.dst != 0) {
                    // Dead pure def: dst VReg unused.
                    if (used.find(in.dst) == used.end())
                        remove = true;
                }
                // NEVER remove side-effecting instrs.
                if (remove) {
                    it = instrs.erase(it);
                    iter_changed = true;
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// SimplifyCFGPass: constant branches, unreachable blocks, trivial merges
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved SimplifyCFGPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (f.blocks.empty()) return EmberPreserved::all();

    bool changed = false;

    // Constant branch folding is intentionally local and narrow: only a
    // ConstBool (or a chain of Moves from one) in the branch's own block is a
    // known condition. Thin IR is not SSA, so carrying facts across blocks or
    // treating arbitrary ConstInt values as booleans would be unsound.
    for (auto& blk : f.blocks) {
        std::unordered_map<VReg, bool> bool_constants;
        std::unordered_set<VReg> integer_zero;
        for (const auto& in : blk.instrs) {
            if (in.op == ThinOp::ConstBool) {
                if (in.dst) {
                    bool_constants[in.dst] = in.imm.i != 0;
                    integer_zero.erase(in.dst);
                }
            } else if (in.op == ThinOp::ConstInt && in.imm.i == 0) {
                if (in.dst) {
                    integer_zero.insert(in.dst);
                    bool_constants.erase(in.dst);
                }
            } else if (in.op == ThinOp::Move) {
                auto bool_src = bool_constants.find(in.src1);
                if (in.dst) {
                    if (bool_src != bool_constants.end())
                        bool_constants[in.dst] = bool_src->second;
                    else
                        bool_constants.erase(in.dst);
                    if (integer_zero.count(in.src1))
                        integer_zero.insert(in.dst);
                    else
                        integer_zero.erase(in.dst);
                }
            } else if (in.op == ThinOp::Cmp && in.dst &&
                       (in.meta.cmp == 0 || in.meta.cmp == 1)) {
                // If/while lowering compares its boolean expression with an
                // explicit integer zero before branching. Preserve the narrow
                // ConstBool contract while recognizing that exact wrapper.
                auto lhs_bool = bool_constants.find(in.src1);
                auto rhs_bool = bool_constants.find(in.src2);
                bool known = false;
                bool value = false;
                if (lhs_bool != bool_constants.end() &&
                    integer_zero.count(in.src2)) {
                    known = true;
                    value = lhs_bool->second;
                } else if (rhs_bool != bool_constants.end() &&
                           integer_zero.count(in.src1)) {
                    known = true;
                    value = rhs_bool->second;
                }
                if (known) {
                    bool_constants[in.dst] = in.meta.cmp == 0 ? !value : value;
                } else {
                    bool_constants.erase(in.dst);
                }
                integer_zero.erase(in.dst);
            } else if (in.dst) {
                bool_constants.erase(in.dst);
                integer_zero.erase(in.dst);
            }
        }

        if (blk.term.kind != TermKind::Branch) continue;
        auto cond = bool_constants.find(blk.term.cond);
        if (cond == bool_constants.end() &&
            blk.term.target != blk.term.false_target)
            continue;

        const uint32_t taken = blk.term.target == blk.term.false_target
            ? blk.term.target
            : (cond->second ? blk.term.target : blk.term.false_target);
        ThinTerm jmp;
        jmp.kind = TermKind::Jmp;
        jmp.target = taken;
        blk.term = jmp;
        changed = true;
    }

    // Remove every block not reachable from entry. A graph traversal, rather
    // than repeatedly deleting zero-predecessor blocks, is required for dead
    // cycles and also guarantees that loop headers reached by a backedge are
    // retained whenever their loop is entry-reachable.
    {
        std::unordered_map<uint32_t, size_t> id_to_index;
        id_to_index.reserve(f.blocks.size());
        for (size_t i = 0; i < f.blocks.size(); ++i)
            id_to_index[f.blocks[i].id] = i;

        std::vector<uint8_t> reachable(f.blocks.size(), 0);
        std::vector<size_t> work{0};
        reachable[0] = 1;
        while (!work.empty()) {
            const size_t bi = work.back();
            work.pop_back();
            const ThinTerm& term = f.blocks[bi].term;
            auto visit = [&](uint32_t target) {
                auto found = id_to_index.find(target);
                if (found != id_to_index.end() && !reachable[found->second]) {
                    reachable[found->second] = 1;
                    work.push_back(found->second);
                }
            };
            if (term.kind == TermKind::Jmp) {
                visit(term.target);
            } else if (term.kind == TermKind::Branch) {
                visit(term.target);
                visit(term.false_target);
            }
        }

        if (std::find(reachable.begin(), reachable.end(), uint8_t{0}) !=
            reachable.end()) {
            std::vector<ThinBlock> kept;
            kept.reserve(f.blocks.size());
            for (size_t i = 0; i < f.blocks.size(); ++i)
                if (reachable[i]) kept.push_back(std::move(f.blocks[i]));
            f.blocks = std::move(kept);
            changed = true;
        }
    }

    // The emitter indexes labels by block id, so every removal/merge must be
    // followed by ID and edge compaction.
    auto compact_ids = [&]() {
        std::unordered_map<uint32_t, uint32_t> remap;
        remap.reserve(f.blocks.size());
        for (size_t i = 0; i < f.blocks.size(); ++i)
            remap[f.blocks[i].id] = static_cast<uint32_t>(i);
        for (size_t i = 0; i < f.blocks.size(); ++i) {
            ThinBlock& blk = f.blocks[i];
            if (blk.term.kind == TermKind::Jmp) {
                auto target = remap.find(blk.term.target);
                if (target != remap.end()) {
                    changed |= blk.term.target != target->second;
                    blk.term.target = target->second;
                }
            } else if (blk.term.kind == TermKind::Branch) {
                auto target = remap.find(blk.term.target);
                auto false_target = remap.find(blk.term.false_target);
                if (target != remap.end()) {
                    changed |= blk.term.target != target->second;
                    blk.term.target = target->second;
                }
                if (false_target != remap.end()) {
                    changed |= blk.term.false_target != false_target->second;
                    blk.term.false_target = false_target->second;
                }
            }
            changed |= blk.id != static_cast<uint32_t>(i);
            blk.id = static_cast<uint32_t>(i);
        }
    };
    compact_ids();

    // Merge one edge at a time and rebuild graph facts after each merge.
    // In addition to the required "B is not a loop header" rule, this pass is
    // deliberately more conservative and does not merge any block belonging
    // to a natural loop. That prevents moving a latch/backedge (notably its
    // BudgetCheck) into the loop body, the exact value-preservation regression
    // that caused the previous implementation to return 116 instead of 177.
    for (;;) {
        const size_t count = f.blocks.size();
        std::vector<std::unordered_set<uint32_t>> predecessors(count);
        std::vector<uint8_t> loop_header(count, 0);

        for (size_t source = 0; source < count; ++source) {
            const ThinTerm& term = f.blocks[source].term;
            auto add_edge = [&](uint32_t target) {
                if (target >= count) return;
                predecessors[target].insert(static_cast<uint32_t>(source));
                if (target <= source) loop_header[target] = 1;
            };
            if (term.kind == TermKind::Jmp) {
                add_edge(term.target);
            } else if (term.kind == TermKind::Branch) {
                add_edge(term.target);
                add_edge(term.false_target);
            }
        }

        // Mark every natural-loop member by walking predecessors backwards
        // from each latch to its header. This is conservative for irreducible
        // graphs (it may skip a legal merge) but never changes semantics.
        std::vector<uint8_t> in_loop(count, 0);
        for (size_t latch = 0; latch < count; ++latch) {
            const ThinTerm& term = f.blocks[latch].term;
            auto mark_backedge = [&](uint32_t header) {
                if (header >= count || header > latch) return;
                std::vector<uint8_t> this_loop(count, 0);
                this_loop[header] = 1;
                std::vector<uint32_t> stack;
                if (latch != header) stack.push_back(static_cast<uint32_t>(latch));
                while (!stack.empty()) {
                    const uint32_t block = stack.back();
                    stack.pop_back();
                    if (this_loop[block]) continue;
                    this_loop[block] = 1;
                    for (uint32_t pred : predecessors[block])
                        if (!this_loop[pred]) stack.push_back(pred);
                }
                for (size_t i = 0; i < count; ++i)
                    in_loop[i] |= this_loop[i];
            };
            if (term.kind == TermKind::Jmp) {
                mark_backedge(term.target);
            } else if (term.kind == TermKind::Branch) {
                mark_backedge(term.target);
                mark_backedge(term.false_target);
            }
        }

        bool merged = false;
        for (size_t ai = 0; ai < count; ++ai) {
            ThinBlock& a = f.blocks[ai];
            if (a.term.kind != TermKind::Jmp) continue;
            const uint32_t bi = a.term.target;
            if (bi == ai || bi >= count) continue;
            if (predecessors[bi].size() != 1 ||
                !predecessors[bi].count(static_cast<uint32_t>(ai)))
                continue;
            if (loop_header[bi] || in_loop[ai] || in_loop[bi]) continue;

            ThinBlock& b = f.blocks[bi];
            a.instrs.insert(a.instrs.end(),
                            std::make_move_iterator(b.instrs.begin()),
                            std::make_move_iterator(b.instrs.end()));
            a.term = b.term;
            f.blocks.erase(f.blocks.begin() + static_cast<ptrdiff_t>(bi));
            compact_ids();
            changed = true;
            merged = true;
            break;
        }
        if (!merged) break;
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// CSEPass: local global-value-numbering CSE within each basic block
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved CSEPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;

    // Thin IR is not SSA, so value numbering is deliberately reset at each
    // block and a VReg redefinition receives a fresh number. This still finds
    // equivalent expressions through Move chains without requiring dominance
    // or phi construction.
    const auto read_slots = compute_read_slots(f);
    for (auto& blk : f.blocks) {
        struct GVNKey {
            uint16_t op;
            uint64_t lhs, rhs;
            int64_t imm_i;
            uint64_t imm_f_bits;
            int32_t width, frame_off, len, slot, mod_id, field_off, data_temp_off;
            uint8_t cmp, base_kind, is_unsigned, is_f32, trap_reason;
            uint32_t addend;
            const Type* type;
            bool operator==(const GVNKey& o) const {
                return op == o.op && lhs == o.lhs && rhs == o.rhs &&
                       imm_i == o.imm_i && imm_f_bits == o.imm_f_bits &&
                       width == o.width && frame_off == o.frame_off &&
                       len == o.len && slot == o.slot && mod_id == o.mod_id &&
                       field_off == o.field_off && data_temp_off == o.data_temp_off &&
                       cmp == o.cmp && base_kind == o.base_kind &&
                       is_unsigned == o.is_unsigned && is_f32 == o.is_f32 &&
                       trap_reason == o.trap_reason && addend == o.addend &&
                       type == o.type;
            }
        };
        struct GVNKeyHash {
            size_t operator()(const GVNKey& k) const {
                size_t h = k.op;
                auto mix = [&h](size_t value) {
                    h ^= value + 0x9e3779b9u + (h << 6) + (h >> 2);
                };
                mix(std::hash<uint64_t>()(k.lhs));
                mix(std::hash<uint64_t>()(k.rhs));
                mix(std::hash<int64_t>()(k.imm_i));
                mix(std::hash<uint64_t>()(k.imm_f_bits));
                mix(uint32_t(k.width)); mix(uint32_t(k.frame_off)); mix(uint32_t(k.len));
                mix(uint32_t(k.slot)); mix(uint32_t(k.mod_id)); mix(uint32_t(k.field_off));
                mix(uint32_t(k.data_temp_off)); mix(k.cmp); mix(k.base_kind);
                mix(k.is_unsigned); mix(k.is_f32); mix(k.trap_reason); mix(k.addend);
                mix(std::hash<const Type*>()(k.type));
                return h;
            }
        };
        struct Available { uint64_t value_number; VReg representative; };

        std::unordered_map<VReg, uint64_t> vreg_number;
        std::unordered_map<GVNKey, Available, GVNKeyHash> expressions;
        std::unordered_map<uint64_t, VReg> representative;
        uint64_t next_number = 1;

        auto number_of = [&](VReg v) -> uint64_t {
            if (v == 0) return 0;
            auto found = vreg_number.find(v);
            if (found != vreg_number.end()) return found->second;
            // A live-in has a stable identity number for this block.
            const uint64_t number = next_number++;
            vreg_number[v] = number;
            representative[number] = v;
            return number;
        };
        auto erase_memory_entries = [&](int32_t off, bool all) {
            for (auto it = expressions.begin(); it != expressions.end(); ) {
                const bool memory =
                    it->first.op == static_cast<uint16_t>(ThinOp::LoadFrame) ||
                    it->first.op == static_cast<uint16_t>(ThinOp::LoadGlobal);
                if (memory && (all || it->first.op != static_cast<uint16_t>(ThinOp::LoadFrame) ||
                               it->first.frame_off == off))
                    it = expressions.erase(it);
                else
                    ++it;
            }
        };
        auto replace_uses = [&](size_t from, VReg old_vreg, VReg new_vreg) {
            for (size_t i = from; i < blk.instrs.size(); ++i) {
                ThinInstr& use = blk.instrs[i];
                // Stop before either name changes meaning in this non-SSA IR.
                if (use.dst == old_vreg || use.dst == new_vreg) break;
                if (use.src1 == old_vreg) use.src1 = new_vreg;
                if (use.src2 == old_vreg) use.src2 = new_vreg;
                for (VReg& arg : use.args)
                    if (arg == old_vreg) arg = new_vreg;
            }
        };

        for (size_t i = 0; i < blk.instrs.size(); ) {
            ThinInstr& in = blk.instrs[i];

            // An available expression cannot keep a VReg as its physical
            // representative after this instruction redefines that name.
            if (in.dst != 0) {
                for (auto it = expressions.begin(); it != expressions.end(); ) {
                    if (it->second.representative == in.dst)
                        it = expressions.erase(it);
                    else
                        ++it;
                }
            }

            // Calls may mutate memory reachable through arguments or globals.
            if (in.op == ThinOp::CallNative || in.op == ThinOp::CallScript ||
                in.op == ThinOp::CallIndirect || in.op == ThinOp::CallCrossModule)
                erase_memory_entries(0, true);
            else if (in.op == ThinOp::StoreFrame)
                erase_memory_entries(in.meta.frame_off, false);
            else if (in.op == ThinOp::StoreGlobal || in.op == ThinOp::StoreAddr ||
                     in.op == ThinOp::CopyBytes || in.op == ThinOp::StructLitInit ||
                     in.op == ThinOp::ArrayLitInit || in.op == ThinOp::StringDecrypt)
                erase_memory_entries(0, true);

            if (!is_pure(in.op) || in.dst == 0) {
                if (in.dst != 0) {
                    const uint64_t fresh = next_number++;
                    vreg_number[in.dst] = fresh;
                    representative[fresh] = in.dst;
                }
                ++i;
                continue;
            }

            // Moves carry the source's value number and need no expression key.
            if (in.op == ThinOp::Move && in.src1 != 0) {
                const uint64_t number = number_of(in.src1);
                vreg_number[in.dst] = number;
                auto rep = representative.find(number);
                if (rep == representative.end()) representative[number] = in.dst;
                ++i;
                continue;
            }

            uint64_t float_bits = 0;
            static_assert(sizeof(float_bits) == sizeof(in.imm.f));
            std::memcpy(&float_bits, &in.imm.f, sizeof(float_bits));
            uint64_t lhs_number = number_of(in.src1);
            uint64_t rhs_number = number_of(in.src2);
            switch (in.op) {
            case ThinOp::Add: case ThinOp::Mul: case ThinOp::And:
            case ThinOp::Or: case ThinOp::Xor:
                if (rhs_number != 0 && rhs_number < lhs_number)
                    std::swap(lhs_number, rhs_number);
                break;
            default:
                break;
            }
            // For ordinary value producers frame_off is only the destination
            // spill home, not part of the computed value. Ignore it when that
            // home has no explicit IR reader; retain address/load offsets.
            int32_t value_frame_off = in.meta.frame_off;
            switch (in.op) {
            case ThinOp::ConstInt: case ThinOp::ConstFloat: case ThinOp::ConstBool:
            case ThinOp::Add: case ThinOp::Sub: case ThinOp::Mul:
            case ThinOp::Div: case ThinOp::Mod: case ThinOp::And:
            case ThinOp::Or: case ThinOp::Xor: case ThinOp::Shl: case ThinOp::Shr:
            case ThinOp::Neg: case ThinOp::Not: case ThinOp::BitNot:
            case ThinOp::FAdd: case ThinOp::FSub: case ThinOp::FMul:
            case ThinOp::FDiv: case ThinOp::FMod: case ThinOp::Cmp:
            case ThinOp::LAnd: case ThinOp::LOr: case ThinOp::Cast:
                if (!read_slots.count(in.meta.frame_off)) value_frame_off = 0;
                break;
            case ThinOp::LoadFrame:
                // A computed-address LoadFrame uses field_off as its source;
                // frame_off is only its result spill home.
                if (in.src1 != 0 && !read_slots.count(in.meta.frame_off))
                    value_frame_off = 0;
                break;
            default:
                break;
            }
            GVNKey key{
                static_cast<uint16_t>(in.op), lhs_number, rhs_number,
                in.imm.i, float_bits, in.meta.width, value_frame_off,
                in.meta.len, in.meta.slot, in.meta.mod_id, in.meta.field_off,
                in.meta.data_temp_off, in.meta.cmp,
                static_cast<uint8_t>(in.meta.base_kind), in.meta.is_unsigned,
                in.meta.is_f32, in.meta.trap_reason, in.meta.addend, in.meta.type
            };

            auto found = expressions.find(key);
            if (found == expressions.end()) {
                const uint64_t number = next_number++;
                expressions.emplace(key, Available{number, in.dst});
                vreg_number[in.dst] = number;
                representative[number] = in.dst;
                ++i;
                continue;
            }

            // The representative must keep its meaning through every remapped
            // use. If it is redefined later, retain this expression rather than
            // perform a partial substitution in non-SSA IR.
            bool representative_redefined = false;
            for (size_t j = i + 1; j < blk.instrs.size(); ++j) {
                if (blk.instrs[j].dst == found->second.representative) {
                    representative_redefined = true;
                    break;
                }
            }
            if (representative_redefined) {
                const uint64_t number = next_number++;
                vreg_number[in.dst] = number;
                representative[number] = in.dst;
                expressions[key] = {number, in.dst};
                ++i;
                continue;
            }

            const VReg old_dst = in.dst;
            const VReg new_dst = found->second.representative;
            bool old_dst_redefined = false;
            for (size_t j = i + 1; j < blk.instrs.size(); ++j) {
                if (blk.instrs[j].dst == old_dst) {
                    old_dst_redefined = true;
                    break;
                }
            }
            replace_uses(i + 1, old_dst, new_dst);
            if (!old_dst_redefined && blk.term.cond == old_dst)
                blk.term.cond = new_dst;
            if (!old_dst_redefined && blk.term.ret == old_dst)
                blk.term.ret = new_dst;
            vreg_number[old_dst] = found->second.value_number;
            blk.instrs.erase(blk.instrs.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
        }
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// LICMPass: loop-invariant code motion
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved LICMPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    const size_t num_blocks = f.blocks.size();
    if (num_blocks < 3) return EmberPreserved::all();  // no loop possible

    // Build a block-id → index map (block ids may not be sequential).
    std::unordered_map<uint32_t, size_t> id_to_idx;
    for (size_t i = 0; i < num_blocks; ++i)
        id_to_idx[f.blocks[i].id] = i;

    // Build predecessor/successor maps. Invalid targets are ignored here; the
    // validator diagnoses them, while an optimization pass must not index past
    // the block vector on malformed hand-built IR.
    std::vector<std::vector<uint32_t>> preds(num_blocks), succs(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i) {
        const auto& blk = f.blocks[i];
        auto add_edge = [&](uint32_t target) {
            auto found = id_to_idx.find(target);
            if (found == id_to_idx.end()) return;
            const uint32_t target_idx = static_cast<uint32_t>(found->second);
            succs[i].push_back(target_idx);
            preds[target_idx].push_back(static_cast<uint32_t>(i));
        };
        if (blk.term.kind == TermKind::Jmp) {
            add_edge(blk.term.target);
        } else if (blk.term.kind == TermKind::Branch) {
            add_edge(blk.term.target);
            add_edge(blk.term.false_target);
        }
    }

    // Compute entry reachability and dominators. A natural-loop backedge is an
    // edge latch -> header for which header dominates latch; block ordering is
    // not a CFG property and cannot safely identify loops.
    std::vector<uint8_t> reachable(num_blocks, 0);
    std::vector<uint32_t> work{0};
    reachable[0] = 1;
    while (!work.empty()) {
        const uint32_t block = work.back();
        work.pop_back();
        for (uint32_t succ : succs[block]) {
            if (!reachable[succ]) {
                reachable[succ] = 1;
                work.push_back(succ);
            }
        }
    }

    std::vector<std::vector<uint8_t>> dominates(
        num_blocks, std::vector<uint8_t>(num_blocks, 0));
    for (size_t block = 0; block < num_blocks; ++block) {
        if (!reachable[block]) continue;
        if (block == 0) {
            dominates[block][0] = 1;
        } else {
            for (size_t candidate = 0; candidate < num_blocks; ++candidate)
                dominates[block][candidate] = reachable[candidate];
        }
    }
    bool dom_changed = true;
    while (dom_changed) {
        dom_changed = false;
        for (size_t block = 1; block < num_blocks; ++block) {
            if (!reachable[block]) continue;
            std::vector<uint8_t> next(num_blocks, 1);
            bool have_pred = false;
            for (uint32_t pred : preds[block]) {
                if (!reachable[pred]) continue;
                if (!have_pred) {
                    next = dominates[pred];
                    have_pred = true;
                } else {
                    for (size_t candidate = 0; candidate < num_blocks; ++candidate)
                        next[candidate] &= dominates[pred][candidate];
                }
            }
            if (!have_pred) std::fill(next.begin(), next.end(), uint8_t{0});
            next[block] = 1;
            if (next != dominates[block]) {
                dominates[block] = std::move(next);
                dom_changed = true;
            }
        }
    }

    struct Loop { uint32_t header_idx; uint32_t latch_idx; std::set<uint32_t> body; };
    std::vector<Loop> loops;
    for (size_t latch = 0; latch < num_blocks; ++latch) {
        if (!reachable[latch]) continue;
        for (uint32_t header : succs[latch]) {
            if (!dominates[latch][header]) continue;
            Loop loop;
            loop.header_idx = header;
            loop.latch_idx = static_cast<uint32_t>(latch);
            loop.body.insert(header);
            std::vector<uint32_t> stack{static_cast<uint32_t>(latch)};
            while (!stack.empty()) {
                const uint32_t block = stack.back();
                stack.pop_back();
                if (block == header || loop.body.count(block)) continue;
                loop.body.insert(block);
                for (uint32_t pred : preds[block])
                    if (reachable[pred] && !loop.body.count(pred))
                        stack.push_back(pred);
            }
            loops.push_back(std::move(loop));
        }
    }

    if (loops.empty()) return EmberPreserved::all();

    // For each loop, find the pre-header (the single non-loop predecessor of
    // the header). If there are 0 or >1 non-loop predecessors, skip (no safe
    // pre-header for hoisting).
    //
    // Then identify loop-invariant instructions in the loop body and hoist
    // them to the pre-header (at the end of its instrs, before the terminator).
    //
    // Only instructions that are unconditionally safe to speculate may move
    // from a conditional/zero-trip loop into its preheader. Keep this whitelist
    // intentionally small: constants and integer Add/Sub/Mul cannot trap and
    // have no language-visible side effects. In particular, Div/Mod, calls,
    // guards, mutable loads, addresses, casts, comparisons, and float ops are
    // not hoisted. A future memory-aware LICM may admit LoadFrame only after
    // proving both non-aliasing writes and must-execute placement.

    for (const auto& loop : loops) {
        // Find the pre-header.
        uint32_t pre_header = UINT32_MAX;
        int non_loop_preds = 0;
        for (uint32_t p : preds[loop.header_idx]) {
            if (!loop.body.count(p)) {
                non_loop_preds++;
                pre_header = p;
            }
        }
        if (non_loop_preds != 1 || pre_header == UINT32_MAX)
            continue;  // no safe pre-header

        // Record all definitions so an operand is accepted only when every
        // possible definition is outside the loop and dominates the preheader.
        // This is conservative for non-SSA VRegs, but never invents a reaching
        // value merely because its numerically named VReg appears elsewhere.
        std::unordered_map<VReg, std::vector<uint32_t>> def_blocks;
        std::set<VReg> loop_def_vregs;
        for (size_t bi = 0; bi < num_blocks; ++bi) {
            for (const auto& in : f.blocks[bi].instrs) {
                if (!in.dst || in.op == ThinOp::CopyBytes) continue;
                def_blocks[in.dst].push_back(static_cast<uint32_t>(bi));
                if (loop.body.count(static_cast<uint32_t>(bi)))
                    loop_def_vregs.insert(in.dst);
            }
        }

        auto is_invariant_vreg = [&](VReg v) -> bool {
            if (v == 0) return true;
            if (loop_def_vregs.count(v)) return false;
            auto defs = def_blocks.find(v);
            if (defs == def_blocks.end()) return true; // function live-in
            for (uint32_t def_block : defs->second)
                if (!dominates[pre_header][def_block]) return false;
            return true;
        };

        auto is_invariant_instr = [&](const ThinInstr& in) -> bool {
            switch (in.op) {
            case ThinOp::ConstInt:
            case ThinOp::ConstBool:
            case ThinOp::ConstFloat:
                return true;
            case ThinOp::Add:
            case ThinOp::Sub:
            case ThinOp::Mul:
                return is_invariant_vreg(in.src1) &&
                       is_invariant_vreg(in.src2);
            default:
                return false;
            }
        };

        // Collect hoistable instructions from the loop body (excluding the
        // header — don't hoist from the header because the header may execute
        // differently on the first vs subsequent iterations).
        std::vector<std::pair<uint32_t, size_t>> to_hoist;  // (block_idx, instr_idx)
        for (uint32_t bi : loop.body) {
            if (bi == loop.header_idx) continue;  // skip header
            auto& instrs = f.blocks[bi].instrs;
            for (size_t ii = 0; ii < instrs.size(); ++ii) {
                if (is_invariant_instr(instrs[ii]))
                    to_hoist.push_back({bi, ii});
            }
        }

        if (to_hoist.empty()) continue;

        // Preserve the original instruction order in the preheader. Collect
        // copies in forward block/instruction order, erase each source block
        // from the back to keep indices valid, then append the copies forward.
        std::sort(to_hoist.begin(), to_hoist.end());
        std::vector<ThinInstr> hoisted;
        hoisted.reserve(to_hoist.size());
        for (const auto& [bi, ii] : to_hoist)
            hoisted.push_back(f.blocks[bi].instrs[ii]);
        for (auto it = to_hoist.rbegin(); it != to_hoist.rend(); ++it) {
            const auto [bi, ii] = *it;
            f.blocks[bi].instrs.erase(
                f.blocks[bi].instrs.begin() + static_cast<ptrdiff_t>(ii));
        }
        auto& pre_hdr_instrs = f.blocks[pre_header].instrs;
        pre_hdr_instrs.insert(pre_hdr_instrs.end(),
                              std::make_move_iterator(hoisted.begin()),
                              std::make_move_iterator(hoisted.end()));
        changed = true;
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// StoreToLoadForwardPass: intra-block store-to-load forwarding
// ═══════════════════════════════════════════════════════════════════════════
//
// The IR backend stores every VReg to a frame slot (StoreFrame src1=vN off=X),
// then the next instruction loads it back (LoadFrame dst=vD off=X). This
// round-trip through memory is the #1 reason the IR backend is 1.2-1.9× slower
// than the tree-walker.
//
// This pass replaces LoadFrame dst=vD off=X with Move dst=vD src1=vN when a
// StoreFrame src1=vN off=X is the last writer to slot X (no intervening
// StoreFrame to the same offset). The emit's rax_vreg cache then keeps the
// result in rax (no frame load). Kill rule: a StoreFrame to offset X kills
// any pending forwarding for X. Only forwards within a block (no inter-block).

EmberPreserved StoreToLoadForwardPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        // Map: frame_off → VReg of the last StoreFrame's src1.
        std::unordered_map<int32_t, VReg> last_store_src;
        for (auto& in : blk.instrs) {
            // Thin IR is SSA-like but deserialized/hand-built IR and later
            // transforms are not required to preserve strict SSA. Forwarding
            // the VReg name after it is redefined would read the NEW value,
            // not the value captured by the earlier store. CopyBytes is the
            // exception: its dst field is an input destination pointer.
            if (in.dst != 0 && in.op != ThinOp::CopyBytes) {
                for (auto it = last_store_src.begin(); it != last_store_src.end(); ) {
                    if (it->second == in.dst) it = last_store_src.erase(it);
                    else ++it;
                }
            }
            // Unknown calls/copies/addresses and computed stores may overwrite
            // any tracked slot. They are barriers, and a computed StoreFrame
            // must never establish an exact frame_off forwarding fact.
            if (is_frame_alias_barrier(in)) {
                last_store_src.clear();
                continue;
            }
            if (in.op == ThinOp::StoreFrame) {
                // Record an exact local store (or update it).
                last_store_src[in.meta.frame_off] = in.src1;
                continue;
            }
            if (in.op == ThinOp::LoadFrame) {
                // Check if there's a pending store to this slot.
                auto it = last_store_src.find(in.meta.frame_off);
                if (it != last_store_src.end() && it->second != 0) {
                    // Forward: replace LoadFrame with Move.
                    in.op = ThinOp::Move;
                    in.src1 = it->second;
                    in.src2 = 0;
                    // Keep meta.frame_off (emit still uses it for the VReg's
                    // frame-backed status — the Move will store to the same slot).
                    changed = true;
                }
                // A LoadFrame does NOT kill the store (the value is still there).
                continue;
            }
            // An instruction that WRITES the frame slot directly
            // (CopyBytes whose dest range covers the slot) overwrites the
            // stored value, so a later LoadFrame must re-read the slot (not
            // forward the now-stale StoreFrame src). Kill any pending forward
            // whose slot lies in the writer's dest range. (StoreFrame to a
            // DIFFERENT slot doesn't alias — v1 frame slots don't overlap,
            // each local gets its own non-overlapping region.) This closes the
            // CopyBytes-dest gap: StoreFrame@X; CopyBytes(dst covers X);
            // LoadFrame@X previously forwarded the stale StoreFrame value
            // instead of the bytes the CopyBytes wrote.
            if (!last_store_src.empty()) {
                for (auto it = last_store_src.begin(); it != last_store_src.end(); ) {
                    if (instr_writes_off(in, it->first))
                        it = last_store_src.erase(it);
                    else
                        ++it;
                }
            }
        }
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// CopyPropPass: intra-block copy propagation
// ═══════════════════════════════════════════════════════════════════════════
//
// After store-to-load forwarding creates Move instrs, copy propagation replaces
// uses of the Move's dst with its src. Then DCE removes the dead Move.
//
// Move dst=vD src1=vN → for all subsequent instructions in the same block (until
// vD or vN is redefined), replace any use of vD with vN.

EmberPreserved CopyPropPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        // Map: VReg → the VReg it's a copy of (from a Move).
        std::unordered_map<VReg, VReg> copy_map;
        auto resolve = [&](VReg v) -> VReg {
            // Follow copy chains (v → v2 → v3 ...) to the root. Malformed or
            // transient cyclic maps must not spin forever.
            VReg cur = v;
            std::unordered_set<VReg> visited;
            while (true) {
                auto it = copy_map.find(cur);
                if (it == copy_map.end() || !visited.insert(cur).second) break;
                cur = it->second;
            }
            return cur;
        };
        for (auto& in : blk.instrs) {
            // First: replace uses of this instr's src1/src2/args with their
            // copy-propagated roots.
            if (in.src1 && copy_map.count(in.src1)) {
                in.src1 = resolve(in.src1);
                changed = true;
            }
            if (in.src2 && copy_map.count(in.src2)) {
                in.src2 = resolve(in.src2);
                changed = true;
            }
            for (auto& a : in.args) {
                if (a && copy_map.count(a)) {
                    a = resolve(a);
                    changed = true;
                }
            }
            // Handle the terminator too (cond/ret are in term, but term is
            // separate from instrs — handled after the loop below).

            // Now handle this instr's dst:
            if (in.dst) {
                // If this is a Move, record the copy.
                if (in.op == ThinOp::Move && in.src1) {
                    copy_map[in.dst] = resolve(in.src1);
                } else {
                    // Any other producing instr kills the copy for its dst.
                    copy_map.erase(in.dst);
                }
                // Also kill any copy entry whose VALUE depends on this dst
                // (i.e., if this dst was used as a src in a prior Move).
                for (auto it = copy_map.begin(); it != copy_map.end(); ) {
                    if (it->second == in.dst) it = copy_map.erase(it);
                    else ++it;
                }
            }
        }
        // Handle the terminator's cond/ret.
        if (blk.term.cond && copy_map.count(blk.term.cond)) {
            blk.term.cond = resolve(blk.term.cond);
            changed = true;
        }
        if (blk.term.ret && copy_map.count(blk.term.ret)) {
            blk.term.ret = resolve(blk.term.ret);
            changed = true;
        }
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// InstCombinePass: intra-block identity-fold of binary ops
// ═══════════════════════════════════════════════════════════════════════════
//
// Identity-fold binary ops where one operand is a known constant. Iterate
// within each block, tracking a VReg→constant-value map from ConstInt/
// ConstBool/ConstFloat defs. When a BinOp has a constant operand that makes
// it an identity, replace it with a Move (or ConstInt 0 for self-annihilation).
// Keep meta.frame_off on the replacement Move so emit still treats the dst as
// frame-backed. Kill a constant-map entry when its VReg is redefined by a
// non-ConstInt instr.

EmberPreserved InstCombinePass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        // VReg → known constant int value. Only int constants are tracked
        // (the identity folds below are all integer ops). ConstFloat is not
        // tracked here — float identity folds are a separate concern.
        std::unordered_map<VReg, int64_t> vreg_const;
        auto is_const = [&](VReg v) -> bool {
            return v != 0 && vreg_const.count(v);
        };
        auto get_const = [&](VReg v) -> int64_t {
            auto it = vreg_const.find(v);
            return it != vreg_const.end() ? it->second : 0;
        };

        for (auto& in : blk.instrs) {
            // Record constants from ConstInt / ConstBool defs.
            if (in.op == ThinOp::ConstInt || in.op == ThinOp::ConstBool) {
                if (in.dst) vreg_const[in.dst] = in.imm.i;
                continue;
            }

            // The set of foldable int binops (matches ConstPropPass's set).
            if (!is_foldable_int_binop(in.op)) {
                // Non-foldable producing instr: kill the dst's constant entry.
                if (in.dst) vreg_const.erase(in.dst);
                continue;
            }

            // Resolve the two operands. src2==0 means the immediate form
            // (the constant is in imm.i) — that's a known constant too.
            bool a_const = is_const(in.src1);
            bool b_const = (in.src2 == 0) || is_const(in.src2);
            int64_t a_val = get_const(in.src1);
            int64_t b_val = (in.src2 == 0) ? in.imm.i : get_const(in.src2);

            // Helper: build a Move dst=dst src1=other, keeping meta.frame_off.
            auto make_move = [&](VReg other) {
                in.op = ThinOp::Move;
                in.src1 = other;
                in.src2 = 0;
                // imm is irrelevant for Move; leave meta intact (frame_off,
                // width, etc. — emit still treats the dst as frame-backed).
            };
            auto make_const0 = [&]() {
                in.op = ThinOp::ConstInt;
                in.imm.i = 0;
                in.src1 = 0;
                in.src2 = 0;
                // Keep meta (frame_off so emit still backs the dst).
            };

            bool folded = false;
            switch (in.op) {
            case ThinOp::Add:
                // x+0 → x, 0+x → x
                if (b_const && b_val == 0) { make_move(in.src1); folded = true; }
                else if (a_const && a_val == 0) { make_move(in.src2); folded = true; }
                break;
            case ThinOp::Sub:
                // x-0 → x
                if (b_const && b_val == 0) { make_move(in.src1); folded = true; }
                // x-x → 0 (both operands the same VReg)
                else if (in.src1 != 0 && in.src1 == in.src2) { make_const0(); folded = true; }
                break;
            case ThinOp::Mul:
                // x*1 → x, 1*x → x
                if (b_const && b_val == 1) { make_move(in.src1); folded = true; }
                else if (a_const && a_val == 1) { make_move(in.src2); folded = true; }
                // x*0 → 0, 0*x → 0 (either operand const 0)
                else if ((a_const && a_val == 0) || (b_const && b_val == 0)) {
                    make_const0(); folded = true;
                }
                break;
            case ThinOp::Div:
                // x/1 → x. Do NOT fold x/0.
                if (b_const && b_val == 1) { make_move(in.src1); folded = true; }
                break;
            case ThinOp::Or:
                // x|0 → x, 0|x → x
                if (b_const && b_val == 0) { make_move(in.src1); folded = true; }
                else if (a_const && a_val == 0) { make_move(in.src2); folded = true; }
                // x|x → x
                else if (in.src1 != 0 && in.src1 == in.src2) { make_move(in.src1); folded = true; }
                break;
            case ThinOp::And:
                // x&-1 → x (all-ones), -1&x → x
                if (b_const && b_val == -1) { make_move(in.src1); folded = true; }
                else if (a_const && a_val == -1) { make_move(in.src2); folded = true; }
                // x&x → x
                else if (in.src1 != 0 && in.src1 == in.src2) { make_move(in.src1); folded = true; }
                // x&0 → 0, 0&x → 0
                else if ((a_const && a_val == 0) || (b_const && b_val == 0)) {
                    make_const0(); folded = true;
                }
                break;
            case ThinOp::Xor:
                // x^0 → x, 0^x → x
                if (b_const && b_val == 0) { make_move(in.src1); folded = true; }
                else if (a_const && a_val == 0) { make_move(in.src2); folded = true; }
                // x^x → 0
                else if (in.src1 != 0 && in.src1 == in.src2) { make_const0(); folded = true; }
                break;
            case ThinOp::Shl:
            case ThinOp::Shr:
                // x<<0 → x, x>>0 → x. Do NOT fold shift-by-const-nonzero here.
                if (b_const && b_val == 0) { make_move(in.src1); folded = true; }
                break;
            default:
                break;
            }

            if (folded) {
                changed = true;
                // The dst is now either a Move of an existing VReg (not a known
                // constant in general) or a ConstInt 0. Update the map.
                if (in.op == ThinOp::ConstInt) {
                    if (in.dst) vreg_const[in.dst] = in.imm.i;
                } else {
                    // Move: propagate the constant if the src is constant.
                    if (in.dst) {
                        if (is_const(in.src1)) vreg_const[in.dst] = get_const(in.src1);
                        else vreg_const.erase(in.dst);
                    }
                }
            } else {
                // Not folded: the BinOp's dst is not a known constant.
                if (in.dst) vreg_const.erase(in.dst);
            }
        }
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// DeadStoreElimPass: intra-block dead store elimination
// ═══════════════════════════════════════════════════════════════════════════
//
// Within each block, track the last StoreFrame to each frame_off. When a
// SECOND StoreFrame to the same off=X appears with NO intervening LoadFrame
// off=X, the FIRST StoreFrame was overwritten before being read → it's dead,
// remove it. The second then becomes the new "last store". Iterate to fixpoint
// within each block.

EmberPreserved DeadStoreElimPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        bool block_changed = true;
        while (block_changed) {
            block_changed = false;
            // Map: frame_off → index of the last StoreFrame to that off (in the
            // current instr vector). A reader of the same off (LoadFrame,
            // CopyBytes source range, or FieldAddr base — see instr_reads_off)
            // kills the pending store (it was read, so it's not dead).
            std::unordered_map<int32_t, size_t> last_store_idx;
            auto& instrs = blk.instrs;
            for (size_t i = 0; i < instrs.size(); ++i) {
                ThinInstr& in = instrs[i];
                // These operations may read every frame slot, so every pending
                // store is live and cannot be removed as overwritten later.
                if (is_frame_alias_barrier(in)) {
                    last_store_idx.clear();
                    continue;
                }
                if (in.op == ThinOp::StoreFrame) {
                    // Alias-barrier handling above excludes computed stores;
                    // only exact local slots reach this tracking path.
                    int32_t off = in.meta.frame_off;
                    auto it = last_store_idx.find(off);
                    if (it != last_store_idx.end()) {
                        // A prior StoreFrame to this off with no intervening
                        // reader → the prior store is dead. Remove it.
                        size_t dead_idx = it->second;
                        instrs.erase(instrs.begin() + ptrdiff_t(dead_idx));
                        // We erased an earlier instr; the current index i has
                        // shifted down by 1. Restart this block's scan.
                        block_changed = true;
                        changed = true;
                        break;
                    }
                    last_store_idx[off] = i;
                } else {
                    // Any reader of a tracked off kills that pending store
                    // (the slot was read between the two stores, so the first
                    // store is NOT dead). This covers LoadFrame, CopyBytes's
                    // source range, and FieldAddr's base — the same reader set
                    // compute_read_slots uses (DCE/ConstProp already honor it;
                    // DSE previously honored ONLY LoadFrame, missing CopyBytes/
                    // FieldAddr, which removed a store that fed a CopyBytes).
                    for (auto it = last_store_idx.begin(); it != last_store_idx.end(); ) {
                        if (instr_reads_off(in, it->first))
                            it = last_store_idx.erase(it);
                        else
                            ++it;
                    }
                }
            }
        }
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// BoundsCheckElimPass: canonical fixed-array loop bounds-check elimination
// ═══════════════════════════════════════════════════════════════════════════
//
// This intentionally recognizes only the simple loop shape emitted for
//
//   i = 0; while (i < N) { ... a[i] ...; i = i + 1; }
//
// The pass removes a fixed-array BoundsCheck only after proving all of:
//   * the header's controlling comparison is the signed/unsigned i < N;
//   * i is loaded from one frame slot initialized to zero in the preheader;
//   * the body has one non-wrapping +1 update to that slot, after the access;
//   * the CFG is a single body and latch with no alternate entries/exits;
//   * no call, unknown copy, or indirect write can change the induction slot;
//   * the check length is the same immediate N and its IndexAddr is fixed-base.
//
// This narrow proof is deliberate: a missed optimization costs performance,
// while a false positive turns a recoverable bounds trap into memory unsafety.

namespace {

const ThinInstr* last_def_before(const ThinBlock& blk, VReg v, size_t before,
                                 size_t* index = nullptr) {
    if (v == 0) return nullptr;
    before = std::min(before, blk.instrs.size());
    for (size_t i = before; i-- > 0;) {
        if (blk.instrs[i].dst == v) {
            if (index) *index = i;
            return &blk.instrs[i];
        }
    }
    return nullptr;
}

bool const_before(const ThinBlock& blk, VReg v, size_t before, int64_t* value) {
    // Follow only local Move chains. Thin IR is not SSA, so each lookup is
    // relative to the use and therefore observes the last reaching local def.
    for (unsigned depth = 0; depth < 8 && v != 0; ++depth) {
        size_t def_index = 0;
        const ThinInstr* def = last_def_before(blk, v, before, &def_index);
        if (!def) return false;
        if (def->op == ThinOp::ConstInt || def->op == ThinOp::ConstBool) {
            *value = def->imm.i;
            return true;
        }
        if (def->op != ThinOp::Move || def->src1 == 0) return false;
        v = def->src1;
        before = def_index;
    }
    return false;
}

bool load_of_slot_before(const ThinBlock& blk, VReg v, size_t before,
                         int32_t slot, size_t* load_index = nullptr) {
    size_t def_index = 0;
    const ThinInstr* def = last_def_before(blk, v, before, &def_index);
    if (!def || def->op != ThinOp::LoadFrame || def->src1 != 0 ||
        def->meta.frame_off != slot)
        return false;
    if (load_index) *load_index = def_index;
    return true;
}

bool intervals_overlap(int64_t a_begin, int64_t a_size,
                       int64_t b_begin, int64_t b_size) {
    if (a_size <= 0 || b_size <= 0) return true;
    const int64_t a_end = a_begin + a_size;
    const int64_t b_end = b_begin + b_size;
    return a_begin < b_end && b_begin < a_end;
}

int64_t frame_store_bytes(const ThinInstr& in) {
    if (in.meta.type && in.meta.type->is_slice) return 16;
    // Ordinary scalar locals occupy eight-byte homes. field_off is the
    // lowerer's marker for an exact-width packed aggregate-field write.
    return in.meta.field_off != 0 ? std::max<int32_t>(1, in.meta.width) : 8;
}

bool bound_fits_compare(int64_t bound, int32_t width, bool is_unsigned) {
    if (bound <= 0 || (width != 1 && width != 2 && width != 4 && width != 8))
        return false;
    if (width == 8) return true; // positive int64 fits either i64 or u64
    const unsigned bits = static_cast<unsigned>(width * 8);
    const uint64_t max = is_unsigned
        ? ((uint64_t{1} << bits) - 1)
        : ((uint64_t{1} << (bits - 1)) - 1);
    return static_cast<uint64_t>(bound) <= max;
}

} // namespace

EmberPreserved BoundsCheckElimPass::run(ThinFunction& f,
                                         EmberAnalysisManager&) {
    if (f.blocks.size() < 4) return EmberPreserved::all();

    std::unordered_map<uint32_t, size_t> id_to_index;
    id_to_index.reserve(f.blocks.size());
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        if (!id_to_index.emplace(f.blocks[i].id, i).second)
            return EmberPreserved::all(); // malformed/ambiguous CFG: no transform
    }

    std::vector<std::vector<size_t>> preds(f.blocks.size());
    auto add_pred = [&](size_t source, uint32_t target) -> bool {
        auto found = id_to_index.find(target);
        if (found == id_to_index.end()) return false;
        preds[found->second].push_back(source);
        return true;
    };
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        const ThinTerm& term = f.blocks[i].term;
        if (term.kind == TermKind::Jmp) {
            if (!add_pred(i, term.target)) return EmberPreserved::all();
        } else if (term.kind == TermKind::Branch) {
            if (!add_pred(i, term.target) || !add_pred(i, term.false_target))
                return EmberPreserved::all();
        }
    }

    bool changed = false;

    for (size_t hi = 0; hi < f.blocks.size(); ++hi) {
        ThinBlock& header = f.blocks[hi];
        if (header.term.kind != TermKind::Branch || preds[hi].size() != 2)
            continue;

        // Recover lowering's boolean wrapper:
        //   inner = (i < N); wrapped = (inner == 0);
        //   branch wrapped ? exit : body.
        size_t wrapper_index = 0;
        const ThinInstr* wrapper = last_def_before(
            header, header.term.cond, header.instrs.size(), &wrapper_index);
        if (!wrapper || wrapper->op != ThinOp::Cmp || wrapper->meta.cmp != 0)
            continue;

        VReg inner_vreg = 0;
        int64_t zero = 1;
        if (const_before(header, wrapper->src1, wrapper_index, &zero) && zero == 0)
            inner_vreg = wrapper->src2;
        else if (const_before(header, wrapper->src2, wrapper_index, &zero) && zero == 0)
            inner_vreg = wrapper->src1;
        if (inner_vreg == 0) continue;

        size_t compare_index = 0;
        const ThinInstr* compare = last_def_before(
            header, inner_vreg, wrapper_index, &compare_index);
        if (!compare || compare->op != ThinOp::Cmp || compare->meta.cmp != 2)
            continue; // v1 accepts exactly i < N

        const ThinInstr* iv_load = last_def_before(
            header, compare->src1, compare_index);
        if (!iv_load || iv_load->op != ThinOp::LoadFrame || iv_load->src1 != 0)
            continue;
        const int32_t iv_slot = iv_load->meta.frame_off;
        if (iv_slot == 0) continue;

        int64_t bound = 0;
        const bool constant_bound = compare->src2 == 0
            ? (bound = compare->imm.i, true)
            : const_before(header, compare->src2, compare_index, &bound);
        if (!constant_bound ||
            !bound_fits_compare(bound, compare->meta.width,
                                compare->meta.is_unsigned != 0))
            continue;

        auto body_found = id_to_index.find(header.term.false_target);
        auto exit_found = id_to_index.find(header.term.target);
        if (body_found == id_to_index.end() || exit_found == id_to_index.end())
            continue;
        const size_t bi = body_found->second;
        if (bi == hi || preds[bi].size() != 1 || preds[bi][0] != hi)
            continue;
        ThinBlock& body = f.blocks[bi];
        if (body.term.kind != TermKind::Jmp) continue;

        auto latch_found = id_to_index.find(body.term.target);
        if (latch_found == id_to_index.end()) continue;
        const size_t li = latch_found->second;
        if (li == hi || li == bi || preds[li].size() != 1 || preds[li][0] != bi)
            continue;
        const ThinBlock& latch = f.blocks[li];
        if (latch.term.kind != TermKind::Jmp || latch.term.target != header.id)
            continue;

        // The other header predecessor must be a unique preheader that jumps
        // directly to the header. This excludes alternate loop entries.
        size_t pi = f.blocks.size();
        for (size_t pred : preds[hi]) {
            if (pred == li) continue;
            if (pi != f.blocks.size()) { pi = f.blocks.size(); break; }
            pi = pred;
        }
        if (pi == f.blocks.size()) continue;
        const ThinBlock& preheader = f.blocks[pi];
        if (preheader.term.kind != TermKind::Jmp ||
            preheader.term.target != header.id)
            continue;

        // Require a reaching `StoreFrame slot, 0` in the preheader. The last
        // exact store is the one that reaches the loop; reject unknown writes
        // after it rather than attempting alias analysis here.
        size_t init_index = preheader.instrs.size();
        for (size_t i = preheader.instrs.size(); i-- > 0;) {
            const ThinInstr& in = preheader.instrs[i];
            if (in.op == ThinOp::StoreFrame && in.src2 == 0 &&
                in.meta.frame_off == iv_slot) {
                init_index = i;
                break;
            }
        }
        if (init_index == preheader.instrs.size()) continue;
        int64_t initial = 1;
        if (!const_before(preheader, preheader.instrs[init_index].src1,
                          init_index, &initial) || initial != 0)
            continue;
        bool unsafe_after_init = false;
        for (size_t i = init_index + 1; i < preheader.instrs.size(); ++i) {
            const ThinInstr& in = preheader.instrs[i];
            if ((in.op == ThinOp::StoreFrame &&
                 intervals_overlap(in.meta.frame_off, frame_store_bytes(in),
                                   iv_slot, 8)) ||
                in.op == ThinOp::CopyBytes || in.op == ThinOp::StructLitInit ||
                in.op == ThinOp::ArrayLitInit || in.op == ThinOp::StringDecrypt ||
                in.op == ThinOp::CallNative || in.op == ThinOp::CallScript ||
                in.op == ThinOp::CallIndirect || in.op == ThinOp::CallCrossModule ||
                in.op == ThinOp::StoreAddr) {
                unsafe_after_init = true;
                break;
            }
        }
        if (unsafe_after_init) continue;

        // Find the sole IV update and prove it is `slot = load(slot) + 1`.
        size_t update_index = body.instrs.size();
        unsigned iv_stores = 0;
        bool unsafe_loop_write = false;
        auto inspect_writes = [&](const ThinBlock& blk, bool is_body) {
            for (size_t i = 0; i < blk.instrs.size(); ++i) {
                const ThinInstr& in = blk.instrs[i];
                if (in.op == ThinOp::StoreFrame) {
                    if (in.src2 != 0) { unsafe_loop_write = true; continue; }
                    if (intervals_overlap(in.meta.frame_off, frame_store_bytes(in),
                                          iv_slot, 8)) {
                        ++iv_stores;
                        if (is_body && in.meta.frame_off == iv_slot)
                            update_index = i;
                        else
                            unsafe_loop_write = true;
                    }
                } else if (in.op == ThinOp::StoreAddr) {
                    // Body stores are checked below against their IndexAddr
                    // provenance. A header/latch indirect store has no such
                    // canonical proof and may alias the induction slot.
                    if (!is_body) unsafe_loop_write = true;
                } else if (in.op == ThinOp::CopyBytes ||
                           in.op == ThinOp::StructLitInit ||
                           in.op == ThinOp::ArrayLitInit ||
                           in.op == ThinOp::StringDecrypt ||
                           in.op == ThinOp::CallNative ||
                           in.op == ThinOp::CallScript ||
                           in.op == ThinOp::CallIndirect ||
                           in.op == ThinOp::CallCrossModule) {
                    unsafe_loop_write = true;
                }
            }
        };
        inspect_writes(header, false);
        inspect_writes(body, true);
        inspect_writes(latch, false);
        if (unsafe_loop_write || iv_stores != 1 ||
            update_index == body.instrs.size())
            continue;

        const ThinInstr& update_store = body.instrs[update_index];
        size_t add_index = 0;
        const ThinInstr* add = last_def_before(
            body, update_store.src1, update_index, &add_index);
        if (!add || add->op != ThinOp::Add || add->meta.width != compare->meta.width)
            continue;
        bool update_ok = false;
        if (load_of_slot_before(body, add->src1, add_index, iv_slot)) {
            if (add->src2 == 0)
                update_ok = add->imm.i == 1;
            else {
                int64_t step = 0;
                update_ok = const_before(body, add->src2, add_index, &step) && step == 1;
            }
        } else if (load_of_slot_before(body, add->src2, add_index, iv_slot)) {
            int64_t step = 0;
            update_ok = const_before(body, add->src1, add_index, &step) && step == 1;
        }
        if (!update_ok) continue;

        // Prove every indirect store in the loop targets a fixed-base indexed
        // range disjoint from the IV slot. This permits the common a[i]=...
        // loop while rejecting arbitrary pointers and slice stores.
        for (size_t i = 0; i < body.instrs.size() && !unsafe_loop_write; ++i) {
            const ThinInstr& store = body.instrs[i];
            if (store.op != ThinOp::StoreAddr) continue;
            // The proof ranges i over [0, bound). After the update i may equal
            // bound, so reject indirect stores at/after the update even when a
            // later retained check would make source-lowered code safe.
            if (i >= update_index) {
                unsafe_loop_write = true;
                break;
            }
            size_t addr_index = 0;
            const ThinInstr* addr = last_def_before(body, store.src2, i, &addr_index);
            if (!addr || addr->op != ThinOp::IndexAddr || addr->src1 != 0 ||
                addr->meta.width <= 0 ||
                !load_of_slot_before(body, addr->src2, addr_index, iv_slot)) {
                unsafe_loop_write = true;
                break;
            }
            if (addr->meta.base_kind == AbsFixup::GlobalsBase) continue;

            const int64_t first = int64_t(addr->meta.frame_off) +
                                  int64_t(store.meta.frame_off);
            const int64_t write_width = std::max<int32_t>(1, store.meta.width);
            const uint64_t iterations = static_cast<uint64_t>(bound - 1);
            const uint64_t elem_width = static_cast<uint64_t>(addr->meta.width);
            const uint64_t max_span = static_cast<uint64_t>(INT64_MAX - write_width);
            if (iterations > max_span / elem_width) {
                unsafe_loop_write = true;
                break;
            }
            const int64_t span = static_cast<int64_t>(iterations * elem_width) +
                                 write_width;
            if (first > INT64_MAX - span ||
                intervals_overlap(first, span, iv_slot, 8))
                unsafe_loop_write = true;
        }
        if (unsafe_loop_write) continue;

        // Select only checks before the update, with the same immediate bound,
        // whose index is a fresh load of the IV slot and feeds a fixed-base
        // IndexAddr later in this block.
        std::vector<size_t> remove;
        for (size_t i = 0; i < update_index; ++i) {
            const ThinInstr& check = body.instrs[i];
            if (check.op != ThinOp::BoundsCheck || check.src2 != 0 ||
                check.imm.i != bound ||
                !load_of_slot_before(body, check.src1, i, iv_slot))
                continue;

            bool matching_index = false;
            for (size_t j = i + 1; j < update_index; ++j) {
                const ThinInstr& candidate = body.instrs[j];
                if (candidate.dst == check.src1) break; // index value redefined
                if (candidate.op == ThinOp::IndexAddr && candidate.src1 == 0 &&
                    candidate.src2 == check.src1 && candidate.meta.width > 0) {
                    matching_index = true;
                    break;
                }
            }
            if (matching_index) remove.push_back(i);
        }
        if (remove.empty()) continue;

        for (auto it = remove.rbegin(); it != remove.rend(); ++it)
            body.instrs.erase(body.instrs.begin() + static_cast<ptrdiff_t>(*it));
        changed = true;
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// LoopStrengthReductionPass: canonical i*K recurrence reduction
// ═══════════════════════════════════════════════════════════════════════════
//
// Restrict the transform to lowering's single body/latch loop shape. The
// induction variable must be an exact frame slot, initialized in the unique
// preheader and updated exactly once by i=i+1. For a Mul in the body using the
// load that reaches that update, create a private frame-backed accumulator:
//
//   preheader: t = i_start * K
//   body:      old_result = LoadFrame(t)
//              ... i = i + 1
//              t = t + K
//
// Keeping the original Mul destination preserves all existing VReg and spill-
// home uses. A fresh private frame slot avoids extending a cross-block VReg in
// non-SSA IR. Only one candidate is transformed per loop invocation; repeated
// pass runs can reduce additional independent products.

EmberPreserved LoopStrengthReductionPass::run(ThinFunction& f,
                                               EmberAnalysisManager&) {
    if (f.blocks.size() < 4) return EmberPreserved::all();

    std::unordered_map<uint32_t, size_t> id_to_index;
    for (size_t i = 0; i < f.blocks.size(); ++i)
        if (!id_to_index.emplace(f.blocks[i].id, i).second)
            return EmberPreserved::all();

    std::vector<std::vector<size_t>> preds(f.blocks.size());
    auto add_pred = [&](size_t source, uint32_t target) {
        auto found = id_to_index.find(target);
        if (found == id_to_index.end()) return false;
        preds[found->second].push_back(source);
        return true;
    };
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        const ThinTerm& term = f.blocks[i].term;
        if (term.kind == TermKind::Jmp) {
            if (!add_pred(i, term.target)) return EmberPreserved::all();
        } else if (term.kind == TermKind::Branch) {
            if (!add_pred(i, term.target) || !add_pred(i, term.false_target))
                return EmberPreserved::all();
        }
    }

    VReg next_vreg = 1;
    int32_t min_frame_off = f.frame.next_local_off;
    auto inspect_storage = [&](const ThinInstr& in) {
        next_vreg = std::max(next_vreg, in.dst + (in.dst != 0));
        next_vreg = std::max(next_vreg, in.src1 + (in.src1 != 0));
        next_vreg = std::max(next_vreg, in.src2 + (in.src2 != 0));
        for (VReg arg : in.args)
            next_vreg = std::max(next_vreg, arg + (arg != 0));
        if (in.meta.frame_off < min_frame_off) min_frame_off = in.meta.frame_off;
        if (in.meta.data_temp_off < min_frame_off) min_frame_off = in.meta.data_temp_off;
        for (int32_t off : in.arg_frame_offs)
            if (off < min_frame_off) min_frame_off = off;
    };
    for (const ThinBlock& block : f.blocks) {
        next_vreg = std::max(next_vreg, block.term.cond + (block.term.cond != 0));
        next_vreg = std::max(next_vreg, block.term.ret + (block.term.ret != 0));
        for (const ThinInstr& in : block.instrs) inspect_storage(in);
    }

    for (size_t hi = 0; hi < f.blocks.size(); ++hi) {
        ThinBlock& header = f.blocks[hi];
        if (header.term.kind != TermKind::Branch || preds[hi].size() != 2)
            continue;

        // Recover the header's `i < bound` from lowering's `(i < bound)==0`
        // branch wrapper. The bound itself need not be constant for LSR.
        size_t wrapper_index = 0;
        const ThinInstr* wrapper = last_def_before(
            header, header.term.cond, header.instrs.size(), &wrapper_index);
        if (!wrapper || wrapper->op != ThinOp::Cmp || wrapper->meta.cmp != 0)
            continue;
        VReg inner = 0;
        int64_t zero = 1;
        if (const_before(header, wrapper->src1, wrapper_index, &zero) && zero == 0)
            inner = wrapper->src2;
        else if (const_before(header, wrapper->src2, wrapper_index, &zero) && zero == 0)
            inner = wrapper->src1;
        if (inner == 0) continue;
        size_t compare_index = 0;
        const ThinInstr* compare = last_def_before(
            header, inner, wrapper_index, &compare_index);
        if (!compare || compare->op != ThinOp::Cmp || compare->meta.cmp != 2)
            continue;
        const ThinInstr* header_load = last_def_before(
            header, compare->src1, compare_index);
        if (!header_load || header_load->op != ThinOp::LoadFrame ||
            header_load->src1 != 0 || header_load->meta.frame_off == 0)
            continue;
        const int32_t iv_slot = header_load->meta.frame_off;

        auto body_found = id_to_index.find(header.term.false_target);
        if (body_found == id_to_index.end()) continue;
        const size_t bi = body_found->second;
        if (bi == hi || preds[bi].size() != 1 || preds[bi][0] != hi)
            continue;
        ThinBlock& body = f.blocks[bi];
        if (body.term.kind != TermKind::Jmp) continue;
        auto latch_found = id_to_index.find(body.term.target);
        if (latch_found == id_to_index.end()) continue;
        const size_t li = latch_found->second;
        if (li == hi || li == bi || preds[li].size() != 1 || preds[li][0] != bi)
            continue;
        ThinBlock& latch = f.blocks[li];
        if (latch.term.kind != TermKind::Jmp || latch.term.target != header.id)
            continue;

        size_t pi = f.blocks.size();
        for (size_t pred : preds[hi]) {
            if (pred == li) continue;
            if (pi != f.blocks.size()) { pi = f.blocks.size(); break; }
            pi = pred;
        }
        if (pi == f.blocks.size()) continue;
        ThinBlock& preheader = f.blocks[pi];
        if (preheader.term.kind != TermKind::Jmp ||
            preheader.term.target != header.id)
            continue;

        size_t init_index = preheader.instrs.size();
        for (size_t i = preheader.instrs.size(); i-- > 0;) {
            const ThinInstr& in = preheader.instrs[i];
            if (in.op == ThinOp::StoreFrame && in.src2 == 0 &&
                in.meta.frame_off == iv_slot) {
                init_index = i;
                break;
            }
        }
        if (init_index == preheader.instrs.size()) continue;
        const VReg initial_vreg = preheader.instrs[init_index].src1;
        if (initial_vreg == 0) continue;
        bool preheader_alias_after_init = false;
        for (size_t i = init_index + 1; i < preheader.instrs.size(); ++i) {
            const ThinInstr& in = preheader.instrs[i];
            if (is_frame_alias_barrier(in) || instr_writes_off(in, iv_slot)) {
                preheader_alias_after_init = true;
                break;
            }
        }
        if (preheader_alias_after_init) continue;

        // Prove a sole exact unit increment, allowing it in body or latch.
        ThinBlock* update_block = nullptr;
        size_t update_index = 0;
        const ThinInstr* update_store = nullptr;
        unsigned iv_writes = 0;
        bool unsafe = false;
        auto inspect_loop = [&](ThinBlock& block) {
            for (size_t i = 0; i < block.instrs.size(); ++i) {
                const ThinInstr& in = block.instrs[i];
                if (is_frame_alias_barrier(in)) unsafe = true;
                if (in.op == ThinOp::StoreFrame && in.src2 == 0 &&
                    in.meta.frame_off == iv_slot) {
                    ++iv_writes;
                    update_block = &block;
                    update_index = i;
                    update_store = &in;
                } else if (instr_writes_off(in, iv_slot)) {
                    unsafe = true;
                }
            }
        };
        inspect_loop(header);
        inspect_loop(body);
        inspect_loop(latch);
        if (unsafe || iv_writes != 1 || !update_store || !update_block)
            continue;
        size_t add_index = 0;
        const ThinInstr* increment = last_def_before(
            *update_block, update_store->src1, update_index, &add_index);
        if (!increment || increment->op != ThinOp::Add) continue;
        bool unit_increment = false;
        if (load_of_slot_before(*update_block, increment->src1, add_index, iv_slot)) {
            if (increment->src2 == 0) unit_increment = increment->imm.i == 1;
            else {
                int64_t step = 0;
                unit_increment = const_before(*update_block, increment->src2,
                                              add_index, &step) && step == 1;
            }
        } else if (load_of_slot_before(*update_block, increment->src2,
                                       add_index, iv_slot)) {
            int64_t step = 0;
            unit_increment = const_before(*update_block, increment->src1,
                                          add_index, &step) && step == 1;
        }
        if (!unit_increment) continue;

        // The recurrence update must execute on every path through an
        // iteration. The accepted shape has a single body/latch chain, but an
        // early Return/Trap in either block would bypass the update.
        if (body.term.kind != TermKind::Jmp || latch.term.kind != TermKind::Jmp)
            continue;

        // Select a Mul before the increment that consumes a direct load of i
        // and a loop-invariant integer constant. Require a unique definition of
        // the Mul destination in the loop to avoid non-SSA reaching ambiguity.
        size_t mul_index = body.instrs.size();
        int64_t factor = 0;
        for (size_t i = 0; i < body.instrs.size(); ++i) {
            ThinInstr& candidate = body.instrs[i];
            if (candidate.op != ThinOp::Mul || candidate.dst == 0 ||
                (&body == update_block && i >= add_index))
                continue;
            int64_t k = 0;
            bool left_iv = load_of_slot_before(body, candidate.src1, i, iv_slot);
            bool right_iv = load_of_slot_before(body, candidate.src2, i, iv_slot);
            bool constant = false;
            if (left_iv) {
                constant = candidate.src2 == 0
                    ? (k = candidate.imm.i, true)
                    : const_before(body, candidate.src2, i, &k);
            } else if (right_iv) {
                constant = const_before(body, candidate.src1, i, &k);
            }
            if (!constant) continue;
            unsigned definitions = 0;
            for (const ThinBlock* block : {&header, &body, &latch})
                for (const ThinInstr& in : block->instrs)
                    definitions += in.dst == candidate.dst &&
                                   in.op != ThinOp::CopyBytes;
            if (definitions != 1) continue;
            mul_index = i;
            factor = k;
            break;
        }
        if (mul_index == body.instrs.size()) continue;

        // Reserve an aligned private eight-byte frame cell and grow the frame.
        const int64_t low = std::min<int64_t>(min_frame_off, -f.frame.frame_size);
        int64_t acc_off64 = ((low - 8) / 8) * 8;
        if (acc_off64 >= 0) acc_off64 = -8;
        if (acc_off64 < INT32_MIN) continue;
        const int32_t acc_off = static_cast<int32_t>(acc_off64);
        min_frame_off = acc_off;
        const int64_t needed = -acc_off64 + 16;
        if (needed > INT32_MAX - 15) continue;
        // Lowering reserves an additional 16-byte safety area beyond its
        // deepest spill. Preserve that convention for the new accumulator.
        f.frame.frame_size = static_cast<int32_t>((needed + 31) & ~int64_t{15});
        f.frame.next_local_off = std::max(
            f.frame.next_local_off, static_cast<int32_t>(-acc_off64));

        const ThinInstr original_mul = body.instrs[mul_index];
        // Read the reaching slot value at the end of the preheader instead of
        // extending the initializer's source VReg across possible non-SSA
        // redefinitions in the preheader.
        ThinInstr load_initial;
        load_initial.op = ThinOp::LoadFrame;
        load_initial.dst = next_vreg++;
        load_initial.meta = original_mul.meta;
        load_initial.meta.frame_off = iv_slot;
        preheader.instrs.push_back(std::move(load_initial));

        ThinInstr init = original_mul;
        init.dst = next_vreg++;
        init.src1 = preheader.instrs.back().dst;
        init.src2 = 0;
        init.imm.i = factor;
        // Give the VReg a normal private spill home, then explicitly store the
        // initialized recurrence into acc_off. Reusing acc_off as both a VReg
        // home and mutable slot confuses regalloc's VReg lifetime model.
        init.meta.frame_off = acc_off - 8;
        preheader.instrs.push_back(std::move(init));
        ThinInstr store_initial;
        store_initial.op = ThinOp::StoreFrame;
        store_initial.src1 = preheader.instrs.back().dst;
        store_initial.meta = original_mul.meta;
        store_initial.meta.frame_off = acc_off;
        preheader.instrs.push_back(std::move(store_initial));

        // An ordinary LoadFrame uses frame_off as its source and therefore
        // cannot simultaneously materialize the original Mul spill home.
        // Load the accumulator into a fresh VReg, then retain the Mul's dst and
        // home with a Move so explicit home readers keep observing the result.
        ThinInstr load_result;
        load_result.op = ThinOp::LoadFrame;
        load_result.dst = next_vreg++;
        load_result.meta = original_mul.meta;
        load_result.meta.frame_off = acc_off;
        body.instrs.insert(body.instrs.begin() + static_cast<ptrdiff_t>(mul_index),
                           std::move(load_result));
        if (update_block == &body && update_index >= mul_index) ++update_index;
        ThinInstr replacement = original_mul;
        replacement.op = ThinOp::Move;
        replacement.src1 = body.instrs[mul_index].dst;
        replacement.src2 = 0;
        body.instrs[mul_index + 1] = std::move(replacement);

        ThinInstr load_acc;
        load_acc.op = ThinOp::LoadFrame;
        load_acc.dst = next_vreg++;
        load_acc.meta = original_mul.meta;
        load_acc.meta.frame_off = acc_off;
        ThinInstr add_acc = original_mul;
        add_acc.op = ThinOp::Add;
        add_acc.dst = next_vreg++;
        add_acc.src1 = load_acc.dst;
        add_acc.src2 = 0;
        add_acc.imm.i = factor;
        add_acc.meta.frame_off = acc_off - 16;
        ThinInstr store_acc;
        store_acc.op = ThinOp::StoreFrame;
        store_acc.src1 = add_acc.dst;
        store_acc.meta = original_mul.meta;
        store_acc.meta.frame_off = acc_off;

        // Insert after the induction store. If replacement and update share the
        // body, the replacement does not alter the update index because it is
        // in-place. Append after StoreFrame so t advances with i.
        auto insert_at = update_block->instrs.begin() +
                         static_cast<ptrdiff_t>(update_index + 1);
        insert_at = update_block->instrs.insert(insert_at, std::move(load_acc));
        insert_at = update_block->instrs.insert(insert_at + 1, std::move(add_acc));
        update_block->instrs.insert(insert_at + 1, std::move(store_acc));
        return EmberPreserved::none();
    }

    return EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// LoopUnrollPass: conservative full unrolling of canonical constant loops
// ═══════════════════════════════════════════════════════════════════════════
//
// Thin lowering emits a simple while/for loop as
//
//   preheader -> header -> body -> continuation -> header
//                        \-> exit
//
// where continuation is the budget-check latch for while, and the step plus
// budget-check block for for. This pass accepts only that four-edge shape. It
// proves `i` starts at zero, the header tests `i < N` with an immediate N, and
// the loop contains exactly one non-aliasing `i = i + 1` store. The body and
// continuation instructions are then concatenated N times in the body block,
// whose terminator is changed to the exit. Keeping the first header test is
// harmless and preserves the original zero-iteration control edge; its truth
// is already established by the proof for positive N.
//
// Reusing VReg numbers in the copies is intentional: Thin IR is explicitly
// non-SSA, and each copied definition precedes its copied uses. Mutable source
// values continue to flow through their frame slots, including the induction
// value, so each copy observes the value stored by the preceding copy.

EmberPreserved LoopUnrollPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (f.blocks.size() < 5) return EmberPreserved::all();

    bool changed = false;

    // Transform one loop at a time and rebuild graph facts. Besides allowing
    // several disjoint loops in one function, this prevents stale predecessor
    // information after a body is redirected around its old latch.
    for (;;) {
        const size_t block_count = f.blocks.size();
        std::unordered_map<uint32_t, size_t> id_to_index;
        id_to_index.reserve(block_count);
        bool valid_ids = true;
        for (size_t i = 0; i < block_count; ++i) {
            if (!id_to_index.emplace(f.blocks[i].id, i).second) {
                valid_ids = false;
                break;
            }
        }
        if (!valid_ids) break;

        std::vector<std::vector<size_t>> preds(block_count);
        auto add_pred = [&](size_t source, uint32_t target) -> bool {
            auto found = id_to_index.find(target);
            if (found == id_to_index.end()) return false;
            preds[found->second].push_back(source);
            return true;
        };
        for (size_t i = 0; i < block_count && valid_ids; ++i) {
            const ThinTerm& term = f.blocks[i].term;
            if (term.kind == TermKind::Jmp) {
                valid_ids = add_pred(i, term.target);
            } else if (term.kind == TermKind::Branch) {
                valid_ids = add_pred(i, term.target) &&
                            add_pred(i, term.false_target);
            }
        }
        if (!valid_ids) break;

        bool transformed = false;
        for (size_t hi = 0; hi < block_count && !transformed; ++hi) {
            const ThinBlock& header = f.blocks[hi];
            if (header.term.kind != TermKind::Branch || preds[hi].size() != 2)
                continue;

            // Recover the lowering's boolean wrapper:
            //   inner = i < N; wrapped = inner == 0;
            //   branch wrapped ? exit : body.
            size_t wrapper_index = 0;
            const ThinInstr* wrapper = last_def_before(
                header, header.term.cond, header.instrs.size(), &wrapper_index);
            if (!wrapper || wrapper->op != ThinOp::Cmp ||
                wrapper->meta.cmp != 0)
                continue;

            VReg inner_vreg = 0;
            int64_t zero = 1;
            if (const_before(header, wrapper->src1, wrapper_index, &zero) &&
                zero == 0)
                inner_vreg = wrapper->src2;
            else if (const_before(header, wrapper->src2, wrapper_index, &zero) &&
                     zero == 0)
                inner_vreg = wrapper->src1;
            if (inner_vreg == 0) continue;

            size_t compare_index = 0;
            const ThinInstr* compare = last_def_before(
                header, inner_vreg, wrapper_index, &compare_index);
            if (!compare || compare->op != ThinOp::Cmp ||
                compare->meta.cmp != 2)
                continue; // exactly i < N

            const ThinInstr* iv_load = last_def_before(
                header, compare->src1, compare_index);
            if (!iv_load || iv_load->op != ThinOp::LoadFrame ||
                iv_load->src1 != 0 || iv_load->meta.frame_off == 0)
                continue;
            const int32_t iv_slot = iv_load->meta.frame_off;

            // Require the bound in the compare's immediate form. This is the
            // unambiguous representation emitted for a source constant and
            // avoids treating a mutable frame load as compile-time constant.
            if (compare->src2 != 0) continue;
            const int64_t trip_count = compare->imm.i;
            if (trip_count <= 0 || trip_count > 8 ||
                !bound_fits_compare(trip_count, compare->meta.width,
                                    compare->meta.is_unsigned != 0))
                continue;

            // Header evaluation is removed after the first successful test.
            // Admit only the lowerer's exact, side-effect-free condition
            // vocabulary and only one frame read, the induction-slot load.
            // This excludes calls, trapping arithmetic, and conditions whose
            // repeated evaluation is observably significant.
            unsigned header_cmps = 0;
            unsigned header_loads = 0;
            bool unsafe_header = false;
            for (const ThinInstr& in : header.instrs) {
                switch (in.op) {
                case ThinOp::ConstInt:
                case ThinOp::ConstBool:
                case ThinOp::Move:
                    break;
                case ThinOp::Cmp:
                    ++header_cmps;
                    break;
                case ThinOp::LoadFrame:
                    ++header_loads;
                    if (in.src1 != 0 || in.meta.frame_off != iv_slot)
                        unsafe_header = true;
                    break;
                default:
                    unsafe_header = true;
                    break;
                }
            }
            if (unsafe_header || header_cmps != 2 || header_loads != 1)
                continue;

            auto body_found = id_to_index.find(header.term.false_target);
            auto exit_found = id_to_index.find(header.term.target);
            if (body_found == id_to_index.end() ||
                exit_found == id_to_index.end())
                continue;
            const size_t bi = body_found->second;
            const size_t ei = exit_found->second;
            if (bi == hi || ei == hi || bi == ei ||
                preds[bi].size() != 1 || preds[bi][0] != hi)
                continue;
            const ThinBlock& body = f.blocks[bi];
            if (body.term.kind != TermKind::Jmp) continue;

            auto continuation_found = id_to_index.find(body.term.target);
            if (continuation_found == id_to_index.end()) continue;
            const size_t ci = continuation_found->second;
            if (ci == hi || ci == bi || ci == ei || ci <= hi ||
                preds[ci].size() != 1 || preds[ci][0] != bi)
                continue;
            const ThinBlock& continuation = f.blocks[ci];
            if (continuation.term.kind != TermKind::Jmp ||
                continuation.term.target != header.id)
                continue; // the required earlier-block backedge

            // The second header predecessor is the unique preheader. Requiring
            // it to occur before the header excludes alternate entries and
            // irreducible arrangements that merely resemble lowering output.
            size_t pi = block_count;
            for (size_t pred : preds[hi]) {
                if (pred == ci) continue;
                if (pi != block_count) {
                    pi = block_count;
                    break;
                }
                pi = pred;
            }
            if (pi == block_count || pi >= hi) continue;
            const ThinBlock& preheader = f.blocks[pi];
            if (preheader.term.kind != TermKind::Jmp ||
                preheader.term.target != header.id)
                continue;

            // Prove the reaching initializer is exactly zero. Reject every
            // possible write/alias after it, rather than guessing whether a
            // call or aggregate operation can touch the induction home.
            size_t init_index = preheader.instrs.size();
            for (size_t i = preheader.instrs.size(); i-- > 0;) {
                const ThinInstr& in = preheader.instrs[i];
                if (in.op == ThinOp::StoreFrame && in.src2 == 0 &&
                    in.meta.frame_off == iv_slot) {
                    init_index = i;
                    break;
                }
            }
            if (init_index == preheader.instrs.size()) continue;
            int64_t initial = 1;
            if (!const_before(preheader, preheader.instrs[init_index].src1,
                              init_index, &initial) || initial != 0)
                continue;

            bool unsafe = false;
            for (size_t i = init_index + 1; i < preheader.instrs.size(); ++i) {
                const ThinInstr& in = preheader.instrs[i];
                if (instr_writes_off(in, iv_slot) ||
                    in.op == ThinOp::CallNative ||
                    in.op == ThinOp::CallScript ||
                    in.op == ThinOp::CallIndirect ||
                    in.op == ThinOp::CallCrossModule ||
                    in.op == ThinOp::CopyBytes ||
                    in.op == ThinOp::StructLitInit ||
                    in.op == ThinOp::ArrayLitInit ||
                    in.op == ThinOp::StringDecrypt ||
                    in.op == ThinOp::StoreAddr) {
                    unsafe = true;
                    break;
                }
            }
            if (unsafe) continue;

            // Find and prove the loop's sole induction-slot write. Unknown
            // aliases and calls are rejected even when they do not visibly
            // name the slot: without alias analysis they could change `i`.
            const ThinInstr* update_store = nullptr;
            const ThinBlock* update_block = nullptr;
            size_t update_index = 0;
            unsigned iv_writes = 0;
            auto inspect_loop_block = [&](const ThinBlock& blk) {
                for (size_t i = 0; i < blk.instrs.size(); ++i) {
                    const ThinInstr& in = blk.instrs[i];
                    if (instr_writes_off(in, iv_slot)) {
                        ++iv_writes;
                        if (in.op == ThinOp::StoreFrame && in.src2 == 0 &&
                            in.meta.frame_off == iv_slot) {
                            update_store = &in;
                            update_block = &blk;
                            update_index = i;
                        } else {
                            unsafe = true;
                        }
                    }
                    if (in.op == ThinOp::CallNative ||
                        in.op == ThinOp::CallScript ||
                        in.op == ThinOp::CallIndirect ||
                        in.op == ThinOp::CallCrossModule ||
                        in.op == ThinOp::CopyBytes ||
                        in.op == ThinOp::StructLitInit ||
                        in.op == ThinOp::ArrayLitInit ||
                        in.op == ThinOp::StringDecrypt ||
                        in.op == ThinOp::StoreAddr)
                        unsafe = true;
                }
            };
            inspect_loop_block(body);
            inspect_loop_block(continuation);
            if (unsafe || iv_writes != 1 || !update_store || !update_block)
                continue;

            size_t add_index = 0;
            const ThinInstr* add = last_def_before(
                *update_block, update_store->src1, update_index, &add_index);
            if (!add || add->op != ThinOp::Add ||
                add->meta.width != compare->meta.width ||
                add->meta.is_unsigned != compare->meta.is_unsigned)
                continue;

            bool unit_increment = false;
            if (load_of_slot_before(*update_block, add->src1, add_index,
                                    iv_slot)) {
                if (add->src2 == 0)
                    unit_increment = add->imm.i == 1;
                else {
                    int64_t step = 0;
                    unit_increment = const_before(*update_block, add->src2,
                                                  add_index, &step) && step == 1;
                }
            } else if (load_of_slot_before(*update_block, add->src2, add_index,
                                           iv_slot)) {
                int64_t step = 0;
                unit_increment = const_before(*update_block, add->src1,
                                              add_index, &step) && step == 1;
            }
            if (!unit_increment) continue;

            // Full unroll. Copy both blocks for every iteration so the for-loop
            // step and every original BudgetCheck retain their exact count and
            // ordering relative to body side effects.
            std::vector<ThinInstr> iteration;
            iteration.reserve(body.instrs.size() + continuation.instrs.size());
            iteration.insert(iteration.end(), body.instrs.begin(),
                             body.instrs.end());
            iteration.insert(iteration.end(), continuation.instrs.begin(),
                             continuation.instrs.end());

            std::vector<ThinInstr> unrolled;
            unrolled.reserve(iteration.size() * static_cast<size_t>(trip_count));
            for (int64_t iteration_number = 0;
                 iteration_number < trip_count; ++iteration_number)
                unrolled.insert(unrolled.end(), iteration.begin(), iteration.end());

            ThinBlock& mutable_body = f.blocks[bi];
            mutable_body.instrs = std::move(unrolled);
            ThinTerm exit_jump;
            exit_jump.kind = TermKind::Jmp;
            exit_jump.target = f.blocks[ei].id;
            mutable_body.term = exit_jump;

            changed = true;
            transformed = true;
        }

        if (!transformed) break;
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// SCCPPass: sparse conditional constant propagation across blocks
// ═══════════════════════════════════════════════════════════════════════
// Unlike ConstProp (per-block), SCCP tracks constants globally across
// block boundaries using a fixpoint iteration. It ONLY replaces vreg uses
// with constants — NO CFG modification (no block merging, no branch folding).
// This makes it inherently value-preserving.

EmberPreserved SCCPPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool ever_changed = false;

    // Global vreg → constant map (persists across blocks)
    struct ConstVal { bool valid; int64_t i; };
    std::unordered_map<VReg, ConstVal> vreg_const;

    auto get_const = [&](VReg v) -> ConstVal {
        auto it = vreg_const.find(v);
        if (it != vreg_const.end()) return it->second;
        return {false, 0};
    };

    auto set_const = [&](VReg v, int64_t val) {
        auto it = vreg_const.find(v);
        if (it != vreg_const.end() && it->second.valid && it->second.i == val)
            return;  // no change
        vreg_const[v] = {true, val};
    };

    // Fixpoint: iterate until no new constants are discovered. ever_changed
    // tracks IR mutations across ALL iterations; the final converged iteration
    // must not erase the preservation result from an earlier rewrite.
    bool iter_changed = true;
    while (iter_changed) {
        iter_changed = false;

        for (auto& blk : f.blocks) {
            for (auto& in : blk.instrs) {
                switch (in.op) {
                case ThinOp::ConstInt:
                case ThinOp::ConstBool:
                    set_const(in.dst, in.imm.i);
                    break;

                case ThinOp::Move: {
                    ConstVal v = get_const(in.src1);
                    if (v.valid) {
                        set_const(in.dst, v.i);
                        // Rewrite: replace Move with ConstInt
                        in.op = ThinOp::ConstInt;
                        in.imm.i = v.i;
                        in.src1 = 0;
                        in.src2 = 0;
                        iter_changed = true;
                        ever_changed = true;
                    } else {
                        vreg_const.erase(in.dst);
                    }
                    break;
                }

                default:
                    if (is_foldable_int_binop(in.op) && in.meta.width != 0) {
                        ConstVal a = get_const(in.src1);
                        ConstVal b = (in.src2 == 0)
                            ? ConstVal{true, in.imm.i}
                            : get_const(in.src2);

                        if (a.valid && b.valid) {
                            // Full fold: both operands constant → ConstInt
                            int64_t result;
                            if (fold_int_binop(in.op, a.i, b.i, in.meta.width,
                                               in.meta.is_unsigned != 0, &result)) {
                                set_const(in.dst, result);
                                in.op = ThinOp::ConstInt;
                                in.imm.i = result;
                                in.src1 = 0;
                                in.src2 = 0;
                                iter_changed = true;
                                ever_changed = true;
                            }
                        } else if (a.valid && in.src2 != 0 &&
                                   a.i >= -0x7FFFFFFFLL && a.i <= 0x7FFFFFFFLL) {
                            // Partial: src1 is constant, move to immediate form
                            // (swap: make src1 the vreg, imm the constant)
                            // Only safe for commutative ops; for non-commutative,
                            // we'd need to negate. Skip for safety.
                            // Actually, just rewrite src2 to immediate:
                            // The emitter handles src2==0 + imm.i as the second operand.
                            // But we need to swap which is immediate. For now,
                            // only fold when BOTH are constant (safe).
                        } else if (b.valid && in.src2 != 0 &&
                                   b.i >= -0x7FFFFFFFLL && b.i <= 0x7FFFFFFFLL) {
                            // Partial: src2 is constant VReg → convert to immediate
                            in.imm.i = b.i;
                            in.src2 = 0;
                            iter_changed = true;
                            ever_changed = true;
                        }

                        if (in.op != ThinOp::ConstInt)
                            vreg_const.erase(in.dst);
                    } else {
                        // Any other producing instr: dst is not constant
                        if (in.dst) vreg_const.erase(in.dst);
                    }
                    break;
                }
            }
        }
    }

    return ever_changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// DeadSpillElimPass: remove provably unobservable frame spills
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved DeadSpillElimPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;

    // Explicit StoreFrame instructions can be removed only when their target
    // has no observable reader anywhere in the function. This includes the
    // aggregate and by-value readers recognized by DCE, not just LoadFrame.
    const auto read_slots = compute_read_slots(f);
    for (auto& blk : f.blocks) {
        for (size_t i = 0; i < blk.instrs.size(); ) {
            ThinInstr& store = blk.instrs[i];
            if (store.op != ThinOp::StoreFrame || store.src1 == 0 ||
                store.src2 != 0 || store.meta.field_off != 0 ||
                read_slots.count(store.meta.frame_off)) {
                ++i;
                continue;
            }

            // A plain source-local store is semantically dead once its slot is
            // unread. Require the value to be available without this store as
            // well: either regalloc assigned it a register, or the immediately
            // preceding instruction produced it in rax/xmm0. No instruction is
            // allowed between producer and store because it may clobber that
            // result register.
            bool available = false;
            auto assignment = f.ra.map.find(store.src1);
            if (f.ra.enabled && assignment != f.ra.map.end() &&
                assignment->second.in_reg)
                available = true;
            if (!available && i > 0) {
                const ThinInstr& producer = blk.instrs[i - 1];
                available = producer.dst == store.src1;
            }
            if (!available) {
                ++i;
                continue;
            }

            // If the preceding producer's own spill home is exactly this dead
            // slot, clear it too. Otherwise emit_x64 would recreate the same
            // store through pin_int_dst even after StoreFrame is erased.
            if (i > 0) {
                ThinInstr& producer = blk.instrs[i - 1];
                if (producer.dst == store.src1 &&
                    producer.meta.frame_off == store.meta.frame_off)
                    producer.meta.frame_off = 0;
            }
            blk.instrs.erase(blk.instrs.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
        }
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// PeepholePass: small canonical no-op and adjacent-sequence simplification
// ═══════════════════════════════════════════════════════════════════════════
//
// Arithmetic identities belong to InstCombinePass and are deliberately not
// duplicated here. This pass handles the remaining local patterns:
//   * a storage-free Move(v,v) is a no-op;
//   * Move(a,b); Move(b,a) has a redundant second move, provided both moves
//     have the same type and the second move does not materialize a spill;
//   * a Cast whose locally known source type equals its target type is a Move.
//
// The frame_off checks matter because Thin IR producers may materialize their
// result in a frame home as part of the instruction. Erasing such an
// instruction would erase a storage write even when its VReg value is unchanged.

EmberPreserved PeepholePass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        auto& instrs = blk.instrs;
        std::unordered_map<VReg, const Type*> vreg_types;

        auto remember_type = [&](const ThinInstr& in) {
            if (in.dst == 0 || in.op == ThinOp::CopyBytes) return;

            const Type* type = in.meta.type;
            if (!type && in.op == ThinOp::Move) {
                auto source = vreg_types.find(in.src1);
                if (source != vreg_types.end()) type = source->second;
            }
            if (!type && (in.op == ThinOp::CallNative ||
                          in.op == ThinOp::CallScript ||
                          in.op == ThinOp::CallIndirect ||
                          in.op == ThinOp::CallCrossModule))
                type = in.ret_type;

            if (type) vreg_types[in.dst] = type;
            else vreg_types.erase(in.dst);
        };

        for (size_t i = 0; i < instrs.size(); ) {
            ThinInstr& in = instrs[i];

            // Cast(x, same_type) has exactly Move semantics. Restrict the
            // proof to a source definition seen in this block: Thin IR is not
            // SSA, so guessing a type across a block boundary or redefinition
            // would not be safe.
            if (in.op == ThinOp::Cast && in.src1 != 0 && in.meta.type) {
                auto source = vreg_types.find(in.src1);
                if (source != vreg_types.end() && source->second &&
                    source->second->same(*in.meta.type)) {
                    in.op = ThinOp::Move;
                    in.src2 = 0;
                    changed = true;
                }
            }

            // A frame-backed self-move still performs a spill. Remove only a
            // storage-free Move(v,v), preserving the current type fact for v.
            if (in.op == ThinOp::Move && in.dst != 0 &&
                in.dst == in.src1 && in.meta.frame_off == 0) {
                instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i));
                changed = true;
                continue;
            }

            // After a=b, the adjacent b=a merely writes b's existing value
            // back to itself. Require nominally equal explicit types and no
            // second spill so neither normalization nor storage is lost.
            if (i > 0 && in.op == ThinOp::Move && in.dst != 0 &&
                in.src1 != 0 && in.meta.frame_off == 0) {
                const ThinInstr& previous = instrs[i - 1];
                if (previous.op == ThinOp::Move &&
                    previous.dst == in.src1 && previous.src1 == in.dst &&
                    previous.meta.type && in.meta.type &&
                    previous.meta.type->same(*in.meta.type) &&
                    previous.meta.width == in.meta.width &&
                    previous.meta.is_f32 == in.meta.is_f32) {
                    instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i));
                    changed = true;
                    continue;
                }
            }

            remember_type(in);
            ++i;
        }
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// BranchFoldingPass: identical conditional targets become an unconditional Jmp
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved BranchFoldingPass::run(ThinFunction& f,
                                       EmberAnalysisManager&) {
    bool changed = false;
    for (auto& blk : f.blocks) {
        if (blk.term.kind != TermKind::Branch ||
            blk.term.target != blk.term.false_target)
            continue;

        ThinTerm jump;
        jump.kind = TermKind::Jmp;
        jump.target = blk.term.target;
        blk.term = jump;
        changed = true;
    }
    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

// register_passes
// ═══════════════════════════════════════════════════════════════════════════

void register_passes(EmberPassRegistry& reg) {
    reg.add<ConstPropPass>("constprop");
    reg.add<DeadCodeElimPass>("dce");
    reg.add<SimplifyCFGPass>("simplifycfg");
    reg.add<CSEPass>("cse");
    reg.add<LICMPass>("licm");
    reg.add<LoopStrengthReductionPass>("lsr");
    reg.add<StoreToLoadForwardPass>("forward");
    reg.add<CopyPropPass>("copyprop");
    reg.add<InstCombinePass>("instcombine");
    reg.add<DeadStoreElimPass>("dse");
    reg.add<BoundsCheckElimPass>("bounds-elim");
    reg.add<SCCPPass>("sccp");
    reg.add<LoopUnrollPass>("unroll");
    reg.add<DeadSpillElimPass>("spill_elim");
    reg.add<PeepholePass>("peephole");
    reg.add<BranchFoldingPass>("branch_folding");
}

} // namespace ember::ext_opt
