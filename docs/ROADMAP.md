# Ember roadmap

This document records what is shipped, what completed after the latest release,
and what remains relevant. Historical planning documents under `docs/planning/`
explain how features were designed; this roadmap and the implementation are the
authority for current status.

## Current status

- **Latest tagged release:** v1.3.0 (2026-07-14).
- **Current branch:** `self-hosting-completion`, post-v1.3.0 and not yet
  version-bumped.
- **CTest:** 95/95 in a fresh supported MinGW configuration.
- **Self-hosted parity:** 188/188, with 0 unsupported, 0 mismatches, and 0
  hangs.
- **Coverage:** approximately 85%+ in the current coverage campaign.
- **Self-hosting:** complete, including one-stage and two-generation bootstrap.

Ember remains a C-style procedural language: structs, enums, namespaces,
functions, closures, and tagged-data patterns rather than classes,
inheritance, or vtables.

## Completed after v1.3.0

### Self-hosting completion

**Status: complete.** The Ember-written lexer, parser, semantic analyzer, and
x86-64 code generator now cover the language surface used by the parity corpus.
The previous “preview subset” and “milestone reached, expansion ongoing”
descriptions are obsolete.

Shipped proofs:

1. **Full differential parity:** 188/188 tests, 0 unsupported, 0 mismatches,
   0 hangs. The runner is
   `self_hosted/correctness_tests/run_parity_audit.ps1`.
2. **One-stage bootstrap:** `self_hosted/lex.ember` self-compiles and the
   generated program returns the expected value, 12.
3. **Two-generation bootstrap:** the host runs compiler A; A compiles the
   pre-inlined approximately 12.7k-line
   `self_hosted/bootstrap_compiler_source.ember` into EMBM module B; B runs the
   selected tests correctly. The driver is `self_hosted/bootstrap.ember`.
4. **Bundled compiler:** `ember_selfhost_preview.exe` is generated through the
   production executable bundler and passes its CTest smoke test.

The self-hosted compiler supports all integer widths, floats, strings, structs,
arrays, slices, enums, match/switch, all loop forms, defer, exceptions,
lambdas with by-value and by-reference capture, coroutines, function handles,
namespaces, constexpr folding, f-strings, layout queries, assertions, casts,
compound assignment, and increment/decrement operators.

The self-hosted code generator emits **EMBM v2**, whose function table creates a
stable dispatch vector. EMBM is the bootstrap compiler's in-memory module image
format, not a version of the public `EMBL` `.em` file format. See
`../self_hosted/MODULE_IMAGE_FORMAT.md`.

### GC and managed values

**Status: complete for the shipped model.** The tracing mark-sweep collector is
integrated with:

- precise JIT frame root maps;
- typed global roots;
- lambda environments, including by-reference capture and write-through;
- extension-owned array/map/vector/string storage tracing;
- language-level `new` and `delete` managed pointers;
- write-barrier bookkeeping;
- concurrent participants and cooperative stop-the-world collection.

The current collector is non-generational and stop-the-world by design. A
concurrent or generational collector would be a future performance project,
not completion work for the existing GC contract.

### Concurrent execution

**Status: shipped.** Worker calls use independent per-call `context_t` state
while sharing immutable compiled code, dispatch state, globals, and the GC heap.
Budget, catch stack, trap checkpoint, depth, and GC shadow-stack state are
thread-local to the call. The shared collector coordinates participants at
safepoints. Legacy `call_mutex` storage remains for source compatibility but is
not the mechanism for the concurrent-entry path.

### VST3 wrapper, ImGui UI, and node graph

**Status: complete for the shipped wrapper/UI scope.** The tree contains 14
Ember plugin scripts and a VST3 wrapper with f32/f64 processing, parameter
automation, MIDI, presets/state, latency/tail reporting, background hot reload,
state migration, and crossfades. The Phase 9 editor-side graph code validates
and persists acyclic graphs and generates deterministic Ember VST3 source.

The plugin now ships a visual editor without VSTGUI. `EmberVst3Editor`
implements `IPlugView` directly, owns a child Win32 `HWND` plus D3D11 swap
chain, drives Dear ImGui's Win32/DX11 backends from a timer, handles host
resize requests, and calls an optional script `on_ui()` callback each frame.
The `ui` extension exposes knobs, sliders, checkbox/combo/button controls,
layout helpers, and a clipped canvas. `demo_ui_vst.ember` demonstrates four
knobs, custom spectrum/waveform canvases, meters, and the retro-neon theme.

