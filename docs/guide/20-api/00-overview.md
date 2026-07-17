# API Reference Overview

Ember's language core has no implicit operating-system access. Hosts choose which extensions to register and which permission bits to grant. The standalone `ember_cli` links and registers the addon set below.

## Script-native extensions

| Extension | Script surface | Reference |
|---|---|---|
| `vec` | `vec2`, `vec3`, `vec4` constructors/accessors/operators | [Math and Vectors](40-math-vectors.md) |
| `quat` | quaternion constructor/accessors/Hamilton product | [Math and Vectors](40-math-vectors.md) |
| `mat` | `mat4` construction/access/multiplication | [Math and Vectors](40-math-vectors.md) |
| `string` | conversion, length, indexing, find/substr, `fmt1`…`fmt4`, `+`, `==` | [Strings](30-strings.md) |
| `array` | growable byte/f32/i64 arrays | [Arrays](50-arrays.md) |
| `math` | f32/f64 transcendental, rounding, min/max/clamp, and integer helpers | [Math and Vectors](40-math-vectors.md) |
| `map` | `map_new/set/get/contains/length/remove/clear` for i64 keys/values | `extensions/map/` |
| `sync` | atomics, swap buffers, SPSC/MPSC/MPMC queues | `docs/THREADING.md`, `extensions/sync/` |
| `thread` | `thread_spawn/join/trap_reason` | `docs/THREADING.md` |
| `coroutine` | `coroutine_start/next/done` | `docs/ROADMAP.md`, `extensions/coroutine/` |
| `lifecycle` | dynamic routine registration | `docs/LIFECYCLE.md` |
| `io` | console, whole-file, and path operations | [I/O and Debug](10-io-debug.md) |
| `call_raw` | executable memory, EMBM loading, dispatch/context queries | [`self_hosted/MODULE_IMAGE_FORMAT.md`](../../../self_hosted/MODULE_IMAGE_FORMAT.md) |
| `audio` | audio block, parameter, transport, and event access | [`docs/VST3_PLUGIN_GUIDE.md`](../../VST3_PLUGIN_GUIDE.md), [`extensions/audio/`](../../../extensions/audio/) |
| `graphics` | Win32 windows, D3D11 HLSL shaders, full-screen drawing, clear/present | [Graphics, UI Widgets, and Rendering](60-graphics-ui-render.md), [`extensions/graphics/`](../../../extensions/graphics/) |
| `render` | stub-backed vertex/pixel shaders, layouts, buffers, pipeline binding, draw/clear/present command capture | [Graphics, UI Widgets, and Rendering](60-graphics-ui-render.md), [`extensions/render/`](../../../extensions/render/) |
| `ui` | Dear ImGui windows, custom knob and standard controls, layout, draw-list canvas | [Graphics, UI Widgets, and Rendering](60-graphics-ui-render.md), [`extensions/ui/`](../../../extensions/ui/) |
| `ui_widgets` | retained subtab/panel/control tree for host-owned rendering | [Graphics, UI Widgets, and Rendering](60-graphics-ui-render.md), [`extensions/ui_widgets/`](../../../extensions/ui_widgets/) |
| `visualize` | waveform, radix-2 FFT spectrum, RMS/peak, and LLM-friendly text export | [Graphics, UI Widgets, and Rendering](60-graphics-ui-render.md), [`extensions/visualize/`](../../../extensions/visualize/) |
| `gc` | managed allocation, deletion, collection, and live-count queries | `docs/spec/MEMORY_AND_GC.md` |

`opt` and `obf` are pass extensions rather than script-native tables. `opt`
registers 18 IR optimization passes; `obf` registers 7 required obfuscation
transforms. See `docs/spec/PASS_SYSTEM_DESIGN.md` and `extensions/README.md`.

## Permission model

A native signature can require permission bits. The I/O, audio, graphics,
render, immediate UI, retained UI widgets, visualization/LLM export,
executable/module-loader, and other host-bound
operations use `PERM_FFI`. The CLI grants it with `--ffi`; without it, sema
rejects the call before code generation. Embedding hosts can provide a narrower
registration set even when permission is granted.

Graphics is implemented with Win32/D3D11; non-Windows builds expose a typed
fail-closed stub. Immediate UI requires an active Dear ImGui context (the VST3
editor owns one); outside a frame its calls safely no-op or return their input
values. Retained `ui_widgets` and generic `render` are portable data-model/
command-store extensions and do not render unless a host consumes them.
Visualization analysis and text export belong on the UI/control thread—the
VST3 audio thread only publishes a bounded atomic sample snapshot.

## Types in signatures

Signatures on these pages use Ember source types. Named handles such as `string`, `vec3`, and `mat4` are semantically distinct even when their host ABI is a machine word. Dynamic array/map/synchronization APIs generally use opaque `i64` handles.

Method syntax is sugar: `s.string_length()` becomes `string_length(s)`. Operator overloads are also resolved statically by type.

## Host-specific APIs

The assertion functions described on the Assertions page and names such as `print_str`, `print_f32`, `print_string`, `str_compare`, and `aim_atan2` belong to particular embedding hosts (notably prism), not the standalone standard extension set. Portable standalone scripts should use the functions documented on the other API pages.
