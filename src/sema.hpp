// ember sema - name resolution + type check (docs/spec/COMPILER_PIPELINE.md Section 4).
// Scoped for the first running version: primitives, calls, operators
// with literal-type-adaptation (docs/spec/TYPE_SYSTEM.md Section 6 - literals adopt the
// other operand's type if the value fits; variables stay strict per Section 7).
#pragma once
#include "ast.hpp"
#include "module_layout.hpp"  // Red 7: DispatchMode for ModuleExport (keyed/legacy target classification)
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace ember {

struct SemaError { std::string msg; uint32_t line; uint32_t col; };

// v0.5 cross-module export entry (docs/MODULES.md §5). A linked module exposes its
// functions by name + signature + slot. The host builds this table from the
// ModuleRegistry's registered modules; sema resolves `mod::fn` against it.
// `module_id` is the registry id (baked into the call site by codegen); `slot`
// is the fn's slot in that module's dispatch table. If a module/fn isn't in the
// table, sema marks the call `cross_module_unresolved` (deferred trap — the
// module may register later, docs/MODULES.md §5 step 1/3).
struct ModuleExport {
    std::string fn_name;
    Type ret;
    std::vector<Type> params;
    uint32_t module_id = 0;
    int slot = -1;
    bool unknown_sig = false;  // v1 .em ABI-trusted export: format has no sigs; sema skips arg/return checks
    // Red 7 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.7): the target
    // module's dispatch mode. The linker sets this when building exports from
    // a registered module's published record (Keyed) or its legacy single
    // count (Identity). Sema stamps it on CallExpr/FnHandleExpr so codegen
    // can emit the correct cross-module call path (keyed resolve vs legacy
    // registry-hop) and reject a legacy caller linking to a keyed target.
    // Default Identity (backward compat: pre-Red-7 modules are identity).
    DispatchMode dispatch_mode = DispatchMode::Identity;
};
// module_alias -> list of exported fns. Host populates from the registry.
using ModuleExportTable = std::unordered_map<std::string, std::vector<ModuleExport>>;

// A registered native function signature (host-side; mirrors NativeFn
// from docs/spec/BINDING_API.md but sema only needs the type info + fn ptr).
//
// `retains` (slice-escape safety Stage 2, C3): does this native RETAIN a slice
// pointer it receives past the call (store it in a host-side data structure
// that outlives the call), or does it COPY/consume the bytes and drop the ptr?
// Default false = copying/consuming (the only shipped slice-taking native,
// string_from_slice, copies the bytes into a host-owned std::string and
// returns a handle — it does NOT retain the ptr). Sema rejects a stack-backed
// slice (a ViewExpr over a fixed array, or an encrypted StringLit temp) passed
// to a `retains=true` native, because the backing frame dies before the
// retained use and the ptr would dangle. A copying native (retains=false) is
// allowed — the bytes are copied out during the call, before the frame dies.
// No shipped native retains a slice ptr today, so C3 is "accidentally safe";
// this field is the annotation surface a future retaining native sets to true
// to get the guard. See demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md §5.4/§6.3.
struct NativeSig {
    std::string name;
    void* fn_ptr = nullptr;
    Type ret;
    std::vector<Type> params;
    uint32_t permission = 0;
    bool retains = false;  // C3: may this native retain a slice ptr past the call?
};

// Operator-overload registry: (type_name, operator) -> native fn ptr + sig.
// Host registers overloads for a type (e.g. "vec3" + Add -> vec3_add).
// Sema rewrites `v1 + v2` (where v1/v2 are typed as the registered type)
// into a native call to the overload fn. (docs/spec/BINDING_API.md Section 3 / docs/spec/TYPE_SYSTEM.md Section 7)
struct OpOverload {
    void* fn_ptr = nullptr;
    std::string fn_name;
    Type ret;
    std::vector<Type> params;
};
struct OpOverloadTable {
    // key: (type_name, BinExpr::Op as int)
    std::unordered_map<std::string, OpOverload> entries;
    std::string key(const std::string& type_name, int op) const {
        return type_name + "\x01" + std::to_string(op);
    }
    void register_op(const std::string& type_name, int op, OpOverload o) {
        entries[key(type_name, op)] = std::move(o);
    }
    const OpOverload* find(const std::string& type_name, int op) const {
        auto it = entries.find(key(type_name, op));
        return it != entries.end() ? &it->second : nullptr;
    }
};

// Ember struct value types: fields are tightly packed in declaration order
// with no alignment or trailing padding. Nested structs/fixed arrays use their
// recursively computed Ember sizes. These are not host-C aggregate layouts.
struct StructFieldLayout { const Type* ty; int32_t offset; };
struct StructLayout {
    int32_t size = 0;
    // The struct's alignment requirement in bytes. Script-declared structs
    // use packed declaration-order layout with alignment 1 (no padding);
    // host-registered structs (register_struct in binding_builder.hpp) carry
    // their computed C++ natural alignment. The ABI classifier
    // (dispatch_abi.hpp) encodes this so two aggregates with identical size
    // but different alignment (packed vs C++-aligned) produce distinct
    // fingerprints. Defaults to 1 so existing script-struct behavior is
    // unchanged; no existing code reads this field.
    uint32_t alignment = 1;
    std::vector<std::string> field_names;   // declaration order
    std::unordered_map<std::string, StructFieldLayout> fields;
    // Host-registered structs own their field Types via shared_ptrs so the
    // const Type* pointers in `fields` remain valid for the table's lifetime.
    // Script-declared structs don't need this (their Types live in Program).
    std::vector<std::shared_ptr<Type>> owned_field_types;
};
using StructLayoutTable = std::unordered_map<std::string, StructLayout>;

// Compute every struct's field layout from Program::structs. Called by the
// host right after parsing - the result feeds both sema (FieldExpr/StructLit
// type-checking) and codegen (frame sizing, field addressing), so it must
// exist before either runs.
StructLayoutTable build_struct_layouts(const Program& prog);

// Const-expression folders (used by sema's bounds/assert folding; exported for
// global-initializer evaluation at load). Each returns true + sets `out` if `e`
// folds to a literal constant of the matching type; false if it's a genuine
// runtime value (Ident/CallExpr/etc). Mirrors codegen's fold exactly so a
// folded value matches what the runtime path would compute (sema.cpp comment).
bool try_eval_const_i64(const Expr& e, int64_t& out);
bool try_eval_const_f32(const Expr& e, float& out);
bool try_eval_const_f64(const Expr& e, double& out);
bool try_eval_const_bool(const Expr& e, bool& out);

struct SemaResult {
    bool ok = true;
    std::vector<SemaError> errors;
    std::vector<SemaError> warnings;  // non-fatal: deprecations, etc. (the CLI prints these)
    // resolved per-function: native calls and script-call slots are stamped
    // onto the AST CallExpr nodes (is_native/native_fn/script_slot) by sema.
};

// Run sema over a program. `natives` is the host-registered native table
// (Prism populates it with ru64/rf32/draw_* etc. before compiling).
// `script_fns` is filled with name->slot mapping for script-to-script calls.
// `overloads` is the operator-overload table (vec3_add, mat4_mul, etc.).
// `structs` is the struct-layout table (build_struct_layouts above).
SemaResult sema(Program& prog,
                const std::unordered_map<std::string, NativeSig>& natives,
                const std::unordered_map<std::string, int>& script_slots,
                uint32_t module_permissions,
                const OpOverloadTable* overloads = nullptr,
                const StructLayoutTable* structs = nullptr,
                const ModuleExportTable* module_exports = nullptr);

} // namespace ember
