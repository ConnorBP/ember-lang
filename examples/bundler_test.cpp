// bundler_test - standalone exe bundler end-to-end test
// (docs/planning/plan_STANDALONE_BUNDLER.md Section 7 test matrix).
//
// Invokes ember_bundle.exe to bundle .ember scripts into standalone exes,
// runs the resulting exes, and asserts the exit codes. Tests:
//   1. bundle + run a simple script (exit 42)
//   2. bundle with multiple functions (entry calls a helper)
//   3. bundle with imports (imported fn is inlined at bundle time)
//   4. bundle with globals (global initializer baked into the .em)
//   5. bundle with natives (io extension: console_write + a math native)
//   6. malformed/corrupt footer rejection
//   7. spaces + Unicode paths, large modules, missing entry, overwrite
//   8. the `ember bundle` CLI front-end
//
// Usage: bundler_test <ember_bundle_exe> <stub_exe> <ember_cli_exe>
//   ember_bundle_exe : path to ember_bundle.exe (built alongside this test)
//   stub_exe         : path to ember_stub_main.exe (the pre-built stub)
//
// Both args are supplied via CMake's $<TARGET_FILE:...> generator expressions.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// Bundle footer constants (must match ember_stub_main.cpp / ember_bundle.cpp).
// Redefined here so the test is self-contained (doesn't depend on a shared
// header that doesn't exist yet). If these ever diverge, the round-trip tests
// will fail loudly.
static constexpr uint32_t BUNDLE_MAGIC      = 0x454D4244u;
static constexpr uint32_t BUNDLE_FOOTER_SIZE = 12u;

// Write a .ember source file to `path`.
static bool write_source(const fs::path& path, const std::string& content) {
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;
    os << content;
    return static_cast<bool>(os);
}

// Run a system command and return the exit code. On Windows, std::system
// returns the exit code directly (via cmd.exe). The command is wrapped in
// quotes to handle paths with spaces (matching em_cli_emit_test's pattern).
static int run_command(const std::string& cmd) {
    return std::system(cmd.c_str());
}

// Quote a path for cmd.exe: wrap in double quotes. cmd.exe strips one
// enclosing quote pair before parsing.
static std::string quote(const std::string& path) {
    return std::string("\"") + path + "\"";
}

// Bundle a .ember source into an output exe via ember_bundle.exe.
// Returns the ember_bundle exit code (0 = success).
static int bundle(const std::string& bundler_exe,
                  const std::string& stub_exe,
                  const std::string& source,
                  const std::string& output,
                  const std::string& extra = std::string()) {
    // ember_bundle <input.ember> <output.exe> --stub <stub.exe>
    std::string cmd = quote(quote(bundler_exe) + " " + quote(source) + " " +
                            quote(output) + " --stub " + quote(stub_exe) +
                            (extra.empty() ? "" : " " + extra));
    return run_command(cmd);
}

