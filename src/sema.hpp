// ember sema - name resolution + type check (COMPILER_PIPELINE.md Section 4).
// Scoped for the first running version: primitives, calls, operators
// with literal-type-adaptation (TYPE_SYSTEM.md Section 6 - literals adopt the
// other operand's type if the value fits; variables stay strict per Section 7).
#pragma once
#include "ast.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace ember {

struct SemaError { std::string msg; uint32_t line; uint32_t col; };

// v0.5 cross-module export entry (MODULES.md §5). A linked module exposes its
// functions by name + signature + slot. The host builds this table from the
// ModuleRegistry's registered modules; sema resolves `mod::fn` against it.
// `module_id` is the registry id (baked into the call site by codegen); `slot`
// is the fn's slot in that module's dispatch table. If a module/fn isn't in the
// table, sema marks the call `cross_module_unresolved` (deferred trap — the
// module may register later, MODULES.md §5 step 1/3).
struct ModuleExport {
    std::string fn_name;
    Type ret;
    std::vector<Type> params;
    uint32_t module_id = 0;
    int slot = -1;
    bool unknown_sig = false;  // v0.5: true for .em exports (name table has no sigs); sema skips arg/return checks
};
// module_alias -> list of exported fns. Host populates from the registry.
using ModuleExportTable = std::unordered_map<std::string, std::vector<ModuleExport>>;

// A registered native function signature (host-side; mirrors NativeFn
// from BINDING_API.md but sema only needs the type info + fn ptr).
struct NativeSig {
    std::string name;
    void* fn_ptr = nullptr;
    Type ret;
    std::vector<Type> params;
    uint32_t permission = 0;
};

// Operator-overload registry: (type_name, operator) -> native fn ptr + sig.
// Host registers overloads for a type (e.g. "vec3" + Add -> vec3_add).
// Sema rewrites `v1 + v2` (where v1/v2 are typed as the registered type)
// into a native call to the overload fn. (BINDING_API.md Section 3 / TYPE_SYSTEM.md Section 7)
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

// Struct value types (task 1.6): field offsets within a POD frame slot.
// v1 scope: fields must be primitive types (no nested struct/array/slice
// fields), tightly packed in declaration order (no alignment padding - these
// are JIT-internal frame layouts only, never passed across the native-call
// ABI boundary, so there's no external struct layout to match).
struct StructFieldLayout { const Type* ty; int32_t offset; };
struct StructLayout {
    int32_t size = 0;
    std::vector<std::string> field_names;   // declaration order
    std::unordered_map<std::string, StructFieldLayout> fields;
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
bool try_eval_const_bool(const Expr& e, bool& out);

struct SemaResult {
    bool ok = true;
    std::vector<SemaError> errors;
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
