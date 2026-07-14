// ext_obf.cpp — Stage C Step 5 / Red 6: IR-level obfuscation passes.
//
// Red 6 (plan_POLYMORPHIC_CODE_ENGINE.md §4–§6, §9.3): every pass is a
// CONFIGURED pass. The configured `run`:
//   - fast-paths to a deterministic no-op when `options.seed_deriver == null`
//     OR `options.site_probability_ppm == 0` (the bare `add<T>()` legacy path
//     and the zero-density configured path);
//   - otherwise derives INDEPENDENT per-site, per-purpose streams through
//     src/seed_derivation.hpp (stable ORIGINAL block IDs + instruction
//     ordinals + separate purposes: select | variant | constant | truth |
//     junk-count), so a draw for one site never reshuffles a later site and
//     there is no single advancing function-wide RNG;
//   - mutates through the shared ThinIRMutation (reserve_site /
//     allocate_scalar / allocate_pair / allocate_frame_bytes / split_block /
//     canonicalize_block_ids / commit), preflying each site's worst case and
//     snapshotting the original candidates so inserted instructions are not
//     recursively transformed;
//   - obeys the configured density (site_probability_ppm) and growth limits
//     (PassGrowthLimits), stopping before a site whose worst case would
//     exceed a soft ceiling (atomic — no partial site).
//
// The private FNV-1a / SplitMix64 / StableRng / MutationState / CFG
// bookkeeping that previously lived here is REMOVED. Seed derivation is
// delegated to the shared SeedDeriver; RNG is the shared StableRng; mutation
// is the shared ThinIRMutation.
//
// `str_encrypt` is registered through the configured factory and has its
// no-op scaffolding (zero density / no candidates → no-op), but its final
// seeded key, rodata rebuild, and ThinIRMutation conversion are deferred to
// Red 7 (blob v2 is the hard gate for that migration). Its `run` keeps the
// existing fixed-key rodata transform gated by the configured density.

#include "ext_obf.hpp"