Audio visualization transfers a bounded mono snapshot through atomics; the
audio thread takes no locks and performs no FFT or formatting. The UI thread
provides waveform, radix-2 FFT spectrum, RMS, and peak. LLM-friendly export
functions serialize parameters, spectrum, waveform, RMS, peak, zero-crossing
rate, and source identity for external model workflows.

Sidechain buses and broader DAW/pluginval coverage remain future work. Basic
note-expression events already exist in the extension ABI and wrapper event
translation; richer note-expression parameter/controller behavior remains a
possible expansion.

### Win32/D3D11 graphics extension

**Status: complete.** `extensions/graphics/` provides `PERM_FFI`-gated Win32
window lifecycle, resize/event handling, D3D11 device/swap-chain ownership,
runtime HLSL `vs_main`/`ps_main` compilation, constant-buffer upload,
full-screen `SV_VertexID` triangle rendering, clear/present, and error text.
Opaque generation handles prevent stale window/program reuse, and window
destruction releases dependent shader resources. Non-Windows builds register
the same API through a fail-closed unsupported stub.

`examples/mandelbrot_shader.ember` is the completed reference demo: it compiles
HLSL at runtime and animates a full-screen Mandelbrot zoom with an eight-float
constant block.

### Retained UI widgets and generic shader/render API

**Status: complete.** `extensions/ui_widgets/` ports Prism's retained widget
model: scripts build subtabs, panels, checkboxes, keybinds, integer/double
sliders and ranges, single/multi selections, color pickers, and text inputs.
The extension retains hierarchy, options, values, and pressed state while a
host owns presentation. `extensions/render/` ports the stub-backed
`ShaderStore` pattern for vertex/pixel shaders, input layouts, vertex/index/
constant buffers, pipeline binding, draw accounting, clear, and present.
`TOPO_*` values are injected as `global const i32`; hosts can install a real
backend behind the unchanged command API. Both extensions are portable and
`PERM_FFI`-gated.

### Audio visualization and LLM export

**Status: complete.** `extensions/visualize/` publishes up to 2048 recent output
samples through atomics and performs Hann-windowed radix-2 FFT analysis only on
the UI/control thread. Script natives expose spectrum, waveform, RMS, and peak,
plus compact state/spectrum/waveform/parameter text exports designed for model
input. The exports are observational and do not invoke an AI service or change
parameters.

### Test and coverage expansion

**Status: shipped.** The current 95-test configuration adds direct coverage for
low-level runtime helpers, extensions, the tree-walking code generator, ThinIR
lowering/emission, optimization passes, pass factories, EMBM loading, node
graphs, and keyed hot reload. The parity suite is a separate 188-case
differential gate.

## Shipped language surface

### Types and data

- [x] `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`
- [x] `f32`, `f64`, `bool`, `void`, and extension-backed `string`
- [x] packed value structs, struct literals, field access, by-value arguments
  and returns
- [x] fixed arrays, array literals, indexing, slices, and views
- [x] typed and untyped enums
- [x] aggregate and constant-initialized globals
- [x] managed pointers produced by `new T` and consumed by `delete`

### Declarations and compile-time facilities

- [x] inferred and explicitly typed locals, mutability, constants, globals
- [x] default function parameters
- [x] namespaces
- [x] textual source imports
- [x] `pub`/`priv` module visibility
- [x] `constexpr fn` folding
- [x] `static_assert`
- [x] `sizeof` and `offsetof`
- [x] annotations, including lifecycle and realtime validation annotations

### Statements and expressions

- [x] `if`/`else`, `while`, C-style `for`, for-each, and `do`/`while`
- [x] `switch`/`case`/`default`
- [x] `match`, guards, enum patterns, and struct destructuring
- [x] `break`, `continue`, `return`, and lexical LIFO `defer`
- [x] `try`/`catch`/`throw`
- [x] arithmetic, bitwise, comparison, short-circuit logical operators
- [x] explicit `as` casts and ternary expressions
- [x] compound assignments and prefix/postfix `++`/`--`
- [x] f-strings and registered operator overloads
- [x] typed function handles and cross-module handles
- [x] lambdas with no capture, by-value capture, and by-reference capture
- [x] coroutines and `yield`

## Shipped runtime and tooling

### JIT, IR, and passes

- [x] baseline AST-to-x86-64 tree walker
- [x] ThinIR lowering and x86-64 emission
- [x] linear-scan allocation with spill handling and loop-carried promotion
- [x] composable pass registry, configured factories, transactional pipeline,
  profiles, and deterministic pass seeds
