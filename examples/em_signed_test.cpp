// em_signed_test - F2 signed `.em` content-authentication regression
// (docs/spec/SPEC_AUDIT_2026-07-10.md F2).
//
// F2 closes the `.em` raw-x86 code-injection risk: a `.em` is arbitrary x86-64
// that gets mapped executable and called. v2/v3 carry a build/abi IDENTITY hash
// (FNV1a of compiler/ABI string literals) + a TYPE signature (sema arg-checking),
// NOT content authentication — a malicious `.em` from the same compiler/ABI with
// valid identity+type-sigs still injects arbitrary x86. F2 bumps `.em` to v4:
// the v3 layout byte-identical + an additive Ed25519 signature block. The
// loader verifies the signature over the content (header -> name directory)
// BEFORE alloc_executable_rw and rejects on mismatch, so a tampered `.em` is
// rejected rather than executed.
//
// This is the NON-CIRCULAR proof. The load-bearing case is (B): a SIGNED `.em`
// that is TAMPERED (a code byte flipped AFTER signing) must be REJECTED at load
// with a clear "Ed25519 verification FAILED" error — NOT executed. With the F2
// fix reverted (no signature block, no verify-before-exec), the tampered module
// WOULD be memcpy'd into an exec page and called (the code-injection surface the
// audit names); the "rejected, not executed" assertion fails. The other cases
// anchor the policy + the backward-compat contract.
//
// Cases:
//   (A) SIGNED v4 .em verifies + runs: load with the matching pubkey in the
//       keyring -> success, double_it(21)==42 (the happy path).
//   (B) TAMPERED signed .em REJECTED: copy the (A) artifact, flip one CODE byte
//       inside the signed payload, load -> FAIL with "verification FAILED",
//       and `out.pages.empty()` (no exec page was ever published). NOT executed.
//   (C) CORRUPTED signature block: flip a SIGNATURE byte -> FAIL with a clear
//       signature error (not "format error"). The block is structurally valid
//       but the signature no longer matches.
//   (D) Key-management policy:
//       (D1) dev mode (no keyring) rejects a v4 module ("requires a verification
//            key; host provided none").
//       (D2) signed-only mode (non-empty keyring) rejects an UNSIGNED v3 module
//            ("host mandates signed modules").
//   (E) Backward compat: an UNSIGNED v3 .em still loads in DEV mode (the existing
//       .em round-trip path is unchanged — F2 is additive; write_em_file emits v3,
//       load_em_file with no policy accepts it).
//
// The signing key is derived from a DETERMINISTIC 32-byte seed (no CSPRNG —
// see thirdparty/ed25519/ed25519_ember.hpp's ED25519_NO_SEED note) so the tamper
// test is reproducible. The SEED never leaves this test; a production host gets
// only the PUBKEY (32 bytes), and the build tool that emits `.em` signs with the
// SEED/priv OFF the host.
//
// Pipeline per case mirrors em_roundtrip_test: lex -> parse -> sema (empty
// natives/structs — double_it needs none) -> codegen -> finalize -> build
// EmModule -> write_em_file_signed (or write_em_file for unsigned) -> load_em_file.

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"

#include "../thirdparty/ed25519/ed25519_ember.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

// FIX 3 (EM_FORMAT_RED_TEAM 2026-07-11): the loader now rejects v1-v4 (raw
// x86) by default. This test uses write_em_file_signed (v4) + write_em_file
// (v3), so it opts in to raw x86 via EmLoadPolicy. The raw-x86 gate runs
// before the signature/dev-mode policy, so all v4/v3 loads need the flag to
// reach the signature/verify checks they actually test.
static const ember::EmLoadPolicy RAW_X86_POLICY{0u, true};

// Win64 i64(i64) call: double_it(x: i64) -> i64 takes one i64 (rcx) -> rax.
static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

