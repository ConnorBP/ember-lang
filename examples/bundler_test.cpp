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
//   6. malformed bundle (a plain stub with no .em appended -> exit 2)
//
// Usage: bundler_test <ember_bundle_exe> <stub_exe>
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

namespace fs = std::filesystem;

// Bundle footer constants (must match ember_stub_main.cpp / ember_bundle.cpp).
// Redefined here so the test is self-contained (doesn't depend on a shared
// header that doesn't exist yet). If these ever diverge, the round-trip tests
// will fail loudly.
static constexpr uint32_t BUNDLE_MAGIC      = 0x454D4244u;
static constexpr uint32_t BUNDLE_FOOTER_SIZE = 12u;

// Write a .ember source file to `path`.
static bool write_source(const std::string& path, const std::string& content) {
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
                  const std::string& output) {
    // ember_bundle <input.ember> <output.exe> --stub <stub.exe>
    std::string cmd = quote(quote(bundler_exe) + " " + quote(source) + " " +
                            quote(output) + " --stub " + quote(stub_exe));
    return run_command(cmd);
}

// Run a bundled exe and return its exit code.
static int run_bundled(const std::string& exe) {
    return run_command(quote(quote(exe)));
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
        "fn main() -> i64 { return helper(21); }\n";
    if (!write_source(src, code)) {
        std::fprintf(stderr, "  multi: failed to write source\n"); return false;
    }
    int brc = bundle(bundler, stub, src, out);
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

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: bundler_test <ember_bundle_exe> <stub_exe>\n");
        return 1;
    }
    const std::string bundler = argv[1];
    const std::string stub = argv[2];

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

    // cleanup
    fs::remove_all(tmp, ec);

    if (!all_ok) {
        std::printf("\n=== FAIL ===\n");
        return 1;
    }
    std::printf("\n=== ALL PASS ===\n");
    return 0;
}
