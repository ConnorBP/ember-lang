# ember

![Calcifer in a prism](./img/calcifer_prism.png)

**A native-JIT embedded scripting language for Windows x86-64.** Ember parses,
type-checks, and compiles source directly to native machine code. There is no
bytecode interpreter or opcode-dispatch loop. The runtime includes a sandboxed
call boundary, hot reload, compiled modules, a tracing garbage collector,
concurrent execution, a standalone executable bundler, and a self-hosted Ember
compiler.

**Release status:** **v1.3.0** is the latest tagged release. The
`self-hosting-completion` branch contains post-v1.3.0 work, including completed
self-hosting, and has not received a new release version yet.

**Current verification:**

- **95 CTest tests are configured and the reported current run is 95/95** in
  the supported MinGW configuration.
- **The reported self-hosted parity run is 188/188**: 0 unsupported, 0
  mismatches, and 0 hangs.
- Reported statement/line coverage is approximately **85%+** in the current
  coverage campaign. Coverage varies with the configured optional targets.
- `self_hosted/lex.ember` compiles itself and returns **12** in the reported
  one-stage bootstrap proof.
- In the two-generation proof, compiler A compiles the complete approximately
  12.7k-line self-hosted compiler into compiler B; B then compiles and runs the
  parity programs correctly.

## Highlights

### Language

- Primitive types: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`,
  `f32`, `f64`, `bool`, `void`, and the extension-backed `string` type.
- Value aggregates: packed structs, fixed arrays, slices, aggregate globals,
  struct arguments and returns, destructuring, `sizeof`, and `offsetof`.
- Declarations: mutable and inferred locals, constants, globals, typed and
  untyped enums, namespaces, default parameters, `constexpr fn`, and
  `static_assert`.
- Control flow: `if`/`else`, `while`, C-style `for`, for-each, `do`/`while`,
  `switch`, `match` with guards and struct patterns, `break`, `continue`, and
  `defer`.
- Expressions: explicit casts, ternary expressions, compound assignments,
  prefix/postfix `++` and `--`, f-strings, function handles, and overloads.
- Exceptions: `try`, `catch`, and `throw` with per-context catch stacks.
- Lambdas: no-capture, by-value capture, and GC-backed by-reference capture.
- Coroutines: `yield` plus `coroutine_start`, `coroutine_next`, and
  `coroutine_done`.
- Managed allocation: tracing mark-sweep GC, precise frame/global/extension
  roots, and language-level `new`/`delete` managed pointers.

### Runtime and tooling

- Baseline tree-walking x86-64 JIT plus a ThinIR backend and linear-scan
  register allocator.
- **25 built-in IR passes**: 18 optimization passes and 7 obfuscation passes.
- Stable dispatch tables, atomic single-function reload, epoch/quiescence page
  reclamation, keyed-dispatch reload, and whole keyed-generation replacement.
- Textual source imports and separately compiled `.em` modules with live
  cross-module calls.
- Raw-x86 `.em` compatibility formats, validated IR-on-disk formats, signed
  modules, and keyed-dispatch v6 artifacts. See
  [`docs/BUNDLING_AND_EM_MODULES.md`](docs/BUNDLING_AND_EM_MODULES.md).
- A standalone executable bundler (`ember bundle` and `ember_bundle`).
- **22 extension libraries:** 20 native/addon extensions plus the optimization
  and obfuscation pass packages. The native set includes Win32/D3D11 graphics,
  a stub-backed generic shader/render command API, retained host-rendered UI
  widget trees, Dear ImGui bindings, and audio visualization/LLM export. See
  [`extensions/README.md`](extensions/README.md).
- Concurrent script entry with per-call contexts and a shared, cooperatively
  collected GC heap.
- A Windows graphics extension for Win32 window management, runtime HLSL
  compilation, D3D11 full-screen shader drawing, clear, and present, plus a
  portable stub-backed render extension for vertex/pixel shaders, input
  layouts, vertex/index/constant buffers, pipeline state, and draw capture. The
  animated [`examples/mandelbrot_shader.ember`](examples/mandelbrot_shader.ember)
  demo renders without a vertex buffer by using `SV_VertexID`.
- VST3 wrapper with a raw-HWND Dear ImGui editor, script-defined `on_ui()`
  frames, custom neon knobs, retained host-rendered widget trees,
  waveform/spectrum/meters, LLM-friendly audio-state
  export, 14 example plugin scripts, realtime validation/stress tests, hot
  reload, and an editor-side node-graph model with JSON persistence and
  deterministic Ember source generation.
- A fully self-hosted compiler and one-/two-generation bootstrap proofs.

The built-in pass names are:

- **Optimization (18):** `constprop`, `dce`, `simplifycfg`, `cse`, `gvn`,
  `licm`, `lsr`, `forward`, `copyprop`, `instcombine`, `dse`, `bounds-elim`,
  `sccp`, `unroll`, `spill_elim`, `peephole`, `branch_folding`, `tailcall`.
- **Obfuscation (7):** `subst`, `mba_expand`, `const_encode`, `opaque_pred`,
  `deadcode`, `str_encrypt`, `block_split`.

Full SSA construction with phi nodes remains a possible optimization upgrade;
the shipped allocator operates on ThinIR with conservative live intervals and
loop-carried frame-slot promotion.

## Example

The following example is compiled by the current CLI. It demonstrates enums,
structs, slices, for-each, pattern matching, string overloads, and native calls.

```rs
enum Damage { Physical, Fire = 10, Ice, Lightning }

