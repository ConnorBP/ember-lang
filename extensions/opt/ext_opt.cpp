// ext_opt.cpp — Stage C: the IR optimization passes.
// See ext_opt.hpp for the design. All passes are value-preserving and
// conservative — when in doubt, do not transform.

#include "ext_opt.hpp"

#include <algorithm>
#include <cstdint>
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

// Compute the set of frame_off values that are READ by any LoadFrame,
// CopyBytes, or FieldAddr in the function. For CopyBytes, the SOURCE offset is
// meta.field_off (when the source is frame-relative) and the dest offset is
// meta.frame_off (when the dest is frame-relative, i.e. in.dst == 0). Both must
// be tracked so DCE does not remove the StoreFrame that feeds the CopyBytes
// source. We conservatively add both offsets — a false positive only keeps a
// dead store alive (a missed optimization), never removes a needed one.
std::unordered_set<int32_t> compute_read_slots(const ThinFunction& f) {
    std::unordered_set<int32_t> read;
    for (const auto& blk : f.blocks) {
        for (const auto& in : blk.instrs) {
            if (in.op == ThinOp::LoadFrame || in.op == ThinOp::CopyBytes ||
                in.op == ThinOp::FieldAddr)
                read.insert(in.meta.frame_off);
            // CopyBytes reads a byte RANGE from the source (meta.field_off ..
            // meta.field_off + meta.len). A StoreFrame anywhere in that range
            // feeds the copy and must not be removed. Add every 8-byte-aligned
            // offset in the range (StoreFrame targets are word-aligned). Also add
            // meta.frame_off (the dest, when in.dst == 0 — already added above)
            // so a subsequent frame->frame copy's dest is tracked too.
            if (in.op == ThinOp::CopyBytes && in.meta.len > 0) {
                int32_t start = in.meta.field_off;
                int32_t end = start + in.meta.len;
                for (int32_t off = start; off < end; off += 8)
                    read.insert(off);
            }
        }
    }
    return read;
}