- [x] 18 optimization passes: `constprop`, `dce`, `simplifycfg`, `cse`, `gvn`,
  `licm`, `lsr`, `forward`, `copyprop`, `instcombine`, `dse`, `bounds-elim`,
  `sccp`, `unroll`, `spill_elim`, `peephole`, `branch_folding`, `tailcall`
- [x] 7 obfuscation passes: `subst`, `mba_expand`, `const_encode`,
  `opaque_pred`, `deadcode`, `str_encrypt`, `block_split`
- [x] named `light`, `balanced`, and experimental `heavy` profiles

The remaining compiler optimization opportunity is full SSA construction and
phi-node insertion. It is not required for correctness and should be undertaken
only with benchmark evidence and no regression in the parity/sandbox gates.

### Modules and dispatch

- [x] `import "path.ember";` textual inclusion with canonical-path deduplication
  and cycle termination
- [x] `link "module.em" as m;` live compiled-module linking
- [x] cross-module calls and function handles
- [x] stable logical dispatch slots
- [x] module registry and canonical ABI fingerprints
- [x] environmental keyed dispatch, capability validation, physical padding,
  and keyed cross-module routing

### Public `.em` (`EMBL`) formats

The public file magic is `EMBL`. Supported versions are:

| Version | Purpose | Current load posture |
|---|---|---|
| v1 | historical raw x86, process/ABI trusted | compatibility opt-in |
| v2 | canonical signatures, symbolic natives, rodata relocations, build/ABI identity | compatibility opt-in |
| v3 | v2 plus export-directory visibility | compatibility opt-in |
| v4 | v3 plus Ed25519 content authentication | trusted raw-x86 policy plus verification key |
| v5 | per-function ThinIR or raw-x86 fallback | secure default accepts all-IR; raw/mixed needs opt-in |
| v6 | v5 body model plus versioned keyed-dispatch metadata/capabilities | host must advertise matching v6 capabilities |

The standard CLI writer/bundler currently emits the ordinary unsigned v3
artifact. Dedicated APIs write v5 and v6 artifacts. The loader validates the
complete image before publication and stages executable ownership transactionally.

### Self-hosted module images (`EMBM`)

- [x] EMBM v1: code, rodata, data, symbols, and relocations
- [x] EMBM v2: v1 plus function table and stable dispatch vector
- [x] native ABI fingerprint and permission checks
- [x] W^X loading and owning module handles

### Reload and lifecycle

- [x] annotation discovery for entry, tick, event, and user annotations
- [x] dynamic routine registration through function handles
- [x] unkeyed atomic single-function reload with stable slot and exact signature
  preservation
- [x] epoch guards and quiescent executable-page reclamation
- [x] keyed single-function reload at the derived physical route
- [x] coherent whole keyed-generation replacement under a stable module ID

A general whole-module transaction for the ordinary identity-dispatch path is
still not exposed. Added/removed functions and global-layout migration on that
path remain relevant future work.

### CLI and distribution

- [x] `run`, `bench`, `test`, `emit-em`, `pipe`, and `live`
- [x] `--passes`, named profiles, and deterministic pass seeds
- [x] standalone executable bundling through `ember bundle` and
  `ember_bundle`
- [x] atomic output replacement and output-permission policy
- [x] release packaging scripts
- [x] bundled self-hosted compiler preview

## Remaining roadmap

The following items are still relevant. They are ordered by dependency and
value, not promised release numbers.

### 1. Documentation and release consistency

- [ ] Choose the next version for the post-v1.3.0 self-hosting completion and
  update the CMake project version as part of that release.
- [ ] Keep generated/current status documents distinct from historical audit
  and planning records.
- [ ] Continue converting stale self-hosting audit baselines into explicitly
  historical records rather than current instructions.

### 2. Identity-dispatch whole-module reload

- [ ] Transactionally replace multiple ordinary dispatch entries.
- [ ] Allocate stable slots for added functions and publish unavailable stubs
  for removals.
- [ ] Define global-layout compatibility and migration hooks.
- [ ] Re-resolve late imports without exposing a partially linked generation.

Dependency: reuse the generation/publication invariants already implemented by
keyed whole-generation replacement. Acceptance requires old-or-new coherent
observation, epoch-safe retirement, and failure atomicity.

### 3. Optimizing backend maturation

- [ ] Full SSA rename and phi construction if benchmark evidence warrants it.
- [ ] Complete any remaining aggregate/string/slice ThinIR parity gaps before
  broadening default optimized-path eligibility.
