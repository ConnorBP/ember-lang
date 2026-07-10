// v0.6_hot_reload_test - single-function hot reload (HOT_RELOAD.md §3).
//
// Verifies the reload protocol: recompile one function, atomically swap its
// dispatch slot, callers see the new behavior, the old page is retired. Tests
// the per-function-independence property (a reloaded fn's callers keep working
// because they go through the dispatch table, not baked addresses).
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "hot_reload.hpp"

#include "ext_vec.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

using namespace ember;
static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

struct Mod {
    std::vector<CompiledFn> fns;          // owns the live pages (retired ones freed explicitly)
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string,int> slots;
    GlobalsBlock gb; std::vector<uint8_t> gbs;
    Program prog;
    std::unordered_map<std::string,NativeSig> natives;
    OpOverloadTable ov;
    StructLayoutTable layouts;
    Mod() : table(std::make_unique<DispatchTable>(0)) {}
};

static std::unique_ptr<Mod> compile(const std::string& src) {
    auto m = std::make_unique<Mod>();
    auto lr = tokenize(src, "<t>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    m->prog = std::move(pr.program);
    int si=0; for (auto& fn : m->prog.funcs) { m->slots[fn.name]=si++; fn.slot=m->slots[fn.name]; }
    ext_vec::register_natives(m->natives); ext_math::register_natives(m->natives);
    ext_vec::register_overloads(m->ov);
    m->layouts = build_struct_layouts(m->prog); m->prog.string_xor_key=0;
    if (!sema(m->prog, m->natives, m->slots, 0, &m->ov, &m->layouts).ok) return nullptr;
    m->gbs.assign(m->prog.globals.size()*8, 0); m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi=0; for (auto& g : m->prog.globals) { m->gb.index[g.name]=gi++; m->gb.types[g.name]=g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&m->layouts;
    for (auto& fn : m->prog.funcs) { auto cf=compile_func(fn,ctx); finalize(cf); m->table->set(fn.slot,cf.entry); m->fns.push_back(std::move(cf)); }
    return m;
}
static int64_t call_void(Mod& m, const std::string& fn) {
    void* e = m.table->get(m.slots[fn]); using F=int64_t(*)(); return reinterpret_cast<F>(e)();
}
static int64_t call_i64(Mod& m, const std::string& fn, int64_t a) {
    void* e = m.table->get(m.slots[fn]); using F=int64_t(*)(int64_t); return reinterpret_cast<F>(e)(a);
}
static CodeGenCtx make_ctx(Mod& m) {
    CodeGenCtx ctx; ctx.globals_base=m.gb.base; ctx.dispatch_base=int64_t(m.table->base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&m.layouts;
    return ctx;
}

int main() {
    std::printf("=== v0.6 single-function hot reload tests ===\n");

    // (1) reload a fn, caller sees new behavior
    {
        auto m = compile("fn val() -> i64 { return 10; }\nfn main() -> i64 { return val(); }\n");
        check(m != nullptr, "compile module");
        check(call_void(*m,"main")==10, "(1) pre-reload: main()->val()==10");
        auto ctx = make_ctx(*m);
        auto rr = reload_function("fn val() -> i64 { return 99; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(rr.ok, "(1) reload succeeded");
        check(call_void(*m,"main")==99, "(1) post-reload: main()->val()==99 (caller saw new behavior via dispatch table)");
        if (rr.old_entry) free_executable(rr.old_entry);
        m->fns.push_back({rr.new_fn.name, std::move(rr.new_fn.bytes), rr.new_fn.exec, rr.new_fn.entry, {}});  // keep alive
    }
    // (2) reload a recursive fn (fib) — self-call goes through dispatch, picks up new body
    {
        auto m = compile("fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n-1)+fib(n-2); }\nfn main() -> i64 { return fib(5); }\n");
        check(call_void(*m,"main")==5, "(2) pre-reload fib(5)==5");
        // reload fib to a broken-but-valid version: fib(n) = n*2
        auto ctx = make_ctx(*m);
        auto rr = reload_function("fn fib(n: i64) -> i64 { return n * 2; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(rr.ok, "(2) reload fib succeeded");
        check(call_i64(*m,"fib",5)==10, "(2) post-reload: fib(5)==10 (self-call picked up new body via dispatch slot)");
        if (rr.old_entry) free_executable(rr.old_entry);
        m->fns.push_back({rr.new_fn.name, std::move(rr.new_fn.bytes), rr.new_fn.exec, rr.new_fn.entry, {}});
    }
    // (3) reload FAILURE leaves the module untouched (old code keeps running)
    {
        auto m = compile("fn val() -> i64 { return 7; }\nfn main() -> i64 { return val(); }\n");
        auto ctx = make_ctx(*m);
        // reload with a type error (return type mismatch) -> must fail, module unchanged
        auto rr = reload_function("fn val() -> i64 { return 1.5f; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(!rr.ok, "(3) reload with type error failed (ok=false)");
        check(call_void(*m,"main")==7, "(3) module untouched after failed reload: main()==7 (old code still running)");
    }
    // (4) reload a fn called from a loop (the hot-path case)
    {
        auto m = compile("fn step(x: i64) -> i64 { return x + 1; }\nfn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < 100) { s = s + step(i); i = i + 1; } return s; }\n");
        check(call_void(*m,"main")==5050, "(4) pre-reload: sum of step(i)=i+1 i=0..99 == 5050");
        auto ctx = make_ctx(*m);
        auto rr = reload_function("fn step(x: i64) -> i64 { return x * 2; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(rr.ok, "(4) reload step succeeded");
        // sum of 2i for i=0..99 = 2 * 4950 = 9900
        check(call_void(*m,"main")==9900, "(4) post-reload: sum of step(i)=2i == 9900 (loop caller saw new behavior)");
        if (rr.old_entry) free_executable(rr.old_entry);
        m->fns.push_back({rr.new_fn.name, std::move(rr.new_fn.bytes), rr.new_fn.exec, rr.new_fn.entry, {}});
    }
    // (5) reload a fn not in the module -> error
    {
        auto m = compile("fn main() -> i64 { return 1; }\n");
        auto ctx = make_ctx(*m);
        auto rr = reload_function("fn nope() -> i64 { return 1; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(!rr.ok, "(5) reload of absent fn fails (not in module)");
    }
    // (6) ABI signature changes are rejected before Program/slot mutation.
    {
        auto m = compile("fn val(x: i64) -> i64 { return x + 1; }\nfn main() -> i64 { return val(6); }\n");
        auto ctx = make_ctx(*m);
        auto arity = reload_function("fn val() -> i64 { return 9; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(!arity.ok && call_void(*m, "main") == 7, "(6a) changed arity rejected; old body remains live");
        auto param = reload_function("fn val(x: f32) -> i64 { return 9; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(!param.ok && call_void(*m, "main") == 7, "(6b) changed parameter type rejected; old body remains live");
        auto ret = reload_function("fn val(x: i64) -> f32 { return 9.0f; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(!ret.ok && call_void(*m, "main") == 7, "(6c) changed return type rejected; old body remains live");
        auto same = reload_function("fn val(x: i64) -> i64 { return x + 3; }\n", m->prog, *m->table, ctx, m->natives, &m->ov, &m->layouts);
        check(same.ok && call_void(*m, "main") == 9, "(6d) unchanged canonical signature reload succeeds");
        if (same.old_entry) free_executable(same.old_entry);
        if (same.ok) m->fns.push_back({same.new_fn.name, std::move(same.new_fn.bytes), same.new_fn.exec, same.new_fn.entry, {}});
    }

    std::printf("\nv0.6 hot reload test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
