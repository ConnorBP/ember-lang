# Ember demos

These demos exercise complete host/runtime paths rather than isolated syntax.
Run commands from the repository root after configuring `buildt`.

## Demo index

| Demo | What it proves | Run |
|---|---|---|
| `main.ember` | multi-file imports, aggregates, slices, f64/narrow integers, defer, recursion, f-strings, function handles, and extensions | `./buildt/ember_cli.exe run demo/main.ember` (returns 777; process exit 9) |
| `main_em.ember` / `main_em.em` | portable `.em` source variant and cross-process load | commands below (returns 888; process exit 120) |
| [`compiler/`](compiler/README.md) | a small lexer/parser/evaluator written in Ember | `./buildt/ember_cli.exe run demo/compiler/main.ember` (exit 0) |
| [`game/`](game/README.md) | lifecycle/tick game simulation using vec/quat/mat/array/math | game README command (exit 1 on the intentional stay-loaded result) |
| [`concurrency/`](concurrency/README.md) | real host-thread queue/atomic contention plus a deterministic tick pipeline | build/run commands in its README |
| [`hotreload/`](hotreload/README.md) | `.em` round-trip, epoch hot reload, reclamation, and a deterministic tick loop | build/run commands in its README |

## Multi-file demo

```bash
./buildt/ember_cli.exe run demo/main.ember
# return 777; Windows process exit = 777 mod 256 = 9
```

The main source intentionally contains a function-reference call. The portable
writer rejects that process-local representation, so use the `main_em.ember`
variant for a `.em` round-trip:

```bash
./buildt/ember_cli.exe emit-em demo/main_em.ember demo/main_em.em
./buildt/ember_cli.exe run --load-em demo/main_em.em
# return 888; Windows process exit = 120
```

The committed `demo/main_em.em` is an older valid artifact. Re-emitting with the
current compiler can change its exact byte size as the module format and codegen
evolve; rely on behavior and format validation, not a historical byte count.

## Current language notes

The old demo workarounds for aggregate globals, array literals, direct struct
literal returns, and struct-returning call arguments are no longer language
limitations. All are implemented and covered by `aggregate_global_test`,
`array_lit_test`, and `binding_abi_test`.

Still-current constraints exposed by these sources:

- `defer` syntax is `defer expression;` (for example `defer cleanup();`), not
  `defer { ... }`;
- a `switch` whose cases all return still needs a trailing return for the
  function-level missing-return check;
- function handles use the distinct `fn` / `fn(Args) -> Ret` type and are not
  interchangeable with `i64`;
- `.em` emission rejects a module if any included function needs a process-local
  representation, even if that function is not called by the selected entry.

The previous `.em` failure-path issue that left an empty output file is fixed:
preflight now occurs before the writer opens/truncates the destination.

## Investigation notes

The three long-form investigation files are retained as historical design
records and now begin with current-status notices:

- [`SLICE_ESCAPE_SAFETY_INVESTIGATION.md`](SLICE_ESCAPE_SAFETY_INVESTIGATION.md)
- [`STRING_CONST_MODE_INVESTIGATION.md`](STRING_CONST_MODE_INVESTIGATION.md)
- [`STRING_ENCRYPTION_ANALYSIS.md`](STRING_ENCRYPTION_ANALYSIS.md)

Do not treat their old line-numbered snapshots as current implementation docs;
use `src/sema.cpp`, `src/codegen.cpp`, and the current tests for that.
