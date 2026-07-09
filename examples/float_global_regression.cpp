// float_global_regression - pins the float-global-write fix (the v1.0-integration-
// found bug). The AssignExpr global-store path stored RAX even for f32 globals, but
// the value is in xmm0; the fix (store_xmm0_to_global, movss with correct REX-B-after-
// F3 prefix ordering) routes float-global stores to movss. Tests the heap-GlobalsBlock
// case (the real host shape) + read-modify-write + multiple f32 globals + i64
// regression (didn't break the int path) + f32 global read in an expression.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;
struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string,int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};
static int g_fail = 0;
static void ck(bool c, const char* m){ std::printf("[%s] %s\n", c?"PASS":"FAIL", m); if(!c) g_fail=1; }

static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "t"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    m->prog = std::move(pr.program);
    int si=0; for (auto& fn : m->prog.funcs) { m->slots[fn.name]=si++; fn.slot=m->slots[fn.name]; }
    NativeTable nt; auto layouts = build_struct_layouts(m->prog); m->prog.string_xor_key=0;
    if (!sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &layouts).ok) return nullptr;
    m->gbs.assign(m->prog.globals.size()*8, 0); m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi=0; for (auto& g : m->prog.globals) { m->gb.index[g.name]=gi++; m->gb.types[g.name]=g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&nt.natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    for (auto& fn : m->prog.funcs) { auto cf=compile_func(fn,ctx); finalize(cf); m->table->set(fn.slot,cf.entry); m->fns.push_back(std::move(cf)); }
    return m;
}
static int64_t call(M& m, const std::string& fn) {
    void* e = m.table->get(m.slots[fn]); using F=int64_t(*)(); return reinterpret_cast<F>(e)();
}
static float g_f32(M& m, int idx){ float v; std::memcpy(&v, &m.gbs[idx*8], 4); return v; }
static int64_t g_i64(M& m, int idx){ int64_t v; std::memcpy(&v, &m.gbs[idx*8], 8); return v; }

int main() {
    std::printf("=== float-global regression (v1.0-integration-found bug fix) ===\n");
    { auto m=compile("global g : f32 = 0.0f;\nfn main() -> i64 { g = 10.0f; return 1; }\n");
      call(*m,"main"); ck(g_f32(*m,0)==10.0f, "(1) f32 global write lands in globals block (heap case)"); }
    { auto m=compile("global g : f32 = 0.0f;\nfn seed() -> i64 { g = 10.0f; return 1; }\nfn dbl() -> i64 { g = g * 2.0f; return 1; }\nfn main() -> i64 { seed(); dbl(); return 1; }\n");
      call(*m,"main"); ck(g_f32(*m,0)==20.0f, "(2) f32 global read-modify-write (g=10; g=g*2 -> 20)"); }
    { auto m=compile("global a : f32 = 0.0f;\nglobal b : f32 = 0.0f;\nfn main() -> i64 { a = 1.5f; b = 2.5f; return 1; }\n");
      call(*m,"main"); ck(g_f32(*m,0)==1.5f && g_f32(*m,1)==2.5f, "(3) two f32 globals both written (a=1.5 b=2.5)"); }
    { auto m=compile("global n : i64 = 0;\nfn main() -> i64 { n = 42; return n; }\n");
      int64_t r=call(*m,"main"); ck(r==42 && g_i64(*m,0)==42, "(4) i64 global write/read still works (int path unbroken)"); }
    { auto m=compile("global g : f32 = 0.0f;\nglobal out : f32 = 0.0f;\nfn seed() -> i64 { g = 10.0f; return 1; }\nfn use() -> i64 { out = g * 3.0f; return 1; }\nfn main() -> i64 { seed(); use(); return 1; }\n");
      call(*m,"main"); ck(g_f32(*m,1)==30.0f, "(5) f32 global read in expr: out = g*3 = 10*3 = 30"); }
    std::printf("\nfloat-global regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
