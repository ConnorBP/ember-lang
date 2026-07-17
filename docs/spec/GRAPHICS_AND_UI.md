# Graphics, VST3 UI, visualization, and LLM export

This specification describes the host-native graphics and UI surfaces added
after v1.3.0. These are extensions, not language grammar: an embedding host must
link/register them and grant `PERM_FFI` before a script can call them.

## 1. Components and platform support

| Component | Location | Platform / owner |
|---|---|---|
| Graphics extension | `extensions/graphics/` | Win32 + D3D11 implementation; typed fail-closed stub elsewhere |
| ImGui extension | `extensions/ui/` | Dear ImGui context owned by a host such as the Windows VST3 editor |
| Retained widget extension | `extensions/ui_widgets/` | Portable widget-tree model; rendering owned by the host |
| Generic render extension | `extensions/render/` | Portable stub store plus optional host backend |
| Visualization/export extension | `extensions/visualize/` | Portable analysis over a host-published atomic snapshot |
| VST3 editor | `examples/vst3_wrapper/vst3_ember_editor.{h,cpp}` | Windows `IPlugView`, child `HWND`, D3D11, ImGui Win32/DX11 |
| ImGui fork | `thirdparty/imgui/` | [`ConnorBP/ember-imgui`](https://github.com/ConnorBP/ember-imgui), Dear ImGui v1.91.9b |

All script functions in this document are `PERM_FFI`-gated. Hosts may omit any
extension even when they grant that permission.

## 2. Graphics extension

### 2.1 Ownership and limits

`window_create` lazily creates all native resources. Windows and shader programs
are represented by opaque `i64` handles packing a 32-bit generation and a
one-based 32-bit slot index. Raw `HWND`, D3D11, and COM pointers do not enter
script memory. Reusing a released slot increments its generation so stale
handles fail.

Current limits:

- 16 live windows;
- 64 live shader programs;
- dimensions from 1 through 16384 pixels;
- 64 KiB HLSL source per shader stage;
- 256 `f32` values (1024 bytes) in each program's constant buffer.

A shader program records its owning window slot and generation. Destroying a
window first releases every dependent program, then its render target, swap
chain, context, device, and `HWND`. `ext_graphics::reset()` performs the same
cleanup globally and is idempotent.

### 2.2 Window API

| Function | Signature | Contract |
|---|---|---|
| `window_create` | `(width: i64, height: i64, title: string) -> i64` | Create a hidden overlapped Win32 window and hardware D3D11 device/swap chain. The title must be valid UTF-8. Return 0 on failure. |
| `window_show` | `(window: i64) -> bool` | Call `ShowWindow(SW_SHOW)` and `UpdateWindow`; false for an invalid handle. |
| `window_poll_events` | `(window: i64) -> bool` | Drain the process Win32 queue with `PeekMessage`; return false after close/quit/device-loss state. |
| `window_should_close` | `(window: i64) -> bool` | Return true when the window was closed or marked unrecoverable. Invalid handles conservatively return true. |
| `window_width` | `(window: i64) -> i64` | Current client width. `WM_SIZE` updates it before render-target resize. |
| `window_height` | `(window: i64) -> i64` | Current client height. Zero is possible while minimized. |
| `window_set_title` | `(window: i64, title: string) -> bool` | Convert UTF-8 to UTF-16 and call `SetWindowTextW`. |
| `window_destroy` | `(window: i64) -> void` | Release dependent programs and all native window resources; invalidate the handle. |

The close handler destroys the native window and marks its slot as wanting to
close. The script still calls `window_destroy` to release D3D ownership. Resize
unbinds and releases the old render target, calls `ResizeBuffers`, and creates a
new target. A minimized zero-size window skips rendering/present work.

### 2.3 Shader API

| Function | Signature | Contract |
|---|---|---|
| `shader_create` | `(window: i64, vertex_hlsl: string, pixel_hlsl: string) -> i64` | Compile `vs_main` as `vs_5_0` and `ps_main` as `ps_5_0`; create both shaders and a dynamic `b0` constant buffer. Return 0 and preserve compiler diagnostics on failure. |
| `shader_destroy` | `(program: i64) -> void` | Release the constant buffer, pixel shader, and vertex shader; invalidate the handle. |
| `shader_set_constants` | `(program: i64, values: i64) -> bool` | Read an `array<f32>` host handle, reject more than 256 values, map with `D3D11_MAP_WRITE_DISCARD`, copy values, and zero-fill the remaining buffer. |
| `shader_draw_fullscreen` | `(program: i64) -> bool` | Bind viewport, render target, shaders, and `b0`, then issue `Draw(3, 0)` with a null input layout and triangle-list topology. |

The vertex shader must synthesize the full-screen triangle from
`SV_VertexID`; no vertex/index buffer is created by this extension. Both shader
stages receive the same constant buffer in slot `b0`.

### 2.4 Presentation and errors

| Function | Signature | Contract |
|---|---|---|
| `graphics_clear` | `(window: i64, r: f32, g: f32, b: f32, a: f32) -> bool` | Clear the active render target to the supplied color. |
| `graphics_present` | `(window: i64, vsync: i64) -> bool` | Present with interval 1 when nonzero and 0 otherwise. Occlusion/minimization sleeps briefly and remains successful. Device removal marks the window for close and returns false. |
| `graphics_last_error` | `() -> string` | Allocate and return the most recent extension error, including HLSL compiler output when available. Successful calls normally clear it. |

The extension serializes native operations with a recursive mutex because
Win32 resize callbacks may re-enter resource management during message
dispatch. It is intended for a UI/render thread, not a realtime audio callback.

### 2.5 Non-Windows behavior

`ext_graphics_stub.cpp` registers the identical signatures. Creation, drawing,
and mutation fail; destroy is a no-op; `window_should_close` returns true; and
`graphics_last_error()` returns:

```text
graphics extension is unsupported on this platform (Win32/D3D11 required)
```

This preserves source validation and host link compatibility without pretending
to provide rendering.

### 2.6 Reference flow

```ember
let window = window_create(1280, 720, "Shader Demo");
if (window == 0) { return 1; }
window_show(window);
let program = shader_create(window, vertex_source, pixel_source);
while (window_poll_events(window) && !window_should_close(window)) {
    shader_set_constants(program, constants);
    graphics_clear(window, 0.0f, 0.0f, 0.0f, 1.0f);
    shader_draw_fullscreen(program);
    graphics_present(window, 1);
}
shader_destroy(program);
window_destroy(window);
```

`examples/mandelbrot_shader.ember` is the full reference and documents its
eight-float convention: time, width, height, mouse x/y, zoom, and center x/y.

## 3. ember-imgui fork

`thirdparty/imgui/` is a Git submodule pointing at the custom Dear ImGui
v1.91.9b fork shared by Ember and the fvc/prism projects. Ember builds the core,
tables, widgets, custom angle-knob source, and Win32/DX11 backends into
`ember_imgui` on Windows.

Fork-specific facilities include:

- `ImGui::SimpleKnob` for conventional continuous controls;
- `ImGui::FovAngleKnob` for angle/FOV controls;
- `themes::retro_neon::ApplyTheme` and its neon palette;
- `themes::retro_neon::ToggleSwitch`;
- CRT scanline/overlay helpers;
- `ImGui::GNeoButtonBg`, a callback hook used to replace standard button/combo
  background drawing while preserving ImGui interaction and layout.

The script API intentionally exposes `SimpleKnob` as `ui_knob` but does not
leak fork C++ pointers or internal ImGui structures.

## 4. VST3 editor architecture

### 4.1 VST interface and native view

`EmberVst3Editor` implements `Steinberg::IPlugView` directly. It supports only
`kPlatformTypeHWND`. `attached` registers a private window class and creates a
visible `WS_CHILD` window under the DAW's parent. The editor starts at 720x520,
advertises resize support, and constrains the minimum to 480x320.

The child window owns:

- a D3D11 hardware device and immediate context;
- a two-buffer `DXGI_FORMAT_R8G8B8A8_UNORM` swap chain;
- a render-target view;
- one Dear ImGui context;
- the ImGui Win32 and DX11 backends.

`removed` kills the render timer, shuts down ImGui backends/context, releases
D3D resources, and destroys the child window. The editor retains/releases the
host's `IPlugFrame` according to VST3 ownership rules.

### 4.2 Frame and resize loop

A 16 ms `SetTimer` drives the normal render cadence. `WM_TIMER` and `WM_PAINT`
call `render`; `WM_SIZE` (except minimization) releases the render target,
resizes swap-chain buffers, and recreates the target. Window messages are first
offered to `ImGui_ImplWin32_WndProcHandler` once ImGui is ready.

Each frame:

1. calls `ImGui_ImplDX11_NewFrame` and `ImGui_ImplWin32_NewFrame`;
2. calls `ImGui::NewFrame`;
3. asks `EmberProcessor::renderScriptUi()` to invoke `on_ui`;
4. if no valid callback succeeds, draws the default knob/visualization view;
5. calls `ImGui::Render`, clears D3D11, submits draw data, and presents with
   sync interval 1.

The `retro_neon` theme is applied when the context is created.

### 4.3 Script callback

The optional callback has one accepted signature:

```ember
fn on_ui() -> void
```

Compilation rejects any `on_ui` with parameters or a non-void result. The JIT
entry is invoked through a dedicated `ui_context`; process, state, report, and
UI calls therefore do not share trap/checkpoint state. A trap/failure causes the
editor to use the default view for that frame.

The callback runs on the UI/editor thread. It is not annotated `@realtime` and
may use array/string/visualization functions. Audio processing remains subject
to the realtime validator.

## 5. UI native API

All functions require `PERM_FFI`. `ready()` requires an active ImGui context.
Without one, action/drawing calls no-op and controls preserve their input value.

### 5.1 Windows, controls, and layout

| Function | Signature | Contract |
|---|---|---|
| `ui_begin` | `(title: string) -> bool` | Begin an ImGui window; use `"Ember"` for an empty title. Pair every call with `ui_end`. |
| `ui_end` | `() -> void` | End the current window. |
| `ui_knob` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Draw `SimpleKnob`; if `max <= min`, preserve `value`. |
| `ui_slider_float` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Draw `SliderFloat`; return the possibly edited value. |
| `ui_slider_int` | `(label: string, value: i64, min: i64, max: i64) -> i64` | Draw `SliderScalar` with signed 64-bit storage. |
| `ui_checkbox` | `(label: string, value: bool) -> bool` | Draw a checkbox and return its state. |
| `ui_combo` | `(label: string, current: i64, csv_items: string) -> i64` | Split comma-separated labels, clamp the initial index, draw a combo, and return the selected index. |
| `ui_button` | `(label: string) -> bool` | Return true on activation. |
| `ui_text` | `(text: string) -> void` | Draw unformatted text. |
| `ui_text_colored` | `(r: f32, g: f32, b: f32, text: string) -> void` | Draw text with supplied RGB and alpha 1. |
| `ui_separator` | `() -> void` | Draw a separator. |
| `ui_same_line` | `() -> void` | Continue layout on the same line. |
| `ui_push_item_width` | `(width: f32) -> void` | Push control width. |
| `ui_pop_item_width` | `() -> void` | Pop control width. |
| `ui_begin_group` | `() -> void` | Begin a layout group. |
| `ui_end_group` | `() -> void` | End a layout group. |
| `ui_get_frame_time` | `() -> f32` | Return `ImGuiIO::DeltaTime`, or 0 without a context. |

Control values are passed by value and returned by value. Scripts persist an
edit by assigning the result to their own state.

### 5.2 Canvas

| Function | Signature | Contract |
|---|---|---|
| `ui_canvas_begin` | `(width: f32, height: f32) -> void` | Record cursor-screen origin, clamp each dimension to at least 1, and push a clip rectangle. |
| `ui_canvas_line` | `(x1, y1, x2, y2, r, g, b, a, thickness: f32) -> void` | Draw a local line; clamp RGBA to 0..1 and thickness to at least 0.5. |
| `ui_canvas_rect` | `(x, y, w, h, r, g, b, a: f32, fill: bool) -> void` | Draw a local filled or outlined rectangle. |
| `ui_canvas_text` | `(x: f32, y: f32, text: string) -> void` | Draw local text with `ImGuiCol_Text`. |
| `ui_canvas_end` | `() -> void` | Pop clipping, submit a dummy item of canvas size, and clear active state. |

Canvas state is thread-local but singular. Nested canvases are unsupported.
Drawing outside an active canvas no-ops.

## 6. Retained widget-tree extension

`ui_widgets` ports Prism's `Widget`/store pattern without carrying its renderer.
Opaque one-based handles identify Subtab, Panel, Checkbox, Keybind, SliderInt,
SliderDouble, SingleSelect, MultiSelect, RangeInt, RangeDouble, ColorPicker,
and Input records. Records retain labels, units, values, limits/steps, options,
selection state, colors, parent/child links, keybind mode, and pressed state.

Every script native is `PERM_FFI`-gated. String arguments are Ember `string`
handles. `ui_make_options` copies an `array<i64>` of string handles into a
separate immutable option list and returns a high-range tagged handle, avoiding
collisions with the independent array store; selection constructors also accept the source
array directly. C++ hosts use `get_widget`, `roots`, and the `set_*` hooks in
`ext_ui_widgets.hpp`; returned widgets are snapshots, not borrowed pointers.
`reset()` clears widgets and options and is idempotent.

The immediate `ui` and retained `ui_widgets` surfaces both contain
`ui_checkbox` and `ui_slider_int` with different signatures. Registration maps
are name-keyed, so hosts must choose one model or deliberately order
registrations. The stock CLI/bundler registers `ui_widgets` after `ui`; the
VST3 wrapper registers immediate `ui` only.

## 7. Generic shader/render extension

`render` is a backend-neutral command/data store. Creation calls copy HLSL,
layout descriptors, `array<f32>` vertex/constant data, and four-byte index data
into `Resource` records. Handles select resources; no raw GPU pointer crosses
the script boundary. Constant-buffer sizes are positive, capped, and rounded
to 16 bytes; uploads cannot exceed the allocated size. Set calls validate the
resource kind, slot indices are limited to 0..63, destroy unbinds a resource,
and negative draw counts are ignored.

`ShaderStore` records the active vertex/pixel shader, input layout, vertex and
index buffers, constant-buffer slots, clear color, and draw/indexed/clear/
present counters. With no backend this is the complete deterministic stub.
A host may install a non-owning `RenderBackend` to receive copied resource
snapshots and state/draw/clear/present callbacks; callbacks occur after the
store mutex is released.

Source hosts call `inject_constants(program)` before sema to publish
`TOPO_TRIANGLE_LIST`, `TOPO_TRIANGLE_STRIP`, `TOPO_LINE_LIST`,
`TOPO_LINE_STRIP`, and `TOPO_POINT_LIST` as synthetic `global const i32`
values. `.em` files preserve those values in their serialized globals, so load
hosts only need the native registrations. `reset()` clears resources, bindings,
and counters while retaining the installed backend pointer.

The complete native tables are listed in the
[script-facing API page](../guide/20-api/60-graphics-ui-render.md).

## 8. Visualization extension

### 6.1 Audio-thread publication

The VST3 processor calls `publish_f32` or `publish_f64` after the selected DSP
callback and f32 crossfade complete. Publication:

- accepts host-owned channel pointers;
- copies at most the newest 2048 frames;
- averages all non-null channels into mono;
- writes samples to `std::atomic<float>` slots with relaxed stores;
- release-stores the final count.

It is `noexcept`, bounded, lock-free at the source level, and allocation-free.
No FFT, vector allocation, or string formatting runs in this producer path.

Readers acquire-load the count, copy each atomic sample, and perform analysis
on the calling thread. A snapshot is observational rather than a block-ID
transaction: readers may observe the latest atomically valid values while a
new publication is in progress, but never race on non-atomic sample storage.

### 6.2 Analysis API

| Function | Signature | Contract |
|---|---|---|
| `viz_get_spectrum` | `(bins: i64) -> i64` | Return an `array<f32>` handle with 1..512 bins. Select FFT size 1024, 512, or 256 based on available data; otherwise return zeros. Apply a Hann window, iterative radix-2 FFT, magnitude normalization, and peak reduction. |
| `viz_get_waveform` | `(samples: i64) -> i64` | Return an `array<f32>` handle with 1..2048 nearest-index samples across the snapshot; empty snapshots produce zeros. |
| `viz_get_rms` | `() -> f32` | Square-root of mean squared samples; 0 when empty. |
| `viz_get_peak` | `() -> f32` | Maximum absolute sample; 0 when empty. |

The returned arrays are host-backed `ext_array` handles. Analysis allocates and
must not be called from `@realtime` DSP.

## 9. LLM-friendly export API

`set_parameter_metadata` is the C++ host input: a parameter-value snapshot,
minimums, maximums, names, count, and source hash. The current wrapper seeds
this snapshot from its selected parameter profile during initialization; hosts
that need exports to track later automation must call the setter again from a
non-realtime control path. A mutex protects metadata because export occurs off
the audio thread.

| Function | Signature | Output |
|---|---|---|
| `llm_export_state` | `() -> string` | JSON-like text with `params.summary`, 32-bin `spectrum`, `rms`, `peak`, `zcr`, and `source_hash`. |
| `llm_export_spectrum` | `(bins: i64) -> string` | Comma-separated magnitudes from the spectrum API. |
| `llm_export_waveform` | `(samples: i64) -> string` | Comma-separated samples from the waveform API. |
| `llm_export_param_summary` | `() -> string` | Semicolon-separated `name=value[min,max]` entries. |

Example state shape:

```text
{"params":{"summary":"Gain:1,Cutoff:1000"},"spectrum":[0.01,0.02],"rms":0.12,"peak":0.31,"zcr":0.08,"source_hash":"..."}
```

The format is intentionally compact and JSON-like. Consumers should not assume
it performs full JSON escaping for arbitrary parameter names/hashes. Export is
a transport aid: Ember does not bundle an LLM client, send data, parse model
responses, or mutate parameters automatically.

## 10. Demo plugin

`examples/vst3_wrapper/demo_ui_vst.ember` demonstrates the integrated path:

- `@realtime process_f32` implements a stereo one-pole filtered mix;
- `on_ui` draws gain, cutoff, resonance, and mix `SimpleKnob` controls;
- a 64-bin spectrum is rendered with canvas rectangles;
- a 128-sample waveform is rendered with canvas lines;
- RMS and peak are displayed;
- a button requests `llm_export_state`;
- the C++ editor supplies the retro-neon theme and D3D11 frame.

Select it before the host loads the plugin:

```text
set EMBER_VST3_SCRIPT=E:\path\to\ember\examples\vst3_wrapper\demo_ui_vst.ember
```

See [`../VST3_PLUGIN_GUIDE.md`](../VST3_PLUGIN_GUIDE.md) for build, callback,
hot-reload, and validation instructions.
