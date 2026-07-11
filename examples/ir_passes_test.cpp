// ir_passes_test.cpp — Stage C: the IR optimization passes value-preservation +
// instr-count-reduction gate.
//
// Verifies each of the three IR optimization passes (ConstPropPass "constprop",
// DeadCodeElimPass "dce", CSEPass "cse") is:
//   (a) value-preserving — the IR after the pass, when emitted via emit_x64,
//       produces the same i64 return as before the pass.
//   (b) instr-count-reducing on its target workload.
//
// The compile helper does the IR path MANUALLY (lower_function → [pass] →
// emit_x64) so a pass can run between lower and emit — unlike thin_ir_test
// which uses compile_func with enable_ir_backend (which does lower+emit
// internally with no pass in between).
//
// Modeled on thin_ir_test.cpp (the M struct, ck() macro, call0_i64 pattern).

#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, build_struct_layouts
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"
#include "../src/thin_ir.hpp"         // ThinFunction, dump()
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/thin_emit.hpp"       // emit_x64
#include "../src/ember_pass.hpp"      // EmberPassManager, EmberAnalysisManager
#include "../src/ember_pass_registry.hpp" // EmberPassRegistry
#include "../extensions/opt/ext_opt.hpp"  // register_passes (opt)
#include "../extensions/obf/ext_obf.hpp"   // register_passes (obf)

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

// ─── The four IR-pass workload sources (%N = 100 for a fast deterministic test) ───

static const char* SRC_CONSTPROP_FOLD =
    "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < 100) { let b: i64 = 3; let c: i64 = b + 4; s = s + c; i = i + 1; } return s; }\n";
static const char* SRC_DCE_DEAD_STORE =
    "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < 100) { let dead: i64 = i * 13; s = s + i; i = i + 1; } return s; }\n";
static const char* SRC_CSE_REDUNDANT =
    "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < 100) { let a: i64 = i * 7; s = s + a + a; i = i + 1; } return s; }\n";
static const char* SRC_LICM_INVARIANT =
    "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < 100) { let k: i64 = 100 * 200; s = s + k + i; i = i + 1; } return s; }\n";
// LLVM-4: InstCombine workload — identity folds (x+0, x*1, x*0, x-x).
// The foldable ops become Moves/ConstInts; with dce they get removed.
static const char* SRC_INSTCOMBINE_FOLD =
    "fn main() -> i64 { let x: i64 = 7; let a: i64 = x + 0; let b: i64 = x * 1; let c: i64 = x * 0; let d: i64 = x - x; return a + b + d; }\n";
// LLVM-5: DSE workload — two stores to the same slot, no intervening load
// (the first store is overwritten before read -> dead).
static const char* SRC_DSE_OVERWRITE =
    "fn main() -> i64 { let mut x: i64 = 5; x = 9; return x; }\n";

// ─── Test infrastructure (modeled on thin_ir_test.cpp) ───

struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    StructLayoutTable layouts;
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile with an optional pass manager (runs between lower_function and
// emit_x64). Returns the module + the main fn's instr count before/after the
// pass. The workloads use NO natives/globals/structs — just i64 arithmetic +
// while + return — so the CodeGenCtx is minimal.
static std::unique_ptr<M> compile_with(const std::string& src,
                                       EmberPassManager* pm,
                                       size_t* instr_after = nullptr,
                                       size_t* instr_before = nullptr) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<ir_passes>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }

    // Empty natives — the workloads have no native calls.
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string encryption
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema: %s\n", sr.errors.empty() ? "<unknown>" : sr.errors[0].msg.c_str());
        return nullptr;
    }

    // No globals — empty globals block.
    m->gbs.assign(0, 0);
    m->gb.base = 0;
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());

    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    ctx.use_context_reg = true;
    ctx.enable_ir_backend = false;  // we do lower+emit manually (not via compile_func)

    EmberAnalysisManager am;
    for (auto& fn : m->prog.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) {
            std::printf("FAIL: lower_function gave empty blocks for %s\n", fn.name.c_str());
            return nullptr;
        }
        // Count instrs before the pass (only for main).
        if (fn.name == "main" && instr_before) {
            *instr_before = 0;
            for (const auto& blk : thf.blocks) *instr_before += blk.instrs.size();
        }
        // Run the pass (if any).
        if (pm) pm->run(thf, am);
        // Count instrs after the pass (only for main).
        if (fn.name == "main" && instr_after) {
            *instr_after = 0;
            for (const auto& blk : thf.blocks) *instr_after += blk.instrs.size();
        }
        // Emit + finalize.
        CompiledFn cf = emit_x64(thf, ctx);
        if (cf.bytes.empty()) {
            std::printf("FAIL: emit_x64 gave empty bytes for %s\n", fn.name.c_str());
            return nullptr;
        }
        if (!finalize(cf)) {
            std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str());
            return nullptr;
        }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

// Call a no-arg script fn returning i64 (the workloads: main()).
static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

// ─── D6/D7 helpers (hand-built ThinFunction pass tests) ───
// Count total instrs across all blocks.
static size_t total_instrs(const ThinFunction& f) {
    size_t n = 0;
    for (const auto& blk : f.blocks) n += blk.instrs.size();
    return n;
}

