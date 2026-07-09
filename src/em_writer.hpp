// ember `.em` serializer API (BUNDLING_AND_EM_MODULES.md Section 2.3)
//
// `write_em_file` runs after the full compile pipeline has produced the
// module's per-function code (post-resolve_fixups) + trailing rodata + the
// absolute-imm64 fixup list (x64_emitter.hpp::AbsFixup, captured by the
// emitter's mov_reg_imm64_external per Section 2.4). It writes the on-disk format
// defined in em_file.hpp / BUNDLING_AND_EM_MODULES.md Section 2.2 to `path`.
//
// The serializer never re-derives relocations by scanning bytes - it consumes
// `mod.functions[i].relocs`, which the caller filled from
// `X64Emitter::abs_fixups()` (the read-only view added in Section 2.4). See
// BUNDLING_AND_EM_MODULES.md Section 2.3 step 3.
//
// Load path (the matching reader) is em_loader.{hpp,cpp}; this header only
// defines the writer side so the CLI can link it without pulling the loader.

#pragma once

#include <string>

#include "em_file.hpp"

namespace ember {

// Serialize `mod` to `path` as a `.em` binary (em_file.hpp /
// BUNDLING_AND_EM_MODULES.md Section 2.2). Returns true on success, false on I/O
// error; on failure `*err` (if non-null) is set to a short human-readable
// message and errno may also be set by the underlying ofstream. The writer
// always emits the current version (EM_VERSION) and the current magic
// (EM_MAGIC); it does not validate them on the way out - validation is the
// loader's job.
bool write_em_file(const EmModule& mod, const char* path, std::string* err);

} // namespace ember
