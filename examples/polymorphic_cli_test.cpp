// polymorphic_cli_test — Red 8 CLI regression for ordinary pipeline profiles.
//
// End-to-end subprocess harness for the `--profile light|balanced|heavy` and
// `--pass-seed <u64>` CLI options added in examples/ember_cli.cpp (Red 8).
// Invokes the real ember_cli.exe as a subprocess and verifies the exit code +
// stdout/stderr for each scenario. Uses CreateProcess (not cmd.exe) with
// stdout/stderr redirected to a temp file, mirroring ember_cli_pipe_live_test.
//
// Built as a standalone executable WITHOUT add_test so the filtered CTest
// total is unchanged (run explicitly: ./buildt/polymorphic_cli_test
// <ember_cli> <source_dir>). Does NOT link ember — it only invokes the CLI as
// a subprocess.
//
// Subtests:
//   PROFILE light      run profile_target.ember --profile light      -> exit 250
//   PROFILE balanced   run profile_target.ember --profile balanced   -> exit 250
//   PROFILE heavy      run profile_target.ember --profile heavy      -> exit 250
//   PROFILE alias      --pass-profile balanced (documented alias)    -> exit 250
//   SEED 0             --profile balanced --pass-seed 0             -> exit 250
//   SEED max           --profile balanced --pass-seed 0xFFFFFFFFFFFFFFFF -> exit 250
//   SEED hex           --profile balanced --pass-seed 0xfeedface    -> exit 250
//   SEED decimal       --profile balanced --pass-seed 42           -> exit 250
//   SEED reproducible  two runs same seed -> same exit (250)
//   NO-PROFILE         run with no profile, no passes -> exit 250 (preserved)
//   PASSES replaces    --profile balanced --passes constprop,dce    -> exit 250
//                     (explicit recipe replaces profile recipe, retains options)
//   UNKNOWN profile    --profile bogus                              -> exit 2 + diagnostic
//   MISSING profile    --profile (no value)                         -> exit 2 + diagnostic
//   MALFORMED seed     --pass-seed abc                              -> exit 2 + diagnostic
//   OVERFLOW seed      --pass-seed 0x10000000000000000              -> exit 2 + diagnostic
//
// Value preservation: every profile + every accepted seed MUST exit 250 (the
// script's documented `// expect: 250`). A profile that breaks value
// preservation changes the exit code, making the break observable.
//
// Usage: polymorphic_cli_test <ember_cli_exe> <source_dir>
//   <ember_cli_exe>  path to the built ember_cli.exe ($<TARGET_FILE:ember_cli>)
//   <source_dir>      the ember source root (so tests/features/ resolves)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) ++g_fail;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

#if defined(_WIN32)
static std::string quote(const std::string& s) {
    if (s.find(' ') != std::string::npos || s.empty()) {
        std::string r = "\"";
        for (char c : s) { if (c == '"') r += '\\'; r += c; }
        r += '"';
        return r;
    }
    return s;
}

static std::string build_cmdline(const std::string& exe,
                                 const std::vector<std::string>& args) {
    std::string line = quote(exe);
    for (const auto& a : args) { line += ' '; line += quote(a); }
    return line;
}

static HANDLE launch_redirect(const std::string& cmdline,
                              const fs::path& out_path) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hOut = CreateFileA(out_path.string().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return nullptr;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int run_sync(const std::string& cmdline, const fs::path& out_path) {
    HANDLE h = launch_redirect(cmdline, out_path);
    if (!h) return -1;
    WaitForSingleObject(h, 60000);
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return int(code);
}
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: polymorphic_cli_test <ember_cli_exe> <source_dir>\n");
        return 1;
    }
    const std::string cli = argv[1];
    const fs::path src = argv[2];
    const fs::path target = src / "tests" / "features" / "profile_target.ember";
    const auto tmp = fs::temp_directory_path() / "ember_polymorphic_cli_test";
    fs::create_directories(tmp);
    auto out_path = tmp / "out.txt";

    std::printf("=== Red 8 polymorphic CLI regression (profiles + seeds) ===\n");
    std::printf("cli: %s\nsrc: %s\ntarget: %s\n\n",
                cli.c_str(), src.string().c_str(), target.string().c_str());

    if (!fs::exists(target)) {
        check(false, "profile_target.ember exists under tests/features/");
        std::printf("\npolymorphic_cli_test: %s\n", g_fail ? "FAIL" : "PASS");
        return g_fail ? 1 : 0;
    }

