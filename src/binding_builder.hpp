// ember binding builder - ergonomic host-side native registration.
//
// v0.3: the working binding API. Extracts the I/H/add boilerplate each
// extension redefined into one shared header (six copies deduped). This is
// the "bindings like AngelScript" floor: RegisterGlobalFunction("sig", &fn)
// shape, one declarative call per native, over the direct NativeSig map.
// The fluent TypeBuilder/StructBuilder/engine_t surface in BINDING_API.md is
// the v1.0 ergonomic target, deferred until a real host wants script-visible
// C++ struct types (these extensions use opaque i64 handles + StructLayoutTable).
//
// Usage:
//   BindingBuilder b;
//   b.add("sqrt",        type_f32(), {type_f32()},                  &my_sqrt);
//   b.add("vec3_new",    struct_t("vec3"), {type_f32(),type_f32(),type_f32()}, &vec3_new);
//   b.add_struct_ret("vec3_x", "vec3", {type_i64()},                &vec3_x); // alt form
//   b.add_overload("vec3", BinExpr::Op::Add, struct_t("vec3"),      &vec3_add);
//   NativeTable t = b.build();
//   SemaResult sr = sema(prog, t.natives, slots, 0, &t.overloads, &layouts);
//
// permission defaults to 0 (no FFI). Set PERM_FFI on natives that should be
// gated behind the FFI permission flag (full codegen-level gating is v0.4
// safety; v0.3 just defines the constant so NativeSig::permission is usable).
#pragma once
#include "ast.hpp"        // Type, Prim, BinExpr::Op, make_prim, make_struct
#include "sema.hpp"        // NativeSig, OpOverload, OpOverloadTable
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// Permission flag bits for NativeSig::permission. v0.3: defined but not
// enforced in codegen (the "check permission before marshalling" is the v0.4
// safety milestone). Hosts may set them; sema stores them on the NativeSig.
// The FFI bit marks natives that perform raw host calls/pointer access and
// should be unavailable to scripts without the FFI module permission.
inline constexpr uint32_t PERM_FFI = 1u << 0;

// Convenience: a primitive Type (Type{Prim::F32} etc.).
inline Type bind_prim(Prim p) { return make_prim(p); }
// Convenience: an opaque host-struct handle (i64 with a struct_name tag,
// the convention extensions use: script holds an i64, the struct_name is
// the type-system tag for operator-overload resolution).
inline Type bind_handle(const char* struct_name) {
    Type t; t.prim = Prim::I64; t.struct_name = struct_name; return t;
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
    BindingBuilder& add(const char* name, Type ret, std::vector<Type> params,
                        void* fn, uint32_t permission = 0) {
        NativeSig s;
        s.name = name;
        s.fn_ptr = fn;
        s.ret = std::move(ret);
        s.params = std::move(params);
        s.permission = permission;
        natives_[name] = std::move(s);
        return *this;
    }

    // Register an operator overload for a struct-tagged type.
    // Overload fns take (i64, i64) -> ret (the two struct handles).
    BindingBuilder& add_overload(const char* type_name, int op, Type ret, void* fn) {
        OpOverload o;
        o.fn_ptr = fn;
        o.fn_name = std::string(type_name) + "_op";
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
