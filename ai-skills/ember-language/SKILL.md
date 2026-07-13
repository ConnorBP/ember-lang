---
name: ember-language
description: Write and debug ember scripts (.ember files) for the ember JIT scripting language. Ember is a C-style language that compiles to native x86-64 (not bytecode) via a tree-walking codegen or an optimizing three-address IR backend with 23 passes. Use when writing .ember scripts, porting AngelScript/ENMA code to ember, debugging ember compile/runtime errors, writing VST3 audio plugins in ember, or authoring custom IR passes.
---

# Ember Language — JIT-compiled C-style scripting (v1.2+)

Ember compiles `.ember` source directly to native x86-64 machine code at runtime
— no interpreter dispatch loop, no bytecode VM. A hot loop runs as real native
instructions. Runs **6-7x faster than AngelScript** on compute-heavy workloads
and **within 1.04x of g++-O2** on integer division. Designed for game engine,
modding, audio plugin, and pipeline embedding.

## Two codegen backends

- **Tree-walker (default):** correctness-first, stack-spilling. The baseline JIT.
  Always available; default-off for `--passes`.
- **ThinIR backend (`--passes`):** three-address IR with linear-scan register
  allocation + 23 optimization/obfuscation passes. Promotes hot loop-carried
  frame slots to callee-saved registers.

## Type system — DO NOT use C++ types

Ember has its own type names. C++ types are a compile error.

| Ember type | C++ equivalent | Notes |
|---|---|---|
| `i8` `i16` `i32` `i64` | `int8_t` ... `int64_t` | Signed integers |
| `u8` `u16` `u32` `u64` | `uint8_t` ... `uint64_t` | Unsigned integers |
| `f32` `f64` | `float` / `double` | IEEE-754 |
| `bool` | `bool` | `true` / `false` |
| `void` | `void` | Return type only |
| `string` | — | Owned string via the string extension (opaque host handle) |
| `slice[T]` | — | Bounded view {ptr, len} — the no-raw-pointer rule |

**Common mistakes:**
- ❌ `int x = 5;` → ✅ `let x : i32 = 5;`
- ❌ `float y = 1.5f;` → ✅ `let y : f32 = 1.5f;`
- ❌ `uint64_t addr;` → ✅ `let addr : u64;`
- ✅ `auto x = 5;` (ember HAS auto — infers i64)
- ✅ `let s = "hi";` (string literals are `slice<u8>`; `string` is an owned extension type)

## Syntax basics

### Variable declaration
```
let x : i64 = 42;          // explicit type
let mut y = 3.14f;         // mut + type inference (infers f32)
auto z = 5;                // infers i64
const Z : i64 = 100;       // immutable
```

### Functions
```
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn void_fn() {             // void return (no -> prefix needed)
    print_i64(42);
}

constexpr fn fib(n: i64) -> i64 {   // constexpr fn — const-evaluated at sema
    if (n < 2) { return n; }        // time when called with all-constant args
    return fib(n - 1) + fib(n - 2); // (i64 integer fns only; falls back to
}                                  //  runtime call for non-constant args)
```

### Control flow
```
if (x > 0) { ... }
else if (x == 0) { ... }
else { ... }

while (i < 10) { i += 1; }

for (let i : i32 = 0; i < 10; i += 1) { ... }

for (hp in slice) { total += hp; }    // for-each over a slice or array handle

do { i += 1; } while (i < 10);

switch (x) {
    case 1: { ... break; }
    default: { ... break; }
}

// match: pattern matching with no-fallthrough arms (no break needed)
match (type) {
    Damage::Fire     => { return base * 2; },
    Damage::Physical => { return base; },
    _                => { return base; },   // _ = wildcard/default
}
```

### Enums (untyped + typed)
```
enum Damage { Physical, Fire = 10, Ice, Lightning }  // untyped: i32 variants
//   Damage::Fire rewrites to i64 literal 10 at sema time
//   Damage::Ice auto-increments to 11

enum Color : i32 { Red, Green, Blue }  // typed: Color is a real type backed by i32
//   enum→int implicit widening allowed; int→enum is rejected
```

