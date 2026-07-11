// ext_mat.cpp - ember extension: mat4 host-store type + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (mat4 block)
// during the restructure Section 6 audit. Math + handle ABI + overload set
// unchanged; only namespace + registration entry points changed.
#include "ext_mat.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_mat {

// --- mat4 host store (opaque i64 handle; host owns 16 floats, row-major) ---
struct Mat4 { float m[16]; };
static std::vector<Mat4> g_mat4s;
// Serializes all g_mat4s store operations (push_back in m4_new_zero /
// m4_new_identity, m4_slot lookups) so concurrent context_t's calling mat4
// natives cannot race on vector reallocation (Sec-5). Mirrors the g_store_mutex
// pattern in ext_sync.cpp and g_mutex in ext_lifecycle.cpp.
static std::mutex g_store_mutex;
static int64_t m4_new_zero() noexcept {
    try {
        Mat4 z{}; g_mat4s.push_back(z);
        return int64_t(g_mat4s.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static int64_t m4_new_identity() noexcept {
    try {
        Mat4 id{};
        id.m[0]=1; id.m[5]=1; id.m[10]=1; id.m[15]=1;
        g_mat4s.push_back(id);
        return int64_t(g_mat4s.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static Mat4* m4_slot(int64_t h) { if (h<1 || h>int64_t(g_mat4s.size())) return nullptr; return &g_mat4s[size_t(h-1)]; }
extern "C" {
    static int64_t n_mat4_new() { std::lock_guard<std::mutex> lock(g_store_mutex); return m4_new_zero(); }
    static int64_t n_mat4_identity() { std::lock_guard<std::mutex> lock(g_store_mutex); return m4_new_identity(); }
    static float n_mat4_get(int64_t h, int64_t row, int64_t col) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* m=m4_slot(h);
        if (!m || row<0 || row>3 || col<0 || col>3) return 0;
        return m->m[size_t(row*4+col)];
    }
    static void n_mat4_set(int64_t h, int64_t row, int64_t col, float v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* m=m4_slot(h);
        if (!m || row<0 || row>3 || col<0 || col>3) return;
        m->m[size_t(row*4+col)] = v;
    }
    // standard row-major 4x4 matrix product (not component-wise)
    static int64_t n_mat4_mul(int64_t a, int64_t b) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x=m4_slot(a); auto* y=m4_slot(b);
        if (!x || !y) return 0;
        int64_t h = m4_new_zero();
        auto* out = m4_slot(h);
        if (!out) return 0;
        for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
            float sum=0;
            for (int k=0;k<4;++k) sum += x->m[size_t(r*4+k)] * y->m[size_t(k*4+c)];
            out->m[size_t(r*4+c)] = sum;
        }
        return h;
    }
    static int64_t n_mat4_eq(int64_t a, int64_t b) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x=m4_slot(a); auto* y=m4_slot(b);
        if (!x || !y) return 0;
        for (int i=0;i<16;++i) if (x->m[size_t(i)] != y->m[size_t(i)]) return 0;
        return 1;
    }
}

// Registered surface is byte-identical to the old I/H/add lambda form
// (ext_registration_test asserts mat4_new -> struct "mat4", 0 params;
//  mat4_get -> f32 3 params; mat4_set -> void 4 params).
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("mat4_new",      bind_handle("mat4"), {},                   (void*)&n_mat4_new);
    b.add("mat4_identity", bind_handle("mat4"), {},                   (void*)&n_mat4_identity);
    b.add("mat4_get", type_f32(), {bind_handle("mat4"),type_i64(),type_i64()}, (void*)&n_mat4_get);
    b.add("mat4_set", type_void(), {bind_handle("mat4"),type_i64(),type_i64(),type_f32()}, (void*)&n_mat4_set);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Overload (type,op) entries preserved exactly. mat4's `*` is the standard
// 4x4 matrix product; addition isn't commonly meaningful here, so (unlike
// vec/quat) it's omitted.
void register_overloads(OpOverloadTable& overloads) {
    BindingBuilder b;
    b.add_overload("mat4", int(BinExpr::Op::Mul), bind_handle("mat4"), (void*)&n_mat4_mul);
    b.add_overload("mat4", int(BinExpr::Op::Eq),  type_bool(),          (void*)&n_mat4_eq);
    NativeTable t = b.build();
    for (auto& kv : t.overloads.entries) overloads.entries[kv.first] = std::move(kv.second);
}

void reset() {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    g_mat4s.clear();
}

} // namespace ember::ext_mat
