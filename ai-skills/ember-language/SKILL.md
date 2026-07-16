---
name: ember-language
description: Write, debug, build, and embed Ember scripts on the self-hosting-completion branch. Use for .ember syntax, CLI commands, native extensions, ThinIR passes, VST3 DSP scripts and node graphs, standalone bundles, or self-hosting work.
---

# Ember language reference (self-hosting-completion)

The latest tag is v1.3.0; this branch is post-v1.3.0 and unreleased. Ember is
a Windows x86-64 native JIT scripting language. The supported build is
MinGW g++ 15.2 + Ninja; MSVC x64 is not supported. The default backend walks the
AST and emits x64. `--passes` or a named profile uses ThinIR, checked pass
execution, register allocation, and x64 emission.

## Build and executable names

```bash
cmake -S . -B buildt -G Ninja \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe
cmake --build buildt -j 8
ctest --test-dir buildt -E bench -LE soak --output-on-failure
```

The binary is `buildt/ember_cli.exe`; documentation often calls it `ember`.

## Core syntax

```ember
struct Pair { a: i64; b: i64; }
enum Kind { A, B = 10, C }
enum Color: i32 { Red, Green, Blue }

constexpr fn twice(x: i64) -> i64 { return x * 2; }
static_assert(twice(3) == 6, "constexpr works");

fn sum(p: Pair, xs: i64[]) -> i64 {
    let mut out: i64 = p.a + p.b;
    for (x in xs) { out += x; }
    defer cleanup();              // defer takes an expression statement, not a block
    return out;
}

fn control(x: i64) -> i64 {
    try {
        match (x) {
            0 => { return 1; },
            _ if x > 0 => { return 2; },
            _ => { throw 9; },
        }
        return 0;                 // trailing return may still be needed after switch/match
    } catch (e: i64) {
        return e;
    }
}
```

### Types

- primitives: `bool`, signed/unsigned `i8`-`i64` / `u8`-`u64`, `f32`, `f64`,
  `void`;
- fixed array: `T[N]`; slice: `T[]` (not `slice[T]`);
- packed value structs and typed/untyped enums;
- extension-backed nominal handles such as `string`, `vec3`, `atomic`, and
  `coroutine`;
- function types: bare `fn` or recorded `fn(i64, f32) -> bool`;
- managed pointer type produced by `new T` and accepted by `delete`.

Integer literals default to `i64`; decimal floating literals default to `f64`;
`1.0f` is f32. Integer width suffixes are rejected—write `42 as u8`. There are
no implicit mixed-type promotions: cast explicitly with `as`.

Locals are immutable unless `mut` is present:

```ember
let x: i64 = 1;
let mut y = 2;
auto z = 3;
const K: i64 = 4;
global counter: i64 = 0;
```

Aggregate globals, array literals, direct struct-literal returns, and aggregate
call arguments are implemented:

```ember
global origin: Pair = Pair { a: 0, b: 0 };
global table: i64[3] = [1, 2, 3];
fn make() -> Pair { return Pair { a: 4, b: 5 }; }
```

### Strings and slices

A literal initially has a bounded `u8[]` representation. In a `string` context
sema requests an owned extension string. Encryption is host-selected (the CLI
uses a nonzero XOR key); both backends decrypt inline into compiler frame
storage. Do not return/store a literal-backed slice—the slice-escape checker
rejects stack-backed escapes. `string_from_slice` copies.

```ember
let bytes: u8[] = "hello";
let owned: string = "hello";
let message: string = f"value={42}";
let raw: u8[] = r"""C:\literal\path""";
```

### Functions, lambdas, and coroutines

```ember
fn add(a: i64, b: i64 = 1) -> i64 { return a + b; }
let h: fn(i64, i64) -> i64 = &add;
let result = h(2, 3);

let base = 10;
let by_value = fn(x: i64) -> i64 { return base + x; };
let mut n = 1;
let by_ref = fn[&n](x: i64) -> i64 { n += x; return n; };

fn generator(n: i64) -> i64 {
    yield n;
    yield n + 1;
    return 0;
}
let c = coroutine_start(&generator, 10); // Windows fiber implementation
let first = coroutine_next(c);
```

Function-handle stack arguments are supported. Use recorded function types
when signatures matter. GC-backed escaping/by-reference closure environments
are enabled by the appropriate host/CLI GC setup (`--gc-env` where required).

### Allocation and safety

```ember
let p = new i64;
delete p;
```

`new` produces a managed pointer and zero-initialized storage; `delete` frees a
live managed object immediately. The runtime also has tracing collection,
precise frame/global/extension roots, bounds checks, call-depth checks, an
instruction budget, and recoverable host checkpoints. FFI-gated calls require
`--ffi` in the CLI.

## Imports and modules

```ember
import "lib/math.ember";    // textual, relative, idempotent, cycle-safe
link "plugin.em" as p;      // compiled module
let x = p::run(4);
```

Keep `import` on its own line; the current resolver does not accept a trailing
`//` comment. `.em` emission rejects functions that still require process-local
representations. A rejection is module-wide, not only entry-reachable.

## CLI

