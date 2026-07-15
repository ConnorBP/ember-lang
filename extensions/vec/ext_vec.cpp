// ext_vec.cpp - ember extension: vec2/vec3/vec4 host-store types + operator overloads.
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (the
// vec2/vec3/vec4 host-store blocks) during the restructure Section 6 audit. The
// only change is namespace + the registration entry points; the math,
// the handle ABI, and the overload set are unchanged so existing scripts
// (e.g. prism/examples/scripts/vector_math_demo.ember) behave identically.
#include "ext_vec.hpp"
#include "ast.hpp"       // ember public: Type, make_prim, make_slice, Prim
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include "../gc/ext_gc.hpp"     // c1 GC trace-callback facade (leaf registration)
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_vec {

// --- vec3 host store (opaque i64 handle; host owns {float x,y,z}) ---
struct Vec3 { float x, y, z; };
static std::vector<Vec3> g_vec3s;
// Serializes all vec/vec2/vec3/vec4 store operations (push_back in v*_new,
// v*_slot lookups) so concurrent context_t's calling vec natives cannot race
// on vector reallocation (Sec-5). Mirrors the g_store_mutex pattern in
// ext_sync.cpp and g_mutex in ext_lifecycle.cpp. One mutex covers all three
// stores; they are all cold allocation/creation paths.
static std::mutex g_store_mutex;

// ===========================================================================
// c1 GC trace-callback integration (LEAF).
//
// A vec2/3/4 holds floats (POD {float x,y[,z[,w]]}) -- it contains NO GC-
// managed children. We therefore register a no-op LEAF trace callback (it
// reports nothing) rather than scanning the float bytes as pointers: a float's
// bit pattern could otherwise look like a live heap address and create a FALSE
// ROOT, keeping an unrelated object alive. Registering the leaf callback marks
// the extension as a known GC-aware leaf so the no-child layout is explicit +
// guarded against a future regression that might (incorrectly) reinterpret the
// float bytes. The callback walks nothing + reports nothing, so it can never
// root or false-root any object.
//
// The token is THREAD-LOCAL (the GC runtime is thread-local). The callback is
// registered lazily on the first vec creation, and only when the GC runtime is
// already initialized on this thread (gc_runtime_initialized), so a thread that
// never uses the GC stays in pure non-GC mode. gc_reset() invalidates tokens;
// the next creation re-registers. reset() unregisters. No write barrier is
// emitted (no pointer stores). See ext_array.cpp for the synchronization /
// deadlock rationale (identical: the callback acquires g_store_mutex during
// collect + reports nothing; mutations never collect).
static thread_local ember::gc::GcTraceToken g_trace_token = 0;

// The leaf trace callback: reports nothing (vec holds floats, not GC pointers).
// No mutex is needed: the body is empty, so the callback never touches the
// process-wide store (unlike array/map, whose callbacks walk g_arrays/g_maps
// under g_store_mutex). Intentionally reports nothing so float bytes are never
// reinterpreted as pointers (no false roots).
static void vec_leaf_trace_cb(void* /*user_data*/, ember::gc::GcTraceVisitor& /*visitor*/) {
    // Leaf: no GC-managed children. Intentionally reports nothing so float
    // bytes are never reinterpreted as pointers (no false roots).
}

// Ensure exactly one leaf trace callback is registered on this thread's GC
// runtime, re-registering if the previous token was invalidated by gc_reset().
// Called UNDER g_store_mutex by the v*_new creation paths. No-op when the GC
// runtime is not initialized on this thread (pure non-GC mode).
static void ensure_gc_leaf_cb() {
    if (!ember::ext_gc::gc_runtime_initialized()) return;
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);  // no-op if stale
    }
    g_trace_token = ember::ext_gc::gc_register_trace_callback(nullptr, &vec_leaf_trace_cb);
}

// Unregister this thread's leaf trace callback (teardown). Called UNDER
// g_store_mutex by reset().
static void drop_gc_leaf_cb() {
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);
        g_trace_token = 0;
    }
}
static int64_t v3_new(float x, float y, float z) noexcept {
    try {
        g_vec3s.push_back({x,y,z});
        ensure_gc_leaf_cb();  // c1: register leaf trace cb (idempotent) on this thread
        return int64_t(g_vec3s.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static Vec3* v3_slot(int64_t h) { if (h<1 || h>int64_t(g_vec3s.size())) return nullptr; return &g_vec3s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec3_new(float x, float y, float z) { std::lock_guard<std::mutex> lock(g_store_mutex); return v3_new(x,y,z); }
    static float n_vec3_x(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v3_slot(h); return v?v->x:0; }
    static float n_vec3_y(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v3_slot(h); return v?v->y:0; }
    static float n_vec3_z(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v3_slot(h); return v?v->z:0; }
    static void n_vec3_set_x(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v3_slot(h); if(s) s->x=v; }
    static void n_vec3_set_y(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v3_slot(h); if(s) s->y=v; }
    static void n_vec3_set_z(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v3_slot(h); if(s) s->z=v; }
    static int64_t n_vec3_add(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x+y->x, x->y+y->y, x->z+y->z):0; }
    static int64_t n_vec3_sub(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x-y->x, x->y-y->y, x->z-y->z):0; }
    static int64_t n_vec3_mul(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(x->x*y->x, x->y*y->y, x->z*y->z):0; }
    static int64_t n_vec3_eq(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y&&x->x==y->x && x->y==y->y && x->z==y->z) ? 1 : 0; }
}

