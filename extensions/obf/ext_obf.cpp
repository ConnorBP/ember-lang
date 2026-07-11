// ext_obf.cpp — Stage C Step 5: IR-level obfuscation passes.
// See ext_obf.hpp for the design. Obfuscation passes INCREASE code complexity
// (more instructions, harder to reverse-engineer) while preserving the result.

#include "ext_obf.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace ember::ext_obf {

namespace {

// Compute the max VReg in the function (for allocating new VRegs).
uint32_t compute_max_vreg(const ThinFunction& f) {
    uint32_t max = 1;
    auto bump = [&](uint32_t v) { if (v >= max) max = v + 1; };
    for (const auto& blk : f.blocks) {
        for (const auto& in : blk.instrs) {
            bump(in.dst); bump(in.src1); bump(in.src2);
            for (uint32_t a : in.args) bump(a);
        }
        bump(blk.term.cond); bump(blk.term.ret);
    }
    return max;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// SubstitutionPass: MBA instruction substitution
// ═══════════════════════════════════════════════════════════════════════════
//
// Replaces simple integer arithmetic with equivalent MBA expressions:
//   a + b  →  (a ^ b) + 2*(a & b)       [the classic MBA identity for Add]
//   a - b  →  (a ^ ~b) + 2*(a & ~b) + 1  [Sub via Add + complement — but this
//                                         is complex; for now only Add]
//
// Only substitutes Add (the most common op). Future: Sub, Mul, Xor.
// Conservative: only substitutes when both operands are VRegs (not the
// immediate form src2==0), so the MBA expansion has real values to work with.
// Skips side-effecting ops, calls, guards, etc.

EmberPreserved SubstitutionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;
    uint32_t next_vreg = compute_max_vreg(f);

    // Allocate frame slots for new VRegs. The frame grows down from
    // next_local_off; each new VReg needs an 8-byte slot.
    int32_t next_off = f.frame.next_local_off;
    auto alloc_frame_slot = [&]() -> int32_t {
        next_off += 8;
        int32_t off = -next_off;
        return off;
    };

    for (auto& blk : f.blocks) {
        auto& instrs = blk.instrs;
        for (auto it = instrs.begin(); it != instrs.end(); ++it) {
            ThinInstr& in = *it;

            // Only substitute integer Add with two VReg operands (not immediate).
            if (in.op != ThinOp::Add) continue;
            if (in.src1 == 0 || in.src2 == 0) continue;  // need two real VRegs
            if (in.meta.width == 0) continue;  // skip if width unspecified

            // MBA: a + b = (a ^ b) + 2*(a & b)
            uint32_t v_xor = next_vreg++;
            uint32_t v_and = next_vreg++;
            uint32_t v_shl = next_vreg++;

            // Allocate frame slots for the new intermediate VRegs.
            int32_t off_xor = alloc_frame_slot();
            int32_t off_and = alloc_frame_slot();
            int32_t off_shl = alloc_frame_slot();

            VReg src_a = in.src1;
            VReg src_b = in.src2;
            int32_t width = in.meta.width;
            const Type* ty = in.meta.type;

            // 1. v_xor = a ^ b
            ThinInstr i_xor;
            i_xor.op = ThinOp::Xor;
            i_xor.dst = v_xor;
            i_xor.src1 = src_a;
            i_xor.src2 = src_b;
            i_xor.meta.width = width;
            i_xor.meta.type = ty;
            i_xor.meta.frame_off = off_xor;  // spill slot for v_xor
            i_xor.loc = in.loc;

            // 2. v_and = a & b
            ThinInstr i_and;
            i_and.op = ThinOp::And;
            i_and.dst = v_and;
            i_and.src1 = src_a;
            i_and.src2 = src_b;
            i_and.meta.width = width;
            i_and.meta.type = ty;
            i_and.meta.frame_off = off_and;  // spill slot for v_and
            i_and.loc = in.loc;

            // 3. v_shl = v_and << 1 (immediate form: src2=0, imm.i=1)
            ThinInstr i_shl;
            i_shl.op = ThinOp::Shl;
            i_shl.dst = v_shl;
            i_shl.src1 = v_and;
            i_shl.src2 = 0;
            i_shl.imm.i = 1;
            i_shl.meta.width = width;
            i_shl.meta.type = ty;
            i_shl.meta.frame_off = off_shl;  // spill slot for v_shl
            i_shl.loc = in.loc;

            // 4. dst = v_xor + v_shl (reuse the original instr)
            in.op = ThinOp::Add;
            in.src1 = v_xor;
            in.src2 = v_shl;
            in.imm.i = 0;
            // meta stays the same (dst VReg + its frame_off are unchanged).

            // Insert the 3 new instructions BEFORE the modified original.
            it = instrs.insert(it, std::move(i_xor));
            ++it;
            it = instrs.insert(it, std::move(i_and));
            ++it;
            it = instrs.insert(it, std::move(i_shl));
            ++it;

            changed = true;
        }
    }

    // Update the frame plan: the new VRegs' slots extend the frame.
    if (changed) {
        f.frame.next_local_off = next_off;
        // Update frame_size to fit the new slots (round up to 16).
        int32_t needed = next_off + 8;  // +8 for the rbx save slot
        if (needed > f.frame.frame_size) {
            f.frame.frame_size = (needed + 15) & ~15;
        }
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

void register_passes(EmberPassRegistry& reg) {
    reg.add<SubstitutionPass>("subst");
}

} // namespace ember::ext_obf
