// em_roundtrip_test - `.em` pre-compile round-trip proof (RESTRUCTURE_PLAN.md Section 5).
//
// Exercises the REAL parser→sema→codegen→.em→load path end to end (better than
// the standalone tree's hand-built fib test, which had no parser). Compiles a
// small `.ember` source string twice - once to JIT (the ground truth), once
// serialized to a temp `.em` then loaded back - and asserts the loaded module
// produces the SAME result as the JIT'd one for the same inputs.
//
// This is the additive, in-place port of the standalone `.em` feature into
// prism's live ember (RESTRUCTURE_PLAN.md step 1). It links only the ember +
// ember_frontend libs - no prism natives, no prism_script_host - because the
// test function (`double_it`) uses no natives/globals/structs, so an empty
// native table + empty struct-layout table is a valid sema input. That keeps
// the test free of the prism panel-API link breakage that currently stops
// ember_cli from linking (GetPanelRegistry etc. - unrelated to the .em port).
//
// What it proves:
//   1. mov_reg_imm64_external + AbsFixup capture works against the real
//      codegen (the dispatch-table base bake became a reloc; the byte output
//      is byte-identical to before, so the JIT result is unchanged).
//   2. em_writer serializes the per-function code + the AbsFixups as relocs
//      (EmReloc {offset, kind=DispatchTableBase}).
//   3. em_loader alloc_executable's the bytes back, patches the reloc at the
//      load-time dispatch-table address, stamps the slot, and the loaded
//      function runs identically to the JIT'd one.
//   4. The whole pipeline is the SAME one prism_script_host uses, so a `.em`
//      loaded module is call-compatible with a JIT'd module.

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/module_linker.hpp"
#include "../src/jit_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

// Win64 i64(i64) call: the compiled functions take one i64 arg (rcx) and
// return i64 (rax), matching double_it(x: i64) -> i64.
static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

