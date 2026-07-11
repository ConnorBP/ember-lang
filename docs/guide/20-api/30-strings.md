# Strings

String values in Ember are handles: opaque `i64` references to host-owned string memory. A script never sees the bytes directly. All string work goes through the native functions on this page, or through the `+` and `==` operator overloads registered for the string type. For the underlying type rules (handle types, string literals, byte slices), see [10-language/10-types.md](../10-language/10-types.md).

## Implicit conversion from string literals

A bare string literal converts automatically to whichever form the surrounding context expects. If the target is a `u8[]` slice, the literal becomes a byte slice. If the target is string-typed (an explicit `string` local, a string-typed parameter, or either side of a `+` against a string), the literal becomes a string handle. You almost never need to call `string_from_slice()` by hand; write the literal and let sema pick the conversion.

```ember
fn greet(name: string) -> string {
    return "Hello, " + name + "!";
}

fn main() -> i64 {
    let mut msg: string = greet("V");
    print_string(msg);
    return 0;
}
```

In `greet`, `"Hello, "` and `"!"` are bare literals next to a `+` on a string, so both become string handles before the concatenation runs. The call `greet("V")` passes a bare literal into a string-typed parameter, so it converts the same way at the call site. No explicit conversion function appears anywhere in this snippet.

> **NOTE:** the implicit conversion is chosen by the expected type at that point in the expression, not by inspecting the literal itself. The same literal text can become a `u8[]` slice in one call and a string handle in another, depending on what the callee or the declared type expects.

## Native functions

| Function | Signature | Description |
|---|---|---|
| `string_new` | `() -> i64` | Creates a new, empty string handle. |
| `string_from_slice` | `(s: u8[]) -> i64` | Creates a string handle from a byte slice. Rarely called directly; bare literals convert implicitly instead. |
| `string_length` | `(s: i64) -> i64` | Byte length of a string handle. Callable as `s.string_length()`. |
| `string_char_at` | `(s: i64, i: i64) -> i64` | Byte at index `i`, returned as an `i64`. Returns `0` if `i` is out of bounds. |
| `string_from_i64` | `(v: i64) -> i64` | Formats an `i64` value as a string handle. |
| `string_from_f32` | `(v: f32) -> i64` | Formats an `f32` value as a string handle. |
| `string_from_f64` | `(v: f64) -> i64` | Formats an `f64` value as a string handle. |
| `string_from_bool` | `(v: bool) -> i64` | Formats a `bool` as `"true"` or `"false"`. |
| `string_identity` | `(s: i64) -> i64` | Returns its input string handle unchanged. |
| `str_compare` | `(a: i64, b: i64) -> i32` | Currently a stub. Always returns `0`. Do not use it for real comparisons yet; use `==` instead. |
| `str_length` | `(ptr: i64) -> i64` | C-string length of a raw, null-terminated pointer. This is for interop with raw memory, not string handles, and is unrelated to `string_length`. |

### string_new

```ember
fn empty_report() -> i64 {
    let s: i64 = string_new();
    return s; // string_length(s) == 0
}
```

### string_from_slice

Most code never needs this; write a bare literal where a string handle is expected and let the implicit conversion handle it. It is useful when you already hold a `u8[]` from elsewhere (for example a slice view of a fixed array) and want a string handle from that same data.

```ember
fn from_bytes(data: u8[]) -> i64 {
    return string_from_slice(data);
}
```

### string_length and string_char_at

```ember
fn first_byte_or_zero(s: i64) -> i64 {
    if (s.string_length() == 0) {
        return 0;
    }
    return string_char_at(s, 0);
}
```

`string_char_at` never traps on an out-of-bounds index; it returns `0` instead, so callers do not need a bounds check before calling it.

### string_from_i64 / string_from_f32 / string_from_f64 / string_from_bool

These build a string handle from a scalar value. They are the building blocks f-strings desugar into internally, and are also useful directly when you need a formatted value without the rest of an f-string around it.

```ember
fn describe(count: i64, ratio: f64, ok: bool) -> string {
    let mut out: string = "count=" + string_from_i64(count);
    out = out + ", ratio=" + string_from_f64(ratio);
    out = out + ", ok=" + string_from_bool(ok);
    return out;
}
```

### string_identity

Returns its argument unchanged. It exists mainly as a no-op handle pass-through, useful when a function pointer or a chained call needs something with a string-in, string-out shape but no actual transformation.

### str_compare and str_length

`str_compare` is a stub in the current host build. It always returns `0` regardless of its arguments, so it cannot yet distinguish equal from unequal strings. Use the `==` operator overload for real content comparisons.

`str_length` is unrelated to the string handle system: it walks a raw, null-terminated pointer and returns the byte count before the terminator, the way C's `strlen` does. Use `string_length` for string handles; reach for `str_length` only when interoperating with raw null-terminated memory.

## Operator overloads

The string handle type has two registered operator overloads.

| Operator | Signature | Behavior |
|---|---|---|
| `+` | `string + string -> string` | Concatenation. Returns a new string handle. |
| `==` | `string == string -> bool` | Content comparison, not identity comparison. |

### Concatenation with +

Either side of a `+` against a string can be a bare literal; it converts to a string handle first, then the registered `+` overload runs.

```ember
fn build_path(root: string, name: string) -> string {
    return root + "/" + name + ".ember";
}
```

Concatenation always produces a new handle. It does not mutate either operand.

### Content comparison with ==

`==` on two string handles compares their contents, not their identity. Two separately constructed strings holding the same bytes compare equal, even though they are different handles pointing at different host-owned memory.

```ember
fn matches_tag(input: string) -> bool {
    let expected: string = "prism";
    return input == expected;
}

fn main() -> i64 {
    let mut a: string = string_from_i64(42);
    let mut b: string = "42";
    print_string(string_from_bool(a == b)); // true: same content, different handles
    return 0;
}
```

> **WARNING:** `str_compare` does not participate in this overload and should not be used as a substitute for `==`. Since it always returns `0`, code that branches on its result will behave as if every pair of strings is equal.

## See also

- [10-language/10-types.md](../10-language/10-types.md) for how string handles fit into Ember's type system alongside slices and the other opaque handle types.
- [20-api/40-math-vectors.md](40-math-vectors.md) for the vector and matrix handle types, which follow the same host-owned-handle pattern as string.
- [30-examples/10-fibonacci.md](../30-examples/10-fibonacci.md) for a complete walkthrough script that combines control flow, globals, and `defer`.
