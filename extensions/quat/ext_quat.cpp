// ext_quat.cpp - ember extension: quat host-store type + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (quat block)
// during the restructure Section 6 audit. Math + handle ABI + overload set
// unchanged; only namespace + registration entry points changed.
#include "ext_quat.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include <new>
#include <stdexcept>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_quat {

// --- quat host store (opaque i64 handle; host owns {float x,y,z,w}) ---
struct Quat { float x, y, z, w; };
static std::vector<Quat> g_quats;
static int64_t q_new(float x, float y, float z, float w) noexcept {
    try {
        g_quats.push_back({x,y,z,w});
        return int64_t(g_quats.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static Quat* q_slot(int64_t h) { if (h<1 || h>int64_t(g_quats.size())) return nullptr; return &g_quats[size_t(h-1)]; }
extern "C" {
    static int64_t n_quat_new(float x, float y, float z, float w) { return q_new(x,y,z,w); }
    static float n_quat_x(int64_t h) { auto* v=q_slot(h); return v?v->x:0; }
    static float n_quat_y(int64_t h) { auto* v=q_slot(h); return v?v->y:0; }
    static float n_quat_z(int64_t h) { auto* v=q_slot(h); return v?v->z:0; }
    static float n_quat_w(int64_t h) { auto* v=q_slot(h); return v?v->w:0; }
    static void n_quat_set_x(int64_t h, float v) { auto* s=q_slot(h); if(s) s->x=v; }
    static void n_quat_set_y(int64_t h, float v) { auto* s=q_slot(h); if(s) s->y=v; }
    static void n_quat_set_z(int64_t h, float v) { auto* s=q_slot(h); if(s) s->z=v; }
    static void n_quat_set_w(int64_t h, float v) { auto* s=q_slot(h); if(s) s->w=v; }
    static int64_t n_quat_add(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return (x&&y)?q_new(x->x+y->x, x->y+y->y, x->z+y->z, x->w+y->w):0; }
    static int64_t n_quat_sub(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return (x&&y)?q_new(x->x-y->x, x->y-y->y, x->z-y->z, x->w-y->w):0; }
    // Hamilton product (real quaternion multiplication, not component-wise -
    // this is what `*` means mathematically for a quat).
    static int64_t n_quat_mul(int64_t a, int64_t b) {
        auto* p=q_slot(a); auto* q=q_slot(b);
        if (!p || !q) return 0;
        float x = p->w*q->x + p->x*q->w + p->y*q->z - p->z*q->y;
        float y = p->w*q->y - p->x*q->z + p->y*q->w + p->z*q->x;
        float z = p->w*q->z + p->x*q->y - p->y*q->x + p->z*q->w;
        float w = p->w*q->w - p->x*q->x - p->y*q->y - p->z*q->z;
        return q_new(x, y, z, w);
    }
    static int64_t n_quat_eq(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return (x&&y&&x->x==y->x && x->y==y->y && x->z==y->z && x->w==y->w) ? 1 : 0; }
}

// Registered surface is byte-identical to the old I/H/add lambda form
// (ext_registration_test asserts quat_new -> struct "quat", 4 f32 params).
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("quat_new", bind_handle("quat"), {type_f32(),type_f32(),type_f32(),type_f32()}, (void*)&n_quat_new);
    b.add("quat_x",   type_f32(), {bind_handle("quat")}, (void*)&n_quat_x);
    b.add("quat_y",   type_f32(), {bind_handle("quat")}, (void*)&n_quat_y);
    b.add("quat_z",   type_f32(), {bind_handle("quat")}, (void*)&n_quat_z);
    b.add("quat_w",   type_f32(), {bind_handle("quat")}, (void*)&n_quat_w);
    b.add("quat_set_x",type_void(),{bind_handle("quat"),type_f32()}, (void*)&n_quat_set_x);
    b.add("quat_set_y",type_void(),{bind_handle("quat"),type_f32()}, (void*)&n_quat_set_y);
    b.add("quat_set_z",type_void(),{bind_handle("quat"),type_f32()}, (void*)&n_quat_set_z);
    b.add("quat_set_w",type_void(),{bind_handle("quat"),type_f32()}, (void*)&n_quat_set_w);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Overload (type,op) entries preserved exactly. quat's `*` is the Hamilton
// product (n_quat_mul) - the mathematically meaningful "multiply" for a quat.
void register_overloads(OpOverloadTable& overloads) {
    BindingBuilder b;
    b.add_overload("quat", int(BinExpr::Op::Add), bind_handle("quat"), (void*)&n_quat_add);
    b.add_overload("quat", int(BinExpr::Op::Sub), bind_handle("quat"), (void*)&n_quat_sub);
    b.add_overload("quat", int(BinExpr::Op::Mul), bind_handle("quat"), (void*)&n_quat_mul);
    b.add_overload("quat", int(BinExpr::Op::Eq),  type_bool(),         (void*)&n_quat_eq);
    NativeTable t = b.build();
    for (auto& kv : t.overloads.entries) overloads.entries[kv.first] = std::move(kv.second);
}

void reset() {
    g_quats.clear();
}

} // namespace ember::ext_quat
