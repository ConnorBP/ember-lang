// ember module linker (docs/MODULES.md §5) - host-side glue for live modules.
//
// The runtime half (ModuleRegistry) and the kind-2 cross-module call sequence
// are in module_registry.hpp / codegen. This header is the LINKER STAGE: it
// builds the ModuleExportTable that sema resolves `mod::fn` against, and
// registers modules (JIT or .em) into the per-process registry. Provenance-
// agnostic (docs/MODULES.md §2.6): a JIT module (parsed Program + DispatchTable)
// and a .em module (LoadedModule) both register the same way and both expose
// the same ModuleExport shape — the registry and the call site neither know
// nor care which kind a module is.
//
// Bidirectionality (the user's ask): once a module is registered here, ANY
// other module (JIT or .em) can call into it via `mod::fn()`, and it can call
// out to any other registered module. The kind-2 sequence resolves the target
// at call time via the registry, so a .em calling a JIT module and a JIT
// calling a .em module are the same code path.
#pragma once
#include "sema.hpp"            // ModuleExport, ModuleExportTable, Type
#include "module_registry.hpp" // ModuleRegistry
#include "em_loader.hpp"       // LoadedModule
#include "ast.hpp"             // Program, FuncDecl
#include "dispatch_table.hpp"  // DispatchTable
#include <string>
#include <vector>

