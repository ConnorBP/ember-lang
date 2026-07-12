# VST3 Ember Wrapper — Planning Document

## Overview

A VST3 plugin wrapper that lets users write audio plugins **fully in ember**. The C++ wrapper IS the VST3 plugin (a .dll/.vst3 loaded by DAWs), and it delegates DSP processing to precompiled ember functions. This showcases ember's real-world capabilities: pipelines, hot reload, high-throughput data flow, and embedding — while providing a genuinely useful starting point for VST plugin development.

## Goals

1. **Real-world use case showcase** — ember as a DSP scripting language inside a real VST3 plugin
2. **Pipeline showcase** — typed audio pipeline model (block buffers, channels, parameters, events)
3. **Hot reload bootstrap** — iterate on plugin DSP without restarting the DAW; ember live's epoch-based module swap applied to audio
4. **Hot reload showcase** — file-content change detection, atomic module swap at block boundaries, guard-based safe page retirement
5. **High data flow stress test** — real-time audio at 48kHz, blocks of 64–1024 samples, f32/f64
6. **Language/tooling improvement revelation** — expose gaps in ember's float performance, array throughput, real-time safety, memory model
7. **Optional visual node graph editor** — for the audio pipeline system (separate from plugin runtime)
8. **Full VST API exposure** — write a complete VST3 plugin (effect or instrument) fully in ember

## VST3 SDK License (Verified)

**VST3 SDK 3.8.0 (October 20, 2025) — MIT License.** Steinberg relicensed VST3 from GPL-v3/proprietary dual to MIT. No signed agreements, no redistribution restrictions, fully compatible with ember's AGPL. SDK can be vendored directly.

Sources:
- https://www.theaudioprogrammer.com/content/steinbergs-vst3-asio-sdks-go-open-source
- https://forums.steinberg.net/t/vst-3-8-0-sdk-released/1011988
- https://github.com/steinbergmedia/vst3sdk (README: "VST 3 SDK is under MIT license")

**Pinned SDK revision:** vst3sdk 3.8.0 (2025-10-20). Vendor as `thirdparty/vst3sdk/` git submodule.

## Architecture

### Design Decision: VST3 wrapper plugin delegating DSP to ember

The C++ wrapper implements the VST3 `IComponent` + `IAudioProcessor` + `IEditController` interfaces. It loads an ember script at instantiation, compiles it, and calls precompiled ember functions for audio processing. The ember script implements the DSP logic (gain, filter, delay, synth, etc.).

A **headless standalone test host** is also built for development/testing without a DAW.

### Two-Plane API

**Control plane (non-real-time, UI/background thread):**
- Plugin manifest: name, vendor, category, version
- Parameter definitions: IDs, names, ranges, units, default values
- Bus configuration: stereo/mono input/output, sidechain
- State/preset serialization (save/load)
- Script compilation and hot reload
- UI messages (open/close editor, parameter updates)

**Audio plane (real-time, audio thread):**
- One precompiled block callback: `process_f32(ctx, inputs, outputs, frames)` or `process_f64(...)`
- Pre-allocated buffer state — no allocations in process()
- Parameter snapshot at block start (sample-accurate automation applied within)
- Never exposes raw COM/VST3 interfaces to ember scripts

### Realtime-Safe Ember Profile

ember's JIT calls are fast (~6-7x AngelScript) but the audio thread has strict requirements:

1. **`@realtime` annotation/validation** — a sema pass that rejects:
   - GC allocation (`gc_alloc`, `new`, lambdas with by-ref capture)
   - I/O operations (file, console, path)
   - Thread creation/synchronization
   - Exceptions (try/catch/throw)
   - Unbounded recursion
   - `call_raw` / executable-memory natives
   - Non-RT pipeline stages

2. **Pre-allocation** — all buffers, filter state, delay lines allocated in `setActive(true)` (UI thread), before `setProcessing(true)`.

3. **Block-level calls** — ember is called once per block (e.g., 256 samples), not once per sample. The ember function processes the entire block in a tight loop.

4. **GC disabled** — while processing, GC is explicitly disabled. Realtime scripts cannot use heap-dependent features.

5. **Ember traps** — caught via the existing `__builtin_setjmp` checkpoint mechanism. A trap in the audio thread triggers bypass (output = input or silence).