#if !defined(_WIN32)
    check(false, "Red 8 CLI regression requires Windows CreateProcess (skipped)");
    std::printf("\npolymorphic_cli_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
#else

    // Helper: run the CLI with a vector of extra args, return {rc, stdout+stderr}.
    auto run = [&](const std::vector<std::string>& extra) -> std::pair<int, std::string> {
        std::vector<std::string> args = {"run", target.string()};
        for (const auto& a : extra) args.push_back(a);
        std::string cmd = build_cmdline(cli, args);
        int rc = run_sync(cmd, out_path);
        return {rc, read_file(out_path)};
    };

    // The documented value-preserving exit code for profile_target.ember.
    const int EXPECT = 250;

    // ---------- PROFILE light (value preservation) ----------
    {
        auto [rc, out] = run({"--profile", "light"});
        bool ok = (rc == EXPECT);
        check(ok, "PROFILE light: --profile light -> exit 250 (value preserved)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- PROFILE balanced ----------
    {
        auto [rc, out] = run({"--profile", "balanced"});
        bool ok = (rc == EXPECT);
        check(ok, "PROFILE balanced: --profile balanced -> exit 250 (value preserved)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- PROFILE heavy (experimental, bounded, still value-preserving) ----------
    {
        auto [rc, out] = run({"--profile", "heavy"});
        bool ok = (rc == EXPECT);
        check(ok, "PROFILE heavy: --profile heavy -> exit 250 (experimental but value preserved)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- PROFILE alias: --pass-profile balanced (documented alias) ----------
    {
        auto [rc, out] = run({"--pass-profile", "balanced"});
        bool ok = (rc == EXPECT);
        check(ok, "PROFILE alias: --pass-profile balanced (alias) -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- SEED 0 ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "0"});
        bool ok = (rc == EXPECT);
        check(ok, "SEED 0: --profile balanced --pass-seed 0 -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- SEED UINT64_MAX (0xFFFFFFFFFFFFFFFF) ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "0xFFFFFFFFFFFFFFFF"});
        bool ok = (rc == EXPECT);
        check(ok, "SEED UINT64_MAX: --pass-seed 0xFFFFFFFFFFFFFFFF -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- SEED hex ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "0xfeedface"});
        bool ok = (rc == EXPECT);
        check(ok, "SEED hex: --pass-seed 0xfeedface -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- SEED decimal ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "42"});
        bool ok = (rc == EXPECT);
        check(ok, "SEED decimal: --pass-seed 42 -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- SEED reproducible: two runs same seed -> same exit ----------
    {
        auto [rc1, out1] = run({"--profile", "heavy", "--pass-seed", "0x123456789abcdef0"});
        auto [rc2, out2] = run({"--profile", "heavy", "--pass-seed", "0x123456789abcdef0"});
        bool ok = (rc1 == rc2) && (rc1 == EXPECT);
        check(ok, "SEED reproducible: two runs same seed -> same exit 250");
        if (!ok) std::fprintf(stderr, "  rc1=%d rc2=%d out1=\n%s\nout2=\n%s\n",
                              rc1, rc2, out1.c_str(), out2.c_str());
    }
    // ---------- NO-PROFILE / NO-PASSES behavior preserved ----------
    {
        auto [rc, out] = run({});
        bool ok = (rc == EXPECT);
        check(ok, "NO-PROFILE: run with no profile, no passes -> exit 250 (behavior preserved)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- PASSES replaces profile recipe (retains profile options) ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--passes", "constprop,dce"});
        bool ok = (rc == EXPECT);
        check(ok, "PASSES replaces: --profile balanced --passes constprop,dce -> exit 250");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- UNKNOWN profile -> exit 2 + structured diagnostic ----------
    {
        auto [rc, out] = run({"--profile", "bogus"});
        bool ok = (rc == 2) &&
                  (out.find("profile") != std::string::npos ||
                   out.find("unknown") != std::string::npos ||
                   out.find("not found") != std::string::npos);
        check(ok, "UNKNOWN profile: --profile bogus -> exit 2 + diagnostic");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- MISSING profile value -> exit 2 + diagnostic ----------
    {
        // --profile with no following value: the CLI must reject (exit 2) and
        // print a structured diagnostic, not crash or silently default.
        std::vector<std::string> args = {"run", target.string(), "--profile"};
        std::string cmd = build_cmdline(cli, args);
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        bool ok = (rc == 2) &&
                  (out.find("profile") != std::string::npos ||
                   out.find("needs") != std::string::npos ||
                   out.find("missing") != std::string::npos);
        check(ok, "MISSING profile: --profile (no value) -> exit 2 + diagnostic");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- MALFORMED seed -> exit 2 + diagnostic ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "abc"});
        bool ok = (rc == 2) &&
                  (out.find("seed") != std::string::npos ||
                   out.find("pass-seed") != std::string::npos ||
                   out.find("invalid") != std::string::npos ||
                   out.find("malformed") != std::string::npos);
        check(ok, "MALFORMED seed: --pass-seed abc -> exit 2 + diagnostic");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- OVERFLOW seed -> exit 2 + diagnostic ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "0x10000000000000000"});
        bool ok = (rc == 2) &&
                  (out.find("seed") != std::string::npos ||
                   out.find("pass-seed") != std::string::npos ||
                   out.find("overflow") != std::string::npos ||
                   out.find("invalid") != std::string::npos ||
                   out.find("range") != std::string::npos);
        check(ok, "OVERFLOW seed: --pass-seed 0x10000000000000000 -> exit 2 + diagnostic");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }
    // ---------- NEGATIVE seed -> exit 2 + diagnostic ----------
    {
        auto [rc, out] = run({"--profile", "balanced", "--pass-seed", "-1"});
        bool ok = (rc == 2);
        check(ok, "NEGATIVE seed: --pass-seed -1 -> exit 2 (rejected)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // cleanup
    fs::remove(out_path);
    std::error_code ec;
    fs::remove(tmp, ec);

    std::printf("\npolymorphic_cli_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
#endif
}
