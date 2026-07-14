// polymorphic_pass_test.cpp — Red 6 part one: the configured polymorphic
// obfuscation pass test.
//
// RED-GREEN TDD driver for the serialization-independent portion of Red 6
// (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 6). Built as a standalone
// executable wired into CMakeLists.txt WITHOUT add_test, so the filtered CTest
// total stays 67 (run explicitly: ./buildt/polymorphic_pass_test).
//
// Coverage (per the Red 6 spec), for each of the six migrated transforms
// subst, mba_expand, const_encode, opaque_pred, deadcode, block_split:
//   (A) no-op (zero density) vs changed (configured density) preservation;
//   (B) same-seed serialized Thin IR equality (two independent runs produce
//       byte-identical serialized blobs);
//   (C) two pinned seeds produce structurally different but value-equivalent
//       IR (where the pass selects ≥1 site);
//   (D) baseline differential execution (emit + call: transformed return ==
//       baseline return);
//   (E) validate → serialize → deserialize → validate round-trip;
//   (F) stale-regalloc invalidation (a pre-seeded ra is cleared on change);
//   (G) exact max_sites / added-instruction / added-block / added-VReg /
//       added-frame / growth-ratio boundaries, including stop-before-site
//       atomicity (a mid-site limit failure commits no partial site).
//
// Plus the shape matrix:
//   - widths 1/2/4/8 (structural no-op + changed eligibility);
//   - empty function + no-candidate function (no-op for every pass);
//   - representative CFGs: straight-line, diamond, loop, long-block.
//
// str_encrypt: registered through the configured factory + no-op scaffolding
// only (its final seeded key / rodata rebuild / ThinIRMutation conversion are
// Red 7; blob v2 is the hard gate).
//
// Options validation:
//   (V1) site_probability_ppm > 1'000'000 → ExtensionError;
//   (V2) growth_denominator == 0 → ExtensionError;
//   (V3) a valid options record → ok.
//
// Configured factory registration:
//   (R1) register_passes(reg, options) registers all 7 names;
//   (R2) register_passes(reg) compat wrapper registers the same 7 names;
//   (R3) two create() calls yield distinct fresh PassConcept instances.

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/extension_registry.hpp"
#include "../src/seed_derivation.hpp"
#include "../src/polymorphic_options.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"      // serialize/deserialize/validate/verify
#include "../src/thin_ir_mutation.hpp" // PassGrowthLimits, compute_central_max_vreg
#include "../src/thin_emit.hpp"       // emit_x64
#include "../src/codegen.hpp"         // CodeGenCtx
#include "../src/engine.hpp"          // CompiledFn, finalize, call_i64_i64
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"      // free_executable
#include "../extensions/obf/ext_obf.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ember;
using namespace ember::ext_obf;

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ─── Helpers ───

static size_t total_instrs(const ThinFunction& f) {
    size_t n = 0;
    for (const auto& b : f.blocks) n += b.instrs.size();
    return n;
}

// Build a shared const FixedRootSeedDeriver from a 64-bit seed.
static std::shared_ptr<const SeedDeriver> fixed_deriver(uint64_t seed) {
    return std::make_shared<FixedRootSeedDeriver>(u64_to_root(seed));
}

// Make a configured options record (deterministic). density_ppm = 0 → no-op;
// 1'000'000 → every eligible site; 500'000 → 50% eligibility (the compat
// default).
static PolymorphicPassOptions make_opts(uint64_t seed, uint32_t density_ppm,
                                         PassGrowthLimits limits = PassGrowthLimits{}) {
    PolymorphicPassOptions o;
    o.seed_deriver = fixed_deriver(seed);
    o.algorithm_version = 1;
    o.engine_version = "ember-test";
    o.module_id = "poly-test-mod";
    o.build_profile_id = "poly-test-profile";
    o.site_probability_ppm = density_ppm;
    o.limits = limits;
    return o;
}

// Run a single named configured pass on f IN PLACE. Returns {preserved, count}
// reflecting the mutated f. Callers that need the mutation to be visible on
// their own f (serialization, emit, regalloc checks) use this directly.
struct SingleResult { bool all_preserved; size_t instr_count; };
static SingleResult run_pass(const EmberPassRegistry& reg, const char* name,
                             ThinFunction& f) {
    EmberAnalysisManager am;
    auto pc = reg.create(name);
    if (!pc) return {false, 0};
    EmberPreserved p = pc->run(f, am);
    return {p.all_preserved(), total_instrs(f)};
}

// ─── Thin IR fixtures (hand-built; no parser/sema/lower) ───
//
// All fixtures are no-arg i64() functions so the differential-execution path
// can call them via call_i64_i64(entry). The builder installs a real i64 Type
// in owned_types so validate/emit have a concrete ret_type.

