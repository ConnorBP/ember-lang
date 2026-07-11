#include "ast.hpp"
#include <cassert>

namespace ember {

bool Type::is_int() const {
    switch (prim) {
    case Prim::I8:case Prim::I16:case Prim::I32:case Prim::I64:
    case Prim::U8:case Prim::U16:case Prim::U32:case Prim::U64:
        return struct_name.empty() && !is_slice && array_len==0;
    default: return false;
    }
}
bool Type::is_uint() const {
    switch (prim) {
    case Prim::U8:case Prim::U16:case Prim::U32:case Prim::U64:
        return struct_name.empty() && !is_slice && array_len==0;
    default: return false;
    }
}
bool Type::is_float() const {
    return (prim==Prim::F32 || prim==Prim::F64) && struct_name.empty() && !is_slice && array_len==0;
}
bool Type::is_bool() const {
    return prim==Prim::Bool && struct_name.empty() && !is_slice && array_len==0;
}

size_t Type::byte_size() const {
    if (is_slice) return 16;             // {ptr,len}
    if (array_len) return size_t(array_len) * (elem ? elem->byte_size() : 1);
    switch (prim) {
    case Prim::Void: return 0;
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    case Prim::I64: case Prim::U64: case Prim::F64: return 8;
    }
    if (!struct_name.empty()) return 0;  // resolved by sema from registered layout
    return 0;
}
uint32_t Type::align() const {
    if (is_slice) return 8;
    if (array_len) return elem ? elem->align() : 1;
    switch (prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    case Prim::I64: case Prim::U64: case Prim::F64: return 8;
    default: return 1;
    }
}
bool Type::same(const Type& o) const {
    if (prim != o.prim) return false;
    if (struct_name != o.struct_name) return false;
    if (enum_name != o.enum_name) return false;
    if (is_slice != o.is_slice) return false;
    if (array_len != o.array_len) return false;
    if (elem && o.elem) { if (!elem->same(*o.elem)) return false; }
    else if (elem || o.elem) return false;
    // v1.0 Tier 2 fn handles: a fn handle is `same` to another fn handle iff
    // both is_fn_handle AND (both carry matching recorded sigs, OR neither has
    // a recorded sig — a bare `fn` param accepts a specific-fn arg, the one
    // subtyping direction we allow; safe because the runtime guard still
    // validates the handle, plan §4.4). A fn handle is NOT same to a plain i64
    // (distinguishes `&fn` from `let x: i64 = 5`).
    if (is_fn_handle != o.is_fn_handle) return false;
    if (is_fn_handle) {
        if (has_recorded_sig && o.has_recorded_sig) {
            if (recorded_params.size() != o.recorded_params.size()) return false;
            for (size_t i = 0; i < recorded_params.size(); ++i)
                if (!recorded_params[i]->same(*o.recorded_params[i])) return false;
            if (recorded_ret && o.recorded_ret) { if (!recorded_ret->same(*o.recorded_ret)) return false; }
            else if (recorded_ret || o.recorded_ret) return false;
        }
        // one recorded + one bare (or both bare): same (the bare-`fn` accepts).
    }
    return true;
}
std::string Type::to_string() const {
    if (is_void()) return "void";
    if (is_slice) return (elem?elem->to_string():std::string("?")) + "[]";
    if (array_len) return (elem?elem->to_string():std::string("?")) + "[" + std::to_string(array_len) + "]";
    if (!enum_name.empty()) return enum_name;   // typed enum type (enum E : T)
    if (!struct_name.empty()) return struct_name;
    if (is_fn_handle) {
        // v1.0 Tier 2: a fn handle displays as `fn` (bare) or `fn(A, B)->R`
        // (parameterized, when has_recorded_sig). The parameterized form is
        // the type-system-sound spelling; bare `fn` is the unsound fallback.
        if (!has_recorded_sig) return "fn";
        std::string s = "fn(";
        for (size_t i = 0; i < recorded_params.size(); ++i) {
            if (i) s += ", ";
            s += recorded_params[i] ? recorded_params[i]->to_string() : std::string("?");
        }
        s += ") -> ";
        s += recorded_ret ? recorded_ret->to_string() : std::string("?");
        return s;
    }
    switch (prim) {
    case Prim::Bool: return "bool";
    case Prim::I8: return "i8"; case Prim::I16: return "i16"; case Prim::I32: return "i32"; case Prim::I64: return "i64";
    case Prim::U8: return "u8"; case Prim::U16: return "u16"; case Prim::U32: return "u32"; case Prim::U64: return "u64";
    case Prim::F32: return "f32"; case Prim::F64: return "f64";
    default: return "?";
    }
}

const Type& type_void() { static Type t; t.prim = Prim::Void; return t; }
const Type& type_bool() { static Type t; t.prim = Prim::Bool; return t; }
const Type& type_i64()  { static Type t; t.prim = Prim::I64;  return t; }
const Type& type_u64()  { static Type t; t.prim = Prim::U64;  return t; }
const Type& type_f32()  { static Type t; t.prim = Prim::F32;  return t; }
const Type& type_f64()  { static Type t; t.prim = Prim::F64;  return t; }
Type make_slice(std::shared_ptr<Type> e) { Type t; t.is_slice=true; t.elem=e; return t; }
Type make_array(std::shared_ptr<Type> e, uint32_t n){ Type t; t.array_len=n; t.elem=e; return t; }
Type make_struct(std::string n){ Type t; t.struct_name=std::move(n); return t; }
Type make_prim(Prim p){ Type t; t.prim=p; return t; }

} // namespace ember
