# Writing VST3 plugins in Ember

The wrapper builds one combined VST3 component/controller named
`ember_gain.vst3`. DSP lives in an Ember script. The default script is
`examples/vst3_wrapper/gain_vst.ember`; set `EMBER_VST3_SCRIPT` before loading
the plugin to select another script.

## Build

```bash
cmake -S . -B buildt -G Ninja \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DEMBER_BUILD_VST3=ON
cmake --build buildt -j 8
```

With the default single-config Release build, the bundle is normally:

```text
buildt/VST3/Release/ember_gain.vst3/
```

Install it under `C:/Program Files/Common Files/VST3/` or point a development
host at the build directory. The SDK submodule is pinned to VST3 3.8.0. The
editor also requires the `thirdparty/imgui` submodule, the custom
[`ember-imgui`](https://github.com/ConnorBP/ember-imgui) v1.91.9b fork. Run
`git submodule update --init --recursive` before configuring.

## Minimal DSP script

```ember
@realtime
fn process_f32(ctx: i64, frames: i64) -> void {
    let channels = audio_get_num_output_channels(ctx);
    let mut frame: i64 = 0;
    while (frame < frames) {
        let mut channel: i64 = 0;
        while (channel < channels) {
            let sample = audio_load_sample(ctx, channel, frame);
            audio_store_sample(ctx, channel, frame, sample * 0.5f);
            channel += 1;
        }
        frame += 1;
    }
}

fn get_latency() -> i64 { return 0; }
fn get_tail() -> i64 { return 0; }
```

The required f32 callback is
`fn process_f32(ctx: i64, frames: i64) -> void`; legacy name `process` is also
accepted. `process_f64` with the same signature is optional. The wrapper only
advertises 64-bit sample processing when that callback exists.

## Callback contract

| Callback | Required | Contract |
|---|---:|---|
| `process_f32(ctx: i64, frames: i64) -> void` | yes (or legacy `process`) | f32 block processing |
| `process_f64(ctx: i64, frames: i64) -> void` | no | f64 block processing |
| `get_latency() -> i64` | no | latency samples; missing/negative becomes 0 |
| `get_tail() -> i64` | no | tail samples; missing/negative becomes 0 |
| `save_state() -> i64` | no | pointer to a wrapper-compatible `ScriptStateBuffer { data, size }` descriptor whose bytes remain valid through migration/write |
| `load_state(ptr: i64, len: i64) -> void` | no | consume state bytes; **two arguments**, not one opaque handle |
| `on_ui() -> void` | no | submit the custom Dear ImGui interface once per editor frame; runs on the UI/editor thread, never the audio callback |

All signatures are checked before compilation. State payloads are capped at 16
MiB. The simple examples often return 0 from `save_state`; stateful plugins must
honor the descriptor lifetime contract in `vst3_ember_processor.cpp`.

## AudioContext natives

The wrapper registers `audio`, `array`, `string`, `math`, `ui`, and `visualize`.
Their host-bound natives are `PERM_FFI`-gated, and the wrapper sema run grants
that permission.

### Block and sample access

| Native | Return | Meaning |
|---|---|---|
| `audio_get_sample_rate(ctx)` | `i64` | current sample rate |
| `audio_get_block_size(ctx)` | `i64` | frames in the current process call |
| `audio_get_num_input_channels(ctx)` | `i64` | 0 or 2 in the current wrapper |
| `audio_get_num_output_channels(ctx)` | `i64` | 2 |
| `audio_load_sample(ctx,ch,frame)` | `f32` | bounds-checked f32 input; 0 when no input |
| `audio_store_sample(ctx,ch,frame,value)` | `void` | bounds-checked f32 output |
| `audio_load_sample_f64(...)` / `audio_store_sample_f64(...)` | `f64` / `void` | f64 equivalents |

Legacy raw helpers `load_f32`/`store_f32`, `load_f64`/`store_f64`, and
`load_i32`/`store_i32` also exist, but the typed `AudioContext` accessors are
the preferred VST surface.

### Parameters and transport

- `audio_get_parameter(ctx, id) -> f32`
- `audio_get_param_change_count(ctx) -> i64`
- `audio_get_param_change_id/offset/value(ctx, index)`
- `audio_is_playing(ctx) -> i64`
- `audio_get_bpm(ctx) -> f64`
- `audio_get_ppq(ctx) -> f64`

The wrapper has at most three parameter IDs. Parameter metadata is selected by
the script filename profile (gain, delay, filter, or oscillator/synth); other
example scripts use the generic gain profile unless the host/wrapper is
extended.

### Events

- `audio_get_event_count(ctx)`
- `audio_get_event_type/channel/note/velocity/offset(ctx, index)`
- `audio_add_event(ctx, type, channel, note, velocity, offset)`

Event kinds are note-on, note-off, note-expression, and controller in the
extension ABI. Input/output event storage is bounded and allocation-free in
`process()`.

## Plugin UI and `on_ui()`

The Windows editor in `vst3_ember_editor.{h,cpp}` implements VST3 `IPlugView`
directly; it does **not** use VSTGUI. `attached()` creates a child Win32 `HWND`
inside the DAW-supplied parent, creates a D3D11 device/swap chain/render target,
and initializes Dear ImGui's Win32 and DX11 backends. The default view is
720x520 and is resizable down to 480x320. `WM_SIZE` recreates the swap-chain
render target.

A 16 ms Win32 timer (plus paint messages) drives rendering:

1. start ImGui's DX11 and Win32 backend frames;
2. begin a new ImGui frame;
3. call the selected script's optional `fn on_ui() -> void` through a dedicated
   UI execution context;
4. if there is no callback or it traps, render the wrapper's default parameter
   knobs, spectrum, waveform, RMS, and peak interface;
5. render draw data to D3D11 and present with vsync.

The editor applies the custom `retro_neon` theme on attachment. The
`ember-imgui` fork also contains `SimpleKnob` (used by `ui_knob`),
`FovAngleKnob`, a neon palette, `ToggleSwitch`, CRT overlay helpers, and the
`GNeoButtonBg` callback hook for custom button backgrounds. Ember scripts see
the stable native API below rather than raw ImGui pointers.

### Widget and layout natives

All UI natives require `PERM_FFI`. Calls made when no ImGui context/frame is
active are harmless: value controls return their input, buttons return false,
and drawing functions no-op. Always call `ui_end()` after `ui_begin()`, even if
`ui_begin()` returned false.

| Native | Signature | Meaning |
|---|---|---|
| `ui_begin` | `(title: string) -> bool` | Begin a window; returns whether contents should be submitted. |
| `ui_end` | `() -> void` | End the current window. |
| `ui_knob` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Custom `SimpleKnob`; returns the edited value. |
| `ui_slider_float` | `(label: string, value: f32, min: f32, max: f32) -> f32` | Float slider; returns the edited value. |
| `ui_slider_int` | `(label: string, value: i64, min: i64, max: i64) -> i64` | Signed 64-bit slider; returns the edited value. |
| `ui_checkbox` | `(label: string, value: bool) -> bool` | Checkbox; returns the edited state. |
| `ui_combo` | `(label: string, current: i64, csv_items: string) -> i64` | Combo from comma-separated labels; returns a zero-based selection. |
| `ui_button` | `(label: string) -> bool` | True on activation. |
| `ui_text` | `(text: string) -> void` | Draw unformatted text. |
| `ui_text_colored` | `(r: f32, g: f32, b: f32, text: string) -> void` | Draw opaque colored text. |
| `ui_separator` | `() -> void` | Insert a separator. |
| `ui_same_line` | `() -> void` | Keep the next item on the same line. |
| `ui_push_item_width` | `(width: f32) -> void` | Push the width used by subsequent controls. |
| `ui_pop_item_width` | `() -> void` | Pop the width stack. |
| `ui_begin_group` / `ui_end_group` | `() -> void` | Begin/end an ImGui layout group. |
| `ui_get_frame_time` | `() -> f32` | Current UI frame delta in seconds. |

Controls use a value-in/value-out ABI. Assign the return value when state should
persist:

```ember
fn on_ui() -> void {
    if (ui_begin("My Effect")) {
        gain = ui_knob("Gain", gain, 0.0f, 2.0f);
        ui_same_line();
        mix = ui_slider_float("Mix", mix, 0.0f, 1.0f);
        enabled = ui_checkbox("Enabled", enabled);
    }
    ui_end();
}
```

Script globals used by DSP must still obey the plugin's cross-thread state
contract. The lock-free guarantee described below applies to visualization
sample publication; do not add locks, allocation, formatting, or other
non-realtime work to `process_f32`/`process_f64`.

### Canvas natives

Canvas coordinates are local to the cursor position at `ui_canvas_begin`.
Drawing is clipped to the requested size. One thread-local canvas can be active
at a time; canvases are not nested and begin/end must be balanced.

| Native | Signature | Meaning |
|---|---|---|
| `ui_canvas_begin` | `(width: f32, height: f32) -> void` | Start a clipped canvas. |
| `ui_canvas_line` | `(x1, y1, x2, y2, r, g, b, a, thickness: f32) -> void` | Draw a local line. |
| `ui_canvas_rect` | `(x, y, w, h, r, g, b, a: f32, fill: bool) -> void` | Draw a filled/outlined local rectangle. |
| `ui_canvas_text` | `(x: f32, y: f32, text: string) -> void` | Draw local text. |
| `ui_canvas_end` | `() -> void` | End clipping and reserve the canvas layout area. |

## Audio visualization

After DSP and any hot-reload crossfade, `process()` publishes a bounded mono
mix of the newest output block. Publication stores at most 2048 samples in
atomic slots and then release-publishes the count. It performs no allocation,
lock acquisition, FFT, or text formatting on the audio thread. The UI/control
thread reads that snapshot and performs analysis:

| Native | Return | Meaning |
|---|---|---|
| `viz_get_spectrum(bins)` | `i64` array handle | `array<f32>` magnitude spectrum; bins clamp to 1..512. A Hann-windowed radix-2 FFT chooses 1024, 512, or 256 source samples and peak-reduces into the requested bins. |
| `viz_get_waveform(samples)` | `i64` array handle | `array<f32>` resampling the recent mono snapshot; samples clamp to 1..2048. |
| `viz_get_rms()` | `f32` | RMS amplitude across the current snapshot, or 0 when empty. |
| `viz_get_peak()` | `f32` | Maximum absolute amplitude, or 0 when empty. |

Use the array extension to inspect returned handles:

```ember
let bins: i64 = viz_get_spectrum(64);
let magnitude: f32 = array_get_f32(bins, 0);
```

The default editor uses C++ accessors over this same snapshot to render a
64-bin histogram, 128-sample waveform, and RMS/peak progress bars when no
script UI is available.

## LLM-friendly export

The export API turns current plugin/audio observations into compact text that
can be passed to an external AI model. Ember does not contact a model and these
calls do not apply parameter changes.

| Native | Return | Format |
|---|---|---|
| `llm_export_state()` | `string` | JSON-like object containing parameter name/value pairs, a 32-bin spectrum, RMS, peak, zero-crossing rate (`zcr`), and `source_hash`. |
| `llm_export_spectrum(bins)` | `string` | Comma-separated magnitudes with the same 1..512 limits and FFT path as `viz_get_spectrum`. |
| `llm_export_waveform(samples)` | `string` | Comma-separated samples with the same 1..2048 limits as `viz_get_waveform`. |
| `llm_export_param_summary()` | `string` | Semicolon-separated `name=value[min,max]` records. |

The parameter metadata is seeded from the wrapper's selected script profile at
initialization. The current setter captures a value snapshot; a host that needs
exports to follow subsequent automation must refresh it from a non-realtime
control path. Export from `on_ui()` or another control-thread call, never from a
realtime callback, because snapshots are copied and FFT/string formatting
allocate.

## Realtime validation

Annotate the process callback with `@realtime`. Sema rejects operations that
cannot be proven safe on the audio thread, including:

- GC/allocation (`new`, `delete`, `gc_*`);
- I/O (`print*`, `file_*`, `path_*`, `read_line`);
- thread/channel/mutex operations;
- raw execution/FFI escape hatches;
- try/catch/throw;
- indirect, cross-module, or unapproved script/native calls;
- by-reference lambda capture.

Audio accessors and the audited math surface are allowed. The stress suite also
checks that warmed `process()` performs zero C++ allocations and no collection.

## Hot reload

A watcher polls the selected script, compiles changed source on a background
thread, and publishes a complete immutable module at a block boundary. A
failed edit retains the last known-good module. The audio thread takes owning
`shared_ptr` snapshots, so retired modules remain alive through an in-flight
block/crossfade. `EMBER_VST3_CROSSFADE_SAMPLES` controls the f32 transition
(default 64; invalid values fall back to the default).

If both old and new scripts implement the state callbacks, reload migrates
state through `save_state` and `load_state(ptr,len)`. Latency and tail reports
are refreshed after publication.

## Example scripts (14)

`gain`, `delay`, `filter`, `oscillator`, `synth`, `distortion`, `panner`,
`tremolo`, `compressor`, `chorus`, `bitcrusher`, `limiter`, `reverb`, and
`demo_ui` live in `examples/vst3_wrapper/`.

[`demo_ui_vst.ember`](../examples/vst3_wrapper/demo_ui_vst.ember) is the
complete custom-editor reference. It provides gain, cutoff, resonance, and mix
knobs; a 64-bin frequency-spectrum canvas; a 128-sample waveform canvas;
RMS/peak text; and a button that displays `llm_export_state()`. The editor
applies the retro-neon theme around the script UI.

They are **script examples for one wrapper binary**, not 14 separately built
VST3 libraries. Only `gain_vst.ember` and `synth_vst.ember` are attached as
bundle resources by current CMake; development selection normally uses
`EMBER_VST3_SCRIPT` and a filesystem path.

## Node graph model and code generation

Phase 9 adds an editor-side graph substrate:

- `src/node_graph.hpp/.cpp`: graph/node/port model, validation, strict JSON
  save/load;
- `src/node_codegen.hpp/.cpp`: deterministic graph-to-Ember VST source;
- built-in nodes: oscillator, filter, gain, mixer, delay;
- numeric parameters receive stable VST parameter IDs;
- directed cycles and invalid ports/names/types are rejected.

This is a **node-graph backend/model**, separate from the shipped ImGui plugin
editor. It is not a VSTGUI editor. An external editor can load/save JSON and
call `graph_to_ember_vst(...)`; the generated source uses the existing wrapper.
Run `buildt/node_graph_test.exe` directly to validate graph round-trips and
generated sema-valid source. The target is intentionally not registered with
CTest.

## Tests and validator

```bash
ctest --test-dir buildt -R '^vst_dsp_harness$' --output-on-failure
ctest --test-dir buildt -R '^vst3_(stress|realtime_contract|fuzz)$' --output-on-failure
ctest --test-dir buildt -R '^vst3_soak$' --output-on-failure
./buildt/node_graph_test.exe
```

The 2026-07-12 Release bundle passed the SDK 3.8.0 validator: 47 tests passed,
0 failed. See the stress-suite README for exact commands.

## Current limitations

- Windows-first, stereo output, with zero or one stereo input bus;
- no sidechain bus;
- custom editor is Windows-only (`HWND` + D3D11 + ImGui), with no VSTGUI or
  non-Windows editor backend;
- one plugin class/bundle, script-selected DSP;
- no f64 hot-reload crossfade (f32 path crossfades; f64 swaps at boundary);
- node graph is a model/JSON/codegen API and is not yet wired to an interactive
  graph canvas in the plugin;
- LLM exports format state for an external consumer; they do not include a
  model client or automatically apply suggested changes.