// Build the EmModule from a set of JIT'd CompiledFns + the slot table, the
// way a host serializer would (BUNDLING_AND_EM_MODULES.md Section 2.3). Each
// function's `relocs` are filled from `CompiledFn::abs_fixups` (captured by
// compile_func from the emitter's mov_reg_imm64_external records).
static ember::EmModule build_em_module(
        const std::vector<ember::CompiledFn>& fns,
        const ember::Program& program,
        const std::vector<uint8_t>& globals_bytes,
        uint32_t entry_slot) {
    ember::EmModule mod;
    mod.functions.reserve(fns.size());
    for (size_t i=0;i<fns.size();++i) {
        const auto& cf=fns[i];
        const auto& decl=program.funcs[i];
        ember::EmFunctionRecord rec;
        rec.name = cf.name;
        rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes; // post-resolve_fixups, pre-finalize (raw bytes)
        rec.rodata = cf.rodata;
        rec.non_serializable_reason = cf.non_serializable_reason;
        rec.signature.ret = decl.ret ? *decl.ret : ember::Type{};
        for(const auto& p:decl.params)rec.signature.params.push_back(p.ty?*p.ty:ember::Type{});
        for(const auto& nf:cf.native_fixups){ember::EmNativeBinding b;b.offset=nf.code_offset;b.name=nf.name;b.signature.ret=nf.ret;b.signature.params=nf.params;rec.native_bindings.push_back(std::move(b));}
        // relocs: one per AbsFixup, mapping Kind -> EmReloc::Kind 1:1.
        rec.relocs.reserve(cf.abs_fixups.size());
        for (const auto& af : cf.abs_fixups) {
            ember::EmReloc r;
            r.offset = af.code_offset;
            r.kind = static_cast<uint8_t>(af.kind);
            r.addend = af.addend;
            rec.relocs.push_back(r);
        }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = globals_bytes;
    mod.entry_slot = entry_slot;
    // name table = name -> slot (for ember_call by name).
    mod.name_table.reserve(program.funcs.size());
    for (const auto& fn : program.funcs)
        mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
    return mod;
}

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- source: a real parsed function (no natives/globals/structs) ----
    // double_it(x) = x * 2 ; fib(n) = n<=1 ? n : fib(n-1)+fib(n-2)  (recursive,
    //   exercises the dispatch-table reloc path with a real self-call).
    const std::string src =
        "fn double_it(x: i64) -> i64 { return x * 2; }\n"
        "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\n";

    // ---- lex ----
    auto lr = tokenize(src, "<em_roundtrip>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }
    if (pr.program.funcs.size() != 2) {
        std::printf("FAIL: expected 2 funcs, got %zu\n", pr.program.funcs.size());
        return 1;
    }

    // ---- slot assignment (mirror prism_script_host.cpp) ----
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }

    // ---- sema (empty natives/overloads/structs - double_it+fib need none) ----
    std::unordered_map<std::string, NativeSig> natives; // empty
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }

    // ---- globals block (empty - no globals declared) ----
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store(0);
    gb.base = int64_t(gb_store.data());
    g_globals_for_codegen = &gb;

    // ---- dispatch table + codegen ctx ----
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;
    // str_decrypt_fn stays null: no encrypted string literals in this source.

    // ---- compile + finalize each function ----
    std::vector<CompiledFn> fns;
    for (auto& fn : pr.program.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        if (!finalize(cf)) {
            std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str());
            for (auto& done : fns) if (done.exec) free_executable(done.exec);
            return 1;
        }
        table.set(fn.slot, cf.entry);
        fns.push_back(std::move(cf));
    }

    // ---- sanity: confirm the dispatch-table reloc was captured ----
    // double_it has no calls -> 0 relocs. fib has self-calls -> >=2 relocs.
    auto& cf_double = fns[0];
    auto& cf_fib     = fns[1];
    size_t double_relocs = cf_double.abs_fixups.size();
    size_t fib_relocs    = cf_fib.abs_fixups.size();
    std::printf("reloc capture: double_it=%zu  fib=%zu\n", double_relocs, fib_relocs);
    bool reloc_ok = (double_relocs == 0) && (fib_relocs >= 2);
    std::printf("[1] reloc capture: %s\n", passfail(reloc_ok));
    if (!reloc_ok) failures++;

    // ---- JIT ground-truth results ----
    int64_t jit_double_7  = call_i64_i64(table.get(slots["double_it"]), 7);
    int64_t jit_double_21 = call_i64_i64(table.get(slots["double_it"]), 21);
    int64_t jit_fib_10    = call_i64_i64(table.get(slots["fib"]), 10);
    int64_t jit_fib_15   = call_i64_i64(table.get(slots["fib"]), 15);
    std::printf("JIT:  double_it(7)=%lld  double_it(21)=%lld  fib(10)=%lld  fib(15)=%lld\n",
                (long long)jit_double_7, (long long)jit_double_21,
                (long long)jit_fib_10, (long long)jit_fib_15);
    bool jit_ok = (jit_double_7 == 14) && (jit_double_21 == 42) &&
                  (jit_fib_10 == 55) && (jit_fib_15 == 610);
    std::printf("[2] JIT ground-truth: %s\n", passfail(jit_ok));
    if (!jit_ok) failures++;

    // ---- serialize to a temp .em ----
    auto mod = build_em_module(fns, pr.program, gb_store,
                               uint32_t(slots["double_it"])); // entry = double_it
    // A second directory name for the same slot must retain the slot's v2
    // signature (name-keyed metadata would incorrectly make this unknown).
    mod.name_table.emplace_back("double_alias",uint32_t(slots["double_it"]));
    // sanity: the module's relocs match the captured AbsFixups.
    bool mod_relocs_ok = (mod.functions[1].relocs.size() == fib_relocs) &&
                        (mod.functions[0].relocs.size() == double_relocs);
    std::printf("[3] EmModule reloc fill: %s\n", passfail(mod_relocs_ok));
    if (!mod_relocs_ok) failures++;

    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "em_roundtrip_test.em";
    std::string werr;
    if (!write_em_file(mod, tmp.string().c_str(), &werr)) {
        std::printf("FAIL: write_em_file: %s\n", werr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", tmp.string().c_str(),
                std::filesystem::file_size(tmp));

    // ---- load it back ----
    LoadedModule lm;
    std::string lerr;
    if (!load_em_file(tmp.string().c_str(), lm, &lerr, nullptr, &natives)) {
        std::printf("FAIL: load_em_file: %s\n", lerr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    std::printf("loaded: %zu funcs, dispatch=%zu slots, entry_slot=%u\n",
                lm.pages.size(), lm.dispatch.size(), lm.entry_slot);

    // v2 carries canonical signatures and must reject an ABI-mismatched call.
    // behavior: exports are marked unknown_sig and sema treats calls as
    // ABI-trusted (standalone args are checked, but arity/return cannot be).
    // Do not execute the deliberately wrong-arity probe.
    auto trusted_exports = build_em_exports(lm, 7);
    bool unknown_sig_ok = trusted_exports.size() == 3;
    for (const auto& exp : trusted_exports)
        unknown_sig_ok &= !exp.unknown_sig && exp.ret.prim==Prim::I64 &&
                          exp.params.size()==1 && exp.params[0].prim==Prim::I64;
    ModuleExportTable mt;
    add_exports(mt, "trusted", trusted_exports);
    auto ulr = tokenize("link \"trusted\"; fn main() -> i64 { return trusted::double_it(1.0f); }", "<known-sig>");
    bool trusted_sema_ok = ulr.ok;
    if (trusted_sema_ok) {
        auto upr = parse(std::move(ulr.toks));
        trusted_sema_ok = upr.ok;
        if (trusted_sema_ok) {
            std::unordered_map<std::string, int> uslots{{"main", 0}};
            upr.program.funcs[0].slot = 0;
            auto ulayouts = build_struct_layouts(upr.program);
            trusted_sema_ok = !sema(upr.program, natives, uslots, 0, nullptr,
                                    &ulayouts, &mt).ok;
        }
    }
    bool h14_contract_ok = unknown_sig_ok && trusted_sema_ok;
    std::printf("[4] v2 signature mismatch rejected before execution: %s\n", passfail(h14_contract_ok));
    if (!h14_contract_ok) failures++;

    // Mutating either v2 identity word must reject before executable allocation.
    auto bad = tmp; bad += ".bad";
    { std::ifstream is(tmp, std::ios::binary); std::vector<char> bytes((std::istreambuf_iterator<char>(is)), {}); bytes[28] ^= 1; std::ofstream os(bad, std::ios::binary); os.write(bytes.data(), bytes.size()); }
    LoadedModule bad_mod; std::string bad_err;
    bool identity_rejected = !load_em_file(bad.string().c_str(), bad_mod, &bad_err, nullptr, &natives) && bad_mod.pages.empty() && bad_err.find("compatibility") != std::string::npos;
    std::filesystem::remove(bad);
    std::printf("[4b] mutated build/ABI identity rejected: %s\n", passfail(identity_rejected));
    if (!identity_rejected) failures++;

    // Native bindings are explicit symbolic slots. Missing and incompatible
    // host allowlists must fail during parse/validation, before page allocation.
    auto bind_path=tmp;bind_path += ".bindings";
    EmModule bind_mod; EmFunctionRecord bind_fn;bind_fn.name="bound";bind_fn.slot_index=0;
    bind_fn.code={0x48,0xB8,0,0,0,0,0,0,0,0,0xC3};bind_fn.signature.ret=make_prim(Prim::I64);
    EmNativeBinding bind;bind.offset=2;bind.name="host_bound";bind.signature.ret=make_prim(Prim::I64);bind.signature.params.push_back(make_prim(Prim::I64));bind_fn.native_bindings.push_back(bind);
    bind_mod.functions.push_back(bind_fn);bind_mod.entry_slot=0;bind_mod.name_table={{"bound",0}};
    std::string bind_werr;bool bind_written=write_em_file(bind_mod,bind_path.string().c_str(),&bind_werr);
    LoadedModule missing_mod;std::string missing_err;std::unordered_map<std::string,NativeSig> empty_allowlist;
    bool missing_rejected=bind_written&&!load_em_file(bind_path.string().c_str(),missing_mod,&missing_err,nullptr,&empty_allowlist)&&missing_mod.pages.empty()&&missing_err.find("missing native")!=std::string::npos;
    NativeSig wrong;wrong.name="host_bound";wrong.fn_ptr=reinterpret_cast<void*>(uintptr_t(1));wrong.ret=make_prim(Prim::F32);wrong.params={make_prim(Prim::I64)};
    std::unordered_map<std::string,NativeSig> wrong_allowlist{{wrong.name,wrong}};LoadedModule wrong_mod;std::string wrong_err;
    bool incompatible_rejected=bind_written&&!load_em_file(bind_path.string().c_str(),wrong_mod,&wrong_err,nullptr,&wrong_allowlist)&&wrong_mod.pages.empty()&&wrong_err.find("signature mismatch")!=std::string::npos;
    std::filesystem::remove(bind_path);bool binding_rejection_ok=missing_rejected&&incompatible_rejected;
    std::printf("[4c] missing/incompatible native bindings rejected: %s\n",passfail(binding_rejection_ok));if(!binding_rejection_ok)failures++;

    // [4e] Per-dimension canonical_type_same mutation tests at the LOADER
    // boundary. Each case builds a v2 module with a native binding whose
    // recorded signature differs from the host allowlist in EXACTLY ONE
    // canonical Type dimension and asserts the load REJECTS with a
    // signature-mismatch error. If canonical_type_same skipped that dim the
    // load would succeed, so each is a real non-circular regression for one
    // dim. The binding has zero params so the ret-type comparison is isolated.
    auto dim_path = tmp; dim_path += ".dim";
    auto dim_reject = [&](const char* tag, const Type& recorded_ret,
                          const std::vector<Type>& recorded_params,
                          const Type& allow_ret,
                          const std::vector<Type>& allow_params) {
        EmModule dm; EmFunctionRecord df; df.name="bound"; df.slot_index=0;
        df.code={0x48,0xB8,0,0,0,0,0,0,0,0,0xC3};
        df.signature.ret=make_prim(Prim::I64);
        EmNativeBinding db; db.offset=2; db.name="host_bound";
        db.signature.ret=recorded_ret;
        for (const auto& p:recorded_params) db.signature.params.push_back(p);
        df.native_bindings.push_back(db);
        dm.functions.push_back(df); dm.entry_slot=0; dm.name_table={{"bound",0}};
        std::string dwerr; bool dw=write_em_file(dm, dim_path.string().c_str(), &dwerr);
        NativeSig allow; allow.name="host_bound";
        allow.fn_ptr=reinterpret_cast<void*>(uintptr_t(1));
        allow.ret=allow_ret;
        allow.params=allow_params;
        std::unordered_map<std::string,NativeSig> allow_list{{allow.name,allow}};
        LoadedModule amod; std::string aerr;
        bool rejected = dw && !load_em_file(dim_path.string().c_str(),amod,&aerr,nullptr,&allow_list)
                            && amod.pages.empty()
                            && aerr.find("signature mismatch")!=std::string::npos;
        std::printf("[4e-%s] %s\n", tag, passfail(rejected));
        if (!rejected) failures++;
        std::filesystem::remove(dim_path);
    };
    // (a) slice-vs-array: recorded ret is_slice, allowlist ret array_len.
    { Type rr=make_slice(std::make_shared<Type>(make_prim(Prim::I8)));
      Type ar=make_array(std::make_shared<Type>(make_prim(Prim::I8)),1);
      dim_reject("slice_vs_array", rr, {}, ar, {}); }
    // (b) struct_name mismatch: recorded ret struct "A", allowlist struct "B".
    { Type rr=make_struct("A"); Type ar=make_struct("B");
      dim_reject("struct_name", rr, {}, ar, {}); }
    // (c) fn_handle mismatch: recorded ret is_fn_handle, allowlist not.
    { Type rr; rr.prim=Prim::I64; rr.is_fn_handle=true;
      Type ar=make_prim(Prim::I64);
      dim_reject("fn_handle", rr, {}, ar, {}); }
    // (d) recorded_sig mismatch: recorded ret has_recorded_sig, allowlist not.
    { Type rr; rr.prim=Prim::I64; rr.is_fn_handle=true; rr.has_recorded_sig=true;
      rr.recorded_ret=std::make_shared<Type>(make_prim(Prim::I64));
      Type ar; ar.prim=Prim::I64; ar.is_fn_handle=true;
      dim_reject("recorded_sig", rr, {}, ar, {}); }
    // (e) recorded_params arity: 1 vs 2.
    { Type rr; rr.prim=Prim::I64; rr.is_fn_handle=true; rr.has_recorded_sig=true;
      rr.recorded_params.push_back(std::make_shared<Type>(make_prim(Prim::I64)));
      rr.recorded_ret=std::make_shared<Type>(make_prim(Prim::I64));
      Type ar; ar.prim=Prim::I64; ar.is_fn_handle=true; ar.has_recorded_sig=true;
      ar.recorded_params.push_back(std::make_shared<Type>(make_prim(Prim::I64)));
      ar.recorded_params.push_back(std::make_shared<Type>(make_prim(Prim::I64)));
      ar.recorded_ret=std::make_shared<Type>(make_prim(Prim::I64));
      dim_reject("recorded_params_arity", rr, {}, ar, {}); }
    // (f) recorded_ret prim: I64 vs I32.
    { Type rr; rr.prim=Prim::I64; rr.is_fn_handle=true; rr.has_recorded_sig=true;
      rr.recorded_ret=std::make_shared<Type>(make_prim(Prim::I64));
      Type ar; ar.prim=Prim::I64; ar.is_fn_handle=true; ar.has_recorded_sig=true;
      ar.recorded_ret=std::make_shared<Type>(make_prim(Prim::I32));
      dim_reject("recorded_ret", rr, {}, ar, {}); }
    // (g) elem recursion: slice of I8 vs slice of I16.
    { Type rr=make_slice(std::make_shared<Type>(make_prim(Prim::I8)));
      Type ar=make_slice(std::make_shared<Type>(make_prim(Prim::I16)));
      dim_reject("elem_recursion", rr, {}, ar, {}); }


    // Explicit v1 compatibility: rewrite the v2 source artifact is not valid
    // because record layouts differ, so construct the minimal historical v1.
    auto v1 = tmp; v1 += ".v1";
    { std::vector<uint8_t> b; auto u16=[&](uint16_t v){b.push_back(uint8_t(v));b.push_back(uint8_t(v>>8));}; auto u32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));};
      u32(EM_MAGIC);u32(EM_VERSION_V1);u32(0);u32(1);u32(0);u32(0);u32(0);u32(0);u32(0);u32(0);
      u16(1);b.push_back('f');u32(0);u32(1);u32(0);b.push_back(0xc3);u32(0);u32(1);u16(1);b.push_back('f');u32(0);
      std::ofstream os(v1,std::ios::binary);os.write(reinterpret_cast<const char*>(b.data()),b.size()); }
    LoadedModule v1_mod; std::string v1_err; bool v1_ok=load_em_file(v1.string().c_str(),v1_mod,&v1_err);
    auto v1_exports=build_em_exports(v1_mod,8); bool v1_contract=v1_ok&&v1_mod.format_version==EM_VERSION_V1&&v1_exports.size()==1&&v1_exports[0].unknown_sig;
    std::filesystem::remove(v1); std::printf("[4d] v1 unknown-signature compatibility: %s\n",passfail(v1_contract)); if(!v1_contract)failures++;

    // ---- call the loaded functions (same args as the JIT ground truth) ----
    void* loaded_double = lm.entry_by_name("double_it");
    void* loaded_fib    = lm.entry_by_name("fib");
    if (!loaded_double || !loaded_fib) {
        std::printf("FAIL: loaded entry_by_name returned null (double=%p fib=%p)\n",
                    loaded_double, loaded_fib);
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    int64_t ld_double_7  = call_i64_i64(loaded_double, 7);
    int64_t ld_double_21 = call_i64_i64(loaded_double, 21);
    int64_t ld_fib_10    = call_i64_i64(loaded_fib, 10);
    int64_t ld_fib_15   = call_i64_i64(loaded_fib, 15);
    std::printf("LOAD: double_it(7)=%lld  double_it(21)=%lld  fib(10)=%lld  fib(15)=%lld\n",
                (long long)ld_double_7, (long long)ld_double_21,
                (long long)ld_fib_10, (long long)ld_fib_15);

    bool roundtrip_ok = (ld_double_7 == jit_double_7) && (ld_double_21 == jit_double_21) &&
                       (ld_fib_10 == jit_fib_10) && (ld_fib_15 == jit_fib_15) &&
                       (ld_double_7 == 14) && (ld_double_21 == 42) &&
                       (ld_fib_10 == 55) && (ld_fib_15 == 610);
    std::printf("[5] round-trip matches JIT: %s\n", passfail(roundtrip_ok));
    if (!roundtrip_ok) failures++;

    // ---- cleanup ----
    std::filesystem::remove(tmp);
    // LoadedModule destructor frees lm.pages via free_executable.
    for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);

    std::printf("\nem round-trip: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
