// field_of_index_test — pins the C1 fix: `arr[i].field` on a struct array
// and slice. Before the fix, FieldExpr { base: IndexExpr { arr, i }, field }
// only handled a bare-Ident base in both backends; the tree-walker emitted
// nothing and the thin IR returned VReg 0, so `arr[0].a` silently returned 0
// (or garbage) instead of the field value. This probe compiles tiny .ember
// sources through the full tree-walker pipeline and reads the i64 return
// directly via C (a non-circular direct-value probe, NOT an in-language
// comparison — matching array_lit_test's shape). Pure language features (no
// native calls), so links only the core libs.
//
// Covers:
//   - fixed array of structs:  arr[0].a == 10, arr[1].b == 200, arr[2].a == 30
//   - slice of structs:        s[0].b == 100, s[1].a == 20, s[2].b == 300
//   - non-zero element index + non-zero field offset (arr[1].b exercises
//     element stride *and* the second field's offset within the struct)
//   - the float-field variant (arr[0].y as f64) to pin the movsd load path
//
// The thin-IR (enable_ir_backend) path has a SEPARATE, pre-existing known
// gap (gap 2j: LoadFrame-from-computed-address is not frame-backed) that
// affects ALL IndexExpr loads on that backend, not just FieldExpr; this test
// therefore pins the tree-walker (default) path, which is what `ember_cli
// run` uses. The thin-IR FieldExpr lowering itself (IndexAddr + LoadFrame at
// the field offset) is structurally correct and mirrors the IndexExpr case;
// its runtime verification is blocked by gap 2j and tracked as Stage B/C.
#include "../src/engine.hpp"
#include "../src/codegen.hpp"     // CodeGenCtx, compile_func, g_globals_for_codegen, GlobalsBlock
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"  // NativeTable (empty here - no natives used)
#include "../src/dispatch_table.hpp"
#include "../src/em_file.hpp"
#include "../src/sema.hpp"        // sema, build_struct_layouts
#include "../src/parser.hpp"
#include "../src/lexer.hpp"

#include <cstdio>
#include <cstring>
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
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile `src` through the full tree-walker pipeline (empty native table -
// these probes use no natives). Returns nullptr + prints the stage on any
// failure (so a sema/codegen regression surfaces as a compile FAIL, not a
// wrong exit code).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<field_of_index>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    NativeTable nt;  // empty: no natives used by these probes
    auto layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // raw rodata (no string natives here anyway)
    auto sr = sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &nt.natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &layouts;
    for (auto& fn : m->prog.funcs) {
        auto cf = compile_func(fn, ctx);
        if (!finalize(cf)) { std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str()); return nullptr; }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

// Call a no-arg script fn that returns i64. The i64 return value IS the
// assertion signal (read directly out of rax by the C host - a non-circular
// direct-value probe, NOT an in-language comparison).
static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

// Call a no-arg script fn that returns f64. The f64 return is read directly
// out of xmm0 by the C host.
static double call0_f64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0;
    void* e = m.table->get(it->second);
    using F = double (*)();
    return reinterpret_cast<F>(e)();
}

