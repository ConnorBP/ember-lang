# ember

C-style scripting language, JIT-compiles to native x86-64. AngelScript
ergonomics, an optimizing native-JIT language's speed. Game-engine/modding embedding target.

Status: **v0.5** - the full frontend + binding API + safe execution + live modules.
  v0.2: lexer/parser/sema/tree-walking codegen, `.em` bundling, standard
  extensions (vec/quat/mat/string/array/math), standalone `ember` CLI +
  language regression suite. v0.3: binding-ABI correctness suite +
  `BindingBuilder` ergonomic registration. v0.4: safe execution — W^X JIT
  memory, per-frame byte budget, `context_t` non-local abort + instruction
  budget + stack-depth guard + PERM_FFI gating + unified trap surface (all
  recoverable, not process death); a red-team writeup ships at the workspace
  root. v0.5: live modules — bidirectional script↔`.em` cross-module linking
  (`link "foo.em" as foo;` + `foo::bar()`) through the real grammar.
  Script-to-script calls run through the dispatch table by slot; the `.em`
  binary bundling format (serialize→load→run round-trip) and cross-module
  textual `import "path";` inclusion both work. The canonical tree is
  standalone-buildable and standalone-testable. See `src/` for the frontend,
  `extensions/` for the addons, `examples/` for the CLI + check exes +
  sample scripts.

  Next milestone: **v0.6** - benchmark harness vs AngelScript (the gate the
  SSA-lite IR + linear-scan regalloc refactor is deferred to). Open v0.4
  items (lifecycle annotation runtime effect, single-function hot reload)
  have complete specs (`LIFECYCLE.md`, `HOT_RELOAD.md`) and may land first.

Next milestone: **v0.3** - native binding API correctness + host↔script
calling-convention validation (struct/annotation machinery already landed in
the v0.2 overshot; v0.3 is the binding-correctness milestone).

## Docs (read `docs/DESIGN.md` first - it's the index)

- `docs/RESEARCH_NOTES.md` - prior-art survey: binprotect, a prior native-JIT scripting language,
  AngelScript, Mun, and what each contributed.
- `docs/DESIGN.md` - top-level plan, architecture diagram, milestone
  list, links to every detail doc.
- `docs/TYPE_SYSTEM.md` - primitives, struct layout, slices, the
  no-raw-pointer rule, conversion matrix, operator types.
- `docs/COMPILER_PIPELINE.md` - lexer tokens, BNF grammar, AST, sema
  passes, SSA-lite IR, lowering rules, error model.
- `docs/CODEGEN_SPEC.md` - the x86-64 backend: calling convention,
  exact instruction encodings, regalloc, label/patch system, call
  emission, trap stubs, v0.1 acceptance criteria.
- `docs/BUNDLING_AND_EM_MODULES.md` - the `.em` binary bundling format:
  the `EMBL` container contract (`em_file.hpp`), relocations
  (DispatchTableBase / GlobalsBase), the writer/loader pair, and the
  module round-trip that acceptance test #7 exercises.
- `docs/BINDING_API.md` - `NativeFn`/`TypeBuilder`/`StructBuilder`,
  script-type→Win64-slot mapping, slice convention.
- `docs/SAFETY_AND_SANDBOX.md` - threat model, budgets, stack-depth
  guard, bounds checks, `PERM_FFI`, the single non-local-unwind
  primitive, documented gaps.
- `docs/HOT_RELOAD.md` - dispatch table, slot stability, reload
  protocol, in-flight calls, epoch reclamation.
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
list of what's deliberately deferred and when each comes back.

Status: **v0.5** (full frontend + binding API + safe execution + live modules).
standalone CLI + language regression suite). The spec docs are stable;
`src/` contains the implementation, `extensions/` the addons, `examples/`
the CLI + check exes + sample scripts.

## Building v0.2

C++17, no external deps beyond `Windows.h` (VirtualAlloc). Builds clean
on MinGW g++ 15.2.0 + Ninja (the config this tree is tested on); MSVC
VS 2022 also supported. The build produces three static libs, six
extension libs, four round-trip/registration test exes, the standalone
`ember` CLI, and two language-check exes:

```bash
cd ember
mkdir build && cd build
# MinGW g++ + Ninja:
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++..exe ..
cmake --build .          # or: ninja
ctest                    # 5 targets: em_roundtrip, import_roundtrip,
                         # ext_runtime, ext_registration, lang_suite
```

`ctest` is 5 targets and all must pass:
- `em_roundtrip` - JIT→serialize→`load_em_file`→run round-trip on a real
  parsed function (double_it + recursive fib), asserting the loaded
  module matches the JIT'd one.
- `import_roundtrip` - cross-module `.em` round-trip through the
  `ModuleRegistry` (kind-2 registry-base reloc), proving the live-import
  link step.
- `ext_runtime` - a standard-extension native (math `sqrt`) runs through
  the full parse→sema→codegen→JIT→call path.
- `ext_registration` - all six extensions register their `NativeSig` +
  `OpOverloadTable` entries (the binding surface the CLI/sema_check use).
- `lang_suite` - the language regression suite (`tests/run_lang_tests.sh`)
  classifies `tests/lang/{valid,invalid,sema_valid,sema_invalid,import_*}.ember`
  against `ember_check` (parse-only) and `sema_check` (parse+sema) - 110
  pass / 0 fail / 1 skip (the skip is a prism-native-surface mismatch,
  documented in the runner output).

## CLI

`ember_cli` is the standalone script runner - prism-decoupled, links only
ember + ember_frontend + the six extension libs (no prism natives, no VFS,
no backends):

```
ember_cli run <file.ember> [--fn <name>] [--dump]
```

- `run` compiles and executes `<file.ember>`'s entry function (default
  `main`). If the entry returns `i64`, the process exits with that code
  (so `main()->i64 { return 42; }` exits 42); if `void`, exits 0. Exit
  codes are 8-bit, so values >255 wrap (OS truncation, not a CLI bug).
- `--fn <name>` overrides the entry (default `main`).
- `--dump` prints each compiled function's slot, byte size, and reloc
  count.

`import "path";` in a script is resolved as textual inclusion before
lexing (cycle-detected, `seen`-set deduped) - multi-file scripts work.
Sample scripts live in `examples/scripts/` (`fib`, `control`, `struct`,
`string`), each with a `// expect: <value>` the CLI's exit code validates.

The `ember_check` (parse-only) and `sema_check` (parse+sema, full
six-extension surface registered) exes are the language-regression
tooling the `lang_suite` CTest target drives; they also double as a
quick "does this script parse/sema-check?" one-shot:

```
ember_check <file.ember>     # exit 0 = parse ok, nonzero = parse error
sema_check  <file.ember>     # exit 0 = parse+sema ok, nonzero = sema error
```

## Honest performance caveat

ember v0.2 is a **baseline** JIT (tree-walking codegen, stack-spilling,
no opt passes, no inlining, no loop opts). AngelScript is a bytecode
interpreter; even baseline native code beats a bytecode interpreter on
tight loops by typically 5-50×, which comfortably satisfies "much faster"
for hot game-logic. Matching an optimizing native-JIT language's speed is
a v2+ goal, deferred to after the v0.5 benchmark harness exists to prove
where ember is slow and justify adding opt passes (`DESIGN.md` Section 9 -
no speculative optimization). The codegen is correctness-first today; the
SSA-lite IR + linear-scan regalloc in `COMPILER_PIPELINE.md` Section 5 is
the target the tree-walker lowers toward conceptually, landed at v0.5.
