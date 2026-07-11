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
// through the host allowlist. v3 (F1 visibility, docs/spec/SPEC_AUDIT_2026-07-10.md
// F1) keeps the v2 per-function record layout byte-identical and repurposes the
// name directory as the module's EXPORT TABLE (only `pub fn`/bare-`fn` entries;
// `priv fn` helpers are serialized but absent from the directory, so they are
// not callable cross-module and `build_em_exports` does not export them). The
// loader accepts v1, v2, and v3. v4 (F2, docs/spec/SPEC_AUDIT_2026-07-10.md F2)
// keeps the v3 layout byte-identical and appends an additive Ed25519 signature
// block after the name directory; the loader verifies that signature over the
// v3 content (header -> end of name directory) BEFORE alloc_executable_rw and
// rejects the module on mismatch — a maliciously-modified `.em` is rejected
// rather than executed. The loader accepts v1, v2, v3, and v4. The build_id /
// abi_hash stay the COMPATIBILITY check; the v4 signature is the CONTENT
// authentication the v2/v3 identity hash is NOT. See `EmVerifyPolicy` for the
// dev-mode vs signed-only key-management model.
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

#include <array>
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

// F2 (docs/spec/SPEC_AUDIT_2026-07-10.md F2): the .em CONTENT-AUTHENTICATION
// policy a host passes to load_em_file. This is the secure-boot-style opt-in:
//
//   - trusted_keys EMPTY  -> DEV MODE. The loader accepts unsigned v1/v2/v3
//     modules (the development convenience the audit names). A v4 (signed)
//     module is REJECTED with a clear error ("v4 module requires a
//     verification key; host provided none") — a v4 module IS signed, so
//     running it without verifying would be worse than running honest
//     unsigned dev code. Existing tests/demos use the unsigned path here.
//
//   - trusted_keys NON-EMPTY -> SIGNED-ONLY MODE. v1/v2/v3 (unsigned) modules
//     are REJECTED ("host mandates signed modules; this is an unsigned vN
//     module"); a v4 module loads ONLY if its signature verifies against one
//     of the trusted keys. The signing key stays OFF the host; the host gets
//     only the verification public keys.
//
// `null` policy (the default arg) == DEV MODE (empty keyring) so every
// existing caller (em_roundtrip_test, import_roundtrip_test, v0_5_live_modules_test,
// pub_priv_test, link_em_file, the CLI without --verify-em-key) keeps working
// unchanged against unsigned v1/v2/v3 modules. The CLI's --verify-em-key flag
// populates a non-empty keyring to opt into signed-only mode.
struct EmVerifyPolicy {
    std::vector<std::array<uint8_t, 32>> trusted_keys;
    bool signed_only() const { return !trusted_keys.empty(); }
};

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
    std::vector<std::pair<std::string, uint32_t>>   name_table;  // name -> slot; v3: the EXPORT TABLE (only pub fns)
    uint32_t                                       entry_slot = EM_NO_ENTRY;
    uint32_t                                       format_version = EM_VERSION_V1;
    // v2+ canonical signatures indexed by dispatch slot. Name-directory aliases
    // resolve to a slot first, so duplicate/aliased names cannot lose metadata.
    // Empty for v1 (whose signatures are intentionally ABI-trusted unknown).
    // v3 carries the same per-slot signatures as v2; the difference is which
    // names appear in `name_table` (the export table), not the signature shape.
    std::vector<EmSignature>                       signatures_by_slot;

    LoadedModule() = default;
    ~LoadedModule();
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    LoadedModule(LoadedModule&& other) noexcept;
    LoadedModule& operator=(LoadedModule&& other) noexcept;

    // Look up a function's entry by name (returns nullptr if not found).
    // O(n) linear scan of name_table; n is small (one entry per EXPORTED
    // function - v3's name_table IS the export table, so a `priv fn` is not
    // reachable here). Matches the writer's name_table convention (Section 2.2).
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
// Load a `.em` file from `path`. Returns true and fills `out` on success;
// returns false and sets *err on any failure (bad magic, bad version, I/O,
// truncated read, unknown reloc kind, a kind-2 reloc with no `registry`, a v4
// signature that fails verification). On failure `out` is unchanged: parsing,
// signature verification, and executable pages are staged privately, and no
// dispatch entry/page is published until every function and relocation has
// validated, the signature (v4) has verified, patched, and sealed successfully.
//
// `registry` is optional (additive, default nullptr): when non-null, kind-2
// (ModuleRegistryBase) relocs are patched with `registry->base()` (docs/MODULES.md
// Section 3 - the cross-module call site's registry hop). When null, encountering a
// kind-2 reloc is a load error (the module has a cross-module call site but
// no registry was supplied to bind it). Kinds 0/1 (DispatchTableBase /
// GlobalsBase) are always patched with the loaded module's own dispatch table
// / globals - they do not need a registry.
//
// `verify` is optional (additive, default nullptr == DEV MODE, empty keyring):
// the F2 .em content-authentication policy. See `EmVerifyPolicy`. In dev mode
// (null OR an empty keyring) the loader accepts unsigned v1/v2/v3 modules and
// rejects a v4 module with a clear error. In signed-only mode (a non-empty
// keyring) the loader rejects unsigned v1/v2/v3 modules and accepts a v4
// module only if its Ed25519 signature verifies against one of the trusted
// keys, BEFORE any executable page is allocated. `null` is the backward-compat
// default so every existing caller keeps working unchanged against unsigned
// v1/v2/v3 modules.
//
// No-throw boundary: malformed files, I/O failures, allocation failures,
// signature-verification failures, and standard-library exceptions all surface
// as `false` plus a categorized `*err` message. Disk-controlled counts/sizes
// are bounded before allocation.
bool load_em_file(const char* path, LoadedModule& out, std::string* err,
                  ModuleRegistry* registry = nullptr,
                  const std::unordered_map<std::string, NativeSig>* native_bindings = nullptr,
                  const EmVerifyPolicy* verify = nullptr);

} // namespace ember
