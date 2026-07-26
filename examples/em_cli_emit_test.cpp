// End-to-end H12 regression: the public CLI must emit evaluated global bytes.
//
// Spawns the real `ember_cli` as a subprocess (emit-em then run --load-em) and
// checks the loaded module's `main` exit code (42). The subprocess launch is
// POSIX-portable: on macOS/Linux we fork+execvp the CLI directly (no shell, so
// no cmd.exe-style `""path""` quoting that POSIX sh mis-parses as one word),
// then waitpid + decode WIFEXITED/WIFSIGNALED. On Windows the original
// std::system + cmd.exe quoting is retained.
#include "em_loader.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <cstdlib>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/types.h>
#  include <cerrno>
#endif

using namespace ember;

#if !defined(_WIN32)
// Run a child program (argv passed directly to execvp — no shell, so paths
// with spaces / no .exe suffix just work) and return its decoded exit status:
//   WIFEXITED  -> WEXITSTATUS (0..255)
//   WIFSIGNALED-> -(128 + WTERMSIG)   (crash: the negative value encodes the
//                                      signal so the failure report shows it)
//   fork/exec failure -> -1
// stdio is inherited so the child's diagnostics surface in the test output.
static int run_child(const std::vector<std::string>& argv_strs) {
    std::vector<char*> argv(argv_strs.size() + 1);
    for (size_t i = 0; i < argv_strs.size(); ++i)
        argv[i] = const_cast<char*>(argv_strs[i].c_str());
    argv[argv_strs.size()] = nullptr;

    pid_t pid = fork();
    if (pid < 0) return -1;            // fork failed
    if (pid == 0) {                    // child
        execvp(argv[0], argv.data());
        _exit(127);                    // exec failed — 127 == "command not found"
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1; // waitpid hard error
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -(128 + WTERMSIG(status));
    return -1;
}
#endif

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: em_cli_emit_test <ember_cli>\n");
        return 1;
    }

    const auto base = std::filesystem::temp_directory_path() / "ember_cli_emit_global";
    const auto source = base.string() + ".ember";
    const auto module = base.string() + ".em";
    {
        std::ofstream os(source, std::ios::binary);
        os << "global answer : i64 = 33;\n"
              "fn main() -> i64 {\n"
              "  let local_string: string = \"cross-process rodata\";\n"
              "  let joined: string = local_string + \"!\";\n"
              "  let v: vec3 = vec3_new(1.0f, 2.0f, 3.0f) + vec3_new(4.0f, 5.0f, 6.0f);\n"
              "  if (string_length(joined) == 0 || vec3_x(v) == 0.0f) { return 1; }\n"
              "  return answer + (sqrt(81.0f) as i64);\n"
              "}\n";
    }

    int cli_rc = -1;
    int run_rc = -1;

#if defined(_WIN32)
    // cmd.exe strips one enclosing quote pair before parsing a command whose
    // executable path is itself quoted.
    const std::string command = std::string("\"\"") + argv[1] + "\" emit-em \"" +
                                source + "\" \"" + module + "\"\"";
    cli_rc = std::system(command.c_str());
    if (cli_rc == 0) {
        const std::string run_command = std::string("\"\"") + argv[1] + "\" run --load-em \"" +
                                        module + "\" --fn main\"";
        run_rc = std::system(run_command.c_str());
    }
#else
    // POSIX: fork+execvp the CLI directly (no shell quoting). macOS
    // executables have no .exe suffix — argv[1] is used verbatim.
    cli_rc = run_child({argv[1], "emit-em", source, module});
    if (cli_rc == 0)
        run_rc = run_child({argv[1], "run", "--load-em", module, "--fn", "main"});
#endif

    const bool ok = cli_rc == 0 && run_rc == 42;

    std::filesystem::remove(source);
    std::filesystem::remove(module);
    if (!ok) {
        std::fprintf(stderr,
                     "CLI cross-process emit/load/native/global regression failed: emit_rc=%d run_rc=%d\n",
                     cli_rc, run_rc);
        return 1;
    }
    std::puts("CLI cross-process v2 native/overload/string-rodata/global regression: PASS");
    return 0;
}
