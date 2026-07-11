# ember

![calcifer in a prism](./img/calcifer_prism.png)

**A native-JIT embedded scripting language.** Ember compiles scripts to x86-64
machine code at runtime — no interpreter dispatch loop, no bytecode VM. A hot
loop runs as real native instructions, not a switch-on-opcode interpreter, so
it posts **5-9× over a bytecode interpreter** on hot loops (measured against
AngelScript: `fib`, `tight_loop`, `nested_calls`) and ties `g++ -O2` at **1.00×**
on straight-line integer arithmetic. Around that core it ships a real sandbox
(per-frame byte budget, stack-depth guard, recoverable traps — a fault unwinds
to your host, it doesn't kill the process), **hot-reload** of live modules with
stable dispatch slots, **`.em` module bundling** (signed, native-code bundles
with a relocation contract), and a Rust-like syntax built for game-engine /
modding embedding. It is a **baseline** JIT — correct-first, no inlining or
loop opts — so it will *not* match an optimizing compiler everywhere (five of
six codegen paths are 5-9× slower than `g++ -O2`); closing that gap is a
benchmark-gated v2+ goal, not a v1 claim. Current version: **v1.0**.

## What it looks like

A squad of archers fires volleys; tally the hits that land in range — structs,
an enum, a script function, a loop, and a native call (`sqrt`):

```rust
enum Stance { Cautious, Aggressive }

struct Archer {
    stance: i64;
    arrows: i64;
    range: f32;
}

fn volley(a: Archer, targets: i64) -> i64 {
    let mut hits: i64 = 0;
    let mut k: i64 = 0;
    while (k < a.arrows) {
        let d: f32 = sqrt((targets as f32) + (k as f32));
        if (d <= a.range) { hits = hits + 1; }
        k = k + 1;
    }
    return hits;
}

fn main() -> i64 {
    let mut total: i64 = 0;
    let mut i: i64 = 0;
    while (i < 5) {
        let a: Archer = Archer{stance: Stance::Aggressive, arrows: 4, range: 8.0f};
        total = total + volley(a, i * i);
        i = i + 1;
    }
    return total;
}
```

## Getting started

C++17, no external deps beyond `Windows.h`. Builds clean on **MinGW g++ 15.2.0 +
Ninja** (the supported path). MSVC x64 is unsupported — CMake fails loudly
because no working MSVC B1 thunk exists.

```bash
cd ember
mkdir buildt && cd buildt
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build .          # or: ninja
ctest                    # 28 tests
```

Run and bench a script (`ember` = `buildt/ember_cli.exe`):

```bash
ember run hello.ember              # compiles + calls main(); exit code = its i64 return (mod 256)
ember bench hello.ember            # microbenchmark: warmup + N timed iters, prints min/median/mean/p99/stddev
```

CLI reference:

```
ember run <file.ember> [--fn NAME] [--dump] [--emit-em OUT.em] [--tick [--tick-count N] [--tick-interval MS]]
ember emit-em <file.ember> <out.em>      # precompile without running
ember bench <file.ember> [--fn NAME] [--iters N] [--warmup N]
ember run --load-em <file.em> [--fn NAME]
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
so multi-file scripts work. There is no CLI `--load-em` *action* in v1 — loading
is via the host `load_em_file`/`link_em_file` API and the `link "mod.em" as m;`
source directive. A v1 `.em` carries embedded process-local pointers, so it is
ABI/process-trusted, not a portable interchange format.

## Docs

**Spec** (`docs/spec/`)
- `TYPE_SYSTEM.md` — primitives, struct layout, slices, the no-raw-pointer rule, conversion matrix
- `COMPILER_PIPELINE.md` — lexer/BNF/AST, sema passes, the shipped tree-walking lowering, deferred SSA-lite
- `CODEGEN_SPEC.md` — the x86-64 backend: calling convention, encodings, regalloc, traps
- `SAFETY_AND_SANDBOX.md` — threat model, budgets, depth guard, bounds checks, `PERM_FFI`, trap surface, thread-safety
- `BINDING_API.md` — the shipped `BindingBuilder`/`NativeSig` API, Win64 slot mapping, slice convention
- `MEMORY_AND_GC.md` — ownership taxonomy, slice-escape check, globals, the v2-GC deferral rationale
- `BENCHMARK_SYSTEM_DESIGN.md` — the bench harness design, the g++ -O2 axis, methodology

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
queryable via `sizeof`/`offsetof`) and `enum` (C-style untyped, `E::A` rewrites
to an `i64` literal at sema). First-class `fn` handles (`&fn` / `handle(args)`)
backed by the dispatch table.

**Syntax (Rust-like).**
- Bindings: `let x: i64 = 0;`, `let mut x = 0;`, `const N: u64 = sizeof(i64);`
- Functions: `fn name(p: i64) -> i64 { ... }`; default+trailing-optional params
- Control: `if`/`else`, `while`, `for (init; cond; step)`, `do { } while(cond);`,
  `switch`/`case`/`default`/`break`, `continue`, `return`
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
host maps script types to Win64 slots and ships a slice convention. The eight
extensions (`vec`/`quat`/`mat`/`string`/`array`/`math`/`sync`/`lifecycle`)
register their `NativeSig` + `OpOverloadTable` entries the same way. The fluent
`TypeBuilder`/`StructBuilder` surface stays trigger-gated on a host needing
script-visible C++ struct types.