static const Type* install_i64(ThinFunction& f) {
    auto t = std::make_shared<Type>();
    t->prim = Prim::I64;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    return f.ret_type;
}

static const Type* install_int(ThinFunction& f, Prim p, int32_t width) {
    auto t = std::make_shared<Type>();
    t->prim = p;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    (void)width;
    return f.ret_type;
}

// A minimal valid frame plan (just the rbx save slot + 16-byte frame).
static void minimal_frame(ThinFunction& f, int32_t next_local_off = 8) {
    f.frame.frame_size = 16;
    f.frame.rbx_save_offset = -8;
    f.frame.struct_ret_ptr_offset = 0;
    f.frame.arg_temps_base = 0;
    f.frame.next_local_off = next_local_off;
    f.frame.returns_struct_by_ptr = false;
}

// A scalar Type for a given width + signedness, stored in owned_types.
static const Type* scalar_type(ThinFunction& f, Prim p) {
    auto t = std::make_shared<Type>();
    t->prim = p;
    const Type* raw = t.get();
    f.owned_types.push_back(std::move(t));
    return raw;
}

// Two-long-block: two separate long blocks (each >8 instrs) joined by a
// jmp, returning the second block's running sum. Provides multiple candidate
// sites for seed-dependent variation (block_split has two long blocks; the
// arith/const/opaque/deadcode passes have many sites across both blocks).
static ThinFunction build_two_long_blocks(size_t n) {
    ThinFunction f;
    f.name = "twolong";
    f.slot = 0;
    install_i64(f);
    // block0: n ConstInt + Add chain -> jmp block1
    ThinBlock b0; b0.id = 0;
    int32_t off = 16; VReg prev = 0;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * i + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b0.instrs.push_back(c); off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * i + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b0.instrs.push_back(add); off += 8; prev = add.dst;
    }
    b0.term.kind = TermKind::Jmp; b0.term.target = 1;
    f.blocks.push_back(std::move(b0));
    // block1: another n ConstInt + Add chain -> return prev
    ThinBlock b1; b1.id = 1;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * (n + i) + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b1.instrs.push_back(c); off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * (n + i) + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b1.instrs.push_back(add); off += 8; prev = add.dst;
    }
    b1.term.kind = TermKind::Return; b1.term.ret = prev;
    f.blocks.push_back(std::move(b1));
    f.declared_max_vreg = prev + 1;
    f.frame.next_local_off = off;
    f.frame.frame_size = (off + 15) & ~15;
    return f;
}

// Straight-line: two ConstInt + Add, returns the sum. Width-parameterized.
//   v1 = ConstInt(a); v2 = ConstInt(b); v3 = Add(v1,v2); return v3.
static ThinFunction build_straight_line(int64_t a, int64_t b, Prim p,
                                         int32_t width) {
    ThinFunction f;
    f.name = "sl";
    f.slot = 0;
    install_int(f, p, width);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c1; c1.op = ThinOp::ConstInt; c1.dst = 1; c1.imm.i = a;
    c1.meta.width = width; c1.meta.type = scalar_type(f, p); c1.meta.frame_off = -16;
    b0.instrs.push_back(c1);
    ThinInstr c2; c2.op = ThinOp::ConstInt; c2.dst = 2; c2.imm.i = b;
    c2.meta.width = width; c2.meta.type = scalar_type(f, p); c2.meta.frame_off = -24;
    b0.instrs.push_back(c2);
    ThinInstr add; add.op = ThinOp::Add; add.dst = 3; add.src1 = 1; add.src2 = 2;
    add.meta.width = width; add.meta.type = scalar_type(f, p); add.meta.frame_off = -32;
    b0.instrs.push_back(add);
    b0.term.kind = TermKind::Return; b0.term.ret = 3;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 4;
    f.frame.next_local_off = 32;
    f.frame.frame_size = 48;
    return f;
}

// Long-block: N ConstInt + Add chain, returns the running sum. Width 8.
// Enough instructions to be a block_split candidate (>= 9 instrs).
static ThinFunction build_long_block(size_t n) {
    ThinFunction f;
    f.name = "longblock";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    int32_t off = 16;
    VReg prev = 0;
    for (size_t i = 0; i < n; ++i) {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = VReg(2 * i + 1);
        c.imm.i = int64_t(i + 1); c.meta.width = 8;
        c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -off;
        b0.instrs.push_back(c);
        off += 8;
        if (prev == 0) { prev = c.dst; continue; }
        ThinInstr add; add.op = ThinOp::Add; add.dst = VReg(2 * i + 2);
        add.src1 = prev; add.src2 = c.dst; add.meta.width = 8;
        add.meta.type = scalar_type(f, Prim::I64); add.meta.frame_off = -off;
        b0.instrs.push_back(add);
        off += 8;
        prev = add.dst;
    }
    b0.term.kind = TermKind::Return; b0.term.ret = prev;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = prev + 1;
    f.frame.next_local_off = off;
    f.frame.frame_size = (off + 15) & ~15;
    return f;
}

