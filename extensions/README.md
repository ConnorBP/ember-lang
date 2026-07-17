# Ember extensions

Ember currently ships **22 extension libraries**: 20 native/addon extensions and
2 ThinIR pass extensions. Extensions are host-side registration units; they do
not add grammar. Native extensions register `NativeSig` entries (and sometimes
operator overloads), while pass extensions register `ThinFunction` transforms.

## Complete inventory

| Directory / target | Kind | Current surface |
|---|---|---|
| `vec/` / `ember_ext_vec` | native + overloads | nominal `vec2`, `vec3`, and `vec4` handles; component access/mutation; `+`, `-`, `*`, `==` |
| `quat/` / `ember_ext_quat` | native + overloads | nominal `quat`; component access/mutation; add/subtract/Hamilton multiply/equality |
| `mat/` / `ember_ext_mat` | native + overloads | nominal `mat4`; construction, identity, indexed get/set, matrix multiply/equality |
| `string/` / `ember_ext_string` | native + overloads | owned `string` handles; conversion, identity, length, indexing, find, substring, `fmt1`-`fmt4`, concatenate/equality |
| `array/` / `ember_ext_array` | native | host-backed arrays; create/length/resize; typed u8/f32/i64 get/set/push/pop; clear/remove; host byte accessors |
| `math/` / `ember_ext_math` | native | f32/f64 trig and elementary functions, floor/ceil/round/trunc, exp/log variants, fmod, min/max/clamp, and integer abs/min/max/clamp |
| `map/` / `ember_ext_map` | native | host-backed i64-key/i64-value map: new/set/get/contains/length/remove/clear |
| `sync/` / `ember_ext_sync` | native | nominal atomics, swap buffers, SPSC/MPSC/MPMC queues, plus host-side accessors |
| `thread/` / `ember_ext_thread` | native | in-context concurrent workers: spawn/join/trap reason; legacy and keyed runtime initialization |
| `coroutine/` / `ember_ext_coroutine` | native | `coroutine_start`/`next`/`done` and `yield`; implemented with Windows fibers. Non-Windows builds link a fail-closed stub, not a coroutine implementation |
| `lifecycle/` / `ember_ext_lifecycle` | native | dynamic `register_routine(fn, data)` / `unregister_routine(id)` |
| `io/` / `ember_ext_io` | native | console, text/byte file, and path operations; all calls are `PERM_FFI`-gated |
| `call_raw/` / `ember_ext_call_raw` | native | raw executable-page creation/call/free plus the **EMBM v1/v2** executable-module loader/query/free API and context-layout queries. Raw/module operations are `PERM_FFI`-gated |
| `gc/` / `ember_ext_gc` | native/runtime | tracing GC, precise frame/global/extension roots, lambda environments, write barriers, `gc_*`, and the runtime substrate for language `new`/`delete` |
| `audio/` / `ember_ext_audio` | native | typed `AudioContext`, parameter/event access, f32/f64 sample I/O, and raw typed buffer helpers; all calls are `PERM_FFI`-gated |
| `graphics/` / `ember_ext_graphics` | native | Win32 windows, D3D11 swap chains, runtime HLSL compilation, full-screen shader drawing, clear/present/error reporting; all calls are `PERM_FFI`-gated; non-Windows uses a fail-closed stub |
| `ui/` / `ember_ext_ui` | native | Dear ImGui windows, custom knobs, controls/layout helpers, and clipped canvas primitives; all calls are `PERM_FFI`-gated; calls no-op when no ImGui frame is active |
| `ui_widgets/` / `ember_ext_ui_widgets` | native | retained Subtab/Panel/control widget tree and host inspection/mutation hooks; data model only, with all script calls `PERM_FFI`-gated |
| `render/` / `ember_ext_render` | native | stub-backed vertex/pixel shaders, input layouts, vertex/index/constant buffers, binding state, draw counters, clear/present, optional host backend, and injected `TOPO_*` constants; all calls are `PERM_FFI`-gated |
| `visualize/` / `ember_ext_visualize` | native | lock-free audio snapshots, radix-2 FFT spectrum, waveform/RMS/peak analysis, and LLM-friendly text export; all calls are `PERM_FFI`-gated |
| `opt/` / `ember_ext_opt` | pass | **18 optimization passes** (listed below) |
| `obf/` / `ember_ext_obf` | pass | **7 obfuscation passes** (listed below) |

