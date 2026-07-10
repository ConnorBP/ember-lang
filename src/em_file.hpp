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
//     version         : u32 = 2 (loader also accepts historical v1)
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
//   Globals block:    bytes[global_size]
//
//   Name directory:
//     name_table_count : u32
//     entries: [ { name_len: u16, name: bytes, slot_index: u32 } ... ]
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
// binds to a deterministic compiler/build identity and target ABI. Neither
// version authenticates native code or makes it an untrusted-code container.

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
constexpr uint32_t EM_VERSION  = 2u;
constexpr uint32_t EM_VERSION_V1 = 1u;
constexpr uint32_t EM_NO_ENTRY = 0xFFFFFFFFu; // entry_slot when no @entry fn

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
};

// A whole `.em` module in memory. `functions` is in no required order; the
// loader indexes by `slot_index`. `globals` is the raw initialized byte block
// copied verbatim into the allocated globals block; producers must pass the
// post-initializer bytes rather than manufacturing a fresh zero-filled block.
// `entry_slot` is
// `EM_NO_ENTRY` if the module has no @entry function. `name_table` is the
// name->slot directory (for `ember_call` by name, docs/HOT_RELOAD.md Section 7).
struct EmModule {
    std::vector<EmFunctionRecord>                functions;
    std::vector<uint8_t>                         globals;
    uint32_t                                     entry_slot = EM_NO_ENTRY;
    std::vector<std::pair<std::string, uint32_t>> name_table;
};

} // namespace ember
