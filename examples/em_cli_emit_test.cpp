// End-to-end H12 regression: the public CLI must emit evaluated global bytes.
#include "em_loader.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ember;

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

    // cmd.exe strips one enclosing quote pair before parsing a command whose
    // executable path is itself quoted.
    const std::string command = std::string("\"\"") + argv[1] + "\" emit-em \"" +
                                source + "\" \"" + module + "\"\"";
    const int cli_rc = std::system(command.c_str());

    const std::string run_command = std::string("\"\"") + argv[1] + "\" run --load-em \"" +
                                    module + "\" --fn main\"";
    const int run_rc = cli_rc == 0 ? std::system(run_command.c_str()) : -1;
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
