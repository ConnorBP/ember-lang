# Strings

`string` is a typed opaque handle to host-owned bytes. Plain literals adapt to `string` when a string is expected; a literal adapts to `u8[]` in a byte-slice context.

```ember
fn greet(name: string) -> string {
    return "Hello, " + name + "!";
}
```

## Native functions

| Function | Signature | Description |
|---|---|---|
| `string_new` | `() -> string` | new empty string |
| `string_from_slice` | `(bytes: u8[]) -> string` | copy a byte slice into a string |
| `string_length` | `(s: string) -> i64` | byte length; invalid handle returns `0` |
| `string_char_at` | `(s: string, index: i64) -> i64` | byte value; invalid/out-of-range returns `0` |
| `string_from_i64` | `(value: i64) -> string` | decimal conversion |
| `string_from_f32` | `(value: f32) -> string` | `%g` conversion |
| `string_from_f64` | `(value: f64) -> string` | `%g` conversion |
| `string_from_bool` | `(value: bool) -> string` | `"true"` or `"false"` |
| `string_identity` | `(s: string) -> string` | returns the same handle |
| `string_find` | `(s: string, needle: string) -> i64` | first byte offset, or `-1` |
| `string_substr` | `(s: string, start: i64, len: i64) -> string` | clamped substring |
| `fmt1`…`fmt4` | `(format: string, i64...) -> string` | one to four printf-style word arguments |

There are no separate `string_concat` or `string_compare` source-level natives: use `+` and `==`.

## Concatenation, length, and equality

```ember
fn string_demo(name: string) -> i64 {
    let message: string = "name=" + name;
    if (message == "name=ember") {
        return message.string_length();
    }
    return -1;
}
```

`+` creates a new handle. `==` compares byte content, not handle identity. `!=` follows from equality semantics.

## Search and substring

```ember
fn extract() -> string {
    let text: string = "compiler.ember";
    let dot: i64 = string_find(text, ".");
    return string_substr(text, dot + 1, -1);
}
```

`string_find` returns `-1` when absent. `string_substr` behavior:

- negative `start` or invalid handle -> empty string
- `start` at/beyond the end -> empty string
- negative `len` -> from `start` through the end
- excessive positive `len` -> clamped to the end

## Scalar conversion and f-strings

```ember
fn describe(count: i64, ratio: f64, ready: bool) -> string {
    let direct: string = "count=" + string_from_i64(count);
    let interpolated: string = f"{direct}, ratio={ratio}, ready={ready}";
    return interpolated;
}
```

F-string interpolation lowers to `string_from_slice`, `string_from_i64`, `string_from_f32`, `string_from_f64`, `string_from_bool`, or `string_identity`, then concatenates the segments.

## `fmt1` through `fmt4`

These functions accept one to four ABI-word arguments. Supported specifiers are:

| Specifier | Meaning |
|---|---|
| `%d` | signed decimal `i64` |
| `%x`, `%X` | lowercase/uppercase hexadecimal |
| `%c` | low byte as a character |
| `%s` | argument interpreted as a string handle |
| `%f` | argument bits reinterpreted as `f64` |
| `%%` | literal percent; consumes no argument |

```ember
fn formatted(who: string) -> string {
    return fmt3("%s: %d (0x%x)", who as i64, 42, 42);
}
```

Because `fmt*` arguments after the format are declared `i64`, `%s` receives a string handle explicitly cast to its ABI word. `%f` expects the raw bits of a double in that word and is primarily an ABI-level facility; normal Ember code should prefer f-strings or `string_from_f64`.

Unknown specifiers are preserved and do not consume an argument. Missing arguments leave the corresponding `%` sequence in the output; extra arguments are ignored.

## Printing

Use the I/O extension's `print(string)` or `println(string)` and run with `--ffi`:

```ember
fn main() -> i64 {
    println(f"answer={42}");
    return 0;
}
```

`print_string`, `str_compare`, and `str_length` are host-specific prism natives, not part of the standalone string extension.