#include "../src/seed_derivation.hpp"      // SeedRequest, SeedDeriver, StableRng
#include "../src/thin_ir_mutation.hpp"     // ThinIRMutation, PassGrowthLimits

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ember::ext_obf {

namespace {

// ─── Per-site, per-purpose stream derivation ───
// Build a SeedRequest for one (pass, function, site, purpose) and derive a
// 256-bit result through the shared immutable SeedDeriver. The site identity
// uses the STABLE ORIGINAL block ID + instruction ordinal (the ordinal the
// candidate occupied in the snapshot before any mutation), so two runs of the
// same function produce identical streams regardless of how earlier sites
// shifted indices. `purpose` domain-separates the streams (select / variant /
// constant / truth / junk-count) so a draw for one purpose never reshuffles
// another.
//
// Returns the 32-byte derivation on success. On failure (a null deriver or a
// deriver error), returns false and leaves `out` zeroed; the caller treats
// this as "skip this site" (a no-op for that site, not a whole-pass abort).
bool derive_site(const PolymorphicPassOptions& opts, const char* pass_name,
                 const ThinFunction& f, uint32_t block_id,
                 uint32_t instruction_ordinal, const char* purpose,
                 std::array<uint8_t, 32>& out) {
    if (!opts.seed_deriver) return false;
    SeedRequest req;
    req.engine_version = opts.engine_version;
    req.module_id = opts.module_id;
    req.build_profile_id = opts.build_profile_id;
    req.pass_name = pass_name;
    req.pass_algorithm_version = opts.algorithm_version;
    req.function_name = f.name;
    req.logical_slot = static_cast<uint32_t>(f.slot);
    req.block_id = block_id;
    req.instruction_ordinal = instruction_ordinal;
    req.purpose = purpose;
    auto dr = opts.seed_deriver->derive(req);
    if (!dr) return false;
    out = std::move(dr.value.value());
    return true;
}

// Read a little-endian uint64 from a byte pointer (lane extraction).
uint64_t lane_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

// Build a local StableRng for one (site, purpose) from lane 0 of the
// derivation. On derivation failure, returns a StableRng seeded 0 (the caller
// checks the returned `ok`).
StableRng site_rng(const PolymorphicPassOptions& opts, const char* pass_name,
                   const ThinFunction& f, uint32_t block_id,
                   uint32_t instruction_ordinal, const char* purpose,
                   bool& ok) {
    std::array<uint8_t, 32> bytes{};
    ok = derive_site(opts, pass_name, f, block_id, instruction_ordinal, purpose, bytes);
    return StableRng{lane_u64(bytes.data())};
}

// Per-site selection: derive the "select" purpose stream and accept the site
// iff a rejection-sampled draw in [0, 1_000_000) is below the configured
// density. At ppm == 0 nothing is selected (the caller fast-paths this); at
// ppm == 1_000_000 every site is selected; at 500_000 ~50% (the compat
// default, matching the legacy `(rng.next() & 1U) == 0` eligibility).
bool site_selected(const PolymorphicPassOptions& opts, const char* pass_name,
                   const ThinFunction& f, uint32_t block_id,
                   uint32_t instruction_ordinal) {
    bool ok = false;
    StableRng rng = site_rng(opts, pass_name, f, block_id, instruction_ordinal,
                             "select", ok);
    if (!ok) return false;
    return rng.bounded(1000000ull) < opts.site_probability_ppm;
}

// A configured pass is a no-op when it cannot derive: null deriver or zero
// density. This is the bare `add<T>()` legacy path (default-constructed
// PolymorphicPassOptions{}) AND the zero-density configured path.
bool configured_noop(const PolymorphicPassOptions& opts) {
    return !opts.seed_deriver || opts.site_probability_ppm == 0;
}

// ─── Candidate predicates (shared with the legacy predicates) ───

bool is_plain_integer(const ThinInstr& in) {
    return in.dst != 0 && in.meta.type && in.meta.type->is_int() &&
           !in.meta.type->is_fn_handle && in.meta.type->struct_name.empty() &&
           !in.meta.type->is_slice && in.meta.type->array_len == 0 &&
           (in.meta.width == 1 || in.meta.width == 2 ||
            in.meta.width == 4 || in.meta.width == 8);
}

// A candidate for the integer-arithmetic passes (subst / mba_expand): an Add
// or Sub or Mul-by-two over a plain integer width with a real first operand.
bool is_arith_candidate(const ThinInstr& in) {
    const bool add_or_sub = in.op == ThinOp::Add || in.op == ThinOp::Sub;
    const bool mul_two = in.op == ThinOp::Mul && in.src1 != 0 &&
                         in.src2 == 0 && in.imm.i == 2;
    return (add_or_sub || mul_two) && in.src1 != 0 && is_plain_integer(in);
}

// A candidate for block_split: a split point that is NOT inseparable (the
// lowering pairs that rely on immediate producer/consumer adjacency). Returns
// the list of valid split indices in [1, count).
std::vector<size_t> split_candidates(const std::vector<ThinInstr>& instrs) {
    std::vector<size_t> out;
    const size_t count = instrs.size();
    out.reserve(count > 0 ? count - 1 : 0);
    for (size_t split = 1; split < count; ++split) {
        const ThinInstr& before = instrs[split - 1];
        const ThinInstr& after = instrs[split];
        bool coupled = false;
        if ((before.op == ThinOp::FieldAddr || before.op == ThinOp::IndexAddr) &&
            before.dst != 0 &&
            (after.src1 == before.dst || after.src2 == before.dst))
            coupled = true;
        if (before.op == ThinOp::DivOverflowCheck &&
            (after.op == ThinOp::Div || after.op == ThinOp::Mod))
            coupled = true;
        if (before.op == ThinOp::CallTargetGuard &&
            after.op == ThinOp::CallIndirect)
            coupled = true;
        if (before.op == ThinOp::DepthCheck &&
            (after.op == ThinOp::CallNative || after.op == ThinOp::CallScript ||
             after.op == ThinOp::CallIndirect ||
             after.op == ThinOp::CallCrossModule))
            coupled = true;
        if (!coupled) out.push_back(split);
    }
    return out;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// SubstitutionPass: MBA instruction substitution (a + b → (a^b) + 2*(a&b))
// ═══════════════════════════════════════════════════════════════════════════
//
// Eligibility: every integer Add with two VReg operands (not the immediate
// form) and a nonzero width. Density gates the whole pass on/off (ppm == 0 →
// no-op; ppm > 0 → substitute EVERY eligible Add). There is no per-site
// probabilistic selection — the identity is unique, so no "variant" stream is
// needed, and the result is fully deterministic per function. This preserves
// the legacy "substitute every eligible site" eligibility exactly.

EmberPreserved SubstitutionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (auto& blk : f.blocks) {
        // Snapshot the original Add-candidate ordinals BEFORE any insert, so
        // the per-site selection stream uses stable ordinals and inserted
        // instructions are not recursively transformed.
        struct Cand { size_t ordinal; };
        std::vector<Cand> cands;
        for (size_t i = 0; i < blk.instrs.size(); ++i) {
            const ThinInstr& in = blk.instrs[i];
            if (in.op != ThinOp::Add) continue;
            if (in.src1 == 0 || in.src2 == 0) continue;  // need two real VRegs
            if (in.meta.width == 0) continue;
            cands.push_back({i});
        }

        size_t inserted_so_far = 0;
        for (const Cand& c : cands) {
            const size_t idx = c.ordinal + inserted_so_far;
            if (idx >= blk.instrs.size()) break;
            ThinInstr& in = blk.instrs[idx];
            if (in.op != ThinOp::Add || in.src1 == 0 || in.src2 == 0 ||
                in.meta.width == 0)
                continue;

            // Per-site selection (density) over the ORIGINAL block id + ordinal.
            if (!site_selected(options, pass_name, f, blk.id, c.ordinal))
                continue;

            // Atomic per-site preflight: 3 VRegs + 24 frame bytes + 3 instrs.
            auto rs = mut.reserve_site(3, 24, 3, 0);
            if (!rs.ok()) goto subst_done;

            // Capture stable fields BEFORE any insert (the insert below
            // invalidates the `in` reference).
            const VReg src_a = in.src1;
            const VReg src_b = in.src2;
            const int32_t width = in.meta.width;
            const Type* ty = in.meta.type;
            const Loc loc = in.loc;

            auto r_xor = mut.allocate_scalar(ty, width);
            if (!r_xor.ok()) goto subst_done;
            auto r_and = mut.allocate_scalar(ty, width);
            if (!r_and.ok()) goto subst_done;
            auto r_shl = mut.allocate_scalar(ty, width);
            if (!r_shl.ok()) goto subst_done;

            const VReg v_xor = r_xor.get().vreg;
            const VReg v_and = r_and.get().vreg;
            const VReg v_shl = r_shl.get().vreg;

            // Rewrite the original (in is still valid — no insert yet).
            in.src1 = v_xor; in.src2 = v_shl; in.imm.i = 0;

            // Build the 3 MBA instructions.
            ThinInstr i_xor; i_xor.op = ThinOp::Xor; i_xor.dst = v_xor;
            i_xor.src1 = src_a; i_xor.src2 = src_b; i_xor.meta.width = width;
            i_xor.meta.type = ty; i_xor.meta.frame_off = r_xor.get().frame_off;
            i_xor.loc = loc;
            ThinInstr i_and; i_and.op = ThinOp::And; i_and.dst = v_and;
            i_and.src1 = src_a; i_and.src2 = src_b; i_and.meta.width = width;
            i_and.meta.type = ty; i_and.meta.frame_off = r_and.get().frame_off;
            i_and.loc = loc;
            ThinInstr i_shl; i_shl.op = ThinOp::Shl; i_shl.dst = v_shl;
            i_shl.src1 = v_and; i_shl.src2 = 0; i_shl.imm.i = 1;
            i_shl.meta.width = width; i_shl.meta.type = ty;
            i_shl.meta.frame_off = r_shl.get().frame_off; i_shl.loc = loc;

            // Insert the 3 new instructions BEFORE the rewritten original.
            // The original's current position is c.ordinal + inserted_so_far;
            // each insert bumps inserted_so_far so the next insert lands right
            // before the (still-last) original.
            const size_t base_pos = c.ordinal + inserted_so_far;
            blk.instrs.insert(blk.instrs.begin() + ptrdiff_t(base_pos),
                              std::move(i_xor)); ++inserted_so_far;
            blk.instrs.insert(blk.instrs.begin() + ptrdiff_t(base_pos + 1),
                              std::move(i_and)); ++inserted_so_far;
            blk.instrs.insert(blk.instrs.begin() + ptrdiff_t(base_pos + 2),
                              std::move(i_shl)); ++inserted_so_far;
            mut.record_added_instructions(3);
            changed = true;
        }
    }
subst_done:
    if (!changed) return EmberPreserved::all();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// MBAExpansionPass: seeded mixed boolean/arithmetic expansion
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-site streams: "select" gates the site (density); "variant" picks the
// MBA identity. Add/Sub accept either the VReg or immediate IR form; an
// immediate operand is first materialized into a fresh VReg. Each identity is
// valid modulo 2^N. New instructions are NOT revisited (snapshot the original
// candidate list so inserted instructions are not recursively transformed).

EmberPreserved MBAExpansionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (auto& blk : f.blocks) {
        // Snapshot the original candidate ordinals BEFORE inserting any new
        // instruction, so inserted instructions (the expansion) are not
        // recursively transformed this run.
        struct Cand { size_t ordinal; };
        std::vector<Cand> cands;
        for (size_t i = 0; i < blk.instrs.size(); ++i)
            if (is_arith_candidate(blk.instrs[i])) cands.push_back({i});

        // Walk the original ordinals; the block grows as we insert, so we
        // re-locate each original by a running offset. Because each expansion
        // inserts BEFORE the modified original and we process ordinals in
        // ascending order, the ordinals of not-yet-processed originals shift
        // by the cumulative inserted count.
        size_t inserted_so_far = 0;
        for (const Cand& c : cands) {
            const size_t idx = c.ordinal + inserted_so_far;
            if (idx >= blk.instrs.size()) break;  // defensive
            ThinInstr& in = blk.instrs[idx];
            if (!is_arith_candidate(in)) continue;  // may have been rewritten

            // Per-site selection (density) over the ORIGINAL block id + ordinal.
            if (!site_selected(options, pass_name, f, blk.id, c.ordinal))
                continue;

            const VReg a = in.src1;
            VReg b = in.src2;
            const int32_t width = in.meta.width;
            const Type* ty = in.meta.type;
            const Loc loc = in.loc;
            const int64_t orig_imm = in.imm.i;  // capture before any insert
            const ThinOp orig_op = in.op;

            // Per-site "variant" stream picks the MBA identity.
            bool vok = false;
            StableRng vrng = site_rng(options, pass_name, f, blk.id, c.ordinal,
                                      "variant", vok);
            if (!vok) continue;

            const bool mul_two = orig_op == ThinOp::Mul;
            // Worst-case preflight: materialize b (1) + up to 5 expansion VRegs
            // (Add xor/and/shl or Sub xor/not/and/shl) = 6 VRegs, 48 frame
            // bytes, 6 instructions. (Mul-by-two rewrites in place: 0 added.)
            const uint32_t wc_vregs = mul_two ? 0 : 6;
            const uint32_t wc_frame = mul_two ? 0 : 48;
            const uint32_t wc_instrs = mul_two ? 0 : 6;
            auto rs = mut.reserve_site(wc_vregs, wc_frame, wc_instrs, 0);
            if (!rs.ok()) continue;

            auto append_fresh = [&](ThinOp op, VReg s1, VReg s2,
                                   int64_t imm = 0) -> VReg {
                auto r = mut.allocate_scalar(ty, width);
                if (!r.ok()) return 0;
                ThinInstr ni; ni.op = op; ni.dst = r.get().vreg;
                ni.src1 = s1; ni.src2 = s2; ni.imm.i = imm;
                ni.meta.width = width; ni.meta.type = ty;
                ni.meta.frame_off = r.get().frame_off; ni.loc = loc;
                // Insert before the original candidate. The original's CURRENT
                // position is c.ordinal + inserted_so_far (inserted_so_far has
                // been bumped by every earlier insert this run, including the
                // ones earlier append_fresh calls in this same site made).
                // NOTE: a vector insert invalidates the `in` reference; the
                // caller re-acquires it after all appends for this site.
                blk.instrs.insert(blk.instrs.begin() +
                                     ptrdiff_t(c.ordinal + inserted_so_far),
                                 std::move(ni));
                ++inserted_so_far;
                mut.record_added_instructions(1);
                return r.get().vreg;
            };

            if (mul_two) {
                // No insert: rewrite the original in place (in is still valid).
                in.op = ThinOp::Shl; in.src2 = 0; in.imm.i = 1;
                changed = true;
                continue;
            }

            // Materialize the immediate operand if needed.
            if (b == 0) {
                b = append_fresh(ThinOp::ConstInt, 0, 0, orig_imm);
                if (b == 0) continue;
            }

            // Compute the new operands + op for the original, inserting the
            // expansion VRegs BEFORE the original. `in` is INVALID after the
            // first append_fresh; we re-acquire the original below.
            VReg new_s1 = 0, new_s2 = 0;
            ThinOp new_op = orig_op;
            bool ok_site = true;
            if (orig_op == ThinOp::Add) {
                if ((vrng.next() & 1ULL) == 0) {
                    const VReg x = append_fresh(ThinOp::Xor, a, b);
                    const VReg carry = append_fresh(ThinOp::And, a, b);
                    const VReg twice = append_fresh(ThinOp::Shl, carry, 0, 1);
                    if (x == 0 || carry == 0 || twice == 0) { ok_site = false; }
                    else { new_s1 = x; new_s2 = twice; }
                } else {
                    const VReg either = append_fresh(ThinOp::Or, a, b);
                    const VReg both = append_fresh(ThinOp::And, a, b);
                    if (either == 0 || both == 0) { ok_site = false; }
                    else { new_s1 = either; new_s2 = both; }
                }
            } else {  // Sub
                if ((vrng.next() & 1ULL) == 0) {
                    const VReg x = append_fresh(ThinOp::Xor, a, b);
                    const VReg not_a = append_fresh(ThinOp::BitNot, a, 0);
                    const VReg borrow = append_fresh(ThinOp::And, not_a, b);
                    const VReg twice = append_fresh(ThinOp::Shl, borrow, 0, 1);
                    if (x == 0 || not_a == 0 || borrow == 0 || twice == 0) { ok_site = false; }
                    else { new_s1 = x; new_s2 = twice; }
                } else {
                    const VReg not_b = append_fresh(ThinOp::BitNot, b, 0);
                    const VReg neg_b = append_fresh(ThinOp::Add, not_b, 0, 1);
                    if (not_b == 0 || neg_b == 0) { ok_site = false; }
                    else { new_op = ThinOp::Add; new_s1 = a; new_s2 = neg_b; }
                }
            }
            if (!ok_site) continue;

            // Re-acquire the original (its current position is c.ordinal +
            // inserted_so_far after all this site's appends) and rewrite it.
            ThinInstr& in2 = blk.instrs[c.ordinal + inserted_so_far];
            in2.op = new_op; in2.src1 = new_s1; in2.src2 = new_s2; in2.imm.i = 0;
            changed = true;
        }
    }

    if (!changed) return EmberPreserved::all();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// ConstantEncodingPass: seeded integer constant encoding
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-site streams: "select" gates the site (density); "constant" derives the
// encoding key; "variant" picks the form. Replaces each selected ConstInt c
// (except 0/1) with one of four exact forms (c=(c-k)+k, c=(c+k)-k, c=(c^k)^k,
// c=(c<<1)>>1 when shift-safe). All identities are modulo 2^N.

EmberPreserved ConstantEncodingPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (auto& blk : f.blocks) {
        struct Cand { size_t ordinal; };
        std::vector<Cand> cands;
        for (size_t i = 0; i < blk.instrs.size(); ++i) {
            const ThinInstr& in = blk.instrs[i];
            if (in.op == ThinOp::ConstInt && in.imm.i != 0 && in.imm.i != 1 &&
                is_plain_integer(in))
                cands.push_back({i});
        }

        size_t inserted_so_far = 0;
        for (const Cand& c : cands) {
            const size_t idx = c.ordinal + inserted_so_far;
            if (idx >= blk.instrs.size()) break;
            ThinInstr& in = blk.instrs[idx];
            if (in.op != ThinOp::ConstInt || in.imm.i == 0 || in.imm.i == 1 ||
                !is_plain_integer(in))
                continue;

            if (!site_selected(options, pass_name, f, blk.id, c.ordinal))
                continue;

            bool cok = false, vok = false;
            StableRng crng = site_rng(options, pass_name, f, blk.id, c.ordinal,
                                       "constant", cok);
            StableRng vrng = site_rng(options, pass_name, f, blk.id, c.ordinal,
                                      "variant", vok);
            if (!cok || !vok) continue;

            const int32_t bits = in.meta.width * 8;
            const uint64_t mask = bits == 64 ? ~uint64_t{0}
                                             : ((uint64_t{1} << bits) - 1);
            const uint64_t value = static_cast<uint64_t>(in.imm.i) & mask;
            uint64_t key = crng.next() & mask;
            if (key == 0 || key == 1) key = (uint64_t{0x5a} & mask);
            if (key == 0 || key == 1) key = 2;
            const uint64_t sign_bit = uint64_t{1} << (bits - 1);

            size_t form = vrng.bounded(4);
            const bool shift_safe = in.meta.type && in.meta.type->is_uint()
                ? (value & sign_bit) == 0
                : ((value >> (bits - 2)) == 0 || (value >> (bits - 2)) == 3);
            if (form == 3 && !shift_safe) form = vrng.bounded(3);

            // Worst case: form 3 (shift) adds a Shl + the base ConstInt (2
            // VRegs, 16 frame, 2 instr); forms 0-2 add 1 VReg, 8 frame, 1
            // instr. Preflight the worst case.
            auto rs = mut.reserve_site(2, 16, 2, 0);
            if (!rs.ok()) continue;

            const uint64_t base_bits = form == 0 ? ((value - key) & mask)
                                      : form == 1 ? ((value + key) & mask)
                                      : form == 2 ? ((value ^ key) & mask)
                                                  : value;
            const int64_t base = (bits < 64 && (base_bits & sign_bit) != 0)
                ? static_cast<int64_t>(base_bits | ~mask)
                : static_cast<int64_t>(base_bits);
            const int64_t encoded_key = (bits < 64 && (key & sign_bit) != 0)
                ? static_cast<int64_t>(key | ~mask)
                : static_cast<int64_t>(key);

            // Capture the original's stable fields into locals BEFORE any
            // insert: a vector insert invalidates the `in` reference, so we
            // must not touch `in` after the first insert this site makes.
            const int32_t in_width = in.meta.width;
            const Type* in_type = in.meta.type;
            const Loc in_loc = in.loc;

            auto r_base = mut.allocate_scalar(in_type, in_width);
            if (!r_base.ok()) continue;
            const VReg base_v = r_base.get().vreg;
            const int32_t base_off = r_base.get().frame_off;

            // Build the base ConstInt (not yet inserted).
            ThinInstr base_i; base_i.op = ThinOp::ConstInt;
            base_i.dst = base_v; base_i.imm.i = base;
            base_i.meta.width = in_width; base_i.meta.type = in_type;
            base_i.meta.frame_off = base_off; base_i.loc = in_loc;

            // For form 3 (shift), allocate the shift result + build the Shl
            // (consumes the base ConstInt, feeds the rewritten original).
            VReg shift_v = 0; int32_t shift_off = 0; ThinInstr sh_i;
            if (form == 3) {
                auto r_sh = mut.allocate_scalar(in_type, in_width);
                if (!r_sh.ok()) continue;
                shift_v = r_sh.get().vreg; shift_off = r_sh.get().frame_off;
                sh_i.op = ThinOp::Shl; sh_i.dst = shift_v; sh_i.src1 = base_v;
                sh_i.src2 = 0; sh_i.imm.i = 1; sh_i.meta.width = in_width;
                sh_i.meta.type = in_type; sh_i.meta.frame_off = shift_off;
                sh_i.loc = in_loc;
            }

            // Rewrite the original (in is still valid — no insert yet).
            in.src1 = (form == 3) ? shift_v : base_v;
            in.src2 = 0;
            if (form == 0) { in.op = ThinOp::Add; in.imm.i = encoded_key; }
            else if (form == 1) { in.op = ThinOp::Sub; in.imm.i = encoded_key; }
            else if (form == 2) { in.op = ThinOp::Xor; in.imm.i = encoded_key; }
            else { in.op = ThinOp::Shr; in.imm.i = 1; }

            // Now insert the new instructions BEFORE the (rewritten) original.
            // The original's current position is c.ordinal + inserted_so_far.
            // Insert the base first, then (for form 3) the shift, so the block
            // order is [base, (shift), rewritten-original].
            const size_t base_pos = c.ordinal + inserted_so_far;
            blk.instrs.insert(blk.instrs.begin() + ptrdiff_t(base_pos),
                              std::move(base_i));
            ++inserted_so_far;
            mut.record_added_instructions(1);
            if (form == 3) {
                const size_t sh_pos = c.ordinal + inserted_so_far;
                blk.instrs.insert(blk.instrs.begin() + ptrdiff_t(sh_pos),
                                  std::move(sh_i));
                ++inserted_so_far;
                mut.record_added_instructions(1);
            }
            changed = true;
        }
    }

    if (!changed) return EmberPreserved::all();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// OpaquePredicatesPass: fixed predicate + harmless rejoining bogus path
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-site streams: "select" gates the chosen site (density); "truth" picks
// always-true vs always-false. For an integer x, (x | 1) is nonzero in every
// fixed-width representation. One site is split: p = x|1; c = p != 0 (or ==0);
// branch c, continuation, bogus; bogus: pure arithmetic; jmp continuation.

EmberPreserved OpaquePredicatesPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();

    // Collect candidate sites (plain-integer defs) with their ORIGINAL block
    // id + ordinal, snapshot before any mutation.
    struct Site { uint32_t block_id; size_t block_idx; size_t ordinal;
                  VReg source; int32_t width; const Type* type; Loc loc; };
    std::vector<Site> sites;
    for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < f.blocks[bi].instrs.size(); ++ii) {
            if (is_plain_integer(f.blocks[bi].instrs[ii]))
                sites.push_back({f.blocks[bi].id, bi, ii,
                                f.blocks[bi].instrs[ii].dst,
                                f.blocks[bi].instrs[ii].meta.width,
                                f.blocks[bi].instrs[ii].meta.type,
                                f.blocks[bi].instrs[ii].loc});
        }
    }
    if (sites.empty()) return EmberPreserved::all();

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (const Site& s : sites) {
        if (!site_selected(options, pass_name, f, s.block_id, s.ordinal))
            continue;

        bool tok = false;
        StableRng truth = site_rng(options, pass_name, f, s.block_id, s.ordinal,
                                   "truth", tok);
        if (!tok) continue;
        const bool always_true = (truth.next() & 1ULL) != 0;

        // Worst case: 2 scalar VRegs (pred, cond) in the original + 2 junk
        // VRegs in the bogus block + 1 split (the continuation). The split
        // moves the suffix into a new block; the bogus block is inserted
        // directly (counted as +2 added blocks: continuation + bogus, but
        // split_block only mints 1 — we insert the bogus block directly as a
        // second block, so preflight 2 blocks). Preflight: 4 VRegs, 32 frame,
        // 4 instrs (or/ cmp / junk-xor / junk-add), 2 blocks.
        auto rs = mut.reserve_site(4, 32, 4, 2);
        if (!rs.ok()) continue;

        // Re-locate the block (its index may have shifted if an earlier site
        // in this same block inserted a continuation). The block is found by
        // its ORIGINAL id, which canonicalize has not yet remapped (we remap
        // once at the end). However, an earlier split in this run may have
        // inserted a continuation block AFTER this block, shifting later
        // block indices but not this block's id. Find by id.
        size_t bi = f.blocks.size();
        for (size_t i = 0; i < f.blocks.size(); ++i)
            if (f.blocks[i].id == s.block_id) { bi = i; break; }
        if (bi == f.blocks.size()) continue;
        // The ordinal may have shifted if an earlier site in THIS block split
        // it. To stay robust, re-scan: find the instruction whose dst matches
        // the original source VReg.
        ThinBlock& original = f.blocks[bi];
        size_t ii = original.instrs.size();
        for (size_t k = 0; k < original.instrs.size(); ++k)
            if (original.instrs[k].dst == s.source &&
                is_plain_integer(original.instrs[k])) { ii = k; break; }
        if (ii == original.instrs.size()) continue;

        // Split the block at ii+1 (the suffix moves to a continuation).
        auto sp = mut.split_block(s.block_id, ii + 1);
        if (!sp.ok()) continue;
        const uint32_t continuation_id = sp.get();

        auto r_pred = mut.allocate_scalar(s.type, s.width);
        if (!r_pred.ok()) continue;
        auto r_cond = mut.allocate_scalar(s.type, s.width);
        if (!r_cond.ok()) continue;

        // Re-find the original block (split inserted the continuation after
        // it; the original block is still at `bi` with its original id).
        ThinBlock& orig = f.blocks[bi];
        orig.instrs.push_back(ThinInstr{});
        ThinInstr& or_i = orig.instrs.back();
        or_i.op = ThinOp::Or; or_i.dst = r_pred.get().vreg;
        or_i.src1 = s.source; or_i.src2 = 0; or_i.imm.i = 1;
        or_i.meta.width = s.width; or_i.meta.type = s.type;
        or_i.meta.frame_off = r_pred.get().frame_off; or_i.loc = s.loc;

        orig.instrs.push_back(ThinInstr{});
        ThinInstr& cmp = orig.instrs.back();
        cmp.op = ThinOp::Cmp; cmp.dst = r_cond.get().vreg;
        cmp.src1 = r_pred.get().vreg; cmp.src2 = 0; cmp.imm.i = 0;
        cmp.meta.width = s.width; cmp.meta.type = s.type;
        cmp.meta.frame_off = r_cond.get().frame_off; cmp.meta.cmp =
            always_true ? 1 : 0;  // Neq zero (true) / Eq zero (false)
        cmp.loc = s.loc;
        mut.record_added_instructions(2);

        const uint32_t bogus_id = [&]() -> uint32_t {
            uint32_t m = 0;
            for (const auto& b : f.blocks) if (b.id >= m) m = b.id + 1;
            return m;
        }();

        orig.term = {};
        orig.term.kind = TermKind::Branch;
        orig.term.cond = r_cond.get().vreg;
        orig.term.target = always_true ? continuation_id : bogus_id;
        orig.term.false_target = always_true ? bogus_id : continuation_id;

        // The bogus block: pure work, rejoins the continuation.
        auto r_j1 = mut.allocate_scalar(s.type, s.width);
        if (!r_j1.ok()) continue;
        auto r_j2 = mut.allocate_scalar(s.type, s.width);
        if (!r_j2.ok()) continue;
        bool jok = false;
        StableRng junk = site_rng(options, pass_name, f, s.block_id, s.ordinal,
                                  "junk-count", jok);
        const int64_t junk_imm = jok
            ? static_cast<int64_t>((junk.next() & 0x7fffULL) | 1ULL) : 1;

        ThinBlock bogus;
        bogus.id = bogus_id;
        ThinInstr j1; j1.op = ThinOp::Xor; j1.dst = r_j1.get().vreg;
        j1.src1 = s.source; j1.src2 = s.source; j1.meta.width = s.width;
        j1.meta.type = s.type; j1.meta.frame_off = r_j1.get().frame_off;
        j1.loc = s.loc; bogus.instrs.push_back(std::move(j1));
        ThinInstr j2; j2.op = ThinOp::Add; j2.dst = r_j2.get().vreg;
        j2.src1 = r_j1.get().vreg; j2.src2 = 0; j2.imm.i = junk_imm;
        j2.meta.width = s.width; j2.meta.type = s.type;
        j2.meta.frame_off = r_j2.get().frame_off; j2.loc = s.loc;
        bogus.instrs.push_back(std::move(j2));
        bogus.term.kind = TermKind::Jmp; bogus.term.target = continuation_id;
        mut.record_added_instructions(2);

        // Insert the bogus block right after the original block (before the
        // continuation, preserving linear order). The continuation is at bi+1.
        f.blocks.insert(f.blocks.begin() + ptrdiff_t(bi + 1),
                        std::move(bogus));
        changed = true;
    }

    if (!changed) return EmberPreserved::all();
    mut.canonicalize_block_ids();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// DeadCodeInjectionPass: pure junk kept live by a same-target branch
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-site streams: "select" gates the chosen (block, split-point) site;
// "constant" derives the junk chain keys. The injected chain's final value
// drives a Branch whose two edges both reach the same continuation, so the
// branch outcome is irrelevant while ordinary DCE sees a real terminator use.

EmberPreserved DeadCodeInjectionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();
    if (f.blocks.empty()) return EmberPreserved::all();

    // Snapshot candidate (block, split-index) sites with ORIGINAL block id +
    // ordinal-ish identity (block id + a stable split index). The split index
    // is chosen by a per-site "select" stream bounded by (instrs+1).
    struct Site { uint32_t block_id; size_t block_idx; };
    std::vector<Site> sites;
    for (size_t bi = 0; bi < f.blocks.size(); ++bi)
        sites.push_back({f.blocks[bi].id, bi});

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (const Site& s : sites) {
        // Per-site selection over the original block id + a 0 ordinal (the
        // site is the whole block; use ordinal 0 as the stable identity).
        if (!site_selected(options, pass_name, f, s.block_id, 0))
            continue;

        bool cok = false;
        StableRng crng = site_rng(options, pass_name, f, s.block_id, 0,
                                  "constant", cok);
        if (!cok) continue;

        // Worst case: 5 scalar VRegs (seed, xor, mul, add, cond) + 1 split
        // (continuation) + 5 instrs. Preflight 5 VRegs, 40 frame, 5 instrs, 1
        // block.
        auto rs = mut.reserve_site(5, 40, 5, 1);
        if (!rs.ok()) continue;

        // Re-find the block by original id (an earlier split may have shifted
        // indices; the id is stable until canonicalize).
        size_t bi = f.blocks.size();
        for (size_t i = 0; i < f.blocks.size(); ++i)
            if (f.blocks[i].id == s.block_id) { bi = i; break; }
        if (bi == f.blocks.size()) continue;

        // Capture stable fields BEFORE the split (a split inserts into
        // f.blocks and may invalidate the block reference).
        const size_t instrs_size = f.blocks[bi].instrs.size();
        const Loc loc = instrs_size != 0 ? f.blocks[bi].instrs.back().loc : Loc{};

        // Choose a split index in [0, instrs.size()] from the select stream
        // (re-derive for a stable, bounded choice).
        bool sok = false;
        StableRng srng = site_rng(options, pass_name, f, s.block_id, 0,
                                  "select", sok);
        if (!sok) continue;
        const size_t split_index = instrs_size == 0
            ? 0 : static_cast<size_t>(srng.bounded(instrs_size + 1));

        auto sp = mut.split_block(s.block_id, split_index);
        if (!sp.ok()) continue;
        const uint32_t continuation_id = sp.get();

        const Type* ty = &type_i64();
        const int64_t key1 = static_cast<int64_t>(crng.next() | 1ULL);
        const int64_t key2 = static_cast<int64_t>(crng.next() | 1ULL);
        const int64_t mul_imm = static_cast<int64_t>((crng.next() & 0xffffULL) | 1ULL);
        const int64_t add_imm = static_cast<int64_t>(crng.next());
        const int64_t cmp_imm = static_cast<int64_t>(crng.next());

        auto r_seed = mut.allocate_scalar(ty, 8); if (!r_seed.ok()) continue;
        auto r_xor  = mut.allocate_scalar(ty, 8); if (!r_xor.ok())  continue;
        auto r_mul  = mut.allocate_scalar(ty, 8); if (!r_mul.ok())  continue;
        auto r_add  = mut.allocate_scalar(ty, 8); if (!r_add.ok())  continue;
        auto r_cond = mut.allocate_scalar(ty, 8); if (!r_cond.ok()) continue;

        // Re-acquire the (now-split) original block by id; the split moved
        // the suffix into a continuation but left the prefix block with the
        // same id at the same index.
        size_t bi2 = f.blocks.size();
        for (size_t i = 0; i < f.blocks.size(); ++i)
            if (f.blocks[i].id == s.block_id) { bi2 = i; break; }
        if (bi2 == f.blocks.size()) continue;
        ThinBlock& original = f.blocks[bi2];

        auto put = [&](ThinOp op, VReg dst, int32_t off, VReg s1, VReg s2,
                       int64_t imm) {
            ThinInstr ni; ni.op = op; ni.dst = dst; ni.src1 = s1; ni.src2 = s2;
            ni.imm.i = imm; ni.meta.width = 8; ni.meta.type = ty;
            ni.meta.frame_off = off; ni.loc = loc;
            original.instrs.push_back(std::move(ni));
        };
        put(ThinOp::ConstInt, r_seed.get().vreg, r_seed.get().frame_off, 0, 0, key1);
        put(ThinOp::Xor, r_xor.get().vreg, r_xor.get().frame_off, r_seed.get().vreg, 0, key2);
        put(ThinOp::Mul, r_mul.get().vreg, r_mul.get().frame_off, r_xor.get().vreg, 0, mul_imm);
        put(ThinOp::Add, r_add.get().vreg, r_add.get().frame_off, r_mul.get().vreg, 0, add_imm);
        ThinInstr cmp; cmp.op = ThinOp::Cmp; cmp.dst = r_cond.get().vreg;
        cmp.src1 = r_add.get().vreg; cmp.src2 = 0; cmp.imm.i = cmp_imm;
        cmp.meta.width = 8; cmp.meta.type = ty;
        cmp.meta.frame_off = r_cond.get().frame_off; cmp.meta.cmp = 1; cmp.loc = loc;
        original.instrs.push_back(std::move(cmp));
        mut.record_added_instructions(5);

        original.term = {};
        original.term.kind = TermKind::Branch;
        original.term.cond = r_cond.get().vreg;
        original.term.target = continuation_id;
        original.term.false_target = continuation_id;
        changed = true;
    }

    if (!changed) return EmberPreserved::all();
    mut.canonicalize_block_ids();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// StringEncryptionPass: plaintext rodata to inline stack decryption
// ═══════════════════════════════════════════════════════════════════════════
//
// Red 6 part one: registered through the configured factory with no-op
// scaffolding (zero density / no ConstStringRef → no-op). The body keeps the
// existing fixed-key (0xA5) rodata transform, GATED by the configured density,
// so a full-density run still rewrites ConstStringRef references. The final
// SEEDED key, rodata rebuild, and ThinIRMutation conversion (distinct
// data/slice slots via allocate_frame_bytes) are deferred to Red 7: the plan
// makes Thin IR blob v2 a hard gate for that migration, and Red 6 part one is
// not complete until the next chunk finishes it.

EmberPreserved StringEncryptionPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();

    constexpr uint8_t key = 0xA5;

    struct Site { ThinInstr* in; uint32_t addend; uint32_t len; };
    std::vector<Site> sites;
    std::vector<uint8_t> encrypt_byte(f.rodata.size(), 0);

    for (auto& block : f.blocks) {
        for (auto& in : block.instrs) {
            if (in.op != ThinOp::ConstStringRef || in.meta.len < 0) continue;
            const uint64_t begin = in.meta.addend;
            const uint64_t end = begin + static_cast<uint32_t>(in.meta.len);
            if (end > f.rodata.size()) continue;
            sites.push_back({&in, in.meta.addend,
                             static_cast<uint32_t>(in.meta.len)});
            for (uint64_t i = begin; i < end; ++i)
                encrypt_byte[static_cast<size_t>(i)] = 1;
        }
    }
    if (sites.empty()) return EmberPreserved::all();

    for (size_t i = 0; i < f.rodata.size(); ++i)
        if (encrypt_byte[i]) f.rodata[i] ^= key;

    int32_t next_off = f.frame.next_local_off;
    for (const Site& site : sites) {
        ThinInstr& in = *site.in;
        next_off += static_cast<int32_t>(site.len);
        const int32_t data_temp_off = -next_off;
        next_off += 16;
        const int32_t slice_off = -next_off;
        in.op = ThinOp::StringDecrypt;
        in.imm.i = key;
        in.meta.addend = site.addend;
        in.meta.len = static_cast<int32_t>(site.len);
        in.meta.data_temp_off = data_temp_off;
        in.meta.frame_off = slice_off;
    }

    f.frame.next_local_off = next_off;
    const int32_t needed = next_off + 16;
    if (needed > f.frame.frame_size)
        f.frame.frame_size = (needed + 15) & ~15;
    f.ra = {};
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// BlockSplittingPass: explicit continuation blocks
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-site streams: "select" gates the chosen long block (density) + picks the
// deterministic split point. Each original long block is split at most once.
// Continuations are inserted directly after their prefixes (preserves the
// emitter's linear block order + register-only producer/consumer adjacency).
// Inseparable boundaries (FieldAddr/IndexAddr/DivOverflowCheck/CallTargetGuard/
// DepthCheck pairs) are excluded. Block IDs + every old CFG edge are remapped
// only after all insertions (canonicalize_block_ids).

EmberPreserved BlockSplittingPass::run(ThinFunction& f, EmberAnalysisManager&) {
    if (configured_noop(options)) return EmberPreserved::all();
    constexpr size_t min_instructions = 8;

    // Snapshot the original long blocks (id + candidate split indices) BEFORE
    // any mutation, so the streams are stable.
    struct Site { uint32_t block_id; std::vector<size_t> cands; };
    std::vector<Site> sites;
    for (const auto& blk : f.blocks) {
        if (blk.instrs.size() <= min_instructions) continue;
        auto cands = split_candidates(blk.instrs);
        if (cands.empty()) continue;
        sites.push_back({blk.id, std::move(cands)});
    }
    if (sites.empty()) return EmberPreserved::all();

    ThinIRMutation mut(f, options.limits);
    bool changed = false;

    for (const Site& s : sites) {
        if (!site_selected(options, pass_name, f, s.block_id, 0))
            continue;

        // Per-site "select" stream picks the split point among the candidates.
        bool sok = false;
        StableRng srng = site_rng(options, pass_name, f, s.block_id, 0,
                                  "select", sok);
        if (!sok) continue;
        const size_t pick = s.cands.size() > 1
            ? static_cast<size_t>(srng.bounded(s.cands.size())) : 0;
        const size_t split_index = s.cands[pick];

        // Worst case: 1 split, 0 VRegs, 0 frame, 0 instrs added (the split
        // moves instructions; it adds 0 net instructions). Preflight 1 block.
        auto rs = mut.reserve_site(0, 0, 0, 1);
        if (!rs.ok()) continue;

        // Re-find the block by original id (a prior split may have shifted
        // indices). The block's instruction count is unchanged by other
        // splits (they touch other blocks), so split_index stays valid.
        size_t bi = f.blocks.size();
        for (size_t i = 0; i < f.blocks.size(); ++i)
            if (f.blocks[i].id == s.block_id) { bi = i; break; }
        if (bi == f.blocks.size()) continue;
        if (split_index > f.blocks[bi].instrs.size()) continue;

        auto sp = mut.split_block(s.block_id, split_index);
        if (!sp.ok()) continue;
        changed = true;
    }

    if (!changed) return EmberPreserved::all();
    mut.canonicalize_block_ids();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();
    return EmberPreserved::none();
}

// ═══════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════

// Configured registration: each pass is registered through a factory that
// captures `options` by value and returns a fresh PassConcept on every
// create(). Strict: rejects empty names, null factories, and duplicates.
void register_passes(EmberPassRegistry& reg, const PolymorphicPassOptions& options) {
    reg.add_factory("subst",        [options]() { return ember::make_pass_concept(SubstitutionPass{options}); });
    reg.add_factory("mba_expand",   [options]() { return ember::make_pass_concept(MBAExpansionPass{options}); });
    reg.add_factory("const_encode", [options]() { return ember::make_pass_concept(ConstantEncodingPass{options}); });
    reg.add_factory("opaque_pred",  [options]() { return ember::make_pass_concept(OpaquePredicatesPass{options}); });
    reg.add_factory("deadcode",     [options]() { return ember::make_pass_concept(DeadCodeInjectionPass{options}); });
    reg.add_factory("str_encrypt",  [options]() { return ember::make_pass_concept(StringEncryptionPass{options}); });
    reg.add_factory("block_split",  [options]() { return ember::make_pass_concept(BlockSplittingPass{options}); });
}

// Compatibility wrapper: DETERMINISTIC DEFAULTS that retain the existing
// pipeline names + eligibility behavior. A fixed-root seed deriver (seed 0,
// fully deterministic) + the legacy 50% site selection density (500_000 ppm,
// matching the legacy `(rng.next() & 1U) == 0` eligibility). Existing
// `register_passes(reg)` callers keep working unchanged; the pipeline names
// and the eligibility behavior are preserved.
void register_passes(EmberPassRegistry& reg) {
    PolymorphicPassOptions defaults;
    defaults.seed_deriver = std::make_shared<FixedRootSeedDeriver>(u64_to_root(0));
    defaults.algorithm_version = 1;
    defaults.engine_version = "ember";
    defaults.module_id = "default";
    defaults.build_profile_id = "default";
    defaults.site_probability_ppm = 500'000;
    defaults.limits = PassGrowthLimits{};
    register_passes(reg, defaults);
}

} // namespace ember::ext_obf