// Diamond: entry branches on a constant-true condition to two edges that
// rejoin a merge block which returns. Provides a multi-block CFG where
// opaque_pred / deadcode / block_split can operate without breaking the join.
static ThinFunction build_diamond() {
    ThinFunction f;
    f.name = "diamond";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    const Type* i64 = scalar_type(f, Prim::I64);
    // block0: v1 = 1; v2 = (v1 != 0) via Cmp; branch v2 -> block1, block2
    ThinBlock b0; b0.id = 0;
    {
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 1;
        c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -16;
        b0.instrs.push_back(c);
        ThinInstr cmp; cmp.op = ThinOp::Cmp; cmp.dst = 2; cmp.src1 = 1; cmp.src2 = 0;
        cmp.imm.i = 0; cmp.meta.width = 8; cmp.meta.type = i64; cmp.meta.cmp = 0;
        cmp.meta.frame_off = -24; b0.instrs.push_back(cmp);
    }
    b0.term.kind = TermKind::Branch; b0.term.cond = 2; b0.term.target = 1; b0.term.false_target = 2;
    f.blocks.push_back(std::move(b0));
    // block1: v3 = 10; jmp block3
    ThinBlock b1; b1.id = 1;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 3; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -32; b1.instrs.push_back(c); }
    b1.term.kind = TermKind::Jmp; b1.term.target = 3;
    f.blocks.push_back(std::move(b1));
    // block2: v4 = 10; jmp block3 (both edges produce 10)
    ThinBlock b2; b2.id = 2;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 4; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -40; b2.instrs.push_back(c); }
    b2.term.kind = TermKind::Jmp; b2.term.target = 3;
    f.blocks.push_back(std::move(b2));
    // block3: return 10 (use v3 or v4? merge via a phi is not modeled; both
    // are 10, so return a fresh ConstInt 10 to keep the IR SSA-free + valid).
    ThinBlock b3; b3.id = 3;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 5; c.imm.i = 10;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -48; b3.instrs.push_back(c); }
    b3.term.kind = TermKind::Return; b3.term.ret = 5;
    f.blocks.push_back(std::move(b3));
    f.declared_max_vreg = 6;
    f.frame.next_local_off = 48;
    f.frame.frame_size = 64;
    return f;
}

// Loop: a counted loop that sums 1..k into an accumulator, returns the sum.
// Straight-line body in a single loop block (no nested CFG). Width 8.
static ThinFunction build_loop(int64_t k) {
    ThinFunction f;
    f.name = "loop";
    f.slot = 0;
    install_i64(f);
    const Type* i64 = scalar_type(f, Prim::I64);
    // block0: v1 = 0 (acc); v2 = 0 (i); jmp block1
    ThinBlock b0; b0.id = 0;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 0;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -16; b0.instrs.push_back(c); }
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 2; c.imm.i = 0;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -24; b0.instrs.push_back(c); }
    b0.term.kind = TermKind::Jmp; b0.term.target = 1;
    f.blocks.push_back(std::move(b0));
    // block1: header: v3 = ConstInt(k); v4 = Cmp(i < k); branch v4 -> block2, block3
    ThinBlock b1; b1.id = 1;
    { ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 3; c.imm.i = k;
      c.meta.width = 8; c.meta.type = i64; c.meta.frame_off = -32; b1.instrs.push_back(c);
      ThinInstr cmp; cmp.op = ThinOp::Cmp; cmp.dst = 4; cmp.src1 = 2; cmp.src2 = 3;
      cmp.imm.i = 0; cmp.meta.width = 8; cmp.meta.type = i64; cmp.meta.cmp = 2; // Lt
      cmp.meta.frame_off = -40; b1.instrs.push_back(cmp); }
    b1.term.kind = TermKind::Branch; b1.term.cond = 4; b1.term.target = 2; b1.term.false_target = 3;
    f.blocks.push_back(std::move(b1));
    // block2: body: v5 = acc + i; v6 = i + 1; (acc=v5, i=v6 via Move); jmp block1
    ThinBlock b2; b2.id = 2;
    { ThinInstr add; add.op = ThinOp::Add; add.dst = 5; add.src1 = 1; add.src2 = 2;
      add.meta.width = 8; add.meta.type = i64; add.meta.frame_off = -48; b2.instrs.push_back(add);
      ThinInstr inc; inc.op = ThinOp::Add; inc.dst = 6; inc.src1 = 2; inc.imm.i = 1;
      inc.meta.width = 8; inc.meta.type = i64; inc.meta.frame_off = -56; b2.instrs.push_back(inc);
      ThinInstr m1; m1.op = ThinOp::Move; m1.dst = 1; m1.src1 = 5;
      m1.meta.width = 8; m1.meta.type = i64; m1.meta.frame_off = -16; b2.instrs.push_back(m1);
      ThinInstr m2; m2.op = ThinOp::Move; m2.dst = 2; m2.src1 = 6;
      m2.meta.width = 8; m2.meta.type = i64; m2.meta.frame_off = -24; b2.instrs.push_back(m2); }
    b2.term.kind = TermKind::Jmp; b2.term.target = 1;
    f.blocks.push_back(std::move(b2));
    // block3: return acc
    ThinBlock b3; b3.id = 3;
    b3.term.kind = TermKind::Return; b3.term.ret = 1;
    f.blocks.push_back(std::move(b3));
    f.declared_max_vreg = 7;
    f.frame.next_local_off = 56;
    f.frame.frame_size = 64;
    return f;
}

