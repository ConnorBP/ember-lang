// ext_quat.cpp - ember extension: quat host-store type + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (quat block)
// during the restructure Section 6 audit. Math + handle ABI + overload set
// unchanged; only namespace + registration entry points changed.
#include "ext_quat.hpp"
#include "ast.hpp"
#include <vector>

namespace ember::ext_quat {

// --- quat host store (opaque i64 handle; host owns {float x,y,z,w}) ---
struct Quat { float x, y, z, w; };
static std::vector<Quat> g_quats;
static int64_t q_new(float x, float y, float z, float w) { g_quats.push_back({x,y,z,w}); return int64_t(g_quats.size()); }
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
    static int64_t n_quat_add(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return q_new(x->x+y->x, x->y+y->y, x->z+y->z, x->w+y->w); }
    static int64_t n_quat_sub(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return q_new(x->x-y->x, x->y-y->y, x->z-y->z, x->w-y->w); }
    // Hamilton product (real quaternion multiplication, not component-wise -
    // this is what `*` means mathematically for a quat).
    static int64_t n_quat_mul(int64_t a, int64_t b) {
        auto* p=q_slot(a); auto* q=q_slot(b);
        float x = p->w*q->x + p->x*q->w + p->y*q->z - p->z*q->y;
        float y = p->w*q->y - p->x*q->z + p->y*q->w + p->z*q->x;
        float z = p->w*q->z + p->x*q->y - p->y*q->x + p->z*q->w;
        float w = p->w*q->w - p->x*q->x - p->y*q->y - p->z*q->z;
        return q_new(x, y, z, w);
    }
    static int64_t n_quat_eq(int64_t a, int64_t b) { auto* x=q_slot(a); auto* y=q_slot(b); return (x->x==y->x && x->y==y->y && x->z==y->z && x->w==y->w) ? 1 : 0; }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto add = [&](const char* n, void* fn, Type r, std::vector<Type> ps) {
        m[n] = NativeSig{n, fn, std::move(r), std::move(ps), 0};
    };
    add("quat_new", (void*)&n_quat_new, H("quat"), {I(Prim::F32),I(Prim::F32),I(Prim::F32),I(Prim::F32)});
    add("quat_x",   (void*)&n_quat_x,   I(Prim::F32), {I(Prim::I64)});
    add("quat_y",   (void*)&n_quat_y,   I(Prim::F32), {I(Prim::I64)});
    add("quat_z",   (void*)&n_quat_z,   I(Prim::F32), {I(Prim::I64)});
    add("quat_w",   (void*)&n_quat_w,   I(Prim::F32), {I(Prim::I64)});
    add("quat_set_x",(void*)&n_quat_set_x,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("quat_set_y",(void*)&n_quat_set_y,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("quat_set_z",(void*)&n_quat_set_z,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
    add("quat_set_w",(void*)&n_quat_set_w,I(Prim::Void),{I(Prim::I64),I(Prim::F32)});
}

void register_overloads(OpOverloadTable& overloads) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto reg_op = [&](const char* type_name, int op, void* fn, Type ret) {
        overloads.register_op(type_name, op, {fn, "", ret, {I(Prim::I64),I(Prim::I64)}});
    };
    // quat's `*` is the Hamilton product (n_quat_mul), not component-wise -
    // the mathematically meaningful "multiply" for a quaternion.
    reg_op("quat", int(BinExpr::Op::Add), (void*)&n_quat_add, H("quat"));
    reg_op("quat", int(BinExpr::Op::Sub), (void*)&n_quat_sub, H("quat"));
    reg_op("quat", int(BinExpr::Op::Mul), (void*)&n_quat_mul, H("quat"));
    reg_op("quat", int(BinExpr::Op::Eq),  (void*)&n_quat_eq,  I(Prim::Bool));
}

void reset() {
    g_quats.clear();
}

} // namespace ember::ext_quat
