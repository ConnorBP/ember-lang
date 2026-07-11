// cross_module_handles_test - v1.0 Tier 2 cross-module function handles
// (`&mod::fn` / `mod::fn` returning a handle that works across module
// boundaries). Companion to function_refs_test (intra-module `&fn`).
//
// PROVES (the five cases the task names):
//   A. `&mod::fn` creates a cross-module handle; calling it dispatches to the
//      correct module's dispatch table (`&lib::double(21)` -> 42). The handle
//      is `(1<<63)|(module_id<<32)|slot`; bit 63 is the cross-module flag.
//   B. A cross-module handle passed to a `fn`-typed parameter and called from
//      INSIDE that function dispatches cross-module (`apply(&lib::double, 21)`
//      -> 42). The bare `fn` param accepts a cross-module handle (the "any fn"
//      escape hatch); the runtime guard routes on bit 63, not the type.
//   C. An out-of-range cross-module handle (module_id past the records table)
//      TRAPS via BadCallTarget (longjmp to the checkpoint), NOT a raw
//      call-of-garbage crash. Forged via a test native returning a `fn`-typed
//      value (the one trusted runtime source, mirroring function_refs_test C1).
//   D. An unregistered cross-module handle (valid module_id, but a slot that is
//      out of range for that module / bit clear in its allowlist) TRAPS via
//      BadCallTarget.
//   E. Cross-module handle vs intra-module handle are DISTINCT spaces: a
//      cross-module handle is NOT assignable to an intra-module recorded
//      `fn(Sig)->Ret`-typed variable (sema error), and vice versa. (A bare `fn`
//      still accepts either — that is case B, not a contradiction.)
//
// Harness: two JIT modules compiled through the real pipeline (tokenize -> parse
// -> slot-assign -> sema with a ModuleExportTable -> codegen with registry_base
// + the handle-records base -> finalize -> call under the safe-trap surface).
// The target module ("lib") registers its dispatch table AND its fn allowlist +
// slot_count so the cross-module guard can validate handles into it. Mirrors
// v0_5_live_modules_test's registry/export wiring + function_refs_test's
// allowlist + trap checkpoint harness.
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"
#include "globals.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"
#include "module_registry.hpp"
#include "module_linker.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

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
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void cmh_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// A compiled JIT module: its functions, dispatch table, slot map, fn allowlist
// (owned, kept alive for the registry's lifetime), and the trap context the
// baked-ptr code expects at the same address at run time.
struct JITModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;   // owned; keep alive (registry holds a ptr)
    int slot_count = 0;
    uint32_t module_id = UINT32_MAX;  // assigned on registration
    ember::context_t ctx{};
};