The native/addon count is 20: vec, quat, mat, string, array, math, map, sync,
thread, coroutine, lifecycle, io, call_raw, gc, audio, graphics, ui,
ui_widgets, render, and visualize. `opt` and `obf` are extension libraries too, but they use
`register_passes`, not `register_natives`.

## Pass inventory (25 registered names)

`ember_ext_opt` registers 18 value-preserving optimization passes:

- `constprop`, `dce`, `simplifycfg`, `cse`, `gvn`, `licm`, `lsr`
- `forward`, `copyprop`, `instcombine`, `dse`, `bounds-elim`, `sccp`
- `unroll`, `spill_elim`, `peephole`, `branch_folding`, `tailcall`

`ember_ext_obf` registers 7 required obfuscation passes:

- `subst`, `mba_expand`, `const_encode`, `opaque_pred`, `deadcode`,
  `str_encrypt`, `block_split`

See [`PASS_AUTHORING.md`](../docs/PASS_AUTHORING.md) for the pass contract and
[`PASS_SYSTEM_DESIGN.md`](../docs/spec/PASS_SYSTEM_DESIGN.md) for architecture.

## Graphics API (`graphics/`)

All graphics calls require `PERM_FFI`. Window and shader values are opaque,
generation-checked `i64` handles; raw `HWND` and COM pointers never cross the
script boundary. `window_create` lazily creates the Win32/D3D11 resources.
Destroying a window also invalidates its shader programs.

| Native | Signature | Behavior |
|---|---|---|
| `window_create` | `(width: i64, height: i64, title: string) -> i64` | Creates a hidden Win32 window, D3D11 device, double-buffered swap chain, and render target. Returns 0 on failure; dimensions must be 1..16384. |
| `window_show` | `(window: i64) -> bool` | Shows and updates the window. |
| `window_poll_events` | `(window: i64) -> bool` | Drains the Win32 message queue; returns false once the window should close. |
| `window_should_close` | `(window: i64) -> bool` | Reports the close/device-loss state. |
| `window_width` / `window_height` | `(window: i64) -> i64` | Returns the current client dimensions, updated by resize events. |
| `window_set_title` | `(window: i64, title: string) -> bool` | Sets a UTF-8 title through `SetWindowTextW`. |
| `window_destroy` | `(window: i64) -> void` | Destroys the window and every shader owned by it. |
| `shader_create` | `(window: i64, vertex_hlsl: string, pixel_hlsl: string) -> i64` | Compiles `vs_main` and `ps_main` as shader model 5.0 and creates a 256-float `b0` constant buffer. Source is capped at 64 KiB per stage. |
| `shader_destroy` | `(program: i64) -> void` | Releases the program's shaders and constant buffer. |
| `shader_set_constants` | `(program: i64, values: i64) -> bool` | Uploads an `array<f32>` handle containing at most 256 values and zero-fills the remainder of `b0`. |
| `shader_draw_fullscreen` | `(program: i64) -> bool` | Draws a three-vertex full-target triangle with no vertex buffer or input layout; the vertex shader synthesizes positions from `SV_VertexID`. |
| `graphics_clear` | `(window: i64, r: f32, g: f32, b: f32, a: f32) -> bool` | Clears the current render target. |
| `graphics_present` | `(window: i64, vsync: i64) -> bool` | Presents with sync interval 1 when `vsync != 0`, otherwise 0; occlusion/minimization is recoverable. |
| `graphics_last_error` | `() -> string` | Returns the last graphics error, including HLSL compiler diagnostics. |

On non-Windows platforms the same signatures are registered by
`ext_graphics_stub.cpp`; operations fail, `window_should_close` returns true,
and `graphics_last_error()` reports that Win32/D3D11 is required. See
[`examples/mandelbrot_shader.ember`](../examples/mandelbrot_shader.ember) and
[`GRAPHICS_AND_UI.md`](../docs/spec/GRAPHICS_AND_UI.md).

## UI API (`ui/`)