struct Enemy {
    hp: i64;
    weakness: i64;
    name: string;
}

fn compute_damage(base: i64, kind: i64) -> i64 {
    match (kind) {
        Damage::Physical => { return base; },
        Damage::Fire     => { return base * 2; },
        _                => { return base; },
    }
    return 0;
}

fn label(e: Enemy) -> string {
    return e.name + string_from_i64(e.hp);
}

fn main() -> i64 {
    let hps: i64[3] = [100, 200, 50];
    let values: i64[] = hps[..];
    let mut total = 0;

    for (hp in values) {
        total += compute_damage(hp, Damage::Fire);
    }

    let goblin: Enemy = Enemy {
        hp: 150,
        weakness: Damage::Fire,
        name: string_from_i64(0)
    };
    let tagged = label(goblin);
    return total + string_length(tagged);
}
```

Save it as `damage.ember`, then run:

```bash
./buildt/ember_cli.exe run damage.ember --fn main
```

`run` returns the low byte of the script result as the process exit status on
Windows. Use host calls or printed output when the complete 64-bit value matters.

## Build

### Requirements

- Windows x64
- CMake 3.16 or newer
- Ninja
- MinGW-w64 GCC (the tested configuration is g++ 15.2.0)
- Bash discoverable by CMake, because `lang_suite` uses a Bash test driver
- Git submodules initialized, including the optional VST3 SDK and the
  `ember-imgui` fork

MSVC x64 is intentionally rejected by CMake because the supported raw-call
thunk and generated ABI are the MinGW/Win64 path.

```bash
git submodule update --init --recursive
cmake -S . -B buildt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe
cmake --build buildt -j 8
ctest --test-dir buildt --output-on-failure
```

A fully configured current tree contains **95 tests**. Optional dependency or
platform conditions can change the configured count; use `ctest --test-dir
buildt -N` to inspect the local inventory.

The build vendors Ed25519 source under `thirdparty/ed25519`. Its Git submodules
also include the VST3 SDK and [`ember-imgui`](https://github.com/ConnorBP/ember-imgui),
a custom Dear ImGui v1.91.9b fork shared with the fvc/prism projects. The fork
provides Win32/D3D11 backends, `SimpleKnob`, `FovAngleKnob`, the `retro_neon`
theme (neon palette, `ToggleSwitch`, and CRT overlays), and the
`GNeoButtonBg` custom button-rendering hook. The VST3 target is built when
`thirdparty/vst3sdk` is present and `EMBER_BUILD_VST3=ON` (the default).

## CLI

In the commands below, `ember` means `buildt/ember_cli.exe`.

```text
ember run <file.ember> [--fn NAME] [--dump] [--emit-em OUT.em]
          [--tick [--tick-count N] [--tick-interval MS]]
          [--passes SPEC] [--profile light|balanced|heavy]
          [--pass-seed U64] [--gc-env] [--ffi]
