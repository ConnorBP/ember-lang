// for_each_array_test - pins the iterable() hook's array<T> case (Tier 1):
//   for (x in array_handle) { ... }
// lowered by codegen to  len = array_length(h); while i < len { x = array_get_*(h, i); ... }.
//
// Each probe compiles a tiny .ember through the full parse->sema->codegen->
// finalize->call pipeline with the array extension natives registered, then
// reads the i64 return value DIRECTLY via a C reinterpret of the JIT entry
// (call0_i64). This is a NON-CIRCULAR direct-value probe: the host reads the
// i64 out of rax in C, not an in-language equality check that could hide a
// codegen bug. A reverted fix (the ForEachStmt array branch removed, the sema
// element-type inference removed, or the array_get_* dispatch wrong) -> a
// wrong i64 here -> this test records FAIL.
//
// Probes (each must PASS; a revert turns it red):
//   [1] i64 array for-each sum:           [10,20,30]            -> 60
//   [2] u8 array for-each sum (x is u8):  [10,20,30] (elem=1)   -> 60
//   [3] f32 array for-each sum (x is f32):[10,20,30] (elem=4)   -> 1 (==60.0)
//   [4] empty array for-each:             array_new(8,0)        -> 0  (body never runs)
//   [5] break in for-each body:           [10,20,30,40,50]      -> 60 (stops before 40)
//   [6] continue in for-each body:        [10,20,30,40,50]      -> 90 (skips 20+40)
//   [7] single-element array for-each:    [42]                  -> 42
//   [8] handle alias tag propagation:     let b = a; for(x in b)-> 60
//        (sema tags `b` from `a`'s array_elem_ty; verifies the Ident-alias path)
//   [9] full-i64 storage pin (>8-bit):    [256,1000,30000]      -> 31256
//        (the direct C read catches a codegen bug that stored only the low
//         byte; ember_cli's 8-bit exit code would hide this)
//
// Links ember + ember_frontend + ember_ext_array (no prism dependency) -
// proving the array extension + the for-each lowering are reusable outside
// any specific host, matching ext_runtime_test's link shape.

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "ext_array.hpp"

#include <cmath>
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
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile `src` through the full pipeline with the array extension natives
// registered. Returns nullptr + prints the stage on any failure (so a
// sema/codegen regression surfaces as a compile FAIL, not a wrong exit code).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<for_each_array>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    // Register the array extension natives (array_new/array_length/array_get_*/
    // array_set_*). The for-each lowering calls array_length + array_get_*.
    std::unordered_map<std::string, NativeSig> natives;
    ext_array::register_natives(natives);
    auto layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string literals in these probes
    auto sr = sema(m->prog, natives, m->slots, 0, nullptr, &layouts);
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
    ctx.natives = &natives;
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

