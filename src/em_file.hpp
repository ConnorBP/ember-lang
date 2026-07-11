// ember `.em` binary format: shared constants + on-disk structs
// (docs/BUNDLING_AND_EM_MODULES.md Section 2.2)
//
// This header is the contract between the serializer (em_writer.{hpp,cpp})
// and the loader (em_loader.{hpp,cpp}) and the CLI. The names below are a
// contract: other workers consume `EmModule` / `EmFunctionRecord` /
// `EmReloc` and the `EM_*` constants directly, so do not rename them without
// coordinating.
//
// Format summary (little-endian throughout, x86-64 host):
//
//   Header (40 bytes):
//     magic           : u32 = 0x454D424C  ("EMBL")
//     version         : u32 = 4 (default writer version; loader also accepts
//                       historical v1, v2, v3, and v5 once the Stage-B
//                       writer/loader land — see the v5 per-function record
//                       below)
//     flags           : u32 = 0           (bit 0 reserved: embeds source)
//     function_count  : u32
//     global_size     : u32
//     rodata_total    : u32               (sum of per-fn rodata_size)
//     entry_slot      : u32               (slot of @entry fn, or EM_NO_ENTRY)
//     v2_build_id     : u64 (occupies historical reserved[0..1])
//     v2_target_abi   : u32 (occupies historical reserved[2])
//
//   Per-function (repeated function_count times):
//     name_len        : u16
//     name            : bytes[name_len]   (UTF-8, no NUL)
//     slot_index      : u32
//     code_size       : u32
//     rodata_size     : u32
//     code            : bytes[code_size]
//     rodata          : bytes[rodata_size]
//     reloc_count     : u32
//     relocs          : reloc[reloc_count]
//       v2 reloc { offset: u32, kind: u8, addend: u32 }
//     canonical export signature (v2)
//     symbolic native binding records (v2)
//
//   v5 per-function record (header version=5; ADDITIVE redesign of the per-fn
//   body ONLY — the 40-byte header, globals block, and name directory are
//   UNCHANGED from v3/v4. A v5 module may MIX IR and raw-x86 functions per-fn
//   ("mixed mode"): IR-serializable fns ship IR; non-serializable fns —
//   aggregate/string/struct/defer gaps, flagged by ThinFunction::non_serializable
//   — ship raw x86. This is the on-disk contract the v5 writer and v5 loader
//   independently implement):
//     name_len        : u16
//     name            : bytes[name_len]   (UTF-8, no NUL)
//     slot_index      : u32
//     is_ir           : u8   (1 = IR function; 0 = raw-x86 fallback)
//     signature       : canonical export signature (SAME encoding as v2+:
//                       emit_type(ret) + u32 param_count + emit_type per param)
//     if is_ir == 1:  // IR function — carries IR, NOT machine code
//       ir_blob_len   : u32
//       ir_blob       : bytes[ir_blob_len]  (OPAQUE to the .em container =
//                       serialize_thin_function output, src/thin_ir_ser.hpp;
//                       parsed ONLY by the IR deserializer, never by the .em
//                       loader). NO code/rodata/relocs/native_bindings fields
//                       follow.
//     if is_ir == 0:  // raw-x86 fallback (byte-identical to the v4 per-fn
//                     // body AFTER the is_ir byte; the signature was hoisted
//                     // above the branch in v5):
//       code_size     : u32, rodata_size : u32, code : bytes, rodata : bytes,
//       reloc_count   : u32, relocs[reloc_count] { offset:u32, kind:u8,
//                       addend:u32 },
//       native_binding_count : u32, native_bindings (SAME as v2+)
//
//   Globals block:    bytes[global_size]
//
//   Name directory (v3: the EXPORT TABLE — only is_exported fns):
//     name_table_count : u32
//     entries: [ { name_len: u16, name: bytes, slot_index: u32 } ... ]
//
//   Signature block (v4 ONLY — additive, after the name directory):
//     sig_magic       : u32 = 0x454D5347  ("EMSG" - ember signature sentinel)
//     payload_len     : u32  (byte length of the SIGNED payload, i.e. the
//                            offset where the signature block starts; the
//                            verifier recomputes this as "end of name dir"
//                            and cross-checks it so a truncated/lying block is
//                            rejected)
//     pubkey_id       : u8[32]  (the public key the signature is bound to; lets
//                            a host with a keyring pick the matching key and
//                            reject an untrusted-key signature with a clear
//                            error instead of generic "verify failed")
//     signature       : u8[64]  (Ed25519 signature over bytes[0 .. payload_len))
//
// `reloc.kind` mirrors `AbsFixup::Kind` (x64_emitter.hpp): 0 =
// DispatchTableBase (patch imm64 -> &this module's DispatchTable), 1 =
// GlobalsBase (patch imm64 -> &this module's globals block), 2 =
// ModuleRegistryBase (reserved; used only if cross-module import lands). The
// `kind` space is versioned with the format (docs/BUNDLING_AND_EM_MODULES.md Section 2.7);
// a new kind requires a version bump.
//
// COMPATIBILITY NOTE: v1 remains ABI/process-trusted. v2 records canonical
// signatures, symbolic native bindings and function-local rodata relocations;
// unsupported trap/allowlist process state is rejected by the writer. v2 also
// binds to a deterministic compiler/build identity and target ABI. v3 (F1,
// docs/spec/SPEC_AUDIT_2026-07-10.md F1) keeps the v2 per-function record layout
// byte-identical and repurposes the name directory as the module's EXPORT TABLE
// (only `pub fn`/bare-`fn` entries; `priv fn` helpers are serialized but absent
// from the directory, so they are not callable cross-module). v4 (F2,
// docs/spec/SPEC_AUDIT_2026-07-10.md F2) keeps the v3 layout byte-identical and
// appends an additive Ed25519 SIGNATURE BLOCK after the name directory. The
// loader verifies the signature over the v3 content (header -> end of name
// directory) BEFORE alloc_executable_rw and rejects the module on mismatch, so
// a maliciously-modified `.em` is rejected rather than executed (the build_id /
// abi_hash are still the compatibility check; the signature is the CONTENT
// authentication the v2/v3 identity hash is NOT). The loader accepts v1, v2,
// v3, and v4. v1/v2/v3 carry NO signature (the v3 "trailing bytes == 0" check
// still holds); v4 carries exactly one signature block.
//
// v5 (Stage B, IL-.em: docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.3) keeps the
// 40-byte header byte-identical with version=5 (reserved[0..1]=EM_BUILD_ID,
// reserved[2]=EM_TARGET_ABI_HASH, same as v2+) and keeps the globals block +
// name directory (export table) exactly as v3/v4. It redesigns ONLY the per-
// function record: an `is_ir` byte selects between an IR blob (the output of
// serialize_thin_function, src/thin_ir_ser.hpp — OPAQUE to the .em container)
// and a raw-x86 fallback (the v4 per-fn body). v5 is UNSIGNED for Stage B (NO
// signature block) — the v3 "trailing bytes == 0" rule still holds. A v5-
// signed variant (v5 + the additive Ed25519 block) is explicitly FUTURE work;
// do not implement it in Stage B.
//
// v5 SECURITY MODEL: a v5 .em carries IR, NOT machine code. The loader
// deserializes + validates the IR (structural well-formedness + a type-check
// against the host native table) and re-lowers it to x86 via emit_x64
// (src/thin_emit.hpp) BEFORE alloc_executable_rw — so a tampered or malformed
// v5 .em is REJECTED at IR validation with NO executable page allocated (the
// raw-x86 fallback path is the v4 path, which still verifies its v4 signature
// first when a keyring is present). v5 is OFF-BY-DEFAULT and INERT until the
// Stage-B writer/loader land: EM_VERSION stays 4 (the default writer version),
// and no v5 code path exists in em_writer.cpp / em_loader.cpp yet. The
// constants + the ir_blob field + this layout spec are the contract those
// parallel chunks implement against.
//
// KEY MANAGEMENT (F2): the signing key stays OFF the host (the build tool that
// emits `.em` signs it). The loader takes a set of TRUSTED verification public
// keys (a keyring). Non-empty keyring -> SIGNED-ONLY (v1/v2/v3 unsigned modules
// rejected; v4 loads only if its signature verifies). Empty keyring -> DEV MODE
// (unsigned v1/v2/v3 accepted; v4 rejected with a clear error — a v4 module IS
// signed, so running it unverified is worse than honest unsigned dev code).
// Mirrors secure-boot: keys present == signed-only; keys absent == unsigned
// dev OK. See docs/spec/SAFETY_AND_SANDBOX.md §1 + docs/BUNDLING_AND_EM_MODULES.md
// §2.5.1.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"

