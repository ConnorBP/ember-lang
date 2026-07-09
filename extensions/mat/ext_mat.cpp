// ext_mat.cpp - ember extension: mat4 host-store type + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (mat4 block)
// during the restructure Section 6 audit. Math + handle ABI + overload set
// unchanged; only namespace + registration entry points changed.
#include "ext_mat.hpp"
#include "ast.hpp"
#include <vector>

namespace ember::ext_mat {

// --- mat4 host store (opaque i64 handle; host owns 16 floats, row-major) ---
struct Mat4 { float m[16]; };
static std::vector<Mat4> g_mat4s;
static int64_t m4_new_zero() { Mat4 z{}; g_mat4s.push_back(z); return int64_t(g_mat4s.size()); }
static int64_t m4_new_identity() {
    Mat4 id{};
    id.m[0]=1; id.m[5]=1; id.m[10]=1; id.m[15]=1;
    g_mat4s.push_back(id);
    return int64_t(g_mat4s.size());
}
static Mat4* m4_slot(int64_t h) { if (h<1 || h>int64_t(g_mat4s.size())) return nullptr; return &g_mat4s[size_t(h-1)]; }
extern "C" {
    static int64_t n_mat4_new() { return m4_new_zero(); }
    static int64_t n_mat4_identity() { return m4_new_identity(); }
    static float n_mat4_get(int64_t h, int64_t row, int64_t col) {
        auto* m=m4_slot(h);
        if (!m || row<0 || row>3 || col<0 || col>3) return 0;
        return m->m[size_t(row*4+col)];
    }
    static void n_mat4_set(int64_t h, int64_t row, int64_t col, float v) {
        auto* m=m4_slot(h);
        if (!m || row<0 || row>3 || col<0 || col>3) return;
        m->m[size_t(row*4+col)] = v;
    }
    // standard row-major 4x4 matrix product (not component-wise)
    static int64_t n_mat4_mul(int64_t a, int64_t b) {
        auto* x=m4_slot(a); auto* y=m4_slot(b);
        int64_t h = m4_new_zero();
        auto* out = m4_slot(h);
        for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
            float sum=0;
            for (int k=0;k<4;++k) sum += x->m[size_t(r*4+k)] * y->m[size_t(k*4+c)];
            out->m[size_t(r*4+c)] = sum;
        }
        return h;
    }
    static int64_t n_mat4_eq(int64_t a, int64_t b) {
        auto* x=m4_slot(a); auto* y=m4_slot(b);
        for (int i=0;i<16;++i) if (x->m[size_t(i)] != y->m[size_t(i)]) return 0;
        return 1;
    }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto add = [&](const char* n, void* fn, Type r, std::vector<Type> ps) {
        m[n] = NativeSig{n, fn, std::move(r), std::move(ps), 0};
    };
    add("mat4_new",      (void*)&n_mat4_new,      H("mat4"), {});
    add("mat4_identity", (void*)&n_mat4_identity, H("mat4"), {});
    add("mat4_get", (void*)&n_mat4_get, I(Prim::F32), {I(Prim::I64),I(Prim::I64),I(Prim::I64)});
    add("mat4_set", (void*)&n_mat4_set, I(Prim::Void),{I(Prim::I64),I(Prim::I64),I(Prim::I64),I(Prim::F32)});
}

void register_overloads(OpOverloadTable& overloads) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto H = [](const char* name){ Type t; t.prim = Prim::I64; t.struct_name = name; return t; };
    auto reg_op = [&](const char* type_name, int op, void* fn, Type ret) {
        overloads.register_op(type_name, op, {fn, "", ret, {I(Prim::I64),I(Prim::I64)}});
    };
    // mat4's `*` is the standard 4x4 matrix product; addition isn't a
    // commonly meaningful op here, so (unlike vec/quat) it's omitted.
    reg_op("mat4", int(BinExpr::Op::Mul), (void*)&n_mat4_mul, H("mat4"));
    reg_op("mat4", int(BinExpr::Op::Eq),  (void*)&n_mat4_eq,  I(Prim::Bool));
}

void reset() {
    g_mat4s.clear();
}

} // namespace ember::ext_mat
