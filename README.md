# ember

C-style scripting language, JIT-compiles to native x86-64. AngelScript
ergonomics, an optimizing native-JIT language's speed. Game-engine/modding embedding target.

Status: **v1.0** - the full frontend + binding API + safe execution + live
  modules + lifecycle/hot reload + the concurrency + Tier 2 batch.
  v0.2: lexer/parser/sema/tree-walking codegen, `.em` bundling, standard
  extensions (vec/quat/mat/string/array/math), standalone `ember` CLI +
  language regression suite. v0.3: binding-ABI correctness suite +
  `BindingBuilder` ergonomic registration. v0.4: safe execution — W^X JIT
  memory, per-frame byte budget, `context_t` non-local abort + instruction
  budget + stack-depth guard + PERM_FFI gating + unified trap surface (all
  recoverable, not process death); a red-team writeup ships at the workspace
  root. v0.5: live modules — bidirectional script↔`.em` cross-module linking
  (`link "foo.em" as foo;` + `foo::bar()`) through the real grammar.
  v0.6: lifecycle annotation runtime effect + single-function hot reload +
  the example game-engine integration (`examples/game_host.cpp`) that proved
  embeddability, the float-global-write bug fix, and const global
  initializer evaluation at load (see `docs/v0.6_INTEGRATION_NOTES.md`).
  v1.0: the concurrency + Tier 2 batch (commit e5d1814) — **context
  thread-safety** (Option D + B1: per-thread `context_t`, `r14` context
  register, one compiled body serves N contexts), **enums** (`enum E {...}` +
  `E::A`, rewritten to an `IntLit` at sema), **sync queue primitives**
  (`extensions/sync/`: atomics, swap buffer, SPSC/MPSC/MPMC queues behind
  i64 handles, internally synchronized host storage), and **first-class
  function refs** (`&fn` / `handle(args)` / the `fn` type keyword + the
  REDSHELL guard #6 call-target-provenance invariant). Three follow-on
  commits then shipped **dynamic routine registration**
  (`extensions/lifecycle/`: `register_routine`/`unregister_routine`) — the
  Tier 2 fn-refs feature's host-native half — and wired the CLI `--tick` to
  the B1 per-call context model (the `--tick` thread-safety bug, fixed; the
  CLI is now a B1 host). See `src/` for the
  frontend, `extensions/` for the addons (now eight: +`sync` +`lifecycle`), `examples/`
  for the CLI + check exes + sample scripts, and `docs/v1.0_INTEGRATION_NOTES.md`
  for the batch + follow-on notes.

  Next milestone: **v1.0+** - SSA-lite IR + linear-scan allocation remain
  deferred and benchmark-gated. The shipped backend is the tree-walking,
  stack-spilling emitter (no speculative optimization). The fluent `TypeBuilder`/
  `StructBuilder`/`engine_t` surface stays trigger-gated on a host wanting
  script-visible C++ struct types (the v0.6 integration didn't fire it).

## Docs (read `docs/DESIGN.md` first - it's the index)

- `docs/RESEARCH_NOTES.md` - prior-art survey: binprotect, a prior native-JIT scripting language,
  AngelScript, Mun, and what each contributed.
- `docs/DESIGN.md` - top-level plan, architecture diagram, milestone
  list, links to every detail doc.
- `docs/TYPE_SYSTEM.md` - primitives, struct layout, slices, the
  no-raw-pointer rule, conversion matrix, operator types.
- `docs/COMPILER_PIPELINE.md` - lexer tokens, BNF grammar, AST, sema
  passes, shipped tree-walking lowering, deferred SSA-lite design, error model.
- `docs/CODEGEN_SPEC.md` - the x86-64 backend: calling convention,
  exact instruction encodings, regalloc, label/patch system, call
  emission, trap stubs, v0.1 acceptance criteria.
- `docs/BUNDLING_AND_EM_MODULES.md` - the `.em` binary bundling format:
  the `EMBL` container contract (`em_file.hpp`), relocations
  (DispatchTableBase / GlobalsBase), the writer/loader pair, and the
  module round-trip that acceptance test #7 exercises.
- `docs/BINDING_API.md` - shipped `BindingBuilder`/`NativeSig` API,
  script-type→Win64-slot mapping, slice convention, and clearly deferred
  fluent builder sketches.
- `docs/SAFETY_AND_SANDBOX.md` - threat model, budgets, stack-depth
  guard, bounds checks, `PERM_FFI`, the single non-local-unwind
  primitive, the v1.0 call-target-provenance guard (#6) + context
  thread-safety (§7a/§8a), documented gaps.
- `docs/ROADMAP.md` - every v2+ deferral with a re-entry trigger and
  dependency, tiered by likely build order; hard non-goals listed;
  Tier 1 `enum` / Tier 2 function refs / Tier 5 sync queues + context
  thread-safety all marked ✓ shipped v1.0 with their open items.
- `docs/HOT_RELOAD.md` - dispatch table, slot stability, reload
  protocol, in-flight calls, caller-owned retirement, and the deferred
  concurrent epoch/quiescence requirement.
- `docs/MEMORY_AND_GC.md` - ownership taxonomy, slice-escape check,
  globals, strings, arena (reserved), v2-GC deferral rationale.
- `docs/LIFECYCLE.md` - `@entry`/`@on_tick`/`@event(...)` annotation-based
  discovery, the ember-equivalent of a native-JIT language's `main()` + `register_routine`.
- `docs/MODULES.md` - the module model: how a `.em` bundle maps to a
  dispatch table + globals block, slot stability across reloads, and
  how `EmModule`/`EmFunctionRecord` feed the JIT re-build on load.
- `docs/GAP_ANALYSIS.md` - systematic completeness audit: original-request
  requirements → where satisfied; surveyed-native-JIT-language features → v1 has / deferred / out.
  Includes the honest performance caveat (baseline JIT, not an optimizing native-JIT language's
  optimizing pipeline, but beats AngelScript's bytecode interpreter).
- `docs/ROADMAP.md` - every v2+ deferral with a re-entry trigger and
  dependency, tiered by likely build order; hard non-goals listed.
- `docs/RESTRUCTURE_PLAN.md` - the (fired) plan that promoted prism's
  in-tree ember to this canonical standalone home, so the language
  carries no cheat references and is reusable.

Every feature in the original request (C-style syntax, JIT to native
x86-64, composable chunks via the dispatch table, safety, AngelScript-
style bindings, hot reload, GC deferral) has a detail doc with edge
cases spelled out. `GAP_ANALYSIS.md` is the audit confirming nothing
the original request asked for is missing; `ROADMAP.md` is the tracked
list of what's deliberately deferred and when each comes back. The v1.0
concurrency + Tier 2 batch (context thread-safety, enums, sync queues,
first-class function refs) is documented at `docs/v1.0_INTEGRATION_NOTES.md`;
the four `plan_*.md` files are the historical plans for that batch.

Status: **v1.0** (full frontend + binding API + safe execution + live
modules + lifecycle/hot reload + concurrency + Tier 2). The spec docs are
stable; `src/` contains the implementation, `extensions/` the eight addons
(vec/quat/mat/string/array/math/sync/lifecycle), `examples/` the CLI + check exes +
sample scripts.

## Building

C++17, no external deps beyond `Windows.h` (VirtualAlloc). Builds clean
on MinGW g++ 15.2.0 + Ninja (the supported and tested compiler path). **MSVC
x64 is unsupported; use MinGW g++** (CMake fails loudly because no working
MSVC B1 thunk exists). The build produces three static libs, eight
extension libs (vec/quat/mat/string/array/math/sync/lifecycle), four
round-trip/registration test exes, the standalone
`ember` CLI, and two language-check exes:

```bash
cd ember
mkdir build && cd build
# MinGW g++ + Ninja:
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build .          # or: ninja
ctest                    # currently 20 tests with AngelScript, 19 without
```

The configured CTest suite is the release gate; all discovered targets must
pass. With the AngelScript SDK present this tree currently configures 20 tests
(including the benchmark); without it, only the benchmark target is omitted.
- `em_roundtrip` - JIT→serialize→`load_em_file`→run round-trip on a real
  parsed function (double_it + recursive fib), asserting the loaded
  module matches the JIT'd one.
- `import_roundtrip` - cross-module `.em` round-trip through the
  `ModuleRegistry` (kind-2 registry-base reloc), proving the live-import
  link step.
- `ext_runtime` - a standard-extension native (math `sqrt`) runs through
  the full parse→sema→codegen→JIT→call path.
- `ext_sync` - the v1.0 sync primitives (atomics, swap buffer, SPSC/MPSC/MPMC
  queues behind i64 handles): 16 tests incl. multi-thread stress (10k SPSC,
  MPMC contention, no lost/dup).
- `ext_lifecycle` - the v1.0 dynamic routine registration (`register_routine`/
  `unregister_routine`, the `host_routines()` accessor): register → host calls
  via dispatch → unregister → free-list reuse → reset.
- `ext_registration` - all eight extensions register their `NativeSig` +
  `OpOverloadTable` entries (the binding surface the CLI/sema_check use).
- `v0_4_hardening` - the safe-execution trap surface (budget/depth/bounds
  traps funnel through one longjmp).
- `thread_safety` - the v1.0 context thread-safety keystone (per-thread
  `context_t`, `r14` indirection, no cross-thread longjmp corruption).
- `function_refs` - the v1.0 first-class function refs (handle creation,
  multi-arg dispatch, recursion via handle, the REDSHELL #6 guard).
- `em_loader_hardening`, `em_cli_emit`, `win64_abi`, `v0_5_live_modules`, `binding_abi`,
  `lang_suite`, optional `bench_ember_vs_as`, `v0_6_lifecycle`,
  `v0_6_hot_reload`, `game_host_integration`, `float_global_regression` -
  loader/ABI hardening and the rest of the v0.3–v1.0 suite.
- `lang_suite` - the language regression suite (`tests/run_lang_tests.sh`)
  classifies `tests/lang/{valid,invalid,sema_valid,sema_invalid,import_*}.ember`
  (70 `.ember` files; the v1.0 batch added the four enum tests) against
  `ember_check` (parse-only) and `sema_check` (parse+sema).

## CLI

`ember_cli` is the standalone script runner - prism-decoupled, links only
ember + ember_frontend + the eight extension libs (no prism natives, no VFS,
no backends):

```
ember_cli run <input.ember> [--fn NAME] [--dump] [--emit-em OUTPUT.em]
                              [--tick [--tick-count N] [--tick-interval MS]]
ember_cli emit-em <input.ember> <output.em>
```

- `run` compiles and executes `<file.ember>`'s entry function (default
  `main`). If the entry returns `i64`, the process exits with that code
  (so `main()->i64 { return 42; }` exits 42); if `void`, exits 0. Exit
  codes are 8-bit, so values >255 wrap (OS truncation, not a CLI bug).
- `--fn <name>` overrides the entry (default `main`).
- `--dump` prints each compiled function's slot, byte size, and reloc
  count.
- `emit-em <input.ember> <output.em>` precompiles without running and always
  requires an explicit output. `run ... --emit-em <output.em>` is retained as
  the option form of the same operation; initialized global bytes are emitted.
- `--tick` (v0.6, B1-wired v1.0) runs the module's `@on_tick` fns on a tick
  thread at `--tick-interval <ms>` (default 16, ~60fps) until a keybind is
  pressed; `--tick-count <N>` auto-stops after N ticks (for tests/
  non-interactive). v1.0: the CLI compiles with `use_context_reg = true` and
  the tick thread runs on its own `context_t` via `ember_call_void`, isolated
  from the main thread's `context_t` — a budget/overflow trap in a tick stops
  the tick thread, never the main thread (the `--tick` thread-safety bug,
  fixed; see `docs/v1.0_INTEGRATION_NOTES.md` §1). The CLI also drives any
  routines a script registered via `register_routine(&fn, data)` (the
  dynamic-registration path, `examples/scripts/dynamic_registration.ember`).

There is no CLI `--load-em` action in v1. Loading is through the host
`load_em_file`/`link_em_file` API and the source `link "module.em" as alias;`
directive. A v1 `.em` contains native code and embedded process-local pointers;
it is ABI/process trusted, not a portable or untrusted interchange format.
Cross-process portability requires a versioned symbolic-relocation redesign
(H12), and v1 exports carry no signatures, so `.em` imports are ABI-trusted
unknown signatures (H14).

`import "path";` in a script is resolved as textual inclusion before
lexing (cycle-detected, `seen`-set deduped) - multi-file scripts work.
Sample scripts live in `examples/scripts/` (`fib`, `control`, `struct`,
`string`), each with a `// expect: <value>` the CLI's exit code validates.

The `ember_check` (parse-only) and `sema_check` (parse+sema, core
vec/quat/mat/string/array/math surface registered) exes are the language-regression
tooling the `lang_suite` CTest target drives; they also double as a
quick "does this script parse/sema-check?" one-shot:

```
ember_check <file.ember>     # exit 0 = parse ok, nonzero = parse error
sema_check  <file.ember>     # exit 0 = parse+sema ok, nonzero = sema error
```

## Honest performance caveat

ember is a **baseline** JIT (tree-walking codegen, stack-spilling,
no opt passes, no inlining, no loop opts). AngelScript is a bytecode
interpreter; even baseline native code beats a bytecode interpreter on
tight loops by typically 5-50×, which comfortably satisfies "much faster"
for hot game-logic — the v0.6 benchmark harness (`examples/bench_ember_vs_as.cpp`,
ctest target `bench_ember_vs_as`) recorded Ember/AngelScript ratios of
**0.15** for `fib(32)`, **0.16** for `tight_loop(1e8)`, **0.18** for
`nested_calls(1e7)`, and **0.55** for mandelbrot (results in
`v0.6_BENCHMARK_RESULTS.md`). Available provenance: Windows x86-64, MinGW g++
15.2.0, Release `-O3 -DNDEBUG`; results were committed before the 2026-07-09
audit (exact CPU and run date were not recorded). These are compile-once hot-path
means with safety checks disabled, not cross-machine statistical claims. Matching an optimizing
native-JIT language's speed is a v2+ goal, gated on a benchmark-proven
need — the SSA-lite IR + linear-scan allocation design
(`COMPILER_PIPELINE.md` Section 5) is explicitly deferred until stronger
benchmarks show where Ember is slow.
No speculative optimization (`DESIGN.md` Section 9). The codegen is
correctness-first today.
