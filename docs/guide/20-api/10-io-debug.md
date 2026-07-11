# I/O and Debug

These are the native functions Ember scripts use to get information out to the host: raw value printing. They are the first natives most scripts touch, since they are the only way to observe what a running script is doing without a debugger attached.

All functions on this page return `void`. They are one-way: value in, text out. None of them read anything back into the script.

A host may expose additional printing natives on top of these (for example a string-handle printer or a prefixed log helper); see your host's documentation for those.

## Overview

| Function | Signature | Purpose |
|---|---|---|
| `print_i64` | `print_i64(v: i64) -> void` | Print a 64-bit integer |
| `print_f32` | `print_f32(v: f32) -> void` | Print a 32-bit float |
| `print_str` | `print_str(s: u8[]) -> void` | Print a raw byte slice |

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
