// em_v5_ir_test.cpp — Stage B c3: the v5 IR .em end-to-end integration test.
//
// Proves the v5 SECURITY PROPERTY end-to-end:
//   1. Compile a function via the IR path (lower_function -> ThinFunction).
//   2. Serialize the ThinFunction to an ir_blob (serialize_thin_function).
//   3. Build a v5 EmModule with the ir_blob in EmFunctionRecord.
//   4. Write it as a v5 .em (write_em_file_v5).
//   5. Load it (load_em_file) — the loader deserializes + validates + native-
//      rebinds + re-emits via emit_x64 BEFORE alloc_executable_rw.
//   6. Call the loaded function and assert value-equivalence (fib(10)==55).
//
// Then MALFORMED REJECTION: corrupt the ir_blob in several ways (bad magic,
// truncated, invalid ThinOp, unknown native name) and assert load_em_file
// returns false with NO executable page (LoadedModule.pages is empty).
//
// Modeled on em_roundtrip_test.cpp (the v3/v4 raw-x86 round-trip) but uses the
// v5 IR path throughout. The module carries IR, NOT raw x86 — the loaded code
// is re-emitted from the deserialized IR at load time.

#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/thin_lower.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"
#include "../src/thin_emit.hpp"
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/module_linker.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int failures = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) failures++;
}

// 1-arg caller (matches em_roundtrip_test.cpp's local helper).
static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

// Build a v5 EmModule from lowered ThinFunctions. Each IR-serializable
// function ships its ir_blob; the EmFunctionRecord's code/rodata/relocs are
// empty (the loader re-emits them from the IR at load time).
static EmModule build_v5_module_from_thinfns(
        const std::vector<ThinFunction>& thfs,
        const Program& program,
        const std::vector<uint8_t>& globals_bytes,
        uint32_t entry_slot,
        std::string* err) {
    EmModule mod;
    mod.functions.reserve(thfs.size());
    for (size_t i = 0; i < thfs.size(); ++i) {
        const auto& thf = thfs[i];
        const auto& decl = program.funcs[i];
        EmFunctionRecord rec;
        rec.name = thf.name;
        rec.slot_index = uint32_t(decl.slot);
        // Serialize the ThinFunction to ir_blob.
        if (!serialize_thin_function(thf, rec.ir_blob, err)) return mod;
        // Signature (from the FuncDecl — the same source the v3/v4 path uses).
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params)
            rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        // code/rodata/relocs/native_bindings stay empty — the loader re-emits.
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = globals_bytes;
    mod.entry_slot = entry_slot;
    mod.name_table.reserve(program.funcs.size());
    for (const auto& fn : program.funcs)
        mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
    return mod;
}

