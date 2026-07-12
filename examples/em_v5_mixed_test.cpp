// em_v5_mixed_test.cpp — D8: v5 mixed-mode (.em with IR + raw-x86 functions) +
// the v5 secure-default raw-x86 rejection (EM_FORMAT_RED_TEAM 2026-07-11,
// D8 follow-up / MAINTENANCE_LOG 2026-07-12 candidate #1).
//
// The v5 loader supports MIXED mode: a single .em module where SOME functions
// ship an ir_blob (is_ir=1, re-emitted from IR at load time) and OTHERS ship
// raw x86 (is_ir=0, the v4 per-fn body). This is a first-class supported
// combination (src/em_loader.cpp, src/em_file.hpp:42-66) (audit finding D8,
// docs/audit/AUDIT_2026-07-11_DOCS_TESTS.md).
//
// SECURITY MODEL (the fix this test pins): the secure default
// (EmLoadPolicy == nullptr, i.e. allow_raw_x86 = false) accepts ONLY all-IR
// v5 modules. A v5 module that contains ANY raw-x86 fallback function
// (is_ir=0) is an arbitrary-code-execution surface by construction — exactly
// the surface FIX 3 rejects for v1-v4 — so the secure default REJECTS it
// BEFORE any executable allocation, with a clear "raw x86 ... rejected by
// default ... allow_raw_x86=true" error and NO exec page. A host that needs
// to load mixed/raw v5 artifacts passes EmLoadPolicy{allow_raw_x86=true} for
// back-compat (the explicit opt-in); under that policy the raw-x86 functions
// load as raw x86 and the IR functions re-emit as usual.
//
// This test builds a 2-function v5 module:
//   - add(a,b):   IR function (is_ir=1)  — lower_function + serialize_thin_function
//   - double(x):  raw-x86 (is_ir=0)       — compile_func with IR OFF, raw bytes
//
// and asserts, in order:
//   Part 1: the mixed module is well-formed (IR fn + raw-x86 fn).
//   Part 2: the SECURE DEFAULT (no EmLoadPolicy) REJECTS the mixed module —
//           load fails with a "raw x86 ... rejected by default" error and NO
//           exec page is allocated.
//   Part 2b: the secure default ACCEPTS an all-IR v5 module (only `add`) —
//           proving the secure default distinguishes all-IR v5 loading from
//           raw-x86-bearing v5 loading.
//   Part 3: the mixed module LOADS under explicit EmLoadPolicy{allow_raw_x86=true}
//           (back-compat opt-in); both functions are callable and
//           value-equivalent (add(3,4)==7, double(21)==42).
//   Part 4: the malformed is_ir=1+empty-ir_blob case (P6) is rejected — this
//           is a parse-level rejection covered directly by em_v5_ir_test
//           Part 2 case (c); we reference it rather than duplicate it.

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

static int64_t call1_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}
static int64_t call2_i64(void* entry, int64_t a, int64_t b) {
    using F = int64_t(*)(int64_t, int64_t);
    return reinterpret_cast<F>(entry)(a, b);
}

