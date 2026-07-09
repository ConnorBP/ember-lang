// ext_array.cpp - ember extension: array<T> host-store type.
// Relocated from prism/src/prism/prism_script_host.cpp (array block) during
// the restructure Section 6 audit. The host keeps a thin prism::GetArrayBytes
// wrapper (in prism_script_host.cpp) that forwards to get_bytes(), so
// shader_api.cpp's custom draw calls and the host's read_bulk native are
// unchanged. The store, handle ABI, bounds-check convention, and get/set
// natives are unchanged.
#include "ext_array.hpp"
#include "ast.hpp"
#include <cstring>
#include <vector>

namespace ember::ext_array {

// --- array<T> host store (opaque i64 handle; host owns the byte vector + elem_size) ---
struct ArraySlot { std::vector<uint8_t> bytes; int64_t elem_size = 1; };
static std::vector<ArraySlot> g_arrays;
static int64_t arr_new(int64_t elem_size, int64_t count) {
    if (elem_size < 1) elem_size = 1;
    g_arrays.push_back(ArraySlot{std::vector<uint8_t>(size_t(elem_size * count), 0), elem_size});
    return int64_t(g_arrays.size());  // handle = index (1-based)
}
static ArraySlot* arr_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_arrays.size())) return nullptr;
    return &g_arrays[size_t(h - 1)];
}
extern "C" {
    static int64_t n_array_new(int64_t esz, int64_t n) { return arr_new(esz, n); }
    static int64_t n_array_length(int64_t h) { auto* s = arr_slot(h); return s ? int64_t(s->bytes.size() / s->elem_size) : 0; }
    static void n_array_resize(int64_t h, int64_t n) { auto* s = arr_slot(h); if (s) s->bytes.resize(size_t(n * s->elem_size), 0); }
    static void n_array_set_u8(int64_t h, int64_t i, int64_t v) { auto* s = arr_slot(h); if (s && i>=0 && i<int64_t(s->bytes.size())) s->bytes[size_t(i)] = uint8_t(v); }
    static int64_t n_array_get_u8(int64_t h, int64_t i) { auto* s = arr_slot(h); return (s && i>=0 && i<int64_t(s->bytes.size())) ? int64_t(s->bytes[size_t(i)]) : 0; }
    static void n_array_set_f32(int64_t h, int64_t i, float v) { auto* s = arr_slot(h); if (s && i>=0 && size_t(i)*4+4<=s->bytes.size()) std::memcpy(&s->bytes[size_t(i)*4], &v, 4); }
    static float n_array_get_f32(int64_t h, int64_t i) { auto* s = arr_slot(h); float v=0; if (s && i>=0 && size_t(i)*4+4<=s->bytes.size()) std::memcpy(&v, &s->bytes[size_t(i)*4], 4); return v; }
    static void n_array_set_i64(int64_t h, int64_t i, int64_t v) { auto* s = arr_slot(h); if (s && i>=0 && size_t(i)*8+8<=s->bytes.size()) std::memcpy(&s->bytes[size_t(i)*8], &v, 8); }
    static int64_t n_array_get_i64(int64_t h, int64_t i) { auto* s = arr_slot(h); int64_t v=0; if (s && i>=0 && size_t(i)*8+8<=s->bytes.size()) std::memcpy(&v, &s->bytes[size_t(i)*8], 8); return v; }
    static void n_array_push_u8(int64_t h, int64_t v) { auto* s = arr_slot(h); if (s && s->elem_size==1) s->bytes.push_back(uint8_t(v)); }
}

// Exposed so a host native that receives an array<u8> handle (process
// read-into-array, custom shader draw calls) can read the backing bytes
// without going through a registered accessor native. The host keeps its
// own thin wrapper (prism::GetArrayBytes) that forwards here.
//
// Re-implemented here (not forwarding to arr_slot) to keep the symbol
// self-contained; the bounds/invalid-handle semantics match the original.
bool get_bytes(int64_t handle, uint8_t** out_data, int64_t* out_len) {
    auto* s = arr_slot(handle);
    if (!s) return false;
    *out_data = s->bytes.data();
    *out_len = int64_t(s->bytes.size());
    return true;
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto add = [&](const char* n, void* fn, Type r, std::vector<Type> ps) {
        m[n] = NativeSig{n, fn, std::move(r), std::move(ps), 0};
    };
    add("array_new",   (void*)&n_array_new,   I(Prim::I64), {I(Prim::I64),I(Prim::I64)});
    add("array_length",(void*)&n_array_length,I(Prim::I64), {I(Prim::I64)});
    add("array_resize",(void*)&n_array_resize,I(Prim::Void), {I(Prim::I64),I(Prim::I64)});
    add("array_set_u8",(void*)&n_array_set_u8,I(Prim::Void), {I(Prim::I64),I(Prim::I64),I(Prim::I64)});
    add("array_get_u8",(void*)&n_array_get_u8,I(Prim::U8),  {I(Prim::I64),I(Prim::I64)});
    add("array_set_f32",(void*)&n_array_set_f32,I(Prim::Void), {I(Prim::I64),I(Prim::I64),I(Prim::F32)});
    add("array_get_f32",(void*)&n_array_get_f32,I(Prim::F32), {I(Prim::I64),I(Prim::I64)});
    add("array_set_i64",(void*)&n_array_set_i64,I(Prim::Void), {I(Prim::I64),I(Prim::I64),I(Prim::I64)});
    add("array_get_i64",(void*)&n_array_get_i64,I(Prim::I64), {I(Prim::I64),I(Prim::I64)});
    add("array_push_u8",(void*)&n_array_push_u8,I(Prim::Void), {I(Prim::I64),I(Prim::I64)});
}

void reset() {
    g_arrays.clear();
}

} // namespace ember::ext_array