### Hot Reload Architecture

```
Background Thread                 Audio Thread
─────────────────                ──────────────
detect file change
compile + validate
build ProcessPlan ──────────────► atomic swap at block boundary
reclaim old module               call new ember function
(epoch/guard mechanism)          (guard ensures no use-after-free)
```

- **Compilation on background thread** — file change detected, new ember script compiled and validated on a worker thread (not audio thread).
- **Atomic ProcessPlan publish** — the new compiled module + parameter mapping is published as an immutable `ProcessPlan` via atomic pointer swap at the next block boundary.
- **Old module reclamation** — using ember's existing epoch/guard mechanism from `HotReloadDomain` (src/hot_reload.hpp). Old JIT pages are retired and freed only when no audio-thread guard is active.
- **State migration** — `save_state()` / `load_state()` callbacks let the ember script migrate its state across reloads (e.g., filter coefficients, delay buffer position).
- **Crossfade** — optional short crossfade (configurable, e.g., 64 samples) between old and new processors to avoid clicks.
- **Failure retention** — on compilation or runtime failure, the last known-good processor is retained. The user sees an error message but audio continues.

### VST3 Surface (Full API Exposure)

The wrapper exposes the following VST3 features to ember scripts:

| Feature | Ember API |
|---------|-----------|
| 32-bit float processing | `process_f32(ctx, in_buf, out_buf, frames)` |
| 64-bit double processing | `process_f64(ctx, in_buf, out_buf, frames)` |
| Dynamic buses (stereo/mono/sidechain) | Bus config in manifest, channel counts in ctx |
| Sample-accurate automation | Parameter queue in ctx, sample offsets |
| MIDI/note input | Event list in ctx (note on/off, CC, pressure) |
| MIDI/note output | Event output list (for instruments) |
| ProcessContext (tempo, transport, PPQ) | Transport info in ctx |
| Latency reporting | `get_latency()` callback |
| Tail samples | `get_tail()` callback |
| State/preset save/load | `save_state()` / `load_state()` callbacks |
| Silence flags | `ctx.silence_flags` input, settable output |
| Offline processing | `ctx.offline` flag |
| Bypass | `ctx.bypass` flag |

### Typed Audio Pipeline Model

The current ember pipeline (`ember pipe`) is i64→i64 and not suitable for audio. A new **typed audio pipeline** model is needed:

```
AudioPipeline {
    nodes: [AudioNode]        // preallocated, fixed capacity
    connections: [Edge]       // node output → node input
    parameters: [ParamSlot]   // preallocated, sample-accurate
    events: [EventSlot]       // MIDI/note events
    block_buffers: [Buffer]   // preallocated per node
}
```

- **Compiled into one immutable ProcessPlan** — the graph is compiled (not interpreted) into a flat execution order. No per-node dynamic dispatch on the audio thread.
- **Fixed capacity** — no dynamic allocation. Nodes and buffers are preallocated.
- **Block-based** — the pipeline processes entire blocks, not individual samples.

The **visual node graph editor** (optional, separate from runtime) would be a GUI tool for constructing these pipelines visually, exporting them as ember scripts or pipeline config files.

## ember Script API (What the User Writes)

A simple gain plugin in ember:

```ember
// gain_plugin.ember — a VST3 gain plugin

@plugin("ember_gain", "ember", "1.0.0", "effect")
@realtime

fn manifest() -> PluginManifest {
    let m: PluginManifest = new_plugin_manifest();
    add_parameter(m, 0, "Gain", 0.0, 2.0, 1.0, "dB");
    add_audio_bus(m, "Input", 2, true);   // stereo in
    add_audio_bus(m, "Output", 2, false); // stereo out
    return m;
}

fn process_f32(ctx: ProcessContext, inputs: f32[][], outputs: f32[][], frames: i64) -> void {
    let gain: f32 = get_parameter_f32(ctx, 0);
    let channels: i64 = bus_channel_count(ctx, 0); // input bus
    let ch: i64 = 0;
    while (ch < channels) {
        let i: i64 = 0;
        while (i < frames) {
            outputs[ch][i] = inputs[ch][i] * gain;
            i = i + 1;
        }
        ch = ch + 1;
    }
}

fn save_state() -> State {
    // save current parameter values
    return save_parameter_state(0);
}

fn load_state(state: State) -> void {
    restore_parameter_state(state);
}
```