The Windows UI extension targets the active Dear ImGui context. Every call is
`PERM_FFI`-gated. Calls are harmless outside an editor frame: controls return
their input value, action buttons return false, and drawing functions no-op.
This permits CLI/bundle validation of scripts that also define VST3 UI code.

| Native | Signature | Behavior |
|---|---|---|
| `ui_begin` | `(title: string) -> bool` | Begins an ImGui window and returns whether its contents should be submitted. Pair with `ui_end` even when false. |
| `ui_end` | `() -> void` | Ends the current ImGui window. |
| `ui_knob` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Draws the fork's custom `SimpleKnob` and returns its current value. |
| `ui_slider_float` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Draws a float slider and returns its value. |
| `ui_slider_int` | `(label: string, value: i64, min: i64, max: i64) -> i64` | Draws a signed 64-bit integer slider and returns its value. |
| `ui_checkbox` | `(label: string, value: bool) -> bool` | Draws a checkbox and returns its state. |
| `ui_combo` | `(label: string, current: i64, csv_items: string) -> i64` | Draws a combo from comma-separated items and returns the selected zero-based index. |
| `ui_button` | `(label: string) -> bool` | Returns true on activation. |
| `ui_text` | `(text: string) -> void` | Draws unformatted text. |
| `ui_text_colored` | `(r: f32, g: f32, b: f32, text: string) -> void` | Draws opaque colored text. |
| `ui_separator` | `() -> void` | Draws a separator. |
| `ui_same_line` | `() -> void` | Places the next item on the current line. |
| `ui_push_item_width` | `(width: f32) -> void` | Pushes a default item width. |
| `ui_pop_item_width` | `() -> void` | Pops the item width stack. |
| `ui_begin_group` / `ui_end_group` | `() -> void` | Begin/end an ImGui layout group. |
| `ui_get_frame_time` | `() -> f32` | Returns the current ImGui frame delta in seconds, or 0 without a context. |
| `ui_canvas_begin` | `(width: f32, height: f32) -> void` | Starts a clipped draw-list canvas at the cursor. |
| `ui_canvas_line` | `(x1, y1, x2, y2, r, g, b, a, thickness: f32) -> void` | Draws a canvas-relative line; color components are clamped to 0..1. |
| `ui_canvas_rect` | `(x, y, w, h, r, g, b, a: f32, fill: bool) -> void` | Draws a filled or outlined canvas-relative rectangle. |
| `ui_canvas_text` | `(x: f32, y: f32, text: string) -> void` | Draws canvas-relative text with the active text color. |
| `ui_canvas_end` | `() -> void` | Ends clipping and advances layout by the canvas size. |

`ui_canvas_*` uses one thread-local active canvas; begin/end calls must be
balanced and canvases are not nested. The implementation uses the custom
`ember-imgui` v1.91.9b fork described in the top-level README.

## Retained widget and generic render APIs

`ui_widgets/` and `render/` are portable, stub/data-model extensions. The
former creates a retained widget tree for host rendering; the latter copies
shader source and typed array data into a `ShaderStore`, validates resource
bindings, counts draw calls, and optionally forwards snapshots/commands to a
host `RenderBackend`. They perform no D3D11 work themselves.

The complete signatures, topology constants, registration collision note for
the immediate and retained `ui_checkbox`/`ui_slider_int` names, and backend
contract are documented in
[`60-graphics-ui-render.md`](../docs/guide/20-api/60-graphics-ui-render.md) and
[`GRAPHICS_AND_UI.md`](../docs/spec/GRAPHICS_AND_UI.md).

## Visualization and LLM export API (`visualize/`)

The VST3 processor publishes at most 2048 recent mono-mixed output samples
through atomic storage after DSP/crossfade. It takes no lock and performs no
allocation, FFT, or string formatting on the audio thread. Analysis and export
run on the UI/control caller.

