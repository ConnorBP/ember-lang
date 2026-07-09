# ember

C-style scripting language, JIT-compiles to native x86-64. AngelScript
ergonomics, an optimizing native-JIT language's speed. Game-engine/modding embedding target.

Status: **v0.1 PASS** - JIT codegen proof implemented and passing on both
MSVC 19.36 (VS 2022) and MinGW g++ 15.2.0. See `src/` for the encoder,
label/patch system, exec-memory allocator, IR data model, the `.em` binary
bundling format (`em_file`/`em_writer`/`em_loader`), and the expanded
acceptance-criteria runner (`CODEGEN_SPEC.md` Section 12 + Section 15). Next step: v0.2
(lexer + parser + AST, IR-driven lowering, script-to-script calls via
dispatch table).

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

Every feature in the original request (C-style syntax, JIT to native
x86-64, composable chunks via the dispatch table, safety, AngelScript-
style bindings, hot reload, GC deferral) has a detail doc with edge
cases spelled out. `GAP_ANALYSIS.md` is the audit confirming nothing
the original request asked for is missing; `ROADMAP.md` is the tracked
list of what's deliberately deferred and when each comes back.

Status: **v0.1 PASS** (JIT codegen proof + `.em` bundling format shipped).
Next step: v0.2 (lexer+parser+AST, IR-driven lowering, script-to-script
calls via dispatch table). The spec docs are stable; `src/` contains the
v0.1 implementation including the `.em` serializer/loader.

## Building v0.1 (the JIT codegen proof)

C++17, no external deps beyond `Windows.h` (VirtualAlloc). Builds clean on
both MSVC (VS 2022) and MinGW g++. Pick a generator with a working toolchain:

```bash
cd ember
mkdir build && cd build
# MinGW g++ + Ninja (fastest path on this machine):
cmake -G Ninja -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe ..
cmake --build .          # or: ninja
./ember_v01.exe          # runs the Section 12 acceptance criteria + ISA coverage
ctest                    # CTest target: ember_v01_acceptance
# MSVC: open a VS x64 developer prompt, then:
#   cmake -G Ninja .. && cmake --build . && ember_v01.exe
```

`ember_v01.exe` exits 0 on full pass and prints `RESULT: PASS`. The eight
assertions: IR model for `fn add` (test 0), the five `CODEGEN_SPEC.md` Section 12
criteria (tests 1 - 5: int add→7, f64 add via addsd, forward-jcc fixup,
spill path, recursive fib via dispatch table), a byte-exact ISA-encoder
coverage check for the full Section 3 instruction subset (test 6), and a `.em`
round-trip - JIT-build fib, serialize via `write_em_file`, reload via
`load_em_file`, assert the loaded module runs identically to the JIT'd
one (test 7).

## CLI

`ember_v01.exe` takes three flags (see `cli_main` in `src/main.cpp`):

- `--run` *(default)* - runs the acceptance suite (tests 0 - 7).
- `--emit-em <in.ember> [-o <out.em>]` - stub. v0.1 has no source parser,
  so the source→`.em` path isn't wired; the emit path itself is proven
  end-to-end by acceptance test #7 (JIT fib → `write_em_file` →
  `load_em_file` → run). Wiring this to real `.ember` input is gated on
  the v0.2 lexer/parser.
- `--load-em <out.em>` - stub. Standalone load+invoke isn't wired yet;
  the loader (`em_loader`) is proven by test #7's round-trip. A
  `--load-em [--run=fn]` driver is a thin CLI wrapper, deferred until
  there's a parser producing non-trivial modules.

The IR data model (`src/ir.hpp`) is an exercised deliverable - test 0
builds the `IrFunction` for `fn add` and asserts the IR types represent
it. IR-driven lowering (an `IrFunction` → machine-code lowering pass) is
*not* implemented yet: tests 1 - 5 go straight to the Encoder per
`CODEGEN_SPEC.md` Section 12.1's hand-built-IR wording. The IR backend is the
v0.2 boundary.
