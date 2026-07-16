# Ember Developer Guide

Ember is a statically typed, C-family language that compiles to native code for embedding. The standalone `ember_cli` host registers the standard extension set and can compile, run, test, bundle, and precompile `.ember` modules.

This guide describes the current language and the extensions shipped by this repository. A different embedding host may register only a subset of the extensions or add host-specific natives.

## Start here

- [Getting Started](01-getting-started.md) — build and run a first program with `ember_cli`

## Language reference

- [Types](10-language/10-types.md) — every integer width, floats, `bool`, `void`, strings, structs, enums, arrays, slices, function types, and opaque handles
- [Declarations](10-language/20-declarations.md) — `fn`, `let`, `let mut`, `auto`, `const`, `global`, `struct`, `enum`, `namespace`, `priv`, `constexpr`, and annotations
- [Statements](10-language/30-statements.md) — branches, all loop forms, `switch`, `match`, exceptions, coroutines, `defer`, jumps, and `static_assert`
- [Expressions and Operators](10-language/40-expressions-operators.md) — operators, casts, indexing, f-strings, lambdas, function handles, `sizeof`, and `offsetof`
- [Gotchas](10-language/50-gotchas.md) — bounds checks, ownership/lifetime rules, switch behavior, and other common surprises

## Extension API reference

- [API Overview](20-api/00-overview.md) — complete extension inventory and permission model
- [I/O and Debug](20-api/10-io-debug.md) — console, file, and path natives
- [Assertions](20-api/20-assertions.md) — optional host assertion helpers; not part of the standalone standard extension set
- [Strings](20-api/30-strings.md) — conversion, concatenation, search, substring, and formatting
- [Math and Vectors](20-api/40-math-vectors.md) — scalar math and `vec2`/`vec3`/`vec4`/`quat`/`mat4`
- [Arrays](20-api/50-arrays.md) — dynamically sized host-owned arrays

The repository also ships map, synchronization, threading, coroutine, lifecycle, GC, audio, raw-call/module-loader, optimization-pass, and obfuscation-pass extensions. They are inventoried in the [API Overview](20-api/00-overview.md); their specialist references live under `docs/` and `extensions/`.

## Worked examples

Each page contains a complete source file and the exact command used to run it.

- [Fibonacci](30-examples/10-fibonacci.md)
- [Vector Math](30-examples/20-vector-math.md)
- [Bubble Sort](30-examples/50-bubble-sort.md)

## Recommended reading order

New users should read Getting Started, Types, Declarations, Statements, and Expressions in that order, then use the API pages as references. Compiler and embedding authors should use the specifications under `docs/spec/` and the extension inventory in `extensions/README.md` in addition to this script-facing guide.