int main() {
    std::printf("=== field-of-index regression (C1: arr[i].field on struct array/slice) ===\n");
    std::printf("(pins the tree-walker FieldExpr IndexExpr-base eval case)\n\n");

    // The shared struct-array source; each case varies only the returned
    // field access. P{a:i64, b:i64} is 16 bytes: field a at offset 0, b at 8.
    const char* head =
        "struct P { a: i64; b: i64; }\n"
        "fn main() -> i64 {\n"
        "    let arr: P[3] = [\n"
        "        P { a: 10, b: 100 },\n"
        "        P { a: 20, b: 200 },\n"
        "        P { a: 30, b: 300 }\n"
        "    ];\n";

    // [1] arr[0].a == 10  (index 0, field at offset 0)
    {
        std::string src = std::string(head) + "    return arr[0].a;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[1] arr[0].a (compile)"); }
        else { ck(call0_i64(*m, "main") == 10, "[1] fixed-array arr[0].a == 10"); }
    }
    // [2] arr[1].b == 200 (non-zero index AND non-zero field offset 8)
    {
        std::string src = std::string(head) + "    return arr[1].b;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[2] arr[1].b (compile)"); }
        else { ck(call0_i64(*m, "main") == 200, "[2] fixed-array arr[1].b == 200 (index stride + field offset)"); }
    }
    // [3] arr[2].a == 30  (index 2, field at offset 0)
    {
        std::string src = std::string(head) + "    return arr[2].a;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[3] arr[2].a (compile)"); }
        else { ck(call0_i64(*m, "main") == 30, "[3] fixed-array arr[2].a == 30"); }
    }
    // [4] slice s[0].b == 100 (slice view + field at offset 8)
    {
        std::string src = std::string(head) +
            "    let s: P[] = arr[..];\n"
            "    return s[0].b;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[4] s[0].b (compile)"); }
        else { ck(call0_i64(*m, "main") == 100, "[4] slice s[0].b == 100 (slice base + field offset)"); }
    }
    // [5] slice s[1].a == 20 (slice + index 1 + field at offset 0)
    {
        std::string src = std::string(head) +
            "    let s: P[] = arr[..];\n"
            "    return s[1].a;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[5] s[1].a (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[5] slice s[1].a == 20"); }
    }
    // [6] slice s[2].b == 300 (slice + index 2 + field at offset 8)
    {
        std::string src = std::string(head) +
            "    let s: P[] = arr[..];\n"
            "    return s[2].b;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[6] s[2].b (compile)"); }
        else { ck(call0_i64(*m, "main") == 300, "[6] slice s[2].b == 300"); }
    }
    // [7] sum of fields across elements (exercises multiple arr[i].field
    // reads in one expression - pins that the element address reloads
    // correctly across clobbers on the tree-walker). 10+200+30 = 240.
    {
        std::string src = std::string(head) +
            "    return arr[0].a + arr[1].b + arr[2].a;\n}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[7] sum of fields (compile)"); }
        else { ck(call0_i64(*m, "main") == 240, "[7] arr[0].a + arr[1].b + arr[2].a == 240 (multi-read)"); }
    }
    // [8] float struct field: arr[0].y (f64) == 1.5 - pins the movsd load path
    // for a float field read off an indexed struct element.
    {
        std::string src =
            "struct V { x: i64; y: f64; }\n"
            "fn main() -> f64 {\n"
            "    let arr: V[2] = [\n"
            "        V { x: 1, y: 1.5 },\n"
            "        V { x: 2, y: 2.5 }\n"
            "    ];\n"
            "    return arr[0].y;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[8] arr[0].y f64 (compile)"); }
        else { ck(call0_f64(*m, "main") == 1.5, "[8] fixed-array arr[0].y (f64) == 1.5 (movsd load path)"); }
    }
    // [9] float struct field at non-zero index + the i64 field after it:
    // arr[1].x == 2 and arr[1].y == 2.5 (index 1, both field offsets).
    {
        std::string src =
            "struct V { x: i64; y: f64; }\n"
            "fn fx() -> i64 {\n"
            "    let arr: V[2] = [\n"
            "        V { x: 1, y: 1.5 },\n"
            "        V { x: 2, y: 2.5 }\n"
            "    ];\n"
            "    return arr[1].x;\n"
            "}\n"
            "fn fy() -> f64 {\n"
            "    let arr: V[2] = [\n"
            "        V { x: 1, y: 1.5 },\n"
            "        V { x: 2, y: 2.5 }\n"
            "    ];\n"
            "    return arr[1].y;\n"
            "}\n";
        auto m = compile(src);
        if (!m) { ck(false, "[9] arr[1] mixed fields (compile)"); }
        else {
            ck(call0_i64(*m, "fx") == 2, "[9a] fixed-array arr[1].x (i64) == 2 (index 1, field offset 0)");
            ck(call0_f64(*m, "fy") == 2.5, "[9b] fixed-array arr[1].y (f64) == 2.5 (index 1, field offset 8)");
        }
    }

    std::printf("\nfield-of-index regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
