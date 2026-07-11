// ember binding builder - ergonomic host-side native registration.
//
// v0.3: the working binding API. Extracts the I/H/add boilerplate each
// extension redefined into one shared header (six copies deduped). This is
// the "bindings like AngelScript" floor: RegisterGlobalFunction("sig", &fn)
// shape, one declarative call per native, over the direct NativeSig map.
// The fluent TypeBuilder/StructBuilder/engine_t surface in docs/spec/BINDING_API.md is
// the v1.0 ergonomic target, deferred until a real host wants script-visible
// C++ struct types (these extensions use opaque i64 handles + StructLayoutTable).
//
// Usage (the actual API):
//   BindingBuilder b;
//   b.add("sqrt",     bind_prim(Prim::F32), {bind_prim(Prim::F32)}, &my_sqrt);
//   b.add("vec3_new", bind_handle("vec3"),
//         {bind_prim(Prim::F32), bind_prim(Prim::F32), bind_prim(Prim::F32)},
//         &vec3_new);
//   b.add_overload("vec3", int(BinExpr::Op::Add),
//                  bind_handle("vec3"), &vec3_add);
//   NativeTable t = b.build();
//   SemaResult sr = sema(prog, t.natives, slots, permissions,
//                        &t.overloads, &layouts);
//
// permission defaults to 0 (no FFI). Set PERM_FFI on natives that should be
// gated behind the FFI permission flag. Sema enforces the permission at each
// native call site before codegen; there is no runtime permission branch.
#pragma once
#include "ast.hpp"        // Type, Prim, BinExpr::Op, make_prim, make_struct
#include "sema.hpp"        // NativeSig, OpOverload, OpOverloadTable, StructLayoutTable, StructLayout
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// Permission flag bits for NativeSig::permission. Sema enforces these at
// native call sites before codegen. The FFI bit marks natives that perform raw
// host calls/pointer access and must be unavailable unless the compiling module
// was granted the FFI permission.
inline constexpr uint32_t PERM_FFI = 1u << 0;

// Convenience: a primitive Type (Type{Prim::F32} etc.).
inline Type bind_prim(Prim p) { return make_prim(p); }
// Convenience: an opaque host-struct handle (i64 with a struct_name tag,
// the convention extensions use: script holds an i64, the struct_name is
// the type-system tag for operator-overload resolution).
inline Type bind_handle(const char* struct_name) {
    Type t; t.prim = Prim::I64; t.struct_name = struct_name; return t;
}

// ─── Host C++ struct registration (first-class by-value types) ───
// register_struct adds a host C++ struct to the StructLayoutTable with
// C++-compatible layout (alignment + padding + trailing padding), so it
// can be passed by value to/from natives with the correct byte layout.
// The struct is visible in script as a named type (e.g. `Vec3`), and
// sema/codegen treat it as a first-class by-value struct (same as script-
// declared structs).
//
// Usage:
//   StructLayoutTable layouts = build_struct_layouts(prog);
//   register_struct(layouts, "Vec3", {
//       {"x", bind_prim(Prim::F32)},
//       {"y", bind_prim(Prim::F32)},
//       {"z", bind_prim(Prim::F32)},
//   });
//   sema(prog, natives, slots, 0, &overloads, &layouts);
//
// The layout uses C++ alignment rules: each field is aligned to its
// natural alignment, padding is inserted between fields, and the struct
// size is rounded up to the struct's alignment (trailing padding).
struct HostStructField {
    const char* name;
    Type ty;
};
inline void register_struct(StructLayoutTable& table, const char* name,
                            std::vector<HostStructField> fields) {
    StructLayout layout;
    int32_t off = 0;
    uint32_t struct_align = 1;
    for (auto& f : fields) {
        uint32_t a = f.ty.align();
        if (a > struct_align) struct_align = a;
        // Align the field offset to the field's alignment.
        off = (off + int32_t(a) - 1) & ~int32_t(a - 1);
        // Store the field layout. The Type is owned by the layout via
        // shared_ptr so the const Type* pointer stays valid.
        auto sp = std::make_shared<Type>(std::move(f.ty));
        layout.fields[f.name] = StructFieldLayout{sp.get(), off};
        layout.field_names.push_back(f.name);
        layout.owned_field_types.push_back(sp);  // keep alive
        off += int32_t(sp->byte_size());
    }
    // Trailing padding: round up to struct alignment.
    off = (off + int32_t(struct_align) - 1) & ~int32_t(struct_align - 1);
    layout.size = off;
    table[name] = std::move(layout);
}

