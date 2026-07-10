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
        os << "global answer : i64 = 42;\nfn main() -> i64 { return answer; }\n";
    }

    // cmd.exe strips one enclosing quote pair before parsing a command whose
    // executable path is itself quoted.
    const std::string command = std::string("\"\"") + argv[1] + "\" emit-em \"" +
                                source + "\" \"" + module + "\"\"";
    const int cli_rc = std::system(command.c_str());

    LoadedModule loaded;
    std::string err;
    const bool ok = cli_rc == 0 && load_em_file(module.c_str(), loaded, &err);
    uint64_t value = 0;
    if (ok && loaded.globals.size() >= 8) {
        for (unsigned i = 0; i < 8; ++i)
            value |= uint64_t(loaded.globals[i]) << (8 * i);
    }

    std::filesystem::remove(source);
    std::filesystem::remove(module);
    if (!ok || loaded.globals.size() != 8 || value != 42 || !loaded.entry()) {
        std::fprintf(stderr,
                     "CLI emit-em global regression failed: cli_rc=%d load=%d globals=%zu value=%llu err=%s\n",
                     cli_rc, int(ok), loaded.globals.size(),
                     static_cast<unsigned long long>(value), err.c_str());
        return 1;
    }
    std::puts("CLI emit-em initialized-global regression: PASS");
    return 0;
}
