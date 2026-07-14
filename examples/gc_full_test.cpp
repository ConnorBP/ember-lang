// gc_full_test.cpp - full GC integration: by-reference capture + new/delete.
//
// The focused end-to-end proof for the GC full-integration task. Exercises the
// three task requirements through the FULL engine pipeline (resolve_imports ->
// tokenize -> parse -> sema -> compile_func -> finalize -> call) with the GC
// extension registered + use_gc_env=true:
//
//   (a) By-reference capture: a lambda captures a variable by ref (`fn[&x]`),
//       the variable is modified AFTER capture, the lambda sees the new value.
//       Also: write-through (the lambda mutates the original through the ref)
//       + mixed by-ref/by-value capture + nested by-ref (transitive).
//   (b) new/delete: allocate many objects via gc_new, release via gc_delete,
//       gc_collect reaps the unreachable ones -> heap stays bounded (gc_live
//       stays low), NOT 5000.
//   (c) No leaks: reachable (pinned) objects survive collection (gc_live counts
//       the survivors, not the freed).
//
// This is a thin, scenario-focused companion to gc_integration_test (which has
// the broader host-API + codegen coverage). Same harness shape (compile+run
// one source, return i64 main()).
#include "../src/gc.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/import.hpp"      // resolve_imports
#include "../src/codegen.hpp"
#include "../src/context.hpp"
#include "../src/engine.hpp"
#include "../src/globals.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/binding_builder.hpp"
#include "../src/jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_gc.hpp"

#include "../src/ast.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void gft_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

struct GftModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    ember::context_t ctx{};
};

static bool compile_gft(const std::string& src, GftModule& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return false; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }
    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    m.slot_count = si;
    ember::OpOverloadTable ov;
    ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
    ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
    ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
    ext_gc::register_natives(m.natives);  // __ember_gc_* + gc_new/gc_delete/gc_collect/gc_live
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("  sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return false;
    }
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = &gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = int64_t(m.slot_count);
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=256; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&gft_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=256; ctx.emit_depth_checks=true;
    ctx.use_gc_env = true;  // GC heap env backend (lambdas outlive the frame)
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

static int64_t run_main(GftModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.has_checkpoint=true;
    if (EMBER_SETJMP(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        return int64_t(m.ctx.last_trap); }
    using F0=int64_t(*)();
    int64_t r = reinterpret_cast<F0>(m.main_entry)();
    m.ctx.has_checkpoint=false;
    return r;
}

static void cleanup(GftModule& m) {
    for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
}

static int64_t run_one(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GftModule m;
    if (!compile_gft(src, m)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
    bool trapped = false;
    int64_t r = run_main(m, &trapped);
    if (trapped) {
        std::printf("  FAIL: runtime trap: %s\n", m.ctx.last_error.c_str());
        *ok = false;
    }
    cleanup(m);
    ext_gc::gc_reset();
    return r;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // === (a) By-reference capture ===
    std::printf("=== (a) by-reference capture ===\n");
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let f = fn[&x]() -> i64 { return x; };\n"
            "    x = 99;\n"
            "    return f();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "(a1) by-ref read: compiled + ran (GC env)");
        check(r==99,"(a2) by-ref read: lambda sees post-capture mutation (99)");
    }
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let mut y: i64 = 20;\n"
            "    let f = fn[&x, y]() -> i64 { x = x + 5; return x + y; };\n"
            "    let r = f();\n"
            "    return r + x;\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "(a3) by-ref write-through + mixed capture: compiled + ran");
        check(r==50,"(a4) by-ref write-through mutates original (50)");
    }
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let outer = fn[&x]() -> i64 {\n"
            "        let inner = fn[&x]() -> i64 { return x; };\n"
            "        return inner();\n"
            "    };\n"
            "    x = 42;\n"
            "    return outer();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "(a5) nested by-ref (transitive): compiled + ran");
        check(r==42,"(a6) nested by-ref sees post-capture mutation (42)");
    }

    // === (b) new/delete: GC collects unreachable, heap stays bounded ===
    std::printf("=== (b) new/delete: GC collects unreachable ===\n");
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 5000) {\n"
            "        let p = gc_new(32);\n"
            "        gc_delete(p);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "(b1) 5000 new/delete: compiled + ran (auto-collect mid-run)");
        check(r==0,"(b2) heap bounded; after collect gc_live()==0 (all unreachable reaped)");
    }

    // === (c) No leaks: reachable (pinned) objects survive collection ===
    std::printf("=== (c) no leaks: reachable survive collection ===\n");
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut kept: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 1000) {\n"
            "        let p = gc_new(16);\n"
            "        if (i < 7) { kept = kept + 1; }\n"
            "        else { gc_delete(p); }\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "(c1) keep-7-delete-rest: compiled + ran");
        check(r==7,"(c2) reachable (pinned) survive collect; gc_live()==7 (no leaks)");
    }

    std::printf("\ngc_full_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
