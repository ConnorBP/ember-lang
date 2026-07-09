// v0.6_lifecycle_test - lifecycle annotation discovery + tick-mode tests.
//
// Tests the host-discovery API (lifecycle.hpp) and the --tick execution model:
//   1. get_annotated_functions returns the right slots for @entry/@on_tick/@event
//   2. multiple @on_tick fns all discovered
//   3. @event("name") arg-matching (only matching event_name returned)
//   4. tick loop calls every @on_tick fn (verified via a global counter the fns
//      increment) — simulates the CLI's --tick via the same get_annotated_functions
//      + dispatch-table-call path, bounded by a tick count (no tty needed)
//   5. @entry return <= 0 semantics: the host's "stay loaded" check (entry_says_stay)
//   6. no annotations -> empty results (not an error)
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "lifecycle.hpp"

#include "ext_vec.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <algorithm>

using namespace ember;
static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

// Compile a source to a runnable module (mirrors the bench's ember_compile, minimal).
struct Mod { std::vector<CompiledFn> fns; std::unique_ptr<DispatchTable> table; std::unordered_map<std::string,int> slots; GlobalsBlock gb; std::vector<uint8_t> gbs; Program prog; Mod() : table(std::make_unique<DispatchTable>(0)) {} };
static std::unique_ptr<Mod> compile(const std::string& src) {
    auto m = std::make_unique<Mod>();
    auto lr = tokenize(src, "<t>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    m->prog = std::move(pr.program);
    int si=0; for (auto& fn : m->prog.funcs) { m->slots[fn.name]=si++; fn.slot=m->slots[fn.name]; }
    std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
    ext_vec::register_natives(natives); ext_math::register_natives(natives);
    auto layouts = build_struct_layouts(m->prog); m->prog.string_xor_key=0;
    if (!sema(m->prog, natives, m->slots, 0, &ov, &layouts).ok) return nullptr;
    m->gbs.assign(m->prog.globals.size()*8, 0); m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi=0; for (auto& g : m->prog.globals) { m->gb.index[g.name]=gi++; m->gb.types[g.name]=g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    for (auto& fn : m->prog.funcs) { auto cf=compile_func(fn,ctx); finalize(cf); m->table->set(fn.slot,cf.entry); m->fns.push_back(std::move(cf)); }
    return m;
}
static int64_t call_void(Mod& m, const std::string& fn) {
    void* e = m.table->get(m.slots[fn]); using F=int64_t(*)(); return reinterpret_cast<F>(e)();
}
// read a global i64 by index (the host-side globals-block access)
static int64_t global_i64(Mod& m, const std::string& name) {
    auto it = std::find_if(m.prog.globals.begin(), m.prog.globals.end(),
                           [&](const GlobalDecl& g){ return g.name==name; });
    if (it == m.prog.globals.end()) return -1;
    // globals are 8 bytes each at [base + i*8]; find the index
    for (size_t i=0;i<m.prog.globals.size();++i) if (m.prog.globals[i].name==name)
        return *reinterpret_cast<int64_t*>(m.gbs.data() + i*8);
    return -1;
}

int main() {
    std::printf("=== v0.6 lifecycle + tick-mode tests ===\n");

    // (1) + (2) @entry/@on_tick discovery, multiple @on_tick
    {
        auto m = compile(
            "global g_count : i64 = 0;\n"
            "@entry\nfn main() -> i64 { return 1; }\n"
            "@on_tick\nfn tick_a() -> i64 { g_count = g_count + 1; return g_count; }\n"
            "@on_tick\nfn tick_b() -> i64 { g_count = g_count + 10; return g_count; }\n");
        check(m != nullptr, "compile annotated module");
        auto entry = get_annotated_functions(m->prog, "@entry");
        auto ticks = get_annotated_functions(m->prog, "@on_tick");
        check(entry.size()==1 && entry[0].name=="main" && entry[0].slot==m->slots["main"], "(1) @entry discovered (name+slot)");
        check(ticks.size()==2, "(2) multiple @on_tick discovered (2)");
        // tick loop: call each @on_tick 3 times, verify g_count = 3*(1+10) = 33
        for (int i=0;i<3;++i) for (auto& af : ticks) call_void(*m, af.name);
        check(global_i64(*m,"g_count")==33, "(4) tick loop called both @on_tick fns (g_count==33)");
    }
    // (3) @event("name") arg-matching
    {
        auto m = compile(
            "@event(\"hit\")\nfn on_hit() -> i64 { return 1; }\n"
            "@event(\"heal\")\nfn on_heal() -> i64 { return 2; }\n"
            "@event(\"hit\")\nfn on_hit2() -> i64 { return 3; }\n");
        auto hits = get_event_handlers(m->prog, "hit");
        auto heals = get_event_handlers(m->prog, "heal");
        auto misses = get_event_handlers(m->prog, "nope");
        check(hits.size()==2, "(3) @event(\"hit\") matched 2 handlers");
        check(heals.size()==1 && heals[0].name=="on_heal", "(3) @event(\"heal\") matched 1 (on_heal)");
        check(misses.empty(), "(3) @event(\"nope\") matched 0");
    }
    // (5) @entry return <= 0 = unload (host's stay-loaded check)
    {
        auto m = compile("@entry\nfn main() -> i64 { return 0; }\n");
        auto entry = get_entry_function(m->prog);
        int64_t r = call_void(*m, entry->name);
        bool stay = (r > 0);  // the host's check
        check(entry && entry->name=="main", "(5) @entry found");
        check(!stay, "(5) @entry returned 0 -> host unloads (stay=false)");
    }
    // (6) no annotations -> empty
    {
        auto m = compile("fn main() -> i64 { return 1; }\n");
        check(get_annotated_functions(m->prog,"@on_tick").empty(), "(6) no @on_tick -> empty");
        check(get_entry_function(m->prog)==nullptr, "(6) no @entry -> nullptr");
        check(get_event_handlers(m->prog,"x").empty(), "(6) no @event -> empty");
    }
    // (7) @on_tick with @event coexisting
    {
        auto m = compile(
            "@on_tick\nfn tick() -> i64 { return 1; }\n"
            "@event(\"boom\")\nfn boom() -> i64 { return 2; }\n");
        check(get_annotated_functions(m->prog,"@on_tick").size()==1, "(7) @on_tick + @event coexist (on_tick found)");
        check(get_event_handlers(m->prog,"boom").size()==1, "(7) @on_tick + @event coexist (event found)");
    }

    std::printf("\nv0.6 lifecycle test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
