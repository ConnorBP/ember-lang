// ext_ui_widgets.hpp - retained UI widget-tree data-model extension.
#pragma once

#include "sema.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember::ext_ui_widgets {

enum class WidgetType : uint8_t {
    Subtab, Panel, Checkbox, Keybind, SliderInt, SliderDouble,
    SingleSelect, MultiSelect, RangeInt, RangeDouble, ColorPicker, Input
};

struct Widget {
    WidgetType type = WidgetType::Panel;
    std::string label;
    std::string unit;
    bool bool_val = false;
    int64_t int_val = 0;
    double double_val = 0.0;
    std::string str_val;
    double min_val = 0.0;
    double max_val = 1.0;
    double step_val = 1.0;
    double step2_val = 1.0;
    double range_low = 0.0;
    double range_high = 1.0;
    uint8_t r = 255, g = 255, b = 255, a = 255;
    std::vector<std::string> options;
    std::vector<bool> multi_selected;
    struct ChildEntry { int64_t slot = 0; int64_t child_handle = 0; };
    std::vector<ChildEntry> children;
    int64_t parent_handle = 0;
    bool two_height = false;
    bool has_children = false;
    std::string keybind_mode;
    bool pressed = false;
};

// Register the retained widget construction/query API. Every native requires
// PERM_FFI. The extension stores only a model; rendering belongs to the host.
void register_natives(std::unordered_map<std::string, NativeSig>& natives);

// Clear all widgets/options and invalidate every previously returned handle.
// ui_make_options handles are tagged in the high range so they cannot collide
// with the independent one-based ext_array handle store.
void reset();

// Host inspection/mutation hooks. Returned snapshots do not borrow extension
// storage, so a host may inspect them without retaining pointers across reset.
bool get_widget(int64_t handle, Widget& out);
bool set_bool(int64_t handle, bool value);
bool set_int(int64_t handle, int64_t value);
bool set_float(int64_t handle, double value);
bool set_pressed(int64_t handle, bool value);
std::vector<int64_t> roots();

} // namespace ember::ext_ui_widgets
