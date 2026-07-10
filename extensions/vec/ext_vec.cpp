// ext_vec.cpp - ember extension: vec2/vec3/vec4 host-store types + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (the
// vec2/vec3/vec4 host-store blocks) during the restructure Section 6 audit. The
// only change is namespace + the registration entry points; the math,
// the handle ABI, and the overload set are unchanged so existing scripts
// (e.g. prism/examples/scripts/vector_math_demo.ember) behave identically.
#include "ext_vec.hpp"
#include "ast.hpp"       // ember public: Type, make_prim, make_slice, Prim
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

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
    static int64_t n_vec3_add(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x+y->x, x->y+y->y, x->z+y->z):0; }
    static int64_t n_vec3_sub(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x-y->x, x->y-y->y, x->z-y->z):0; }
    static int64_t n_vec3_mul(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x*y->x, x->y*y->y, x->z*y->z):0; }
    static int64_t n_vec3_eq(int64_t a, int64_t b) { auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y&&x->x==y->x && x->y==y->y && x->z==y->z) ? 1 : 0; }
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
    static int64_t n_vec2_add(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x+y->x, x->y+y->y):0; }
    static int64_t n_vec2_sub(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x-y->x, x->y-y->y):0; }
    static int64_t n_vec2_mul(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x*y->x, x->y*y->y):0; }
    static int64_t n_vec2_eq(int64_t a, int64_t b) { auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y&&x->x==y->x && x->y==y->y) ? 1 : 0; }
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
    static int64_t n_vec4_add(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x+y->x, x->y+y->y, x->z+y->z, x->w+y->w):0; }
    static int64_t n_vec4_sub(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x-y->x, x->y-y->y, x->z-y->z, x->w-y->w):0; }
    static int64_t n_vec4_mul(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x*y->x, x->y*y->y, x->z*y->z, x->w*y->w):0; }
    static int64_t n_vec4_eq(int64_t a, int64_t b) { auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y&&x->x==y->x && x->y==y->y && x->z==y->z && x->w==y->w) ? 1 : 0; }
}

// Registered surface is byte-identical to the old I/H/add lambda form:
// bind_handle("vec3") == old H("vec3"); type_f32() == old I(Prim::F32).
// (ext_registration_test asserts the names/arity/ret/struct_name.)
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("vec3_new", bind_handle("vec3"), {type_f32(),type_f32(),type_f32()}, (void*)&n_vec3_new);
    b.add("vec3_x",   type_f32(), {bind_handle("vec3")}, (void*)&n_vec3_x);
    b.add("vec3_y",   type_f32(), {bind_handle("vec3")}, (void*)&n_vec3_y);
    b.add("vec3_z",   type_f32(), {bind_handle("vec3")}, (void*)&n_vec3_z);
    b.add("vec3_set_x",type_void(),{bind_handle("vec3"),type_f32()}, (void*)&n_vec3_set_x);
    b.add("vec3_set_y",type_void(),{bind_handle("vec3"),type_f32()}, (void*)&n_vec3_set_y);
    b.add("vec3_set_z",type_void(),{bind_handle("vec3"),type_f32()}, (void*)&n_vec3_set_z);
    b.add("vec2_new", bind_handle("vec2"), {type_f32(),type_f32()}, (void*)&n_vec2_new);
    b.add("vec2_x",   type_f32(), {bind_handle("vec2")}, (void*)&n_vec2_x);
    b.add("vec2_y",   type_f32(), {bind_handle("vec2")}, (void*)&n_vec2_y);
    b.add("vec2_set_x",type_void(),{bind_handle("vec2"),type_f32()}, (void*)&n_vec2_set_x);
    b.add("vec2_set_y",type_void(),{bind_handle("vec2"),type_f32()}, (void*)&n_vec2_set_y);
    b.add("vec4_new", bind_handle("vec4"), {type_f32(),type_f32(),type_f32(),type_f32()}, (void*)&n_vec4_new);
    b.add("vec4_x",   type_f32(), {bind_handle("vec4")}, (void*)&n_vec4_x);
    b.add("vec4_y",   type_f32(), {bind_handle("vec4")}, (void*)&n_vec4_y);
    b.add("vec4_z",   type_f32(), {bind_handle("vec4")}, (void*)&n_vec4_z);
    b.add("vec4_w",   type_f32(), {bind_handle("vec4")}, (void*)&n_vec4_w);
    b.add("vec4_set_x",type_void(),{bind_handle("vec4"),type_f32()}, (void*)&n_vec4_set_x);
    b.add("vec4_set_y",type_void(),{bind_handle("vec4"),type_f32()}, (void*)&n_vec4_set_y);
    b.add("vec4_set_z",type_void(),{bind_handle("vec4"),type_f32()}, (void*)&n_vec4_set_z);
    b.add("vec4_set_w",type_void(),{bind_handle("vec4"),type_f32()}, (void*)&n_vec4_set_w);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Overload (type,op) entries preserve the same target/signature. BindingBuilder
// also assigns the stable symbolic names (vec3_add, vec3_eq, ...) that sema
// carries into v2 .em native-binding records.
void register_overloads(OpOverloadTable& overloads) {
    BindingBuilder b;
    b.add_overload("vec3", int(BinExpr::Op::Add), bind_handle("vec3"), (void*)&n_vec3_add);
    b.add_overload("vec3", int(BinExpr::Op::Sub), bind_handle("vec3"), (void*)&n_vec3_sub);
    b.add_overload("vec3", int(BinExpr::Op::Mul), bind_handle("vec3"), (void*)&n_vec3_mul);
    b.add_overload("vec3", int(BinExpr::Op::Eq),  type_bool(),          (void*)&n_vec3_eq);

    b.add_overload("vec2", int(BinExpr::Op::Add), bind_handle("vec2"), (void*)&n_vec2_add);
    b.add_overload("vec2", int(BinExpr::Op::Sub), bind_handle("vec2"), (void*)&n_vec2_sub);
    b.add_overload("vec2", int(BinExpr::Op::Mul), bind_handle("vec2"), (void*)&n_vec2_mul);
    b.add_overload("vec2", int(BinExpr::Op::Eq),  type_bool(),          (void*)&n_vec2_eq);

    b.add_overload("vec4", int(BinExpr::Op::Add), bind_handle("vec4"), (void*)&n_vec4_add);
    b.add_overload("vec4", int(BinExpr::Op::Sub), bind_handle("vec4"), (void*)&n_vec4_sub);
    b.add_overload("vec4", int(BinExpr::Op::Mul), bind_handle("vec4"), (void*)&n_vec4_mul);
    b.add_overload("vec4", int(BinExpr::Op::Eq),  type_bool(),          (void*)&n_vec4_eq);

    NativeTable t = b.build();
    for (auto& kv : t.overloads.entries) overloads.entries[kv.first] = std::move(kv.second);
}

void reset() {
    g_vec2s.clear(); g_vec3s.clear(); g_vec4s.clear();
}

} // namespace ember::ext_vec
