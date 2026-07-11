# API Reference Overview

This section documents the standard extension native functions that every Ember host can expose to a running script. Native functions are the only way a script observes or affects anything outside its own values: printing, asserting, string handling, math, and arrays.

A host application may register additional, host-specific natives on top of these (drawing, process memory, UI widgets, and so on). Those are documented by the host, not in this guide. This page covers only the standard extension surface that ships with Ember itself.

## How This Section Is Organized

Each functional category has its own page. A script rarely needs the whole surface at once, so grouping by purpose keeps each page short enough to scan while writing a script.

- [I/O and Debug](10-io-debug.md): `print`, `println`, `print_i64`, `print_f64`, and `read_line` (the `io` extension's console natives), plus `file_*`/`path_*` — all `PERM_FFI`-gated
- [Assertions](20-assertions.md): `assert_eq_*`, lightweight in-script self-checks
- [Strings](30-strings.md): the `string` extension's `string_new`/`string_from_slice`/`string_from_i64`/`string_from_f32`/`string_from_f64`/`string_from_bool`/`string_length`/`string_char_at`/`string_find`/`string_substr`/`string_identity`, plus the string `+` and `==` overloads. (`str_compare`/`str_length` are **prism-host** natives, not the standard extension — see the note on that page.)
- [Math and Vectors](40-math-vectors.md): `vec2`/`vec3`/`vec4`/`quat`/`mat4` handle types and the `math` extension's scalar helpers (`sqrt`/`sin`/`cos`/`tan` f32; `*_f64` + `floor_f64`/`ceil_f64`/`abs_f64`/`pow_f64`/`abs_i64`). (`aim_atan2`/`clamp` are **prism-host** natives, not the standard extension.)
- [Arrays](50-arrays.md): `array_*` handle functions for the `array of T` handle type

## Naming Conventions

Every native function name is snake_case, matching Ember's own identifier convention for functions and variables. A few consistent patterns show up across categories:

- **`get_` / `set_` prefixes** mark a paired accessor and mutator for the same underlying value. If you see a `get_something`, check the same page for a matching `set_something` before assuming the value is read-only.
- **`assert_eq_` prefix** marks the self-check family described on the Assertions page. Every one of these compares two values of a fixed type and traps the script on mismatch rather than returning a bool.
- Everything else follows a plain `noun_verb` or `category_action` shape, such as `string_from_slice`, `array_push`, or `vec3_new`.

```ember
// assert_eq_ self-check and plain snake_case in one script
let total: i64 = 0;
for (let i: i64 = 1; i <= 5; i += 1) {
    total += i;
}
assert_eq_i64(total, 15);
print_i64(total);
```

## Parameter Types Shown Are Ember-Side Types

Every signature in this section is written in Ember's own type syntax, the same syntax you would use in a `fn` declaration. These are the types the function expects when called from a script, not the C++ types on the host side. A parameter documented as `s: i64` for a string function means "pass a string handle here," since `string` is itself an opaque `i64` handle from Ember's point of view. A parameter documented as `s: u8[]` means a byte slice, and a bare string literal converts to either automatically depending on which the function expects.

> **NOTE:** Several native functions internally call a compiler-generated string-decrypt helper (its name is double-underscore prefixed) as part of string-obfuscation output. This helper is not documented on any page in this section because scripts never call it directly. It is inserted automatically by the compiler wherever an `obf` or `obf_keyed` annotation applies to a string literal, and it has no callable form in Ember source.

## Reading a Signature

Each entry in the category pages follows the same shape: the function name, its parameter list with Ember types, its return type (or no arrow if it returns nothing), and a short description of behavior and any gotchas (stub status, trap conditions, side effects). Where a function is also reachable through method-call sugar, such as `s.string_length()` desugaring to `string_length(s)`, the entry notes it.