// --- vec2 host store (opaque i64 handle; host owns {float x,y}) ---
struct Vec2 { float x, y; };
static std::vector<Vec2> g_vec2s;
static int64_t v2_new(float x, float y) noexcept {
    try {
        g_vec2s.push_back({x,y});
        ensure_gc_leaf_cb();  // c1: register leaf trace cb (idempotent) on this thread
        return int64_t(g_vec2s.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static Vec2* v2_slot(int64_t h) { if (h<1 || h>int64_t(g_vec2s.size())) return nullptr; return &g_vec2s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec2_new(float x, float y) { std::lock_guard<std::mutex> lock(g_store_mutex); return v2_new(x,y); }
    static float n_vec2_x(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v2_slot(h); return v?v->x:0; }
    static float n_vec2_y(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v2_slot(h); return v?v->y:0; }
    static void n_vec2_set_x(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v2_slot(h); if(s) s->x=v; }
    static void n_vec2_set_y(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v2_slot(h); if(s) s->y=v; }
    static int64_t n_vec2_add(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x+y->x, x->y+y->y):0; }
    static int64_t n_vec2_sub(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x-y->x, x->y-y->y):0; }
    static int64_t n_vec2_mul(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y)?v2_new(x->x*y->x, x->y*y->y):0; }
    static int64_t n_vec2_eq(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v2_slot(a); auto* y=v2_slot(b); return (x&&y&&x->x==y->x && x->y==y->y) ? 1 : 0; }
}

// --- vec4 host store (opaque i64 handle; host owns {float x,y,z,w}) ---
struct Vec4 { float x, y, z, w; };
static std::vector<Vec4> g_vec4s;
static int64_t v4_new(float x, float y, float z, float w) noexcept {
    try {
        g_vec4s.push_back({x,y,z,w});
        ensure_gc_leaf_cb();  // c1: register leaf trace cb (idempotent) on this thread
        return int64_t(g_vec4s.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static Vec4* v4_slot(int64_t h) { if (h<1 || h>int64_t(g_vec4s.size())) return nullptr; return &g_vec4s[size_t(h-1)]; }
extern "C" {
    static int64_t n_vec4_new(float x, float y, float z, float w) { std::lock_guard<std::mutex> lock(g_store_mutex); return v4_new(x,y,z,w); }
    static float n_vec4_x(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v4_slot(h); return v?v->x:0; }
    static float n_vec4_y(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v4_slot(h); return v?v->y:0; }
    static float n_vec4_z(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v4_slot(h); return v?v->z:0; }
    static float n_vec4_w(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* v=v4_slot(h); return v?v->w:0; }
    static void n_vec4_set_x(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v4_slot(h); if(s) s->x=v; }
    static void n_vec4_set_y(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v4_slot(h); if(s) s->y=v; }
    static void n_vec4_set_z(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v4_slot(h); if(s) s->z=v; }
    static void n_vec4_set_w(int64_t h, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s=v4_slot(h); if(s) s->w=v; }
    static int64_t n_vec4_add(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x+y->x, x->y+y->y, x->z+y->z, x->w+y->w):0; }
    static int64_t n_vec4_sub(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x-y->x, x->y-y->y, x->z-y->z, x->w-y->w):0; }
    static int64_t n_vec4_mul(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y)?v4_new(x->x*y->x, x->y*y->y, x->z*y->z, x->w*y->w):0; }
    static int64_t n_vec4_eq(int64_t a, int64_t b) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* x=v4_slot(a); auto* y=v4_slot(b); return (x&&y&&x->x==y->x && x->y==y->y && x->z==y->z && x->w==y->w) ? 1 : 0; }
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
    std::lock_guard<std::mutex> lock(g_store_mutex);
    g_vec2s.clear(); g_vec3s.clear(); g_vec4s.clear();
    // c1: drop this thread's leaf trace callback so it does not outlive the store.
    drop_gc_leaf_cb();
}

} // namespace ember::ext_vec
