// static_assert_test - Tier 1 compile-time assertion regression.
//
// Verifies `static_assert(cond, "msg")` semantics end-to-end through the
// full parse->sema->codegen->finalize pipeline:
//   - a PASSING assertion (cond folds to true) is ELIDED — it produces no
//     runtime code, and the function body runs exactly as if the assertion
//     were absent (the i64 return is the direct-value probe).
//   - a FAILING assertion (cond folds to false) is a SEMA compile error
//     carrying the user message — compile() returns nullptr with the sema
//     errors printed (the assertion never reaches codegen).
//   - a NON-CONSTANT condition (a runtime value) is a SEMA compile error
//     ("condition must be a compile-time constant") — also never reaches
//     codegen.
//   - a constexpr fn in the condition is folded by the constexpr-call
//     pre-pass BEFORE check_static_assert runs, so `square(7) == 49` folds
//     to true (the cross-feature interaction the Tier 1 unblock enables).
//   - in-function static_assert (inside a body) applies the identical
//     verdict as a top-level one.
//   - logical operators (&&, ||, !) and comparisons (==, !=, <, <=, >, >=)
//     over compile-time integer constants fold via the extended
//     try_eval_const_bool.
//
// Modeled on constexpr_test.cpp (the M struct, compile() helper, call0_i64
// pattern). Links ember ember_frontend ember_import (pure language features).

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"  // NativeTable (empty here)

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

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

// Compile `src` through the full parse->sema->codegen->finalize pipeline.
// Returns nullptr + prints the stage on any failure (so a sema failure from
// a failing/non-const static_assert surfaces as a compile FAIL, not a wrong
// exit code). `expect_sema_fail` just toggles the diagnostic framing.
static std::unique_ptr<M> compile(const std::string& src, bool expect_sema_fail = false) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<static_assert>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    NativeTable nt;  // empty: no natives used by these probes
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // raw rodata (no string natives here)
    auto sr = sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &m->layouts);
    if (!sr.ok) {
        if (!expect_sema_fail) {
            std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
            for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        }
        return nullptr;
    }
    if (expect_sema_fail) {
        std::printf("FAIL: sema was expected to FAIL but succeeded\n");
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    GlobalInitCtx gic{m->gbs, m->gb.index, m->gb.types};
    eval_global_initializers(m->prog, gic);
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.globals_index = &m->gb.index;
    ctx.globals_types = &m->gb.types;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &nt.natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    for (auto& fn : m->prog.funcs) {
        auto cf = compile_func(fn, ctx);
        if (!finalize(cf)) { std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str()); return nullptr; }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

// Call a no-arg script fn that returns i64. The i64 return value IS the
// assertion signal (read directly out of rax — a non-circular direct-value
// probe, NOT an in-language comparison).
static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

int main() {
    std::printf("=== static_assert_test: Tier 1 compile-time assertion ===\n\n");

    // [1] top-level passing assertion (literal arithmetic) -> elided, main runs
    {
        const char* src =
            "static_assert(1 + 1 == 2, \"one plus one is two\");\n"
            "fn main() -> i64 { return 42; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[1] top-level true (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[1] top-level static_assert(1+1==2) elided, main==42"); }
    }

    // [2] top-level FAILING assertion -> sema error (compile returns nullptr)
    {
        const char* src =
            "static_assert(1 + 1 == 3, \"deliberately wrong\");\n"
            "fn main() -> i64 { return 42; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[2] top-level static_assert(1+1==3) -> sema FAIL (false condition)");
    }

    // [3] non-constant condition (a global reference) -> sema error
    {
        const char* src =
            "global g : i64 = 5;\n"
            "static_assert(g == 5, \"g is five\");\n"
            "fn main() -> i64 { return g; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[3] static_assert(g==5) -> sema FAIL (non-constant condition)");
    }

    // [4] constexpr fn condition -> folded by the pre-pass, passes, main runs
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "static_assert(square(7) == 49, \"square(7) == 49\");\n"
            "fn main() -> i64 { return square(7); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[4] constexpr cond (compile)"); }
        else { ck(call0_i64(*m, "main") == 49, "[4] static_assert(square(7)==49) folds + main==49"); }
    }

    // [5] in-function passing assertion -> elided, body runs
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    static_assert(2 * 3 == 6, \"two times three is six\");\n"
            "    static_assert(10 > 5, \"ten greater than five\");\n"
            "    return 42;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[5] in-fn true (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[5] in-fn static_assert elided, main==42"); }
    }

    // [6] in-function FAILING assertion -> sema error
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    static_assert(1 == 2, \"one is not two\");\n"
            "    return 42;\n"
            "}\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[6] in-fn static_assert(1==2) -> sema FAIL (false condition)");
    }

    // [7] in-function non-constant condition (a local) -> sema error
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let x: i64 = 5;\n"
            "    static_assert(x == 5, \"x is five\");\n"
            "    return x;\n"
            "}\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[7] in-fn static_assert(x==5) -> sema FAIL (non-constant local)");
    }

    // [8] logical operators (&&, ||, !) over compile-time comparisons -> passes
    {
        const char* src =
            "static_assert((1 == 1) && (2 == 2), \"both true\");\n"
            "static_assert((1 == 2) || (3 == 3), \"one true\");\n"
            "static_assert(!(1 == 2), \"not false\");\n"
            "static_assert(5 >= 5 && 5 <= 5, \"ge and le\");\n"
            "fn main() -> i64 { return 42; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[8] logical ops (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[8] static_assert with &&/||/! folds, main==42"); }
    }

    // [9] non-bool condition (an int literal) -> sema type error
    {
        const char* src =
            "static_assert(5, \"five is truthy\");\n"
            "fn main() -> i64 { return 42; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[9] static_assert(5) -> sema FAIL (non-bool condition)");
    }

    // [10] nested constexpr in the condition: square(square(3)) == 81
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "static_assert(square(square(3)) == 81, \"nested constexpr fold\");\n"
            "fn main() -> i64 { return 81; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[10] nested constexpr cond (compile)"); }
        else { ck(call0_i64(*m, "main") == 81, "[10] static_assert(square(square(3))==81) folds, main==81"); }
    }

    if (g_fail) {
        std::printf("\nFAILED\n");
        return 1;
    }
    std::printf("\nAll static_assert probes passed.\n");
    return 0;
}
