// ember `.em` binary format: shared constants + on-disk structs
// (BUNDLING_AND_EM_MODULES.md Section 2.2)
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
//     version         : u32 = 1
//     flags           : u32 = 0           (bit 0 reserved: embeds source)
//     function_count  : u32
//     global_size     : u32
//     rodata_total    : u32               (sum of per-fn rodata_size)
//     entry_slot      : u32               (slot of @entry fn, or EM_NO_ENTRY)
//     reserved        : u32[3] = 0
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
//       reloc { offset: u32, kind: u8 }
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
// `kind` space is versioned with the format (BUNDLING_AND_EM_MODULES.md Section 2.7);
// a new kind requires a version bump.
//
// TRUST/PORTABILITY NOTE: v1 code can embed process-local native-function,
// trap-stub, function-reference allowlist, and string-storage pointers which
// are not represented by these relocation kinds. A v1 `.em` is therefore
// ABI/process-trusted native code, not a portable or untrusted-code container.
// TODO(v2): symbolic signatures/import binding and authentication/signatures
// require a versioned format; do not reinterpret or silently extend v1.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ember {

// ---- on-disk identity ----

// "EMBL" - ember bundle, loadable. Read as a little-endian u32 this is
// 'E','M','B','L' = 0x4C,0x42,0x4D,0x45 -> 0x454D424C.
constexpr uint32_t EM_MAGIC   = 0x454D424Cu;
constexpr uint32_t EM_VERSION  = 1u;
constexpr uint32_t EM_NO_ENTRY = 0xFFFFFFFFu; // entry_slot when no @entry fn

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

    // Kind values - explicit enum so the on-disk numbering is pinned in one
    // place even though the struct stays POD for serialization.
    enum Kind : uint8_t {
        DispatchTableBase  = 0, // patch imm64 -> &module DispatchTable
        GlobalsBase        = 1, // patch imm64 -> &module globals block
        ModuleRegistryBase = 2, // reserved; cross-module import (unused in v1)
    };
};

// ---- in-memory records (serializer fills, loader reads) ----

// One serialized function. `code` is the post-resolve_fixups x64 bytes
// (position-independent except for the reloc'd imm64s); `rodata` is the
// function's trailing rodata blob (string/const literals, RIP-relative from
// `code`, same page - MEMORY_AND_GC.md Section 6); `relocs` lists every baked
// absolute-imm64 the loader must repoint. `rodata` is empty for emitters
// (like prism's) that bake string-literal pointers as raw imm64s rather than
// as in-page RIP-relative rodata - the format handles `rodata_size == 0`.
struct EmFunctionRecord {
    std::string           name;
    uint32_t              slot_index = 0;
    std::vector<uint8_t>  code;
    std::vector<uint8_t>  rodata;
    std::vector<EmReloc>  relocs;
};

// A whole `.em` module in memory. `functions` is in no required order; the
// loader indexes by `slot_index`. `globals` is the raw initialized byte block
// copied verbatim into the allocated globals block; producers must pass the
// post-initializer bytes rather than manufacturing a fresh zero-filled block.
// `entry_slot` is
// `EM_NO_ENTRY` if the module has no @entry function. `name_table` is the
// name->slot directory (for `ember_call` by name, HOT_RELOAD.md Section 7).
struct EmModule {
    std::vector<EmFunctionRecord>                functions;
    std::vector<uint8_t>                         globals;
    uint32_t                                     entry_slot = EM_NO_ENTRY;
    std::vector<std::pair<std::string, uint32_t>> name_table;
};

} // namespace ember
