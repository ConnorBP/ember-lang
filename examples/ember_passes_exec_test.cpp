// ember_passes_exec_test.cpp — end-to-end execution gate for the IR-backend
// (--passes) path of the public CLI.
//
// Reproduces and permanently regress-guards the loop-carried accumulator defect
// that surfaced as valid_unroll.ember --passes dce returning 26 (the for-loop's
// loop-carried accumulator was dropped / the loop variable escaped its back
// edge) instead of 56. The root cause was in the Stage-3 frame-slot promotion
// consumed by emit_x64 (a promoted VReg reused across instructions was reloaded
// from its stale frame slot instead of the promoted callee-saved register);
// the fix lives in src/thin_emit.cpp load_int_vreg. This test pins the
// end-to-end behavior so a regression is reported as a CTest failure.
//
// Reusable CMake test driver: invoked as
//     ember_passes_exec_test <ember_cli> <source.ember> <expected_exit> <passes_spec>
// It shells out to `<ember_cli> run <source.ember> --fn main --passes <passes_spec>`
// and compares the process exit code to <expected_exit>. A wrong exit code
// (including a runtime-trap exit 70 from an infinite loop, or the wrong
// computed value) is reported as a CTest failure (this driver exits non-zero).
//
// Modeled on em_cli_emit_test.cpp (the cross-process std::system pattern +
// cmd.exe double-quote handling).

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: ember_passes_exec_test <ember_cli> <source.ember> "
                     "<expected_exit> <passes_spec>\n");
        return 1;
    }

    const char* ember_cli = argv[1];
    const char* source = argv[2];
    int expected_exit = 0;
    {
        char* endp = nullptr;
        long v = std::strtol(argv[3], &endp, 10);
        if (endp == argv[3] || *endp != '\0') {
            std::fprintf(stderr, "ember_passes_exec_test: bad expected_exit '%s'\n", argv[3]);
            return 1;
        }
        expected_exit = int(v);
    }
    const char* passes_spec = argv[4];

    // cmd.exe strips one enclosing quote pair before parsing a command whose
    // executable path is itself quoted — wrap the whole command in an extra
    // "" pair (same trick as em_cli_emit_test). The passes spec contains only
    // commas (no spaces), so it needs no quoting.
    const std::string command = std::string("\"\"") + ember_cli + "\" run \"" +
                                source + "\" --fn main --passes " + passes_spec + "\"";
    const int rc = std::system(command.c_str());

    // std::system returns the child exit code on this MinGW build (mirrors
    // em_cli_emit_test, which compares run_rc == 42 and passes under ctest).
    const int actual_exit = rc;

    if (actual_exit != expected_exit) {
        std::fprintf(stderr,
                     "FAIL: %s --passes %s: expected exit %d, got %d\n",
                     source, passes_spec, expected_exit, actual_exit);
        return 1;
    }
    std::printf("PASS: %s --passes %s -> exit %d (expected %d)\n",
                source, passes_spec, actual_exit, expected_exit);
    return 0;
}
