// em_redteam_audit_test.cpp — regression tests for the .em format red-team
// audit (docs/audit/EM_FORMAT_RED_TEAM_2026-07-11.md).
//
// Three fixes were applied from the audit:
//   FIX 1 (Finding A, CRITICAL): frame_size=0 bypasses the frame_off bounds
//     check — tested in thin_ir_ser_test.cpp (validation_edge_cases, "Finding
//     A" cases A1/A2/A3). NOT repeated here (the validator is unit-tested
//     directly there).
//   FIX 2 (Finding B, HIGH): PERM_FFI not enforced at load time — tested here.
//     A hand-crafted .em with a native binding to a PERM_FFI-gated native is
//     rejected when module_permissions lacks PERM_FFI, and accepted when it
//     has it. Tests both the v2-v4 raw-x86 path (write_em_file = v3) and the
//     v5 IR path (write_em_file_v5).
//   FIX 3 (drop raw x86 v1-v4): refuse v1-v4 by default — tested here. A v3
//     .em is rejected by default (no EmLoadPolicy) and accepted with
//     EmLoadPolicy{allow_raw_x86=true}.
//
// Modeled on em_loader_hardening_test.cpp (the hand-built .em boundary test)
// + em_roundtrip_test.cpp's native-binding rejection pattern. Links ember +
// ember_frontend (the loader + writer + v5 IR re-emit path).

#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/binding_builder.hpp"  // PERM_FFI
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"      // serialize_thin_function
#include "../src/thin_lower.hpp"       // lower_function
#include "../src/thin_emit.hpp"        // emit_x64
#include "../src/codegen.hpp"          // CodeGenCtx
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/engine.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int failures = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) ++failures;
}

static std::filesystem::path temp_path(const char* tag) {
    return std::filesystem::temp_directory_path() /
           (std::string("ember_em_redteam_") + tag + ".em");
}

// Best-effort temp-file cleanup. Uses the non-throwing std::error_code
// overload of std::filesystem::remove so a cleanup race (file already gone,
// held open, or permission flipped between create and cleanup) cannot throw
// and terminate the whole test harness. The error code is intentionally
// ignored: these are throwaway files in the temp directory, and a failed
// remove of an already-absent path is the benign common case.
static void remove_quiet(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// ─── FIX 2: PERM_FFI load-side enforcement ───
//
// Build a v3 .em with a native binding to a PERM_FFI-gated native, then:
//   (a) load with module_permissions = 0 -> REJECTED ("requires PERM_FFI")
//   (b) load with module_permissions = PERM_FFI -> ACCEPTED (the native binds)
//
// This is the v2-v4 raw-x86 native binding path (parse_file line ~424).

static bool test_perm_ffi_raw_x86() {
    std::printf("FIX 2 (Finding B): PERM_FFI load-side enforcement (v3 raw-x86 path)\n");

    // Build a v3 module with one function that has a native binding to a
    // PERM_FFI-gated native "ffi_secret". The function is a stub: mov rax,
    // [native_imm64]; ret. The native binding at offset 2 patches the imm64.
    EmModule mod;
    EmFunctionRecord fn;
    fn.name = "caller";
    fn.slot_index = 0;
    // mov rax, 0x0; ret  (the 0x0 imm64 is patched by the native binding)
    fn.code = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3};
    fn.signature.ret = make_prim(Prim::I64);
    EmNativeBinding bind;
    bind.offset = 2;
    bind.name = "ffi_secret";
    bind.signature.ret = make_prim(Prim::I64);
    fn.native_bindings.push_back(bind);
    mod.functions.push_back(fn);
    mod.entry_slot = 0;
    mod.name_table = {{"caller", 0}};

    const auto path = temp_path("ffi_raw");
    std::string werr;
    if (!write_em_file(mod, path.string().c_str(), &werr)) {
        std::printf("  FAIL: write_em_file: %s\n", werr.c_str());
        remove_quiet(path);
        return false;
    }

    // Register the native with PERM_FFI set + a dummy fn_ptr.
    std::unordered_map<std::string, NativeSig> natives;
    NativeSig sig;
    sig.name = "ffi_secret";
    sig.fn_ptr = reinterpret_cast<void*>(uintptr_t(0xDEAD));
    sig.ret = make_prim(Prim::I64);
    sig.permission = PERM_FFI;  // the gate
    natives["ffi_secret"] = sig;

    // (a) load WITHOUT PERM_FFI -> must be REJECTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy no_ffi{0u, true};  // allow_raw_x86=true (v3), no PERM_FFI
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &natives, nullptr, &no_ffi);
        bool rejected = !ok && lm.pages.empty();
        bool clear = lerr.find("PERM_FFI") != std::string::npos;
        check(!ok && rejected && clear,
              "(a) v3 .em with PERM_FFI native rejected (module lacks PERM_FFI)");
        if (!ok && !clear)
            std::printf("    err: %s\n", lerr.c_str());
    }

    // (b) load WITH PERM_FFI -> must be ACCEPTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy with_ffi{PERM_FFI, true};  // allow_raw_x86=true + PERM_FFI
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &natives, nullptr, &with_ffi);
        bool accepted = ok && lm.pages.size() == 1;
        check(accepted, "(b) v3 .em with PERM_FFI native accepted (module has PERM_FFI)");
        if (!accepted)
            std::printf("    err: %s\n", lerr.c_str());
    }

    remove_quiet(path);
    return true;
}