## Validation Methodology

### Phase 1: Headless DSP Harness
- Implement known DSP algorithms in ember: gain, pan, delay, biquad filter, oscillator, MIDI synth
- Compare against C++ reference implementations
- **Bit equality** where possible (gain, delay); **numeric tolerance** otherwise (filter: 1e-6 relative)
- **Block partition invariance**: processing 1024 samples as one block vs. varying sub-blocks (1, 16, 64, 128, 256, 512, 1024) gives equivalent output
- Test stereo, mono, and multi-channel

### Phase 2: Realtime Contract Tests
- Instrument allocator (malloc/new), locks (mutex), system calls, and GC
- **Assert zero usage inside process()** — any allocation, lock, or GC during process() is a test failure
- Stress: 32kHz, 44.1kHz, 48kHz, 96kHz, 192kHz; block sizes 1, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
- Variable block sizes (host may change block size between calls)
- Zero frames, silence-only frames, many channels, many events, many automation points
- **Measure worst-case time, not only averages** — fail when processing exceeds block deadline (block_samples / sample_rate * 0.5 for 50% CPU headroom)

### Phase 3: Hot Reload Tests
- Reload continuously while audio runs (1000+ reloads in a soak test)
- Verify: no use-after-free, no deadlocks, no missed blocks, no audio discontinuities beyond crossfade tolerance
- Invalid replacement scripts must leave the prior processor active
- State migration: verify state is preserved across reloads
- Crossfade: verify no clicks/pops during reload

### Phase 4: VST Compliance
- Run **Steinberg VST3 Validator** (from SDK) — automated compliance testing
- Run **Tracktion pluginval** — community validation tool
- DAW smoke tests: REAPER + at least one other host (FL Studio, Ableton, or Bitwig)
- Test: scan/load/unload cycles, state restoration, automation, sample-rate changes, bus changes, offline render, editor open/close

### Phase 5: Fuzz and Soak
- Fuzz ProcessData shapes, event lists, parameter queues, state blobs, reload timing
- Multi-hour soak tests with CPU and memory monitoring
- Differential native-vs-ember processing (same algorithm in C++ and ember, compare outputs)
- NaN/Inf, denormal, runaway-gain, and CPU-budget handling

## Implementation Phases

### Phase 1: Research, SDK Pin, Architecture Document
- [x] Web research on VST3 API (completed — see VST3 SDK License section)
- [x] Verify SDK license: MIT (v3.8.0, October 2025)
- [ ] Vendor VST3 SDK 3.8.0 as `thirdparty/vst3sdk/` git submodule
- [ ] Write architecture document (this file, expanded with C++ class diagrams)
- [ ] Define the ember→VST3 binding API (natives for ProcessContext, buffers, parameters, events)

### Phase 2: Headless Realtime DSP Harness + @realtime Checker
- [ ] Implement `@realtime` sema validation (reject GC, alloc, I/O, locks, threads, exceptions)
- [ ] Build headless DSP test harness (no VST — just call ember process functions with buffers)
- [ ] Implement reference DSP algorithms in C++ and ember
- [ ] Validate bit-exactness and tolerance
- [ ] Validate block partition invariance
- [ ] Profile ember DSP performance (samples/sec, CPU%)

### Phase 3: Minimal VST3 Effect (Stereo f32 Gain)
- [ ] Implement VST3 `IComponent` + `IAudioProcessor` wrapper in C++
- [ ] Load ember script, compile, call process_f32
- [ ] Expose ProcessContext, buffer access, parameter read natives
- [ ] Test in VST3 Validator + REAPER
- [ ] Verify real-time safety (no alloc/lock/GC in process)

### Phase 4: Typed Audio Pipeline + Parameter Automation
- [ ] Design typed audio pipeline model (nodes, edges, preallocated buffers)
- [ ] Compile pipeline graph into immutable ProcessPlan
- [ ] Sample-accurate parameter automation (IParameterChanges → ctx param queue)
- [ ] Test with multi-stage DSP chain (e.g., gain → filter → delay)

