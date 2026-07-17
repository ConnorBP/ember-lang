// ext_ui_widgets.cpp - retained UI widget-tree data model, ported from Prism.
#include "ext_ui_widgets.hpp"

#include "ast.hpp"
#include "binding_builder.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace ember::ext_ui_widgets {
namespace {

constexpr std::size_t kMaxWidgets = 65536;
constexpr std::size_t kMaxOptions = 4096;
constexpr uint64_t kOptionHandleTag = uint64_t(1) << 62;
std::vector<Widget> g_widgets;
std::vector<std::vector<std::string>> g_option_lists;
std::mutex g_mutex;

Widget* widget(int64_t handle) {
    if (handle < 1 || handle > int64_t(g_widgets.size())) return nullptr;
    return &g_widgets[std::size_t(handle - 1)];
}

int64_t alloc_widget(Widget value) {
    if (g_widgets.size() >= kMaxWidgets) return 0;
    try {
        g_widgets.push_back(std::move(value));
        return int64_t(g_widgets.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}

std::string string_value(int64_t handle) {
    std::string out;
    ext_string::copy(handle, out);
    return out;
}

std::vector<std::string> resolve_string_array(int64_t array_handle) {
    std::vector<std::string> out;
    if (!array_handle) return out;
    std::vector<int64_t> handles(kMaxOptions);
    int64_t count = 0;
    if (!ext_array::copy_i64(array_handle, handles.data(),
                             int64_t(handles.size()), &count))
        return out;
    out.reserve(std::size_t(count));
    for (int64_t i = 0; i < count; ++i) out.push_back(string_value(handles[std::size_t(i)]));
    return out;
}

std::vector<std::string> resolve_options(int64_t handle) {
    const uint64_t encoded = uint64_t(handle);
    if ((encoded & kOptionHandleTag) != 0) {
        const uint64_t index = encoded & ~kOptionHandleTag;
        if (index >= 1 && index <= g_option_lists.size())
            return g_option_lists[std::size_t(index - 1)];
        return {};
    }
    // Compatibility with Prism: selection constructors also accept an
    // array<i64> of string handles directly. Tagged option handles avoid a
    // collision between the two independent one-based handle stores.
    return resolve_string_array(handle);
}

int64_t n_ui_create_subtab(int64_t tab_index, int64_t name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget w; w.type = WidgetType::Subtab; w.int_val = tab_index;
    w.label = string_value(name);
    return alloc_widget(std::move(w));
}

int64_t n_ui_panel_add(int64_t subtab, int64_t name, int64_t two_height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(subtab) || widget(subtab)->type != WidgetType::Subtab) return 0;
    Widget w; w.type = WidgetType::Panel; w.label = string_value(name);
    w.parent_handle = subtab; w.two_height = two_height != 0;
    return alloc_widget(std::move(w));
}

int64_t n_ui_checkbox(int64_t panel, int64_t label, int64_t default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::Checkbox; w.label = string_value(label);
    w.parent_handle = panel; w.bool_val = default_value != 0;
    return alloc_widget(std::move(w));
}

int64_t n_ui_keybind(int64_t panel, int64_t label, int64_t default_value, int64_t mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::Keybind; w.label = string_value(label);
    w.parent_handle = panel; w.int_val = default_value;
    w.keybind_mode = string_value(mode);
    return alloc_widget(std::move(w));
}

int64_t n_ui_slider_int(int64_t panel, int64_t label, int64_t unit,
                        int64_t default_value, int64_t minimum, int64_t maximum,
                        int64_t step) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::SliderInt; w.label = string_value(label);
    w.unit = string_value(unit); w.parent_handle = panel; w.int_val = default_value;
    w.min_val = double(minimum); w.max_val = double(maximum); w.step_val = double(step);
    return alloc_widget(std::move(w));
}

int64_t n_ui_slider_double(int64_t panel, int64_t label, int64_t unit,
                           double default_value, double minimum, double maximum,
                           double step) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::SliderDouble; w.label = string_value(label);
    w.unit = string_value(unit); w.parent_handle = panel; w.double_val = default_value;
    w.min_val = minimum; w.max_val = maximum; w.step_val = step;
    return alloc_widget(std::move(w));
}

int64_t n_ui_single_select(int64_t panel, int64_t label, int64_t options,
                           int64_t default_value, int64_t has_children) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::SingleSelect; w.label = string_value(label);
    w.parent_handle = panel; w.int_val = default_value;
    w.options = resolve_options(options); w.has_children = has_children != 0;
    return alloc_widget(std::move(w));
}

int64_t n_ui_multi_select(int64_t panel, int64_t label, int64_t options,
                          int64_t has_children) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::MultiSelect; w.label = string_value(label);
    w.parent_handle = panel; w.options = resolve_options(options);
    w.multi_selected.assign(w.options.size(), false); w.has_children = has_children != 0;
    return alloc_widget(std::move(w));
}

int64_t n_ui_range_int(int64_t panel, int64_t label, int64_t unit,
                       int64_t default_value, int64_t minimum, int64_t maximum,
                       int64_t step, int64_t step2) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::RangeInt; w.label = string_value(label);
    w.unit = string_value(unit); w.parent_handle = panel; w.int_val = default_value;
    w.min_val = double(minimum); w.max_val = double(maximum);
    w.step_val = double(step); w.step2_val = double(step2);
    w.range_low = std::clamp(double(default_value), w.min_val, w.max_val);
    w.range_high = w.max_val;
    return alloc_widget(std::move(w));
}

