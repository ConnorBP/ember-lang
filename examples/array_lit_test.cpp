// array_lit_test - pins array-literal expressions (chunk c2):
//   let arr: i64[3] = [10, 20, 30];   (fixed-array construction)
//   let s:   i64[]  = [1, 2, 3];      (slice construction - backing alloc + {ptr,len})
//
// Each probe compiles a tiny .ember through the full parse->sema->codegen->
// finalize->call pipeline (the em_roundtrip_test / binding_abi_test shape) and
// reads the i64 return value DIRECTLY via a C reinterpret of the JIT entry
// (call0_i64), asserting the exit code. This is a NON-CIRCULAR direct-value
// probe: the host reads the i64 return out of rax in C, not an in-language
// equality check that could circularly hide a codegen bug. A reverted fix
// (the LetStmt init ArrayLit branch removed, or the eval() slice-ArrayLit
// branch removed, or the sema type-bake removed) -> compile fails or a wrong
// exit code -> this test records FAIL.
//
// Probes (each must PASS; a revert turns it red):
//   [1] fixed-array let-init:  arr[0]+arr[1]+arr[2] == 60  (10+20+30)
//   [2] fixed-array single elem read: arr[1] == 20
//   [3] slice let-init:  s[2] == 3  (backing storage + ptr/len correct)
//   [4] slice let-init first elem: s[0] == 1  (ptr points at the right place)
//   [5] slice ArrayLit as a SCRIPT call arg: third([5,6,7]) s[2]==7
//       (materializes backing into a temp, yields {rax=ptr,rdx=len}, the
//        existing 2-word slice-arg path stashes it; callee indexes s[2])
//   [6] slice ArrayLit as a SCRIPT return value: mk()[1]==20
//       (eval() yields {rax=ptr,rdx=len}; the slice-return path returns it;
//        caller stores into its own slice local and indexes s[1])
//
// Links only the core libs (ember ember_frontend ember_import) - the probes
// are pure language features (no native calls), so this test is INDEPENDENT
// of the extension refactor (no ember_ext_* link needed).

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"  // NativeTable (empty here - no natives used)

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

// Compile `src` through the full pipeline (empty native table - these probes
// use no natives). Returns nullptr + prints the stage on any failure (so a
// sema/codegen regression surfaces as a compile FAIL, not a wrong exit code).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<array_lit>");
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

int main() {
    std::printf("=== array-literal regression (chunk c2) ===\n");
    std::printf("(pins fixed-array + slice array-literal construction)\n\n");

    {
        auto m = compile("fn main() -> i64 { let arr: i64[3] = [10, 20, 30]; return arr[0] + arr[1] + arr[2]; }\n");
        if (!m) { ck(false, "[1] fixed-array let-init (compile)"); }
        else { ck(call0_i64(*m, "main") == 60, "[1] fixed-array let-init: [10,20,30] sum == 60"); }
    }
    {
        auto m = compile("fn main() -> i64 { let arr: i64[3] = [10, 20, 30]; return arr[1]; }\n");
        if (!m) { ck(false, "[2] fixed-array single elem (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[2] fixed-array single elem read: arr[1] == 20"); }
    }
    {
        auto m = compile("fn main() -> i64 { let s: i64[] = [1, 2, 3]; return s[2]; }\n");
        if (!m) { ck(false, "[3] slice let-init (compile)"); }
        else { ck(call0_i64(*m, "main") == 3, "[3] slice let-init: [1,2,3] s[2] == 3"); }
    }
    {
        auto m = compile("fn main() -> i64 { let s: i64[] = [1, 2, 3]; return s[0]; }\n");
        if (!m) { ck(false, "[4] slice let-init first elem (compile)"); }
        else { ck(call0_i64(*m, "main") == 1, "[4] slice let-init first elem: s[0] == 1 (ptr correct)"); }
    }
    {
        // Slice ArrayLit as a SCRIPT call arg: the literal materializes a
        // backing temp + yields {rax=ptr, rdx=len}; the callee's slice param
        // receives it and indexes s[2].
        auto m = compile(
            "fn third(s: i64[]) -> i64 { return s[2]; }\n"
            "fn main() -> i64 { return third([5, 6, 7]); }\n");
        if (!m) { ck(false, "[5] slice ArrayLit as arg (compile)"); }
        else { ck(call0_i64(*m, "main") == 7, "[5] slice ArrayLit as script arg: third([5,6,7]) s[2] == 7"); }
    }
    {
        // Slice ArrayLit as a SCRIPT return value: eval() yields {rax=ptr,
        // rdx=len}; the slice-return path returns it; the caller stores it
        // into its own slice local and indexes s[1].
        auto m = compile(
            "fn mk() -> i64[] { return [10, 20, 30]; }\n"
            "fn main() -> i64 { let s: i64[] = mk(); return s[1]; }\n");
        if (!m) { ck(false, "[6] slice ArrayLit as return (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[6] slice ArrayLit as return: mk()[1] == 20"); }
    }
    {
        // Full-i64 storage pin (NON-CIRCULAR via the direct C read, NOT the
        // 8-bit OS exit code): elements >= 256 prove the backing stores a
        // full 8-byte i64 per element, not a byte. `ember_cli run` can't
        // observe values >= 256 because Windows exit codes truncate to 8
        // bits (2003 & 0xFF == 211 - the documented CLI limitation); this
        // probe reads the i64 return DIRECTLY via call0_i64 in C, so a
        // codegen bug that stored only the low byte (256 -> 0, 1000 -> 232)
        // would surface as a wrong i64 here, not be hidden by exit-code
        // truncation. 256+1000+30000 = 31256.
        auto m = compile("fn main() -> i64 { let b: i64[3] = [256, 1000, 30000]; return b[0] + b[1] + b[2]; }\n");
        if (!m) { ck(false, "[7] full-i64 storage (compile)"); }
        else {
            int64_t r = call0_i64(*m, "main");
            ck(r == 31256, "[7] full-i64 storage: [256,1000,30000] sum == 31256 (not byte-truncated)");
        }
    }
    {
        // Slice full-i64 storage pin: same as [7] but for a slice (the
        // backing temp must hold full i64s too). 256+1000 = 1256.
        auto m = compile("fn main() -> i64 { let s: i64[] = [256, 1000]; return s[0] + s[1]; }\n");
        if (!m) { ck(false, "[8] slice full-i64 storage (compile)"); }
        else {
            int64_t r = call0_i64(*m, "main");
            ck(r == 1256, "[8] slice full-i64 storage: [256,1000] sum == 1256 (not byte-truncated)");
        }
    }

    std::printf("\narray-literal regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