### Phase 5: Background Compilation + Atomic Hot Reload
- [ ] Background thread file watcher + compilation
- [ ] Atomic ProcessPlan publish at block boundary
- [ ] Epoch/guard-based old module reclamation (reuse HotReloadDomain)
- [ ] State migration (save_state/load_state)
- [ ] Optional crossfade (configurable length)
- [ ] Failure retention (last known-good)
- [ ] Test: continuous reload while audio runs, no glitches

### Phase 6: MIDI/Instrument, Sidechain, f64, Latency, Presets
- [ ] MIDI/note event input (IEventList → ctx event list)
- [ ] MIDI/note event output (for instruments)
- [ ] Sidechain bus support
- [ ] 64-bit double processing (process_f64)
- [ ] Latency and tail sample reporting
- [ ] State/preset serialization (IBStream)
- [ ] Example instrument plugin (simple synth)

### Phase 7: Validator/pluginval/DAW Test Matrix
- [ ] Run Steinberg VST3 Validator
- [ ] Run Tracktion pluginval
- [ ] DAW smoke tests (REAPER + one other)
- [ ] Stress suite (sample rates, block sizes, channel configs)
- [ ] Fuzz and soak tests

### Phase 8: Example Plugins + Release Packaging
- [ ] Gain plugin (simplest)
- [ ] Delay plugin (uses state, block processing)
- [ ] Biquad filter plugin (float math, parameter smoothing)
- [ ] Simple synth instrument (MIDI input, oscillator, envelope)
- [ ] Hot reload demo (change gain while audio plays)
- [ ] Package as release artifact
- [ ] Documentation: "Writing VST Plugins in ember" guide

### Phase 9: Optional Visual Node Graph Editor
- [ ] GUI tool for constructing audio pipelines visually
- [ ] Export as ember script or pipeline config
- [ ] Separate from plugin runtime (not loaded in VST)
- [ ] Could use Dear ImGui or a web-based editor

## Safety Requirements

- **Buffer validation**: verify channel counts, frame counts, parameter offsets, event bounds before entering JIT code
- **Deterministic fallback**: bypass (output = input), silence, or last-known-good output on any error
- **Trap handling**: ember traps cannot unwind unsafely across VST SDK frames (use setjmp checkpoint)
- **NaN/Inf handling**: detect and sanitize NaN/Inf in audio output
- **Denormal handling**: flush denormals to zero (FTZ/DAZ) to prevent CPU performance issues
- **Runaway gain**: detect and clamp excessive output values
- **CPU budget**: monitor processing time, warn/fallback if exceeding block deadline

## What This Will Reveal About ember

This project will stress-test ember in ways game-engine embedding doesn't:

1. **Float performance** — can ember's SSE emission keep up with 48kHz real-time? (Expected: yes for simple DSP, may need optimization for complex filters)
2. **Array throughput** — processing 256×2 f32 samples per block tests array indexing performance
3. **Real-time safety** — the @realtime checker will reveal what ember features are unsafe for real-time
4. **Hot reload under load** — audio-thread-safe module swap is harder than game tick reload
5. **Memory model** — no GC during process() means ember needs a real-time-safe memory story
6. **Native binding ergonomics** — the VST3 API surface is large; BindingBuilder needs to handle complex types
7. **Error recovery** — what happens when a JIT trap occurs on the audio thread?

## References

- VST3 SDK: https://github.com/steinbergmedia/vst3sdk (MIT, v3.8.0)
- VST3 Developer Portal: https://steinbergmedia.github.io/vst3_dev_portal/
- VST3 Audio Processing: https://deepwiki.com/steinbergmedia/vst3sdk/3.3-audio-processing-pipeline
- VST3 Threading: https://deepwiki.com/steinbergmedia/vst3_public_sdk/2.3-audio-processing-and-threading
- JUCE (alternative framework): https://github.com/juce-framework/JUCE
- ember hot reload: src/hot_reload.hpp (HotReloadDomain, epoch/guard mechanism)
- ember pipeline: examples/ember_cli.cpp (run_pipe_command, ~line 1094)
- ember live: examples/ember_cli.cpp (run_live_command, ~line 1289)
- ember float support: src/thin_emit.cpp (FAdd/FSub/FMul/FDiv, SSE emission)
- ember native binding: src/binding_builder.hpp (BindingBuilder::add)