int main() {
    std::printf("=== em_v5_ir_test: Stage B c3 v5 IR .em end-to-end ===\n");

    // ---- source: fib (recursive, exercises the dispatch-table reloc path) ----
    const std::string src =
        "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\n";
    auto lr = tokenize(src, "<v5_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;  // empty — fib needs none
    auto layouts = build_struct_layouts(pr.program);
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
    if (!sr.ok) { std::printf("FAIL: sema:\n"); for (auto& e : sr.errors) std::printf("  %s\n", e.msg.c_str()); return 1; }

    // ---- lower to ThinFunction (the IR path) ----
    CodeGenCtx ctx;
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.enable_ir_backend = true;
    std::vector<ThinFunction> thfs;
    for (const auto& fn : pr.program.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) { std::printf("FAIL: lower gave empty blocks for %s\n", fn.name.c_str()); return 1; }
        thfs.push_back(std::move(thf));
    }
    check(true, "lower_function produced ThinFunctions");

    // ---- serialize to v5 .em ----
    std::vector<uint8_t> gb_store(0);  // no globals
    std::string serr;
    auto mod = build_v5_module_from_thinfns(thfs, pr.program, gb_store,
                                            uint32_t(slots["fib"]), &serr);
    if (!serr.empty()) { std::printf("FAIL: build v5 module: %s\n", serr.c_str()); return 1; }
    check(!mod.functions[0].ir_blob.empty(), "ir_blob populated");

    auto tmp = std::filesystem::temp_directory_path() / "em_v5_ir_test.em";
    std::string werr;
    if (!write_em_file_v5(mod, tmp.string().c_str(), &werr)) {
        std::printf("FAIL: write_em_file_v5: %s\n", werr.c_str()); return 1;
    }
    check(true, "write_em_file_v5 succeeded");

    // ---- load the v5 .em (deserialize + validate + re-emit + exec page) ----
    LoadedModule lm;
    std::string lerr;
    // v5 is unsigned -> dev mode (no verify policy). No natives needed (fib
    // has no native calls), but pass the empty table for the re-emit path.
    bool loaded = load_em_file(tmp.string().c_str(), lm, &lerr, nullptr, &natives);
    if (!loaded) {
        std::printf("FAIL: load_em_file: %s\n", lerr.c_str());
        std::filesystem::remove(tmp);
        return 1;
    }
    check(lm.format_version == EM_VERSION_V5, "loaded module is v5");
    check(!lm.pages.empty(), "exec page allocated (re-emit produced code)");

    // ---- call the loaded function ----
    void* loaded_fib = lm.entry_by_name("fib");
    if (!loaded_fib) { std::printf("FAIL: entry_by_name(\"fib\") returned null\n"); std::filesystem::remove(tmp); return 1; }
    int64_t result = call_i64_i64(loaded_fib, 10);
    check(result == 55, "loaded fib(10) == 55 (value-equivalent re-emit)");

    // ---- JIT ground-truth comparison (compile via IR path, call directly) ----
    DispatchTable table(1);
    ctx.dispatch_base = int64_t(table.base());
    ctx.globals_base = 0;
    CompiledFn cf = emit_x64(thfs[0], ctx);
    finalize(cf);
    table.set(0, cf.entry);
    int64_t jit_result = call_i64_i64(table.get(0), 10);
    free_executable(cf.exec);
    check(result == jit_result, "loaded result matches JIT IR-path result");

    std::filesystem::remove(tmp);

    // ---- MALFORMED REJECTION ----
    std::printf("Part 2: malformed v5 .em rejection (no exec page)\n");

    // Helper: write a v5 .em with a custom ir_blob and try to load it.
    auto try_load_blob = [&](const std::vector<uint8_t>& blob,
                             const std::unordered_map<std::string, NativeSig>& nat_table,
                             const char* label) -> bool {
        EmModule bad_mod;
        EmFunctionRecord rec;
        rec.name = "fib";
        rec.slot_index = 0;
        rec.ir_blob = blob;
        rec.signature.ret = Type{Prim::I64};
        rec.signature.params.push_back(Type{Prim::I64});
        bad_mod.functions.push_back(std::move(rec));
        bad_mod.globals = {};
        bad_mod.entry_slot = 0;
        bad_mod.name_table.emplace_back("fib", 0u);
        auto path = std::filesystem::temp_directory_path() / "em_v5_malformed.em";
        std::string we;
        if (!write_em_file_v5(bad_mod, path.string().c_str(), &we)) {
            // If the writer rejects it (e.g. bad ir_blob), that's also a pass
            // for the "no exec page" property — but we want to test the loader,
            // so report the writer rejection.
            std::printf("  [SKIP] %s: writer rejected (%s) — can't test loader\n", label, we.c_str());
            std::filesystem::remove(path);
            return true;  // not a failure — the blob was rejected before load
        }
        LoadedModule bad_lm;
        std::string le;
        bool ok = load_em_file(path.string().c_str(), bad_lm, &le, nullptr, &nat_table);
        std::filesystem::remove(path);
        // PASS: load returned false AND no exec page was allocated.
        bool pass = !ok && bad_lm.pages.empty();
        check(pass, label);
        if (!pass) std::printf("       (ok=%d pages=%zu err=%s)\n", ok, bad_lm.pages.size(), le.c_str());
        return pass;
    };

    // (a) bad magic: flip the first byte of a valid blob.
    {
        std::vector<uint8_t> blob = mod.functions[0].ir_blob;
        if (!blob.empty()) blob[0] ^= 0xFF;
        try_load_blob(blob, natives, "bad ir_blob magic -> rejected, no exec page");
    }
    // (b) truncated: cut the blob to 10 bytes.
    {
        std::vector<uint8_t> blob = mod.functions[0].ir_blob;
        blob.resize(10);
        try_load_blob(blob, natives, "truncated ir_blob -> rejected, no exec page");
    }
    // (c) empty blob (0 bytes).
    {
        std::vector<uint8_t> blob;
        try_load_blob(blob, natives, "empty ir_blob -> rejected, no exec page");
    }
    // (d) unknown native name: build a blob that calls a native the host
    //     didn't register. Use a simple i64 native call so the IR path
    //     handles it (float paths have known gaps at Stage A).
    {
        // Register a dummy native 'my_op' with i64(i64) sig so sema passes.
        std::unordered_map<std::string, NativeSig> nat_with_op;
        NativeSig op_sig;
        op_sig.fn_ptr = reinterpret_cast<void*>(0xDEAD);  // dummy — sema only needs the signature
        op_sig.ret = Type{Prim::I64};
        op_sig.params.push_back(Type{Prim::I64});
        nat_with_op["my_op"] = op_sig;
        const std::string nat_src =
            "fn f(x: i64) -> i64 { return my_op(x); }\n";
        auto nlr = tokenize(nat_src, "<nat_test>");
        if (!nlr.ok) { std::printf("FAIL: nat lex\n"); failures++; goto done; }
        auto npr = parse(std::move(nlr.toks));
        if (!npr.ok) { std::printf("FAIL: nat parse\n"); failures++; goto done; }
        std::unordered_map<std::string, int> nslots;
        for (auto& fn : npr.program.funcs) { fn.slot = 0; nslots[fn.name] = 0; }
        auto nlayouts = build_struct_layouts(npr.program);
        auto nsr = sema(npr.program, nat_with_op, nslots, 0, nullptr, &nlayouts);
        if (!nsr.ok) { std::printf("FAIL: nat sema\n"); failures++; goto done; }
        CodeGenCtx nctx;
        nctx.natives = &nat_with_op;
        nctx.script_slots = &nslots;
        nctx.structs = &nlayouts;
        nctx.enable_ir_backend = true;
        ThinFunction nthf = lower_function(npr.program.funcs[0], nctx);
        if (nthf.blocks.empty()) { std::printf("FAIL: nat lower\n"); failures++; goto done; }
        std::vector<uint8_t> nat_blob;
        std::string nse;
        if (!serialize_thin_function(nthf, nat_blob, &nse)) { std::printf("FAIL: nat serialize: %s\n", nse.c_str()); failures++; goto done; }
        // Load with an EMPTY native table — the loader should reject because
        // my_op is not in the table (the core v5 security gate).
        std::unordered_map<std::string, NativeSig> empty_natives;
        try_load_blob(nat_blob, empty_natives, "unknown native name -> rejected, no exec page (v5 security gate)");
    }
    // (e) Item 2 fix: natives == nullptr + CallNative in IR. The dead-code
    //     guard (checking pf.ir_blob which was cleared) is now fixed to scan
    //     thf.blocks. Load with nullptr natives — must reject, no exec page.
    {
        // Reuse the nat_blob from case (d) — it has a CallNative to my_op.
        // Build it again (case (d)'s locals are out of scope).
        std::unordered_map<std::string, NativeSig> nat_with_op2;
        NativeSig op_sig2;
        op_sig2.fn_ptr = reinterpret_cast<void*>(0xDEAD);
        op_sig2.ret = Type{Prim::I64};
        op_sig2.params.push_back(Type{Prim::I64});
        nat_with_op2["my_op"] = op_sig2;
        const std::string nat_src2 = "fn f(x: i64) -> i64 { return my_op(x); }\n";
        auto nlr2 = tokenize(nat_src2, "<nat_test2>");
        auto npr2 = parse(std::move(nlr2.toks));
        std::unordered_map<std::string, int> nslots2;
        for (auto& fn : npr2.program.funcs) { fn.slot = 0; nslots2[fn.name] = 0; }
        auto nlayouts2 = build_struct_layouts(npr2.program);
        auto nsr2 = sema(npr2.program, nat_with_op2, nslots2, 0, nullptr, &nlayouts2);
        if (!nsr2.ok) { std::printf("FAIL: nat2 sema\n"); failures++; goto done; }
        CodeGenCtx nctx2;
        nctx2.natives = &nat_with_op2; nctx2.script_slots = &nslots2;
        nctx2.structs = &nlayouts2; nctx2.enable_ir_backend = true;
        ThinFunction nthf2 = lower_function(npr2.program.funcs[0], nctx2);
        if (nthf2.blocks.empty()) { std::printf("FAIL: nat2 lower\n"); failures++; goto done; }
        std::vector<uint8_t> nat_blob2;
        std::string nse2;
        if (!serialize_thin_function(nthf2, nat_blob2, &nse2)) { std::printf("FAIL: nat2 serialize\n"); failures++; goto done; }
        // Write a v5 .em with this blob, then load with natives == nullptr.
        EmModule bad_mod2;
        EmFunctionRecord rec2;
        rec2.name = "f"; rec2.slot_index = 0; rec2.ir_blob = nat_blob2;
        rec2.signature.ret = Type{Prim::I64}; rec2.signature.params.push_back(Type{Prim::I64});
        bad_mod2.functions.push_back(std::move(rec2));
        bad_mod2.globals = {}; bad_mod2.entry_slot = 0;
        bad_mod2.name_table.emplace_back("f", 0u);
        auto path2 = std::filesystem::temp_directory_path() / "em_v5_nullptr_nat.em";
        std::string we2;
        if (!write_em_file_v5(bad_mod2, path2.string().c_str(), &we2)) {
            std::printf("  [SKIP] natives==nullptr: writer rejected\n");
            std::filesystem::remove(path2);
        } else {
            LoadedModule bad_lm2;
            std::string le2;
            // Pass nullptr for native_bindings — the Item 2 fix must reject.
            bool ok2 = load_em_file(path2.string().c_str(), bad_lm2, &le2, nullptr, nullptr);
            std::filesystem::remove(path2);
            bool pass2 = !ok2 && bad_lm2.pages.empty();
            check(pass2, "natives==nullptr + CallNative -> rejected, no exec page (Item 2 fix)");
            if (!pass2) std::printf("       (ok=%d pages=%zu err=%s)\n", ok2, bad_lm2.pages.size(), le2.c_str());
        }
    }
    // (f) Finding C residual (instr frame_off full-span): a hand-built IR with
    //     a producing op whose 8-byte store span crosses rbp (frame_off=-1,
    //     frame_size=16 -> writes [rbp-1, rbp+7)) MUST be rejected by the
    //     loader's validate_thin_function BEFORE any executable page is
    //     allocated. The serializer does not validate frame_off (validation is
    //     the loader's job), so a malformed blob reaches the loader and is
    //     rejected at validation time. This proves malformed serialized IR is
    //     rejected before executable allocation (the task's loader-harness
    //     requirement).
    {
        ThinFunction bad_thf;
        bad_thf.name = "bad_span";
        bad_thf.slot = 0;
        bad_thf.frame.frame_size = 16;
        bad_thf.frame.rbx_save_offset = -8;
        bad_thf.frame.next_local_off = 8;
        ThinBlock blk;
        blk.id = 0;
        ThinInstr ci;
        ci.op = ThinOp::ConstInt;
        ci.dst = 1;
        ci.imm.i = 0x4141414142424242LL;
        ci.meta.frame_off = -1;  // 8-byte store spans [-1, +7) -> crosses rbp
        ci.meta.width = 8;
        blk.instrs.push_back(ci);
        blk.term.kind = TermKind::Return;
        blk.term.ret = 1;
        bad_thf.blocks.push_back(std::move(blk));
        std::vector<uint8_t> bad_blob;
        std::string bse;
        if (!serialize_thin_function(bad_thf, bad_blob, &bse)) {
            std::printf("  [SKIP] frame_off span: serializer rejected (%s)\n", bse.c_str());
        } else {
            try_load_blob(bad_blob, natives, "instr frame_off span crosses rbp -> rejected, no exec page (Finding C residual)");
        }
    }

done:
    std::printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
