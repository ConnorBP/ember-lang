// ember_check - parse-only check exe for the lang regression suite.
//
// Reads a .ember file from argv[1], resolves its `import "path";` directives
// (src/import.hpp) against the file's own directory, then tokenizes
// (src/lexer.hpp) and parses (src/parser.hpp). Exits 0 if parse OK, nonzero
// with the error on stderr if any stage fails (lex / parse / import-resolve).
//
// Modeled on em_roundtrip_test's lex+parse section (the canonical parse path)
// and prism's examples/ember_check.cpp (the runner-facing shell). This is the
// PARSE half of the lang suite: every valid_*/sema_valid_*/sema_invalid_* case
// must exit 0 here (they parse fine - sema_invalid_* only fails later at sema);
// every invalid_* case must exit nonzero here (syntactic errors).
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "safety.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ember_check <file.ember>\n");
        return 2;
    }

    std::ifstream f(argv[1]);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    // resolve imports relative to the input file's own directory (so the
    // import_* tests find their lib/lib2/lib3/sub support files).
    std::string base_dir = std::filesystem::path(argv[1]).parent_path().string();
    std::unordered_set<std::string> seen;
    try {
        src = ember::resolve_imports(src, base_dir, seen);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "IMPORT_ERROR: %s\n", e.what());
        return 1;
    }

    auto lr = ember::tokenize(src, argv[1]);
    if (!lr.ok) {
        std::fprintf(stderr, "LEX_ERROR: %s\n", lr.error.c_str());
        return 1;
    }

    ember::ParseResult pr;
    try {
        pr = ember::parse(std::move(lr.toks));
    } catch (const ember::safety::DepthLimitExceeded& e) {
        std::fprintf(stderr, "PARSE_ERROR: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "PARSE_ERROR: %s\n", e.what());
        return 1;
    }
    if (!pr.ok) {
        std::fprintf(stderr, "PARSE_ERROR:\n%s", pr.error.c_str());
        return 1;
    }

    std::printf("OK: %zu funcs, %zu structs, %zu globals\n",
                pr.program.funcs.size(), pr.program.structs.size(),
                pr.program.globals.size());
    return 0;
}