int main() {
    std::printf("=== em_v5_mixed_test: D8 v5 mixed-mode + secure-default raw-x86 rejection ===\n");

    // Source: two independent functions, no cross-calls, no natives.
    //   add(a,b)    -> a+b          (will be the IR function, is_ir=1)
    //   double(x)   -> x*2          (will be the raw-x86 function, is_ir=0)
    const std::string src =
        "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
        "fn double(x: i64) -> i64 { return x * 2; }\n";

    auto lr = tokenize(src, "<v5_mixed>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }

    // Slots + sema.
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;  // none needed
    auto layouts = build_struct_layouts(pr.program);
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
    if (!sr.ok) { std::printf("FAIL: sema:\n"); for (auto& e : sr.errors) std::printf("  %s\n", e.msg.c_str()); return 1; }
    check(true, "lex+parse+sema succeeded for 2-function source");

    // --- IR half: lower `add` via the IR path and serialize to ir_blob ---
    CodeGenCtx ictx;
    ictx.natives = &natives;
    ictx.script_slots = &slots;
    ictx.structs = &layouts;
    ictx.enable_ir_backend = true;
    const FuncDecl* add_decl = nullptr;
    const FuncDecl* dbl_decl = nullptr;
    for (const auto& fn : pr.program.funcs) {
        if (fn.name == "add") add_decl = &fn;
        if (fn.name == "double") dbl_decl = &fn;
    }
    if (!add_decl || !dbl_decl) { std::printf("FAIL: could not find add/double decls\n"); return 1; }

    ThinFunction add_thf = lower_function(*add_decl, ictx);
    if (add_thf.blocks.empty()) { std::printf("FAIL: lower gave empty blocks for add\n"); return 1; }
    check(true, "lower_function produced IR for `add` (serializable)");

    std::string serr;
    std::vector<uint8_t> add_blob;
    if (!serialize_thin_function(add_thf, add_blob, &serr)) {
        std::printf("FAIL: serialize_thin_function(add): %s\n", serr.c_str()); return 1;
    }
    check(!add_blob.empty(), "serialize_thin_function produced non-empty ir_blob for add");

    // --- raw-x86 half: compile `double` via compile_func with IR OFF ---
    // We need the tree-walker (IR off) to produce raw x86 bytes + relocs.
    // Use a DispatchTable so the dispatch_base imm64 is available (double has
    // no calls, so no relocs will be produced, but the ctx field is required).
    DispatchTable table(2);
    CodeGenCtx rctx;
    rctx.natives = &natives;
    rctx.script_slots = &slots;
    rctx.structs = &layouts;
    rctx.dispatch_base = int64_t(table.base());
    rctx.globals_base = 0;
    rctx.enable_ir_backend = false;  // tree-walker -> raw x86 bytes
    CompiledFn dbl_cf = compile_func(*dbl_decl, rctx);
    if (dbl_cf.bytes.empty()) { std::printf("FAIL: compile_func gave empty bytes for double\n"); return 1; }
    check(!dbl_cf.bytes.empty(), "compile_func produced raw x86 bytes for `double` (IR off)");

    // --- build the MIXED EmModule ---
    EmModule mod;
    // Function 0: add (is_ir=1 — ir_blob populated, code empty)
    {
        EmFunctionRecord rec;
        rec.name = "add";
        rec.slot_index = uint32_t(add_decl->slot);
        rec.ir_blob = add_blob;  // non-empty -> writer emits is_ir=1
        rec.signature.ret = Type{Prim::I64};
        rec.signature.params.push_back(Type{Prim::I64});
        rec.signature.params.push_back(Type{Prim::I64});
        mod.functions.push_back(std::move(rec));
    }
    // Function 1: double (is_ir=0 — ir_blob empty, code populated)
    {
        EmFunctionRecord rec;
        rec.name = "double";
        rec.slot_index = uint32_t(dbl_decl->slot);
        rec.code = dbl_cf.bytes;     // raw x86 -> writer emits is_ir=0
        rec.rodata = dbl_cf.rodata;
        rec.signature.ret = Type{Prim::I64};
        rec.signature.params.push_back(Type{Prim::I64});
        // relocs + native_bindings: none (double has no calls/natives)
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = {};
    mod.entry_slot = uint32_t(slots["add"]);  // entry = add (arbitrary)
    mod.name_table.emplace_back("add", uint32_t(slots["add"]));
    mod.name_table.emplace_back("double", uint32_t(slots["double"]));

    // Sanity: confirm the mixed shape before serialization.
    check(!mod.functions[0].ir_blob.empty() && mod.functions[0].code.empty(),
          "fn 0 (add) is IR: ir_blob non-empty, code empty");
    check(mod.functions[1].ir_blob.empty() && !mod.functions[1].code.empty(),
          "fn 1 (double) is raw-x86: ir_blob empty, code non-empty");

    // --- write the mixed v5 module ---
    auto mixed_path = std::filesystem::temp_directory_path() / "em_v5_mixed_test.em";
    std::string werr;
    if (!write_em_file_v5(mod, mixed_path.string().c_str(), &werr)) {
        std::printf("FAIL: write_em_file_v5: %s\n", werr.c_str()); return 1;
    }
    check(true, "write_em_file_v5 succeeded for mixed module");

    // ============================================================
    // Part 2: SECURE DEFAULT (no EmLoadPolicy) REJECTS the mixed module.
    // The mixed module contains a raw-x86 function (double, is_ir=0) — an
    // arbitrary-code-execution surface. The secure default (allow_raw_x86 =
    // false) must reject it BEFORE any executable allocation, with a clear
    // error and NO exec page. This is the v5 mixed-mode raw-x86 secure-default
    // gate (the fix this test pins).
    // ============================================================
    std::printf("\nPart 2: secure default rejects mixed (raw-x86-bearing) v5 module\n");
    {
        LoadedModule lm;
        std::string lerr;
        // NO EmLoadPolicy (nullptr == secure default: allow_raw_x86 = false).
        bool loaded = load_em_file(mixed_path.string().c_str(), lm, &lerr,
                                   nullptr, &natives, nullptr, nullptr);
        bool rejected = !loaded && lm.pages.empty();
        bool clear = !loaded &&
                     lerr.find("raw x86") != std::string::npos &&
                     lerr.find("rejected by default") != std::string::npos &&
                     lerr.find("allow_raw_x86") != std::string::npos;
        check(rejected, "mixed v5 module rejected by secure default (no exec page)");
        check(clear, "rejection error names 'raw x86', 'rejected by default', 'allow_raw_x86'");
        if (!loaded && !clear)
            std::printf("    err: %s\n", lerr.c_str());
        if (loaded)
            std::printf("    UNEXPECTED: secure default loaded a raw-x86-bearing v5 module\n");
    }

    // ============================================================
    // Part 2b: SECURE DEFAULT ACCEPTS an all-IR v5 module.
    // The secure default must distinguish all-IR v5 loading (safe — every
    // function re-emitted from validated IR) from raw-x86-bearing v5 loading
    // (the Part 2 rejection). Build a module with ONLY the IR `add` function
    // and load it with no policy — it must be accepted, and add must work.
    // ============================================================
    std::printf("\nPart 2b: secure default accepts all-IR v5 module\n");
    {
        EmModule ir_only;
        {
            EmFunctionRecord rec;
            rec.name = "add";
            rec.slot_index = uint32_t(add_decl->slot);
            rec.ir_blob = add_blob;  // is_ir=1, no code
            rec.signature.ret = Type{Prim::I64};
            rec.signature.params.push_back(Type{Prim::I64});
            rec.signature.params.push_back(Type{Prim::I64});
            ir_only.functions.push_back(std::move(rec));
        }
        ir_only.globals = {};
        ir_only.entry_slot = uint32_t(slots["add"]);
        ir_only.name_table.emplace_back("add", uint32_t(slots["add"]));

        auto ir_path = std::filesystem::temp_directory_path() / "em_v5_ir_only_test.em";
        std::string iwerr;
        if (!write_em_file_v5(ir_only, ir_path.string().c_str(), &iwerr)) {
            std::printf("FAIL: write_em_file_v5(ir_only): %s\n", iwerr.c_str());
            std::filesystem::remove(mixed_path);
            return 1;
        }

        LoadedModule lm;
        std::string lerr;
        // NO EmLoadPolicy (secure default) — all-IR v5 must be accepted.
        bool loaded = load_em_file(ir_path.string().c_str(), lm, &lerr,
                                   nullptr, &natives, nullptr, nullptr);
        check(loaded && lm.format_version == EM_VERSION_V5,
              "all-IR v5 module accepted by secure default (v5)");
        check(loaded && !lm.pages.empty(),
              "all-IR v5 module got an exec page (IR re-emit)");
        if (!loaded) {
            std::printf("    err: %s\n", lerr.c_str());
            std::filesystem::remove(ir_path);
            std::filesystem::remove(mixed_path);
            return 1;
        }
        void* ir_add = lm.entry_by_name("add");
        check(ir_add != nullptr, "entry_by_name(add) resolved (all-IR)");
        if (ir_add) {
            check(call2_i64(ir_add, 3, 4) == 7, "all-IR: add(3,4) == 7 (secure default)");
            check(call2_i64(ir_add, -10, 25) == 15, "all-IR: add(-10,25) == 15 (secure default)");
        }
        std::filesystem::remove(ir_path);
    }

    // ============================================================
    // Part 3: EXPLICIT EmLoadPolicy{allow_raw_x86=true} ACCEPTS the mixed
    // module (back-compat opt-in). Under this policy the raw-x86 `double`
    // loads as raw x86 and the IR `add` re-emits; both are callable and
    // value-equivalent.
    // ============================================================
    std::printf("\nPart 3: explicit allow_raw_x86=true accepts mixed module (back-compat)\n");
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy allow{0u, true};  // allow_raw_x86 = true (back-compat opt-in)
        bool loaded = load_em_file(mixed_path.string().c_str(), lm, &lerr,
                                   nullptr, &natives, nullptr, &allow);
        if (!loaded) {
            std::printf("FAIL: load_em_file(allow_raw_x86=true): %s\n", lerr.c_str());
            std::filesystem::remove(mixed_path); return 1;
        }
        check(loaded && lm.format_version == EM_VERSION_V5,
              "mixed module accepted with allow_raw_x86=true (v5)");
        check(!lm.pages.empty(), "exec page allocated (IR re-emit + raw-x86 load)");

        void* loaded_add = lm.entry_by_name("add");
        void* loaded_dbl = lm.entry_by_name("double");
        if (!loaded_add) { std::printf("FAIL: entry_by_name(\"add\") null\n"); std::filesystem::remove(mixed_path); return 1; }
        if (!loaded_dbl) { std::printf("FAIL: entry_by_name(\"double\") null\n"); std::filesystem::remove(mixed_path); return 1; }
        check(loaded_add != nullptr, "entry_by_name(add) resolved");
        check(loaded_dbl != nullptr, "entry_by_name(double) resolved");

        // add: IR-re-emitted function.
        check(call2_i64(loaded_add, 3, 4) == 7, "mixed: add(3,4) == 7 (IR function, re-emitted)");
        check(call2_i64(loaded_add, -10, 25) == 15, "mixed: add(-10,25) == 15 (IR function)");

        // double: raw-x86 function (loaded as raw x86 under the opt-in).
        check(call1_i64(loaded_dbl, 21) == 42, "mixed: double(21) == 42 (raw-x86 function)");
        check(call1_i64(loaded_dbl, 0) == 0, "mixed: double(0) == 0 (raw-x86 function)");
    }

    std::filesystem::remove(mixed_path);

    // ============================================================
    // Part 4: malformed is_ir=1 + empty ir_blob (P6) rejection.
    // This is a parse-level rejection (an IR function must carry a non-empty
    // blob), independent of EmLoadPolicy, and is covered directly by
    // em_v5_ir_test Part 2 case (c) ("empty ir_blob -> rejected, no exec
    // page"). We reference it here rather than duplicate the case; the
    // secure-default gate (Part 2) and the all-IR acceptance (Part 2b) are
    // the load-policy behaviors this test owns.
    // ============================================================
    std::printf("\nPart 4: malformed is_ir=1+empty (covered by em_v5_ir_test Part 2 case c)\n");
    check(true, "is_ir=1+empty ir_blob parse-level rejection covered by em_v5_ir_test");

    std::printf("\nem_v5_mixed_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