ember emit-em <file.ember> <out.em>
ember run --load-em <file.em> [--fn NAME]
ember bench <file.ember> [--fn NAME] [--iters N] [--warmup N]
ember test [dir]
ember pipe <config.pipe>
ember live <file.ember> [--tick-count N] [--tick-interval MS] [--poll-ms MS]
ember bundle <file.ember> <output.exe> [--stub PATH] [--fn NAME]
             [--permissions none|ffi]
             [--output-permissions stub|preserve]
```

Important options:

- `--ffi` grants `PERM_FFI`, including the standard I/O and module-loader
  natives. `--allow-io` is an alias.
- `--gc-env` enables GC-backed lambda environments and precise GC roots; use it
  for escaping by-reference captures and managed `new`/`delete` workloads.
- `--passes` selects an explicit comma-separated pipeline.
- `--profile` (alias `--pass-profile`) selects `light`, `balanced`, or the
  explicitly experimental `heavy` recipe. `--pass-seed` fixes its root seed.
- `--dump` prints compiled function slots, sizes, and relocation counts.
- `emit-em` and `--emit-em` currently write the standard unsigned `.em`
  artifact used by the CLI and bundler. Loading raw-x86 compatibility artifacts
  is an explicit trusted/development policy in the embedding API.

`import "path.ember";` performs cycle-detected, deduplicated textual inclusion
before lexing. `link "module.em" as m;` keeps a separately compiled module and
allows calls such as `m::process(value)` through the module registry.

## Self-hosting and bootstrap

Self-hosting is complete on this branch. The compiler stages are written in
Ember:

- `self_hosted/lex.ember`
- `self_hosted/parse.ember`
- `self_hosted/sema.ember`
- `self_hosted/codegen.ember`
- `self_hosted/full_pipeline.ember`
- `self_hosted/emberc.ember`

The generated `ember_selfhost_preview.exe` bundles the self-hosted compiler.
It reads a source path from standard input, compiles that source, and executes
its `main` function.

```console
echo self_hosted\preview_smoke_input.ember | buildt\ember_selfhost_preview.exe
```

Bootstrap proofs:

- The one-stage harness compiles `self_hosted/lex.ember` with the self-hosted
  pipeline and checks the expected result, 12. `lex.ember` is a compiler-stage
  library and intentionally has no directly runnable `main`, so invoking it as
  a normal CLI entry is not the bootstrap command.
- The two-generation driver can be run directly. Compiler A emits B; B reads
  the piped test path, compiles it, and returns the test result:

```bash
echo tests/lang/valid_try_catch.ember | \
  ./buildt/ember_cli.exe run self_hosted/bootstrap.ember --fn main --ffi

# Full differential parity audit.
powershell -ExecutionPolicy Bypass \
  -File self_hosted/correctness_tests/run_parity_audit.ps1 \
  -EmberCli buildt/ember_cli.exe