| Native | Signature | Behavior |
|---|---|---|
| `viz_get_spectrum` | `(bins: i64) -> i64` | Returns an `array<f32>` magnitude spectrum. Bins are clamped to 1..512; a Hann-windowed radix-2 FFT uses 1024, 512, or 256 available samples and peak-reduces source bins. |
| `viz_get_waveform` | `(samples: i64) -> i64` | Returns an `array<f32>` resampling the latest snapshot; count is clamped to 1..2048. |
| `viz_get_rms` | `() -> f32` | Returns snapshot root-mean-square amplitude, or 0 when empty. |
| `viz_get_peak` | `() -> f32` | Returns peak absolute snapshot amplitude, or 0 when empty. |
| `llm_export_state` | `() -> string` | Returns JSON-like text containing parameter name/value pairs, a 32-bin spectrum, RMS, peak, zero-crossing rate (`zcr`), and source hash. |
| `llm_export_spectrum` | `(bins: i64) -> string` | Returns comma-separated spectrum magnitudes using the same bin limits and FFT path. |
| `llm_export_waveform` | `(samples: i64) -> string` | Returns comma-separated waveform samples using the same sample limits. |
| `llm_export_param_summary` | `() -> string` | Returns semicolon-separated `name=value[min,max]` parameter records. |

The export functions are observational: they provide compact audio/plugin state
that an AI model can consume before suggesting parameter changes; they do not
call a model or mutate plugin parameters. Parameter metadata is a host-supplied
snapshot (`set_parameter_metadata` in the C++ API); hosts refresh it from a
non-realtime path when parameter values change.

## Registering extensions in a host

Link only the extension targets the host intends to expose, then register them
explicitly. Linking is not discovery.

```cpp
std::unordered_map<std::string, ember::NativeSig> natives;
ember::OpOverloadTable overloads;

ember::ext_vec::register_natives(natives);
ember::ext_vec::register_overloads(overloads);
ember::ext_string::register_natives(natives);
ember::ext_string::register_overloads(overloads);
ember::ext_array::register_natives(natives);
ember::ext_math::register_natives(natives);
ember::ext_map::register_natives(natives);
ember::ext_sync::register_natives(natives);
ember::ext_thread::register_natives(natives);
ember::ext_coroutine::register_natives(natives);
ember::ext_lifecycle::register_natives(natives);
ember::ext_io::register_natives(natives);
ember::ext_call_raw::register_natives(natives);
ember::ext_gc::register_natives(natives);
ember::ext_audio::register_natives(natives);
ember::ext_graphics::register_natives(natives);
ember::ext_ui::register_natives(natives);
ember::ext_ui_widgets::register_natives(natives);
ember::ext_render::register_natives(natives);
ember::ext_visualize::register_natives(natives);
```

Quat and mat follow the same native/overload pattern as vec. Host-backed stores
must be reset between independent runs. Thread, coroutine, GC, call_raw's EMBM
loader, and lifecycle also have host initialization/context APIs; read their
public `ext_*.hpp` files before exposing them. In particular:

- call `ext_thread::thread_init(...)` or its keyed counterpart before spawning;
- call `ext_coroutine::coroutine_init(...)` on Windows (or the keyed API where
  supported) before starting coroutines;
- initialize/attach GC when using GC environments or language `new`/`delete`;
- call `ext_call_raw::set_loader_context(...)` before EMBM images with native
  relocations are loaded.

A host can omit any capability simply by not linking/registering it. `io`,
`audio`, `graphics`, `ui`, `ui_widgets`, `render`, `visualize`, and raw/module operations in `call_raw`
additionally require the `PERM_FFI` permission at sema time.

Pass registration is separate:

```cpp
ember::EmberPassRegistry passes;
ember::ext_opt::register_passes(passes);
ember::ext_obf::register_passes(passes); // deterministic compatibility defaults
```

Configured obfuscation factories and named profiles are covered in the pass
authoring guide.

## Build integration

The root `CMakeLists.txt` defines every `ember_ext_*` library. The stock
`ember_cli` links and registers all 20 native/addon extensions and both pass
extensions on Windows. `ember_ext_ui` is an interface/no-op build target on
non-Windows, while graphics provides its typed unsupported stub. The standalone
bundle/stub intentionally expose a smaller runtime surface than the CLI; do not
infer host capabilities from the mere existence of an extension library.

## Historical Prism relocation audit

[`AUDIT.md`](AUDIT.md) records the original six-extension Prism classification.
It is a historical provenance audit, not the current extension inventory. All
later generic extensions are listed above.
