// ember_cli_pipe_live_test — Family C CLI regression (ember pipe + ember live).
//
// End-to-end test harness for the two Family C CLI subcommands added in
// examples/ember_cli.cpp (docs/ROADMAP.md Family C). Invokes the real
// ember_cli.exe as a subprocess and verifies both the exit code and the stdout
// output of each scenario. Uses CreateProcess (not cmd.exe) with stdout/stderr
// redirected to a temp file, so paths with spaces and quoting are handled by
// the Windows command-line parser directly (no cmd.exe quote-stripping quirks).
//
// Subtests:
//   PIPE 2-stage      pipe_a.process -> pipe_b.reduce, input 1..5 -> exit 60
//   PIPE 3-stage      process -> reduce -> square, input 1..5 -> exit 810
//   PIPE empty stream input 1..0 -> exit 0 (no stage calls)
//   PIPE missing mod  config referencing a nonexistent module -> exit 2 (error)
//   PIPE .em bundler  emit-em a stage to .em, pipe through the .em module -> exit 9
//   LIVE tick loop    live_tick.ember --tick-count 3 -> stdout has tick 1:11,2:12,3:13
//   LIVE file change  v1 -> overwrite v2 mid-run -> stdout has BOTH v1 + v2 values + reload
//
// Usage: ember_cli_pipe_live_test <ember_cli_exe> <source_dir>
//   <ember_cli_exe>  path to the built ember_cli.exe ($<TARGET_FILE:ember_cli>)
//   <source_dir>      the ember source root (so tests/features/ resolves)
//
// Build: a standalone exe (links only the C++ stdlib + Windows CreateProcess).
//        Does NOT link ember — it only invokes the CLI as a subprocess, like
//        em_cli_emit_test. The Windows-only CreateProcess is consistent with
//        the rest of the tree (ember_cli uses <conio.h>, jit_memory uses
//        VirtualAlloc). Non-Windows builds compile to a skip-and-pass shell so
//        the ctest registration is portable.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
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

static void write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

#if defined(_WIN32)
// Quote a path for the Windows command line (wrap in quotes if it contains a
// space or is empty; otherwise return as-is). CreateProcess parses a quoted
// token as a single argv element.
static std::string quote(const std::string& s) {
    if (s.find(' ') != std::string::npos || s.empty()) {
        std::string r = "\"";
        for (char c : s) { if (c == '"') r += '\\'; r += c; }
        r += '"';
        return r;
    }
    return s;
}

// Build a Windows command line from an exe path + an args vector. The exe path
// is quoted per CreateProcess rules (the first token; if it contains spaces it
// MUST be quoted, and CreateProcess keeps the quotes as part of the token only
// if the application path needs them).
static std::string build_cmdline(const std::string& exe, const std::vector<std::string>& args) {
    std::string line = quote(exe);
    for (const auto& a : args) { line += ' '; line += quote(a); }
    return line;
}

// Launch a process with stdout+stderr redirected to `out_path`. Returns the
// process handle (null on failure). The caller waits (WaitForSingleObject) +
// closes it. CREATE_NO_WINDOW suppresses any console pop-up.
static HANDLE launch_redirect(const std::string& cmdline, const fs::path& out_path) {
    // Open the output file for writing (inherited by the child as stdout/stderr).
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };  // inheritable handle
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
    CloseHandle(hOut);  // parent closes its copy; child keeps its inherited one
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Run a process synchronously, redirecting stdout+stderr to a temp file, and
// return its exit code (or -1 on launch failure). `out_path` receives the
// captured output (the caller reads it after this returns).
static int run_sync(const std::string& cmdline, const fs::path& out_path) {
    HANDLE h = launch_redirect(cmdline, out_path);
    if (!h) return -1;
    WaitForSingleObject(h, 60000);  // 60s cap (tests are sub-second)
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return int(code);
}
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ember_cli_pipe_live_test <ember_cli_exe> <source_dir>\n");
        return 1;
    }
    const std::string cli = argv[1];
    const fs::path src = argv[2];
    const fs::path features = src / "tests" / "features";
    const auto tmp = fs::temp_directory_path() / "ember_pipe_live_test";
    fs::create_directories(tmp);
    auto out_path = tmp / "out.txt";

    std::printf("=== ember pipe + ember live CLI regression (Family C) ===\n");
    std::printf("cli: %s\nsrc: %s\n\n", cli.c_str(), src.string().c_str());