namespace ember {

// ---- on-disk identity ----

// "EMBL" - ember bundle, loadable. Read as a little-endian u32 this is
// 'E','M','B','L' = 0x4C,0x42,0x4D,0x45 -> 0x454D424C.
constexpr uint32_t EM_MAGIC    = 0x454D424Cu;
constexpr uint32_t EM_VERSION  = 4u;       // v4: v3 layout + Ed25519 signature block (F2 content authentication)
constexpr uint32_t EM_VERSION_V5 = 5u;     // v5 (Stage B, IL-.em): per-function IR blob (is_ir byte + ir_blob) OR
                                            // raw-x86 fallback; carries IR NOT machine code (see the v5 per-function
                                            // record + SECURITY MODEL below). UNSIGNED for Stage B; a v5-signed variant
                                            // (v5 + Ed25519 block) is FUTURE work. INERT until the Stage-B writer/
                                            // loader land — EM_VERSION stays 4 (the default writer version); no v5
                                            // code path exists in em_writer.cpp / em_loader.cpp yet.
constexpr uint32_t EM_VERSION_V3 = 3u;     // historical v3: name directory IS the export table (F1 pub/priv visibility)
constexpr uint32_t EM_VERSION_V2 = 2u;     // historical v2: canonical signatures, native bindings, rodata relocs
constexpr uint32_t EM_VERSION_V1 = 1u;     // historical v1: ABI-trusted, unknown signatures
constexpr uint32_t EM_NO_ENTRY = 0xFFFFFFFFu; // entry_slot when no @entry fn

// v4 signature-block sentinel ("EMSG" = ember signature). Read as a LE u32:
// 'E','M','S','G' = 0x47,0x53,0x4D,0x45 -> 0x454D5347. The signature block is
// appended AFTER the name directory in a v4 module; this sentinel lets a
// reader distinguish a v4 trailing block from a corrupt v3 file (whose
// trailing-bytes check is still == 0). docs/spec/SPEC_AUDIT_2026-07-10.md F2.
constexpr uint32_t EM_SIG_MAGIC = 0x454D5347u;
// On-disk size of the additive v4 signature block: 4 (sig_magic) + 4
// (payload_len) + 32 (pubkey_id) + 64 (signature) = 104 bytes.
constexpr uint32_t EM_SIG_BLOCK_SIZE = 104u;
constexpr uint32_t EM_SIG_PUBKEY_SIZE  = 32u;
constexpr uint32_t EM_SIG_SIGNATURE_SIZE = 64u;

// v2 binding identity. These are deliberately derived from stable compiler /
// format-ABI facts, never a timestamp, so separately launched processes built
// from the same compiler artifact agree. The three formerly-reserved header
// words carry build-id low/high and target ABI hash in v2.
constexpr uint64_t em_fnv1a64(const char* s, uint64_t h = 1469598103934665603ull) {
    return *s ? em_fnv1a64(s + 1, (h ^ static_cast<uint8_t>(*s)) * 1099511628211ull) : h;
}
#define EMBER_EM_STR2(x) #x
#define EMBER_EM_STR(x) EMBER_EM_STR2(x)
#if defined(__clang__)
constexpr uint64_t EM_BUILD_ID = em_fnv1a64("ember-em-v2;clang=" EMBER_EM_STR(__clang_major__) "." EMBER_EM_STR(__clang_minor__) "." EMBER_EM_STR(__clang_patchlevel__));
#elif defined(_MSC_VER)
constexpr uint64_t EM_BUILD_ID = em_fnv1a64("ember-em-v2;msvc=" EMBER_EM_STR(_MSC_VER));
#elif defined(__GNUC__)
constexpr uint64_t EM_BUILD_ID = em_fnv1a64("ember-em-v2;gcc=" EMBER_EM_STR(__GNUC__) "." EMBER_EM_STR(__GNUC_MINOR__) "." EMBER_EM_STR(__GNUC_PATCHLEVEL__));
#else
constexpr uint64_t EM_BUILD_ID = em_fnv1a64("ember-em-v2;compiler=unknown");
#endif
// Encode every native-code compatibility dimension independently. In
// particular MSVC spells x64 `_M_X64`, not `__x86_64__`.
#if defined(_M_X64) || defined(__x86_64__)
#define EMBER_EM_ARCH "arch=x86_64;"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define EMBER_EM_ARCH "arch=arm64;"
#elif defined(_M_IX86) || defined(__i386__)
#define EMBER_EM_ARCH "arch=x86;"
#else
#define EMBER_EM_ARCH "arch=unknown;"
#endif
#if INTPTR_MAX == INT64_MAX
#define EMBER_EM_PTR "ptr=64;"
#elif INTPTR_MAX == INT32_MAX
#define EMBER_EM_PTR "ptr=32;"
#else
#define EMBER_EM_PTR "ptr=unknown;"
#endif
#if defined(_WIN32)
#define EMBER_EM_OS "os=windows;"
#elif defined(__linux__)
#define EMBER_EM_OS "os=linux;"
#elif defined(__APPLE__)
#define EMBER_EM_OS "os=darwin;"
#elif defined(__FreeBSD__)
#define EMBER_EM_OS "os=freebsd;"
#else
#define EMBER_EM_OS "os=unknown;"
#endif
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
#define EMBER_EM_CC "cc=win64-x64;"
#elif !defined(_WIN32) && defined(__x86_64__)
#define EMBER_EM_CC "cc=sysv-x64;"
#elif !defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define EMBER_EM_CC "cc=aapcs64;"
#elif defined(_WIN32)
#define EMBER_EM_CC "cc=windows-other;"
#else
#define EMBER_EM_CC "cc=unknown;"
#endif
constexpr uint32_t EM_TARGET_ABI_HASH = static_cast<uint32_t>(em_fnv1a64(
    EMBER_EM_ARCH EMBER_EM_PTR EMBER_EM_OS EMBER_EM_CC
    "code=x64-v1;type=ember-type-v1;reloc=em-v2"));
#undef EMBER_EM_ARCH
#undef EMBER_EM_PTR
#undef EMBER_EM_OS
#undef EMBER_EM_CC
#undef EMBER_EM_STR
#undef EMBER_EM_STR2

// Header size in bytes (7 u32 fields + 3 u32 reserved). Kept as a constant so
// the writer and loader agree on the fixed prefix length.
constexpr uint32_t EM_HEADER_SIZE = 40u;

// Conservative v1 parser limits. These are part of the load contract: every
// disk-controlled count/size is checked before reserve/resize/allocation.
constexpr uint64_t MAX_FILE_SIZE       = 256ull * 1024ull * 1024ull;
constexpr uint32_t MAX_FUNCTIONS       = 16u * 1024u;
constexpr uint32_t MAX_GLOBALS         = 64u * 1024u * 1024u; // bytes in globals block
constexpr uint32_t MAX_CODE_PER_FN     = 16u * 1024u * 1024u;
constexpr uint32_t MAX_RODATA_PER_FN   = 16u * 1024u * 1024u;
constexpr uint32_t MAX_RELOCS_PER_FN   = 256u * 1024u;
constexpr uint32_t MAX_SLOTS           = 64u * 1024u;
constexpr uint32_t MAX_NAMES           = 64u * 1024u;
constexpr uint32_t MAX_NAME_SIZE       = 4u * 1024u;

// ---- on-disk reloc (POD, mirrors x64_emitter.hpp::AbsFixup) ----
//
// `offset` is the byte offset within the function's `code` of the 8-byte
// imm64 placeholder to patch. `kind` selects what address the loader writes
// there. Values must stay byte-identical to AbsFixup::Kind so a
// static_cast<uint8_t>(AbsFixup.kind) round-trips losslessly through the file.
struct EmReloc {
    uint32_t offset = 0;
    uint8_t  kind   = 0;
    uint32_t addend = 0; // v2; only FunctionRodataBase accepts a nonzero value