// Empty function: one block, no instrs, returns v0... but v0 is invalid. Use
// a single ConstInt 0 return so the function is valid but has no candidates.
static ThinFunction build_empty() {
    ThinFunction f;
    f.name = "empty";
    f.slot = 0;
    install_i64(f);
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 1; c.imm.i = 0;
    c.meta.width = 8; c.meta.type = scalar_type(f, Prim::I64); c.meta.frame_off = -16;
    b0.instrs.push_back(c);
    b0.term.kind = TermKind::Return; b0.term.ret = 1;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 2;
    f.frame.next_local_off = 16;
    f.frame.frame_size = 32;
    return f;
}

// No-candidate function: has an instruction but NONE are eligible for ANY
// of the six passes — a single ConstBool (bool is NOT plain-integer, so
// opaque_pred skips; not a ConstInt, so const_encode skips; not an Add/Sub,
// so subst/mba_expand skip; 1 instruction <= 8, so block_split skips; and
// deadcode's 5-instr injection would exceed the 3x growth ratio on a 1-instr
// function, so reserve_site stops it). Used to assert every pass is a no-op
// on a genuinely-candidate-free function at full density.
static ThinFunction build_no_candidate() {
    ThinFunction f;
    f.name = "nocand";
    f.slot = 0;
    auto t = std::make_shared<Type>();
    t->prim = Prim::Bool;
    f.ret_type = t.get();
    f.owned_types.push_back(std::move(t));
    const Type* bool_ty = f.ret_type;
    minimal_frame(f, 8);
    ThinBlock b0; b0.id = 0;
    ThinInstr c; c.op = ThinOp::ConstBool; c.dst = 1; c.imm.i = 1;
    c.meta.width = 1; c.meta.type = bool_ty; c.meta.frame_off = -16; b0.instrs.push_back(c);
    b0.term.kind = TermKind::Return; b0.term.ret = 1;
    f.blocks.push_back(std::move(b0));
    f.declared_max_vreg = 2;
    f.frame.next_local_off = 16;
    f.frame.frame_size = 32;
    return f;
}

// ─── Execution harness ───
// Emit + finalize + call a no-arg i64() ThinFunction. Returns the i64 result,
// or INT64_MIN on emit/finalize failure (so the caller can distinguish).
static int64_t emit_and_call(ThinFunction& f) {
    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = 0;
    ctx.enable_ir_backend = true;
    ctx.use_context_reg = false;
    CompiledFn cf = emit_x64(f, ctx);
    if (cf.bytes.empty()) return INT64_MIN;
    if (!finalize(cf)) return INT64_MIN;
    int64_t r = call_i64_i64(cf.entry);
    if (cf.exec) free_executable(cf.exec);
    return r;
}

// ─── The six migrated passes + str_encrypt ───
static const char* kSixPasses[] = {
    "subst", "mba_expand", "const_encode", "opaque_pred", "deadcode", "block_split"
};
static const size_t kSixCount = sizeof(kSixPasses) / sizeof(kSixPasses[0]);