// Does `in` READ frame offset `off`? Used by DSE's intra-block dead-store
// scan to kill a pending dead store when a reader appears between two
// StoreFrames to the same slot. Mirrors compute_read_slots's conservative
// reader set: LoadFrame reads meta.frame_off; CopyBytes reads its SOURCE
// range [meta.field_off, meta.field_off + meta.len) AND (conservatively)
// meta.frame_off (the dest, when frame-relative — treating the dest as a
// read is a false positive that only keeps a dead store alive, never
// removes a needed one); FieldAddr reads meta.frame_off (the struct base).
// StoreFrame to a DIFFERENT offset does NOT read `off` (frame slots don't
// overlap in v1 — each local gets its own non-overlapping region, per the
// StoreToLoadForward comment), so a store to another slot is not a reader.
//
// The CopyBytes case is the one DSE previously MISSED (it only killed on
// LoadFrame), which let DSE remove a StoreFrame that fed a subsequent
// CopyBytes source — a value-preservation bug (hand-built IR repro: two
// StoreFrames to slot X with a CopyBytes reading X between them; DSE
// removed the first store, so the copy read an uninitialized slot).
bool instr_reads_off(const ThinInstr& in, int32_t off) {
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
    return false;
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
bool instr_writes_off(const ThinInstr& in, int32_t off) {
    if (in.op == ThinOp::CopyBytes) {
        // Dest is frame-relative when in.dst == 0 (no dest vreg) and the dest
        // side is not global-backed. copy_frame_frame / copy_global_frame set
        // meta.frame_off = dst_off; copy_frame_vptr / copy_global_vptr set
        // in.dst to a vreg (dest is [vreg+0], not a frame slot). For a
        // frame-relative dest, the written range is [frame_off, frame_off+len).
        if (in.dst == 0 && in.meta.len > 0 &&
            off >= in.meta.frame_off && off < in.meta.frame_off + in.meta.len)
            return true;
        return false;
    }
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
                // StoreFrame src1=vM off=X: if vM is constant, mark slot X.
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
                    // Dead store: slot never read by any LoadFrame/CopyBytes/FieldAddr.
                    if (read_slots.find(in.meta.frame_off) == read_slots.end())
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
                    // Dead store: slot never read.
                    if (read_slots.find(in.meta.frame_off) == read_slots.end())
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
// CSEPass: local common-subexpression elimination within a block
// ═══════════════════════════════════════════════════════════════════════════

EmberPreserved CSEPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;

    for (auto& blk : f.blocks) {
        // Table: key → (first_dst VReg, index of the first instr).
        // Key = op + src1 + src2 + imm.i + width + cmp + frame_off + base_kind + addend.
        struct CSEKey {
            uint16_t op;
            VReg src1, src2;
            int64_t imm_i;
            int32_t width, frame_off;
            uint8_t cmp, base_kind;
            uint32_t addend;
            bool operator==(const CSEKey& o) const {
                return op == o.op && src1 == o.src1 && src2 == o.src2 &&
                       imm_i == o.imm_i && width == o.width &&
                       frame_off == o.frame_off && cmp == o.cmp &&
                       base_kind == o.base_kind && addend == o.addend;
            }
        };
        struct CSEKeyHash {
            size_t operator()(const CSEKey& k) const {
                size_t h = k.op;
                h = h * 31 + k.src1;
                h = h * 31 + k.src2;
                h = h * 31 + std::hash<int64_t>()(k.imm_i);
                h = h * 31 + k.width;
                h = h * 31 + k.frame_off;
                return h;
            }
        };

        std::unordered_map<CSEKey, VReg, CSEKeyHash> table;
        std::unordered_set<VReg> killed;  // VRegs redefined after their first use

        for (auto it = blk.instrs.begin(); it != blk.instrs.end(); ) {
            ThinInstr& in = *it;

            // Kill rule: if this instr redefines a VReg, remove table entries
            // that use it as a source, and remove its old entry as a result.
            if (in.dst != 0) {
                for (auto t_it = table.begin(); t_it != table.end(); ) {
                    if (t_it->second == in.dst ||
                        t_it->first.src1 == in.dst ||
                        t_it->first.src2 == in.dst)
                        t_it = table.erase(t_it);
                    else
                        ++t_it;
                }
            }

            // StoreFrame kills any CSE entry for the same slot (the value changed).
            if (in.op == ThinOp::StoreFrame) {
                for (auto t_it = table.begin(); t_it != table.end(); ) {
                    if (t_it->first.op == static_cast<uint16_t>(ThinOp::LoadFrame) &&
                        t_it->first.frame_off == in.meta.frame_off)
                        t_it = table.erase(t_it);
                    else
                        ++t_it;
                }
                ++it;
                continue;
            }

            // Only CSE pure instrs with a dst.
            if (!is_pure(in.op) || in.dst == 0) {
                ++it;
                continue;
            }

            CSEKey key{
                static_cast<uint16_t>(in.op), in.src1, in.src2,
                in.imm.i, in.meta.width, in.meta.frame_off,
                in.meta.cmp, static_cast<uint8_t>(in.meta.base_kind),
                in.meta.addend
            };

            auto found = table.find(key);
            if (found != table.end()) {
                // Redundant: remap uses of in.dst to found->second (the first's
                // dst), for all instrs AFTER this one in the block. Then remove
                // this instr.
                VReg old_dst = in.dst;
                VReg new_dst = found->second;
                // Remap: for every instr after this one, replace old_dst with
                // new_dst in src1/src2/args. Stop if new_dst is redefined.
                bool new_dst_redefined = false;
                for (auto after = std::next(it); after != blk.instrs.end(); ++after) {
                    if (after->dst == new_dst) { new_dst_redefined = true; break; }
                    if (after->src1 == old_dst) after->src1 = new_dst;
                    if (after->src2 == old_dst) after->src2 = new_dst;
                    for (VReg& a : after->args) if (a == old_dst) a = new_dst;
                }
                // If new_dst was NOT redefined before the end, the remap is
                // complete and we can safely remove the redundant instr.
                if (!new_dst_redefined) {
                    it = blk.instrs.erase(it);
                    changed = true;
                    continue;
                }
                // If new_dst WAS redefined, the remap only covers part of the
                // range. This is still value-preserving (we remapped up to the
                // redefinition), but we can't remove the instr (its dst still
                // has uses after the redefinition point). Leave it.
            } else {
                // New entry.
                table[key] = in.dst;
            }
            ++it;
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

    // Build a predecessor map: for each block, which blocks jump to it?
    std::vector<std::vector<uint32_t>> preds(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i) {
        const auto& blk = f.blocks[i];
        if (blk.term.kind == TermKind::Jmp)
            preds[id_to_idx[blk.term.target]].push_back(uint32_t(i));
        else if (blk.term.kind == TermKind::Branch) {
            preds[id_to_idx[blk.term.target]].push_back(uint32_t(i));
            preds[id_to_idx[blk.term.false_target]].push_back(uint32_t(i));
        }
    }

    // Find back-edges: a Jmp/Branch whose target index < current index.
    // (The lowering produces blocks in topological-ish order, so a back-edge
    // is a jump to an earlier block.)
    struct Loop { uint32_t header_idx; uint32_t latch_idx; std::set<uint32_t> body; };
    std::vector<Loop> loops;
    for (size_t i = 0; i < num_blocks; ++i) {
        const auto& blk = f.blocks[i];
        auto check_back = [&](uint32_t target) {
            auto it = id_to_idx.find(target);
            if (it == id_to_idx.end()) return;
            uint32_t target_idx = uint32_t(it->second);
            if (target_idx < i) {
                // Back-edge: i → target_idx. Natural loop = {target_idx} ∪
                // {all blocks that can reach i without going through target_idx}.
                Loop loop;
                loop.header_idx = target_idx;
                loop.latch_idx = uint32_t(i);
                loop.body.insert(target_idx);
                // Reverse BFS from latch to header, avoiding the header.
                std::vector<uint32_t> stack = {uint32_t(i)};
                while (!stack.empty()) {
                    uint32_t b = stack.back(); stack.pop_back();
                    if (b == target_idx) continue;
                    if (loop.body.count(b)) continue;
                    loop.body.insert(b);
                    for (uint32_t p : preds[b])
                        if (!loop.body.count(p)) stack.push_back(p);
                }
                loops.push_back(loop);
            }
        };
        if (blk.term.kind == TermKind::Jmp)
            check_back(blk.term.target);
        else if (blk.term.kind == TermKind::Branch) {
            check_back(blk.term.target);
            check_back(blk.term.false_target);
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
    // An instruction is loop-invariant if:
    // - It's a ConstInt/ConstFloat/ConstBool (no operands → always invariant).
    // - It's a pure arithmetic op (Add/Sub/Mul/...) and ALL its source VRegs
    //   are defined OUTSIDE the loop (in a block not in the loop body), or the
    //   source is the immediate form (src2==0, using imm.i — always invariant).
    // - It's a LoadFrame from a slot that is NEVER written (StoreFrame) inside
    //   the loop.
    // - It's a Move whose src1 is invariant.
    //
    // We do NOT hoist StoreFrame (memory writes) in this first implementation —
    // hoisting stores requires proving the slot is not read before the store in
    // the loop, which is more complex. The hoisted pure instructions reduce the
    // per-iteration compute cost even if the store stays in the loop.

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

        // Compute the set of VRegs defined INSIDE the loop.
        std::set<VReg> loop_def_vregs;
        // Compute the set of frame slots written INSIDE the loop.
        std::set<int32_t> loop_written_slots;
        for (uint32_t bi : loop.body) {
            for (const auto& in : f.blocks[bi].instrs) {
                if (in.dst) loop_def_vregs.insert(in.dst);
                if (in.op == ThinOp::StoreFrame)
                    loop_written_slots.insert(in.meta.frame_off);
            }
        }

        // Check if a VReg is loop-invariant (defined outside the loop).
        auto is_invariant_vreg = [&](VReg v) -> bool {
            if (v == 0) return true;  // 0 = invalid/none/immediate
            return !loop_def_vregs.count(v);
        };

        // Check if an instruction is loop-invariant and hoistable (pure).
        auto is_invariant_instr = [&](const ThinInstr& in) -> bool {
            if (is_side_effecting(in.op)) return false;
            if (in.op == ThinOp::StoreFrame) return false;  // don't hoist stores (conservative)
            if (in.op == ThinOp::ConstInt || in.op == ThinOp::ConstFloat ||
                in.op == ThinOp::ConstBool)
                return true;  // constants are always invariant
            if (in.op == ThinOp::LoadFrame)
                return !loop_written_slots.count(in.meta.frame_off);
            if (in.op == ThinOp::Move)
                return is_invariant_vreg(in.src1);
            // Binary int ops / Cmp / Cast: all source VRegs must be invariant.
            // The immediate form (src2==0) is always invariant.
            return is_invariant_vreg(in.src1) && is_invariant_vreg(in.src2);
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

        // Hoist: move each invariant instruction to the end of the pre-header's
        // instrs (before the terminator). Remove from the original block.
        // Process in reverse order so indices don't shift as we erase.
        std::sort(to_hoist.begin(), to_hoist.end(),
            [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second > b.second;
            });
        auto& pre_hdr_instrs = f.blocks[pre_header].instrs;
        for (const auto& [bi, ii] : to_hoist) {
            ThinInstr hoisted = std::move(f.blocks[bi].instrs[ii]);
            f.blocks[bi].instrs.erase(f.blocks[bi].instrs.begin() + ptrdiff_t(ii));
            pre_hdr_instrs.push_back(std::move(hoisted));
            changed = true;
        }
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
            if (in.op == ThinOp::StoreFrame) {
                // Record the store (or update it).
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
            // Any other instruction that writes to a VReg could invalidate a
            // store if the VReg was the store's source. But since we're only
            // tracking frame_off → src VReg, and the src VReg is the value AT
            // THE TIME of the store (VRegs are SSA — each VReg is written
            // exactly once by thin_lower's monotonic new_vreg, so the src VReg
            // is never redefined), a subsequent redefinition cannot happen.
            // So we don't need to kill on VReg redefinition — the frame slot
            // holds the value that was stored, regardless of what happens to
            // the VReg later. This is correct because we're forwarding the
            // VALUE that was stored, not the VReg itself.
            //
            // HOWEVER: an instruction that WRITES the frame slot directly
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
            // Follow copy chains (v → v2 → v3 ...) to the root.
            VReg cur = v;
            while (copy_map.count(cur)) cur = copy_map[cur];
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
                if (in.op == ThinOp::StoreFrame) {
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
// register_passes
// ═══════════════════════════════════════════════════════════════════════════

void register_passes(EmberPassRegistry& reg) {
    reg.add<ConstPropPass>("constprop");
    reg.add<DeadCodeElimPass>("dce");
    reg.add<CSEPass>("cse");
    reg.add<LICMPass>("licm");
    reg.add<StoreToLoadForwardPass>("forward");
    reg.add<CopyPropPass>("copyprop");
    reg.add<InstCombinePass>("instcombine");
    reg.add<DeadStoreElimPass>("dse");
}

} // namespace ember::ext_opt
