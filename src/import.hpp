// ember include resolver - textual file inclusion (the `include`/bundle
// mechanism per docs/BUNDLING_AND_EM_MODULES.md Section 1.2; not a C preprocessor -
// no #define/#ifdef, just `include "path";` inlined before lexing).
// docs/ROADMAP.md Tier 6 live `import` (runtime multi-module linking) is a
// future refinement; this is the v1 single-module bundle shape.
#pragma once
#include <string>
#include <unordered_set>

namespace ember {

// Resolve all `import "path";` directives in `src` by inlining file
// contents recursively. `base_dir` is the directory to resolve relative
// paths against (typically the importing file's directory). Returns the
// fully-inlined source. On a missing file, throws std::runtime_error.
// Cycle detection: a file imported twice is only inlined once (idempotent),
// and import cycles are rejected (throws).
std::string resolve_imports(const std::string& src, const std::string& base_dir,
                            std::unordered_set<std::string>& seen);

} // namespace ember