    // Kind values - explicit enum so the on-disk numbering is pinned in one
    // place even though the struct stays POD for serialization.
    enum Kind : uint8_t {
        DispatchTableBase  = 0, // patch imm64 -> &module DispatchTable
        GlobalsBase        = 1, // patch imm64 -> &module globals block
        ModuleRegistryBase = 2, // cross-module registry base
        FunctionRodataBase = 3, // patch imm64 -> loaded code end + addend (v2)
    };
};

// ---- in-memory records (serializer fills, loader reads) ----

// One serialized function. `code` is the post-resolve_fixups x64 bytes
// (position-independent except for the reloc'd imm64s); `rodata` is the
// function's trailing rodata blob (string/const literals, addressed through
// explicit FunctionRodataBase relocations); `relocs` lists every baked
// absolute-imm64 the loader must repoint. `rodata_size == 0` remains valid.
struct EmSignature {
    Type ret;
    std::vector<Type> params;
};

// A native immediate is kept separate from base relocations so the writer can
// zero the pointer bytes and serialize only an allowlisted symbolic identity.
struct EmNativeBinding {
    uint32_t offset = 0;
    std::string name;
    EmSignature signature;
};

struct EmFunctionRecord {
    std::string           name;
    uint32_t              slot_index = 0;
    EmSignature           signature;
    std::vector<uint8_t>  code;
    std::vector<uint8_t>  rodata;
    std::vector<EmReloc>  relocs;
    std::vector<EmNativeBinding> native_bindings;
    // Set by codegen for features whose process state has no sound v2 binding
    // (currently trap stub/context/detail and function-reference allowlists).
    std::string non_serializable_reason;
    // v5: when non-empty, this function is an IR function — `ir_blob` is the
    // output of `serialize_thin_function` (src/thin_ir_ser.hpp) and the v4
    // `code`/`rodata`/`relocs`/`native_bindings` fields are UNUSED (empty).
    // When empty, the function is a raw-x86 fallback (serialized exactly as
    // v4). A v5 module may MIX the two per-function (mixed mode): IR-
    // serializable fns ship IR; non-serializable fns (aggregate/string/struct/
    // defer gaps, flagged by ThinFunction::non_serializable) ship raw x86. On
    // disk the v5 `is_ir` byte is derived as `!ir_blob.empty()`. ADDITIVE:
    // default-constructed empty, untouched by the v1-v4 writer/loader paths
    // (they never read or write it; a v4 record has no is_ir byte).
    std::vector<uint8_t>  ir_blob;
};

// A whole `.em` module in memory. `functions` is in no required order; the
// loader indexes by `slot_index`. `globals` is the raw initialized byte block
// copied verbatim into the allocated globals block; producers must pass the
// post-initializer bytes rather than manufacturing a fresh zero-filled block.
// `entry_slot` is
// `EM_NO_ENTRY` if the module has no @entry function. `name_table` is the
// name->slot directory (for `ember_call` by name, docs/HOT_RELOAD.md Section 7).
// From v3 (F1 visibility, docs/spec/SPEC_AUDIT_2026-07-10.md F1) `name_table` is
// also the module's EXPORT TABLE: the writer/host populates it from only the
// `is_exported` (`pub fn` / bare `fn`) functions, so a `priv fn` is absent and
// is not callable cross-module. v1/v2 directories listed every function.
struct EmModule {
    std::vector<EmFunctionRecord>                functions;
    std::vector<uint8_t>                         globals;
    uint32_t                                     entry_slot = EM_NO_ENTRY;
    std::vector<std::pair<std::string, uint32_t>> name_table;
};

} // namespace ember
