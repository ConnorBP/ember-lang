// ext_graphics_stub.cpp - non-Windows graphics extension compatibility stub.
#include "ext_graphics.hpp"

#include "ast.hpp"
#include "binding_builder.hpp"
#include "ext_string.hpp"

namespace ember::ext_graphics {
namespace {
constexpr const char* kUnsupported =
    "graphics extension is unsupported on this platform (Win32/D3D11 required)";

int64_t fail_create(int64_t, int64_t, int64_t) { return 0; }
int64_t fail_one(int64_t) { return 0; }
int64_t fail_two(int64_t, int64_t) { return 0; }
int64_t fail_three(int64_t, int64_t, int64_t) { return 0; }
int64_t fail_clear(int64_t, float, float, float, float) { return 0; }
int64_t should_close(int64_t) { return 1; }
void no_op_one(int64_t) {}
int64_t last_error() { return ext_string::alloc(kUnsupported); }
} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b;
    const Type i64 = type_i64();
    const Type f32 = type_f32();
    const Type boolean = type_bool();
    const Type string = bind_handle("string");
    b.add("window_create", i64, {i64, i64, string}, (void*)&fail_create, PERM_FFI);
    b.add("window_show", boolean, {i64}, (void*)&fail_one, PERM_FFI);
    b.add("window_poll_events", boolean, {i64}, (void*)&fail_one, PERM_FFI);
    b.add("window_should_close", boolean, {i64}, (void*)&should_close, PERM_FFI);
    b.add("window_width", i64, {i64}, (void*)&fail_one, PERM_FFI);
    b.add("window_height", i64, {i64}, (void*)&fail_one, PERM_FFI);
    b.add("window_set_title", boolean, {i64, string}, (void*)&fail_two, PERM_FFI);
    b.add("window_destroy", type_void(), {i64}, (void*)&no_op_one, PERM_FFI);
    b.add("shader_create", i64, {i64, string, string}, (void*)&fail_three, PERM_FFI);
    b.add("shader_destroy", type_void(), {i64}, (void*)&no_op_one, PERM_FFI);
    b.add("shader_set_constants", boolean, {i64, i64}, (void*)&fail_two, PERM_FFI);
    b.add("shader_draw_fullscreen", boolean, {i64}, (void*)&fail_one, PERM_FFI);
    b.add("graphics_clear", boolean, {i64, f32, f32, f32, f32}, (void*)&fail_clear, PERM_FFI);
    b.add("graphics_present", boolean, {i64, i64}, (void*)&fail_two, PERM_FFI);
    b.add("graphics_last_error", string, {}, (void*)&last_error, PERM_FFI);
    NativeTable table = b.build();
    for (auto& item : table.natives) natives[item.first] = std::move(item.second);
}

void register_overloads(OpOverloadTable&) {}
void reset() {}

} // namespace ember::ext_graphics