// Per-pass candidate-rich fixtures (one per pass, chosen so the pass has ≥1
// eligible site at full density).
static ThinFunction candidate_fixture(const char* pass) {
    if (std::strcmp(pass, "subst") == 0)        return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "mba_expand") == 0)  return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "const_encode") == 0) return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "opaque_pred") == 0)  return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "deadcode") == 0)     return build_straight_line(7, 9, Prim::I64, 8);
    if (std::strcmp(pass, "block_split") == 0)  return build_long_block(9);
    return ThinFunction{};
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== polymorphic_pass_test: Red 6 part one ===\n");

    // ═══ (V) Options validation ═══
    std::printf("\n--- (V) options validation ---\n");
    {
        // V1: ppm > 1'000'000 → error.
        auto r = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p",
                                          1'000'001u, PassGrowthLimits{});
        ck(!r, "V1: site_probability_ppm > 1000000 rejected");
        if (!r) ck(r.error->registry == std::string(kPolymorphicOptionsRegistryId),
                   "V1: error registry == ember-polymorphic-options");
        // V2: growth_denominator == 0 → error.
        PassGrowthLimits bad; bad.growth_denominator = 0;
        auto r2 = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p", 500000u, bad);
        ck(!r2, "V2: growth_denominator == 0 rejected");
        // V3: valid record → ok.
        auto r3 = make_polymorphic_options(fixed_deriver(0), 1, "e", "m", "p", 500000u,
                                           PassGrowthLimits{});
        ck(bool(r3), "V3: valid options accepted");
        // V4: validate_polymorphic_options on a good record → ok.
        auto good = make_opts(0, 500000);
        ck(bool(validate_polymorphic_options(good)), "V4: validate ok on good record");
        // V5: null seed_deriver is allowed by validate (no-op wrapper path).
        PolymorphicPassOptions nulld; nulld.seed_deriver = nullptr;
        nulld.site_probability_ppm = 0;
        ck(bool(validate_polymorphic_options(nulld)), "V5: null deriver + 0 ppm validates");
    }

    // ═══ (R) Configured factory registration ═══
    std::printf("\n--- (R) configured factory registration ---\n");
    EmberPassRegistry reg;
    register_passes(reg, make_opts(0, 500000));
    const char* kAllNames[] = {"subst","mba_expand","const_encode","opaque_pred",
                               "deadcode","str_encrypt","block_split"};
    for (const char* n : kAllNames) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "R1: configured reg.has(\"%s\")", n);
        ck(reg.has(n), buf);
    }
    // R3: two create() calls yield distinct instances.
    {
        auto a = reg.create("subst");
        auto b = reg.create("subst");
        ck(bool(a) && bool(b), "R3a: create subst yields two non-null concepts");
        ck(a.get() != b.get(), "R3b: the two concepts are distinct instances");
    }
    // R2: compat wrapper registers the same names.
    EmberPassRegistry reg2;
    register_passes(reg2);
    for (const char* n : kAllNames) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "R2: compat reg.has(\"%s\")", n);
        ck(reg2.has(n), buf);
    }

    // ═══ (A) no-op vs changed preservation ═══
    std::printf("\n--- (A) no-op vs changed preservation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        // no-op: zero density → all_preserved + instr count unchanged.
        EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
        auto base = candidate_fixture(pass);
        size_t before = total_instrs(base);
        SingleResult r = run_pass(nopreg, pass, base);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "A1 %s: zero-density -> no-op (all_preserved)", pass);
        ck(r.all_preserved, buf);
        std::snprintf(buf, sizeof(buf), "A2 %s: zero-density leaves instr count unchanged (%zu==%zu)", pass, r.instr_count, before);
        ck(r.instr_count == before, buf);
        // changed: full density → not all_preserved (the pass selects ≥1 site).
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto base2 = candidate_fixture(pass);
        SingleResult r2 = run_pass(fullreg, pass, base2);
        std::snprintf(buf, sizeof(buf), "A3 %s: full-density -> changed (not all_preserved)", pass);
        ck(!r2.all_preserved, buf);
    }

    // ═══ (Widths 1/2/4/8) structural no-op + changed eligibility ═══
    std::printf("\n--- (W) widths 1/2/4/8 eligibility ---\n");
    {
        Prim prims[] = {Prim::I8, Prim::I16, Prim::I32, Prim::I64};
        int32_t widths[] = {1, 2, 4, 8};
        for (size_t w = 0; w < 4; ++w) {
            // subst + mba_expand + const_encode operate on integer Add/Const.
            for (size_t pi = 0; pi < 3; ++pi) {
                const char* pass = kSixPasses[pi];
                EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
                auto f = build_straight_line(7, 9, prims[w], widths[w]);
                SingleResult r = run_pass(nopreg, pass, f);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "W1 %s w=%d: zero-density no-op", pass, widths[w]);
                ck(r.all_preserved, buf);
                EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
                auto f2 = build_straight_line(7, 9, prims[w], widths[w]);
                SingleResult r2 = run_pass(fullreg, pass, f2);
                std::snprintf(buf, sizeof(buf), "W2 %s w=%d: full-density validates (structure)", pass, widths[w]);
                std::string verr;
                bool ok = verify_thin_function_for_codegen(f2, &verr);
                ck(ok, buf);
                if (!ok) std::printf("    verr: %s\n", verr.c_str());
                (void)r2;
            }
        }
    }

    // ═══ (Empty / no-candidate) every pass is a no-op ═══
    std::printf("\n--- (E) empty / no-candidate functions ---\n");
    {
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        for (size_t i = 0; i < kSixCount; ++i) {
            const char* pass = kSixPasses[i];
            auto e = build_empty();
            SingleResult r = run_pass(fullreg, pass, e);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "E1 %s: empty -> no-op", pass);
            ck(r.all_preserved, buf);
            auto nc = build_no_candidate();
            SingleResult r2 = run_pass(fullreg, pass, nc);
            std::snprintf(buf, sizeof(buf), "E2 %s: no-candidate -> no-op", pass);
            ck(r2.all_preserved, buf);
        }
    }

    // ═══ (CFGs) straight-line / diamond / loop / long-block validate after each pass ═══
    std::printf("\n--- (C) representative CFGs validate after each pass ---\n");
    {
        struct Cfg { const char* name; ThinFunction (*build)(); };
        // loop builder takes an arg; wrap it.
        Cfg cfgs[] = {
            {"straight-line", []() { return build_straight_line(7, 9, Prim::I64, 8); }},
            {"diamond",       []() { return build_diamond(); }},
            {"loop",           []() { return build_loop(5); }},
            {"long-block",     []() { return build_long_block(9); }},
        };
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        for (auto& cfg : cfgs) {
            for (size_t i = 0; i < kSixCount; ++i) {
                const char* pass = kSixPasses[i];
                auto f = cfg.build();
                run_pass(fullreg, pass, f);
                std::string verr;
                bool ok = verify_thin_function_for_codegen(f, &verr);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "C %s/%s: validates after pass", cfg.name, pass);
                ck(ok, buf);
                if (!ok) std::printf("    verr: %s\n", verr.c_str());
            }
        }
    }

    // ═══ (B) same-seed serialized Thin IR equality ═══
    std::printf("\n--- (B) same-seed serialized Thin IR equality ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry r1; register_passes(r1, make_opts(42, 1'000'000));
        EmberPassRegistry r2; register_passes(r2, make_opts(42, 1'000'000));
        auto fa = candidate_fixture(pass);
        auto fb = candidate_fixture(pass);
        // Run in place so the serialized blobs reflect the transformed IR.
        EmberAnalysisManager am;
        auto p1 = r1.create(pass); if (p1) p1->run(fa, am);
        auto p2 = r2.create(pass); if (p2) p2->run(fb, am);
        std::vector<uint8_t> ba, bb; std::string ea, eb;
        bool sa = serialize_thin_function(fa, ba, &ea);
        bool sb = serialize_thin_function(fb, bb, &eb);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "B1 %s: both serialize ok", pass);
        ck(sa && sb, buf);
        std::snprintf(buf, sizeof(buf), "B2 %s: same-seed blobs byte-identical (size %zu==%zu)", pass, ba.size(), bb.size());
        ck(ba == bb, buf);
    }

    // ═══ (C2) two pinned seeds → structurally different (where ≥1 site) ═══
    // Two pinned seeds must produce structurally different serialized IR for
    // each pass that has ≥1 selected site. Uses a multi-site fixture
    // (build_two_long_blocks(9) — two long blocks, many Add/ConstInt sites) so
    // the variation is robust: the per-purpose streams (select / variant /
    // constant / truth / junk-count / split-pick) differ by seed across many
    // sites, so two different seeds produce different serialized blobs.
    //   - subst has NO variant stream (the MBA identity is unique), so it is
    //     run at 50% density (500'000 ppm) where the per-site SELECTION differs
    //     by seed (different seeds select different subsets of the many Adds).
    //   - the other five passes have variant/truth/constant/split-pick streams,
    //     so they vary by seed even at full density (1'000'000 ppm).
    std::printf("\n--- (C2) two pinned seeds structural variation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        const uint32_t dens = (std::strcmp(pass, "subst") == 0) ? 500'000u : 1'000'000u;
        EmberPassRegistry ra; register_passes(ra, make_opts(1, dens));
        EmberPassRegistry rb; register_passes(rb, make_opts(999, dens));
        auto fa = build_two_long_blocks(9);
        auto fb = build_two_long_blocks(9);
        // Run in place so the serialized blobs reflect the transformed IR.
        EmberAnalysisManager am;
        auto pca = ra.create(pass); if (pca) pca->run(fa, am);
        auto pcb = rb.create(pass); if (pcb) pcb->run(fb, am);
        std::vector<uint8_t> ba, bb; std::string ea, eb;
        serialize_thin_function(fa, ba, &ea);
        serialize_thin_function(fb, bb, &eb);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "C2 %s: two seeds -> structural variation (blobs differ)", pass);
        ck(ba != bb, buf);
    }

    // ═══ (D) baseline differential execution ═══
    std::printf("\n--- (D) baseline differential execution ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        auto base = candidate_fixture(pass);
        int64_t rb = emit_and_call(base);
        ck(rb != INT64_MIN, "D: baseline emit/call succeeded (precondition)");
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        run_pass(fullreg, pass, f);
        std::string verr;
        ck(verify_thin_function_for_codegen(f, &verr), "D: transformed IR verifies for codegen");
        int64_t rt = emit_and_call(f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D %s: transformed return == baseline (%lld==%lld)",
                      pass, (long long)rt, (long long)rb);
        ck(rt == rb, buf);
    }

    // ═══ (E) validate → serialize → deserialize → validate ═══
    std::printf("\n--- (E) serialize/deserialize round-trip ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        run_pass(fullreg, pass, f);
        std::string v1;
        ck(verify_thin_function_for_codegen(f, &v1), "E1: validate transformed");
        std::vector<uint8_t> blob; std::string serr;
        ck(serialize_thin_function(f, blob, &serr), "E2: serialize transformed");
        ThinFunction g; std::string derr;
        const uint8_t* cur = blob.data(); const uint8_t* end = blob.data() + blob.size();
        ck(deserialize_thin_function(cur, end, f.name, f.slot, g, &derr), "E3: deserialize");
        std::string v2;
        ck(validate_thin_function(g, &v2), "E4: validate deserialized");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "E5 %s: round-trip preserves block count", pass);
        ck(g.blocks.size() == f.blocks.size(), buf);
        std::snprintf(buf, sizeof(buf), "E6 %s: round-trip preserves total instr count", pass);
        ck(total_instrs(g) == total_instrs(f), buf);
    }

    // ═══ (F) stale-regalloc invalidation ═══
    std::printf("\n--- (F) stale-regalloc invalidation ---\n");
    for (size_t i = 0; i < kSixCount; ++i) {
        const char* pass = kSixPasses[i];
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f = candidate_fixture(pass);
        // Seed a stale regalloc.
        f.ra.enabled = true; f.ra.num_regs = 4; f.ra.map[1] = {true, 3, -16};
        // Run the pass IN PLACE (run_pass copies f; the ra mutation must be
        // observed on this f, so drive the concept directly here).
        EmberAnalysisManager am;
        auto pc = fullreg.create(pass);
        ck(bool(pc), "F: create pass concept");
        EmberPreserved pres = pc ? pc->run(f, am) : EmberPreserved::all();
        char buf[128];
        if (pres.all_preserved()) {
            // No site was selected for this seed/fixture: regalloc stays as we
            // seeded it (the pass did not touch the function). This is valid.
            std::snprintf(buf, sizeof(buf), "F %s: no-op -> regalloc untouched (valid)", pass);
            ck(f.ra.enabled, buf);
        } else {
            std::snprintf(buf, sizeof(buf), "F %s: changed -> stale regalloc cleared (enabled=false)", pass);
            ck(!f.ra.enabled, buf);
            std::snprintf(buf, sizeof(buf), "F %s: changed -> stale regalloc map empty", pass);
            ck(f.ra.map.empty(), buf);
        }
    }

    // ═══ (G) exact boundaries + stop-before-site atomicity ═══
    std::printf("\n--- (G) growth boundaries + stop-before-site atomicity ---\n");
    {
        // G1: max_sites = 1 on a fixture with ≥2 eligible sites for subst
        //     (long_block has many Adds). At most 1 site is rewritten; the
        //     result is valid (atomic — no partial site).
        EmberPassRegistry r; PassGrowthLimits lim; lim.max_sites = 1;
        register_passes(r, make_opts(0, 1'000'000, lim));
        auto f = build_long_block(9);  // many Add sites
        size_t before = total_instrs(f);
        SingleResult res = run_pass(r, "subst", f);
        ck(!res.all_preserved, "G1a: subst with max_sites=1 still changes (>=1 site)");
        // Each Add site adds 3 instructions; with max_sites=1 the growth is
        // exactly +3 (atomic: no partial site).
        char buf[128];
        std::snprintf(buf, sizeof(buf), "G1b: max_sites=1 -> exactly +3 instrs (atomic, before=%zu after=%zu)", before, res.instr_count);
        ck(res.instr_count == before + 3, buf);
        std::string verr;
        ck(verify_thin_function_for_codegen(f, &verr), "G1c: bounded result validates");

        // G2: max_added_instructions so tight that even one site's 3 instrs
        //     would exceed it. The pass must stop BEFORE the first site:
        //     no-op, atomic (zero partial growth).
        EmberPassRegistry r2; PassGrowthLimits lim2; lim2.max_added_instructions = 2;
        register_passes(r2, make_opts(0, 1'000'000, lim2));
        auto f2 = build_straight_line(7, 9, Prim::I64, 8);
        size_t before2 = total_instrs(f2);
        SingleResult res2 = run_pass(r2, "subst", f2);
        ck(res2.all_preserved, "G2a: subst with max_added_instructions=2 -> no-op (stop-before-site)");
        std::snprintf(buf, sizeof(buf), "G2b: stop-before-site atomic (before=%zu after=%zu)", before2, res2.instr_count);
        ck(res2.instr_count == before2, buf);

        // G3: growth ratio boundary. growth_numerator=1, denominator=1 → the
        //     final count may equal but not exceed the initial count. subst on
        //     a single-Add fixture wants +3 (ratio > 1), so it must stop before
        //     the site: no-op.
        EmberPassRegistry r3; PassGrowthLimits lim3;
        lim3.growth_numerator = 1; lim3.growth_denominator = 1;  // ratio 1.0
        register_passes(r3, make_opts(0, 1'000'000, lim3));
        auto f3 = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult res3 = run_pass(r3, "subst", f3);
        ck(res3.all_preserved, "G3a: growth ratio 1.0 -> subst no-op (would exceed)");

        // G4: growth ratio exactly 2.0 admits the single-Add +3 (initial=3 →
        //     cap=6; +3 → 6 <= 6). Changed + atomic.
        EmberPassRegistry r4; PassGrowthLimits lim4;
        lim4.growth_numerator = 2; lim4.growth_denominator = 1;  // ratio 2.0
        register_passes(r4, make_opts(0, 1'000'000, lim4));
        auto f4 = build_straight_line(7, 9, Prim::I64, 8);
        size_t before4 = total_instrs(f4);
        SingleResult res4 = run_pass(r4, "subst", f4);
        ck(!res4.all_preserved, "G4a: growth ratio 2.0 -> subst changed");
        std::snprintf(buf, sizeof(buf), "G4b: ratio 2.0 -> exactly +3 (before=%zu after=%zu)", before4, res4.instr_count);
        ck(res4.instr_count == before4 + 3, buf);

        // G5: max_added_blocks = 0 forbids any block split. block_split on a
        //     long block must stop before the site: no-op, atomic.
        EmberPassRegistry r5; PassGrowthLimits lim5; lim5.max_added_blocks = 0;
        register_passes(r5, make_opts(0, 1'000'000, lim5));
        auto f5 = build_long_block(9);
        size_t before5 = f5.blocks.size();
        EmberAnalysisManager am5; auto pc5 = r5.create("block_split");
        EmberPreserved pres5 = pc5->run(f5, am5);
        ck(pres5.all_preserved(), "G5a: max_added_blocks=0 -> block_split no-op");
        ck(f5.blocks.size() == before5, "G5b: block count unchanged (atomic)");

        // G6: max_added_blocks = 1 admits exactly one split on a long block.
        EmberPassRegistry r6; PassGrowthLimits lim6; lim6.max_added_blocks = 1;
        register_passes(r6, make_opts(0, 1'000'000, lim6));
        auto f6 = build_long_block(9);
        size_t before6 = f6.blocks.size();
        EmberAnalysisManager am6; auto pc6 = r6.create("block_split");
        EmberPreserved pres6 = pc6->run(f6, am6);
        ck(!pres6.all_preserved(), "G6a: max_added_blocks=1 -> block_split changed");
        std::snprintf(buf, sizeof(buf), "G6b: exactly +1 block (before=%zu after=%zu)", before6, f6.blocks.size());
        ck(f6.blocks.size() == before6 + 1, buf);
    }

    // ═══ (str_encrypt) configured factory + no-op scaffolding ═══
    std::printf("\n--- (S) str_encrypt configured factory + no-op scaffolding ---\n");
    {
        // S1: registered via configured factory (already checked in R1, but
        //     restate explicitly for the deferred pass).
        ck(reg.has("str_encrypt"), "S1: str_encrypt registered via configured factory");
        // S2: zero density -> no-op.
        EmberPassRegistry nopreg; register_passes(nopreg, make_opts(0, 0));
        // A function with no ConstStringRef -> no-op regardless of density.
        auto f = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult r = run_pass(nopreg, "str_encrypt", f);
        ck(r.all_preserved, "S2: str_encrypt no ConstStringRef -> no-op");
        // S3: full density on a no-stringref function -> still no-op.
        EmberPassRegistry fullreg; register_passes(fullreg, make_opts(0, 1'000'000));
        auto f2 = build_straight_line(7, 9, Prim::I64, 8);
        SingleResult r2 = run_pass(fullreg, "str_encrypt", f2);
        ck(r2.all_preserved, "S3: str_encrypt no candidates -> no-op (full density)");
    }

    // ═══ Summary ═══
    std::printf("\n=== polymorphic_pass_test: %s ===\n",
                g_fail ? "FAIL" : "PASS");
    return g_fail;
}
