// ember `.em` loader API (docs/BUNDLING_AND_EM_MODULES.md Section 2.5)
//
// The load-side counterpart to em_writer.{hpp,cpp}. Reads the on-disk
// binary defined in em_file.hpp / Section 2.2 and produces a `LoadedModule` the
// runtime (ember_call, hot reload) treats identically to a JIT-compiled
// module: per-function exec pages with relocations patched to the live
// dispatch-table + globals-block addresses, plus a name→slot directory.
//
// No regalloc, no emit, no parser, no sema. Load is header-read + memcpy +
// two pointer writes per function (Section 2.5 step 6's payoff). The relocations
// are the only post-memcpy work: each AbsFixup baked by the emitter's
// `mov_reg_imm64_external` (Section 2.4) is repointed at the load-time address of
// the module's dispatch table (kind 0) or globals block (kind 1).
//
// Field/byte order on disk is byte-identical to em_writer.cpp; this loader
// mirrors the writer's emit order (header → per-fn records → globals block
// → name directory) and its LE-by-shifts style. See em_loader.cpp for the
// matching read helpers.
//
// Cross-module relocs (kind 2, docs/MODULES.md Section 3) require a ModuleRegistry to be
// supplied to the loader: a module that contains a `mov r11, [registry_base +
// ...]` call site was serialized against some registry's base, and on load into
// a fresh process that base is at a different address. The loader patches the
// kind-2 imm64 with the supplied registry's `base()`. If a kind-2 reloc is
// encountered but no registry was supplied, the load fails with an error - a
// cross-module call site with no registry to bind to would execute a wild
// jump on the first call, which is worse than a loud load-time reject.
//
// COMPATIBILITY: historical v1 remains ABI/process-trusted and exposes unknown
// signatures. v2 is cross-process bindable: it checks deterministic compiler/
// target identities before allocation, carries canonical export signatures,
// function-local rodata relocations, and resolves symbolic natives exclusively
// through the host allowlist. This is still native code, not a sandbox or an
// authenticated untrusted-code container.
//
// prism port note (docs/planning/RESTRUCTURE_PLAN.md Section 5): the standalone loader used an
// RAII `ExecArena` per function (owning a VirtualAlloc page); prism's
// `jit_memory` is simpler - `alloc_executable`/`free_executable` return/own
// raw `void*` pages with no RAII wrapper. `LoadedModule` therefore holds
// `std::vector<void*> pages` (the exec allocations) and frees them via
// `free_executable` in its destructor. The two-pass + pre-sizing invariant
// is preserved verbatim (dispatch/globals reserved before reloc patching so
// their `.data()` is stable).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "sema.hpp"

#include "em_file.hpp"          // EM_MAGIC, EM_VERSION, EM_NO_ENTRY, EM_HEADER_SIZE, EmReloc
#include "jit_memory.hpp"       // alloc_executable / free_executable
#include "module_registry.hpp"  // ModuleRegistry (kind-2 reloc target, docs/MODULES.md Section 3)

namespace ember {

// A loaded `.em` module: owns the dispatch table, the globals block, and the
// per-function exec pages. The runtime (ember_call, hot reload) treats this
// identically to a JIT-compiled module.
//
// Ownership:
//   - `dispatch` and `globals` own the backing storage whose addresses are
//     baked as imm64s into the exec pages during load (via the AbsFixup
//     relocations). They MUST NOT realloc after the per-function pages are
//     published + patched: a vector move would change `.data()` and
//     invalidate every baked absolute address. `load_em_file` therefore
//     reserves both vectors to their final size before patching and fills
//     `dispatch` by index (never push_back). See em_loader.cpp's
//     PRE-SIZING INVARIANT comment.
//   - `pages` owns one `void*` exec allocation per function (returned by
//     `alloc_executable`); destroying `LoadedModule` frees every page via
//     `free_executable`. (prism's `jit_memory` has no RAII wrapper, so the
//     destructor does the frees explicitly.)
struct LoadedModule {
    std::vector<void*>                             dispatch;     // dispatch[i] = entry addr of slot i
    std::vector<uint8_t>                            globals;      // the globals block (initial values copied in)
    std::vector<void*>                             pages;        // owns the exec pages (one per function)
    std::vector<std::pair<std::string, uint32_t>>   name_table;  // name -> slot (for ember_call by name)
    uint32_t                                       entry_slot = EM_NO_ENTRY;
    uint32_t                                       format_version = EM_VERSION_V1;
    // v2 canonical signatures indexed by dispatch slot. Name-directory aliases
    // resolve to a slot first, so duplicate/aliased names cannot lose metadata.
    // Empty for v1 (whose signatures are intentionally ABI-trusted unknown).
    std::vector<EmSignature>                       signatures_by_slot;

    LoadedModule() = default;
    ~LoadedModule();
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    LoadedModule(LoadedModule&& other) noexcept;
    LoadedModule& operator=(LoadedModule&& other) noexcept;

    // Look up a function's entry by name (returns nullptr if not found).
    // O(n) linear scan of name_table; n is small (one entry per exported
    // function). Matches the writer's name_table convention (Section 2.2).
    void* entry_by_name(const char* name) const;

    // The @entry function's entry (nullptr if no entry, i.e. entry_slot ==
    // EM_NO_ENTRY or the slot is out of range).
    void* entry() const;
};

// Load a `.em` file from `path`. Returns true and fills `out` on success;
// returns false and sets *err on any failure (bad magic, bad version, I/O,
// truncated read, unknown reloc kind, a kind-2 reloc with no `registry`). On
// failure `out` is unchanged: parsing/validation and executable pages are
// staged privately, and no dispatch entry/page is published until every
// function and relocation has validated, patched, and sealed successfully.
//
// `registry` is optional (additive, default nullptr): when non-null, kind-2
// (ModuleRegistryBase) relocs are patched with `registry->base()` (docs/MODULES.md
// Section 3 - the cross-module call site's registry hop). When null, encountering a
// kind-2 reloc is a load error (the module has a cross-module call site but
// no registry was supplied to bind it). Kinds 0/1 (DispatchTableBase /
// GlobalsBase) are always patched with the loaded module's own dispatch table
// / globals - they do not need a registry.
//
// No-throw boundary: malformed files, I/O failures, allocation failures, and
// standard-library exceptions all surface as `false` plus a categorized
// `*err` message. Disk-controlled counts/sizes are bounded before allocation.
bool load_em_file(const char* path, LoadedModule& out, std::string* err,
                  ModuleRegistry* registry = nullptr,
                  const std::unordered_map<std::string, NativeSig>* native_bindings = nullptr);

} // namespace ember