// Compile a module from source. `exports` (if non-null) is the ModuleExportTable
// sema resolves `&mod::fn` / `mod::fn()` against. `registry` (if non-null) is
// used for registry_base + (for the target) registering with its allowlist.
// `register_with_handles` = register this module's dispatch table + allowlist +
// slot_count into the registry under `name` (so other modules can take
// `&name::fn` handles into it). Returns false on any compile error (prints it).
static bool compile_module(const std::string& src, const std::string& name,
                           JITModule& m,
                           const ModuleExportTable* exports,
                           ModuleRegistry* registry,
                           bool register_with_handles) {
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
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts,exports);
    if(!sr.ok){ std::printf("FAIL: sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str()); return false; }

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = &gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    // build + wire the fn allowlist (this module's own guard for intra handles)
    m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = int64_t(m.slot_count);
    // cross-module: registry_base (kind-2 calls) + the handle-records table
    if (registry) {
        ctx.registry_base = int64_t(registry->base());
        ctx.module_handle_records_base = int64_t(registry->handle_records_base());
        ctx.module_handle_records_count = int64_t(registry->handle_records_count());
    }
    // safe-trap surface (baked-ptr path, default). trap_ctx address baked into
    // JIT'd code -> run MUST use m.ctx (stable module lifetime).
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=64; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&cmh_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    m.main_entry = (sit!=m.slots.end()) ? m.table.get(sit->second) : nullptr;
    // (a target module like 'lib' has no main; that's fine — it is only called
    //  cross-module. An importer without main is a test-harness error, but we
    //  don't hard-fail here so registration still happens for target modules.)
    // register into the registry (with or without the allowlist surface)
    if (registry) {
        std::string e;
        if (register_with_handles) {
            m.module_id = registry->register_module(name, m.table.base(), &e,
                                                    m.allowlist.data(), int64_t(m.slot_count));
        } else {
            m.module_id = registry->register_module(name, m.table.base(), &e);
        }
        if (m.module_id == UINT32_MAX) { std::printf("FAIL: register_module: %s\n", e.c_str()); return false; }
    }
    return true;
}

// Build the ModuleExportTable entry for a registered JIT module (one export per
// non-private fn, carrying slot + module_id + signature). Mirrors
// module_linker's build_jit_exports but inlined here so the test is self-contained.
static void add_module_exports(ModuleExportTable& table, const std::string& src,
                               const std::string& alias, uint32_t module_id) {
    // parse + slot-assign (the host assigns dense slots 0,1,2...; parse alone
    // leaves fn.slot = -1). Then build one export per non-private fn carrying
    // slot + module_id + signature. Mirrors module_linker's build_jit_exports.
    auto lr = tokenize(src, "<x>"); auto pr = parse(std::move(lr.toks));
    int si = 0;
    for (auto& fn : pr.program.funcs) { fn.slot = si++; }  // dense slot assignment
    std::vector<ModuleExport> exps;
    for (const auto& fn : pr.program.funcs) {
        if (!fn.is_exported) continue;
        ModuleExport exp;
        exp.fn_name = fn.name; exp.module_id = module_id; exp.slot = fn.slot;
        exp.ret = fn.ret ? *fn.ret : Type{};
        for (const auto& p : fn.params) exp.params.push_back(p.ty ? *p.ty : Type{});
        exps.push_back(std::move(exp));
    }
    table[alias] = std::move(exps);
}

// Run a module's main under its own context (the baked trap_ctx). Returns the
// i64 result or *trapped=true (with m.ctx.last_trap set).
static int64_t run_main(JITModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.has_checkpoint=true;
    if (__builtin_setjmp(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        return int64_t(m.ctx.last_trap); }
    using F0=int64_t(*)();
    int64_t r = reinterpret_cast<F0>(m.main_entry)();
    m.ctx.has_checkpoint=false;
    return r;
}

// A test-only forge native: returns a `fn`-typed value (sema accepts assigning
// it to a fn-typed var — the one trusted runtime source, mirroring
// function_refs_test's forge_fn). Its C impl returns the raw arg so a script can
// inject a FORGED cross-module handle the guard must catch. Overloads: one takes
// the full 64-bit handle directly (forge_xhandle), one takes (mod_id, slot) and
// assembles the handle (forge_xhandle_parts) for convenience.
static int64_t g_forge_raw = 0;
extern "C" int64_t n_forge_xhandle(int64_t raw) { g_forge_raw = raw; return raw; }
extern "C" int64_t n_forge_xhandle_parts(int64_t mod_id, int64_t slot) {
    uint64_t h = (uint64_t(1) << 63) | (uint64_t(mod_id) << 32) | uint64_t(uint32_t(slot));
    g_forge_raw = int64_t(h);
    return int64_t(h);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== v1.0 Tier 2 cross-module function handles ===\n");

    // The target module source (lib). double + add1, both exported.
    const std::string lib_src =
        "fn double(x: i64) -> i64 { return x * 2; }\n"
        "fn add1(x: i64) -> i64 { return x + 1; }\n";

    // ---- A. cross-module handle creation + call ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        // register lib FIRST (so main's sema can resolve &lib::double). lib
        // publishes its allowlist so the cross-module guard can validate.
        JITModule lib;
        bool ok = compile_module(lib_src, "lib", lib, nullptr, &reg, /*with_handles*/true);
        check(ok, "A: compile + register target module 'lib' (with allowlist)");
        if (ok) {
            add_module_exports(exports, lib_src, "lib", lib.module_id);
            // now compile main, which takes &lib::double and calls it
            JITModule main;
            ok = compile_module(
                "fn main() -> i64 { let h = &lib::double; return h(21); }\n",
                "main", main, &exports, &reg, /*with_handles*/false);
            check(ok, "A: compile importer `let h = &lib::double; return h(21);`");
            if (ok) { bool t=false; int64_t r=run_main(main,&t);
                check(!t && r==42, "A: &lib::double(21) dispatches cross-module, returns 42"); }
            for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        }
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- A2. two-arg cross-module handle call (Win64 rcx/rdx) ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module("fn addsub(a: i64, b: i64) -> i64 { return a*100 + b; }\n", "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, "fn addsub(a: i64, b: i64) -> i64 { return a*100 + b; }\n", "lib", lib.module_id);
        JITModule main;
        bool ok = compile_module(
            "fn main() -> i64 { let h = &lib::addsub; return h(3, 4); }\n",
            "main", main, &exports, &reg, false);
        check(ok, "A2: compile two-arg cross-module handle call");
        if (ok) { bool t=false; int64_t r=run_main(main,&t);
            check(!t && r==304, "A2: &lib::addsub(3,4) -> 304 (3*100+4)"); }
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- B. cross-module handle passed to a fn + called ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        // apply takes a bare `fn` (accepts a cross-module handle) + an i64,
        // and calls the handle. main passes &lib::double.
        JITModule main;
        bool ok = compile_module(
            "fn apply(h: fn, x: i64) -> i64 { return h(x); }\n"
            "fn main() -> i64 { return apply(&lib::double, 21); }\n",
            "main", main, &exports, &reg, false);
        check(ok, "B: compile `fn apply(h: fn, x: i64) -> i64 { return h(x); }` + pass &lib::double");
        if (ok) { bool t=false; int64_t r=run_main(main,&t);
            check(!t && r==42, "B: apply(&lib::double, 21) -> 42 (handle called inside apply, cross-module)"); }
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- B2. cross-module handle stored in a bare-`fn` var, then called ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        JITModule main;
        bool ok = compile_module(
            "fn main() -> i64 { let h: fn = &lib::double; return h(21); }\n",
            "main", main, &exports, &reg, false);
        check(ok, "B2: compile `let h: fn = &lib::double; return h(21);` (bare fn var)");
        if (ok) { bool t=false; int64_t r=run_main(main,&t);
            check(!t && r==42, "B2: bare-`fn`-typed cross-module handle call returns 42"); }
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- B3. a cross-module handle AND an intra-module handle both work, side by side ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        JITModule main;
        // &lib::double (cross) + &sq (intra), call both, sum results.
        bool ok = compile_module(
            "fn sq(x: i64) -> i64 { return x * x; }\n"
            "fn main() -> i64 { let c = &lib::double; let l = &sq; return c(10) + l(5); }\n",
            "main", main, &exports, &reg, false);
        check(ok, "B3: compile a module using BOTH a cross-module and an intra-module handle");
        if (ok) { bool t=false; int64_t r=run_main(main,&t);
            check(!t && r==45, "B3: &lib::double(10) + &sq(5) = 20 + 25 = 45"); }
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- C. out-of-range cross-module handle (traps) ----
    // Forge a handle whose module_id is past the records table count. The guard
    // range-checks mod_id < records_count and traps (BadCallTarget), not a crash.
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        // lib is module_id 0 (first registered). main will be module_id 1 after
        // it registers, but we forge mod_id=200 (way past count=1..2). Use the
        // parts-forge native: forge_xhandle_parts(200, 0) -> (1<<63)|(200<<32)|0.
        JITModule main;
        std::string src =
            "fn main() -> i64 { let h: fn = forge_xhandle_parts(200, 0); return h(1); }\n";
        // inject the forge native (declared `fn` return so sema accepts the assign)
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { resolved = src; }
        auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
        int si=0; for(auto&fn:pr.program.funcs){main.slots[fn.name]=si++;fn.slot=main.slots[fn.name];}
        main.slot_count = si;
        ember::OpOverloadTable ov;
        ext_vec::register_natives(main.natives);ext_quat::register_natives(main.natives);
        ext_mat::register_natives(main.natives);ext_string::register_natives(main.natives);
        ext_array::register_natives(main.natives);ext_math::register_natives(main.natives);
        ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
        ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
        { NativeSig ns; ns.fn_ptr=(void*)&n_forge_xhandle_parts;
          ns.ret=type_i64(); ns.ret.is_fn_handle=true;
          ns.params.push_back(type_i64()); ns.params.push_back(type_i64());
          main.natives["forge_xhandle_parts"]=std::move(ns); }
        auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
        auto sr=sema(pr.program,main.natives,main.slots,0,&ov,&layouts,&exports);
        check(sr.ok, "C: sema accepts `let h: fn = forge_xhandle_parts(200, 0)` (trusted native returns fn)");
        if (sr.ok) {
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
            ember::g_globals_for_codegen=&gb;
            main.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(main.table.base());
            ctx.natives=&main.natives; ctx.script_slots=&main.slots; ctx.structs=&layouts;
            main.allowlist = build_fn_allowlist(main.slots, main.slot_count);
            ctx.fn_allowlist_base = int64_t(main.allowlist.data());
            ctx.fn_slot_count = int64_t(main.slot_count);
            ctx.registry_base = int64_t(reg.base());
            ctx.module_handle_records_base = int64_t(reg.handle_records_base());
            ctx.module_handle_records_count = int64_t(reg.handle_records_count());
            ember::context_t ectx; ectx.budget_remaining=2'000'000'000LL; ectx.max_call_depth=64;
            ctx.trap_stub=(void*)&cmh_trap; ctx.trap_ctx=&ectx;
            ctx.emit_budget_checks=true; ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); main.table.set(fn.slot,cf.entry); main.fns.push_back(std::move(cf));}
            auto sit=main.slots.find("main"); void* entry = main.table.get(sit->second);
            ectx.has_checkpoint=true; bool trapped=false;
            if (__builtin_setjmp(ectx.checkpoint)) { trapped=true; ectx.has_checkpoint=false; }
            else { using F0=int64_t(*)(); reinterpret_cast<F0>(entry)(); ectx.has_checkpoint=false; }
            check(trapped, "C: out-of-range cross-module handle (mod_id=200) traps via the guard, not a raw call");
            check(ectx.last_trap == TrapReason::BadCallTarget, "C: trap reason is BadCallTarget");
        }
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- D. unregistered cross-module handle (valid mod_id, bad slot) (traps) ----
    // lib has 2 fns (slots 0,1). Forge a handle into lib with slot=999 (in-range
    // mod_id but slot out of range for lib's slot_count=2). The guard's slot <
    // slot_count check traps (BadCallTarget).
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        const uint32_t lib_id = lib.module_id;  // 0
        JITModule main;
        std::string src =
            "fn main() -> i64 { let h: fn = forge_xhandle_parts(LIBID, 999); return h(1); }\n";
        // substitute LIBID with lib's actual module_id
        size_t pos = src.find("LIBID");
        src.replace(pos, 5, std::to_string(lib_id));
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { resolved = src; }
        auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
        int si=0; for(auto&fn:pr.program.funcs){main.slots[fn.name]=si++;fn.slot=main.slots[fn.name];}
        main.slot_count = si;
        ember::OpOverloadTable ov;
        ext_vec::register_natives(main.natives);ext_quat::register_natives(main.natives);
        ext_mat::register_natives(main.natives);ext_string::register_natives(main.natives);
        ext_array::register_natives(main.natives);ext_math::register_natives(main.natives);
        ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
        ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
        { NativeSig ns; ns.fn_ptr=(void*)&n_forge_xhandle_parts;
          ns.ret=type_i64(); ns.ret.is_fn_handle=true;
          ns.params.push_back(type_i64()); ns.params.push_back(type_i64());
          main.natives["forge_xhandle_parts"]=std::move(ns); }
        auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
        auto sr=sema(pr.program,main.natives,main.slots,0,&ov,&layouts,&exports);
        check(sr.ok, "D: sema accepts the unregistered-slot forge setup");
        if (sr.ok) {
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
            ember::g_globals_for_codegen=&gb;
            main.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(main.table.base());
            ctx.natives=&main.natives; ctx.script_slots=&main.slots; ctx.structs=&layouts;
            main.allowlist = build_fn_allowlist(main.slots, main.slot_count);
            ctx.fn_allowlist_base = int64_t(main.allowlist.data());
            ctx.fn_slot_count = int64_t(main.slot_count);
            ctx.registry_base = int64_t(reg.base());
            ctx.module_handle_records_base = int64_t(reg.handle_records_base());
            ctx.module_handle_records_count = int64_t(reg.handle_records_count());
            ember::context_t ectx; ectx.budget_remaining=2'000'000'000LL; ectx.max_call_depth=64;
            ctx.trap_stub=(void*)&cmh_trap; ctx.trap_ctx=&ectx;
            ctx.emit_budget_checks=true; ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); main.table.set(fn.slot,cf.entry); main.fns.push_back(std::move(cf));}
            auto sit=main.slots.find("main"); void* entry = main.table.get(sit->second);
            ectx.has_checkpoint=true; bool trapped=false;
            if (__builtin_setjmp(ectx.checkpoint)) { trapped=true; ectx.has_checkpoint=false; }
            else { using F0=int64_t(*)(); reinterpret_cast<F0>(entry)(); ectx.has_checkpoint=false; }
            check(trapped, "D: in-mod_id-but-out-of-slot handle (slot=999, lib has 2) traps (slot range check)");
            check(ectx.last_trap == TrapReason::BadCallTarget, "D: trap reason BadCallTarget");
        }
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- D2. unregistered cross-module handle: valid mod_id, in-range slot, bit CLEAR ----
    // Forge a handle into lib with slot=5, but inflate lib's published slot_count
    // to 8 so slot 5 is IN RANGE yet its allowlist bit is clear (lib only has
    // slots 0,1 registered). The guard's bt check traps (BadCallTarget).
    // We do this by re-registering lib with an inflated allowlist (8 slots, bits
    // 0,1 set) + slot_count=8, directly into the registry.
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        // compile lib WITHOUT registering through compile_module (we register
        // manually with the inflated allowlist)
        JITModule lib;
        {
            std::unordered_set<std::string> seen; std::string resolved;
            try { resolved = resolve_imports(lib_src, "./", seen); } catch (...) {}
            auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
            int si=0; for(auto&fn:pr.program.funcs){lib.slots[fn.name]=si++;fn.slot=lib.slots[fn.name];}
            lib.slot_count = si;
            std::unordered_map<std::string,NativeSig> nat; OpOverloadTable ov;
            ext_vec::register_natives(nat);ext_quat::register_natives(nat);ext_mat::register_natives(nat);
            ext_string::register_natives(nat);ext_array::register_natives(nat);ext_math::register_natives(nat);
            ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
            ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
            auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
            sema(pr.program,nat,lib.slots,0,&ov,&layouts);
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=0; g_globals_for_codegen=&gb;
            lib.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.dispatch_base=int64_t(lib.table.base());
            ctx.natives=&nat; ctx.script_slots=&lib.slots; ctx.structs=&layouts;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); lib.table.set(fn.slot,cf.entry); lib.fns.push_back(std::move(cf));}
        }
        // register lib with an INFLATED allowlist: 8 slots, only bits 0,1 set.
        lib.allowlist = build_fn_allowlist(lib.slots, 8);  // 8 slots, bits 0,1 set; 2..7 clear
        std::string e;
        lib.module_id = reg.register_module("lib", lib.table.base(), &e,
                                            lib.allowlist.data(), /*slot_count*/ int64_t(8));
        check(lib.module_id != UINT32_MAX, "D2: register lib with inflated slot_count=8 (bits 0,1 set)");
        add_module_exports(exports, lib_src, "lib", lib.module_id);
        const uint32_t lib_id = lib.module_id;
        // forge handle into lib, slot=5 (in range [0,8), bit CLEAR)
        JITModule main;
        std::string src =
            "fn main() -> i64 { let h: fn = forge_xhandle_parts(LIBID, 5); return h(1); }\n";
        size_t pos = src.find("LIBID"); src.replace(pos, 5, std::to_string(lib_id));
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { resolved = src; }
        auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
        int si=0; for(auto&fn:pr.program.funcs){main.slots[fn.name]=si++;fn.slot=main.slots[fn.name];}
        main.slot_count = si;
        ember::OpOverloadTable ov;
        ext_vec::register_natives(main.natives);ext_quat::register_natives(main.natives);
        ext_mat::register_natives(main.natives);ext_string::register_natives(main.natives);
        ext_array::register_natives(main.natives);ext_math::register_natives(main.natives);
        ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
        ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
        { NativeSig ns; ns.fn_ptr=(void*)&n_forge_xhandle_parts;
          ns.ret=type_i64(); ns.ret.is_fn_handle=true;
          ns.params.push_back(type_i64()); ns.params.push_back(type_i64());
          main.natives["forge_xhandle_parts"]=std::move(ns); }
        auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
        auto sr=sema(pr.program,main.natives,main.slots,0,&ov,&layouts,&exports);
        check(sr.ok, "D2: sema accepts the in-range-unregistered forge setup");
        if (sr.ok) {
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
            g_globals_for_codegen=&gb;
            main.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(main.table.base());
            ctx.natives=&main.natives; ctx.script_slots=&main.slots; ctx.structs=&layouts;
            main.allowlist = build_fn_allowlist(main.slots, main.slot_count);
            ctx.fn_allowlist_base = int64_t(main.allowlist.data());
            ctx.fn_slot_count = int64_t(main.slot_count);
            ctx.registry_base = int64_t(reg.base());
            ctx.module_handle_records_base = int64_t(reg.handle_records_base());
            ctx.module_handle_records_count = int64_t(reg.handle_records_count());
            ember::context_t ectx; ectx.budget_remaining=2'000'000'000LL; ectx.max_call_depth=64;
            ctx.trap_stub=(void*)&cmh_trap; ctx.trap_ctx=&ectx;
            ctx.emit_budget_checks=true; ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); main.table.set(fn.slot,cf.entry); main.fns.push_back(std::move(cf));}
            auto sit=main.slots.find("main"); void* entry = main.table.get(sit->second);
            ectx.has_checkpoint=true; bool trapped=false;
            if (__builtin_setjmp(ectx.checkpoint)) { trapped=true; ectx.has_checkpoint=false; }
            else { using F0=int64_t(*)(); reinterpret_cast<F0>(entry)(); ectx.has_checkpoint=false; }
            check(trapped, "D2: in-range-but-unregistered handle (slot 5, bit clear) traps (bt fires)");
            check(ectx.last_trap == TrapReason::BadCallTarget, "D2: trap reason BadCallTarget");
        }
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- E. cross-module handle vs intra-module handle (distinct spaces) ----
    // A cross-module handle is NOT assignable to an intra-module recorded
    // `fn(Sig)->Ret`-typed variable (sema error). And the reverse. A bare `fn`
    // (no recorded sig) DOES accept either (case B) — that is not a contradiction.
    auto sema_bad = [](const char* desc, const std::string& main_src,
                       const std::string& lib_src_local)->bool{
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule lib;
        compile_module(lib_src_local, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_src_local, "lib", lib.module_id);
        JITModule main;
        bool ok = compile_module(main_src, "main", main, &exports, &reg, false);
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        bool rejected = !ok;  // compile_module prints the sema errors + returns false
        check(rejected, desc);
        return rejected;
    };
    // E1: assign a cross-module handle to an intra-module recorded fn type -> error
    sema_bad("E1: `let h: fn(i64)->i64 = &lib::double;` rejected (cross-module handle into intra-module fn type)",
        "fn main() -> i64 { let h: fn(i64)->i64 = &lib::double; return h(1); }\n", lib_src);
    // E2: assign an intra-module handle to a CROSS-MODULE recorded fn type.
    // There's no source syntax for a cross-module fn type annotation, so test the
    // reverse direction differently: a fn param typed `fn(i64)->i64` (intra)
    // rejects a cross-module handle argument.
    sema_bad("E2: passing &lib::double to a `fn(i64)->i64` param rejected (cross handle != intra fn type)",
        "fn take(h: fn(i64)->i64) -> i64 { return h(1); }\n"
        "fn main() -> i64 { return take(&lib::double); }\n", lib_src);
    // E3: &unlinked_mod::fn (module not linked) is a hard sema error
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;  // empty: no modules linked
        JITModule main;
        bool ok = compile_module(
            "fn main() -> i64 { let h = &nope::foo; return h(1); }\n",
            "main", main, &exports, &reg, false);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        check(!ok, "E3: `&nope::foo` (unlinked module) rejected at sema (not a deferred trap)");
    }
    // E4: &lib::private_fn (module present, fn not exported) is a hard sema error
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        const std::string lib_priv_src =
            "priv fn helper(x: i64) -> i64 { return x; }\n"
            "fn pub(x: i64) -> i64 { return x + 1; }\n";
        JITModule lib;
        compile_module(lib_priv_src, "lib", lib, nullptr, &reg, true);
        add_module_exports(exports, lib_priv_src, "lib", lib.module_id);
        JITModule main;
        bool ok = compile_module(
            "fn main() -> i64 { let h = &lib::helper; return h(1); }\n",
            "main", main, &exports, &reg, false);
        for(auto&fn:lib.fns)if(fn.exec)free_executable(fn.exec);
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
        check(!ok, "E4: `&lib::helper` (private fn) rejected at sema (F1: not exported)");
    }

    // ---- E5. positive control: an intra-module handle still works (no regression) ----
    {
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        JITModule main;
        bool ok = compile_module(
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main() -> i64 { let h: fn(i64)->i64 = &id; return h(42); }\n",
            "main", main, &exports, &reg, false);
        check(ok, "E5: intra-module `let h: fn(i64)->i64 = &id;` still compiles (no regression)");
        if (ok) { bool t=false; int64_t r=run_main(main,&t);
            check(!t && r==42, "E5: intra-module handle call returns 42 (regression check)"); }
        for(auto&fn:main.fns)if(fn.exec)free_executable(fn.exec);
    }

    std::printf("\nv1.0 Tier 2 cross-module handles test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