int64_t n_ui_range_double(int64_t panel, int64_t label, int64_t unit,
                          double default_value, double minimum, double maximum,
                          double step, double step2) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::RangeDouble; w.label = string_value(label);
    w.unit = string_value(unit); w.parent_handle = panel; w.double_val = default_value;
    w.min_val = minimum; w.max_val = maximum; w.step_val = step; w.step2_val = step2;
    w.range_low = std::clamp(default_value, minimum, maximum); w.range_high = maximum;
    return alloc_widget(std::move(w));
}

int64_t n_ui_color_picker(int64_t panel, int64_t label, int64_t r, int64_t g,
                          int64_t b, int64_t a) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::ColorPicker; w.label = string_value(label);
    w.parent_handle = panel;
    w.r = uint8_t(std::clamp<int64_t>(r, 0, 255));
    w.g = uint8_t(std::clamp<int64_t>(g, 0, 255));
    w.b = uint8_t(std::clamp<int64_t>(b, 0, 255));
    w.a = uint8_t(std::clamp<int64_t>(a, 0, 255));
    return alloc_widget(std::move(w));
}

int64_t n_ui_input(int64_t panel, int64_t label, int64_t default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!widget(panel) || widget(panel)->type != WidgetType::Panel) return 0;
    Widget w; w.type = WidgetType::Input; w.label = string_value(label);
    w.parent_handle = panel; w.str_val = string_value(default_value);
    return alloc_widget(std::move(w));
}

int64_t n_ui_widget_get_bool(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); return w && w->bool_val ? 1 : 0;
}
int64_t n_ui_widget_get_int(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); return w ? w->int_val : 0;
}
double n_ui_widget_get_float(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); return w ? w->double_val : 0.0;
}
int64_t n_ui_widget_is_pressed(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); return w && w->pressed ? 1 : 0;
}

void n_ui_widget_add_child(int64_t parent, int64_t slot, int64_t child) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* p = widget(parent); Widget* c = widget(child);
    if (!p || !c || parent == child) return;
    p->children.push_back({slot, child});
    c->parent_handle = parent;
}

int64_t n_ui_make_options(int64_t items) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_option_lists.size() >= kMaxWidgets) return 0;
    std::vector<std::string> values = resolve_string_array(items);
    try {
        g_option_lists.push_back(std::move(values));
        return int64_t(kOptionHandleTag | uint64_t(g_option_lists.size()));
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder b;
    const Type i64 = type_i64(), f64 = type_f64(), boolean = type_bool();
    const Type string = bind_handle("string");
    const uint32_t p = PERM_FFI;
    b.add("ui_create_subtab", i64, {i64,string}, (void*)&n_ui_create_subtab, p);
    b.add("ui_panel_add", i64, {i64,string,boolean}, (void*)&n_ui_panel_add, p);
    b.add("ui_checkbox", i64, {i64,string,boolean}, (void*)&n_ui_checkbox, p);
    b.add("ui_keybind", i64, {i64,string,i64,string}, (void*)&n_ui_keybind, p);
    b.add("ui_slider_int", i64, {i64,string,string,i64,i64,i64,i64}, (void*)&n_ui_slider_int, p);
    b.add("ui_slider_double", i64, {i64,string,string,f64,f64,f64,f64}, (void*)&n_ui_slider_double, p);
    b.add("ui_single_select", i64, {i64,string,i64,i64,boolean}, (void*)&n_ui_single_select, p);
    b.add("ui_multi_select", i64, {i64,string,i64,boolean}, (void*)&n_ui_multi_select, p);
    b.add("ui_range_int", i64, {i64,string,string,i64,i64,i64,i64,i64}, (void*)&n_ui_range_int, p);
    b.add("ui_range_double", i64, {i64,string,string,f64,f64,f64,f64,f64}, (void*)&n_ui_range_double, p);
    b.add("ui_color_picker", i64, {i64,string,i64,i64,i64,i64}, (void*)&n_ui_color_picker, p);
    b.add("ui_input", i64, {i64,string,string}, (void*)&n_ui_input, p);
    b.add("ui_widget_get_bool", boolean, {i64}, (void*)&n_ui_widget_get_bool, p);
    b.add("ui_widget_get_int", i64, {i64}, (void*)&n_ui_widget_get_int, p);
    b.add("ui_widget_get_float", f64, {i64}, (void*)&n_ui_widget_get_float, p);
    b.add("ui_widget_is_pressed", boolean, {i64}, (void*)&n_ui_widget_is_pressed, p);
    b.add("ui_widget_add_child", type_void(), {i64,i64,i64}, (void*)&n_ui_widget_add_child, p);
    b.add("ui_make_options", i64, {i64}, (void*)&n_ui_make_options, p);
    NativeTable table = b.build();
    for (auto& item : table.natives) natives[item.first] = std::move(item.second);
}

void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_widgets.clear(); g_option_lists.clear();
}

bool get_widget(int64_t handle, Widget& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); if (!w) return false; out = *w; return true;
}
bool set_bool(int64_t handle, bool value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); if (!w) return false; w->bool_val = value; return true;
}
bool set_int(int64_t handle, int64_t value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); if (!w) return false; w->int_val = value; return true;
}
bool set_float(int64_t handle, double value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); if (!w) return false; w->double_val = value; return true;
}
bool set_pressed(int64_t handle, bool value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Widget* w = widget(handle); if (!w) return false; w->pressed = value; return true;
}
std::vector<int64_t> roots() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<int64_t> result;
    for (std::size_t i = 0; i < g_widgets.size(); ++i)
        if (g_widgets[i].parent_handle == 0) result.push_back(int64_t(i + 1));
    return result;
}

} // namespace ember::ext_ui_widgets