// Build the EmModule from the JIT'd double_it, the way a host serializer would
// (mirrors em_roundtrip_test::build_em_module, minus the alias + native paths
// this minimal function does not need).
static ember::EmModule build_em_module(const std::vector<ember::CompiledFn>& fns,
                                       const ember::Program& program,
                                       uint32_t entry_slot) {
    ember::EmModule mod;
    mod.functions.reserve(fns.size());
    for (size_t i = 0; i < fns.size(); ++i) {
        const auto& cf = fns[i];
        const auto& decl = program.funcs[i];
        ember::EmFunctionRecord rec;
        rec.name = cf.name;
        rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes;
        rec.rodata = cf.rodata;
        rec.non_serializable_reason = cf.non_serializable_reason;
        rec.signature.ret = decl.ret ? *decl.ret : ember::Type{};
        for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : ember::Type{});
        for (const auto& af : cf.abs_fixups) {
            ember::EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend = af.addend;
            rec.relocs.push_back(r);
        }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals.clear();
    mod.entry_slot = entry_slot;
    mod.name_table.reserve(program.funcs.size());
    // F1: name directory IS the export table. double_it is a bare fn -> exported.
    for (const auto& fn : program.funcs)
        if (fn.is_exported)
            mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
    return mod;
}

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- deterministic keypair (the signing key; OFF the host in production) ----
    std::array<uint8_t, 32> seed{};
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 1);
    ed25519::PubKey pub{};
    ed25519::PrivKey priv{};
    ed25519::keypair_from_seed(seed, pub, priv);

    // ---- source: double_it(x) = x * 2 (no natives/globals/structs) ----
    const std::string src = "fn double_it(x: i64) -> i64 { return x * 2; }\n";
    auto lr = tokenize(src, "<em_signed>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;  // empty
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &struct_layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }
    GlobalsBlock gb; std::vector<uint8_t> gb_store(0); gb.base = 0;
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
        if (!finalize(cf)) { std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str()); return 1; }
        table.set(fn.slot, cf.entry);
        fns.push_back(std::move(cf));
    }
    int64_t jit_result = call_i64_i64(table.get(slots["double_it"]), 21);
    bool jit_ok = (jit_result == 42);
    std::printf("[0] JIT ground-truth double_it(21)==42: %s\n", passfail(jit_ok));
    if (!jit_ok) failures++;

    // ---- build the module + write the SIGNED v4 artifact ----
    auto mod = build_em_module(fns, pr.program, uint32_t(slots["double_it"]));
    std::filesystem::path signed_path = std::filesystem::temp_directory_path() / "em_signed_test.em";
    std::string werr;
    if (!write_em_file_signed(mod, signed_path.string().c_str(), pub, priv, &werr)) {
        std::printf("FAIL: write_em_file_signed: %s\n", werr.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 1;
    }
    auto signed_size = std::filesystem::file_size(signed_path);
    std::printf("wrote signed %s (%zu bytes)\n", signed_path.string().c_str(), signed_size);

    EmVerifyPolicy signed_only_policy;
    signed_only_policy.trusted_keys.push_back(pub);

    // ---- (A) SIGNED v4 .em verifies + runs (happy path) ----
    {
        LoadedModule lm; std::string lerr;
        bool ok = load_em_file(signed_path.string().c_str(), lm, &lerr, nullptr, &natives, &signed_only_policy, &RAW_X86_POLICY);
        bool ran_ok = ok && lm.pages.size() == 1 && lm.format_version == EM_VERSION;
        if (ran_ok) {
            void* e = lm.entry_by_name("double_it");
            ran_ok = e && call_i64_i64(e, 21) == 42;
        }
        std::printf("[A] signed v4 verifies + runs double_it(21)==42: %s%s%s\n",
                    passfail(ran_ok),
                    ok ? "" : " (load err: ", ok ? "" : (lerr + ")").c_str());
        if (!ran_ok) failures++;
    }

    // ---- (B) TAMPERED signed .em REJECTED, NOT executed ----
    {
        auto tampered = signed_path; tampered += ".tampered";
        {
            std::ifstream is(signed_path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(is)), {});
            // code starts after header(40) + name_len(2) + name("double_it"=10)
            // + slot(4) + code_size(4) + rodata_size(4) = 64. Flip first CODE byte.
            if (bytes.size() > 64) bytes[64] ^= 0x01;
            std::ofstream os(tampered, std::ios::binary);
            os.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        LoadedModule lm; std::string lerr;
        bool ok = load_em_file(tampered.string().c_str(), lm, &lerr, nullptr, &natives, &signed_only_policy, &RAW_X86_POLICY);
        bool rejected = !ok && lm.pages.empty();
        bool clear_error = lerr.find("verification FAILED") != std::string::npos ||
                           lerr.find("Ed25519 verification") != std::string::npos;
        bool case_b = rejected && clear_error;
        std::printf("[B] tampered signed .em REJECTED (not executed), clear verify error: %s\n", passfail(case_b));
        if (!case_b) std::printf("    (ok=%d pages=%zu err='%s')\n", (int)ok, lm.pages.size(), lerr.c_str());
        if (!case_b) failures++;
        std::filesystem::remove(tampered);
    }

    // ---- (C) CORRUPTED signature block REJECTED ----
    {
        auto corupt = signed_path; corupt += ".corrupt";
        {
            std::ifstream is(signed_path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(is)), {});
            // signature block is last 104 bytes; 64-byte signature is the final 64.
            if (bytes.size() >= 104) bytes[bytes.size() - 1] ^= 0x01;
            std::ofstream os(corupt, std::ios::binary);
            os.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        LoadedModule lm; std::string lerr;
        bool ok = load_em_file(corupt.string().c_str(), lm, &lerr, nullptr, &natives, &signed_only_policy, &RAW_X86_POLICY);
        bool rejected = !ok && lm.pages.empty();
        bool sig_error = lerr.find("verification FAILED") != std::string::npos ||
                         lerr.find("Ed25519 verification") != std::string::npos;
        bool case_c = rejected && sig_error;
        std::printf("[C] corrupted signature block REJECTED, clear verify error: %s\n", passfail(case_c));
        if (!case_c) std::printf("    (ok=%d pages=%zu err='%s')\n", (int)ok, lm.pages.size(), lerr.c_str());
        if (!case_c) failures++;
        std::filesystem::remove(corupt);
    }

    // ---- (D) Key-management policy ----
    // (D1) dev mode (no keyring) rejects a v4 module.
    {
        LoadedModule lm; std::string lerr;
        bool ok = load_em_file(signed_path.string().c_str(), lm, &lerr, nullptr, &natives, nullptr, &RAW_X86_POLICY);
        bool rejected = !ok && lm.pages.empty();
        bool clear = lerr.find("requires a verification key") != std::string::npos;
        bool case_d1 = rejected && clear;
        std::printf("[D1] dev mode (no key) rejects v4 signed module: %s\n", passfail(case_d1));
        if (!case_d1) std::printf("    (ok=%d err='%s')\n", (int)ok, lerr.c_str());
        if (!case_d1) failures++;
    }
    // (D2) signed-only mode rejects an UNSIGNED v3 module + (E) backward compat.
    {
        auto unsigned_path = signed_path; unsigned_path += ".unsigned";
        std::string uwerr;
        if (!write_em_file(mod, unsigned_path.string().c_str(), &uwerr)) {
            std::printf("FAIL: write_em_file (unsigned): %s\n", uwerr.c_str());
            failures++;
        } else {
            LoadedModule lm; std::string lerr;
            bool ok = load_em_file(unsigned_path.string().c_str(), lm, &lerr, nullptr, &natives, &signed_only_policy, &RAW_X86_POLICY);
            bool rejected = !ok && lm.pages.empty();
            bool clear = lerr.find("mandates signed modules") != std::string::npos;
            bool case_d2 = rejected && clear;
            std::printf("[D2] signed-only mode rejects unsigned v3 module: %s\n", passfail(case_d2));
            if (!case_d2) std::printf("    (ok=%d err='%s')\n", (int)ok, lerr.c_str());
            if (!case_d2) failures++;
            // (E) backward compat: the SAME unsigned v3 module loads fine in DEV mode.
            LoadedModule lm2; std::string lerr2;
            bool ok2 = load_em_file(unsigned_path.string().c_str(), lm2, &lerr2, nullptr, &natives, nullptr, &RAW_X86_POLICY);
            bool ran_ok = ok2 && lm2.pages.size() == 1 && lm2.format_version == EM_VERSION_V3;
            if (ran_ok) {
                void* e = lm2.entry_by_name("double_it");
                ran_ok = e && call_i64_i64(e, 21) == 42;
            }
            std::printf("[E] unsigned v3 still loads in DEV mode (backward compat): %s\n", passfail(ran_ok));
            if (!ran_ok) std::printf("    (ok=%d err='%s')\n", (int)ok2, lerr2.c_str());
            if (!ran_ok) failures++;
        }
        std::filesystem::remove(unsigned_path);
    }

    // ---- cleanup ----
    std::filesystem::remove(signed_path);
    for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);

    std::printf("\nF2 signed .em content authentication: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