namespace ember {

// Red 7 overload: build the export table with the target's DispatchMode so
// sema can stamp it on CallExpr/FnHandleExpr for the keyed/legacy cross-module
// emit path selection + the legacy→keyed rejection. Defined BEFORE the
// 1-arg backward-compat wrapper so the wrapper can delegate to it.
inline std::vector<ModuleExport> build_jit_exports(const Program& prog, uint32_t module_id,
                                                    DispatchMode mode) {
    std::vector<ModuleExport> out;
    out.reserve(prog.funcs.size());
    for (const auto& fn : prog.funcs) {
        if (!fn.is_exported) continue;  // F1: `priv fn` is not part of the export surface
        ModuleExport exp;
        exp.fn_name = fn.name;
        exp.module_id = module_id;
        exp.slot = fn.slot;
        exp.ret = fn.ret ? *fn.ret : Type{};
        for (const auto& p : fn.params) exp.params.push_back(p.ty ? *p.ty : Type{});
        exp.dispatch_mode = mode;
        out.push_back(std::move(exp));
    }
    return out;
}

// Backward-compat wrapper: build the export table with Identity mode (the
// pre-Red-7 default). Existing callers that do not pass a DispatchMode get
// Identity exports, preserving the legacy behavior.
inline std::vector<ModuleExport> build_jit_exports(const Program& prog, uint32_t module_id) {
    return build_jit_exports(prog, module_id, DispatchMode::Identity);
}

// Build one export per name-directory entry. v2 looks up its canonical
// signature by slot, so aliases remain typed and duplicate names cannot shadow
// metadata. v1 stores no signatures and remains ABI-TRUSTED UNKNOWN.
//
// F1 visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): from v3 onward the
// `.em` name directory IS the module's EXPORT TABLE - the writer/host populates
// `LoadedModule::name_table` from only the `is_exported` (`pub fn`) functions,
// so a `priv fn` is absent from the directory and therefore absent from the
// exports built here. v1/v2 name directories historically listed every
// function (no visibility), and those files keep loading with every function
// exported - backward compat. `build_em_exports` does not itself filter; it
// trusts the name_table it is handed (the filtering happens at EmModule
// construction, see ember_cli's --emit-em path / the test helpers).
inline std::vector<ModuleExport> build_em_exports(const LoadedModule& mod, uint32_t module_id) {
    std::vector<ModuleExport> out;
    out.reserve(mod.name_table.size());
    for (const auto& [name, slot] : mod.name_table) {
        ModuleExport exp;
        exp.fn_name = name;
        exp.module_id = module_id;
        exp.slot = int(slot);
        if (mod.format_version >= EM_VERSION_V2 && slot < mod.signatures_by_slot.size()) {
            exp.ret = mod.signatures_by_slot[slot].ret;
            exp.params = mod.signatures_by_slot[slot].params;
            exp.unknown_sig = false;
        } else {
            exp.unknown_sig = true;  // v1 ABI-trusted unknown signature
        }
        out.push_back(std::move(exp));
    }
    return out;
}

// Register a JIT module into the registry + return its module_id + exports.
// `dispatch_table_base` is the DispatchTable's slots-array address (callers
// bake it as the kind-0 base; the registry stores it for cross-module callers).
inline uint32_t register_jit_module(ModuleRegistry& reg, const std::string& name,
                                    void* dispatch_table_base, std::string* err = nullptr) {
    return reg.register_module(name, dispatch_table_base, err);
}

// v1.0 Tier 2 cross-module handles: register a JIT module AND publish its fn
// allowlist + slot_count so other modules can take `&name::fn` handles into it
// (the cross-module guard validates those handles against this module's
// allowlist before dispatch). `allowlist` is the host-owned byte array from
// build_fn_allowlist (the caller must keep it alive for the registry's
// lifetime, same as the dispatch table). A module registered via the plain
// register_jit_module above does NOT publish an allowlist, so cross-module
// handles into it trap (it did not opt into being a handle target).
inline uint32_t register_jit_module_with_handles(ModuleRegistry& reg, const std::string& name,
                                                  void* dispatch_table_base,
                                                  void* allowlist_base,
                                                  int64_t slot_count,
                                                  std::string* err = nullptr) {
    return reg.register_module(name, dispatch_table_base, err, allowlist_base, slot_count);
}

// Register a .em module: load the file, register it, apply its kind-2 relocs
// against the registry (load_em_file does this when given the registry), and
// return the LoadedModule (caller owns it — keep it alive for the registry's
// lifetime, since the registry holds a pointer to its dispatch table).
// On failure returns false + sets *err (out is partially filled; discard it).
inline bool link_em_file(ModuleRegistry& reg, const char* path, const std::string& name,
                        LoadedModule& out, std::string* err = nullptr,
                        const std::unordered_map<std::string, NativeSig>* natives = nullptr,
                        const EmVerifyPolicy* verify = nullptr,
                        const EmLoadPolicy* load_policy = nullptr) {
    if (!load_em_file(path, out, err, &reg, natives, verify, load_policy)) return false;
    uint32_t id = reg.register_module(name, out.dispatch.data(), err);
    if (id == UINT32_MAX) return false;
    // X1 redesign: publish the loaded module's dispatch-table slot count so a
    // v5 IR CallCrossModule into this module range-checks meta.slot against
    // its REAL dispatch size at load time (the loader threads
    // dispatch_slot_count into validate_thin_function). Without this a
    // cross-module caller could index out of this module's dispatch table.
    //
    // Red 7 (§9.7): logical_slot_count falls back to this single count for an
    // identity module (no published keyed record), so the validator's
    // logical-count range check is identical to the pre-Red-7 X1 check for
    // identity .em modules (no weakening of V5).
    reg.set_dispatch_slot_count(id, int64_t(out.dispatch.size()));
    return true;
}

// ─── Red 7: keyed/legacy cross-module compatibility (§9.7) ────────────────
//
// The cross-module call path's (caller_mode, target_mode) matrix:
//   - keyed caller → keyed target:   the keyed caller resolves the target's
//                                     current record at call time via r15.
//   - keyed caller → identity target: the keyed caller uses the legacy
//                                     registry-hop (r15 unused for identity).
//   - legacy caller → identity target: the existing path, unchanged.
//   - legacy caller → keyed target:   REJECTED at codegen (the legacy caller
//                                     has no r15 route word; emit_cross_module
//                                     call emits a trap).
//
// A module's DispatchMode is published via publish_dispatch_record (a keyed
// module) or defaults to Identity (a legacy module with no published record).
// The linker inspects the target's mode (via ModuleExport::dispatch_mode, set
// when building exports) before allowing a legacy caller to bind to it; the
// codegen emit_cross_module_call enforces the reject at the call site.

// Register a JIT module AND publish its keyed ModuleDispatchRecord. The
// module's dispatch record (built from its layout plan + compiled entries) is
// published through one release/acquire atomic pointer update (§10.2), so
// concurrent readers observe a coherent generation. The module_id is stable
// (a same-name reload keeps the id). `register_with_handles` publishes the
// fn allowlist for cross-module handles. Returns the module_id or UINT32_MAX
// on failure.
inline uint32_t register_keyed_jit_module(ModuleRegistry& reg, const std::string& name,
                                          void* dispatch_table_base,
                                          const ModuleDispatchRecord* rec,
                                          void* allowlist_base = nullptr,
                                          int64_t slot_count = 0,
                                          std::string* err = nullptr) {
    uint32_t id = reg.register_module(name, dispatch_table_base, err,
                                      allowlist_base, slot_count);
    if (id == UINT32_MAX) return id;
    // Publish the logical/physical counts from the record (the record carries
    // them; the registry reads them via acquire-load for its typed accessors).
    reg.set_dispatch_slot_count(id, int64_t(rec->logical_slot_count));
    reg.publish_dispatch_record(id, rec);
    return id;
}

// Convenience: add a module's exports to a ModuleExportTable under its alias.
// (The alias is what scripts write in `alias::fn()`; the registry name is the
// canonical id. They're usually the same but `link "x" as y;` decouples them.)
inline void add_exports(ModuleExportTable& table, const std::string& alias,
                        const std::vector<ModuleExport>& exports) {
    table[alias] = exports;
}

inline uint32_t link_em_file_v6(ModuleRegistry& reg, const std::string& name,
                                LoadedModule& out, std::string* err = nullptr) {
    uint32_t id = reg.register_module(name, out.dispatch.data(), err);
    if (id == UINT32_MAX) return id;
    if (out.is_v6 && out.v6_keyed && out.v6_record) {
        reg.set_dispatch_slot_count(id, int64_t(out.v6_logical_slot_count));
        reg.publish_dispatch_record(id, out.v6_record.get());
    } else {
        reg.set_dispatch_slot_count(id, int64_t(out.dispatch.size()));
    }
    return id;
}

inline std::vector<ModuleExport> build_em_v6_exports(const LoadedModule& mod, uint32_t module_id) {
    std::vector<ModuleExport> out;
    out.reserve(mod.name_table.size());
    DispatchMode mode = (mod.is_v6 && mod.v6_keyed) ? DispatchMode::Keyed : DispatchMode::Identity;
    for (const auto& [name, slot] : mod.name_table) {
        ModuleExport exp;
        exp.fn_name = name; exp.module_id = module_id; exp.slot = int(slot);
        if (mod.format_version >= EM_VERSION_V2 && slot < mod.signatures_by_slot.size()) {
            exp.ret = mod.signatures_by_slot[slot].ret;
            exp.params = mod.signatures_by_slot[slot].params;
            exp.unknown_sig = false;
        } else { exp.unknown_sig = true; }
        exp.dispatch_mode = mode;
        out.push_back(std::move(exp));
    }
    return out;
}

} // namespace ember
