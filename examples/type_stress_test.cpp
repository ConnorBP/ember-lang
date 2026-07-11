// type_stress_test - auto type-detection stress suite (ctest).
//
// A programmatic companion to tests/lang/valid_type_stress.ember and
// sema_invalid_type_narrow.ember / sema_invalid_type_edge.ember. Compiles
// small probes through the full parse->sema->codegen->finalize->call pipeline
// and asserts either (a) the i64 return out of rax for a positive probe, or
// (b) sema REJECTION for a negative probe (compile() returns nullptr on a
// sema error, so a rejection that compiles is a FAIL, and an acceptance that
// should reject is a FAIL). Mirrors typed_enum_test.cpp's M struct + compile()
// + call0_i64 pattern.
//
// Probes are grouped by the usage chain the user asked to stress
// (declaration -> assignment -> binary op -> arg passing -> return -> cast ->
// array index -> match pattern -> native call) and cover every integer width
// / signedness, typed enums, floats, and the constexpr-fold interaction. The
// three bugs the suite was built to find (and pin) are carried as explicit
// regression probes:
//   [B1] f32 -> f64 implicit widening (was rejected; spec Section 6 requires it)
//   [B2] typed-enum variant literal narrowing / signedness change (was silently
//        accepted via adapt_int_lit re-typing; now rejected like an enum var)
//   [B3] binary bitwise (& | ^) on floats (was silently accepted; spec Section
//        7 makes bitwise integer-only; now rejected, matching unary ~)
//
// Links ember ember_frontend ember_import (pure language features, no natives
// — the abs_i64 native-call probe is exercised in the .ember script instead).

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
    auto lr = tokenize(src, "<type_stress>");
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
    std::printf("=== type_stress_test: auto type-detection stress suite ===\n\n");

    // ============================================================
    // GROUP 1 — INTEGER WIDENING (assignment chain, same signedness)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let a: i8 = 5; let b: i16 = a; let c: i32 = b; let d: i64 = c; return d; }");
        if (!m) ck(false, "[1a] i8->i16->i32->i64 chain (compile)");
        else ck(call0_i64(*m, "main") == 5, "[1a] i8->i16->i32->i64 widening chain == 5");
    }
    {
        auto m = compile("fn main() -> i64 { let e: u8 = 7; let f: u16 = e; let g: u32 = f; let h: u64 = g; return h as i64; }");
        if (!m) ck(false, "[1b] u8->u16->u32->u64 chain (compile)");
        else ck(call0_i64(*m, "main") == 7, "[1b] u8->u16->u32->u64 widening chain == 7");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i16 = 11; let b: i64 = a; return b; }");
        if (!m) ck(false, "[1c] i16->i64 skip-width (compile)");
        else ck(call0_i64(*m, "main") == 11, "[1c] i16->i64 skip-width widening == 11");
    }

    // ============================================================
    // GROUP 2 — INTEGER NARROWING / SIGNEDNESS (must REJECT)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let x: i64 = 5; let y: i32 = x; return 0; }", true);
        ck(m == nullptr, "[2a] REJECT i64->i32 narrowing (let)");
    }
    {
        auto m = compile("fn main() -> i64 { let x: u64 = 5; let y: u32 = x; return 0; }", true);
        ck(m == nullptr, "[2b] REJECT u64->u32 narrowing (let)");
    }
    {
        auto m = compile("fn main() -> i64 { let x: i32 = 5; let y: u32 = x; return 0; }", true);
        ck(m == nullptr, "[2c] REJECT i32->u32 signedness change (let)");
    }
    {
        auto m = compile("fn main() -> i64 { let x: u32 = 5; let y: i32 = x; return 0; }", true);
        ck(m == nullptr, "[2d] REJECT u32->i32 signedness change (let)");
    }

    // ============================================================
    // GROUP 3 — RETURN-TYPE WIDENING + NARROWING
    // ============================================================
    {
        auto m = compile("fn g() -> i64 { let x: i32 = 42; return x; } fn main() -> i64 { return g(); }");
        if (!m) ck(false, "[3a] return i32 from i64 fn (compile)");
        else ck(call0_i64(*m, "main") == 42, "[3a] i32->i64 return widening == 42");
    }
    {
        auto m = compile("fn g() -> i32 { let x: i64 = 5; return x; } fn main() -> i64 { return 0; }", true);
        ck(m == nullptr, "[3b] REJECT return i64 from i32 fn (narrowing)");
    }
    {
        auto m = compile("fn g() -> i64 { let x: u8 = 5; return x; } fn main() -> i64 { return 0; }", true);
        ck(m == nullptr, "[3c] REJECT return u8 from i64 fn (signedness)");
    }

    // ============================================================
    // GROUP 4 — ARG-PASSING WIDENING + NARROWING
    // ============================================================
    {
        auto m = compile("fn g(p: i64) -> i64 { return p; } fn main() -> i64 { let x: i32 = 7; return g(x); }");
        if (!m) ck(false, "[4a] arg i32 to i64 param (compile)");
        else ck(call0_i64(*m, "main") == 7, "[4a] i32->i64 arg widening == 7");
    }
    {
        auto m = compile("fn g(p: i32) -> i64 { return p; } fn main() -> i64 { let x: i64 = 7; return g(x); }", true);
        ck(m == nullptr, "[4b] REJECT arg i64 to i32 param (narrowing)");
    }
    {
        auto m = compile("fn g(p: i64) -> i64 { return p; } fn main() -> i64 { let x: u8 = 7; return g(x); }", true);
        ck(m == nullptr, "[4c] REJECT arg u8 to i64 param (signedness)");
    }
    {
        auto m = compile("fn g(p: u8) -> i64 { return p as i64; } fn main() -> i64 { return g(300); }", true);
        ck(m == nullptr, "[4d] REJECT literal 300 to u8 param (out of range)");
    }

    // ============================================================
    // GROUP 5 — BINARY OPS (same-type result + literal adaptation)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let a: u64 = 100; let b = 5 + a; return b as i64; }");
        if (!m) ck(false, "[5a] literal 5 + u64 var (compile)");
        else ck(call0_i64(*m, "main") == 105, "[5a] literal adapts to u64, 5 + 100 == 105");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i32 = 1; let b: i64 = 2; let c = a + b; return c; }", true);
        ck(m == nullptr, "[5b] REJECT i32 + i64 mixed-type arithmetic");
    }
    {
        auto m = compile("fn main() -> i64 { let a: u32 = 1; let b: i32 = 2; let c = a + b; return 0; }", true);
        ck(m == nullptr, "[5c] REJECT u32 + i32 mixed signedness arithmetic");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i32 = 1; let b: i64 = 1; if (a == b) { return 1; } return 0; }", true);
        ck(m == nullptr, "[5d] REJECT i32 == i64 mixed-type comparison");
    }

    // ============================================================
    // GROUP 6 — CAST MATRIX (explicit `as`)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let x: u8 = 256 as u8; if (x == 0) { return 1; } return 0; }");
        if (!m) ck(false, "[6a] 256 as u8 (compile)");
        else ck(call0_i64(*m, "main") == 1, "[6a] 256 as u8 truncates to 0");
    }
    {
        auto m = compile("fn main() -> i64 { let x: u8 = (-1) as u8; if (x == 255) { return 1; } return 0; }");
        if (!m) ck(false, "[6b] -1 as u8 (compile)");
        else ck(call0_i64(*m, "main") == 1, "[6b] -1 as u8 -> 255 (sign + trunc)");
    }
    {
        auto m = compile("fn main() -> i64 { let x: u32 = 5; let y: i64 = x as i64; return y; }");
        if (!m) ck(false, "[6c] u32 as i64 (compile)");
        else ck(call0_i64(*m, "main") == 5, "[6c] u32 as i64 signedness cast == 5");
    }
    {
        auto m = compile("fn main() -> i64 { let x: u64 = 5; let y: f64 = x as f64; return 0; }", true);
        ck(m == nullptr, "[6d] REJECT u64 as f64 (unsigned int->float out of v1 matrix)");
    }
    {
        auto m = compile("fn main() -> i64 { let x: f64 = 5.0; let y: u64 = x as u64; return 0; }", true);
        ck(m == nullptr, "[6e] REJECT f64 as u64 (float->unsigned out of v1 matrix)");
    }

    // ============================================================
    // GROUP 7 — ARRAY INDEX (any integer type; non-integer rejected)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let a: i64[4]; a[0]=10; a[1]=20; a[2]=30; a[3]=40; let i: u32 = 2; return a[i]; }");
        if (!m) ck(false, "[7a] arr[u32] (compile)");
        else ck(call0_i64(*m, "main") == 30, "[7a] unsigned index arr[u32] == 30");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i64[4]; a[0]=10; a[1]=20; a[2]=30; a[3]=40; let i: i8 = 3; return a[i]; }");
        if (!m) ck(false, "[7b] arr[i8] (compile)");
        else ck(call0_i64(*m, "main") == 40, "[7b] small signed index arr[i8] == 40");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i64[4]; a[0]=10; let i: f32 = 0.0f; return a[i]; }", true);
        ck(m == nullptr, "[7c] REJECT arr[f32] (non-integer index)");
    }
    {
        auto m = compile("fn main() -> i64 { let a: i64[4]; a[0]=10; return a[true]; }", true);
        ck(m == nullptr, "[7d] REJECT arr[true] (bool index)");
    }

    // ============================================================
    // GROUP 8 — MATCH PATTERN TYPES
    // ============================================================
    {
        auto m = compile("enum Color : i32 { Red, Green, Blue } fn main() -> i64 { let c: Color = Color::Green; let mut r: i64 = 0; match (c) { Color::Red => { r = 10; }, Color::Green => { r = 20; }, _ => { r = 99; } } return r; }");
        if (!m) ck(false, "[8a] typed-enum match (compile)");
        else ck(call0_i64(*m, "main") == 20, "[8a] match on Color, Green arm == 20");
    }
    {
        auto m = compile("fn main() -> i64 { let n: i64 = 5; let mut r: i64 = 0; match (n) { 1 => { r = 10; }, 5 => { r = 20; }, _ => { r = 99; } } return r; }");
        if (!m) ck(false, "[8b] i64 match with i32 lit (compile)");
        else ck(call0_i64(*m, "main") == 20, "[8b] i64 subject, literal pattern (both int) == 20");
    }

    // ============================================================
    // GROUP 9 — TYPED ENUM + INTEGER INTERACTIONS
    // ============================================================
    {
        auto m = compile("enum Color : i32 { Red, Green, Blue } fn main() -> i64 { let c: Color = Color::Red; let i: i64 = c; return i; }");
        if (!m) ck(false, "[9a] enum:i32 -> i64 (compile)");
        else ck(call0_i64(*m, "main") == 0, "[9a] enum:i32 -> i64 widening == 0");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green, Blue } fn main() -> i64 { let c: Color = 5; return 0; }", true);
        ck(m == nullptr, "[9b] REJECT int -> enum (let c: Color = 5)");
    }
    {
        auto m = compile("enum Small : u8 { A = 200, B = 255 } fn main() -> i64 { let x: u8 = Small::A; return x as i64; }");
        if (!m) ck(false, "[9c] enum:u8 -> u8 (compile)");
        else ck(call0_i64(*m, "main") == 200, "[9c] u8-backed enum -> u8 == 200");
    }
    {
        auto m = compile("enum Big : i64 { X = 1000000000000 } fn main() -> i64 { let x: i64 = Big::X; return x; }");
        if (!m) ck(false, "[9d] enum:i64 large value (compile)");
        else ck(call0_i64(*m, "main") == 1000000000000LL, "[9d] i64-backed enum large value == 10^12");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green } fn main() -> i64 { let c: Color = Color::Red; if (c == 0) { return 1; } return 0; }");
        if (!m) ck(false, "[9e] enum == int cmp (compile)");
        else ck(call0_i64(*m, "main") == 1, "[9e] typed enum == plain int comparison allowed == 1");
    }
    {
        auto m = compile("enum Color : i32 { Red } enum Hue : i32 { A } fn main() -> i64 { let a: Color = Color::Red; let b: Hue = Hue::A; if (a == b) { return 1; } return 0; }", true);
        ck(m == nullptr, "[9f] REJECT Color == Hue (different typed enums)");
    }

    // ============================================================
    // GROUP 10 — CONSTEXPR + TYPE INTERACTIONS
    // ============================================================
    {
        auto m = compile("constexpr fn f() -> i32 { return 7; } fn main() -> i64 { let x: i64 = f(); return x; }");
        if (!m) ck(false, "[10a] constexpr i32 -> i64 (compile)");
        else ck(call0_i64(*m, "main") == 7, "[10a] constexpr fn i32 used as i64 (fold + widen) == 7");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green } constexpr fn gc() -> Color { return Color::Green; } fn main() -> i64 { let x: i64 = gc(); return x; }");
        if (!m) ck(false, "[10b] constexpr -> Color -> i64 (compile)");
        else ck(call0_i64(*m, "main") == 1, "[10b] constexpr fn returning typed enum, used as i64 == 1");
    }
    {
        auto m = compile("constexpr fn f() -> i32 { return 5; } enum Color : i32 { Red } fn main() -> i64 { let c: Color = f(); return 0; }", true);
        ck(m == nullptr, "[10c] REJECT constexpr i32 -> enum (int->enum even via fold)");
    }

    // ============================================================
    // GROUP 11 — FLOAT WIDENING (B1 regression: f32 -> f64 implicit)
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let x: f32 = 1.5f; let y: f64 = x; if (y == 1.5) { return 1; } return 0; }");
        if (!m) ck(false, "[11a] f32 -> f64 implicit widening (compile) [B1 regression]");
        else ck(call0_i64(*m, "main") == 1, "[11a] f32 -> f64 implicit widening works == 1 [B1 regression]");
    }
    {
        auto m = compile("fn g() -> f64 { let x: f32 = 2.25f; return x; } fn main() -> i64 { let y: f64 = g(); if (y == 2.25) { return 1; } return 0; }");
        if (!m) ck(false, "[11b] f32 -> f64 return widening (compile)");
        else ck(call0_i64(*m, "main") == 1, "[11b] f32 -> f64 return widening == 1");
    }
    {
        auto m = compile("fn g(p: f64) -> f64 { return p; } fn main() -> i64 { let x: f32 = 3.5f; let y: f64 = g(x); if (y == 3.5) { return 1; } return 0; }");
        if (!m) ck(false, "[11c] f32 -> f64 arg widening (compile)");
        else ck(call0_i64(*m, "main") == 1, "[11c] f32 -> f64 arg widening == 1");
    }
    {
        auto m = compile("fn main() -> i64 { let x: f64 = 1.5; let y: f32 = x; return 0; }", true);
        ck(m == nullptr, "[11d] REJECT f64 -> f32 narrowing (still explicit-only)");
    }

    // ============================================================
    // GROUP 12 — INFERENCE
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let x = 5; return x; }");
        if (!m) ck(false, "[12a] let x = 5 infers i64 (compile)");
        else ck(call0_i64(*m, "main") == 5, "[12a] let x = 5 infers i64 == 5");
    }
    {
        auto m = compile("fn main() -> i64 { let x = 5.0f; if (x == 5.0f) { return 1; } return 0; }");
        if (!m) ck(false, "[12b] let x = 5.0f infers f32 (compile)");
        else ck(call0_i64(*m, "main") == 1, "[12b] let x = 5.0f infers f32 == 1");
    }
    {
        auto m = compile("fn main() -> i64 { let x = 5.0; if (x == 5.0) { return 1; } return 0; }");
        if (!m) ck(false, "[12c] let x = 5.0 infers f64 (compile)");
        else ck(call0_i64(*m, "main") == 1, "[12c] let x = 5.0 infers f64 == 1");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green } fn main() -> i64 { let x = Color::Red; return x as i64; }");
        if (!m) ck(false, "[12d] let x = Color::Red infers Color (compile)");
        else ck(call0_i64(*m, "main") == 0, "[12d] let x = Color::Red infers typed enum == 0");
    }

    // ============================================================
    // GROUP 13 — B2 regression: typed-enum variant literal narrowing
    // ============================================================
    {
        auto m = compile("enum Color : i32 { Red, Green } fn main() -> i64 { let x: i16 = Color::Red; return 0; }", true);
        ck(m == nullptr, "[13a] REJECT enum variant literal -> i16 (narrowing) [B2 regression]");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green } fn main() -> i64 { let x: u8 = Color::Red; return 0; }", true);
        ck(m == nullptr, "[13b] REJECT enum variant literal -> u8 (signedness) [B2 regression]");
    }
    {
        auto m = compile("enum Color : i32 { Red, Green } fn main() -> i64 { let a: i16[2] = [Color::Red, Color::Green]; return 0; }", true);
        ck(m == nullptr, "[13c] REJECT enum variant literals in i16 array [B2 regression]");
    }
    {
        // widening still works (the positive side of B2)
        auto m = compile("enum Color : i32 { Red, Green, Blue } fn main() -> i64 { let x: i64 = Color::Blue; return x; }");
        if (!m) ck(false, "[13d] enum variant literal -> i64 widening (compile) [B2 positive]");
        else ck(call0_i64(*m, "main") == 2, "[13d] enum variant literal -> i64 widening == 2 [B2 positive]");
    }

    // ============================================================
    // GROUP 14 — B3 regression: binary bitwise on floats
    // ============================================================
    {
        auto m = compile("fn main() -> i64 { let a: f32 = 1.0f; let b: f32 = 2.0f; let c = a & b; return 0; }", true);
        ck(m == nullptr, "[14a] REJECT f32 & f32 (bitwise on float) [B3 regression]");
    }
    {
        auto m = compile("fn main() -> i64 { let a: f64 = 1.0; let b: f64 = 2.0; let c = a | b; return 0; }", true);
        ck(m == nullptr, "[14b] REJECT f64 | f64 (bitwise on float) [B3 regression]");
    }
    {
        auto m = compile("fn main() -> i64 { let a: f64 = 1.0; let b: f64 = 2.0; let c = a ^ b; return 0; }", true);
        ck(m == nullptr, "[14c] REJECT f64 ^ f64 (bitwise on float) [B3 regression]");
    }
    {
        // integer bitwise still works (the positive side of B3)
        auto m = compile("fn main() -> i64 { let a: i32 = 6; let b: i32 = 3; if ((a & b) != 2) { return 1; } if ((a | b) != 7) { return 2; } if ((a ^ b) != 5) { return 3; } return 42; }");
        if (!m) ck(false, "[14d] int bitwise still works (compile) [B3 positive]");
        else ck(call0_i64(*m, "main") == 42, "[14d] int & | ^ still work == 42 [B3 positive]");
    }
    {
        // float arithmetic still works (B3 must not over-reach into + - * /)
        auto m = compile("fn main() -> i64 { let a: f32 = 1.5f; let b: f32 = 2.5f; if (a + b != 4.0f) { return 1; } return 42; }");
        if (!m) ck(false, "[14e] float arithmetic still works (compile) [B3 positive]");
        else ck(call0_i64(*m, "main") == 42, "[14e] f32 + f32 still works == 42 [B3 positive]");
    }

    if (g_fail) {
        std::printf("\nFAILED\n");
        return 1;
    }
    std::printf("\nAll type-stress probes passed.\n");
    return 0;
}