// ─── FIX 2 (v5 IR path): PERM_FFI load-side enforcement for v5 IR ───
//
// Build a v5 IR .em with a CallNative to a PERM_FFI-gated native, then:
//   (a) load with module_permissions = 0 -> REJECTED ("requires PERM_FFI")
//   (b) load with module_permissions = PERM_FFI -> ACCEPTED
//
// This is the v5 IR native rebind path (load_em_bytes_impl line ~665).

static bool test_perm_ffi_v5_ir() {
    std::printf("FIX 2 (Finding B): PERM_FFI load-side enforcement (v5 IR path)\n");

    // Lower a tiny function that calls a native: fn caller() -> i64 { return
    // ffi_secret(); }. This produces a ThinFunction with a CallNative instr.
    const std::string src =
        "fn caller() -> i64 { return ffi_secret(); }\n";
    auto lr = tokenize(src, "<ffi_v5>");
    if (!lr.ok) { std::printf("  FAIL: lex\n"); return false; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("  FAIL: parse: %s\n", pr.error.c_str()); return false; }

    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }

    // Register the native with PERM_FFI set + a real fn_ptr (a stub that
    // returns 42).
    std::unordered_map<std::string, NativeSig> natives;
    NativeSig sig;
    sig.name = "ffi_secret";
    sig.fn_ptr = reinterpret_cast<void*>(uintptr_t(0xDEAD));
    sig.ret = make_prim(Prim::I64);
    sig.permission = PERM_FFI;
    natives["ffi_secret"] = sig;

    auto layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;
    auto sr = sema(pr.program, natives, slots, PERM_FFI, nullptr, &layouts);
    if (!sr.ok) {
        std::printf("  FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("    line %u: %s\n", e.line, e.msg.c_str());
        return false;
    }

    // Lower to ThinFunction (IR).
    CodeGenCtx ctx;
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.enable_ir_backend = true;
    ThinFunction thf = lower_function(pr.program.funcs[0], ctx);
    if (thf.blocks.empty()) { std::printf("  FAIL: lower gave empty blocks\n"); return false; }

    // Build the v5 EmModule with the IR blob.
    std::vector<uint8_t> blob;
    std::string serr;
    if (!serialize_thin_function(thf, blob, &serr)) {
        std::printf("  FAIL: serialize: %s\n", serr.c_str());
        return false;
    }

    EmModule mod;
    EmFunctionRecord fn;
    fn.name = "caller";
    fn.slot_index = 0;
    fn.ir_blob = blob;
    fn.signature.ret = make_prim(Prim::I64);
    fn.non_serializable_reason.clear();  // IR-serializable
    mod.functions.push_back(fn);
    mod.entry_slot = 0;
    mod.name_table = {{"caller", 0}};

    const auto path = temp_path("ffi_v5");
    std::string werr;
    if (!write_em_file_v5(mod, path.string().c_str(), &werr)) {
        std::printf("  FAIL: write_em_file_v5: %s\n", werr.c_str());
        remove_quiet(path);
        return false;
    }

    // (a) load WITHOUT PERM_FFI -> must be REJECTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy no_ffi{0u, false};  // v5 IR (no raw-x86 needed), no PERM_FFI
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &natives, nullptr, &no_ffi);
        bool rejected = !ok && lm.pages.empty();
        bool clear = lerr.find("PERM_FFI") != std::string::npos;
        check(!ok && rejected && clear,
              "(a) v5 IR .em with PERM_FFI native rejected (module lacks PERM_FFI)");
        if (!ok && !clear)
            std::printf("    err: %s\n", lerr.c_str());
    }

    // (b) load WITH PERM_FFI -> must be ACCEPTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy with_ffi{PERM_FFI, false};  // v5 IR + PERM_FFI
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &natives, nullptr, &with_ffi);
        bool accepted = ok && lm.pages.size() == 1;
        check(accepted, "(b) v5 IR .em with PERM_FFI native accepted (module has PERM_FFI)");
        if (!accepted)
            std::printf("    err: %s\n", lerr.c_str());
    }

    remove_quiet(path);
    return true;
}

// ─── FIX 3: raw-x86 (v1-v4) rejection by default ───
//
// Build a v3 .em (via write_em_file), then:
//   (a) load with NO EmLoadPolicy (nullptr = secure default) -> REJECTED
//       ("raw x86 format v3 rejected by default")
//   (b) load with EmLoadPolicy{allow_raw_x86=true} -> ACCEPTED
//   (c) load with EmLoadPolicy{allow_raw_x86=false} -> REJECTED (same as null)
//
// Also test a v5 .em: load with NO policy -> ACCEPTED (v5 is the default-
// accepted format).