#if !defined(_WIN32)
    check(false, "Family C CLI regression requires Windows CreateProcess (skipped)");
    std::printf("\nember pipe + ember live CLI regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
#else

    // ---------- PIPE: 2-stage (correct output) ----------
    {
        std::string cmd = build_cmdline(cli, { "pipe", (features / "pipe_2stage.pipe").string() });
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        bool ok = (rc == 60) && out.find("pipe result sum: 60") != std::string::npos;
        check(ok, "PIPE 2-stage: pipe_a.process -> pipe_b.reduce, input 1..5, exit 60 + sum line");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // ---------- PIPE: 3-stage chain (correct output) ----------
    // sum = 36+81+144+225+324 = 810. CreateProcess + GetExitCodeProcess
    // returns the FULL 32-bit exit code (unlike std::system/cmd.exe which
    // truncate to 8 bits), so the expected exit is 810 (not 810 mod 256).
    {
        std::string cmd = build_cmdline(cli, { "pipe", (features / "pipe_3stage.pipe").string() });
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        bool ok = (rc == 810) && out.find("pipe result sum: 810") != std::string::npos;
        check(ok, "PIPE 3-stage: process -> reduce -> square, input 1..5, exit 810 + sum line");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // ---------- PIPE: empty stream (edge case) ----------
    {
        std::string cmd = build_cmdline(cli, { "pipe", (features / "pipe_empty.pipe").string() });
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        bool ok = (rc == 0) && out.find("pipe result sum: 0") != std::string::npos;
        check(ok, "PIPE empty stream: input 1..0, exit 0 + sum:0 (no stage calls)");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // ---------- PIPE: missing module (error handling) ----------
    {
        std::string cmd = build_cmdline(cli, { "pipe", (features / "pipe_missing.pipe").string() });
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        // exit 2 (error) + a diagnostic naming the failed module/file.
        bool ok = (rc == 2) && (out.find("pipe_nonexistent_module") != std::string::npos ||
                                out.find("no such file") != std::string::npos ||
                                out.find("failed") != std::string::npos);
        check(ok, "PIPE missing module: nonexistent stage module, exit 2 + diagnostic");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // ---------- PIPE: .em bundler exercise ----------
    // Pre-compile pipe_a.ember to a .em via `ember emit-em`, then run a pipe
    // config that loads the .em as stage A. Exercises the bundler (load_em_file)
    // path inside `ember pipe`. pipe_a.process adds 1, so with a single-stage
    // config input 1..3: 1->2, 2->3, 3->4, sum=9, exit 9.
    {
        auto a_em = tmp / "pipe_a.em";
        std::string emit_cmd = build_cmdline(cli, { "emit-em",
            (features / "pipe_a.ember").string(), a_em.string() });
        int erc = run_sync(emit_cmd, out_path);
        std::string eout = read_file(out_path);
        bool emit_ok = (erc == 0) && fs::exists(a_em) &&
                       eout.find("wrote") != std::string::npos;
        check(emit_ok, "PIPE .em bundler: emit-em pipe_a.ember -> pipe_a.em");
        if (!emit_ok) std::fprintf(stderr, "  erc=%d out=\n%s\n", erc, eout.c_str());

        // write a pipe config that uses the .em as stage A (single stage).
        auto em_cfg = tmp / "pipe_em.pipe";
        write_file(em_cfg,
            "# auto: .em-bundler pipe config\n"
            "module A " + a_em.string() + "\n"
            "stage A::process\n"
            "input 1 3\n");
        std::string pipe_cmd = build_cmdline(cli, { "pipe", em_cfg.string() });
        int rc = run_sync(pipe_cmd, out_path);
        std::string out = read_file(out_path);
        // 1->2, 2->3, 3->4, sum=9, exit 9.
        bool ok = (rc == 9) && out.find("pipe result sum: 9") != std::string::npos;
        check(ok, "PIPE .em bundler: pipe through the loaded .em stage A::process, exit 9");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
        fs::remove(a_em);
        fs::remove(em_cfg);
    }

    // ---------- LIVE: tick loop (correct tick output) ----------
    // Run live_tick.ember for 3 ticks. The @on_tick advances a counter and
    // returns 10+count, so the output must contain "tick 1: 11", "tick 2: 12",
    // "tick 3: 13" and the stop line.
    {
        std::string cmd = build_cmdline(cli, { "live",
            (features / "live_tick.ember").string(),
            "--tick-count", "3", "--tick-interval", "5", "--poll-ms", "5" });
        int rc = run_sync(cmd, out_path);
        std::string out = read_file(out_path);
        bool ok = (rc == 0) &&
                  out.find("tick 1: 11") != std::string::npos &&
                  out.find("tick 2: 12") != std::string::npos &&
                  out.find("tick 3: 13") != std::string::npos &&
                  out.find("stopped after 3 ticks") != std::string::npos;
        check(ok, "LIVE tick loop: live_tick.ember --tick-count 3 -> tick 1:11, 2:12, 3:13 + stop line");
        if (!ok) std::fprintf(stderr, "  rc=%d out=\n%s\n", rc, out.c_str());
    }

    // ---------- LIVE: file change (recompile + new output) ----------
    // Write v1 (returns 111 each tick) to a temp file, launch `ember live` in
    // the background (tick-count 40, 5ms tick, 5ms poll), let it tick v1 a few
    // times, overwrite with v2 (returns 222 each tick), wait for it to finish,
    // and verify the captured stdout has BOTH v1's 111 and v2's 222 plus a
    // reload marker ("recompiling" / "reloaded").
    {
        auto live_file = tmp / "live_change.ember";
        const std::string v1 =
            "global c : i64 = 0;\n"
            "@on_tick\n"
            "fn tick() -> i64 {\n"
            "    c = c + 1;\n"
            "    return 111;\n"
            "}\n";
        const std::string v2 =
            "global c : i64 = 0;\n"
            "@on_tick\n"
            "fn tick() -> i64 {\n"
            "    c = c + 1;\n"
            "    return 222;\n"
            "}\n";
        write_file(live_file, v1);
        auto live_out = tmp / "live_out.txt";
        std::string cmd = build_cmdline(cli, { "live", live_file.string(),
            "--tick-count", "40", "--tick-interval", "5", "--poll-ms", "5" });
        HANDLE hProc = launch_redirect(cmd, live_out);
        check(hProc != nullptr, "LIVE file change: background ember live launched");
        if (hProc) {
            // let v1 tick ~12 times (60ms at 5ms/tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            // overwrite with v2 — the live process's mtime poll detects the change
            write_file(live_file, v2);
            // wait for it to finish (auto-stops at tick 40). 40 ticks * 5ms = 200ms
            // + reload recompile; allow up to 10s.
            WaitForSingleObject(hProc, 10000);
            CloseHandle(hProc);
        }
        std::string out = read_file(live_out);
        bool has_v1 = out.find("111") != std::string::npos;
        bool has_v2 = out.find("222") != std::string::npos;
        bool has_reload = out.find("recompiling") != std::string::npos ||
                          out.find("reloaded") != std::string::npos;
        bool ok = has_v1 && has_v2 && has_reload;
        check(ok, "LIVE file change: v1(111) -> overwrite v2(222) -> stdout has BOTH + reload marker");
        if (!ok) std::fprintf(stderr, "  v1=%d v2=%d reload=%d out=\n%s\n",
                              (int)has_v1, (int)has_v2, (int)has_reload, out.c_str());
        fs::remove(live_file);
        fs::remove(live_out);
    }

    // cleanup
    fs::remove(out_path);
    fs::remove(tmp);

    std::printf("\nember pipe + ember live CLI regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
#endif
}