// Run a single named pass on a COPY of f (the original is untouched).
// Returns {all_preserved, instr_count_after}.
struct SinglePassResult { bool all_preserved; size_t instr_count; };
static SinglePassResult run_single_pass(const EmberPassRegistry& reg,
                                        const char* name, ThinFunction f) {
    EmberAnalysisManager am;
    auto pc = reg.create(name);
    if (!pc) return {false, 0};  // pass not found
    EmberPreserved pres = pc->run(f, am);
    return {pres.all_preserved(), total_instrs(f)};
}

int main() {
    std::printf("=== ir_passes_test: Stage C IR optimization passes ===\n");

    // (1) Registry: register_passes provides all passes by name.
    std::printf("(1) Registry\n");
    EmberPassRegistry reg;
    ext_opt::register_passes(reg);
    ext_obf::register_passes(reg);
    ck(reg.has("constprop"), "registry has \"constprop\"");
    ck(reg.has("dce"), "registry has \"dce\"");
    ck(reg.has("cse"), "registry has \"cse\"");
    ck(reg.has("licm"), "registry has \"licm\"");
    ck(reg.has("subst"), "registry has \"subst\" (obf)");
    ck(reg.has("instcombine"), "registry has \"instcombine\"");
    ck(reg.has("dse"), "registry has \"dse\"");

    // The four workloads.
    struct Workload { const char* name; const char* src; };
    Workload workloads[] = {
        {"constprop_fold", SRC_CONSTPROP_FOLD},
        {"dce_dead_store", SRC_DCE_DEAD_STORE},
        {"cse_redundant",  SRC_CSE_REDUNDANT},
        {"licm_invariant", SRC_LICM_INVARIANT},
        {"instcombine_fold", SRC_INSTCOMBINE_FOLD},
        {"dse_overwrite", SRC_DSE_OVERWRITE},
    };
    const int NW = 6;

    // For each pass: (a) value-preserving on all 4 workloads, (b) instr-count
    // reduction on its target workload (except LICM which moves, not removes).
    struct PassTest { const char* name; const char* target; bool check_instr_reduction; };
    PassTest passes[] = {
        {"constprop", "constprop_fold", true},
        {"dce",       "dce_dead_store", true},
        {"cse",       "cse_redundant",  true},
        {"licm",      "licm_invariant", false},  // LICM hoists, doesn't remove
        {"instcombine", "instcombine_fold", false},  // folds to Move/ConstInt (same count; DCE removes)
        {"dse",         "dse_overwrite",    true},
    };

    for (const auto& pt : passes) {
        std::printf("\n--- pass: %s (target: %s) ---\n", pt.name, pt.target);

        // (a) Value-preserving on ALL four workloads.
        for (int wi = 0; wi < NW; ++wi) {
            const auto& wl = workloads[wi];
            // Baseline: no pass.
            auto mb = compile_with(wl.src, nullptr);
            if (!mb) { ck(false, "baseline compile failed"); continue; }
            int64_t rb = call0_i64(*mb, "main");
            // With pass: build a manager, add the pass via the registry.
            EmberPassManager pm;
            pm.add_pass_concept(reg.create(pt.name));
            auto mp = compile_with(wl.src, &pm);
            if (!mp) { ck(false, "pass compile failed"); continue; }
            int64_t rp = call0_i64(*mp, "main");
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s value-preserving on %s (baseline=%lld pass=%lld)",
                          pt.name, wl.name, (long long)rb, (long long)rp);
            ck(rb == rp, msg);
        }

        // (b) Instr-count reduction on the target workload (skip for LICM —
        // it hoists instructions, doesn't remove them; the benefit is runtime,
        // not code size. Just check value-preservation, already done in (a)).
        if (pt.check_instr_reduction) {
            size_t before = 0, after = 0;
            const char* target_src = nullptr;
            for (int wi = 0; wi < NW; ++wi)
                if (std::string(workloads[wi].name) == pt.target) { target_src = workloads[wi].src; break; }
            EmberPassManager pm;
            pm.add_pass_concept(reg.create(pt.name));
            auto mt = compile_with(target_src, &pm, &after, &before);
            if (mt) {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s reduces instr count on %s (before=%zu after=%zu)",
                              pt.name, pt.target, before, after);
                ck(after < before, msg);
            } else {
                ck(false, "target compile failed");
            }
        }
    }

    // (3) All passes on licm_invariant (no target pass — just no-crash +
    // value-preserving, already covered by (a) for each pass).
    std::printf("\n--- licm_invariant (no target pass; all value-preserving) ---\n");
    {
        auto mb = compile_with(SRC_LICM_INVARIANT, nullptr);
        int64_t rb = mb ? call0_i64(*mb, "main") : -1;
        for (const auto& pt : passes) {
            EmberPassManager pm;
            pm.add_pass_concept(reg.create(pt.name));
            auto mp = compile_with(SRC_LICM_INVARIANT, &pm);
            int64_t rp = mp ? call0_i64(*mp, "main") : -1;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s value-preserving on licm_invariant (b=%lld p=%lld)",
                          pt.name, (long long)rb, (long long)rp);
            ck(rb == rp, msg);
        }
    }

    // (4) SubstitutionPass (obfuscation): value-preserving + instr count INCREASES.
    std::printf("\n--- subst (obfuscation: MBA substitution) ---\n");
    {
        // Value-preserving on all 4 workloads.
        for (int wi = 0; wi < NW; ++wi) {
            const auto& wl = workloads[wi];
            auto mb = compile_with(wl.src, nullptr);
            if (!mb) { ck(false, "subst baseline compile failed"); continue; }
            int64_t rb = call0_i64(*mb, "main");
            EmberPassManager pm;
            pm.add_pass_concept(reg.create("subst"));
            auto mp = compile_with(wl.src, &pm);
            if (!mp) { ck(false, "subst pass compile failed"); continue; }
            int64_t rp = call0_i64(*mp, "main");
            char msg[128];
            std::snprintf(msg, sizeof(msg), "subst value-preserving on %s (b=%lld p=%lld)",
                          wl.name, (long long)rb, (long long)rp);
            ck(rb == rp, msg);
        }
        // Instr count INCREASES on a workload with Add (cse_redundant has i*7 + a+a).
        size_t before = 0, after = 0;
        EmberPassManager pm;
        pm.add_pass_concept(reg.create("subst"));
        auto mt = compile_with(SRC_CSE_REDUNDANT, &pm, &after, &before);
        if (mt) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "subst INCREASES instr count on cse_redundant (before=%zu after=%zu)",
                          before, after);
            ck(after > before, msg);
        } else {
            ck(false, "subst target compile failed");
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // D6: No-op pass paths — passes on functions with no optimizable patterns.
    // Each pass must return Preserved::all() and leave the IR unchanged.
    // ═══════════════════════════════════════════════════════════════════════
    std::printf("\n--- D6: no-op pass paths ---\n");

    // D6a: trivial function fn main()->i64 { return 7; }
    //   block0: ConstInt dst=v0 imm=7, Return ret=v0.
    // No optimizable pattern: constprop has nothing to fold (no binop), dce has
    // no dead defs (v0 is used by Return), cse has no redundancy, licm has no
    // loop (<3 blocks), forward has no store-load pair, copyprop has no Move.
    {
        ThinFunction tf;
        tf.name = "trivial";
        ThinBlock b0;
        b0.id = 0;
        ThinInstr ci;
        ci.op = ThinOp::ConstInt;
        ci.dst = 1;            // v0
        ci.imm.i = 7;
        ci.meta.width = 8;
        b0.instrs.push_back(std::move(ci));
        b0.term.kind = TermKind::Return;
        b0.term.ret = 1;       // return v0
        tf.blocks.push_back(std::move(b0));

        size_t orig = total_instrs(tf);
        ck(orig == 1, "D6 trivial: original instr count is 1");

        const char* pnames[] = {"constprop", "dce", "cse", "licm", "forward", "copyprop"};
        for (const char* pn : pnames) {
            auto r = run_single_pass(reg, pn, tf);
            char msg[160];
            std::snprintf(msg, sizeof(msg), "D6 trivial: %s returns Preserved::all()", pn);
            ck(r.all_preserved, msg);
            std::snprintf(msg, sizeof(msg), "D6 trivial: %s instr count unchanged (%zu->%zu)",
                          pn, orig, r.instr_count);
            ck(r.instr_count == orig, msg);
        }
    }

    // D6b: empty-body function (no instrs, void return).
    //   block0: Return ret=0 (void).
    // Every pass should return Preserved::all() with zero instrs.
    {
        ThinFunction tf;
        tf.name = "empty";
        ThinBlock b0;
        b0.id = 0;
        b0.term.kind = TermKind::Return;
        b0.term.ret = 0;       // void return
        tf.blocks.push_back(std::move(b0));

        size_t orig = total_instrs(tf);
        ck(orig == 0, "D6 empty: original instr count is 0");

        const char* pnames[] = {"constprop", "dce", "cse", "licm", "forward", "copyprop"};
        for (const char* pn : pnames) {
            auto r = run_single_pass(reg, pn, tf);
            char msg[160];
            std::snprintf(msg, sizeof(msg), "D6 empty: %s returns Preserved::all()", pn);
            ck(r.all_preserved, msg);
            std::snprintf(msg, sizeof(msg), "D6 empty: %s instr count unchanged (%zu->%zu)",
                          pn, orig, r.instr_count);
            ck(r.instr_count == orig, msg);
        }
    }

    // D6c: function with side-effecting instrs (CallNative + StoreFrame).
    //   block0:
    //     v0 = ConstInt 42
    //     v1 = CallNative "side_effect" args=[v0]   (side-effecting; v1 unused)
    //     StoreFrame src1=v0 off=-8                 (store to slot -8)
    //     v2 = LoadFrame off=-8                     (read back — keeps store alive)
    //     Return v2
    // DCE must NOT remove the CallNative (side-effecting) or the StoreFrame
    // (slot -8 is read by LoadFrame → not a dead store). CSE must NOT coalesce
    // them (CallNative is not pure; StoreFrame is special-cased).
    {
        ThinFunction tf;
        tf.name = "sideeffect";
        ThinBlock b0;
        b0.id = 0;

        ThinInstr ci;
        ci.op = ThinOp::ConstInt;
        ci.dst = 1;            // v0
        ci.imm.i = 42;
        ci.meta.width = 8;
        b0.instrs.push_back(std::move(ci));

        ThinInstr call;
        call.op = ThinOp::CallNative;
        call.dst = 2;          // v1 (unused — tests unused-dst side-effecting calls survive)
        call.args = {1};       // arg = v0
        call.arg_frame_offs = {-1};
        call.meta.native_name = "side_effect";
        b0.instrs.push_back(std::move(call));

        ThinInstr st;
        st.op = ThinOp::StoreFrame;
        st.src1 = 1;           // v0
        st.meta.frame_off = -8;
        st.meta.width = 8;
        b0.instrs.push_back(std::move(st));

        ThinInstr ld;
        ld.op = ThinOp::LoadFrame;
        ld.dst = 3;            // v2
        ld.meta.frame_off = -8;
        ld.meta.width = 8;
        b0.instrs.push_back(std::move(ld));

        b0.term.kind = TermKind::Return;
        b0.term.ret = 3;       // return v2
        tf.blocks.push_back(std::move(b0));

        size_t orig = total_instrs(tf);
        ck(orig == 4, "D6 sideeffect: original instr count is 4");

        // DCE must not remove the CallNative or the non-dead StoreFrame.
        {
            ThinFunction copy = tf;
            EmberAnalysisManager am;
            auto pc = reg.create("dce");
            EmberPreserved pres = pc->run(copy, am);
            ck(pres.all_preserved(), "D6 sideeffect: dce returns Preserved::all()");
            char msg[160];
            std::snprintf(msg, sizeof(msg), "D6 sideeffect: dce instr count unchanged (%zu->%zu)",
                          orig, total_instrs(copy));
            ck(total_instrs(copy) == orig, msg);
            bool has_call = false, has_store = false;
            for (const auto& blk : copy.blocks)
                for (const auto& in : blk.instrs) {
                    if (in.op == ThinOp::CallNative) has_call = true;
                    if (in.op == ThinOp::StoreFrame) has_store = true;
                }
            ck(has_call, "D6 sideeffect: dce does not remove CallNative");
            ck(has_store, "D6 sideeffect: dce does not remove StoreFrame (slot is read)");
        }
        // CSE must not coalesce the CallNative or the StoreFrame.
        {
            ThinFunction copy = tf;
            EmberAnalysisManager am;
            auto pc = reg.create("cse");
            EmberPreserved pres = pc->run(copy, am);
            ck(pres.all_preserved(), "D6 sideeffect: cse returns Preserved::all()");
            char msg[160];
            std::snprintf(msg, sizeof(msg), "D6 sideeffect: cse instr count unchanged (%zu->%zu)",
                          orig, total_instrs(copy));
            ck(total_instrs(copy) == orig, msg);
            bool has_call = false, has_store = false;
            for (const auto& blk : copy.blocks)
                for (const auto& in : blk.instrs) {
                    if (in.op == ThinOp::CallNative) has_call = true;
                    if (in.op == ThinOp::StoreFrame) has_store = true;
                }
            ck(has_call, "D6 sideeffect: cse does not coalesce CallNative");
            ck(has_store, "D6 sideeffect: cse does not coalesce StoreFrame");
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // D7: LICM hoist verification — assert the invariant actually moves from
    // the loop body to the pre-header, not just value-preservation.
    // ═══════════════════════════════════════════════════════════════════════
    std::printf("\n--- D7: LICM hoist verification ---\n");

    // D7a: loop with an invariant Mul — LICM must hoist it from the loop body
    // to the pre-header.
    //
    //   block0 (pre-header): ConstInt v0=100, ConstInt v1=200, Jmp -> block1
    //   block1 (header):     ConstBool v_cond=1, CondBranch ? block2 : block3
    //   block2 (latch/body): Mul v2=v0*v1  (INVARIANT), Jmp -> block1 (back-edge)
    //   block3 (exit):       Return v2
    //
    // The invariant (Mul v2=v0*v1) starts in block2 (loop body, NOT the header
    // — LICM skips the header for hoisting). After LICM it must be in block0
    // (pre-header) and absent from block2.
    {
        ThinFunction tf;
        tf.name = "licm_loop";

        const VReg v0 = 1, v1 = 2, v2 = 3, v_cond = 4;

        // block0: pre-header
        ThinBlock b0;
        b0.id = 0;
        {
            ThinInstr c0;
            c0.op = ThinOp::ConstInt; c0.dst = v0; c0.imm.i = 100; c0.meta.width = 8;
            b0.instrs.push_back(std::move(c0));
        }
        {
            ThinInstr c1;
            c1.op = ThinOp::ConstInt; c1.dst = v1; c1.imm.i = 200; c1.meta.width = 8;
            b0.instrs.push_back(std::move(c1));
        }
        b0.term.kind = TermKind::Jmp;
        b0.term.target = 1;
        tf.blocks.push_back(std::move(b0));

        // block1: header (CondBranch to block2 or block3)
        ThinBlock b1;
        b1.id = 1;
        {
            ThinInstr cb;
            cb.op = ThinOp::ConstBool; cb.dst = v_cond; cb.imm.i = 1;
            b1.instrs.push_back(std::move(cb));
        }
        b1.term.kind = TermKind::Branch;
        b1.term.cond = v_cond;
        b1.term.target = 2;       // true  -> block2 (loop body)
        b1.term.false_target = 3; // false -> block3 (exit)
        tf.blocks.push_back(std::move(b1));

        // block2: latch/body (Mul v2=v0*v1 + back-edge)
        ThinBlock b2;
        b2.id = 2;
        {
            ThinInstr mul;
            mul.op = ThinOp::Mul; mul.dst = v2; mul.src1 = v0; mul.src2 = v1; mul.meta.width = 8;
            b2.instrs.push_back(std::move(mul));
        }
        b2.term.kind = TermKind::Jmp;
        b2.term.target = 1;       // back-edge -> block1
        tf.blocks.push_back(std::move(b2));

        // block3: exit (Return v2)
        ThinBlock b3;
        b3.id = 3;
        b3.term.kind = TermKind::Return;
        b3.term.ret = v2;
        tf.blocks.push_back(std::move(b3));

        // Helper: does a block contain a Mul with the given src vregs?
        auto block_has_mul = [](const ThinBlock& blk, VReg s1, VReg s2) -> bool {
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::Mul && in.src1 == s1 && in.src2 == s2)
                    return true;
            return false;
        };

        // Before LICM: Mul is in block2 (loop body), NOT in block0 (pre-header).
        ck(!block_has_mul(tf.blocks[0], v0, v1), "D7a before: Mul NOT in pre-header (block0)");
        ck(block_has_mul(tf.blocks[2], v0, v1), "D7a before: Mul IS in loop body (block2)");

        // Run LICM.
        EmberAnalysisManager am;
        auto pc = reg.create("licm");
        EmberPreserved pres = pc->run(tf, am);
        ck(!pres.all_preserved(), "D7a: licm returns Preserved::none() (changed)");

        // After LICM: Mul is in block0 (pre-header), NOT in block2 (loop body).
        ck(block_has_mul(tf.blocks[0], v0, v1), "D7a after: Mul IS in pre-header (block0)");
        ck(!block_has_mul(tf.blocks[2], v0, v1), "D7a after: Mul NOT in loop body (block2)");

        // The hoisted Mul preserves dst + operands + width (value-preservation
        // at the IR-structure level — the instruction is identical, just moved).
        bool found_hoisted = false;
        for (const auto& in : tf.blocks[0].instrs) {
            if (in.op == ThinOp::Mul && in.src1 == v0 && in.src2 == v1) {
                found_hoisted = true;
                ck(in.dst == v2, "D7a: hoisted Mul preserves dst vreg");
                ck(in.meta.width == 8, "D7a: hoisted Mul preserves width");
            }
        }
        ck(found_hoisted, "D7a: hoisted Mul found in pre-header with correct operands");

        // block2 (loop body) is now empty — its only instr was hoisted.
        ck(tf.blocks[2].instrs.empty(), "D7a: loop body (block2) empty after hoist");

        // The header's ConstBool was NOT hoisted (LICM skips the header).
        bool header_has_constbool = false;
        for (const auto& in : tf.blocks[1].instrs)
            if (in.op == ThinOp::ConstBool) { header_has_constbool = true; break; }
        ck(header_has_constbool, "D7a: header ConstBool NOT hoisted (LICM skips header)");

        // The pre-header's ConstInts are still there (LICM doesn't move them).
        bool prehdr_has_100 = false, prehdr_has_200 = false;
        for (const auto& in : tf.blocks[0].instrs) {
            if (in.op == ThinOp::ConstInt && in.imm.i == 100) prehdr_has_100 = true;
            if (in.op == ThinOp::ConstInt && in.imm.i == 200) prehdr_has_200 = true;
        }
        ck(prehdr_has_100, "D7a: pre-header ConstInt 100 still present");
        ck(prehdr_has_200, "D7a: pre-header ConstInt 200 still present");
    }

    // D7b: loop with NO invariant instrs — LICM must return Preserved::all()
    // and hoist nothing.
    //
    //   block0 (pre-header): ConstInt v0=1, Jmp -> block1
    //   block1 (header):     ConstBool v_cond=1, CondBranch ? block2 : block3
    //   block2 (latch/body): StoreFrame src1=v0 off=-8, LoadFrame dst=v1 off=-8,
    //                        Jmp -> block1 (back-edge)
    //   block3 (exit):       Return v1
    //
    // The LoadFrame reads a slot written inside the loop (StoreFrame off=-8) →
    // NOT invariant. StoreFrame is never hoisted. So nothing is hoistable.
    {
        ThinFunction tf;
        tf.name = "licm_no_invariant";

        const VReg v0 = 1, v_cond = 2, v1 = 3;

        // block0: pre-header
        ThinBlock b0;
        b0.id = 0;
        {
            ThinInstr c0;
            c0.op = ThinOp::ConstInt; c0.dst = v0; c0.imm.i = 1; c0.meta.width = 8;
            b0.instrs.push_back(std::move(c0));
        }
        b0.term.kind = TermKind::Jmp;
        b0.term.target = 1;
        tf.blocks.push_back(std::move(b0));

        // block1: header
        ThinBlock b1;
        b1.id = 1;
        {
            ThinInstr cb;
            cb.op = ThinOp::ConstBool; cb.dst = v_cond; cb.imm.i = 1;
            b1.instrs.push_back(std::move(cb));
        }
        b1.term.kind = TermKind::Branch;
        b1.term.cond = v_cond;
        b1.term.target = 2;
        b1.term.false_target = 3;
        tf.blocks.push_back(std::move(b1));

        // block2: latch/body (StoreFrame + LoadFrame — neither is invariant)
        ThinBlock b2;
        b2.id = 2;
        {
            ThinInstr st;
            st.op = ThinOp::StoreFrame; st.src1 = v0; st.meta.frame_off = -8; st.meta.width = 8;
            b2.instrs.push_back(std::move(st));
        }
        {
            ThinInstr ld;
            ld.op = ThinOp::LoadFrame; ld.dst = v1; ld.meta.frame_off = -8; ld.meta.width = 8;
            b2.instrs.push_back(std::move(ld));
        }
        b2.term.kind = TermKind::Jmp;
        b2.term.target = 1;   // back-edge
        tf.blocks.push_back(std::move(b2));

        // block3: exit
        ThinBlock b3;
        b3.id = 3;
        b3.term.kind = TermKind::Return;
        b3.term.ret = v1;
        tf.blocks.push_back(std::move(b3));

        size_t orig = total_instrs(tf);

        EmberAnalysisManager am;
        auto pc = reg.create("licm");
        EmberPreserved pres = pc->run(tf, am);
        ck(pres.all_preserved(), "D7b: licm returns Preserved::all() (no invariant to hoist)");
        ck(total_instrs(tf) == orig, "D7b: licm instr count unchanged (nothing hoisted)");

        // block2 still has both instrs (StoreFrame + LoadFrame).
        ck(tf.blocks[2].instrs.size() == 2, "D7b: loop body still has 2 instrs (nothing hoisted)");
    }

    // -----------------------------------------------------------------
    // D8 (LLVM-4): InstCombine identity-fold hand-built tests.
    // Build a ThinFunction with BinOps that have a constant operand and
    // verify instcombine folds them to Move/ConstInt (instr-count drops or
    // BinOp count drops).
    // -----------------------------------------------------------------
    std::printf("\n--- D8: InstCombine identity folds ---\n");
    {
        // block0: ConstInt v0=7, ConstInt v1=0, ConstInt v2=1,
        //         BinOp Add  v3=v0+v1  (x+0 -> Move v3=v0)
        //         BinOp Mul  v4=v0*v2  (x*1 -> Move v4=v0)
        //         BinOp Mul  v5=v0*v1  (x*0 -> ConstInt v5=0)
        //         BinOp Sub  v6=v0-v0  (x-x -> ConstInt v6=0)
        //         Return v3
        ThinFunction tf;
        tf.name = "instcombine_test";
        const VReg v0=1, v1=2, v2=3, v3=4, v4=5, v5=6, v6=7;
        ThinBlock b0; b0.id = 0;
        auto ci = [&](VReg dst, int64_t imm) -> ThinInstr {
            ThinInstr x; x.op = ThinOp::ConstInt; x.dst = dst; x.imm.i = imm; x.meta.width = 8; return x;
        };
        auto bin = [&](ThinOp op, VReg dst, VReg s1, VReg s2) -> ThinInstr {
            ThinInstr x; x.op = op; x.dst = dst; x.src1 = s1; x.src2 = s2; x.meta.width = 8; return x;
        };
        b0.instrs.push_back(ci(v0, 7));
        b0.instrs.push_back(ci(v1, 0));
        b0.instrs.push_back(ci(v2, 1));
        b0.instrs.push_back(bin(ThinOp::Add, v3, v0, v1));
        b0.instrs.push_back(bin(ThinOp::Mul, v4, v0, v2));
        b0.instrs.push_back(bin(ThinOp::Mul, v5, v0, v1));
        b0.instrs.push_back(bin(ThinOp::Sub, v6, v0, v0));
        b0.term.kind = TermKind::Return; b0.term.ret = v3;
        tf.blocks.push_back(std::move(b0));

        size_t orig_binops = 0;
        for (const auto& blk : tf.blocks)
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::Add || in.op == ThinOp::Sub || in.op == ThinOp::Mul)
                    ++orig_binops;

        EmberAnalysisManager am;
        auto pc = reg.create("instcombine");
        EmberPreserved pres = pc->run(tf, am);
        ck(!pres.all_preserved(), "D8.1: instcombine returns Preserved::none() (folds happened)");

        // After instcombine: the 4 BinOps should become Move/ConstInt.
        // Count remaining BinOps (Add/Sub/Mul) — should be 0.
        size_t after_binops = 0;
        for (const auto& blk : tf.blocks)
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::Add || in.op == ThinOp::Sub || in.op == ThinOp::Mul)
                    ++after_binops;
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "D8.2: instcombine removed all identity BinOps (before=%zu after=%zu)",
            orig_binops, after_binops);
        ck(after_binops == 0, msg);
        ck(after_binops < orig_binops, "D8.3: instcombine reduced BinOp count");

        // Verify the fold results: v5 (x*0) and v6 (x-x) should be ConstInt 0;
        // v3 (x+0) and v4 (x*1) should be Move.
        bool v5_is_const0 = false, v6_is_const0 = false;
        bool v3_is_move = false, v4_is_move = false;
        for (const auto& in : tf.blocks[0].instrs) {
            if (in.dst == v5 && in.op == ThinOp::ConstInt && in.imm.i == 0) v5_is_const0 = true;
            if (in.dst == v6 && in.op == ThinOp::ConstInt && in.imm.i == 0) v6_is_const0 = true;
            if (in.dst == v3 && in.op == ThinOp::Move) v3_is_move = true;
            if (in.dst == v4 && in.op == ThinOp::Move) v4_is_move = true;
        }
        ck(v5_is_const0, "D8.4: x*0 folded to ConstInt 0");
        ck(v6_is_const0, "D8.5: x-x folded to ConstInt 0");
        ck(v3_is_move, "D8.6: x+0 folded to Move");
        ck(v4_is_move, "D8.7: x*1 folded to Move");
    }

    // -----------------------------------------------------------------
    // D9 (LLVM-5): Dead store elimination hand-built test.
    // block0: ConstInt v0=5, StoreFrame v0 off=-8,
    //         ConstInt v1=9, StoreFrame v1 off=-8,  (overwrites - no load between)
    //         LoadFrame v2 off=-8, Return v2.
    // The first StoreFrame is dead (overwritten before read) -> dse removes it.
    // -----------------------------------------------------------------
    std::printf("\n--- D9: Dead store elimination ---\n");
    {
        ThinFunction tf;
        tf.name = "dse_test";
        const VReg v0=1, v1=2, v2=3;
        ThinBlock b0; b0.id = 0;
        auto ci = [&](VReg dst, int64_t imm) -> ThinInstr {
            ThinInstr x; x.op = ThinOp::ConstInt; x.dst = dst; x.imm.i = imm; x.meta.width = 8; return x;
        };
        auto st = [&](VReg src, int32_t off) -> ThinInstr {
            ThinInstr x; x.op = ThinOp::StoreFrame; x.src1 = src; x.meta.frame_off = off; x.meta.width = 8; return x;
        };
        b0.instrs.push_back(ci(v0, 5));
        b0.instrs.push_back(st(v0, -8));   // dead store (overwritten)
        b0.instrs.push_back(ci(v1, 9));
        b0.instrs.push_back(st(v1, -8));   // live store (read by LoadFrame)
        ThinInstr ld; ld.op = ThinOp::LoadFrame; ld.dst = v2; ld.meta.frame_off = -8; ld.meta.width = 8;
        b0.instrs.push_back(std::move(ld));
        b0.term.kind = TermKind::Return; b0.term.ret = v2;
        tf.blocks.push_back(std::move(b0));

        size_t orig = total_instrs(tf);
        size_t orig_stores = 0;
        for (const auto& in : tf.blocks[0].instrs)
            if (in.op == ThinOp::StoreFrame) ++orig_stores;

        EmberAnalysisManager am;
        auto pc = reg.create("dse");
        EmberPreserved pres = pc->run(tf, am);
        ck(!pres.all_preserved(), "D9.1: dse returns Preserved::none() (store removed)");

        size_t after_stores = 0;
        for (const auto& in : tf.blocks[0].instrs)
            if (in.op == ThinOp::StoreFrame) ++after_stores;
        char msg[128];
        std::snprintf(msg, sizeof msg,
            "D9.2: dse removed dead store (before=%zu after=%zu)", orig_stores, after_stores);
        ck(after_stores == 1, msg);
        ck(after_stores < orig_stores, "D9.3: dse reduced store count");
        ck(total_instrs(tf) < orig, "D9.4: dse reduced total instr count");
    }

    // -----------------------------------------------------------------
    // D10 (regression): DSE must NOT remove a StoreFrame that feeds a
    // CopyBytes reader. Two StoreFrames to slot -8 with a CopyBytes that
    // READS -8 (its source range) between them: the first store feeds the
    // copy, so it is NOT dead. Before the fix DSE only killed on LoadFrame,
    // so it removed the first store and the CopyBytes read an uninitialized
    // slot (a value-preservation bug, confirmed by a hand-built IR repro).
    // -----------------------------------------------------------------
    std::printf("\n--- D10: DSE keeps a store that feeds a CopyBytes reader ---\n");
    {
        ThinFunction tf; tf.name = "dse_copybytes_reader";
        const VReg v0=1, v1=2, v2=3;
        ThinBlock b0; b0.id = 0;
        auto ci = [&](VReg d, int64_t i)->ThinInstr{ ThinInstr x; x.op=ThinOp::ConstInt; x.dst=d; x.imm.i=i; x.meta.width=8; return x; };
        auto st = [&](VReg s, int32_t off)->ThinInstr{ ThinInstr x; x.op=ThinOp::StoreFrame; x.src1=s; x.meta.frame_off=off; x.meta.width=8; return x; };
        b0.instrs.push_back(ci(v0, 5));
        b0.instrs.push_back(st(v0, -8));                 // first store to -8 (=5)
        // CopyBytes: read 8 bytes from src -8, write to dst -16. Reads slot -8.
        { ThinInstr x; x.op=ThinOp::CopyBytes; x.dst=0; x.src1=0;
          x.meta.frame_off=-16; x.meta.field_off=-8; x.meta.len=8; b0.instrs.push_back(std::move(x)); }
        b0.instrs.push_back(ci(v1, 9));
        b0.instrs.push_back(st(v1, -8));                 // second store to -8 (=9, overwrites)
        { ThinInstr x; x.op=ThinOp::LoadFrame; x.dst=v2; x.meta.frame_off=-16; x.meta.width=8; b0.instrs.push_back(std::move(x)); }
        b0.term.kind=TermKind::Return; b0.term.ret=v2;
        tf.blocks.push_back(std::move(b0));

        size_t stores_before=0; for(auto&b:tf.blocks)for(auto&in:b.instrs)if(in.op==ThinOp::StoreFrame)++stores_before;
        ck(stores_before == 2, "D10.0: two stores before dse");

        EmberAnalysisManager am; auto pc=reg.create("dse"); pc->run(tf,am);
        size_t stores_after=0; for(auto&b:tf.blocks)for(auto&in:b.instrs)if(in.op==ThinOp::StoreFrame)++stores_after;
        char msg[128]; std::snprintf(msg,sizeof msg,"D10.1: dse keeps both stores (CopyBytes is a reader) (before=%zu after=%zu)",stores_before,stores_after);
        ck(stores_after == 2, msg);
        // The first store (off=-8, src=v0/5) must specifically survive.
        bool first_store_survives = false;
        for (const auto& blk : tf.blocks)
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::StoreFrame && in.meta.frame_off == -8 && in.src1 == v0)
                    first_store_survives = true;
        ck(first_store_survives, "D10.2: the first StoreFrame(-8,v0=5) survives (feeds the CopyBytes)");
    }

    // -----------------------------------------------------------------
    // D11 (regression): StoreToLoadForward must NOT forward a LoadFrame
    // past an intervening CopyBytes that WRITES the slot. StoreFrame to -8,
    // then CopyBytes whose dest range covers -8 (overwrites it), then
    // LoadFrame of -8: the load must read what the CopyBytes wrote, NOT the
    // stale StoreFrame value. Before the fix, forward rewrote the LoadFrame
    // to a Move of the stale StoreFrame src (confirmed by IR dump),
    // delivering the wrong value.
    // -----------------------------------------------------------------
    std::printf("\n--- D11: forward does not cross a CopyBytes writer ---\n");
    {
        ThinFunction tf; tf.name = "forward_copybytes_writer";
        const VReg v0=1, v77=2, vload=3;
        ThinBlock b0; b0.id = 0;
        auto ci = [&](VReg d, int64_t i)->ThinInstr{ ThinInstr x; x.op=ThinOp::ConstInt; x.dst=d; x.imm.i=i; x.meta.width=8; return x; };
        auto st = [&](VReg s, int32_t off)->ThinInstr{ ThinInstr x; x.op=ThinOp::StoreFrame; x.src1=s; x.meta.frame_off=off; x.meta.width=8; return x; };
        b0.instrs.push_back(ci(v0, 5));
        b0.instrs.push_back(st(v0, -8));                 // slot -8 = 5
        b0.instrs.push_back(ci(v77, 77));
        b0.instrs.push_back(st(v77, -16));               // slot -16 = 77 (copy source)
        // CopyBytes: write 8 bytes from src -16 to dst -8 (OVERWRITES slot -8).
        { ThinInstr x; x.op=ThinOp::CopyBytes; x.dst=0; x.src1=0;
          x.meta.frame_off=-8; x.meta.field_off=-16; x.meta.len=8; b0.instrs.push_back(std::move(x)); }
        { ThinInstr x; x.op=ThinOp::LoadFrame; x.dst=vload; x.meta.frame_off=-8; x.meta.width=8; b0.instrs.push_back(std::move(x)); }
        b0.term.kind=TermKind::Return; b0.term.ret=vload;
        tf.blocks.push_back(std::move(b0));

        EmberAnalysisManager am; auto pc=reg.create("forward"); pc->run(tf,am);
        // The LoadFrame dst=vload off=-8 must NOT have been rewritten to a Move
        // (the CopyBytes overwrote -8, so the load must read the slot, not the
        // stale StoreFrame src v0).
        bool load_preserved = false;
        for (const auto& blk : tf.blocks)
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::LoadFrame && in.dst == vload && in.meta.frame_off == -8)
                    load_preserved = true;
        ck(load_preserved, "D11.1: LoadFrame(-8) preserved (not forwarded past the CopyBytes writer)");
        // And there must be no Move dst=vload src=v0 (the stale forward).
        bool stale_move = false;
        for (const auto& blk : tf.blocks)
            for (const auto& in : blk.instrs)
                if (in.op == ThinOp::Move && in.dst == vload && in.src1 == v0)
                    stale_move = true;
        ck(!stale_move, "D11.2: no stale Move vload=v0 (forward correctly killed by CopyBytes write)");
    }

    std::printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