- [ ] Re-evaluate making an optimized pipeline the default only after it is
  value-equivalent and non-regressing on every supported path.

Acceptance: 95/95 CTest, 188/188 self-host parity, no benchmark correctness
mismatches, and measured wins rather than pass-count growth.

### 4. Platform ports

- [ ] Linux x86-64: System V ABI, `mmap`/`mprotect`, executable path handling,
  coroutine backend, and platform tests.
- [ ] macOS x86-64/ARM64: hardened-runtime JIT permissions and Mach-O/platform
  integration.
- [ ] Windows/Linux ARM64: new emitter, ABI lowering, and call thunks.

ThinIR is the intended reusable middle layer, but the current tree walker,
bootstrap emitter, and EMBM images are Win64 x86-64 specific.

### 5. VST3 host and workflow expansion

The original VST3/UI expansion item is **done**: the shipped plugin has a
resizable raw-HWND ImGui/D3D11 editor, script `on_ui()` callback, custom widgets,
canvas visualizations, and realtime-safe sample transfer. A VSTGUI editor is no
longer planned because the direct `IPlugView` implementation supplies the
visual plugin UI without that dependency.

Remaining optional expansions:

- [ ] Connect the node-graph model to an interactive ImGui graph editing view;
  the current in-plugin UI is script-driven and the graph remains a separate
  JSON/code-generation model.
- [ ] Add sidechain buses.
- [ ] Expand note-expression behavior beyond the current event translation.
- [ ] Expand DAW/pluginval validation beyond the existing validator, stress,
  editor-smoke, and soak gates.
- [ ] Add explicit host-to-script parameter writeback helpers so custom script
  controls can participate in host automation/undo with the same contract as
  the editor's default controls.

### 6. Language/API cleanup

- [ ] Remove deprecated `auto x = expr;` after the compatibility window;
  `let x = expr;` is canonical.
- [ ] Consider deprecating unparameterized bare `fn` where a typed
  `fn(Args)->Ret` can be used. Typed handles close the argument-signature hole;
  bare `fn` remains only for compatibility.
- [ ] Generalize for-each iterable support beyond slices and array handles when
  a concrete map/host-collection iterator contract is needed.
- [ ] Add richer host type-builder ergonomics only in response to an embedding
  use case that needs script-visible C++ aggregate types.

## Hard non-goals

- No bytecode interpreter fallback. Native code remains the sole execution
  path; IR-on-disk is re-emitted to native code.
- No `goto`.
- No C preprocessor.
- No class inheritance, vtables, interfaces, mixins, or template-based OOP.
  Ember uses structs, free functions, function handles, enums, and match.
- No cross-process module calls through the in-process module registry.
- No silent reuse of module IDs or dispatch slot identities.

## Version history

### v1.3.0 — 2026-07-14

- Polymorphic pass pipeline and configured factories
- GVN and direct script tail-call optimization
- Implicit environmental keyed dispatch and keyed module artifacts
- Keyed single-function and whole-generation hot reload
- Expanded pass/IR coverage and release packaging

### v1.2.0 — 2026-07-12

- VST3 wrapper phases through the example-plugin and stress/validation work
- Realtime checker, audio natives, parameter automation, hot reload, MIDI,
  state/presets, latency/tail reporting
- Native and self-hosted compiler release packaging

### v1.1.0 — 2026-07-12

- Documentation/pass-authoring and maintenance release following the v1.0
  language/runtime batch

### v1.0 generation

- Full integer/float procedural language foundation
- enums, structs, arrays, slices, match/switch/loop forms
- function references and dynamic lifecycle registration
- context-based safety and concurrency foundations
- `.em` modules, hot reload, standard extensions, and CLI tooling

### Post-v1.3.0, unreleased

- Full self-hosted feature parity and 188-case differential completion
- EMBM v2 dispatch table
- one-stage and two-generation bootstrap completion
- integrated precise GC roots, by-reference captures, and managed allocation
- concurrent shared-context execution
- VST3 node-graph model/source generation
- Win32/D3D11 graphics extension and Mandelbrot shader demo
- custom `ember-imgui` v1.91.9b fork with neon widgets/theme and Win32/DX11 backends
- retained UI widget-tree extension and stub-backed generic shader/render API
- direct-`IPlugView` ImGui VST3 editor with script `on_ui()` rendering
- realtime-safe audio visualization and LLM-friendly state export
- test inventory expanded to 95 passing CTest tests