// Run a bundled exe and return its exit code.
static int run_bundled(const std::string& exe) {
#if defined(_WIN32)
    // std::system goes through cmd.exe's active code page and cannot reliably
    // launch UTF-8 paths. CreateProcessW exercises the bundled executable's
    // real Unicode self-path handling instead.
    const std::wstring app = fs::u8path(exe).wstring();
    std::wstring cmd = L"\"" + app + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    return run_command(quote(quote(exe)));
#endif
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int run_captured(const std::string& executable, const std::string& args,
                        const fs::path& capture) {
    const std::string inner = quote(executable) + (args.empty() ? "" : " " + args) +
                              " > " + quote(capture.string()) + " 2>&1";
    return run_command(quote(inner));
}

// ---- individual test cases ----

// Test 1: bundle + run a simple script (exit 42).
static bool test_simple_bundle(const std::string& bundler, const std::string& stub,
                               const fs::path& tmp) {
    const auto src = (tmp / "simple.ember").string();
    const auto out = (tmp / "simple.exe").string();
    if (!write_source(src, "fn main() -> i64 { return 42; }\n")) {
        std::fprintf(stderr, "  simple: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out);
    if (brc != 0) { std::fprintf(stderr, "  simple: bundle failed (rc=%d)\n", brc); return false; }
    if (!fs::exists(out)) { std::fprintf(stderr, "  simple: output exe not created\n"); return false; }
    int rrc = run_bundled(out);
    if (rrc != 42) { std::fprintf(stderr, "  simple: expected exit 42, got %d\n", rrc); return false; }
    std::printf("  simple: PASS (exit %d)\n", rrc);
    return true;
}

// Test 2: bundle with multiple functions (entry calls a helper).
static bool test_multiple_functions(const std::string& bundler, const std::string& stub,
                                    const fs::path& tmp) {
    const auto src = (tmp / "multi.ember").string();
    const auto out = (tmp / "multi.exe").string();
    const char* code =
        "fn helper(x: i64) -> i64 { return x * 2; }\n"
        "fn selected() -> i64 { return helper(21); }\n"
        "fn main() -> i64 { return 1; }\n";
    if (!write_source(src, code)) {
        std::fprintf(stderr, "  multi: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out, "--fn selected");
    if (brc != 0) { std::fprintf(stderr, "  multi: bundle failed (rc=%d)\n", brc); return false; }
    int rrc = run_bundled(out);
    if (rrc != 42) { std::fprintf(stderr, "  multi: expected exit 42, got %d\n", rrc); return false; }
    std::printf("  multi: PASS (exit %d)\n", rrc);
    return true;
}

// Test 3: bundle with imports (imported fn is inlined at bundle time).
static bool test_imports(const std::string& bundler, const std::string& stub,
                         const fs::path& tmp) {
    // Create an import target file + the main file that imports it.
    const auto lib_path = tmp / "lib.ember";
    const auto src = (tmp / "imports.ember").string();
    const auto out = (tmp / "imports.exe").string();
    if (!write_source(lib_path.string(), "fn lib_value() -> i64 { return 100; }\n")) {
        std::fprintf(stderr, "  imports: failed to write lib\n"); return false;
    }
    const char* code =
        "import \"lib.ember\";\n"
        "fn main() -> i64 { return lib_value() + 7; }\n";
    if (!write_source(src, code)) {
        std::fprintf(stderr, "  imports: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out);
    if (brc != 0) { std::fprintf(stderr, "  imports: bundle failed (rc=%d)\n", brc); return false; }
    int rrc = run_bundled(out);
    if (rrc != 107) { std::fprintf(stderr, "  imports: expected exit 107, got %d\n", rrc); return false; }
    std::printf("  imports: PASS (exit %d)\n", rrc);
    return true;
}

// Test 4: bundle with globals (global initializer baked into the .em).
static bool test_globals(const std::string& bundler, const std::string& stub,
                         const fs::path& tmp) {
    const auto src = (tmp / "globals.ember").string();
    const auto out = (tmp / "globals.exe").string();
    const char* code =
        "global cfg : i64 = 7;\n"
        "fn main() -> i64 { return cfg + 35; }\n";
    if (!write_source(src, code)) {
        std::fprintf(stderr, "  globals: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out);
    if (brc != 0) { std::fprintf(stderr, "  globals: bundle failed (rc=%d)\n", brc); return false; }
    int rrc = run_bundled(out);
    if (rrc != 42) { std::fprintf(stderr, "  globals: expected exit 42, got %d\n", rrc); return false; }
    std::printf("  globals: PASS (exit %d)\n", rrc);
    return true;
}

// Test 5: bundle with natives (math + string + array extension natives).
static bool test_natives(const std::string& bundler, const std::string& stub,
                         const fs::path& tmp) {
    const auto src = (tmp / "natives.ember").string();
    const auto out = (tmp / "natives.exe").string();
    // Use sqrt (math extension) + string_length (string extension) to prove
    // the stub's native allowlist resolves the .em's native bindings.
    const char* code =
        "fn main() -> i64 {\n"
        "  let s: string = \"hello world\";\n"
        "  let n: i64 = string_length(s);\n"
        "  let r: f32 = sqrt(81.0f);\n"
        "  return n + (r as i64);\n"  // 11 + 9 = 20
        "}\n";
    if (!write_source(src, code)) {
        std::fprintf(stderr, "  natives: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out);
    if (brc != 0) { std::fprintf(stderr, "  natives: bundle failed (rc=%d)\n", brc); return false; }
    int rrc = run_bundled(out);
    if (rrc != 20) { std::fprintf(stderr, "  natives: expected exit 20, got %d\n", rrc); return false; }
    std::printf("  natives: PASS (exit %d)\n", rrc);
    return true;
}

// Test 6: malformed bundle — a plain stub with no .em appended -> exit 2.
// This proves the stub detects the no-embed case cleanly (bad footer magic).
static bool test_malformed_bundle(const std::string& bundler, const std::string& stub,
                                  const fs::path& tmp) {
    const auto out = (tmp / "malformed.exe").string();
    // Just copy the stub — no .em appended. The stub should detect the
    // missing footer and exit 2.
    std::error_code ec;
    fs::copy_file(stub, out, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "  malformed: copy stub failed: %s\n", ec.message().c_str()); return false; }
    int rrc = run_bundled(out);
    if (rrc != 2) { std::fprintf(stderr, "  malformed: expected exit 2, got %d\n", rrc); return false; }
    std::printf("  malformed: PASS (exit %d)\n", rrc);
    return true;
}

// Test 7: bad footer magic — append garbage with a wrong footer magic -> exit 2.
static bool test_bad_footer(const std::string& bundler, const std::string& stub,
                            const fs::path& tmp) {
    const auto out = (tmp / "badfooter.exe").string();
    std::error_code ec;
    fs::copy_file(stub, out, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "  badfooter: copy stub failed: %s\n", ec.message().c_str()); return false; }
    // Append a fake footer with a WRONG magic (not EMBD).
    std::ofstream app(out, std::ios::binary | std::ios::app);
    if (!app) { std::fprintf(stderr, "  badfooter: cannot open for append\n"); return false; }
    uint32_t bad_magic = 0xDEADBEEFu;  // wrong magic
    uint64_t fake_len = 100;
    app.write(reinterpret_cast<const char*>(&bad_magic), 4);
    app.write(reinterpret_cast<const char*>(&fake_len), 8);
    app.close();
    int rrc = run_bundled(out);
    if (rrc != 2) { std::fprintf(stderr, "  badfooter: expected exit 2, got %d\n", rrc); return false; }
    std::printf("  badfooter: PASS (exit %d)\n", rrc);
    return true;
}

// Overflow regression: valid EMBD magic + UINT64_MAX length must be rejected
// before arithmetic, allocation, seeking, or reading.
static bool test_footer_length_overflow(const std::string& stub, const fs::path& tmp) {
    const auto out = tmp / "overflow.exe";
    std::error_code ec;
    fs::copy_file(stub, out, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    std::ofstream app(out, std::ios::binary | std::ios::app);
    const uint32_t magic = BUNDLE_MAGIC;
    const uint64_t impossible = UINT64_MAX;
    app.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    app.write(reinterpret_cast<const char*>(&impossible), sizeof(impossible));
    app.close();
    const int rc = run_bundled(out.string());
    if (rc != 2) { std::fprintf(stderr, "  overflow: expected exit 2, got %d\n", rc); return false; }
    std::printf("  overflow: PASS\n");
    return true;
}

static bool test_spaces_unicode_and_cli(const std::string& cli, const std::string& stub,
                                        const fs::path& tmp) {
    const fs::path dir = tmp / fs::u8path(u8"space dir ünicode");
    std::error_code ec; fs::create_directories(dir, ec);
    const fs::path src = dir / fs::u8path(u8"héllo world.ember");
    const fs::path out = dir / fs::u8path(u8"héllo world.exe");
    if (!write_source(src, "fn main() -> i64 { return 43; }\n")) return false;
    const std::string args = "bundle " + quote(src.string()) + " " + quote(out.string()) +
                             " --stub " + quote(stub);
    const int brc = run_command(quote(quote(cli) + " " + args));
    if (brc != 0 || run_bundled(out.string()) != 43) {
        std::fprintf(stderr, "  unicode: CLI bundle/run failed (rc=%d)\n", brc); return false;
    }
    std::printf("  spaces/unicode + ember bundle CLI: PASS\n");
    return true;
}

static bool test_large_module(const std::string& bundler, const std::string& stub,
                              const fs::path& tmp) {
    const fs::path src = tmp / "large.ember", out = tmp / "large.exe";
    std::string code;
    code.reserve(512 * 1024);
    for (int i = 0; i < 1800; ++i)
        code += "fn helper_" + std::to_string(i) + "() -> i64 { return " + std::to_string(i % 200) + "; }\n";
    code += "fn main() -> i64 { return helper_1799(); }\n";
    if (!write_source(src, code)) return false;
    const int brc = bundle(bundler, stub, src.string(), out.string());
    if (brc != 0 || run_bundled(out.string()) != 199) {
        std::fprintf(stderr, "  large: bundle/run failed (rc=%d)\n", brc); return false;
    }
    std::printf("  large module: PASS (%zu source bytes)\n", code.size());
    return true;
}

static bool test_missing_entry(const std::string& bundler, const std::string& stub,
                               const fs::path& tmp) {
    const fs::path src = tmp / "missing_entry.ember", out = tmp / "missing_entry.exe";
    const fs::path log = tmp / "missing_entry.log";
    if (!write_source(src, "fn helper() -> i64 { return 1; }\n")) return false;
    const std::string args = quote(src.string()) + " " + quote(out.string()) +
                             " --stub " + quote(stub) + " --fn absent";
    const int rc = run_captured(bundler, args, log);
    const std::string msg = read_text(log);
    if (rc != 2 || msg.find("entry function 'absent' not found") == std::string::npos || fs::exists(out)) {
        std::fprintf(stderr, "  missing entry: unclear rejection (rc=%d, msg=%s)\n", rc, msg.c_str()); return false;
    }
    std::printf("  missing entry: PASS\n");
    return true;
}

static bool test_overwrite(const std::string& bundler, const std::string& stub,
                           const fs::path& tmp) {
    const fs::path src = tmp / "overwrite.ember", out = tmp / "overwrite.exe";
    if (!write_source(src, "fn main() -> i64 { return 10; }\n") ||
        bundle(bundler, stub, src.string(), out.string()) != 0 || run_bundled(out.string()) != 10) return false;
    if (!write_source(src, "fn main() -> i64 { return 11; }\n") ||
        bundle(bundler, stub, src.string(), out.string()) != 0 || run_bundled(out.string()) != 11) {
        std::fprintf(stderr, "  overwrite: second bundle did not replace output\n"); return false;
    }
    std::printf("  overwrite existing output: PASS\n");
    return true;
}

// Stub MAX_FILE_SIZE cap regression (ember_stub_main.cpp): a valid EMBD footer
// whose em_length exceeds ember::MAX_FILE_SIZE (256 MiB) but is below the
// size_t/streamsize maxima MUST be rejected by the stub BEFORE it allocates
// em_length bytes. The loader's own parse_file cap would only fire AFTER the
// stub had already allocated, so a malicious footer with a 1 GiB em_length
// (below size_t max but far above the .em cap) drove an allocation-amplification
// / OOM DoS. The stub now checks `em_len <= ember::MAX_FILE_SIZE` before the
// allocation and contains allocation exceptions.
//
// To exercise the MAX_FILE_SIZE gate specifically (not the file-size
// consistency gate that precedes it), the on-disk file must be large enough
// that `em_len <= file_size - 12` holds, so the cap clause is the one that
// fires. We make the file logically MAX_FILE_SIZE+1 bytes with a SPARSE
// resize_file (instant — no 256 MiB zero write), then append the 12-byte
// footer; file_size - 12 == em_len == MAX_FILE_SIZE + 1 passes the
// consistency check, and em_len > MAX_FILE_SIZE rejects on the cap.
static bool test_stub_cap_rejects_oversized_footer(const std::string& stub,
                                                   const fs::path& tmp) {
    const fs::path out = tmp / "stub_cap.exe";
    std::error_code ec;
    fs::copy_file(stub, out, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "  stub_cap: copy stub failed: %s\n", ec.message().c_str()); return false; }
    constexpr uint64_t CAP = 256ull * 1024ull * 1024ull;  // ember::MAX_FILE_SIZE
    const uint64_t over_cap = CAP + 1;
    // Sparse-extend the on-disk file to over_cap bytes (a hole — no real write).
    fs::resize_file(out, over_cap, ec);
    if (ec) { std::fprintf(stderr, "  stub_cap: resize_file failed: %s\n", ec.message().c_str()); return false; }
    // Append the footer: u32 EMBD magic + u64 em_length = over_cap.
    std::ofstream app(out, std::ios::binary | std::ios::app);
    if (!app) { std::fprintf(stderr, "  stub_cap: cannot open for footer\n"); return false; }
    const uint32_t magic = BUNDLE_MAGIC;
    app.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    app.write(reinterpret_cast<const char*>(&over_cap), sizeof(over_cap));
    app.close();
    if (!app) { std::fprintf(stderr, "  stub_cap: close failed\n"); return false; }
    // Consistency: file_size - 12 == over_cap (so the size gate passes) and
    // over_cap > CAP (so the MAX_FILE_SIZE gate fires).
    const uint64_t fsize = fs::file_size(out, ec);
    if (ec || fsize - BUNDLE_FOOTER_SIZE != over_cap) {
        std::fprintf(stderr, "  stub_cap: bad file size %llu (expected %llu)\n",
                     static_cast<unsigned long long>(fsize),
                     static_cast<unsigned long long>(over_cap + BUNDLE_FOOTER_SIZE));
        return false;
    }
    const int rc = run_bundled(out.string());
    // PASS: the stub rejects with exit 2 BEFORE allocating ~over_cap bytes.
    // (Without the cap the stub would attempt a ~256 MiB+ allocation + read;
    // on a constrained host that is an OOM DoS. We assert the clean reject.)
    if (rc != 2) { std::fprintf(stderr, "  stub_cap: expected exit 2, got %d\n", rc); return false; }
    std::printf("  stub MAX_FILE_SIZE cap rejects oversized footer: PASS\n");
    return true;
}

// Bundle failure-path regression (ember_bundle.cpp): a failed publish MUST
// preserve an existing output. The bundler now writes the full bundle to a
// same-directory temp file and atomically renames it over the destination only
// after a successful flush+close; RAII removes the temp on every failure path.
// The prior implementation copied the stub over output_file FIRST and then
// appended in place, so an append-open/write failure left a partial stub and
// destroyed the original. We inject a publish-step failure by pre-creating the
// bundler's temp path (output + ".ember-bundle-tmp") as a NON-EMPTY directory:
// the new code's `copy_file(stub, temp)` fails (cannot copy a file over a non-
// empty directory) and returns 2, leaving the EXISTING output file untouched. A
// reverted copy-then-append bundler would instead `copy_file(stub, output)` —
// overwriting and destroying the existing output's sentinel content.
static bool test_bundle_failure_preserves_existing_output(const std::string& bundler,
                                                            const std::string& stub,
                                                            const fs::path& tmp) {
    const fs::path src = tmp / "failpub.ember", out = tmp / "failpub.exe";
    const fs::path temp = fs::path(out.string() + ".ember-bundle-tmp");
    if (!write_source(src, "fn main() -> i64 { return 42; }\n")) {
        std::fprintf(stderr, "  failpub: cannot write source\n"); return false;
    }
    // 1. Create an EXISTING output with sentinel content.
    {
        std::ofstream os(out, std::ios::binary | std::ios::trunc);
        if (!os) { std::fprintf(stderr, "  failpub: cannot create existing output\n"); return false; }
        os << "ORIGINAL_OUTPUT_SENTINEL";
    }
    // 2. Pre-create the bundler's temp path as a NON-EMPTY directory so the
    //    publish-step copy_file fails. (A marker file makes the dir non-empty,
    //    so fs::remove cannot clear it and copy_file cannot overwrite it.)
    std::error_code ec;
    fs::create_directories(temp, ec);
    if (ec) { std::fprintf(stderr, "  failpub: cannot create temp dir: %s\n", ec.message().c_str()); return false; }
    {
        std::ofstream mk(temp / "marker.txt", std::ios::binary);
        mk << "keep";
    }
    // 3. Run the bundler. The compile succeeds (valid source); publish fails at
    //    copy_file(stub, temp=non-empty-dir). Exit code must be 2.
    const int brc = bundle(bundler, stub, src.string(), out.string());
    if (brc != 2) {
        std::fprintf(stderr, "  failpub: expected bundle exit 2 (publish failure), got %d\n", brc);
        return false;
    }
    // 4. The EXISTING output MUST be preserved (sentinel content unchanged).
    const std::string after = read_text(out);
    if (after != "ORIGINAL_OUTPUT_SENTINEL") {
        std::fprintf(stderr, "  failpub: existing output was NOT preserved (got: %s)\n", after.c_str());
        return false;
    }
    std::printf("  bundle failure preserves existing output: PASS\n");
    // cleanup the pre-created temp dir (the test owns it).
    fs::remove_all(temp, ec);
    return true;
}

static bool test_permission_policy(const std::string& bundler, const std::string& stub,
                                   const fs::path& tmp) {
    const fs::path src = tmp / "permissions.ember", out = tmp / "permissions.exe";
    const fs::path log = tmp / "permissions.log";
    if (!write_source(src, "fn main() -> i64 { print(\"\"); return 0; }\n")) return false;
    std::string args = quote(src.string()) + " " + quote(out.string()) + " --stub " + quote(stub);
    int rc = run_captured(bundler, args, log);
    const std::string denied = read_text(log);
    if (rc != 2 || (denied.find("PERM_FFI permission") == std::string::npos &&
                    denied.find("unknown function") == std::string::npos)) {
        std::fprintf(stderr, "  permissions: default policy did not reject FFI\n"); return false;
    }
    rc = bundle(bundler, stub, src.string(), out.string(), "--permissions ffi");
    if (rc != 0 || run_bundled(out.string()) != 0) {
        std::fprintf(stderr, "  permissions: explicit ffi policy failed\n"); return false;
    }
    std::printf("  permission policy: PASS\n");
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: bundler_test <ember_bundle_exe> <stub_exe> <ember_cli_exe>\n");
        return 1;
    }
    const std::string bundler = argv[1];
    const std::string stub = argv[2];
    const std::string cli = argv[3];

    if (!fs::exists(bundler)) {
        std::fprintf(stderr, "bundler_test: ember_bundle not found: %s\n", bundler.c_str());
        return 1;
    }
    if (!fs::exists(stub)) {
        std::fprintf(stderr, "bundler_test: stub not found: %s\n", stub.c_str());
        return 1;
    }

    // Use a temp directory for all generated files.
    const auto tmp = fs::temp_directory_path() / "ember_bundler_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    bool all_ok = true;
    std::printf("=== standalone exe bundler tests ===\n");

    all_ok &= test_simple_bundle(bundler, stub, tmp);
    all_ok &= test_multiple_functions(bundler, stub, tmp);
    all_ok &= test_imports(bundler, stub, tmp);
    all_ok &= test_globals(bundler, stub, tmp);
    all_ok &= test_natives(bundler, stub, tmp);
    all_ok &= test_malformed_bundle(bundler, stub, tmp);
    all_ok &= test_bad_footer(bundler, stub, tmp);
    all_ok &= test_footer_length_overflow(stub, tmp);
    all_ok &= test_spaces_unicode_and_cli(cli, stub, tmp);
    all_ok &= test_large_module(bundler, stub, tmp);
    all_ok &= test_missing_entry(bundler, stub, tmp);
    all_ok &= test_overwrite(bundler, stub, tmp);
    all_ok &= test_stub_cap_rejects_oversized_footer(stub, tmp);
    all_ok &= test_bundle_failure_preserves_existing_output(bundler, stub, tmp);
    all_ok &= test_permission_policy(bundler, stub, tmp);

    // cleanup
    fs::remove_all(tmp, ec);

    if (!all_ok) {
        std::printf("\n=== FAIL ===\n");
        return 1;
    }
    std::printf("\n=== ALL PASS ===\n");
    return 0;
}
