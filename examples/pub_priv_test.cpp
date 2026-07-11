// pub_priv_test - F1 visibility regression (docs/spec/SPEC_AUDIT_2026-07-10.md F1).
//
// F1 makes pub/priv visibility a BUNDLING concern: a bundled `.em` module's
// exported surface is a SUBSET of its functions (the `pub fn` / bare-`fn`
// entries), not "every function." A `priv fn` is serialized (its code/relocs
// occupy a dispatch slot for intra-module calls) but is absent from the `.em`
// name directory, so other modules cannot resolve it cross-module. A cross-
// module `mod::priv_fn()` call is a SEMA error (the callee is not exported),
// not a runtime trap and not silent success.
//
// Backward-compat decision (documented in the F1 commit): a bare `fn` stays
// EXPORTED by default (preserves every existing cross-module test/demo that
// uses bare `fn`); `priv fn` opts OUT of the export surface. This test pins
// that decision: a bare `fn` IS still exported (case D), and `priv fn` is NOT
// (case C).
//
// NON-CIRCULAR: with the F1 fix reverted (no `priv` keyword, no name_table
// filtering, no cross-module sema error), the assertions break as follows:
//   - case A (priv fn called INTRA-module -> success): `priv` is not a keyword
//     -> parse fails -> the success assertion fails.
//   - case B (priv fn absent from the .em name directory): the unfiltered
//     name_table would list `secret_fn` -> the size==2 assertion fails.
//   - case C (cross-module call to priv fn -> SEMA error): the old behavior is
//     a runtime TRAP (or silent success if the slot happens to be valid), not a
//     sema rejection -> the "sema-rejected, not a runtime trap" assertion fails.
//   - case D (bare fn still exported cross-module -> success): this is the
//     backward-compat invariant; it holds before AND after, and anchors the
//     decision (a revert that flipped `fn` to private-by-default would break
//     it).
//
// Pipeline per case mirrors v0_5_live_modules_test: parse -> sema (with the
// ModuleExportTable built from the loaded .em's name directory) -> codegen
// (with registry_base) -> finalize -> call. The .em callee is JIT'd once,
// serialized to a temp .em with a FILTERED name directory (only is_exported
// fns), then loaded via link_em_file.
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "module_registry.hpp"
#include "module_linker.hpp"
#include "em_file.hpp"
#include "em_writer.hpp"
#include "em_loader.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void test_trap_stub(context_t* ctx, int reason, const char* detail) {
    if (ctx) { ctx->last_trap=static_cast<TrapReason>(reason); ctx->last_error=detail?detail:"";
               if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1); }
    std::abort();
}

// A richer run result: distinguishes SEMA failure (the F1 cross-module
// rejection) from a RUNTIME trap from SUCCESS. The F1 assertion is that a
// cross-module call to a private fn is a sema rejection, NOT a runtime trap
// and NOT a silent success.
enum class RunKind { Success, SemaFailed, RuntimeTrap };
struct RunResult { int64_t value = 0; RunKind kind = RunKind::Success; TrapReason reason = TrapReason::None; };

