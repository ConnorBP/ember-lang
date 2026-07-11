# I/O and Debug

> **STALENESS NOTICE (2026-07-11):** this page documents `print_i64`,
> `print_f32`, and `print_str`. **Only `print_i64` is in the standard `io`
> extension.** The actual `io` extension (`extensions/io/ext_io.cpp`) ships:
> `print(s: string)`, `println(s: string)`, `print_i64(n: i64)`,
> `print_f64(n: f64)`, `read_line() -> string`, `file_read_bytes(path) ->
> array<u8>`, `file_write_bytes(path, buf) -> i64`, `file_exists(path) -> i64`,
> `path_exists(p) -> i64`, `path_basename(p) -> string`, `path_dirname(p) ->
> string` — **all `PERM_FFI`-gated** (the CLI grants the bit via `--ffi` /
> `--allow-io`). `print_f32` and `print_str` are **prism-host** natives (per
> `extensions/AUDIT.md`), not the standard extension. The examples below use
> the prism naming and are not accurate against the standalone `ember` CLI;
> see `extensions/README.md` + `docs/ROADMAP.md` (Family B) for the real
> surface. A full rewrite of this page is tracked as a follow-up.

These are the native functions Ember scripts use to get information out to the host: raw value printing. They are the first natives most scripts touch, since they are the only way to observe what a running script is doing without a debugger attached.

All functions on this page return `void`. They are one-way: value in, text out. None of them read anything back into the script.

A host may expose additional printing natives on top of these (for example a string-handle printer or a prefixed log helper); see your host's documentation for those.

## Overview

| Function | Signature | Purpose |
|---|---|---|
| `print` | `print(s: string) -> void` | Print a string handle (io extension, `PERM_FFI`-gated) |
| `println` | `println(s: string) -> void` | Print a string handle + newline (io extension, `PERM_FFI`-gated) |
| `print_i64` | `print_i64(v: i64) -> void` | Print a 64-bit integer (io extension, `PERM_FFI`-gated) |
| `print_f64` | `print_f64(v: f64) -> void` | Print a 64-bit float (io extension, `PERM_FFI`-gated) |
| `read_line` | `read_line() -> string` | Read a line from stdin into a string handle (io extension, `PERM_FFI`-gated) |
| `file_read_bytes` | `file_read_bytes(path: string) -> array<u8>` | Read a file into a byte array handle (io extension, `PERM_FFI`-gated) |
| `file_write_bytes` | `file_write_bytes(path: string, buf: array<u8>) -> i64` | Write a byte array to a file (io extension, `PERM_FFI`-gated) |
| `file_exists` | `file_exists(path: string) -> i64` | 1 if the file exists, 0 otherwise (io extension, `PERM_FFI`-gated) |
| `path_exists` | `path_exists(p: string) -> i64` | 1 if the path exists, 0 otherwise (io extension, `PERM_FFI`-gated) |
| `path_basename` | `path_basename(p: string) -> string` | The basename of a path (io extension, `PERM_FFI`-gated) |
| `path_dirname` | `path_dirname(p: string) -> string` | The dirname of a path (io extension, `PERM_FFI`-gated) |

> The `print_f32` and `print_str` entries in the original version of this page
> are **prism-host** natives, not the standard `io` extension — they are not
> registered by `ext_io::register_natives`.

## print_i64

```text
print_i64(v: i64) -> void
```

Prints a 64-bit signed integer to the host's output, formatted as a plain decimal number. No prefix, no newline handling beyond what the host print sink does, no thousands separators.

This is the function you reach for first when debugging a script: cheap, unambiguous, and works on anything that fits in an `i64` (which includes `bool` and pointer-sized values via a cast).

```ember
fn main() -> i64 {
    let total: i64 = 0;
    for (let i: i64 = 1; i <= 5; i += 1) {
        total += i;
    }
    print_i64(total);  // 15
    return 0;
}
```

## print_f32

```text
print_f32(v: f32) -> void
```

Prints a 32-bit float to the host's output. Formatting matches C's `%g`: the shortest representation that round-trips, switching to exponential notation for very large or very small magnitudes.

```ember
fn main() -> i64 {
    let a: f32 = 1.5;
    let b: f32 = 2.5;
    print_f32(a + b);   // 4
    print_f32(b / a);   // 1.66667
    return 0;
}
```

## print_str

```text
print_str(s: u8[]) -> void
```

Prints a raw `u8[]` byte slice to the host's output, writing exactly the bytes in the slice with no null-termination assumptions and no handle lookup. A string literal passed directly as this argument converts to `u8[]`, because that is what `print_str`'s parameter is declared as; the same literal would convert to a `string` handle instead if passed somewhere a `string` is expected. See [Types](../10-language/10-types.md#string-literals-and-the-string-handle) for the full context-dependent conversion rule.

```ember
fn main() -> i64 {
    print_str("still just a slice, not a string handle");
    return 0;
}
```

`print_str` also works on any `u8[]` you have constructed or sliced yourself, not just literals:

```ember
fn main() -> i64 {
    let letters: u8[5] = [104, 101, 108, 108, 111];
    print_str(letters[..]);  // hello
    return 0;
}
```

> **NOTE:** `print_str` takes a `u8[]` slice (a string literal, or any raw byte buffer). To print a `string` *handle*, the value you get back from `string_from_slice`, f-string interpolation, or the `+` concatenation overload, use your host's string-handle printing native (see [Strings](30-strings.md) for how `string` handles are created and combined). Passing a `string` handle where a `u8[]` is expected, or vice versa, is a compile error, not a runtime one.

---

Related pages: [API Overview](00-overview.md), [Strings](30-strings.md), [Assertions](20-assertions.md).
