# Writing VST Plugins in ember

This guide covers how to write VST3 audio plugins using ember as the DSP scripting language.

## Quick Start

### Build the VST3 wrapper

```bash
cd ember
mkdir buildt && cd buildt
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build . -j 8
```

The VST3 plugin is built at `buildt/VST3/Release/ember_gain.vst3/`.

### Load in a DAW

Copy the `.vst3` bundle to your DAW's VST3 plugin directory:
- **Windows**: `C:/Program Files/Common Files/VST3/`
- The DAW will scan and find "ember_gain" in its plugin browser

### Write your own plugin

Create a `.ember` file with a `@realtime` `process_f32` function:

```ember
@realtime
fn process_f32(ctx: i64, frames: i64) -> void {
    let channels: i64 = audio_get_num_input_channels(ctx);
    let mut i: i64 = 0;
    while (i < frames) {
        let mut ch: i64 = 0;
        while (ch < channels) {
            let s: f32 = audio_load_sample(ctx, ch, i);
            audio_store_sample(ctx, ch, i, s * 0.5);  // gain = 0.5
            ch = ch + 1;
        }
        i = i + 1;
    }
}
```

## The ember VST API

### AudioContext

The `ctx: i64` parameter is a pointer to an AudioContext struct containing all audio data. Access it via natives:

| Native | Returns | Description |
|--------|---------|-------------|
| `audio_get_sample_rate(ctx)` | `i64` | Sample rate (e.g., 48000) |
| `audio_get_block_size(ctx)` | `i64` | Max block size |
| `audio_get_num_input_channels(ctx)` | `i64` | Input channel count |
| `audio_get_num_output_channels(ctx)` | `i64` | Output channel count |
| `audio_load_sample(ctx, ch, i)` | `f32` | Load input sample (channel ch, index i) |
| `audio_store_sample(ctx, ch, i, val)` | `void` | Store output sample |
| `audio_load_sample_f64(ctx, ch, i)` | `f64` | Load f64 input sample |
| `audio_store_sample_f64(ctx, ch, i, val)` | `void` | Store f64 output sample |
| `audio_get_parameter(ctx, id)` | `f32` | Get current parameter value |
| `audio_is_playing(ctx)` | `i64` | Transport playing (1/0) |
| `audio_get_bpm(ctx)` | `f64` | Tempo (BPM) |
| `audio_get_ppq(ctx)` | `f64` | Musical position (PPQ) |

### Parameter Automation

Sample-accurate parameter changes are available via:

| Native | Returns | Description |
|--------|---------|-------------|
| `audio_get_param_change_count(ctx)` | `i64` | Number of parameter changes in this block |
| `audio_get_param_change_id(ctx, i)` | `i64` | Parameter ID for change i |
| `audio_get_param_change_offset(ctx, i)` | `i64` | Sample offset for change i |
| `audio_get_param_change_value(ctx, i)` | `f32` | New value for change i |

### MIDI Events

| Native | Returns | Description |
|--------|---------|-------------|
| `audio_get_event_count(ctx)` | `i64` | Number of MIDI events in this block |
| `audio_get_event_type(ctx, i)` | `i64` | Event type (0=NoteOn, 1=NoteOff) |
| `audio_get_event_channel(ctx, i)` | `i64` | MIDI channel |
| `audio_get_event_note(ctx, i)` | `i64` | Note number (0-127) |
| `audio_get_event_velocity(ctx, i)` | `i64` | Velocity (0-127) |
| `audio_get_event_offset(ctx, i)` | `i64` | Sample offset |
| `audio_add_event(ctx, type, ch, note, vel, offset)` | `void` | Output a MIDI event |

### Latency, Tail, State

```ember
fn get_latency() -> i64 { return 0; }  // report latency in samples
fn get_tail() -> i64 { return 0; }     // report tail length in samples
fn save_state() -> i64 { ... }         // save DSP state, return handle
fn load_state(state: i64) -> void { ... }  // restore DSP state
```

### @realtime Annotation

The `@realtime` annotation tells the ember compiler to validate that your function is real-time safe. The following are **forbidden** in `@realtime` functions:

- GC allocation (`gc_alloc`, `new`, `delete`)
- I/O operations (`print`, `file_*`, `path_*`)
- Thread operations (`thread_create`, `mutex_*`)
- Exceptions (`try`/`catch`/`throw`)
- FFI (`call_raw`, `make_executable`)

## Example Plugins

| Plugin | File | Description |
|--------|------|-------------|
| Gain | `gain_vst.ember` | Simple gain effect with parameter automation |
| Delay | `delay_vst.ember` | Delay with feedback and mix controls |
| Filter | `filter_vst.ember` | Biquad lowpass filter with cutoff/resonance |
| Oscillator | `oscillator_vst.ember` | MIDI instrument with sine/square/saw waveforms |
| Synth | `synth_vst.ember` | Simple synthesizer with ADSR envelope |

## Hot Reload

The VST3 wrapper supports **hot reload** — edit your `.ember` DSP script while the plugin runs in the DAW:

1. The wrapper watches the `.ember` file for changes
2. On change, it recompiles the script on a background thread
3. The new module is swapped atomically at the next block boundary
4. Old JIT pages are reclaimed safely (no audio glitches)
5. DSP state is preserved via `save_state`/`load_state`

**Failure retention**: if the new script fails to compile, the last known-good processor continues running.

## Testing

### Headless DSP Harness

```bash
# Run the DSP test harness (compares ember output against C++ references)
ctest -R vst_dsp
```

### Stress Tests

```bash
# Run VST3 stress tests
ctest -R vst3_stress
ctest -R vst3_realtime_contract
ctest -R vst3_fuzz
ctest -R vst3_soak
```

### Block Partition Invariance

The DSP harness verifies that processing N samples as one block gives the same result as processing them as multiple sub-blocks (e.g., 1024 as 1×1024 vs 4×256 vs 16×64).

## Limitations

The current VST3 wrapper supports:
- ✅ f32 and f64 processing
- ✅ Stereo/mono/multi-channel
- ✅ Sample-accurate parameter automation
- ✅ MIDI input/output
- ✅ Latency/tail reporting
- ✅ State/preset serialization
- ✅ Hot reload with state migration
- ✅ Silence flags

Not yet supported:
- ❌ VSTGUI editor (no custom UI — use the DAW's generic parameter view)
- ❌ Multiple plugins per library (one plugin per .vst3)
- ❌ Sidechain buses (planned)
- ❌ Note expression (planned)
