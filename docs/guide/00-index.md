# Ember Developer Guide

> **STALENESS NOTICE (2026-07-11):** this guide was written against an early
> pre-v1.0 state of ember and has **not** been kept current with the v1.0
> language. Several claims below are now **wrong** or **incomplete**:
> - **Bounds checking IS now emitted at runtime** for slice/fixed-array
>   indexing (`TrapReason::BoundsCheck`, `src/codegen.cpp`); this guide
>   repeatedly says indexing is *not* bounds-checked — that is **false**.
> - **Structs CAN now be passed by value** across a call boundary (≤8 bytes in
>   a register, >8 bytes via the Win64 hidden-pointer path, native args up to
>   128 bytes); this guide says they cannot — that is **false**.
> - **`for (x in slice)` / `for (x in array_handle)` for-each**, **`match`**,
>   **enums** (untyped + typed `enum E : T`), **`try`/`catch`/`throw`**,
>   **`constexpr fn`**, **`static_assert`**, **namespaces**, **lambdas**,
>   **coroutines** (`yield`), **parameterized `fn(Args)->Ret` types**, and
>   **cross-module `&mod::fn` handles** all **shipped** and are **not**
>   documented here.
> - The **standard extension API** documented in §20-api is the **prism host's**
>   native set (`print_str`/`print_f32`/`print_string`/`assert_eq_*`/
>   `str_compare`/`str_length`/`aim_atan2`/`clamp`), **not** the natives the
>   `ember` `io`/`string`/`math` extensions actually ship (`print`/`println`/
>   `print_i64`/`print_f64`/`read_line`, `string_find`/`string_substr`,
>   `floor_f64`/`ceil_f64`/`abs_f64`/`pow_f64`/`abs_i64`, ...). Several
>   guide examples call natives that are **not** in the standard extension set.
> - The example scripts referenced (`fibonacci.ember`, `vector_math_demo.ember`,
>   `bubble_sort.ember`) **do not exist** in `examples/scripts/`; their
>   "Full Source" blocks are illustrative, not real files.
>
> For **accurate, current** language + extension documentation, see the **spec
> docs** (`docs/spec/TYPE_SYSTEM.md`, `COMPILER_PIPELINE.md`, `CODEGEN_SPEC.md`,
> `SAFETY_AND_SANDBOX.md`, `BINDING_API.md`, `MEMORY_AND_GC.md`), the
> **README.md**, `extensions/README.md`, and `docs/ROADMAP.md`. The fixes below
> correct the most actively-false claims; a full guide rewrite is tracked as a
> follow-up.

Ember is a small, statically-typed scripting language that compiles to native code for embedding in host applications. It gives you fast, checked, near-native execution with a syntax deliberately kept close to C-family languages, plus a standard extension API surface (printing, assertions, string handling, math and vector types, arrays, and more) that every Ember host can expose into a running script.

> **NOTE:** This guide is for people writing `.ember` scripts against *any* host application that embeds Ember. It documents the language itself and the standard extension API that ships with Ember. It does not cover the Ember compiler internals, code generation, or how to embed the Ember toolchain into a new host application. Host-specific natives (a particular host's drawing, memory access, UI widgets, and so on) are documented by that host, not here.

## Who This Is For

If you are opening a `.ember` file, writing a script that calls the standard `print_*` / `string_*` / `vec*` / `array_*` natives, or wiring up an `@entry` or `@on_tick` handler, this documentation is for you. It assumes you can already read C-like code. It does not assume you know anything about how Ember is implemented.

## How the Guide Is Organized

The guide has three sections, meant to be used in different ways.

### Language Reference

The rules of the language itself: types, declarations, statements, operators, and the sharp edges worth knowing about before you hit them. Start with types, since almost everything else (function signatures, struct layout, casts, indexing) is stated in terms of them.

- [Types](10-language/10-types.md)

### API Reference

The standard extension natives every Ember host can expose to a running script: I/O and debug printing, assertions, string handling, math and vector types, and arrays. Each page documents one functional area. Start at the overview to see how the surface is organized. (Your host may expose additional, host-specific natives on top of these; see your host's documentation for those.)

- [API Overview](20-api/00-overview.md)

### Examples

Complete, working `.ember` scripts with a walkthrough of what each one does and why. Read these when you want to see idioms in context rather than in isolation. Fibonacci is the shortest and the best starting point.

- [Fibonacci](30-examples/10-fibonacci.md)

## Reading Order

New to Ember: read the Language Reference in order, skim the API Overview, then read the Fibonacci example end to end.

Already comfortable with the language: jump straight to the API Reference section you need, and treat the Examples as recipes.
