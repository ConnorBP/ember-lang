# Ember self-hosted compiler

The self-hosted compiler is complete. Its lexer, parser, semantic checker, and x64 code generator are written in Ember and execute through the same extension/runtime surface as ordinary scripts.

## Status

- **Parity corpus:** 188/188 passing, 0 unsupported, 0 mismatches
  - 87 classified `tests/lang/valid_*.ember` programs
  - 101 self-hosted correctness programs
- **Local differential suite:** 101/101 passing against the native C++ compiler
- **Bootstrap:** one-stage and two-generation flows working
- **Module output:** EMBM v2, 96-byte header, function table, stable dispatch vector, and `RELOC_DISPATCH = 4`
- **Language coverage in the 188-case corpus:** every integer width, floats, strings/f-strings, structs, enums, arrays/slices, globals, namespaces, constexpr/static assertions, all loop forms, switch/match, defer, try/catch/throw, lambdas/function handles used by the corpus, and coroutines/yield

The authoritative corpus inventory is [`FULL_PARITY_MATRIX.md`](FULL_PARITY_MATRIX.md). The 188/188 number applies to the classified parity corpus; native-only or deliberately rejected self-hosted surfaces (including annotations and managed `new`/`delete`) are called out separately rather than being counted as unsupported corpus cases. The emitted module ABI is documented in [`MODULE_IMAGE_FORMAT.md`](MODULE_IMAGE_FORMAT.md).

## One-stage self-hosted pipeline

The file adapter reads a source path from stdin, compiles it with the Ember-written pipeline, loads the emitted EMBM module, and runs `main`:

```console
echo tests/lang/valid_try_catch.ember | build/ember_cli.exe run self_hosted/correctness_tests/file_pipeline_runner.ember --fn run_file --ffi
```

This returns `42`.

For direct source-string embedding, import `full_pipeline.ember` and call `compile_and_run(source)`.

## Two-generation bootstrap

`bootstrap.ember` proves that the compiler can compile itself:

1. Native `ember_cli` compiles and runs `bootstrap.ember` (compiler A).
2. A reads `bootstrap_compiler_source.ember`, the pre-inlined compiler source.
3. A compiles that source to an EMBM v2 module (compiler B).
4. A loads B, installs B's dispatch table, runs B's globals initializer, and invokes B's `main`.
5. B reads the target path from stdin, compiles it, and runs its `main`.

```console
echo tests/lang/valid_try_catch.ember | build/ember_cli.exe run self_hosted/bootstrap.ember --fn main --ffi
```

Expected process result: `42`.

The bootstrap uses `--ffi` because it needs text file I/O, executable module loading, and raw entry invocation. Run commands from the repository root so the pre-inlined compiler source and target paths resolve correctly.

## Regenerating the pre-inlined source

`bootstrap_compiler_source.ember` is the concatenated bootstrap input containing the compiler stages with imports removed. When changing `lex.ember`, `parse.ember`, `sema.ember`, `codegen.ember`, `full_pipeline.ember`, or `emberc.ember`, regenerate/update this artifact using the project's established bootstrap generation flow before claiming two-generation parity.

## Tests

```console
# 101 local positive differential cases
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -EmberCli build/ember_cli.exe

# Full 188-case native-vs-self-hosted parity corpus
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_parity_audit.ps1 -EmberCli build/ember_cli.exe

# Native CTest suite (94 tests in the current build)
ctest --test-dir build --output-on-failure
```

See [`correctness_tests/README.md`](correctness_tests/README.md) for corpus composition and result interpretation.
