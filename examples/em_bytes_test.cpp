// em_bytes_test - unit test for load_em_bytes / write_em_bytes (the in-memory
// .em round-trip APIs added for the standalone exe bundler).
//
// The existing em_roundtrip_test proves the file-based round-trip
// (write_em_file -> load_em_file). This test proves the in-memory equivalent
// (write_em_bytes -> load_em_bytes) — the path the stub and bundler use. It
// also covers the error cases the stub depends on: null data, too-short
// buffer, bad magic.
//
// Compiles a small source (double_it + fib, same as em_roundtrip_test),
// serializes to a byte buffer via write_em_bytes, loads via load_em_bytes,
// and asserts the loaded module produces the SAME results as the JIT'd one.
// Then exercises the error paths.

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/jit_memory.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

static int64_t call_i64_i64_0(void* entry) {
    using F = int64_t(*)();
    return reinterpret_cast<F>(entry)();
}

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- source: double_it(x) = x*2 ; forty_two() = 42 ----
    const std::string src =
        "fn double_it(x: i64) -> i64 { return x * 2; }\n"
        "fn forty_two() -> i64 { return 42; }\n";

    // ---- lex + parse + slot assignment + sema ----
    auto lr = tokenize(src, "<em_bytes_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }
    if (pr.program.funcs.size() != 2) {
        std::printf("FAIL: expected 2 funcs, got %zu\n", pr.program.funcs.size());
        return 1;
    }
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }

    // ---- globals block (empty) + dispatch table + codegen ----
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store(0);
    gb.base = int64_t(gb_store.data());
    g_globals_for_codegen = &gb;
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;
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

    // ---- JIT ground-truth ----
    int64_t jit_double_7 = call_i64_i64(table.get(slots["double_it"]), 7);
    int64_t jit_forty_two = call_i64_i64_0(table.get(slots["forty_two"]));
    bool jit_ok = (jit_double_7 == 14) && (jit_forty_two == 42);
    std::printf("[1] JIT ground-truth: %s (double_it(7)=%lld, forty_two()=%lld)\n",
                passfail(jit_ok), (long long)jit_double_7, (long long)jit_forty_two);
    if (!jit_ok) failures++;

    // ---- build EmModule + serialize to a byte buffer (write_em_bytes) ----
    EmModule mod;
    mod.functions.reserve(fns.size());
    for (size_t i = 0; i < fns.size(); ++i) {
        const auto& cf = fns[i];
        const auto& decl = pr.program.funcs[i];
        EmFunctionRecord rec;
        rec.name = cf.name;
        rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes;
        rec.rodata = cf.rodata;
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        for (const auto& af : cf.abs_fixups) {
            EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend = af.addend;
            rec.relocs.push_back(r);
        }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = gb_store;
    mod.entry_slot = uint32_t(slots["forty_two"]);  // entry = forty_two
    for (const auto& fn : pr.program.funcs)
        mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));

    std::vector<uint8_t> em_bytes;
    std::string werr;
    if (!write_em_bytes(mod, em_bytes, &werr)) {
        std::printf("FAIL: write_em_bytes: %s\n", werr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    std::printf("[2] write_em_bytes: %s (%zu bytes)\n", passfail(!em_bytes.empty()), em_bytes.size());
    if (em_bytes.empty()) failures++;

    // ---- load from the byte buffer (load_em_bytes) ----
    LoadedModule lm;
    std::string lerr;
    if (!load_em_bytes(em_bytes.data(), em_bytes.size(), lm, &lerr, nullptr, &natives)) {
        std::printf("FAIL: load_em_bytes: %s\n", lerr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    std::printf("[3] load_em_bytes: %s (%zu funcs, entry_slot=%u)\n",
                passfail(true), lm.pages.size(), lm.entry_slot);

    // ---- run the loaded module and compare to JIT ground-truth ----
    void* loaded_double = lm.entry_by_name("double_it");
    void* loaded_forty_two_ptr = lm.entry_by_name("forty_two");
    if (!loaded_double || !loaded_forty_two_ptr) {
        std::printf("FAIL: loaded entry not found (double=%p, forty_two=%p)\n",
                    loaded_double, loaded_forty_two_ptr);
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    int64_t loaded_double_7 = call_i64_i64(loaded_double, 7);
    int64_t loaded_forty_two = call_i64_i64_0(loaded_forty_two_ptr);
    bool loaded_ok = (loaded_double_7 == jit_double_7) && (loaded_forty_two == jit_forty_two);
    std::printf("[4] loaded results match JIT: %s (double_it(7)=%lld, forty_two()=%lld)\n",
                passfail(loaded_ok), (long long)loaded_double_7, (long long)loaded_forty_two);
    if (!loaded_ok) failures++;

    // ---- the @entry (entry_slot = forty_two) ----
    void* entry = lm.entry();
    bool entry_ok = entry == loaded_forty_two_ptr;
    std::printf("[5] entry() == forty_two: %s\n", passfail(entry_ok));
    if (!entry_ok) failures++;

    // ---- error path: null data ----
    {
        LoadedModule bad;
        std::string e;
        bool ok = load_em_bytes(nullptr, 100, bad, &e, nullptr, nullptr);
        bool null_ok = !ok && !e.empty();
        std::printf("[6] null data rejected: %s\n", passfail(null_ok));
        if (!null_ok) failures++;
    }

    // ---- error path: too-short buffer ----
    {
        uint8_t tiny[10] = {};
        LoadedModule bad;
        std::string e;
        bool ok = load_em_bytes(tiny, 10, bad, &e, nullptr, nullptr);
        bool short_ok = !ok && !e.empty();
        std::printf("[7] short buffer rejected: %s\n", passfail(short_ok));
        if (!short_ok) failures++;
    }

    // ---- error path: bad magic ----
    {
        std::vector<uint8_t> bad_data(EM_HEADER_SIZE, 0);
        // fill with a wrong magic
        bad_data[0] = 0xFF; bad_data[1] = 0xFF; bad_data[2] = 0xFF; bad_data[3] = 0xFF;
        LoadedModule bad;
        std::string e;
        bool ok = load_em_bytes(bad_data.data(), bad_data.size(), bad, &e, nullptr, nullptr);
        bool magic_ok = !ok && !e.empty();
        std::printf("[8] bad magic rejected: %s\n", passfail(magic_ok));
        if (!magic_ok) failures++;
    }

    // ---- cleanup JIT pages ----
    for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);

    if (failures > 0) {
        std::printf("\n%d FAILURE(s)\n", failures);
        return 1;
    }
    std::printf("\nALL PASS\n");
    return 0;
}
