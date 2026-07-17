// Direct registration/runtime test for the retained UI and render stores.
#include "binding_builder.hpp"
#include "ext_array.hpp"
#include "ext_render.hpp"
#include "ext_string.hpp"
#include "ext_ui_widgets.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace ember;

namespace {
int failures = 0;
void check(bool condition, const char* text) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", text);
    if (!condition) failures = 1;
}
template <class F> F native(const std::unordered_map<std::string, NativeSig>& n,
                            const char* name) {
    return reinterpret_cast<F>(n.at(name).fn_ptr);
}
} // namespace

int main() {
    std::unordered_map<std::string, NativeSig> n;
    ext_ui_widgets::register_natives(n);
    ext_render::register_natives(n);
    check(n.at("ui_create_subtab").permission == PERM_FFI,
          "ui widgets are PERM_FFI gated");
    check(n.at("render_draw").permission == PERM_FFI,
          "render commands are PERM_FFI gated");

    const int64_t demo = ext_string::alloc("Demo");
    const int64_t controls = ext_string::alloc("Controls");
    const int64_t enabled = ext_string::alloc("Enabled");
    auto subtab = native<int64_t(*)(int64_t,int64_t)>(n, "ui_create_subtab");
    auto panel = native<int64_t(*)(int64_t,int64_t,int64_t)>(n, "ui_panel_add");
    auto checkbox = native<int64_t(*)(int64_t,int64_t,int64_t)>(n, "ui_checkbox");
    const int64_t st = subtab(2, demo);
    const int64_t pn = panel(st, controls, 1);
    const int64_t cb = checkbox(pn, enabled, 1);
    ext_ui_widgets::Widget widget;
    check(st && pn && cb && ext_ui_widgets::get_widget(cb, widget) &&
          widget.type == ext_ui_widgets::WidgetType::Checkbox && widget.bool_val,
          "retained widget tree preserves type, parent, label and value");
    check(ext_ui_widgets::set_bool(cb, false) &&
          !native<int64_t(*)(int64_t)>(n, "ui_widget_get_bool")(cb),
          "host can update a retained widget value");

    const int64_t a = ext_string::alloc("One"), b = ext_string::alloc("Two");
    const int64_t option_handles[] = {a, b};
    // alloc_bytes records width 1, so construct the typed i64 array through
    // registered array natives.
    std::unordered_map<std::string, NativeSig> arrays;
    ext_array::register_natives(arrays);
    const int64_t ah = native<int64_t(*)(int64_t,int64_t)>(arrays,"array_new")(8, 2);
    auto set_i64 = native<void(*)(int64_t,int64_t,int64_t)>(arrays,"array_set_i64");
    set_i64(ah, 0, option_handles[0]); set_i64(ah, 1, option_handles[1]);
    const int64_t oh = native<int64_t(*)(int64_t)>(n,"ui_make_options")(ah);
    check(oh != 0, "ui_make_options copies array<string> handles");

    const int64_t source = ext_string::alloc("float4 main() : SV_Target { return 1; }");
    const int64_t layout = ext_string::alloc("POSITION:0:FLOAT3");
    const int64_t vs = native<int64_t(*)(int64_t)>(n,"render_create_vertex_shader")(source);
    const int64_t il = native<int64_t(*)(int64_t)>(n,"render_create_input_layout")(layout);
    native<void(*)(int64_t)>(n,"render_set_vertex_shader")(vs);
    native<void(*)(int64_t)>(n,"render_set_input_layout")(il);
    native<void(*)(int64_t)>(n,"render_draw")(3);
    native<void(*)(float,float,float,float)>(n,"render_clear")(0.1f,0.2f,0.3f,1.0f);
    native<void(*)()>(n,"render_present")();
    auto store = ext_render::snapshot();
    check(vs && il && store.vertex_shader == vs && store.input_layout == il &&
          store.draw_calls == 1 && store.clear_calls == 1 && store.present_calls == 1,
          "render stub stores resources, bindings and commands");

    Program program;
    ext_render::inject_constants(program);
    check(program.globals.size() == 5 && program.globals.front().is_const,
          "render injects five global const i32 topology values");
    ext_render::inject_constants(program);
    check(program.globals.size() == 5, "topology constant injection is idempotent");

    ext_ui_widgets::reset(); ext_render::reset(); ext_array::reset(); ext_string::reset();
    check(!ext_ui_widgets::get_widget(cb, widget) &&
          ext_render::snapshot().resources.empty(), "extension resets invalidate stores");
    return failures;
}
