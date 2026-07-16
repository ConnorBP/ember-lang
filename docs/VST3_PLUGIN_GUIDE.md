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
host at the build directory. The SDK submodule is pinned to VST3 3.8.0.

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

All signatures are checked before compilation. State payloads are capped at 16
MiB. The simple examples often return 0 from `save_state`; stateful plugins must
honor the descriptor lifetime contract in `vst3_ember_processor.cpp`.

## AudioContext natives

The wrapper registers only `ember_ext_audio` and `ember_ext_math`. Audio calls
are `PERM_FFI`-gated, and the wrapper sema run grants that permission.

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

## Example scripts (13)

`gain`, `delay`, `filter`, `oscillator`, `synth`, `distortion`, `panner`,
`tremolo`, `compressor`, `chorus`, `bitcrusher`, `limiter`, and `reverb` live in
`examples/vst3_wrapper/`.

They are **script examples for one wrapper binary**, not 13 separately built
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

This is a **node-graph editor backend/model**, not a shipped VSTGUI visual
editor. An external editor can load/save JSON and call
`graph_to_ember_vst(...)`; the generated source uses the existing wrapper.
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
- no custom VSTGUI view (DAW generic parameter UI only);
- one plugin class/bundle, script-selected DSP;
- no f64 hot-reload crossfade (f32 path crossfades; f64 swaps at boundary);
- node graph is a model/JSON/codegen API, not a complete visual UI.