// Convenience: make a by-value struct type (prim=Void, struct_name set).
// This is the type a script variable/argument would have for a host struct.
inline Type bind_struct(const char* struct_name) {
    Type t; t.prim = Prim::Void; t.struct_name = struct_name; return t;
}

// The built binding table handed to sema().
struct NativeTable {
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
};

// Ergonomic builder. Each add() is one native; add_overload() registers an
// operator overload for a tagged struct type. build() returns the filled
// NativeTable (moves out, so call once after all add()s).
class BindingBuilder {
public:
    // Register a native function: name, return type, param types, fn ptr.
    // permission defaults 0; pass PERM_FFI for FFI-gated natives.
    // retains defaults false (the native copies/consumes slice bytes and drops
    // the ptr during the call — the only shipped slice-taking native,
    // string_from_slice, copies). Pass retains=true ONLY for a native that
    // stores a slice ptr past the call (semA then rejects stack-backed slice
    // args to it, slice-escape safety C3). See NativeSig::retains.
    BindingBuilder& add(const char* name, Type ret, std::vector<Type> params,
                        void* fn, uint32_t permission = 0, bool retains = false) {
        NativeSig s;
        s.name = name;
        s.fn_ptr = fn;
        s.ret = std::move(ret);
        s.params = std::move(params);
        s.permission = permission;
        s.retains = retains;
        natives_[name] = std::move(s);
        return *this;
    }

    // Register an operator overload for a struct-tagged type.
    // Overload fns take (i64, i64) -> ret (the two struct handles).
    BindingBuilder& add_overload(const char* type_name, int op, Type ret, void* fn) {
        OpOverload o;
        o.fn_ptr = fn;
        const char* suffix = "op";
        switch (static_cast<BinExpr::Op>(op)) {
        case BinExpr::Op::Add: suffix = "add"; break;
        case BinExpr::Op::Sub: suffix = "sub"; break;
        case BinExpr::Op::Mul: suffix = "mul"; break;
        case BinExpr::Op::Div: suffix = "div"; break;
        case BinExpr::Op::Mod: suffix = "mod"; break;
        case BinExpr::Op::And: suffix = "and"; break;
        case BinExpr::Op::Or: suffix = "or"; break;
        case BinExpr::Op::Xor: suffix = "xor"; break;
        case BinExpr::Op::Shl: suffix = "shl"; break;
        case BinExpr::Op::Shr: suffix = "shr"; break;
        case BinExpr::Op::Eq: suffix = "eq"; break;
        case BinExpr::Op::Neq: suffix = "neq"; break;
        case BinExpr::Op::Lt: suffix = "lt"; break;
        case BinExpr::Op::Le: suffix = "le"; break;
        case BinExpr::Op::Gt: suffix = "gt"; break;
        case BinExpr::Op::Ge: suffix = "ge"; break;
        case BinExpr::Op::LAnd: suffix = "land"; break;
        case BinExpr::Op::LOr: suffix = "lor"; break;
        }
        o.fn_name = std::string(type_name) + "_" + suffix;
        o.ret = std::move(ret);
        o.params = {bind_prim(Prim::I64), bind_prim(Prim::I64)};
        overloads_.register_op(type_name, op, std::move(o));
        return *this;
    }

    NativeTable build() {
        NativeTable t;
        t.natives = std::move(natives_);
        t.overloads = std::move(overloads_);
        return t;
    }

private:
    std::unordered_map<std::string, NativeSig> natives_;
    OpOverloadTable overloads_;
};

} // namespace ember
