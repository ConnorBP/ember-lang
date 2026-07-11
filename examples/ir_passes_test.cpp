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
#include "../extensions/opt/ext_opt.hpp"  // register_passes

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

int main() {
    std::printf("=== ir_passes_test: Stage C IR optimization passes ===\n");

    // (1) Registry: register_passes provides all three passes by name.
    std::printf("(1) Registry\n");
    EmberPassRegistry reg;
    ext_opt::register_passes(reg);
    ck(reg.has("constprop"), "registry has \"constprop\"");
    ck(reg.has("dce"), "registry has \"dce\"");
    ck(reg.has("cse"), "registry has \"cse\"");

    // The four workloads.
    struct Workload { const char* name; const char* src; };
    Workload workloads[] = {
        {"constprop_fold", SRC_CONSTPROP_FOLD},
        {"dce_dead_store", SRC_DCE_DEAD_STORE},
        {"cse_redundant",  SRC_CSE_REDUNDANT},
        {"licm_invariant", SRC_LICM_INVARIANT},
    };
    const int NW = 4;

    // For each pass: (a) value-preserving on all 4 workloads, (b) instr-count
    // reduction on its target workload.
    struct PassTest { const char* name; const char* target; };
    PassTest passes[] = {
        {"constprop", "constprop_fold"},
        {"dce",       "dce_dead_store"},
        {"cse",       "cse_redundant"},
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

        // (b) Instr-count reduction on the target workload.
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

    // (3) All three passes on licm_invariant (no target pass — just no-crash +
    // value-preserving, already covered by (a) for each pass).
    std::printf("\n--- licm_invariant (no target pass; all three value-preserving) ---\n");
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

    std::printf("\n%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