### Structs (POD value types, tight-packed layout)
```
struct Pair { a: i64; b: i64; }
fn sum(p: Pair) -> i64 { return p.a + p.b; }
let x : Pair = Pair { a: 10, b: 20 };

// struct destructure:
let Pair { a, b } = x;    // binds a = 10, b = 20
```

### Namespaces
```
namespace cs2 {
    fn process(addr: u64) -> u64 { ... }
}
// call: cs2::process(0x1234)
```

### Defer (lexical-block-exit cleanup, LIFO)
```
fn example() -> i64 {
    let buf = array_new(1, 256);
    defer { array_free(buf); }    // runs at block exit (incl. return/break)
    // ... use buf ...
    return 0;                      // defer runs here
}
```

### Try / catch / throw (exceptions)
```
fn risky() -> i64 {
    try {
        if (bad) { throw 42; }
        return 1;
    } catch (e: i64) {
        return e;                  // e = thrown value
    }
}
```

### Coroutines / yield
```
fn generator() -> i64 {
    yield 1;
    yield 2;
    yield 3;
    return 0;    // done
}
// driven by the coroutine extension (Windows fibers)
```

### Lambdas (with capture)
```
fn make_adder(n: i64) -> i64 {
    let add = fn(x: i64) -> i64 { return x + n; };  // captures n by value
    return add(10);
}
// by-ref capture via the GC-backed lambda env (use_gc_env flag)
```

### Static assertions
```
static_assert(sizeof(i64) == 8, "i64 must be 8 bytes");
static_assert(MY_CONST == 42, "config mismatch");
// cond is folded at sema time (may be a constexpr fn call)
// false = compile error with msg; true = elided (no runtime code)
```

### Type conversions (explicit `as`, never implicit)
```
let i : i64 = 42;
let f : f32 = i as f32;     // explicit cast
let n : i32 = i as i32;     // narrowing (truncates)
let u : u64 = i as u64;     // signed→unsigned (explicit)

// ❌ implicit conversion is an ERROR:
// let f : f32 = i;          // compile error — must use `as`
```

### Operators
```
+  -  *  /  %          // arithmetic (same-type operands required)
&  |  ^  ~  <<  >>     // bitwise (integers only)
== != < <= > >=        // comparison (same-type required, produces bool)
&& || !                // logical (bool operands only, short-circuit)
+= -= *= /= %=         // compound assign
++ --                  // prefix and postfix (integers only)
?  :                   // ternary (both arms must be same type)
```

**No implicit promotion** — `i32 + i64` is a compile error. Both operands
must be the same type. Use `as` to convert one.

### Literals
```
42            // i64 (default integer type)
42u           // u64 (suffix)
0xFF          // hex (i64)
3.14f         // f32 (f suffix forces float)
3.14          // f64 (default float type)
"hello"       // slice<u8> (string literal)
true false    // bool
```

### Globals
```
global g_count : i64 = 0;
global g_factor : f32 = 1.5f;
const MAX : i32 = 100;
```

### Annotations
```
@entry
fn main() -> i64 { return 1; }

@on_tick
fn render() { ... }

@obf("mba")       // MBA arithmetic obfuscation
@obf_keyed        // CPUID-keyed entry gate
fn sensitive() { ... }

@realtime         // real-time safety validation (rejects GC/alloc/IO/threads)
fn dsp_process() { ... }   // for VST3 audio plugins
```

### Imports (textual include, not a C preprocessor)
```
import "lib/cs2.ember";
import "../features/bomb_timer.ember";
```
- Idempotent (a file imported twice inlines once)
- Cycle-safe
- Resolves relative to the importing file's directory

### Cross-module linking
```
link "mod.em" as m;        // load a pre-compiled .em bundle
let v : i64 = m::do_thing(42);   // cross-module call through dispatch table
```

### Method-call sugar
```
let buf : i64 = array_new(1, 64);
buf.array_set_u8(0, 0xFF);     // desugars to array_set_u8(buf, 0, 0xFF)
let n : i64 = buf.array_length();
```
`obj.method(args)` → `method(obj, args)` — receiver becomes arg[0].

