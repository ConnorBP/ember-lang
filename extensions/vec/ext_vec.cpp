// ext_vec.cpp - ember extension: vec2/vec3/vec4 host-store types + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (the
// vec2/vec3/vec4 host-store blocks) during the restructure Section 6 audit. The
// only change is namespace + the registration entry points; the math,
// the handle ABI, and the overload set are unchanged so existing scripts
// (e.g. prism/examples/scripts/vector_math_demo.ember) behave identically.
#include "ext_vec.hpp"
#include "ast.hpp"       // ember public: Type, make_prim, make_slice, Prim
#include <vector>

namespace ember::ext_vec {

// --- vec3 host store (opaque i64 handle; host owns {float x,y,z}) ---
struct Vec3 { float x, y, z; };
static std::vector<Vec3> g_vec3s;
static int64_t v3_new(float x, float y, float z) { g_vec3s.push_back({x,y,z}); return int64_t(g_vec3s.size()); }
static Vec3* v3_slot(int64_t h) { if (h<1 || h>int64_t(g_vec3s.size())) return nullptr; return &g_vec3s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec3_new(float x, float y, float z) { return v3_new(x,y,z); }
    static float n_vec3_x(int64_t h) { auto* v=v3_slot(h); return v?v->x:0; }
    static float n_vec3_y(int64_t h) { auto* v=v3_slot(h); return v?v->y:0; }
    static float n_vec3_z(int64_t h) { auto* v=v3_slot(h); return v?v->z:0; }
    static void n_vec3_set_x(int64_t h, float v) { auto* s=v3_slot(h); if(s) s->x=v; }
    static void n_vec3_set_y(int64_t h, float v) { auto* s=v3_slot(h); if(s) s->y=v; }
    static void n_vec3_set_z(int64_t h, float v) { auto* s=v3_slot(h); if(s) s->z=v; }
    static int64_t n_vec3_add(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return v3_new(x->x+y->x, x->y+y->y, x->z+y->z); }
    static int64_t n_vec3_sub(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return v3_new(x->x-y->x, x->y-y->y, x->z-y->z); }
    static int64_t n_vec3_mul(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return v3_new(x->x*y->x, x->y*y->y, x->z*y->z); }
    static int64_t n_vec3_eq(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return (x->x==y->x && x->y==y->y && x->z==y->z) ? 1 : 0; }
}

// --- vec2 host store (opaque i64 handle; host owns {float x,y}) ---
struct Vec2 { float x, y; };
static std::vector<Vec2> g_vec2s;
static int64_t v2_new(float x, float y) { g_vec2s.push_back({x,y}); return int64_t(g_vec2s.size()); }
static Vec2* v2_slot(int64_t h) { if (h<1 || h>int64_t(g_vec2s.size())) return nullptr; return &g_vec2s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec2_new(float x, float y) { return v2_new(x,y); }
    static float n_vec2_x(int64_t h) { auto* v=v2_slot(h); return v?v->x:0; }
    static float n_vec2_y(int64_t h) { auto* v=v2_slot(h); return v?v->y:0; }
    static void n_vec2_set_x(int64_t h, float v) { auto* s=v2_slot(h); if(s) s->x=v; }
    static void n_vec2_set_y(int64_t h, float v) { auto* s=v2_slot(h); if(s) s->y=v; }
    static int64_t n_vec2_add(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return v2_new(x->x+y->x, x->y+y->y); }
    static int64_t n_vec2_sub(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return v2_new(x->x-y->x, x->y-y->y); }
    static int64_t n_vec2_mul(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return v2_new(x->x*y->x, x->y*y->y); }
    static int64_t n_vec2_eq(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return (x->x==y->x && x->y==y->y) ? 1 : 0; }
}

// --- vec4 host store (opaque i64 handle; host owns {float x,y,z,w}) ---
struct Vec4 { float x, y, z, w; };
static std::vector<Vec4> g_vec4s;
static int64_t v4_new(float x, float y, float z, float w) { g_vec4s.push_back({x,y,z,w}); return int64_t(g_vec4s.size()); }
static Vec4* v4_slot(int64_t h) { if (h<1 || h>int64_t(g_vec4s.size())) return nullptr; return &g_vec4s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec4_new(float x, float y, float z, float w) { return v4_new(x,y,z,w); }
    static float n_vec4_x(int64_t h) { auto* v=v4_slot(h); return v?v->x:0; }
    static float n_vec4_y(int64_t h) { auto* v=v4_slot(h); return v?v->y:0; }
    static float n_vec4_z(int64_t h) { auto* v=v4_slot(h); return v?v->z:0; }
    static float n_vec4_w(int64_t h) { auto* v=v4_slot(h); return v?v->w:0; }
    static void n_vec4_set_x(int64_t h, float v) { auto* s=v4_slot(h); if(s) s->x=v; }
    static void n_vec4_set_y(int64_t h, float v) { auto* s=v4_slot(h); if(s) s->y=v; }
    static void n_vec4_set_z(int64_t h, float v) { auto* s=v4_slot(h); if(s) s->z=v; }
    static void n_vec4_set_w(int64_t h, float v) { auto* s=v4_slot(h); if(s) s->w=v; }
    static int64_t n_vec4_add(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return v4_new(x->x+y->x, x->y+y->y, x->z+y->z, x->w+y->w); }
    static int64_t n_vec4_sub(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return v4_new(x->x-y->x, x->y-y->y, x->z-y->z, x->w-y->w); }
    static int64_t n_vec4_mul(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return v4_new(x->x*y->x, x->y*y->y, x->z*y->z, x->w*y->w); }
    static int64_t n_vec4_eq(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return (x->x==y->x && x->y==y->y && x->z==y->z && x->w==y->w) ? 1 : 0; }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto add = [&](const char* n, void* fn, Type r, std::vector<Type> ps) {
        m[n] = NativeSig{n, fn, std::move(r), std::move(ps), 0};
    };
    add("vec3_new", (void*)&n_vec3_new, H("vec3"), {I(Prim::F32),I(Prim::F32),I(Prim::F32)});
    add("vec3_x",   (void*)&n_vec3_x,   I(Prim::F32), {I(Prim::I64)});
    add("vec3_y",   (void*)&n_vec3_y,   I(Prim::F32), {I(Prim::I64)});
    add("vec3_z",   (void*)&n_vec3_z,   I(Prim::F32), {I(Prim::I64)});
    add("vec3_set_x",(void*)&n_vec3_set_x,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec3_set_y",(void*)&n_vec3_set_y,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec3_set_z",(void*)&n_vec3_set_z,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec2_new", (void*)&n_vec2_new, H("vec2"), {I(Prim::F32),I(Prim::F32)});
    add("vec2_x",   (void*)&n_vec2_x,   I(Prim::F32), {I(Prim::I64)});
    add("vec2_y",   (void*)&n_vec2_y,   I(Prim::F32), {I(Prim::I64)});
    add("vec2_set_x",(void*)&n_vec2_set_x,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec2_set_y",(void*)&n_vec2_set_y,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec4_new", (void*)&n_vec4_new, H("vec4"), {I(Prim::F32),I(Prim::F32),I(Prim::F32),I(Prim::F32)});
    add("vec4_x",   (void*)&n_vec4_x,   I(Prim::F32), {I(Prim::I64)});
    add("vec4_y",   (void*)&n_vec4_y,   I(Prim::F32), {I(Prim::I64)});
    add("vec4_z",   (void*)&n_vec4_z,   I(Prim::F32), {I(Prim::I64)});
    add("vec4_w",   (void*)&n_vec4_w,   I(Prim::F32), {I(Prim::I64)});
    add("vec4_set_x",(void*)&n_vec4_set_x,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec4_set_y",(void*)&n_vec4_set_y,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec4_set_z",(void*)&n_vec4_set_z,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("vec4_set_w",(void*)&n_vec4_set_w,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
}

void register_overloads(OpOverloadTable& overloads) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto reg_op = [&](const char* type_name, int op, void* fn, Type ret) {
        overloads.register_op(type_name, op, {fn, "", ret, {I(Prim::I64),I(Prim::I64)}});
    };
    reg_op("vec3", int(BinExpr::Op::Add), (void*)&n_vec3_add, H("vec3"));
    reg_op("vec3", int(BinExpr::Op::Sub), (void*)&n_vec3_sub, H("vec3"));
    reg_op("vec3", int(BinExpr::Op::Mul), (void*)&n_vec3_mul, H("vec3"));
    reg_op("vec3", int(BinExpr::Op::Eq),  (void*)&n_vec3_eq,  I(Prim::Bool));

    reg_op("vec2", int(BinExpr::Op::Add), (void*)&n_vec2_add, H("vec2"));
    reg_op("vec2", int(BinExpr::Op::Sub), (void*)&n_vec2_sub, H("vec2"));
    reg_op("vec2", int(BinExpr::Op::Mul), (void*)&n_vec2_mul, H("vec2"));
    reg_op("vec2", int(BinExpr::Op::Eq),  (void*)&n_vec2_eq,  I(Prim::Bool));

    reg_op("vec4", int(BinExpr::Op::Add), (void*)&n_vec4_add, H("vec4"));
    reg_op("vec4", int(BinExpr::Op::Sub), (void*)&n_vec4_sub, H("vec4"));
    reg_op("vec4", int(BinExpr::Op::Mul), (void*)&n_vec4_mul, H("vec4"));
    reg_op("vec4", int(BinExpr::Op::Eq),  (void*)&n_vec4_eq,  I(Prim::Bool));
}

void reset() {
    g_vec2s.clear(); g_vec3s.clear(); g_vec4s.clear();
}

} // namespace ember::ext_vec
