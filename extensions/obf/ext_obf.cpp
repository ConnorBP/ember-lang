// ext_obf.cpp — Stage C Step 5: IR-level obfuscation passes.
// See ext_obf.hpp for the design. Obfuscation passes INCREASE code complexity
// (more instructions, harder to reverse-engineer) while preserving the result.

#include "ext_obf.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
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
            // Slices and lambdas occupy a pair even when the second word has
            // no explicit instruction at this point in the function.
            if (in.dst != 0 && in.meta.type &&
                (in.meta.type->is_slice || in.meta.type->is_lambda))
                bump(in.dst + 1);
            for (uint32_t a : in.args) bump(a);
        }
        bump(blk.term.cond); bump(blk.term.ret);
    }
    return std::max(max, f.declared_max_vreg);
}

// Fixed, implementation-independent seed derivation. Obfuscation output is
// reproducible for a function/pass pair and does not depend on compile order.
uint64_t fnv1a(uint64_t h, const char* s) {
    while (*s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t pass_seed(const ThinFunction& f, const char* pass_name) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a(h, "ember-obf-v1");
    h = fnv1a(h, pass_name);
    h = fnv1a(h, f.name.c_str());
    h ^= static_cast<uint32_t>(f.slot);
    h *= 1099511628211ULL;
    return h;
}

struct StableRng {
    uint64_t state;

    explicit StableRng(uint64_t seed) : state(seed) {}

    uint64_t next() {
        // SplitMix64: small, deterministic, and fully specified here rather
        // than delegated to a standard-library distribution.
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    size_t index(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next() % n); }
};

struct MutationState {
    ThinFunction& f;
    uint32_t next_vreg;
    int32_t next_off;
    bool changed = false;

    explicit MutationState(ThinFunction& fn)
        : f(fn), next_vreg(compute_max_vreg(fn)), next_off(fn.frame.next_local_off) {}

    std::pair<VReg, int32_t> scalar() {
        VReg v = next_vreg++;
        next_off += 8;
        changed = true;
        return {v, -next_off};
    }

    void finish() {
        if (!changed) return;
        f.frame.next_local_off = next_off;
        f.declared_max_vreg = std::max(f.declared_max_vreg, next_vreg);
        // Match the lowerer's conservative frame reserve and retain 16-byte
        // alignment. Existing frame_size may already be larger.
        const int32_t needed = next_off + 16;
        if (needed > f.frame.frame_size)
            f.frame.frame_size = (needed + 15) & ~15;
        // Passes run before regalloc normally. If a host applies one later,
        // stale assignments must not survive the newly allocated VRegs.
        f.ra = {};
    }
};

ThinInstr make_value_instr(ThinOp op, VReg dst, int32_t off,
                           VReg src1, VReg src2, int64_t imm,
                           int32_t width, const Type* type, Loc loc) {
    ThinInstr in;
    in.op = op;
    in.dst = dst;
    in.src1 = src1;
    in.src2 = src2;
    in.imm.i = imm;
    in.meta.frame_off = off;
    in.meta.width = width;
    in.meta.type = type;
    in.loc = loc;
    return in;
}

bool is_plain_integer(const ThinInstr& in) {
    return in.dst != 0 && in.meta.type && in.meta.type->is_int() &&
           !in.meta.type->is_fn_handle && in.meta.type->struct_name.empty() &&
           !in.meta.type->is_slice && in.meta.type->array_len == 0 &&
           (in.meta.width == 1 || in.meta.width == 2 ||
            in.meta.width == 4 || in.meta.width == 8);
}

// Keep block vector order and block IDs in lock-step. Besides satisfying the
// emitter's label indexing contract, inserting continuations adjacent to their
// source preserves the order expected by the current linear-scan allocator.
void canonicalize_block_ids(ThinFunction& f) {
    std::vector<uint32_t> old_ids;
    old_ids.reserve(f.blocks.size());
    for (const auto& block : f.blocks) old_ids.push_back(block.id);

    auto remap = [&](uint32_t old_id) -> uint32_t {
        for (size_t i = 0; i < old_ids.size(); ++i)
            if (old_ids[i] == old_id) return static_cast<uint32_t>(i);
        return old_id;
    };

    for (size_t i = 0; i < f.blocks.size(); ++i) {
        ThinBlock& block = f.blocks[i];
        if (block.term.kind == TermKind::Jmp) {
            block.term.target = remap(block.term.target);
        } else if (block.term.kind == TermKind::Branch) {
            block.term.target = remap(block.term.target);
            block.term.false_target = remap(block.term.false_target);
        }
        block.id = static_cast<uint32_t>(i);
    }
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

// ═══════════════════════════════════════════════════════════════════════════
// MBAExpansionPass: seeded mixed boolean/arithmetic expansion
// ═══════════════════════════════════════════════════════════════════════════
//
// Candidate sites are selected by a stable per-function RNG. Add/Sub accept
// either the VReg or immediate IR form; immediate operands are first
// materialized into fresh VRegs. Each identity is valid modulo 2^N, matching
// Ember's fixed-width integer normalization:
//
//   a + b = (a ^ b) + ((a & b) << 1)
//   a + b = (a | b) + (a & b)
//   a - b = (a ^ b) - ((~a & b) << 1)
//   a - b = a + (~b + 1)
//   a * 2 = a << 1
//
// New instructions are deliberately not revisited during this invocation, so
// one pass run always terminates and does not recursively expand itself.

EmberPreserved MBAExpansionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    StableRng rng(pass_seed(f, pass_name));
    MutationState mut(f);

    for (auto& blk : f.blocks) {
        std::vector<ThinInstr> expanded;
        expanded.reserve(blk.instrs.size() * 2);

        for (auto& in : blk.instrs) {
            const bool add_or_sub = in.op == ThinOp::Add || in.op == ThinOp::Sub;
            const bool mul_two = in.op == ThinOp::Mul && in.src1 != 0 &&
                                 in.src2 == 0 && in.imm.i == 2;
            if ((!add_or_sub && !mul_two) || in.src1 == 0 ||
                !is_plain_integer(in) || (rng.next() & 1U) == 0) {
                expanded.push_back(std::move(in));
                continue;
            }

            const VReg a = in.src1;
            VReg b = in.src2;
            const int32_t width = in.meta.width;
            const Type* ty = in.meta.type;
            const Loc loc = in.loc;

            auto append_fresh = [&](ThinOp op, VReg src1, VReg src2,
                                    int64_t imm = 0) -> VReg {
                auto [dst, off] = mut.scalar();
                expanded.push_back(make_value_instr(
                    op, dst, off, src1, src2, imm, width, ty, loc));
                return dst;
            };

            if (mul_two) {
                in.op = ThinOp::Shl;
                in.src2 = 0;
                in.imm.i = 1;
                expanded.push_back(std::move(in));
                mut.changed = true;
                continue;
            }

            // The immediate form uses VReg 0 as its sentinel. Materializing it
            // makes the two operands available to all MBA identities.
            if (b == 0) {
                b = append_fresh(ThinOp::ConstInt, 0, 0, in.imm.i);
            }

            if (in.op == ThinOp::Add) {
                if ((rng.next() & 1U) == 0) {
                    const VReg x = append_fresh(ThinOp::Xor, a, b);
                    const VReg carry = append_fresh(ThinOp::And, a, b);
                    const VReg twice_carry = append_fresh(ThinOp::Shl, carry, 0, 1);
                    in.src1 = x;
                    in.src2 = twice_carry;
                } else {
                    const VReg either = append_fresh(ThinOp::Or, a, b);
                    const VReg both = append_fresh(ThinOp::And, a, b);
                    in.src1 = either;
                    in.src2 = both;
                }
                in.imm.i = 0;
            } else if ((rng.next() & 1U) == 0) {
                const VReg x = append_fresh(ThinOp::Xor, a, b);
                const VReg not_a = append_fresh(ThinOp::BitNot, a, 0);
                const VReg borrow = append_fresh(ThinOp::And, not_a, b);
                const VReg twice_borrow = append_fresh(ThinOp::Shl, borrow, 0, 1);
                in.src1 = x;
                in.src2 = twice_borrow;
                in.imm.i = 0;
            } else {
                const VReg not_b = append_fresh(ThinOp::BitNot, b, 0);
                const VReg neg_b = append_fresh(ThinOp::Add, not_b, 0, 1);
                in.op = ThinOp::Add;
                in.src1 = a;
                in.src2 = neg_b;
                in.imm.i = 0;
            }

            expanded.push_back(std::move(in));
            mut.changed = true;
        }
        blk.instrs = std::move(expanded);
    }

    mut.finish();
    return mut.changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// ConstantEncodingPass: seeded integer constant encoding
// ═══════════════════════════════════════════════════════════════════════════
//
// Replaces each selected ConstInt c (except 0/1) with one of four exact forms:
//   c = (c - k) + k
//   c = (c + k) - k
//   c = (c ^ k) ^ k
//   c = (c << 1) >> 1       (only when that signed/unsigned shift round-trip
//                             is valid for the destination width)
// The first three identities are valid modulo 2^N and therefore include all
// signed/unsigned values without invoking host-language signed arithmetic.

EmberPreserved ConstantEncodingPass::run(ThinFunction& f, EmberAnalysisManager&) {
    StableRng rng(pass_seed(f, pass_name));
    MutationState mut(f);

    for (auto& blk : f.blocks) {
        std::vector<ThinInstr> encoded;
        encoded.reserve(blk.instrs.size() * 2);

        for (auto& in : blk.instrs) {
            if (in.op != ThinOp::ConstInt || in.imm.i == 0 || in.imm.i == 1 ||
                !is_plain_integer(in) || (rng.next() & 1U) == 0) {
                encoded.push_back(std::move(in));
                continue;
            }

            const int32_t bits = in.meta.width * 8;
            const uint64_t mask = bits == 64 ? ~uint64_t{0}
                                             : ((uint64_t{1} << bits) - 1);
            const uint64_t value = static_cast<uint64_t>(in.imm.i) & mask;
            uint64_t key = rng.next() & mask;
            if (key == 0 || key == 1) key = (uint64_t{0x5a} & mask);
            if (key == 0 || key == 1) key = 2;

            size_t form = rng.index(4);
            const uint64_t top_two = value >> (bits - 2);
            const bool shift_safe = in.meta.type && in.meta.type->is_uint()
                ? (value & (uint64_t{1} << (bits - 1))) == 0
                : (top_two == 0 || top_two == 3);
            if (form == 3 && !shift_safe) form = rng.index(3);

            const uint64_t base_bits = form == 0 ? ((value - key) & mask)
                                      : form == 1 ? ((value + key) & mask)
                                      : form == 2 ? ((value ^ key) & mask)
                                                  : value;
            const uint64_t sign_bit = uint64_t{1} << (bits - 1);
            const int64_t base = (bits < 64 && (base_bits & sign_bit) != 0)
                ? static_cast<int64_t>(base_bits | ~mask)
                : static_cast<int64_t>(base_bits);
            const int64_t encoded_key = (bits < 64 && (key & sign_bit) != 0)
                ? static_cast<int64_t>(key | ~mask)
                : static_cast<int64_t>(key);
            auto [base_v, base_off] = mut.scalar();
            encoded.push_back(make_value_instr(
                ThinOp::ConstInt, base_v, base_off, 0, 0, base,
                in.meta.width, in.meta.type, in.loc));

            in.src1 = base_v;
            in.src2 = 0;
            if (form == 0) {
                in.op = ThinOp::Add;
                in.imm.i = encoded_key;
            } else if (form == 1) {
                in.op = ThinOp::Sub;
                in.imm.i = encoded_key;
            } else if (form == 2) {
                in.op = ThinOp::Xor;
                in.imm.i = encoded_key;
            } else {
                auto [shifted_v, shifted_off] = mut.scalar();
                encoded.push_back(make_value_instr(
                    ThinOp::Shl, shifted_v, shifted_off, base_v, 0, 1,
                    in.meta.width, in.meta.type, in.loc));
                in.op = ThinOp::Shr;
                in.src1 = shifted_v;
                in.imm.i = 1;
            }
            encoded.push_back(std::move(in));
            mut.changed = true;
        }
        blk.instrs = std::move(encoded);
    }

    mut.finish();
    return mut.changed ? EmberPreserved::none() : EmberPreserved::all();
}

// ═══════════════════════════════════════════════════════════════════════════
// OpaquePredicatesPass: fixed predicates with a harmless rejoining path
// ═══════════════════════════════════════════════════════════════════════════
//
// For an integer x, (x | 1) is nonzero in every fixed-width representation.
// One deterministic site is split into:
//
//   p = x | 1
//   c = p != 0                 (or p == 0 for the always-false variant)
//   branch c, continuation, bogus
// bogus: pure arithmetic only; jmp continuation
//
// The bogus edge is semantically harmless even independently of the identity:
// it changes no existing VReg or visible memory and rejoins the real path.

EmberPreserved OpaquePredicatesPass::run(ThinFunction& f, EmberAnalysisManager&) {
    struct Site {
        size_t block;
        size_t instr;
        VReg source;
        int32_t width;
        const Type* type;
        Loc loc;
    };

    std::vector<Site> sites;
    for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
        const auto& instrs = f.blocks[bi].instrs;
        for (size_t ii = 0; ii < instrs.size(); ++ii) {
            if (is_plain_integer(instrs[ii])) {
                sites.push_back({bi, ii, instrs[ii].dst, instrs[ii].meta.width,
                                 instrs[ii].meta.type, instrs[ii].loc});
            }
        }
    }
    if (sites.empty()) return EmberPreserved::all();

    StableRng rng(pass_seed(f, pass_name));
    const Site site = sites[rng.index(sites.size())];
    const bool always_true = (rng.next() & 1U) != 0;
    MutationState mut(f);

    // Save references only until vector growth; all data required below was
    // copied into Site. The continuation receives the suffix and old term.
    ThinBlock& original = f.blocks[site.block];
    std::vector<ThinInstr> suffix;
    auto split = original.instrs.begin() + static_cast<ptrdiff_t>(site.instr + 1);
    suffix.insert(suffix.end(), std::make_move_iterator(split),
                  std::make_move_iterator(original.instrs.end()));
    original.instrs.erase(split, original.instrs.end());
    ThinTerm old_term = original.term;

    auto [pred_v, pred_off] = mut.scalar();
    auto [cond_v, cond_off] = mut.scalar();
    original.instrs.push_back(make_value_instr(
        ThinOp::Or, pred_v, pred_off, site.source, 0, 1,
        site.width, site.type, site.loc));
    ThinInstr cmp = make_value_instr(
        ThinOp::Cmp, cond_v, cond_off, pred_v, 0, 0,
        site.width, site.type, site.loc);
    cmp.meta.cmp = always_true ? 1 : 0; // Neq zero (true), or Eq zero (false)
    original.instrs.push_back(std::move(cmp));

    const uint32_t continuation_id = static_cast<uint32_t>(f.blocks.size());
    const uint32_t bogus_id = continuation_id + 1;
    original.term = {};
    original.term.kind = TermKind::Branch;
    original.term.cond = cond_v;
    original.term.target = always_true ? continuation_id : bogus_id;
    original.term.false_target = always_true ? bogus_id : continuation_id;

    ThinBlock continuation;
    continuation.id = continuation_id;
    continuation.instrs = std::move(suffix);
    continuation.term = old_term;

    // The untaken path contains convincing but pure work. Its destinations and
    // spill slots are fresh, and it rejoins without touching program state.
    ThinBlock bogus;
    bogus.id = bogus_id;
    auto [junk1_v, junk1_off] = mut.scalar();
    auto [junk2_v, junk2_off] = mut.scalar();
    bogus.instrs.push_back(make_value_instr(
        ThinOp::Xor, junk1_v, junk1_off, site.source, site.source, 0,
        site.width, site.type, site.loc));
    bogus.instrs.push_back(make_value_instr(
        ThinOp::Add, junk2_v, junk2_off, junk1_v, 0,
        static_cast<int64_t>((rng.next() & 0x7fffU) | 1U),
        site.width, site.type, site.loc));
    bogus.term.kind = TermKind::Jmp;
    bogus.term.target = continuation_id;

    auto insert_at = f.blocks.begin() + static_cast<ptrdiff_t>(site.block + 1);
    insert_at = f.blocks.insert(insert_at, std::move(bogus));
    f.blocks.insert(std::next(insert_at), std::move(continuation));
    canonicalize_block_ids(f);
    mut.finish();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// DeadCodeInjectionPass: pure junk kept live by a same-target branch
// ═══════════════════════════════════════════════════════════════════════════
//
// DCE correctly removes an unused pure chain. To retain injected work without
// lying about side effects, its final value drives a Branch whose two edges
// both reach the same continuation. The branch outcome is irrelevant, while
// ordinary DCE sees a real terminator use and therefore retains the chain.

EmberPreserved DeadCodeInjectionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (f.blocks.empty()) return EmberPreserved::all();

    StableRng rng(pass_seed(f, pass_name));
    const size_t block_index = rng.index(f.blocks.size());
    MutationState mut(f); // scan all VRegs before temporarily moving the suffix
    ThinBlock& original = f.blocks[block_index];
    const size_t split_index = rng.index(original.instrs.size() + 1);

    std::vector<ThinInstr> suffix;
    auto split = original.instrs.begin() + static_cast<ptrdiff_t>(split_index);
    suffix.insert(suffix.end(), std::make_move_iterator(split),
                  std::make_move_iterator(original.instrs.end()));
    original.instrs.erase(split, original.instrs.end());
    ThinTerm old_term = original.term;

    const Type* ty = &type_i64();
    const Loc loc = !suffix.empty() ? suffix.front().loc
                                    : (!original.instrs.empty() ? original.instrs.back().loc : Loc{});
    const int64_t key1 = static_cast<int64_t>(rng.next() | 1ULL);
    const int64_t key2 = static_cast<int64_t>(rng.next() | 1ULL);

    auto [seed_v, seed_off] = mut.scalar();
    auto [xor_v, xor_off] = mut.scalar();
    auto [mul_v, mul_off] = mut.scalar();
    auto [add_v, add_off] = mut.scalar();
    auto [cond_v, cond_off] = mut.scalar();

    original.instrs.push_back(make_value_instr(
        ThinOp::ConstInt, seed_v, seed_off, 0, 0, key1, 8, ty, loc));
    original.instrs.push_back(make_value_instr(
        ThinOp::Xor, xor_v, xor_off, seed_v, 0, key2, 8, ty, loc));
    original.instrs.push_back(make_value_instr(
        ThinOp::Mul, mul_v, mul_off, xor_v, 0,
        static_cast<int64_t>((rng.next() & 0xffffU) | 1U), 8, ty, loc));
    original.instrs.push_back(make_value_instr(
        ThinOp::Add, add_v, add_off, mul_v, 0,
        static_cast<int64_t>(rng.next()), 8, ty, loc));
    ThinInstr cmp = make_value_instr(
        ThinOp::Cmp, cond_v, cond_off, add_v, 0,
        static_cast<int64_t>(rng.next()), 8, ty, loc);
    cmp.meta.cmp = 1; // Neq; either result is safe because both edges coincide.
    original.instrs.push_back(std::move(cmp));

    const uint32_t continuation_id = static_cast<uint32_t>(f.blocks.size());
    original.term = {};
    original.term.kind = TermKind::Branch;
    original.term.cond = cond_v;
    original.term.target = continuation_id;
    original.term.false_target = continuation_id;

    ThinBlock continuation;
    continuation.id = continuation_id;
    continuation.instrs = std::move(suffix);
    continuation.term = old_term;
    f.blocks.insert(f.blocks.begin() + static_cast<ptrdiff_t>(block_index + 1),
                    std::move(continuation));
    canonicalize_block_ids(f);

    mut.finish();
    return EmberPreserved::none();
}

void register_passes(EmberPassRegistry& reg) {
    reg.add<SubstitutionPass>("subst");
    reg.add<MBAExpansionPass>("mba_expand");
    reg.add<ConstantEncodingPass>("const_encode");
    reg.add<OpaquePredicatesPass>("opaque_pred");
    reg.add<DeadCodeInjectionPass>("deadcode");
}

} // namespace ember::ext_obf