static RunResult run_script(const std::string& src, ModuleRegistry& registry,
                            const ModuleExportTable& module_exports,
                            std::vector<LoadedModule>* keep_alive) {
    RunResult rr;
    std::unordered_set<std::string> seen;
    std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { rr.kind=RunKind::RuntimeTrap; rr.reason=TrapReason::None; return rr; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) { rr.kind=RunKind::SemaFailed; return rr; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { rr.kind=RunKind::SemaFailed; return rr; }
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
    ext_vec::register_natives(natives);ext_quat::register_natives(natives);ext_mat::register_natives(natives);
    ext_string::register_natives(natives);ext_array::register_natives(natives);ext_math::register_natives(natives);
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    // resolve link directives BEFORE sema: load .em files, register, build the
    // export table sema resolves `mod::fn` against (matches ember_cli's order).
    ModuleExportTable local_exports = module_exports;
    keep_alive->reserve(keep_alive->size() + pr.program.links.size());
    for (const auto& ld : pr.program.links) {
        if (ld.is_file) {
            keep_alive->emplace_back();
            std::string lerr;
            bool ok = link_em_file(registry, ld.target.c_str(), ld.alias, keep_alive->back(), &lerr, &natives);
            if (!ok) { rr.kind=RunKind::RuntimeTrap; return rr; }
            uint32_t id = registry.find_by_name(ld.alias);
            add_exports(local_exports, ld.alias, build_em_exports(keep_alive->back(), id));
        }
    }
    // The load-bearing F1 distinction: a cross-module call to a private fn
    // must surface as a SEMA failure here (sema rejects the call before any
    // codegen/run), not a runtime trap and not a silent success.
    auto sr = sema(pr.program,natives,slots,0,&ov,&layouts,&local_exports);
    if (!sr.ok) { rr.kind=RunKind::SemaFailed; return rr; }
    GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8,0); gb.base=int64_t(gbs.data()); g_globals_for_codegen=&gb;
    DispatchTable table(pr.program.funcs.size()); CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base()); ctx.natives=&natives;
    ctx.script_slots=&slots; ctx.structs=&layouts; ctx.registry_base=int64_t(registry.base());
    std::string e; registry.register_module("__main__", table.base(), &e);
    context_t ectx; ectx.budget_remaining=100000000; ectx.max_call_depth=512;
    ctx.trap_stub=(void*)&test_trap_stub; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=512; ctx.emit_depth_checks=true;
    std::vector<CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); table.set(fn.slot,cf.entry); fns.push_back(std::move(cf));}
    auto sit=slots.find("main"); if(sit==slots.end()){rr.kind=RunKind::SemaFailed;return rr;}
    void* entry=table.get(sit->second);
    ectx.has_checkpoint=true;
    if (__builtin_setjmp(ectx.checkpoint)) { rr.kind=RunKind::RuntimeTrap; rr.reason=ectx.last_trap; return rr; }
    using F0=int64_t(*)(); rr.value=reinterpret_cast<F0>(entry)();
    ectx.has_checkpoint=false;
    for(auto&fn:fns) if(fn.exec) free_executable(fn.exec);
    return rr;
}

// Compile a source to a .em file at a temp path, return the path. DIFFERS from
// v0_5's emit_em: this one filters the name directory to only the EXPORTED
// (is_exported==true, i.e. `pub fn`/bare `fn`) functions - the F1 v3 contract
// (the .em name directory IS the export table). A `priv fn` is serialized into
// mod.functions (its code/relocs occupy a dispatch slot for intra-module
// calls) but is omitted from the name directory.
static std::string emit_em_filtered(const std::string& src, const std::string& name) {
    auto lr = tokenize(src, "<t>"); auto pr = parse(std::move(lr.toks));
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
    ext_vec::register_natives(natives);ext_math::register_natives(natives);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    sema(pr.program,natives,slots,0,&ov,&layouts);
    GlobalsBlock gb; std::vector<uint8_t> gbs(0,0); gb.base=0; g_globals_for_codegen=&gb;
    DispatchTable table(pr.program.funcs.size()); CodeGenCtx ctx; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    std::vector<CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); table.set(fn.slot,cf.entry); fns.push_back(std::move(cf));}
    EmModule mod;
    for(size_t i=0;i<fns.size();++i){const auto& cf=fns[i];const auto& decl=pr.program.funcs[i];EmFunctionRecord rec; rec.name=cf.name; rec.slot_index=uint32_t(decl.slot);
        rec.code=cf.bytes;rec.rodata=cf.rodata;rec.non_serializable_reason=cf.non_serializable_reason;
        rec.signature.ret=decl.ret?*decl.ret:Type{};for(const auto&p:decl.params)rec.signature.params.push_back(p.ty?*p.ty:Type{});
        for(const auto& af:cf.abs_fixups){EmReloc r; r.offset=af.code_offset; r.kind=uint8_t(af.kind);r.addend=af.addend;rec.relocs.push_back(r);}
        for(const auto&nf:cf.native_fixups){EmNativeBinding b;b.offset=nf.code_offset;b.name=nf.name;b.signature.ret=nf.ret;b.signature.params=nf.params;rec.native_bindings.push_back(std::move(b));}
        mod.functions.push_back(std::move(rec));}
    mod.globals=gbs; mod.entry_slot=EM_NO_ENTRY;
    // F1: the name directory IS the export table - publish only is_exported fns.
    for(const auto&fn:pr.program.funcs)
        if (fn.is_exported)
            mod.name_table.push_back({fn.name,uint32_t(fn.slot)});
    auto path = std::filesystem::temp_directory_path() / (name + ".em");
    std::string werr;
    if (!write_em_file(mod, path.string().c_str(), &werr)) { std::printf("emit_em FAIL: %s\n", werr.c_str()); return ""; }
    std::string ps = path.string();
    for (auto& c : ps) if (c == '\\') c = '/';
    return ps;
}

