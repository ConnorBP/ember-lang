# ember

![calcifer in a prism](./img/calcifer_prism.png)

**A native-JIT embedded scripting language.** Ember compiles scripts to x86-64
machine code at runtime — no interpreter dispatch loop, no bytecode VM. A hot
loop runs as real native instructions, not a switch-on-opcode interpreter, so
it posts **6-7x faster than AngelScript's bytecode interpreter** on compute-
heavy workloads (`fib`, `tight_loop`, `nested_calls`) and **within 1.04x of
g++-O2** on integer division (the one path where ember's tree-walker codegen
is already near-optimal). Around that core it ships a real sandbox (per-frame
byte budget, stack-depth guard, recoverable traps — a fault unwinds to your
host, it doesn't kill the process), **hot-reload** of live modules with stable
dispatch slots, **`.em` module bundling** (signed, native-code bundles with a
relocation contract, plus a v5 IR-on-disk variant that is re-emitted to x64 at
load time), a **VST3 audio plugin wrapper** (write VST plugins fully in ember
with hot-reload DSP), and a Rust-like syntax built for game-engine /
modding embedding.

The **default codegen path is a baseline JIT** — correctness-first, tree-
walking, stack-spilling — which is 5-8x slower than `g++ -O2` on most paths
(loop_overhead 5.1x, call_overhead 5.1x, dce_dead_store 7.2x). Closing that gap
is the purpose of the **IR backend + optimization passes + register allocator**,
which ship behind the `--passes` flag (default-off, so the default path is the
unchanged baseline tree-walker). With `--passes`, the optimization passes
provide targeted speedups: **constprop 1.34x**, **int_div 1.31x** (closing to
within 1.30x of g++-O2). The linear-scan register allocator promotes hot
loop-carried frame slots to callee-saved registers, reducing memory traffic in
tight loops.

**16 IR passes shipped** (11 optimization + 5 obfuscation):
- Optimization: constprop, dce, cse, licm, forward, copyprop, instcombine, dse,
  simplifycfg, bounds-elim, sccp
- Obfuscation: subst (MBA), opaque_pred, deadcode, mba_expand, const_encode

String encryption and block splitting obfuscation passes are in development.

Full SSA construction (phi nodes, SSA renaming) remains the future upgrade;
the shipped regalloc is the linear-scan-over-thin-IR subset (assigns scalar
int/bool VRegs to Win64 callee-saved registers, spills to frame slots under
pressure, promotes hot loop-carried frame slots to registers). Current
version: **v1.2**.

## What it looks like

A modular damage calculator that exercises type inference, enums, structs,
for-each over slices, pattern matching, operator overloads, and native calls —
with comments marking where ember's unique properties show:

```rust
// Enums are C-style named constants. The untyped form (`enum E { ... }`)
// has i32 variants auto-incrementing from 0; an explicit `= N` sets the next
// base. The typed form `enum E : T` (e.g. `enum Color : i32`) makes the enum
// name a real type backed by T — see the Language spec below. This example
// uses the untyped form; `E::A` rewrites to an i64 literal at sema time.
enum Damage { Physical, Fire = 10, Ice, Lightning }

// Structs are value types with Ember's tight-packed layout (no C++ alignment).
// Passed by value: ≤8 bytes → one register; >8 bytes → hidden pointer (Win64 ABI).
struct Enemy {
    hp: i64;
    weakness: i64;   // holds a Damage enum value (untyped i32 → i64)
    name: string;     // opaque host handle — string is an extension type
}

// Type inference: `let x = expr` infers x's type from the initializer.
// No need to write `let x: i64 = 0` — ember infers it.
fn compute_damage(base: i64, type: i64) -> i64 {
    // match: pattern matching with no-fallthrough arms (unlike switch).
    // Each arm is a separate branch — no `break` needed.
    // `_` is the wildcard/default arm.
    match (type) {
        Damage::Physical => { return base; },
        Damage::Fire     => { return base * 2; },
        _                => { return base; },
    }
    return 0;
}

// Operator overloads: `string + string` concatenates (registered by the
// string extension via BindingBuilder::add_overload). The `+` here is
// a real native call, not syntax sugar.
fn label(e: Enemy) -> string {
    return e.name + string_from_i64(e.hp);
}

fn main() -> i64 {
    // Array literal — a fixed-size i64[3] allocated on the frame.
    let hps: i64[3] = [100, 200, 50];

    // View: `arr[..]` creates a slice {ptr, len} pointing at the array's
    // backing storage. No copy — the slice is a two-word {pointer, length}.
    let slice: i64[] = hps[..];

    // for-each: iterates a slice, binding each element to `hp`.
    // Desugars to a while loop with index + bounds check.
    // The element type is inferred from the slice's element type.
    let mut total: i64 = 0;
    for (hp in slice) {
        // `as` is the explicit cast operator. Implicit narrowing is rejected
        // by sema (i64 → i32 requires `as i32`; `let y: i8 = x_i64` is an error).
        let d: i64 = compute_damage(hp as i64, Damage::Fire);
        total = total + d;
    }

    // String construction via the string extension's native functions.
    let goblin: Enemy = Enemy { hp: 150, weakness: Damage::Fire, name: string_from_i64(0) };
    let tagged: string = label(goblin);  // operator overload: string + string
    let len: i64 = string_length(tagged); // opaque handle → i64

    // The i64 return becomes the process exit code (8-bit, so >255 wraps).
    return total + len;
}
```

**What this example shows:**
- **Type inference** (`let total = 0`, `let d = compute_damage(...)`) — no redundant type annotations
- **Enums** — untyped (`enum E { ... }`, i32 variants) + typed (`enum E : T`); auto-increment + explicit values (`Damage::Fire = 10`, `Damage::Ice = 11`); variant values may be a `constexpr fn` call
- **Structs** as value types with field access (`e.hp`, `e.name`)
- **for-each** over a slice (`for (hp in slice)`) — no manual index loop
- **match** with no-fallthrough arms + wildcard (`_ => default`)
- **Operator overloads** (`string + string` → concatenation via a registered native)
- **Explicit casts** (`hp as i64`) — no implicit narrowing
- **Opaque host handles** (`string` — an i64 backed by a host-side `std::string`)
- **Slice views** (`arr[..]` — zero-copy {ptr, len} pair)
- **Native calls** (`string_from_i64`, `string_length`, `sqrt` — registered via `BindingBuilder`)

All of this compiles to native x86-64 machine code at runtime — no interpreter, no bytecode.

## Getting started

C++17, no external deps beyond `Windows.h`. Builds clean on **MinGW g++ 15.2.0 +
Ninja** (the supported path). MSVC x64 is unsupported — CMake fails loudly
because no working MSVC B1 thunk exists.

```bash
cd ember
mkdir buildt && cd buildt
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build .          # or: ninja
ctest                    # 65 tests (63 excluding benchmarks + soak)
```

Run and bench a script (`ember` = `buildt/ember_cli.exe`):

```bash
ember run hello.ember              # compiles + calls main(); exit code = its i64 return (mod 256)
ember bench hello.ember            # microbenchmark: warmup + N timed iters, prints min/median/mean/p99/stddev
ember test tests/lang              # native test runner: classify + compile/run every .ember in a directory
```

CLI reference:

```
ember run <file.ember> [--fn NAME] [--dump] [--emit-em OUT.em] [--passes P1,P2,...] [--tick [--tick-count N] [--tick-interval MS]]
ember emit-em <file.ember> <out.em>      # precompile without running
ember bench <file.ember> [--fn NAME] [--iters N] [--warmup N]
ember test [dir]                        # run every .ember file in <dir> (default tests/lang/)
ember run --load-em <file.em> [--fn NAME]
ember bundle <file.ember> <output.exe> [--stub <stub.exe>] [--fn NAME]   # bundle into standalone exe
ember pipe <config>                     # dataflow pipeline runner (Family C): load N .em modules, wire a stage graph, stream i64s through it
ember live <file.ember> [--tick ...]    # live-coding/reload runner: recompile on file change, show tick output evolve
```

- `run` compiles and calls the entry function (default `main`). An `i64` return
  becomes the process exit code (8-bit, so >255 wraps — OS truncation, not a
  bug); `void` exits 0.
- `--fn` overrides the entry. `--dump` prints each compiled fn's slot, byte
  size, and reloc count. `--emit-em` precompiles to a `.em` bundle.
- `--tick` runs `@on_tick` fns + any `register_routine`-registered routines on a
  tick thread (its own `context_t`, isolated from the main thread) at
  `--tick-interval` ms (default 16); `--tick-count N` auto-stops after N ticks.
- `ember_check <file>` (parse-only) and `sema_check <file>` (parse+sema) are the
  one-shot "does this parse/sema-check?" tools the lang regression suite drives.

`import "path";` is textual inclusion before lexing (cycle-detected, deduped),
so multi-file scripts work. The CLI `run --load-em <file.em>` action loads a
pre-compiled `.em` bundle (see the CLI reference above); the host
`load_em_file`/`link_em_file` API and the `link "mod.em" as m;` source
directive are the embedding-side equivalents. A v1 `.em` carries embedded
process-local pointers, so it is ABI/process-trusted, not a portable
interchange format.

## Use cases & quick start

ember is built for **embedding** — you use it as a scripting layer inside a
host application (game engine, audio plugin, tool, pipeline). Pick your use
case and follow the quick start:

### 1. Game engine scripting (hot reload, game logic)

Write game logic in ember, hot-reload it while the game runs:

```bash
# Write a script with @on_tick
ember live examples/scripts/game_logic.ember --tick --tick-interval 16

# Edit the .ember file → ember recompiles on save, no restart needed
```

- **Hot reload:** `docs/HOT_RELOAD.md` — dispatch slots, epoch/quiescence reclamation
- **Lifecycle:** `docs/LIFECYCLE.md` — `@entry`, `@on_tick`, `@event(...)` annotations
- **Example:** `examples/scripts/game_logic.ember` — modular damage calculator
- **Host embedding:** `examples/game_host.cpp` — how to embed ember in a C++ host

### 2. VST3 audio plugins (DSP, hot reload, real-time)

Write VST3 plugins **fully in ember** — the C++ wrapper handles the VST3 API,
you write the DSP:

```bash
# Build the VST3 wrapper plugin
cmake --build buildt -j 8
# Output: buildt/VST3/Release/ember_gain.vst3/

# Write your DSP in ember (see examples/vst3_wrapper/gain_vst.ember)
# @realtime annotation validates real-time safety at compile time
```

- **Plan:** `docs/planning/plan_VST3_EMBER_WRAPPER.md` — full architecture
- **Example plugin:** `examples/vst3_wrapper/gain_vst.ember` — stereo gain
- **DSP harness:** `examples/vst_dsp_harness.cpp` — headless DSP testing
- **Audio natives:** `extensions/audio/` — `load_f32`/`store_f32`/`audio_*`
- **@realtime checker:** rejects GC/alloc/IO/threads/exceptions in RT functions

### 3. Dataflow pipelines (stream processing)

Chain ember modules into a pipeline that processes a stream of values:

```bash
# Run a 3-stage pipeline: A::process -> B::reduce -> C::square
ember pipe tests/features/pipe_3stage.pipe
```

- **Pipeline config:** `.pipe` text format — `module <alias> <path>`, `stage <alias>::<fn>`, `input <start> <count>`
- **Examples:** `tests/features/pipe_2stage.pipe`, `pipe_3stage.pipe`
- **Modules:** `docs/MODULES.md` — `.em` bundling + cross-module linking

### 4. Live coding (iterate on tick-based scripts)

Run a script with `@on_tick` functions, recompile on file change:

```bash
ember live examples/scripts/control.ember --tick --tick-count 10 --tick-interval 100
# Edit control.ember → ember recompiles, prints "reloaded (epoch N)"
```

- **Hot reload mechanics:** `src/hot_reload.hpp` — `HotReloadDomain`, `ExecutionGuard`
- **Example:** `tests/features/live_tick.ember`

### 5. Standalone exe bundling (distribute without ember installed)

Bundle a `.ember` script + the ember runtime into a single `.exe`:

```bash
# Bundle a script into a standalone executable
ember bundle my_script.ember my_app.exe
# Or use the dedicated bundler:
ember_bundle my_script.ember my_app.exe

# Run it anywhere — no ember install needed
./my_app.exe
```

- **Bundling:** `docs/BUNDLING_AND_EM_MODULES.md` — the `.em` format, relocations
- **Stub:** `examples/ember_stub_main.cpp` — the runtime stub
- **Bundler:** `examples/ember_bundle.cpp` — the bundler tool
- **Release packaging:** `scripts/package_release.sh`

### 6. Self-hosted compiler (ember written in ember)

The ember compiler (lexer, parser, sema, codegen) is being ported to ember itself:

```bash
# Run the self-hosted end-to-end pipeline
ember run self_hosted/full_pipeline.ember --fn run_demo --ffi
# Compiles + executes `fn main() -> i64 { return 1 + 2 * 3; }` → 7
```

- **Stages:** `self_hosted/lex.ember`, `parse.ember`, `sema.ember`, `codegen.ember`
- **Compiler entry:** `self_hosted/emberc.ember` — the self-hosted compiler
- **Tests:** `self_hosted/*_test.ember` (wired into ctest)
- **Status:** subset of ember supported (i64/bool/void, let, if/while/for, arithmetic, calls)

### 7. Custom optimization & obfuscation passes

Write your own IR passes — optimization or obfuscation:

```bash
# Run built-in passes
ember run my_script.ember --passes constprop,forward,copyprop,instcombine,dce,licm,dse,simplifycfg,bounds-elim,sccp
# Obfuscation passes
ember run my_script.ember --passes opaque_pred,deadcode,mba_expand,const_encode
```

**16 passes shipped** (11 optimization + 5 obfuscation):
- Optimization: ConstProp, DCE, CSE, LICM, Forward, CopyProp, InstCombine, DSE, SimplifyCFG, bounds-elim, SCCP
- Obfuscation: SubstitutionPass (MBA), opaque_pred, deadcode, mba_expand, const_encode

String encryption (`str_encrypt`) and block splitting (`block_split`) obfuscation
passes are in development — string encryption transforms `ConstStringRef` IR
ops into `StringDecrypt` (XOR-decrypt at runtime), matching ember's built-in
`string_xor_key` feature but as a composable pass.

- **Pass system:** `docs/spec/PASS_SYSTEM_DESIGN.md` — architecture, registration, lifecycle
- **Pass authoring guide:** `docs/PASS_AUTHORING.md` — how to write a custom pass
- **Custom pass examples:** `examples/custom_pass/` — minimal_pass, nop_injection, block_merge + README
- **Optimization research:** `docs/planning/plan_OPTIMIZATION_PASSES.md`
- **Obfuscation research:** `docs/planning/plan_OBFUSCATION_PASSES.md`

## Docs

**Spec** (`docs/spec/`)
- `TYPE_SYSTEM.md` — primitives, struct layout, slices, the no-raw-pointer rule, conversion matrix
- `COMPILER_PIPELINE.md` — lexer/BNF/AST, sema passes, the shipped tree-walking lowering, deferred SSA-lite
- `CODEGEN_SPEC.md` — the x86-64 backend: calling convention, encodings, regalloc, traps
- `SAFETY_AND_SANDBOX.md` — threat model, budgets, depth guard, bounds checks, `PERM_FFI`, trap surface, thread-safety
- `BINDING_API.md` — the shipped `BindingBuilder`/`NativeSig` API, Win64 slot mapping, slice convention
- `MEMORY_AND_GC.md` — ownership taxonomy, slice-escape check, globals, the v2-GC deferral rationale
- `BENCHMARK_SYSTEM_DESIGN.md` — the bench harness design, the g++ -O2 axis, methodology
- `PASS_SYSTEM_DESIGN.md` — the composable pass architecture: extension-style discovery + LLVM-style pass interface, the shipped opt/obf passes
- `CODEGEN_OPTIMIZATION_DESIGN.md` — the codegen optimization design: per-path waste mapping, staged SSA-lite IR plan, pass interface

**Usage**
- `docs/BUNDLING_AND_EM_MODULES.md` — the `.em` binary bundling format: `EMBL` container, relocations, writer/loader
- `docs/MODULES.md` — the module model: `.em` → dispatch table + globals, slot stability across reloads
- `docs/HOT_RELOAD.md` — dispatch table, slot stability, reload protocol, epoch/quiescence reclamation
- `docs/LIFECYCLE.md` — `@entry` / `@on_tick` / `@event(...)` annotation discovery + `register_routine`

**Planning**
- `docs/ROADMAP.md` — every v2+ deferral with a re-entry trigger + dependency, tiered by build order; hard non-goals
- `docs/planning/DESIGN.md` — the index doc: top-level plan, architecture diagram, milestone list
- `docs/planning/GAP_ANALYSIS.md` — the completeness audit: original-request requirements → where satisfied

## Language spec

**Type system.** Primitives `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`,
`bool`, `string` (owned, via the string extension), and `slice[T]` (a bounded
view — the no-raw-pointer rule). User types: `struct` (value type, field layout
queryable via `sizeof`/`offsetof`) and `enum` — **untyped** (`enum E { ... }`,
i32 variants, `E::A` rewrites to an `i64` literal at sema) **or typed**
(`enum E : T`, e.g. `enum Color : i32`, makes the enum name a real type backed
by `T`; enum→int implicit widening is allowed, int→enum is rejected). First-class `fn` handles (`&fn` / `handle(args)`)
backed by the dispatch table.

**Syntax (Rust-like).**
- Bindings: `let x: i64 = 0;`, `let mut x = 0;`, `const N: u64 = sizeof(i64);`
- Functions: `fn name(p: i64) -> i64 { ... }`; default+trailing-optional params;
  `constexpr fn name(...) -> i64 { ... }` — a fn that **can** be const-evaluated
  at sema time when called with all-constant args (the call is rewritten to an
  `IntLit`; bounded interpreter, i64 integer fns only, falls back to a runtime
  call for non-constant args or float/bool/struct fns)
- Compile-time assertions: `static_assert(cond, "msg");` — folds `cond` at
  sema (cond may be a literal expr or a `constexpr fn` call); a false result is
  a compile error with `msg`, a true result is elided (no runtime code)
- Control: `if`/`else`, `while`, `for (init; cond; step)`, `for (x in slice)`
  (and `for (x in array_handle)` via the `iterable()` hook),
  `do { } while(cond);`, `switch`/`case`/`default`/`break`,
  `match (expr) { pat => body, _ => default }`, `continue`, `return`
- `defer` — cleanup runs at **lexical-block exit** (LIFO), including on
  `break`/`continue`/`return` edges
- Operators: `+ - * / %`, comparisons, `&& ||`, `as` casts, `++/--`, `+=` etc.
- F-string lowering through the string extension

**Safety model.** W^X JIT memory; a per-frame byte budget (instructions), a
stack-depth guard, and bounds checks. Every fault — budget exhaustion, stack
overflow, div-by-zero, `INT64_MIN/-1`, OOB — funnels through **one non-local
unwind** to the host: a trap is recoverable, never process death. `PERM_FFI`
gates native calls. v1.0 adds call-target-provenance (the REDSHELL #6 guard) and
context thread-safety (per-thread `context_t`, one compiled body serves N
contexts). Documented gaps are listed in `SAFETY_AND_SANDBOX.md`.

**Modules / `.em`.** `link "mod.em" as m;` resolves a compiled bundle to a
dispatch table + globals block; `m::fn()` cross-module calls go through the real
grammar. `.em` bundles are signed, carry relocations (`DispatchTableBase` /
`GlobalsBase`), and keep slot stability across hot reloads. A v1 `.em` is
ABI/process-trusted (process-local pointers), not a portable format —
cross-process portability is a versioned-relocation v2+ item.

**Binding API.** Register natives with `BindingBuilder` + `NativeSig`; the
host maps script types to Win64 slots and ships a slice convention. The thirteen
`NativeSig` addon extensions (`vec`/`quat`/`mat`/`string`/`array`/`math`/`map`/
`sync`/`thread`/`coroutine`/`lifecycle`/`io`/`call_raw`/`audio`) register their
`NativeSig` + `OpOverloadTable` entries the same way. Two pass extensions
(`opt`, `obf`) are a separate category — they register IR→IR transforms via
`register_passes` (not `register_natives`); see
`docs/spec/PASS_SYSTEM_DESIGN.md` and `docs/PASS_AUTHORING.md`. The fluent `TypeBuilder`/`StructBuilder`
surface stays trigger-gated on a host needing script-visible C++ struct types.

## License

ember is **dual-licensed**:

- **AGPL-3.0** for open-source and community use — if you use ember in a
  project that you distribute or make available as a network service, you must
  open-source your integration under AGPL-3.0. See `LICENSE`.

- **Commercial license** for proprietary/commercial use — if you want to use
  ember without the AGPL copyleft obligation (e.g., in a proprietary product or
  commercial service), you need a commercial license. See `COMMERCIAL_LICENSE.md`
  or contact connor.postma@gmail.com.

This dual-license model ensures ember stays free for open-source development
while allowing sustainable commercial integration. External contributions
require a CLA (Contributor License Agreement) to preserve the ability to offer
commercial licenses.
