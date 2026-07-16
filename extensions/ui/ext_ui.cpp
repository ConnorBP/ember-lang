// ext_ui.cpp - immediate-mode controls and draw-list canvas bindings.
#include "ext_ui.hpp"

#include "ast.hpp"
#include "binding_builder.hpp"
#include "ext_string.hpp"
#include "imgui.h"
#include "AngleKnob.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ember::ext_ui {
namespace {

struct CanvasState {
    bool active = false;
    ImVec2 origin {};
    ImVec2 size {};
};
thread_local CanvasState g_canvas;

bool ready() { return ImGui::GetCurrentContext() != nullptr; }
std::string text(int64_t handle) {
    std::string value;
    ext_string::copy(handle, value);
    return value;
}
ImU32 color(float r, float g, float b, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f), std::clamp(a, 0.0f, 1.0f)));
}

int64_t n_ui_begin(int64_t title) {
    if (!ready()) return 0;
    const std::string label = text(title);
    return ImGui::Begin(label.empty() ? "Ember" : label.c_str()) ? 1 : 0;
}
void n_ui_end() { if (ready()) ImGui::End(); }
float n_ui_knob(int64_t label, float value, float minimum, float maximum) {
    if (!ready() || !(maximum > minimum)) return value;
    const std::string name = text(label);
    ImGui::SimpleKnob(name.c_str(), &value, minimum, maximum);
    return value;
}
float n_ui_slider_float(int64_t label, float value, float minimum, float maximum) {
    if (!ready()) return value;
    const std::string name = text(label);
    ImGui::SliderFloat(name.c_str(), &value, minimum, maximum);
    return value;
}
int64_t n_ui_slider_int(int64_t label, int64_t value, int64_t minimum, int64_t maximum) {
    if (!ready()) return value;
    const std::string name = text(label);
    ImGui::SliderScalar(name.c_str(), ImGuiDataType_S64, &value, &minimum, &maximum);
    return value;
}
int64_t n_ui_checkbox(int64_t label, int64_t value) {
    if (!ready()) return value != 0;
    const std::string name = text(label);
    bool checked = value != 0;
    ImGui::Checkbox(name.c_str(), &checked);
    return checked ? 1 : 0;
}
int64_t n_ui_combo(int64_t label, int64_t current, int64_t itemsHandle) {
    if (!ready()) return current;
    const std::string name = text(label);
    const std::string csv = text(itemsHandle);
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(',', start);
        values.emplace_back(csv.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (values.empty()) return current;
    int selected = static_cast<int>(std::clamp<int64_t>(current, 0, int64_t(values.size() - 1)));
    std::vector<const char*> pointers;
    pointers.reserve(values.size());
    for (const auto& item : values) pointers.push_back(item.c_str());
    ImGui::Combo(name.c_str(), &selected, pointers.data(), static_cast<int>(pointers.size()));
    return selected;
}
int64_t n_ui_button(int64_t label) {
    if (!ready()) return 0;
    const std::string name = text(label);
    return ImGui::Button(name.c_str()) ? 1 : 0;
}
void n_ui_text(int64_t value) {
    if (ready()) { const std::string s = text(value); ImGui::TextUnformatted(s.c_str()); }
}
void n_ui_text_colored(float r, float g, float b, int64_t value) {
    if (ready()) { const std::string s = text(value); ImGui::TextColored(ImVec4(r, g, b, 1.0f), "%s", s.c_str()); }
}
void n_ui_separator() { if (ready()) ImGui::Separator(); }
void n_ui_same_line() { if (ready()) ImGui::SameLine(); }
void n_ui_push_item_width(float width) { if (ready()) ImGui::PushItemWidth(width); }
void n_ui_pop_item_width() { if (ready()) ImGui::PopItemWidth(); }
void n_ui_begin_group() { if (ready()) ImGui::BeginGroup(); }
void n_ui_end_group() { if (ready()) ImGui::EndGroup(); }
float n_ui_get_frame_time() { return ready() ? ImGui::GetIO().DeltaTime : 0.0f; }

void n_ui_canvas_begin(float width, float height) {
    if (!ready()) return;
    g_canvas.active = true;
    g_canvas.origin = ImGui::GetCursorScreenPos();
    g_canvas.size = ImVec2(std::max(width, 1.0f), std::max(height, 1.0f));
    ImGui::GetWindowDrawList()->PushClipRect(g_canvas.origin,
        ImVec2(g_canvas.origin.x + g_canvas.size.x, g_canvas.origin.y + g_canvas.size.y), true);
}
void n_ui_canvas_line(float x1, float y1, float x2, float y2,
                      float r, float g, float b, float a, float thickness) {
    if (!ready() || !g_canvas.active) return;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(g_canvas.origin.x + x1, g_canvas.origin.y + y1),
        ImVec2(g_canvas.origin.x + x2, g_canvas.origin.y + y2),
        color(r, g, b, a), std::max(thickness, 0.5f));
}
void n_ui_canvas_rect(float x, float y, float w, float h,
                      float r, float g, float b, float a, int64_t fill) {
    if (!ready() || !g_canvas.active) return;
    const ImVec2 lo(g_canvas.origin.x + x, g_canvas.origin.y + y);
    const ImVec2 hi(lo.x + w, lo.y + h);
    if (fill) ImGui::GetWindowDrawList()->AddRectFilled(lo, hi, color(r, g, b, a));
    else ImGui::GetWindowDrawList()->AddRect(lo, hi, color(r, g, b, a));
}
void n_ui_canvas_text(float x, float y, int64_t value) {
    if (!ready() || !g_canvas.active) return;
    const std::string s = text(value);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(g_canvas.origin.x + x, g_canvas.origin.y + y),
        ImGui::GetColorU32(ImGuiCol_Text), s.c_str());
}
void n_ui_canvas_end() {
    if (!ready() || !g_canvas.active) return;
    ImGui::GetWindowDrawList()->PopClipRect();
    ImGui::Dummy(g_canvas.size);
    g_canvas.active = false;
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b;
    const Type str = bind_handle("string");
    const Type i64 = type_i64();
    const Type f32 = type_f32();
    const Type boolean = type_bool();
    const uint32_t p = PERM_FFI;
    b.add("ui_begin", boolean, {str}, (void*)&n_ui_begin, p);
    b.add("ui_end", type_void(), {}, (void*)&n_ui_end, p);
    b.add("ui_knob", f32, {str,f32,f32,f32}, (void*)&n_ui_knob, p);
    b.add("ui_slider_float", f32, {str,f32,f32,f32}, (void*)&n_ui_slider_float, p);
    b.add("ui_slider_int", i64, {str,i64,i64,i64}, (void*)&n_ui_slider_int, p);
    b.add("ui_checkbox", boolean, {str,boolean}, (void*)&n_ui_checkbox, p);
    b.add("ui_combo", i64, {str,i64,str}, (void*)&n_ui_combo, p);
    b.add("ui_button", boolean, {str}, (void*)&n_ui_button, p);
    b.add("ui_text", type_void(), {str}, (void*)&n_ui_text, p);
    b.add("ui_text_colored", type_void(), {f32,f32,f32,str}, (void*)&n_ui_text_colored, p);
    b.add("ui_separator", type_void(), {}, (void*)&n_ui_separator, p);
    b.add("ui_same_line", type_void(), {}, (void*)&n_ui_same_line, p);
    b.add("ui_push_item_width", type_void(), {f32}, (void*)&n_ui_push_item_width, p);
    b.add("ui_pop_item_width", type_void(), {}, (void*)&n_ui_pop_item_width, p);
    b.add("ui_begin_group", type_void(), {}, (void*)&n_ui_begin_group, p);
    b.add("ui_end_group", type_void(), {}, (void*)&n_ui_end_group, p);
    b.add("ui_get_frame_time", f32, {}, (void*)&n_ui_get_frame_time, p);
    b.add("ui_canvas_begin", type_void(), {f32,f32}, (void*)&n_ui_canvas_begin, p);
    b.add("ui_canvas_line", type_void(), {f32,f32,f32,f32,f32,f32,f32,f32,f32}, (void*)&n_ui_canvas_line, p);
    b.add("ui_canvas_rect", type_void(), {f32,f32,f32,f32,f32,f32,f32,f32,boolean}, (void*)&n_ui_canvas_rect, p);
    b.add("ui_canvas_text", type_void(), {f32,f32,str}, (void*)&n_ui_canvas_text, p);
    b.add("ui_canvas_end", type_void(), {}, (void*)&n_ui_canvas_end, p);
    NativeTable table = b.build();
    for (auto& item : table.natives) natives[item.first] = std::move(item.second);
}

} // namespace ember::ext_ui