### Slices + fixed arrays
```
let s = "hello";              // slice<u8> (rodata, immutable)
let arr : i64[8] = 0;         // fixed array (8 i64's, zero-init)
let view : i64[] = arr[..];   // whole-array view as slice {ptr, len}
```

### Operator overloads (for registered handle types)
vec3 is an opaque `i64` handle with registered overloads:
```
let a : vec3 = vec3_new(1.0f, 2.0f, 3.0f);
let c : vec3 = a + b;           // auto-dispatches to vec3_add(a, b)
let eq : bool = (a == a);
```

## String encryption (default-on)
All string literals are XOR-encrypted in the compiled rodata. Raw strings
never appear in the executable memory. The codegen emits a decrypt call at
each use site. No script-side action needed. The `str_encrypt` pass can also
apply this as a composable IR transform.

## Safety model + failsafes

Ember ships a real sandbox with recoverable traps:
- **Per-frame instruction budget** (default 100M) — budget exhaustion → trap
- **Stack-depth guard** (call-depth limit, default 512) — stack overflow → trap
- **Bounds checks** — slice/array OOB → trap
- **Recoverable traps** — every fault funnels through ONE non-local unwind to
  the host. A trap is recoverable, never process death.
- **`PERM_FFI`** gates native calls (FFI must be explicitly enabled)
- **RSS memory cap** (2 GiB default) — unbounded allocation → instant-fail
  (abort) to prevent host freeze
- **Compiler recursion depth guards** — deep ASTs → clean compile error
  (not a C++ stack overflow crash)
- **Wall-clock deadlines** on all harnesses (test runner 120s, bench 60s, VST3
  process 5s) — runaway loops are killed before they freeze the host

## Optimization + obfuscation passes (23 total)

Use `--passes P1,P2,...` to enable the ThinIR backend + passes:

```
ember run my_script.ember --passes constprop,forward,copyprop,instcombine,dce,licm,dse,simplifycfg,bounds-elim,sccp,unroll,spill_elim,peephole,lsr
```

**16 optimization passes:** constprop, dce, cse, licm, forward, copyprop,
instcombine, dse, simplifycfg, bounds-elim, sccp, unroll, spill_elim,
peephole, branch_folding, lsr

**7 obfuscation passes:** subst (MBA), opaque_pred, deadcode, mba_expand,
const_encode, str_encrypt, block_split

Write your own passes — see `examples/custom_pass/` and `docs/PASS_AUTHORING.md`.

## CLI reference

```
ember run <file.ember> [--fn NAME] [--dump] [--emit-em OUT.em] [--passes P1,P2,...] [--tick ...]
ember bench <file.ember> [--fn NAME] [--iters N] [--warmup N]
ember test [dir]                        # run every .ember file in <dir>
ember run --load-em <file.em> [--fn NAME]   # run a pre-compiled .em bundle
ember bundle <file.ember> <output.exe> [--fn NAME]   # bundle into standalone exe
ember pipe <config.pipe>                # dataflow pipeline: load N .em modules, stream i64s
ember live <file.ember> [--tick ...]    # live-coding: recompile on file change
```

- `run` compiles and calls the entry function (default `main`). An `i64` return
  becomes the process exit code (8-bit, so >255 wraps — OS truncation).
- `--tick` runs `@on_tick` fns on a tick thread at `--tick-interval` ms.
- `--passes` enables the ThinIR backend + optimization/obfuscation passes.
- `ember_check <file>` (parse-only) and `sema_check <file>` (parse+sema) are
  one-shot check tools.

## VST3 audio plugins

Write VST3 plugins **fully in ember** — the C++ wrapper handles the VST3 API,
you write the DSP. `@realtime` annotation validates real-time safety at compile
time (rejects GC/alloc/IO/threads/exceptions in RT functions).

