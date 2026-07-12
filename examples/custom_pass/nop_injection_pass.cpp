// nop_injection_pass.cpp — deterministic semantic-NOP instruction insertion.
#include "custom_passes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ember::examples::custom_pass {
namespace {

uint64_t fnv1a(uint64_t h, const char* text) {
    while (*text) {
        h ^= static_cast<uint8_t>(*text++);
        h *= 1099511628211ULL;
    }
    return h;
}

struct StableRng {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

uint32_t next_vreg(const ThinFunction& f) {
    uint32_t next = 1;
    auto see = [&](VReg v) { if (v >= next) next = v + 1; };
    for (const auto& block : f.blocks) {
        for (const auto& in : block.instrs) {
            see(in.dst); see(in.src1); see(in.src2);
            for (VReg arg : in.args) see(arg);
            // Slice/lambda destinations reserve the adjacent word as well.
            if (in.dst && in.meta.type &&
                (in.meta.type->is_slice || in.meta.type->is_lambda))
                see(in.dst + 1);
        }
        see(block.term.cond); see(block.term.ret);
    }
    return std::max(next, f.declared_max_vreg);
}

ThinInstr value_instr(ThinOp op, VReg dst, VReg src1, int64_t imm,
                      int32_t frame_off, Loc loc) {
    ThinInstr in;
    in.op = op;
    in.dst = dst;
    in.src1 = src1;
    in.imm.i = imm;
    in.meta.frame_off = frame_off;
    in.meta.width = 8;
    in.meta.type = &type_i64();
    in.loc = loc;
    return in;
}

} // namespace

EmberPreserved NopInjectionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (f.blocks.empty()) return EmberPreserved::all();

    // Derive a function-local stream. Output therefore does not depend on the
    // order in which a host compiles functions.
    uint64_t local_seed = fnv1a(seed, pass_name);
    local_seed = fnv1a(local_seed, f.name.c_str());
    local_seed ^= static_cast<uint32_t>(f.slot);
    StableRng rng{local_seed};

    // Choose from vector order, never unordered-container iteration.
    const size_t block_index = static_cast<size_t>(rng.next() % f.blocks.size());
    ThinBlock& block = f.blocks[block_index];
    const size_t insert_index = static_cast<size_t>(
        rng.next() % (block.instrs.size() + 1));

    const VReg v_seed = next_vreg(f);
    const VReg v_add = v_seed + 1;
    const int32_t old_local_off = f.frame.next_local_off;
    const int32_t seed_off = -(old_local_off + 8);
    const int32_t add_off = -(old_local_off + 16);
    const Loc loc = block.instrs.empty() ? Loc{} : block.instrs.front().loc;

    // `seed + 0` is a semantic NOP. Both destinations and both frame slots are
    // fresh, and the result is unused, so observable program state is unchanged.
    // A later DCE pass is expected to remove this chain.
    std::vector<ThinInstr> injected;
    injected.push_back(value_instr(ThinOp::ConstInt, v_seed, 0,
                                   static_cast<int64_t>(rng.next()), seed_off, loc));
    injected.push_back(value_instr(ThinOp::Add, v_add, v_seed, 0, add_off, loc));
    block.instrs.insert(block.instrs.begin() + static_cast<ptrdiff_t>(insert_index),
                        injected.begin(), injected.end());

    // New VRegs need frame-backed spill slots. Keep the conservative reserve
    // and 16-byte alignment used by Ember's existing obfuscation passes.
    f.frame.next_local_off = old_local_off + 16;
    const int32_t needed = f.frame.next_local_off + 16;
    if (needed > f.frame.frame_size)
        f.frame.frame_size = (needed + 15) & ~15;
    f.declared_max_vreg = std::max(f.declared_max_vreg, v_add + 1);
    f.ra = {}; // a previous register-allocation result is now stale

    return EmberPreserved::none();
}

} // namespace ember::examples::custom_pass