```text
ember run <file.ember> [--fn NAME] [--dump] [--emit-em OUT.em]
          [--tick --tick-count N --tick-interval MS]
          [--passes SPEC] [--profile light|balanced|heavy]
          [--pass-seed U64] [--ffi] [--gc-env]
ember emit-em <file.ember> <out.em>
ember run --load-em <file.em> [--fn NAME]
ember bench <file.ember> [--fn NAME] [--iters N] [--warmup N]
ember test [directory]
ember bundle <input.ember> <output.exe> [--stub PATH] [--fn NAME]
             [--permissions none|ffi] [--output-permissions stub|preserve]
ember pipe <config>
ember live <file.ember> [--tick ...] [--poll-ms MS]
```

`--profile` has alias `--pass-profile`. An explicit `--passes` recipe replaces
the selected profile recipe but retains the profile's configured seed/options.
`heavy` is explicitly experimental. Ordinary run/tick budgets differ by host
path; do not describe one numeric value as a universal language default.

## Extensions (17 libraries)

Fifteen native/addon extensions:

| Extension | Surface |
|---|---|
| `vec`, `quat`, `mat` | nominal math handles, accessors, operators |
| `string` | owned strings, conversion/find/substr/format, concat/equality |
| `array` | host-backed u8/f32/i64 arrays |
| `math` | broad f32/f64 elementary math and min/max/clamp |
| `map` | i64/i64 host map |
| `sync` | atomics, swap buffers, SPSC/MPSC/MPMC |
| `thread` | concurrent in-context spawn/join |
| `coroutine` | Windows fibers and yield; non-Windows stub only |
| `lifecycle` | dynamic routine registration |
| `io` | console/file/path; `PERM_FFI` |
| `call_raw` | raw x64 page API plus EMBM v1/v2 loader; dangerous operations `PERM_FFI` |
| `gc` | tracing GC, closure environments, new/delete substrate |
| `audio` | typed audio context, samples, parameters/events; `PERM_FFI` |

Two pass extensions: `opt` and `obf`. Hosts expose capabilities explicitly by
linking and calling registration/init APIs; linking alone is not discovery.

## Built-in ThinIR passes (25)

Optimization (18):

```text
constprop,dce,simplifycfg,cse,gvn,licm,lsr,forward,copyprop,
instcombine,dse,bounds-elim,sccp,unroll,spill_elim,peephole,
branch_folding,tailcall
```

Obfuscation (7):

```text
subst,mba_expand,const_encode,opaque_pred,deadcode,str_encrypt,block_split
```

Example:

```bash
buildt/ember_cli.exe run script.ember \
  --passes simplifycfg,constprop,gvn,licm,dce,tailcall
buildt/ember_cli.exe run script.ember --profile balanced --pass-seed 42
```

Custom passes operate on `ThinFunction`, register through `EmberPassRegistry`,
return `all()` only when unchanged, use `ThinIRMutation` for transactional
growth, classify effects with `classify_thin_effects`, and should run through
checked validation. See `examples/custom_pass/` and `docs/PASS_AUTHORING.md`.

## VST3 DSP and node graphs

Required callback:

```ember
@realtime
fn process_f32(ctx: i64, frames: i64) -> void { ... }
```

`process` is accepted as a legacy f32 name; `process_f64` is optional. Optional
callbacks are `get_latency() -> i64`, `get_tail() -> i64`,
`save_state() -> i64`, and **`load_state(ptr: i64, len: i64) -> void`**.
`save_state` returns a pointer to the wrapper's two-word state descriptor
contract, not an arbitrary opaque handle.

The wrapper is one stereo VST3 binary (`ember_gain.vst3`) whose script is
selected with `EMBER_VST3_SCRIPT`; it is not 13 separately built libraries.
There are 13 example scripts, but only gain, delay, filter, and oscillator/synth
filenames select specialized host parameter metadata; other scripts currently
receive the generic gain profile unless the wrapper is extended. The component
accepts stereo output and zero or one stereo input; it does not support
sidechains or a VSTGUI custom view.

The editor-side node graph is a C++ model/code generator, not a finished GUI:
`src/node_graph.*` provides strict JSON and validation;
`src/node_codegen.*` generates VST-ready Ember for oscillator, filter, gain,
mixer, and delay nodes. Cycles are rejected. Run `node_graph_test` explicitly
(the target is intentionally not registered with CTest).

## Self-hosting status

Self-hosting is complete on this branch. The Ember lexer/parser/sema/codegen
under `self_hosted/` cover the current parity corpus; the tracked result is
188/188 supported and matching. A two-generation bootstrap is present: compiler
A builds compiler B, and B compiles/runs the parity programs. Do not repeat the
older claim that the self-hosted compiler only supports i64/bool/void.

## Common mistakes

1. Use `i32`/`f32`/`u64`, not C++ `int`/`float`/`uint64_t`.
2. Use `let x: i32 = 5;`, not `int x = 5;`.
3. Use `T[]` for slices, not `slice[T]`.
4. Add `mut` before reassigning a local.
5. Cast mixed/narrow values explicitly with `as`.
6. Nominal handles such as `vec3` or `atomic` are not forgeable/comparable as
   plain i64; do not assume every extension handle has the array extension's
   legacy plain-i64 type.
7. `defer` takes an expression statement, not a brace block.
8. `load_state` in the VST wrapper takes pointer and length.
9. `@realtime` rejects allocation, I/O, threads, exceptions, unknown natives,
   indirect calls, and other operations it cannot prove realtime-safe.
10. Run commands from the repository root so relative imports/scripts resolve.
