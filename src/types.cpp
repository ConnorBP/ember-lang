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
    if (is_slice != o.is_slice) return false;
    if (array_len != o.array_len) return false;
    if (elem && o.elem) { if (!elem->same(*o.elem)) return false; }
    else if (elem || o.elem) return false;
    return true;
}
std::string Type::to_string() const {
    if (is_void()) return "void";
    if (is_slice) return (elem?elem->to_string():std::string("?")) + "[]";
    if (array_len) return (elem?elem->to_string():std::string("?")) + "[" + std::to_string(array_len) + "]";
    if (!struct_name.empty()) return struct_name;
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
