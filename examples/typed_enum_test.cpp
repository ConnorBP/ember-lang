// typed_enum_test - Tier 1 typed enums + enum-from-constexpr-expr regression.
//
// Verifies `enum E : T { ... }` makes E a real type backed by the integer T,
// and that a variant's explicit value may be a constexpr fn call. Probes the
// full sema + codegen + JIT pipeline (parse->sema->codegen->finalize->call):
// each positive probe compiles a small module, calls main, and asserts the
// i64 return out of rax (a non-circular direct-value probe, mirroring
// constexpr_test.cpp); each negative probe asserts sema REJECTS the source
// (compile() returns nullptr on a sema error, so a rejection that compiles
// is a FAIL, and an acceptance that should reject is a FAIL).
//
// Probes (each must PASS):
//   [1]  let c: Color = Color::Red; return c        -> 0   (basic typed enum)
//   [2]  match on a Color variable, Green arm fires  -> 20  (typed-enum match)
//   [3]  return c (Color) from an i64 fn             -> 1   (enum->int widen)
//   [4]  let y: i64 = c (implicit enum->int)         -> 2   (enum->int assign)
//   [5]  u8-backed enum: Small::C == 255             -> 0   (unsigned backing)
//   [6]  i64-backed enum: Big::X == 10^12            -> 0   (wide backing)
//   [7]  fn param Color, call with Color::Blue       -> 2   (typed param)
//   [8]  global g: Color = Color::Green              -> 1   (typed-enum global)
//   [9]  X = base() (constexpr fn) -> E::X == 42     -> 42  (enum-from-constexpr)
//   [10] X = add(10,32) (constexpr fn w/ args)       -> 42  (constexpr w/ args)
//   [11] REJECT: let c: Color = 5                    (int->enum is a sema error)
//   [12] REJECT: let c: Color = Hue::A               (enum mismatch is sema err)
//   [13] REJECT: enum E : f32 { ... }                (non-integer backing type)
//   [14] explicit `c as i64` enum->int cast          -> 1   (explicit cast matrix)
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
// Returns nullptr + prints the stage on any failure (so a sema rejection that
// SHOULD have compiled surfaces as a compile FAIL, and prints the sema errors
// so a rejection probe can see WHY it failed).
static std::unique_ptr<M> compile(const std::string& src, bool expect_sema_fail = false) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<typed_enum>");
    if (!lr.ok) { if (!expect_sema_fail) std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { if (!expect_sema_fail) std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    NativeTable nt;  // empty: no natives used by these probes
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // raw rodata (no string natives here)
    auto sr = sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &m->layouts);
    if (!sr.ok) {
        if (expect_sema_fail) return nullptr;  // rejection expected: return null (success)
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    if (expect_sema_fail) {
        std::printf("FAIL: expected a sema error but sema SUCCEEDED\n");
        return nullptr;  // should have rejected but didn't
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

static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

int main() {
    std::printf("=== typed_enum_test: Tier 1 typed enums + enum-from-constexpr ===\n\n");

    // [1] basic typed enum: let c: Color = Color::Red; return c -> 0
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 { let c: Color = Color::Red; return c; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[1] basic typed enum (compile)"); }
        else { ck(call0_i64(*m, "main") == 0, "[1] Color::Red == 0 (basic typed enum)"); }
    }

    // [2] typed-enum match: Green arm fires -> 20
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 {\n"
            "    let c: Color = Color::Green;\n"
            "    let mut r: i64 = 0;\n"
            "    match (c) {\n"
            "        Color::Red => { r = 10; },\n"
            "        Color::Green => { r = 20; },\n"
            "        Color::Blue => { r = 30; },\n"
            "        _ => { r = 99; }\n"
            "    }\n"
            "    return r;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[2] typed-enum match (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[2] match Green == 20 (typed-enum match)"); }
    }

    // [3] enum->int widening: return c (Color) from an i64 fn -> 1
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn get_green() -> i64 { let c: Color = Color::Green; return c; }\n"
            "fn main() -> i64 { return get_green(); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[3] enum->int return widening (compile)"); }
        else { ck(call0_i64(*m, "main") == 1, "[3] return Color from i64 fn == 1 (enum->int widen)"); }
    }

    // [4] implicit enum->int assignment: let y: i64 = c -> 2
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 {\n"
            "    let c: Color = Color::Blue;\n"
            "    let y: i64 = c;\n"
            "    return y;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[4] implicit enum->int assign (compile)"); }
        else { ck(call0_i64(*m, "main") == 2, "[4] let y: i64 = c == 2 (implicit enum->int)"); }
    }

    // [5] u8-backed enum: Small::C == 255 (unsigned backing, range check)
    {
        const char* src =
            "enum Small : u8 { A, B, C = 255 }\n"
            "fn main() -> i64 {\n"
            "    if (Small::A != 0) { return 1; }\n"
            "    if (Small::B != 1) { return 2; }\n"
            "    if (Small::C != 255) { return 3; }\n"
            "    return 0;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[5] u8-backed enum (compile)"); }
        else { ck(call0_i64(*m, "main") == 0, "[5] u8-backed enum Small::C == 255 (unsigned backing)"); }
    }

    // [6] i64-backed enum: Big::X == 10^12 (wide backing, large value)
    {
        const char* src =
            "enum Big : i64 { X = 1000000000000 }\n"
            "fn main() -> i64 {\n"
            "    if (Big::X != 1000000000000) { return 1; }\n"
            "    return 0;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[6] i64-backed enum (compile)"); }
        else { ck(call0_i64(*m, "main") == 0, "[6] i64-backed enum Big::X == 10^12 (wide backing)"); }
    }

    // [7] typed enum as fn param: takes_color(Color::Blue) -> 2
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn takes_color(c: Color) -> i64 { return c; }\n"
            "fn main() -> i64 { return takes_color(Color::Blue); }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[7] typed enum fn param (compile)"); }
        else { ck(call0_i64(*m, "main") == 2, "[7] takes_color(Color::Blue) == 2 (typed param)"); }
    }

    // [8] typed-enum global: global g: Color = Color::Green -> 1
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "global g: Color = Color::Green;\n"
            "fn main() -> i64 { return g; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[8] typed-enum global (compile)"); }
        else { ck(call0_i64(*m, "main") == 1, "[8] global g: Color = Color::Green == 1 (typed-enum global)"); }
    }

    // [9] enum-from-constexpr: X = base() (constexpr fn) -> E::X == 42
    {
        const char* src =
            "constexpr fn base() -> i64 { return 42; }\n"
            "enum E : i64 { X = base() }\n"
            "fn main() -> i64 { return E::X; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[9] enum-from-constexpr (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[9] E::X = base() == 42 (enum-from-constexpr)"); }
    }

    // [10] enum-from-constexpr with args: X = add(10, 32) -> 42
    {
        const char* src =
            "constexpr fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "enum E : i64 { X = add(10, 32) }\n"
            "fn main() -> i64 { return E::X; }\n";
        auto m = compile(src);
        if (!m) { ck(false, "[10] enum-from-constexpr w/ args (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[10] E::X = add(10,32) == 42 (constexpr w/ args)"); }
    }

    // [11] REJECT: let c: Color = 5 (int->enum implicit assignment)
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 { let c: Color = 5; return 0; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[11] REJECT let c: Color = 5 (int->enum is a sema error)");
    }

    // [12] REJECT: let c: Color = Hue::A (enum mismatch)
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "enum Hue : i32 { A, B, C }\n"
            "fn main() -> i64 { let c: Color = Hue::A; return 0; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[12] REJECT let c: Color = Hue::A (enum mismatch is a sema error)");
    }

    // [13] REJECT: enum E : f32 (non-integer backing type)
    {
        const char* src =
            "enum E : f32 { A, B }\n"
            "fn main() -> i64 { return 0; }\n";
        auto m = compile(src, /*expect_sema_fail=*/true);
        ck(m == nullptr, "[13] REJECT enum E : f32 (non-integer backing type)");
    }

    // [14] explicit `c as i64` enum->int cast -> 1
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 {\n"
            "    let c: Color = Color::Green;\n"
            "    return c as i64;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[14] explicit enum->int cast (compile)"); }
        else { ck(call0_i64(*m, "main") == 1, "[14] c as i64 == 1 (explicit enum->int cast)"); }
    }

    if (g_fail) {
        std::printf("\nFAILED\n");
        return 1;
    }
    std::printf("\nAll typed-enum probes passed.\n");
    return 0;
}
