// ember module linker (MODULES.md §5) - host-side glue for live modules.
//
// The runtime half (ModuleRegistry) and the kind-2 cross-module call sequence
// are in module_registry.hpp / codegen. This header is the LINKER STAGE: it
// builds the ModuleExportTable that sema resolves `mod::fn` against, and
// registers modules (JIT or .em) into the per-process registry. Provenance-
// agnostic (MODULES.md §2.6): a JIT module (parsed Program + DispatchTable)
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

// Build the ModuleExportTable entry for a JIT-compiled module: one export per
// function in `prog.funcs`, carrying its slot, the module_id (assigned on
// registration), and its signature (ret + param types) for sema's cross-module
// arg/return type-checking. Call AFTER slot assignment + sema (so FuncDecl::ret
// and param types are resolved) and AFTER registration (so module_id is known).
inline std::vector<ModuleExport> build_jit_exports(const Program& prog, uint32_t module_id) {
    std::vector<ModuleExport> out;
    out.reserve(prog.funcs.size());
    for (const auto& fn : prog.funcs) {
        ModuleExport exp;
        exp.fn_name = fn.name;
        exp.module_id = module_id;
        exp.slot = fn.slot;
        exp.ret = fn.ret ? *fn.ret : Type{};
        for (const auto& p : fn.params) exp.params.push_back(p.ty ? *p.ty : Type{});
        out.push_back(std::move(exp));
    }
    return out;
}

// Build the ModuleExportTable entry for a v1 .em-loaded module: one export per
// name_table entry. v1 stores name+slot only, so these are explicitly
// ABI-TRUSTED UNKNOWN signatures: sema cannot verify arity, argument types, or
// return type, and the host/caller must supply the exact original ABI.
// TODO(format v2): version the format and serialize canonical export/import
// signatures before enabling link-time verification. Do not reinterpret v1.
inline std::vector<ModuleExport> build_em_exports(const LoadedModule& mod, uint32_t module_id) {
    std::vector<ModuleExport> out;
    out.reserve(mod.name_table.size());
    for (const auto& [name, slot] : mod.name_table) {
        ModuleExport exp;
        exp.fn_name = name;
        exp.module_id = module_id;
        exp.slot = int(slot);
        exp.unknown_sig = true;  // v1 ABI-trusted unknown signature
        // ret/params left default; sema deliberately skips all signature
        // checking because the v1 file contains no data with which to verify.
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

// Register a .em module: load the file, register it, apply its kind-2 relocs
// against the registry (load_em_file does this when given the registry), and
// return the LoadedModule (caller owns it — keep it alive for the registry's
// lifetime, since the registry holds a pointer to its dispatch table).
// On failure returns false + sets *err (out is partially filled; discard it).
inline bool link_em_file(ModuleRegistry& reg, const char* path, const std::string& name,
                        LoadedModule& out, std::string* err = nullptr) {
    if (!load_em_file(path, out, err, &reg)) return false;
    uint32_t id = reg.register_module(name, out.dispatch.data(), err);
    if (id == UINT32_MAX) return false;
    return true;
}

// Convenience: add a module's exports to a ModuleExportTable under its alias.
// (The alias is what scripts write in `alias::fn()`; the registry name is the
// canonical id. They're usually the same but `link "x" as y;` decouples them.)
inline void add_exports(ModuleExportTable& table, const std::string& alias,
                        const std::vector<ModuleExport>& exports) {
    table[alias] = exports;
}

} // namespace ember
