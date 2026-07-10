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
        const std::unordered_map<std::string, int>& slots,
        const std::vector<uint8_t>& globals_bytes,
        uint32_t entry_slot) {
    ember::EmModule mod;
    mod.functions.reserve(fns.size());
    for (const auto& cf : fns) {
        ember::EmFunctionRecord rec;
        rec.name = cf.name;
        auto sit = slots.find(cf.name);
        rec.slot_index = (sit != slots.end()) ? uint32_t(sit->second) : 0xFFFFFFFFu;
        rec.code = cf.bytes; // post-resolve_fixups, pre-finalize (raw bytes)
        // rodata is empty for prism's codegen (string literals bake raw
        // imm64 pointers, not in-page RIP-relative rodata).
        // relocs: one per AbsFixup, mapping Kind -> EmReloc::Kind 1:1.
        rec.relocs.reserve(cf.abs_fixups.size());
        for (const auto& af : cf.abs_fixups) {
            ember::EmReloc r;
            r.offset = af.code_offset;
            r.kind = static_cast<uint8_t>(af.kind);
            rec.relocs.push_back(r);
        }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = globals_bytes;
    mod.entry_slot = entry_slot;
    // name table = name -> slot (for ember_call by name).
    mod.name_table.reserve(slots.size());
    for (auto& cf : fns) {
        auto sit = slots.find(cf.name);
        if (sit != slots.end())
            mod.name_table.emplace_back(cf.name, uint32_t(sit->second));
    }
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
    auto mod = build_em_module(fns, slots, gb_store,
                               uint32_t(slots["double_it"])); // entry = double_it
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
    if (!load_em_file(tmp.string().c_str(), lm, &lerr)) {
        std::printf("FAIL: load_em_file: %s\n", lerr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    std::printf("loaded: %zu funcs, dispatch=%zu slots, entry_slot=%u\n",
                lm.pages.size(), lm.dispatch.size(), lm.entry_slot);

    // v1 carries names/slots but no signatures. Pin the intentional H14
    // behavior: exports are marked unknown_sig and sema treats calls as
    // ABI-trusted (standalone args are checked, but arity/return cannot be).
    // Do not execute the deliberately wrong-arity probe.
    auto trusted_exports = build_em_exports(lm, 7);
    bool unknown_sig_ok = trusted_exports.size() == 2;
    for (const auto& exp : trusted_exports) unknown_sig_ok &= exp.unknown_sig;
    ModuleExportTable mt;
    add_exports(mt, "trusted", trusted_exports);
    auto ulr = tokenize("link \"trusted\"; fn main() -> i64 { return trusted::double_it(); }", "<unknown-sig>");
    bool trusted_sema_ok = ulr.ok;
    if (trusted_sema_ok) {
        auto upr = parse(std::move(ulr.toks));
        trusted_sema_ok = upr.ok;
        if (trusted_sema_ok) {
            std::unordered_map<std::string, int> uslots{{"main", 0}};
            upr.program.funcs[0].slot = 0;
            auto ulayouts = build_struct_layouts(upr.program);
            trusted_sema_ok = sema(upr.program, natives, uslots, 0, nullptr,
                                   &ulayouts, &mt).ok;
        }
    }
    bool h14_contract_ok = unknown_sig_ok && trusted_sema_ok;
    std::printf("[4] v1 unknown_sig ABI-trusted contract: %s\n", passfail(h14_contract_ok));
    if (!h14_contract_ok) failures++;

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
