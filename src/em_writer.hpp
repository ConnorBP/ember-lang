// ember `.em` serializer API (docs/BUNDLING_AND_EM_MODULES.md Section 2.3)
//
// `write_em_file` runs after the full compile pipeline has produced the
// module's per-function code (post-resolve_fixups) + trailing rodata + the
// absolute-imm64 fixup list (x64_emitter.hpp::AbsFixup, captured by the
// emitter's mov_reg_imm64_external per Section 2.4). It writes the on-disk format
// defined in em_file.hpp / docs/BUNDLING_AND_EM_MODULES.md Section 2.2 to `path`.
//
// The serializer never re-derives relocations by scanning bytes - it consumes
// `mod.functions[i].relocs`, which the caller filled from
// `X64Emitter::abs_fixups()` (the read-only view added in Section 2.4). See
// docs/BUNDLING_AND_EM_MODULES.md Section 2.3 step 3.
//
// Load path (the matching reader) is em_loader.{hpp,cpp}; this header only
// defines the writer side so the CLI can link it without pulling the loader.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "em_file.hpp"

namespace ember {

// Serialize `mod` to `path` as a `.em` binary (em_file.hpp /
// docs/BUNDLING_AND_EM_MODULES.md Section 2.2). Returns true on success, false on I/O
// error; on failure `*err` (if non-null) is set to a short human-readable
// message and errno may also be set by the underlying ofstream. The writer
// always emits the current magic (EM_MAGIC); it does not validate it on the way
// out - validation is the loader's job.
//
// `write_em_file` emits an UNSIGNED v3 module (the name directory IS the
// export table from F1; no signature block). v3 is the dev/unsigned artifact;
// the loader accepts it in dev mode (no verification keys) and rejects it in
// signed-only mode. The existing .em round-trip tests + demos use this path
// unchanged.
bool write_em_file(const EmModule& mod, const char* path, std::string* err);

// Serialize `mod` to an in-memory byte buffer (the standalone-exe bundler's
// path — the .em is held in memory and appended to the stub exe, no temp
// file). Same format as `write_em_file` (unsigned v3). Returns true on
// success, false + *err on failure. This is the symmetric companion to
// `load_em_bytes`: the bundler serializes to a buffer, appends it to the
// stub, and the stub loads it from the appended bytes.
bool write_em_bytes(const EmModule& mod, std::vector<uint8_t>& out, std::string* err);

// F2 (docs/spec/SPEC_AUDIT_2026-07-10.md F2): write a SIGNED `.em` v4 module — the
// v3 layout (header -> per-fn records -> globals -> name directory) followed by
// an additive Ed25519 signature block (em_file.hpp `EM_SIG_BLOCK_SIZE`). The
// signature is computed over the v3 content bytes (offset 0 .. end of name
// directory) with the supplied expanded `priv` key + `pub` key (see
// thirdparty/ed25519/ed25519_ember.hpp). The signing key stays OFF the host; the
// host loader gets only `pub` (the verification key). On success a v4 file is
// on disk at `path`; the loader verifies the signature before
// alloc_executable_rw and rejects on mismatch.
//
// `pub` is the 32-byte Ed25519 public key; `priv` is the 64-byte EXPANDED
// private key (SHA-512(seed) || pub), as orlp/ed25519's `ed25519_create_keypair`
// emits. Derive both from a 32-byte seed via `ember::ed25519::keypair_from_seed`.
bool write_em_file_signed(const EmModule& mod, const char* path,
                          const std::array<uint8_t,32>& pub,
                          const std::array<uint8_t,64>& priv,
                          std::string* err);

// v5 (Stage B, IL-.em): write an UNSIGNED v5 module. A v5 module carries IR
// (not raw x86) for IR-serializable functions (EmFunctionRecord::ir_blob
// non-empty -> is_ir=1) and raw-x86 fallback for non-serializable functions
// (ir_blob empty -> is_ir=0). UNSIGNED for Stage B (the v3 "trailing bytes ==
// 0" rule holds; a v5-signed variant is FUTURE work). See em_file.hpp for the
// v5 per-function record layout + the security model.
bool write_em_file_v5(const EmModule& mod, const char* path, std::string* err);

} // namespace ember
