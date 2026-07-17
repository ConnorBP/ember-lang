// mov_substitute_pass.cpp — deterministic ThinIR constant substitution and
// semantic-junk insertion example used by the embedding documentation.
//
// ThinIR is pre-register-allocation IR, so this pass deliberately works with
// VRegs rather than pretending that physical x64 registers already exist.
#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"
#include "thin_ir_mutation.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ember::examples::custom_pass {
namespace {

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t hash_text(uint64_t h, const std::string& text) {
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

int64_t from_bits(uint64_t bits) {
    int64_t value = 0;
    static_assert(sizeof(value) == sizeof(bits), "i64 size");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

ThinInstr fresh_value(ThinOp op, VReg dst, VReg src, int64_t imm,
                      int32_t frame_off, const Type* type, Loc loc) {
    ThinInstr out;
    out.op = op;
    out.dst = dst;
    out.src1 = src;
    out.imm.i = imm;
    out.meta.frame_off = frame_off;
    out.meta.width = 8;
    out.meta.type = type;
    out.loc = loc;
    return out;
}

} // namespace

struct MovSubstitutePass : EmberPassInfoMixin<MovSubstitutePass> {
    static constexpr const char* pass_name = "example-mov-substitute";
    uint64_t seed = 0x4d4f565f53554253ULL;

    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        struct Site { size_t block; size_t ordinal; };
        std::vector<Site> sites;
        for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
            for (size_t ii = 0; ii < f.blocks[bi].instrs.size(); ++ii) {
                const ThinInstr& in = f.blocks[bi].instrs[ii];
                if (in.op == ThinOp::ConstInt && in.dst != 0 &&
                    in.meta.type && in.meta.type->is_int() &&
                    in.meta.width == 8)
                    sites.push_back({bi, ii});
            }
        }
        if (sites.empty()) return EmberPreserved::all();

        ThinIRMutation mutation(f, PassGrowthLimits{});
        bool changed = false;

        // Reverse order keeps every snapshotted ordinal valid while vectors grow.
        for (auto it = sites.rbegin(); it != sites.rend(); ++it) {
            ThinBlock& block = f.blocks[it->block];
            const ThinInstr original = block.instrs[it->ordinal];
            if (!mutation.reserve_site(2, 16, 3, 0).ok()) break;
            auto encoded = mutation.allocate_scalar(original.meta.type, 8);
            auto junk = mutation.allocate_scalar(original.meta.type, 8);
            if (!encoded.ok() || !junk.ok()) break;

            uint64_t stream = hash_text(seed, f.name);
            stream = mix(stream ^ uint64_t(block.id));
            stream = mix(stream ^ uint64_t(it->ordinal));

            ThinInstr seed_instr;
            ThinInstr replacement = original;
            if ((stream & 1ULL) == 0) {
                // C = K xor (C xor K). The second operand is ThinIR's immediate
                // form (src2 == 0), not VReg zero.
                const uint64_t key_bits = mix(stream ^ 0x584f52ULL);
                seed_instr = fresh_value(ThinOp::ConstInt, encoded.get().vreg, 0,
                                         from_bits(key_bits), encoded.get().frame_off,
                                         original.meta.type, original.loc);
                replacement.op = ThinOp::Xor;
                replacement.src1 = encoded.get().vreg;
                replacement.src2 = 0;
                replacement.imm.i = from_bits(uint64_t(original.imm.i) ^ key_bits);
            } else {
                // C = (C - D) + D, with arithmetic defined modulo 2^64.
                const uint64_t delta = (mix(stream ^ 0x414444ULL) & 0x7fffULL) + 1;
                const uint64_t base = uint64_t(original.imm.i) - delta;
                seed_instr = fresh_value(ThinOp::ConstInt, encoded.get().vreg, 0,
                                         from_bits(base), encoded.get().frame_off,
                                         original.meta.type, original.loc);
                replacement.op = ThinOp::Add;
                replacement.src1 = encoded.get().vreg;
                replacement.src2 = 0;
                replacement.imm.i = from_bits(delta);
            }

            // The junk VReg is fresh and dead. Copying the real result into it
            // makes the following identity well-defined without borrowing a
            // physical scratch register or changing a live VReg.
            ThinInstr junk_copy = fresh_value(ThinOp::Move, junk.get().vreg,
                                               original.dst, 0,
                                               junk.get().frame_off,
                                               original.meta.type, original.loc);
            const ThinOp identities[] = {
                ThinOp::Move, ThinOp::Add, ThinOp::Sub,
                ThinOp::Xor, ThinOp::Or
            };
            ThinInstr junk_identity = fresh_value(
                identities[(stream >> 1) % 5], junk.get().vreg, junk.get().vreg,
                0, junk.get().frame_off, original.meta.type, original.loc);

            std::vector<ThinInstr> sequence;
            sequence.reserve(4);
            sequence.push_back(std::move(seed_instr));
            sequence.push_back(std::move(replacement));
            sequence.push_back(std::move(junk_copy));
            sequence.push_back(std::move(junk_identity));
            block.instrs.erase(block.instrs.begin() +
                               static_cast<std::ptrdiff_t>(it->ordinal));
            block.instrs.insert(block.instrs.begin() +
                                static_cast<std::ptrdiff_t>(it->ordinal),
                                std::make_move_iterator(sequence.begin()),
                                std::make_move_iterator(sequence.end()));
            if (!mutation.record_added_instructions(3).ok()) break;
            changed = true;
        }

        if (!changed) return EmberPreserved::all();
        if (!mutation.commit().ok()) return EmberPreserved::all();
        return EmberPreserved::none();
    }
};

void register_mov_substitute_pass(EmberPassRegistry& registry, uint64_t seed) {
    registry.add_factory(MovSubstitutePass::pass_name, [seed]() {
        MovSubstitutePass pass;
        pass.seed = seed;
        return make_pass_concept(std::move(pass));
    });
}

} // namespace ember::examples::custom_pass