int main() {
    std::printf("=== F1 pub/priv visibility (bundling concern) ===\n");

    // (A) A `priv fn` is callable INTRA-module (visibility unchanged within its
    //     own module). The .em has a priv helper + a pub entry that calls it; the
    //     caller links the .em and calls the pub entry cross-module, which in turn
    //     calls the priv helper locally. Must succeed (not trap, not sema-fail).
    {
        ModuleRegistry reg(16); ModuleExportTable mt; std::vector<LoadedModule> ka;
        // Callee: priv fn double_it(x) -> x*2; pub fn call_local(x) -> double_it(x).
        // call_local is the cross-module entry; it calls the priv helper LOCALLY.
        auto em = emit_em_filtered(
            "priv fn double_it(x: i64) -> i64 { return x * 2; }\n"
            "fn call_local(x: i64) -> i64 { return double_it(x); }\n",
            "f1a");
        RunResult r = run_script("link \"" + em + "\" as lib;\nfn main() -> i64 { return lib::call_local(21); }\n", reg, mt, &ka);
        check(r.kind == RunKind::Success && r.value == 42,
              "(A) priv fn callable INTRA-module: lib::call_local(21)==42 (calls priv double_it locally)");
    }

    // (B) The .em name directory (the export table) contains the pub fns but NOT
    //     the priv fn. Load the (A) callee and inspect LoadedModule::name_table.
    {
        ModuleRegistry reg(16); ModuleExportTable mt; std::vector<LoadedModule> ka;
        auto em = emit_em_filtered(
            "priv fn double_it(x: i64) -> i64 { return x * 2; }\n"
            "fn call_local(x: i64) -> i64 { return double_it(x); }\n",
            "f1b");
        LoadedModule lm; std::string lerr;
        bool ok = load_em_file(em.c_str(), lm, &lerr);
        check(ok, "(B) the F1 .em loads (v3 name-directory-as-export-table)");
        if (ok) {
            // name_table must list call_local (exported) but NOT double_it (priv).
            bool has_call_local = false, has_double_it = false;
            for (const auto& [nm, slot] : lm.name_table) {
                if (nm == "call_local") has_call_local = true;
                if (nm == "double_it") has_double_it = true;
            }
            check(has_call_local, "(B) export table contains the pub fn call_local");
            check(!has_double_it, "(B) export table does NOT contain the priv fn double_it (it is hidden)");
            check(lm.name_table.size() == 1, "(B) export table size == 1 (only the pub fn; priv fn omitted)");
        }
    }

    // (C) A cross-module call to a PRIVATE fn is a SEMA error, not a runtime
    //     trap and not a silent success. The callee has a priv fn secret_fn; the
    //     caller links it and calls lib::secret_fn cross-module. Sema must
    //     reject ("targets a function that is not exported by module 'lib'").
    {
        ModuleRegistry reg(16); ModuleExportTable mt; std::vector<LoadedModule> ka;
        auto em = emit_em_filtered("priv fn secret_fn(x: i64) -> i64 { return x + 1; }\n", "f1c");
        RunResult r = run_script("link \"" + em + "\" as lib;\nfn main() -> i64 { return lib::secret_fn(5); }\n", reg, mt, &ka);
        check(r.kind == RunKind::SemaFailed,
              "(C) cross-module call to priv fn -> SEMA error (callee not exported), not a runtime trap");
    }

    // (D) Backward compat: a bare `fn` (no `priv`) is STILL EXPORTED by default.
    //     A cross-module call to a bare `fn` succeeds. This pins the decision
    //     (fn=public, priv=private); a revert that flipped fn to private-by-
    //     default would break this.
    {
        ModuleRegistry reg(16); ModuleExportTable mt; std::vector<LoadedModule> ka;
        auto em = emit_em_filtered("fn plain_pub(x: i64) -> i64 { return x * 3; }\n", "f1d");
        RunResult r = run_script("link \"" + em + "\" as lib;\nfn main() -> i64 { return lib::plain_pub(14); }\n", reg, mt, &ka);
        check(r.kind == RunKind::Success && r.value == 42,
              "(D) bare fn stays EXPORTED by default: lib::plain_pub(14)==42 (backward compat)");
    }

    std::printf("\nF1 pub/priv visibility test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
