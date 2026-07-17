# Graphics, UI Widgets, and Rendering

Ember ships three complementary graphics/UI layers. They are extensions, not
language syntax, and all script calls described here require `PERM_FFI` (use
`ember run ... --ffi` in the stock CLI).

- `graphics` owns real Win32/D3D11 windows and full-screen shader programs.
- `ui` submits immediate-mode Dear ImGui controls into a host-owned frame.
- `ui_widgets` builds a retained widget tree for a host to render later.
- `render` records generic shader resources, bindings, draw calls, clears, and
  presents in a stub store. A host can attach a real backend.
- `visualize` derives waveforms, FFT spectra, RMS, and peak from host-published
  audio samples.

The full ownership, limits, and VST3 integration contracts are in
[`GRAPHICS_AND_UI.md`](../../spec/GRAPHICS_AND_UI.md).

## Retained widget trees (`ui_widgets`)

The retained API returns opaque `i64` handles. It performs no rendering. A host
uses the C++ inspection API in `ext_ui_widgets.hpp` to read roots and widget
snapshots, update current values, and set one-frame pressed state.

| Native | Signature |
|---|---|
| `ui_create_subtab` | `(tab_index: i64, name: string) -> i64` |
| `ui_panel_add` | `(subtab: i64, name: string, two_height: bool) -> i64` |
| `ui_checkbox` | `(panel: i64, label: string, default: bool) -> i64` |
| `ui_keybind` | `(panel: i64, label: string, default: i64, mode: string) -> i64` |
| `ui_slider_int` | `(panel, label, unit, default, min, max, step) -> i64` |
| `ui_slider_double` | `(panel, label, unit, default, min, max, step) -> i64` (numeric values are `f64`) |
| `ui_single_select` | `(panel: i64, label: string, options: i64, default: i64, has_children: bool) -> i64` |
| `ui_multi_select` | `(panel: i64, label: string, options: i64, has_children: bool) -> i64` |
| `ui_range_int` | `(panel, label, unit, default, min, max, step, step2) -> i64` |
| `ui_range_double` | `(panel, label, unit, default, min, max, step, step2) -> i64` (numeric values are `f64`) |
| `ui_color_picker` | `(panel, label, r, g, b, a) -> i64` |
| `ui_input` | `(panel: i64, label: string, default: string) -> i64` |
| `ui_widget_get_bool` | `(handle: i64) -> bool` |
| `ui_widget_get_int` | `(handle: i64) -> i64` |
| `ui_widget_get_float` | `(handle: i64) -> f64` |
| `ui_widget_is_pressed` | `(handle: i64) -> bool` |
| `ui_widget_add_child` | `(parent: i64, slot: i64, child: i64) -> void` |
| `ui_make_options` | `(items: i64) -> i64` |

`ui_make_options` copies an `array<i64>` whose entries are `string` handles and
returns a tagged options handle that cannot collide with array handles.
Selection constructors also accept the original array handle directly for Prism
compatibility. `has_children` records intent; actual child relationships are
added with `ui_widget_add_child`.

`ui_checkbox` and `ui_slider_int` intentionally share names with two ImGui
value-in/value-out calls. A host should register the retained and immediate
surfaces according to the UI model it exposes. In the stock CLI/bundler,
`ui_widgets` is registered after `ui`, so the retained signatures own those two
names; the VST3 wrapper registers the immediate `ui` surface for `on_ui()`.
Consequently, compile retained widget-tree source with the stock CLI, and
compile immediate `on_ui()` source through the VST3 wrapper rather than the
CLI's combined table.

## Generic render command store (`render`)

Resource creation copies all source and array data into host-owned storage.
Handles are never raw GPU pointers. Invalid resource kinds are ignored, destroy
is idempotent, constant-buffer uploads are bounds checked, and reset clears
resources, bindings, and counters.

| Native | Signature |
|---|---|
| `render_create_vertex_shader` | `(hlsl_src: string) -> i64` |
| `render_create_pixel_shader` | `(hlsl_src: string) -> i64` |
| `render_create_input_layout` | `(elements: string) -> i64` |
| `render_create_vertex_buffer` | `(data: i64, stride: i64) -> i64` (`array<f32>`) |
| `render_create_index_buffer` | `(data: i64) -> i64` (four-byte integer array) |
| `render_create_constant_buffer` | `(size: i64) -> i64` (rounded to 16 bytes) |
| `render_update_constant_buffer` | `(handle: i64, data: i64) -> void` (`array<f32>`) |
| `render_set_vertex_shader` / `render_set_pixel_shader` | `(handle: i64) -> void` |
| `render_set_input_layout` / `render_set_vertex_buffer` / `render_set_index_buffer` | `(handle: i64) -> void` |
| `render_set_constant_buffer` | `(slot: i64, handle: i64) -> void` |
| `render_draw` | `(vertex_count: i64) -> void` |
| `render_draw_indexed` | `(index_count: i64) -> void` |
| `render_destroy_resource` | `(handle: i64) -> void` |
| `render_get_draw_calls` | `() -> i64` |
| `render_clear` | `(r: f32, g: f32, b: f32, a: f32) -> void` |
| `render_present` | `() -> void` |

The stock source compiler injects these host constants as `global const i32`
before semantic analysis:

```text
TOPO_TRIANGLE_LIST   = 0
TOPO_TRIANGLE_STRIP  = 1
TOPO_LINE_LIST       = 2
TOPO_LINE_STRIP      = 3
TOPO_POINT_LIST      = 4
```

Input layouts use a backend-neutral descriptor string, for example
`"POSITION:0:FLOAT3;COLOR:0:FLOAT4"`. The stub validates and retains the string
but does not interpret or compile it. A backend installed through
`ext_render::set_backend` receives resource, state, draw, clear, and present
callbacks while the same script API remains unchanged.

## Choosing a layer

Use `graphics` for a self-owned Windows window and full-screen D3D11 effect.
Use immediate `ui` inside an active ImGui/VST3 editor frame. Use `ui_widgets`
when the host owns layout and rendering but the script declares the control
model. Use `render` for portable command capture, tests, or a host-supplied
renderer. The layers can be combined by an embedding host, but registration is
always explicit.