```bash
# Build the VST3 wrapper plugin
cmake --build buildt -j 8
# Output: buildt/VST3/Release/ember_gain.vst3/

# 13 example plugins: gain, delay, filter, oscillator, synth,
# distortion, panner, tremolo, compressor, chorus, bitcrusher, limiter, reverb
```

Audio natives: `load_f32`/`store_f32`/`load_f64`/`store_f64`/`load_i32`/`store_i32`
+ `audio_get_sample_rate`/`audio_get_parameter`/`audio_get_event_*`/etc.

## Available extensions (15 addon + 2 pass)

| Extension | Provides |
|---|---|
| `vec` | vec3/vec4 types + operator overloads |
| `quat` | quaternion type + operations |
| `mat` | matrix types + operations |
| `string` | owned `string` type, concat, format, from_i64, length, etc. |
| `array` | host-backed arrays (opaque i64 handle) |
| `math` | sqrt, sin, cos, tan, atan, atan2, exp, log, min, max, clamp, etc. |
| `map` | hash map (opaque i64 handle) |
| `sync` | MPSC/MPMC queues, swap buffers |
| `thread` | in-context threads, thread_join |
| `coroutine` | coroutines via Windows fibers, yield/resume |
| `lifecycle` | @entry/@on_tick/@event annotation discovery, register_routine |
| `io` | console + file + path I/O |
| `call_raw` | raw native function calls |
| `gc` | tracing mark-sweep GC (for lambda env, by-ref capture, new/delete) |
| `audio` | VST3 audio buffer natives + AudioContext |
| `opt` | 16 optimization passes (registered via register_passes) |
| `obf` | 7 obfuscation passes (registered via register_passes) |

## Common syntax mistakes to avoid

1. **C-style type names** — use `i32` not `int`, `f32` not `float`, `u64` not `uint64_t`
2. **C-style declaration** — use `let x : i32 = 5;` not `int x = 5;`
3. **Implicit promotion** — `i32 + f32` is a compile error; cast explicitly with `as`
4. **`int` as bool** — `if (x)` is an error when `x` is `i32`; write `if (x != 0)`
5. **Mixed-type arithmetic** — `i32 + i64` is an error; both must be same type
6. **Signed index on array/slice** — `arr[i]` requires `i` to be unsigned; use `as u64`
7. **`cast<T>(x)`** — this is AngelScript syntax; ember uses `x as T`
8. **`#include`** — ember uses `import "path";` (not a C preprocessor)
9. **`nullptr`/`NULL`** — use `0` (handles are `i64`; `0` = null/invalid)
10. **`NULL` checks on handles** — use `if (handle == 0)` not `if (handle == null)`
11. **`string` keyword** — `string` IS now a type (owned, via the string extension);
    string literals are `slice<u8>`, use `string_from_i64` etc. to get a `string`

## Running scripts
```
# CLI runner (compiles + JITs + executes):
ember_cli run scripts/entry/main.ember

# With a specific entry function:
ember_cli run script.ember --fn my_function

# With optimization passes:
ember_cli run script.ember --passes constprop,forward,copyprop,instcombine,dce,licm,dse

# Dump JIT'd bytes for debugging:
ember_cli run script.ember --dump

# Microbenchmark:
ember_cli bench script.ember --iters 10000

# Native test runner:
ember_cli test tests/lang

# Bundle into standalone exe:
ember_cli bundle my_script.ember my_app.exe
```

## Build

C++17, MinGW g++ 15.2.0 + Ninja, Windows-first. MSVC x64 unsupported.

```bash
cd ember
mkdir buildt && cd buildt
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build . -j 8
ctest -E bench -LE soak --timeout 60    # 64 tests
```

## Self-hosted compiler

The ember compiler (lexer, parser, sema, codegen) is being ported to ember itself.
Entry point: `self_hosted/emberc.ember`. Currently supports a subset of ember
(i64/bool/void, let, if/while/for, arithmetic, calls). Out-of-subset constructs
are rejected by the self-hosted sema.

## License

Dual-licensed: **AGPL-3.0** (open source) or **commercial license** (proprietary
use). See `COMMERCIAL_LICENSE.md`.
