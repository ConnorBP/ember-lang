// constexpr_test - Tier 1 constexpr fn evaluation regression.
//
// Verifies that a `constexpr fn` called with all-constant args is
// const-evaluated at sema time, producing the correct i64 result when the
// JIT'd code is called. The folded call is replaced with an IntLit before
// codegen, so the emitted code contains no call instruction for the folded
// call — but the primary assertion is value-correctness (the non-circular
// direct-value probe: the C host reads the i64 return out of rax).
//
// Probes (each must PASS):
//   [1] square(7) = 49           (basic constexpr fold)
//   [2] fib(10) = 55             (recursive constexpr fold, depth <= 256)
//   [3] square(5) + square(4) = 41  (constexpr in a larger expression)
//   [4] global g = square(6) + 100 = 136  (constexpr in a global initializer)
//   [5] factorial(5) = 120       (constexpr with a while loop + mutation)
//   [6] double_it(x+10) = 20     (constexpr fn called with RUNTIME arg —
//                                 falls back to a runtime call, still correct)
//   [7] sum_to(100) = 5050       (constexpr with a for loop)
//   [8] nested: square(square(3)) = 81  (nested constexpr calls fold bottom-up)
//
// Modeled on field_of_index_test.cpp (the M struct, compile() helper,
// call0_i64 pattern). Links ember ember_frontend ember_import (pure language
// features — no natives used by these probes).

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
// Returns nullptr + prints the stage on any failure (so a sema/codegen
// regression surfaces as a compile FAIL, not a wrong exit code).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<constexpr>");
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
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    // Seed const + scalar initializers at load (globals.hpp).
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
// assertion signal (read directly out of rax by the C host — a non-circular
// direct-value probe, NOT an in-language comparison).
static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

int main() {
    std::printf("=== constexpr_test: Tier 1 constexpr fn evaluation ===\n\n");

    // [1] square(7) = 49 (basic constexpr fold)
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "fn main() -> i64 { return square(7); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[1] square(7) (compile)"); }
        else { ck(call0_i64(*m, "main") == 49, "[1] square(7) == 49 (basic constexpr fold)"); }
    }

    // [2] fib(10) = 55 (recursive constexpr fold)
    {
        const char* src =
            "constexpr fn fib(n: i64) -> i64 {\n"
            "    if (n <= 1) { return n; }\n"
            "    return fib(n - 1) + fib(n - 2);\n"
            "}\n"
            "fn main() -> i64 { return fib(10); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[2] fib(10) (compile)"); }
        else { ck(call0_i64(*m, "main") == 55, "[2] fib(10) == 55 (recursive constexpr fold)"); }
    }

    // [3] square(5) + square(4) = 41 (constexpr in a larger expression)
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "fn main() -> i64 { return square(5) + square(4); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[3] square(5)+square(4) (compile)"); }
        else { ck(call0_i64(*m, "main") == 41, "[3] square(5) + square(4) == 41 (constexpr in expression)"); }
    }

    // [4] global g = square(6) + 100 = 136 (constexpr in a global initializer)
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "global g : i64 = square(6) + 100;\n"
            "fn main() -> i64 { return g; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[4] global g = square(6)+100 (compile)"); }
        else { ck(call0_i64(*m, "main") == 136, "[4] global g = square(6) + 100 == 136 (constexpr in global init)"); }
    }

    // [5] factorial(5) = 120 (constexpr with a while loop + mutation)
    {
        const char* src =
            "constexpr fn factorial(n: i64) -> i64 {\n"
            "    let mut result: i64 = 1;\n"
            "    let mut i: i64 = 1;\n"
            "    while (i <= n) {\n"
            "        result = result * i;\n"
            "        i = i + 1;\n"
            "    }\n"
            "    return result;\n"
            "}\n"
            "fn main() -> i64 { return factorial(5); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[5] factorial(5) (compile)"); }
        else { ck(call0_i64(*m, "main") == 120, "[5] factorial(5) == 120 (constexpr while loop + mutation)"); }
    }

    // [6] double_it(x+10) = 20 (constexpr fn called with RUNTIME arg —
    //     falls back to a runtime call, still correct)
    {
        const char* src =
            "constexpr fn double_it(n: i64) -> i64 { return n * 2; }\n"
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 0;\n"
            "    x = double_it(x + 10);\n"
            "    return x;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[6] double_it(x+10) runtime fallback (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[6] double_it(x+10) == 20 (constexpr runtime fallback)"); }
    }

    // [7] sum_to(100) = 5050 (constexpr with a for loop)
    {
        const char* src =
            "constexpr fn sum_to(n: i64) -> i64 {\n"
            "    let mut s: i64 = 0;\n"
            "    for (let mut i: i64 = 1; i <= n; i = i + 1) {\n"
            "        s = s + i;\n"
            "    }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 { return sum_to(100); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[7] sum_to(100) (compile)"); }
        else { ck(call0_i64(*m, "main") == 5050, "[7] sum_to(100) == 5050 (constexpr for loop)"); }
    }

    // [8] nested: square(square(3)) = 81 (nested constexpr calls fold bottom-up)
    {
        const char* src =
            "constexpr fn square(n: i64) -> i64 { return n * n; }\n"
            "fn main() -> i64 { return square(square(3)); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[8] square(square(3)) (compile)"); }
        else { ck(call0_i64(*m, "main") == 81, "[8] square(square(3)) == 81 (nested constexpr fold)"); }
    }

    // [9] constexpr fn with compound assignment + if/else (power via repeated mul)
    {
        const char* src =
            "constexpr fn ipow(base: i64, exp: i64) -> i64 {\n"
            "    let mut result: i64 = 1;\n"
            "    let mut e: i64 = exp;\n"
            "    while (e > 0) {\n"
            "        if ((e & 1) == 1) { result *= base; }\n"
            "        base = base * base;\n"
            "        e = e >> 1;\n"
            "    }\n"
            "    return result;\n"
            "}\n"
            "fn main() -> i64 { return ipow(2, 10); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[9] ipow(2,10) (compile)"); }
        else { ck(call0_i64(*m, "main") == 1024, "[9] ipow(2,10) == 1024 (constexpr compound assign + if/else + bit ops)"); }
    }

    // [10] priv constexpr fn — both modifiers in either order
    {
        const char* src =
            "priv constexpr fn secret(n: i64) -> i64 { return n + 1; }\n"
            "constexpr fn helper(n: i64) -> i64 { return secret(n) * 2; }\n"
            "fn main() -> i64 { return helper(5); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[10] priv constexpr fn (compile)"); }
        else { ck(call0_i64(*m, "main") == 12, "[10] priv constexpr fn == 12 (secret(5)=6 * 2, both modifiers)"); }
    }

    if (g_fail) {
        std::printf("\nFAILED\n");
        return 1;
    }
    std::printf("\nAll constexpr probes passed.\n");
    return 0;
}
