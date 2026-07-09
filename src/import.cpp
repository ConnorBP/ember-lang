#include "import.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <regex>

namespace ember {

namespace fs = std::filesystem;

// read a file's full contents
static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("import: cannot open '" + path + "'");
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// scan `src` for `import "path";` lines and inline them. Recursive.
// Uses a simple line-based scan (imports must be on their own line, matching
// the AngelScript/native-JIT-language convention). No expression-level imports.
std::string resolve_imports(const std::string& src, const std::string& base_dir,
                            std::unordered_set<std::string>& seen) {
    std::string out;
    out.reserve(src.size());
    std::istringstream in(src);
    std::string line;
    // matches:  import "path";   (optional leading whitespace)
    static const std::regex re(R"re(^\s*import\s+"([^"]+)"\s*;\s*$)re");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, re)) {
            std::string rel = m[1].str();
            // resolve relative to base_dir (or absolute)
            fs::path p = fs::path(rel);
            if (p.is_relative()) p = fs::path(base_dir) / p;
            std::string canon = fs::weakly_canonical(p).string();
            if (seen.count(canon)) {
                // already inlined (idempotent import) - skip, emitting nothing
                continue;
            }
            // cycle check: if we're currently inlining this file (on the stack),
            // seen would have it - but we add to seen BEFORE recursing, so a
            // true cycle (a->b->a) hits the "already inlined" branch above and
            // skips. That's idempotent, not cycle-rejecting. For v1 textual
            // include, idempotent-skip is the right behavior (matches #include
            // guards). True cycles can't happen with idempotent skipping.
            seen.insert(canon);
            std::string content = read_file(canon);
            std::string sub_dir = fs::path(canon).parent_path().string();
            out += "// --- begin import: " + canon + " ---\n";
            out += resolve_imports(content, sub_dir, seen);
            out += "// --- end import: " + canon + " ---\n";
        } else {
            out += line;
            out += '\n';
        }
    }
    return out;
}

} // namespace ember