static bool test_raw_x86_rejection() {
    std::printf("FIX 3: raw-x86 (v1-v4) rejected by default, v5 accepted\n");

    // Build a minimal v3 module: one function (ret), no natives, no globals.
    EmModule mod;
    EmFunctionRecord fn;
    fn.name = "f";
    fn.slot_index = 0;
    fn.code = {0xC3};  // ret
    fn.signature.ret = make_prim(Prim::I64);
    mod.functions.push_back(fn);
    mod.entry_slot = 0;
    mod.name_table = {{"f", 0}};

    const auto path = temp_path("rawx86");
    std::string werr;
    if (!write_em_file(mod, path.string().c_str(), &werr)) {
        std::printf("  FAIL: write_em_file: %s\n", werr.c_str());
        remove_quiet(path);
        return false;
    }

    std::unordered_map<std::string, NativeSig> empty_natives;

    // (a) no policy (nullptr = secure default) -> REJECTED.
    {
        LoadedModule lm;
        std::string lerr;
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &empty_natives, nullptr, nullptr);
        bool rejected = !ok && lm.pages.empty();
        bool clear = lerr.find("raw x86") != std::string::npos &&
                     lerr.find("rejected by default") != std::string::npos;
        check(!ok && rejected && clear,
              "(a) v3 .em rejected by default (no EmLoadPolicy)");
        if (!ok && !clear)
            std::printf("    err: %s\n", lerr.c_str());
    }

    // (b) allow_raw_x86=true -> ACCEPTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy allow{0u, true};
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &empty_natives, nullptr, &allow);
        bool accepted = ok && lm.pages.size() == 1;
        check(accepted, "(b) v3 .em accepted with allow_raw_x86=true");
        if (!accepted)
            std::printf("    err: %s\n", lerr.c_str());
    }

    // (c) allow_raw_x86=false (explicit) -> REJECTED.
    {
        LoadedModule lm;
        std::string lerr;
        EmLoadPolicy deny{0u, false};
        bool ok = load_em_file(path.string().c_str(), lm, &lerr,
                               nullptr, &empty_natives, nullptr, &deny);
        bool rejected = !ok && lm.pages.empty();
        check(rejected, "(c) v3 .em rejected with allow_raw_x86=false (explicit)");
    }

    remove_quiet(path);

    // (d) v5 .em with NO policy -> ACCEPTED (v5 is the default-accepted format).
    // Build a minimal v5 IR module: one IR function.
    {
        const std::string src = "fn f() -> i64 { return 1; }\n";
        auto lr = tokenize(src, "<v5_default>");
        if (!lr.ok) { check(false, "(d) lex"); return false; }
        auto pr = parse(std::move(lr.toks));
        if (!pr.ok) { check(false, "(d) parse"); return false; }
        std::unordered_map<std::string, int> slots;
        int si = 0;
        for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
        std::unordered_map<std::string, NativeSig> natives;
        auto layouts = build_struct_layouts(pr.program);
        pr.program.string_xor_key = 0xA5;
        auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
        if (!sr.ok) { check(false, "(d) sema"); return false; }

        CodeGenCtx ctx;
        ctx.natives = &natives;
        ctx.script_slots = &slots;
        ctx.structs = &layouts;
        ctx.enable_ir_backend = true;
        ThinFunction thf = lower_function(pr.program.funcs[0], ctx);
        if (thf.blocks.empty()) { check(false, "(d) lower"); return false; }

        std::vector<uint8_t> blob;
        std::string serr;
        if (!serialize_thin_function(thf, blob, &serr)) {
            check(false, "(d) serialize"); return false;
        }

        EmModule v5mod;
        EmFunctionRecord v5fn;
        v5fn.name = "f";
        v5fn.slot_index = 0;
        v5fn.ir_blob = blob;
        v5fn.signature.ret = make_prim(Prim::I64);
        v5mod.functions.push_back(v5fn);
        v5mod.entry_slot = 0;
        v5mod.name_table = {{"f", 0}};

        const auto v5path = temp_path("v5_default");
        std::string vwerr;
        if (!write_em_file_v5(v5mod, v5path.string().c_str(), &vwerr)) {
            check(false, "(d) write_em_file_v5");
            remove_quiet(v5path);
            return false;
        }

        LoadedModule lm;
        std::string lerr;
        // NO EmLoadPolicy (nullptr = secure default) — v5 must be accepted.
        bool ok = load_em_file(v5path.string().c_str(), lm, &lerr,
                               nullptr, &natives, nullptr, nullptr);
        bool accepted = ok && lm.pages.size() == 1 && lm.format_version == EM_VERSION_V5;
        check(accepted, "(d) v5 .em accepted by default (no EmLoadPolicy needed)");
        if (!accepted)
            std::printf("    err: %s\n", lerr.c_str());
        remove_quiet(v5path);
    }

    return true;
}

int main() {
    std::printf("=== em_redteam_audit_test: .em format red-team audit fixes ===\n");

    std::printf("\n");
    test_perm_ffi_raw_x86();

    std::printf("\n");
    test_perm_ffi_v5_ir();

    std::printf("\n");
    test_raw_x86_rejection();

    std::printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