```

The parity result is **188/188**, with no unsupported cases, mismatches, or
hangs. The module image produced by the self-hosted compiler is **EMBM v2**,
which adds a function table and stable dispatch vector to EMBM v1. EMBM is a
separate in-memory bootstrap image format from the public `EMBL`/`.em` file
format. See [`self_hosted/MODULE_IMAGE_FORMAT.md`](self_hosted/MODULE_IMAGE_FORMAT.md).

## VST3 wrapper

Build the default VST3 target with the normal build command. The package is
normally emitted under `buildt/VST3/Release/ember_gain.vst3` in a Release
configuration.

The wrapper provides realtime-safe processing validation, sample-accurate
parameter automation, MIDI, f32/f64 processing, state/preset persistence,
latency/tail reporting, background hot reload, and crossfades. It now also
implements `IPlugView` directly with a Win32 child `HWND`, D3D11, and the ImGui
Win32/DX11 backends; it does not depend on VSTGUI. A timer-driven render loop
calls an optional Ember `fn on_ui() -> void` every frame. UI scripts can use
knobs, sliders, checkboxes, combo boxes, buttons, layout helpers, and a clipped
line/rectangle/text canvas.

The visualization extension transfers a bounded mono output snapshot from the
audio thread through atomics, with no audio-thread lock or FFT. The UI/control
thread derives waveform data, a radix-2 FFT spectrum, RMS, and peak. The same
state is available as LLM-friendly text through `llm_export_state`,
`llm_export_spectrum`, `llm_export_waveform`, and
`llm_export_param_summary`. The 14 scripts in `examples/vst3_wrapper/` include
[`demo_ui_vst.ember`](examples/vst3_wrapper/demo_ui_vst.ember), which combines
four custom knobs, spectrum and waveform canvases, RMS/peak meters, and the
retro-neon theme.

The editor-side node-graph code in `src/node_graph.*` and `src/node_codegen.*`
validates acyclic audio graphs, persists them as JSON, and generates
self-contained Ember VST3 source. It remains a model/code-generation layer;
the shipped in-plugin editor is the script-driven ImGui view, not VSTGUI.

See [`docs/VST3_PLUGIN_GUIDE.md`](docs/VST3_PLUGIN_GUIDE.md),
[`docs/spec/GRAPHICS_AND_UI.md`](docs/spec/GRAPHICS_AND_UI.md), and
[`examples/vst3_wrapper/stress_tests/README.md`](examples/vst3_wrapper/stress_tests/README.md).

## Embedding and runtime model

- `engine_t`-style state owns registered native signatures and overloads.
- A compiled module owns stable dispatch slots and a globals block.
- Each invocation uses a `context_t` for budget, depth, trap, catch, thread, and
  GC state.
- Native functions are registered with `BindingBuilder` and `NativeSig`.
- `PERM_FFI` is checked at source compilation and at module load.
- JIT pages are written RW and sealed RX before publication.
- Recoverable traps use the host checkpoint instead of terminating the process
  when the host follows the documented call contract.

Read these first:

- [`docs/spec/BINDING_API.md`](docs/spec/BINDING_API.md)
- [`docs/spec/SAFETY_AND_SANDBOX.md`](docs/spec/SAFETY_AND_SANDBOX.md)
- [`docs/spec/MEMORY_AND_GC.md`](docs/spec/MEMORY_AND_GC.md)
- [`docs/LIFECYCLE.md`](docs/LIFECYCLE.md)
- [`docs/HOT_RELOAD.md`](docs/HOT_RELOAD.md)

## Documentation index

### User and runtime documentation

- [`docs/guide/00-index.md`](docs/guide/00-index.md) — language guide index
- [`docs/MODULES.md`](docs/MODULES.md) — source imports, live modules, dispatch,
  EMBL `.em`, and EMBM bootstrap images
- [`docs/BUNDLING_AND_EM_MODULES.md`](docs/BUNDLING_AND_EM_MODULES.md) — `.em`
  versions, loader policy, and executable bundling
- [`docs/HOT_RELOAD.md`](docs/HOT_RELOAD.md) — identity and keyed reload
- [`docs/LIFECYCLE.md`](docs/LIFECYCLE.md) — annotations and dynamic routines
- [`docs/PASS_AUTHORING.md`](docs/PASS_AUTHORING.md) — custom IR passes
- [`docs/VST3_PLUGIN_GUIDE.md`](docs/VST3_PLUGIN_GUIDE.md) — VST3 wrapper

### Specifications and project history

- [`docs/spec/TYPE_SYSTEM.md`](docs/spec/TYPE_SYSTEM.md)
- [`docs/spec/COMPILER_PIPELINE.md`](docs/spec/COMPILER_PIPELINE.md)
- [`docs/spec/CODEGEN_SPEC.md`](docs/spec/CODEGEN_SPEC.md)
- [`docs/spec/GRAPHICS_AND_UI.md`](docs/spec/GRAPHICS_AND_UI.md)
- [`docs/spec/PASS_SYSTEM_DESIGN.md`](docs/spec/PASS_SYSTEM_DESIGN.md)
- [`docs/ROADMAP.md`](docs/ROADMAP.md)
- [`docs/RESEARCH_NOTES.md`](docs/RESEARCH_NOTES.md)

## License

Ember is offered under AGPL-3.0 and under separately negotiated commercial
terms. `LICENSE` is the authoritative AGPL text. The commercial overview is in
[`COMMERCIAL_LICENSE.md`](COMMERCIAL_LICENSE.md). Nothing in this README is
legal advice.