int main() {
    // Start from a clean host store so leftover slots from another test in the
    // same process don't shift the 1-based handles.
    ext_array::reset();

    std::printf("=== for-each over array<T> handles (iterable() hook, Tier 1) ===\n\n");

    {
        // [1] i64 array for-each sum.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 3);\n"
            "    array_set_i64(a, 0, 10);\n"
            "    array_set_i64(a, 1, 20);\n"
            "    array_set_i64(a, 2, 30);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { sum = sum + x; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[1] i64 array for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 60, "[1] i64 array for-each sum: [10,20,30] == 60"); }
    }
    {
        // [2] u8 array for-each sum (x is u8; cast to i64 to accumulate).
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(1, 3);\n"
            "    array_set_u8(a, 0, 10);\n"
            "    array_set_u8(a, 1, 20);\n"
            "    array_set_u8(a, 2, 30);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { sum = sum + (x as i64); }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[2] u8 array for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 60, "[2] u8 array for-each sum: [10,20,30] == 60 (array_get_u8 dispatch)"); }
    }
    {
        // [3] f32 array for-each sum (x is f32; result arrives in xmm0).
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(4, 3);\n"
            "    array_set_f32(a, 0, 10.0);\n"
            "    array_set_f32(a, 1, 20.0);\n"
            "    array_set_f32(a, 2, 30.0);\n"
            "    let mut sum: f32 = 0.0;\n"
            "    for (x in a) { sum = sum + x; }\n"
            "    if (sum == 60.0) { return 1; }\n"
            "    return 0;\n"
            "}\n");
        if (!m) { ck(false, "[3] f32 array for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 1, "[3] f32 array for-each sum: [10,20,30] == 60.0 (array_get_f32/xmm0 path)"); }
    }
    {
        // [4] empty array for-each: the desugared while loop never runs.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 0);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { sum = sum + 1; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[4] empty array for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 0, "[4] empty array for-each: array_new(8,0) -> 0 (body never runs)"); }
    }
    {
        // [5] break in for-each body over an array handle.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 5);\n"
            "    array_set_i64(a, 0, 10);\n"
            "    array_set_i64(a, 1, 20);\n"
            "    array_set_i64(a, 2, 30);\n"
            "    array_set_i64(a, 3, 40);\n"
            "    array_set_i64(a, 4, 50);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { if (x >= 40) { break; } sum = sum + x; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[5] break in for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 60, "[5] break in for-each: [10,20,30,40,50] -> 60 (stops before 40)"); }
    }
    {
        // [6] continue in for-each body over an array handle.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 5);\n"
            "    array_set_i64(a, 0, 10);\n"
            "    array_set_i64(a, 1, 20);\n"
            "    array_set_i64(a, 2, 30);\n"
            "    array_set_i64(a, 3, 40);\n"
            "    array_set_i64(a, 4, 50);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { if (x == 20) { continue; } if (x == 40) { continue; } sum = sum + x; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[6] continue in for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 90, "[6] continue in for-each: [10,20,30,40,50] -> 90 (skips 20+40)"); }
    }
    {
        // [7] single-element array for-each: the loop runs exactly once.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 1);\n"
            "    array_set_i64(a, 0, 42);\n"
            "    let mut found: i64 = 0;\n"
            "    for (x in a) { found = x; }\n"
            "    return found;\n"
            "}\n");
        if (!m) { ck(false, "[7] single-element for-each (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[7] single-element for-each: [42] -> 42"); }
    }
    {
        // [8] handle alias tag propagation: `let b = a;` carries the array
        // element-type tag from `a` (sema's Ident-alias path in the LetStmt
        // check), so `for (x in b)` type-checks and lowers correctly even
        // though `b`'s initializer is a bare Ident, not an array_new call.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 3);\n"
            "    array_set_i64(a, 0, 10);\n"
            "    array_set_i64(a, 1, 20);\n"
            "    array_set_i64(a, 2, 30);\n"
            "    let b = a;\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in b) { sum = sum + x; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[8] handle alias tag propagation (compile)"); }
        else { ck(call0_i64(*m, "main") == 60, "[8] handle alias: let b = a; for (x in b) -> 60"); }
    }
    {
        // [9] full-i64 storage pin (NON-CIRCULAR via the direct C read, NOT
        // the 8-bit OS exit code): elements >= 256 prove array_get_i64
        // returns a full 8-byte i64 per element, not a byte. ember_cli run
        // can't observe values >= 256 (Windows exit codes truncate to 8
        // bits); this probe reads the i64 return DIRECTLY via call0_i64 in C,
        // so a codegen bug that stored/loaded only the low byte (256 -> 0,
        // 1000 -> 232) would surface as a wrong i64 here. 256+1000+30000 = 31256.
        auto m = compile(
            "fn main() -> i64 {\n"
            "    let a: i64 = array_new(8, 3);\n"
            "    array_set_i64(a, 0, 256);\n"
            "    array_set_i64(a, 1, 1000);\n"
            "    array_set_i64(a, 2, 30000);\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a) { sum = sum + x; }\n"
            "    return sum;\n"
            "}\n");
        if (!m) { ck(false, "[9] full-i64 storage (compile)"); }
        else {
            int64_t r = call0_i64(*m, "main");
            ck(r == 31256, "[9] full-i64 storage: [256,1000,30000] sum == 31256 (not byte-truncated)");
        }
    }

    std::printf("\nfor-each array regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
