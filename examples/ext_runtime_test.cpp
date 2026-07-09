// ext_runtime_test - runtime coverage for the relocated ember extensions
// (RESTRUCTURE_PLAN.md Section 6). em_roundtrip_test and import_roundtrip_test prove
// the .em + cross-module paths but call NO relocated extension native; this
// test closes that gap by exercising the math extension's `sqrt` through the
// full lex→parse→sema→codegen→JIT→call path and asserting the result.
//
// It links only ember + ember_frontend + ember_ext_math (no prism/prism_script_
// host) - ext_math depends only on ember_frontend, so a non-prism consumer can
// register these natives directly, proving the extensions are reusable outside
// prism (the restructure's "for future reuse" goal).

#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../extensions/math/ext_math.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- source: a function that calls the relocated sqrt native ----
    const std::string src =
        "fn t(x: f32) -> f32 { return sqrt(x); }\n";

    // ---- lex ----
    auto lr = tokenize(src, "<ext_runtime>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }
    if (pr.program.funcs.size() != 1) {
        std::printf("FAIL: expected 1 func, got %zu\n", pr.program.funcs.size());
        return 1;
    }

    // ---- slot assignment ----
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }

    // ---- natives: register ONLY the math extension (proves it's reusable
    //      without prism_script_host). This is the load-bearing line: if the
    //      extension relocation left sqrt unregistered or double-registered,
    //      sema or the JIT call fails here. ----
    std::unordered_map<std::string, NativeSig> natives;
    ember::ext_math::register_natives(natives);

    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0; // no string literals -> no encryption needed
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }

    // ---- globals (empty) ----
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store(0);
    gb.base = int64_t(gb_store.data());
    g_globals_for_codegen = &gb;

    // ---- dispatch table + codegen ctx ----
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;

    // ---- compile + finalize ----
    std::vector<CompiledFn> fns;
    for (auto& fn : pr.program.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        if (!finalize(cf)) {
            std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str());
            for (auto& done : fns) if (done.exec) free_executable(done.exec);
            return 1;
        }
        table.set(fn.slot, cf.entry);
        fns.push_back(std::move(cf));
    }

    // ---- call t(4.0) and assert ~2.0 ----
    // Win64 f32(f32): arg in xmm0, return in xmm0.
    using F = float(*)(float);
    auto* fn = reinterpret_cast<F>(fns[0].entry);
    float r4 = fn(4.0f);
    float r9 = fn(9.0f);
    bool ok4 = std::fabs(r4 - 2.0f) < 1e-5f;
    bool ok9 = std::fabs(r9 - 3.0f) < 1e-5f;
    std::printf("[1] sqrt(4.0) == 2.0 : %s (got %.6f)\n", passfail(ok4), r4);
    std::printf("[2] sqrt(9.0) == 3.0 : %s (got %.6f)\n", passfail(ok9), r9);
    if (!ok4) failures++;
    if (!ok9) failures++;

    for (auto& cf : fns) if (cf.exec) free_executable(cf.exec);

    if (failures == 0) { std::printf("\next runtime: PASS\n"); return 0; }
    std::printf("\next runtime: FAIL (%d)\n", failures);
    return 1;
}
