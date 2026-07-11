# Types

Ember is statically typed. Every value has exactly one type, decided at compile time, and there is no implicit
conversion between unrelated types. This page covers every type category in the language: primitives, structs,
fixed arrays, slices, string literals and the string handle, and the opaque handle types.

For how these types are used in declarations (`fn`, `struct`, `let`, `auto`, `const`, `global`), see
[Declarations](20-declarations.md). For the actual functions and operators available on the handle types
(`vec2`, `vec3`, `vec4`, `quat`, `mat4`), see [Math and Vectors](../20-api/40-math-vectors.md).

## Primitive Types

Ember has a fixed set of primitive scalar types: one boolean type, eight sized integer types, two floating-point
types, and `void` as a return-type-only marker.

| Type | Size | Signed | Range / Notes |
|---|---|---|---|
| `bool` | 1 byte | n/a | canonical values `0` or `1` |
| `i8` | 1 byte | yes | -128 to 127 |
| `i16` | 2 bytes | yes | -32768 to 32767 |
| `i32` | 4 bytes | yes | -2147483648 to 2147483647 |
| `i64` | 8 bytes | yes | -9223372036854775808 to 9223372036854775807 |
| `u8` | 1 byte | no | 0 to 255 |
| `u16` | 2 bytes | no | 0 to 65535 |
| `u32` | 4 bytes | no | 0 to 4294967295 |
| `u64` | 8 bytes | no | 0 to 18446744073709551615 |
| `f32` | 4 bytes | n/a | IEEE 754 single precision |
| `f64` | 8 bytes | n/a | IEEE 754 double precision |
| `void` | n/a | n/a | valid only as a function return type |

There is no `char` or `wchar` type. Text is represented as `u8` slices, or as the opaque `string` handle type
described below. There is also no `usize` or other pointer-sized integer type, and no raw pointer type at all.
Slices are the only form of indirection in the language.

```ember
let flag: bool = true;
let small: i8 = -12;
let big: u64 = 18446744073709551615;
let ratio: f64 = 3.14159;
```

### Widening and Narrowing

Safe widening conversions happen implicitly: `i8` to `i16` to `i32` to `i64`, `u8` to `u16` to `u32` to `u64`, and
`f32` to `f64`. Signed and unsigned families do not implicitly widen into each other.

```ember
let a: i16 = 10;
let b: i64 = a;   // implicit widen, no cast needed
```

Narrowing, signed-unsigned conversion, and int-float conversion all require an explicit `as` cast. See
[Expressions and Operators](40-expressions-operators.md) for the full cast rules.

```ember
let x: i64 = 300;
let y: i32 = x as i32;   // explicit narrowing cast required
```

## Structs

A struct is a script-declared aggregate of named fields, laid out using MSVC x64 layout rules. Fields are
separated by semicolons, not commas.

```ember
struct Vec2i {
    x: i32;
    y: i32;
}
```

Struct rules:

- No self-referential cycles. A struct cannot contain itself, directly or through a chain of other struct fields;
  this is a compile error.
- Empty structs are allowed and have size 0, alignment 1.
- Structs cannot declare methods directly. Any methods you see called with `obj.method(args)` syntax on a struct
  come from host-registered operator overloads, not script-side struct declarations.
- Structs cannot be passed by value across a function call boundary in v1. This is a deliberate scope limit,
  enforced as a sema error. A function that logically needs a struct's data takes the individual fields as
  separate scalar parameters instead.

```ember
struct Rect {
    width: f32;
    height: f32;
}

// Cannot do: fn area(r: Rect) -> f32
// Instead, pass the fields directly:
fn area(width: f32, height: f32) -> f32 {
    return width * height;
}
```

Struct values are read and written field by field with `.` access:

```ember
let mut r: Rect = Rect { width: 4.0, height: 2.0 };
let a: f32 = area(r.width, r.height);
```

See [Declarations](20-declarations.md) for full `struct` declaration syntax and field initialization.

## Fixed Arrays

A fixed array type is written `T[N]`, where `N` is a compile-time constant integer literal greater than 0. Fixed
arrays are value types.

```ember
let mut scores: i32[4] = [10, 20, 30, 40];
let first: i32 = scores[0u];
```

An array index can be any integer type, signed or unsigned (`i8`...`i64`, `u8`...`u64`); sema only requires the
index expression to be *some* integer type. Indexing is **not** bounds-checked at runtime: an out-of-range index
computes an address past the end of the array (or slice) and reads or writes whatever memory is there instead of
trapping. Keeping every index inside `[0, N)` is entirely the script's responsibility.

```ember
let mut buf: u8[16];
let i: u32 = 3u;
buf[i] = 0xFFu8;
```

## Slices

A slice type is written `T[]`. A slice is a two-word value: a pointer and a length. There is no slice-of-slice
type, and no raw pointer type exists independently of slices; slices are the only indirection Ember has.

Slices are created from fixed arrays with the view conversion syntax `arr[..]`, which is the only slicing syntax
in v1 (there is no partial-range slicing yet).

```ember
let mut buf: u8[16];
let view: u8[] = buf[..];
```

Like fixed arrays, slice indexing accepts any integer index type and is not bounds-checked at runtime.

```ember
fn sum_all(values: u32[]) -> u64 {
    let mut total: u64 = 0;
    let mut i: u32 = 0u;
    while (i < 4u) {
        total += values[i] as u64;
        i += 1u;
    }
    return total;
}
```

Slices of `u8` are also the type that plain string literals convert into when a `u8[]`-typed context expects one.
See below.

## String Literals and the String Handle

Ember has two distinct string-related types: a plain string literal that can act as a `u8` slice, and the opaque
`string` handle type backed by host-owned memory.

A string literal is written `"..."`, or with interpolation as an f-string: `f"...{expr}..."`. A bare string
literal implicitly converts to whichever type the surrounding context expects:

- to `u8[]` when a byte slice is expected
- to the `string` handle (an opaque `i64`) when the target is string-typed: a `let` with an explicit `string`
  type, a `string`-typed function parameter, or either side of a `+` expression against a `string`

There is no need to manually wrap a literal in a conversion call; the compiler resolves this at each use site
based on the expected type.

```ember
let mut name: string = "PRISM";        // literal converts to a string handle
let bytes: u8[] = "raw bytes";         // same literal syntax, converts to u8[] here instead

let greeting: string = "Hello, " + name;   // literal + string handle, uses the string + overload

let user_id: i32 = 42;
let report: string = f"user {user_id} ready";  // f-string desugars to a string handle
```

### Multi-Line Strings: Continuation vs. Raw Strings

A plain `"..."` (or f-string) literal is single-line by design: an un-escaped newline inside one is a compile
error, not something that silently spans the literal across lines. This is deliberate - it means a stray or
missing closing quote fails immediately, at the exact line the mistake is on, instead of the literal silently
swallowing every line after it (including the rest of the file) as "content" until it happens to find some
later `"` to close on.

If a plain string needs to continue onto the next source line, end that line with a backslash: this is shorthand
for an explicit `\n` - it inserts a real newline into the value, just spelled as a trailing backslash plus an
actual source line break instead of the two characters `\` `n`.

```ember
let s: string = "first line \
second line continues here";   // value: "first line \nsecond line continues here"
```

For multi-line text where writing a `\` at the end of every single line would be tedious - or for text that needs
literal backslashes without doubling them, like a Windows path - use a raw string instead: `r"""..."""`. Everything
between the triple quotes is taken completely literally, including newlines and backslashes; there is no escape
processing at all inside one (so `r"""C:\Users\name"""` needs no `\\` doubling), and it closes only at the next
literal `"""`.

```ember
let block: string = r"""Hello there
today is Tuesday.
New line.
""";
```

The `string` type itself is one of the opaque handle types; see the next section for what that means, and see
[Strings](../20-api/30-strings.md) for the full function and operator reference (`string_*` functions, `str_compare`,
`str_length`, and the `+` / `==` overloads).

## Opaque Handle Types

Ember has a family of handle types that are all, under the hood, an opaque `i64` referring to host-owned memory:
`vec2`, `vec3`, `vec4`, `quat`, `mat4`, `string`, and the generic array handle.

Key properties shared by every handle type:

- A handle is a plain `i64` value as far as the ABI is concerned, but the script cannot read or write its bytes
  directly. All access goes through registered accessor functions, methods, or operator overloads.
- The script never manages a handle's lifetime; the host owns the underlying memory.
- Operators on handle types (`+`, `-`, `==`, indexing, and so on) are resolved statically at compile time by sema
  based on the operand types. There is no runtime virtual dispatch. If no matching overload is registered for a
  given type pair, it is a compile error.
- Method-call syntax on a handle, `obj.method(args)`, desugars at sema time to `method(obj, args)`, with the
  receiver becoming the first argument.

```ember
let a: vec3 = vec3_new(1.0, 2.0, 3.0);
let b: vec3 = vec3_new(0.0, 1.0, 0.0);
let c: vec3 = a + b;              // operator overload resolved at compile time
let x: f32 = a.vec3_x();          // desugars to vec3_x(a)
```

> **NOTE:** This page only describes the handle types as *types*: what they are, how they behave under the type
> system, and how they relate to structs, primitives, and each other. The actual constructor, accessor, and
> operator functions for `vec2`, `vec3`, `vec4`, `quat`, and `mat4` are documented in
> [Math and Vectors](../20-api/40-math-vectors.md). The array handle functions live in
> [Arrays](../20-api/50-arrays.md), and the `string` handle functions live in [Strings](../20-api/30-strings.md).

### The array handle

The array handle is a generic opaque `i64` representing a host-owned, dynamically sized array. It is distinct
from the fixed array type `T[N]` and the slice type `T[]`: those are script-visible value types with direct
indexing, while the array handle is manipulated exclusively through host-registered `array_*` functions and
carries its element size (in bytes) rather than an element type known to the compiler. There is no `array<T>`
generic syntax in the language; the handle is just an `i64` like every other handle type.

```ember
let list: i64 = array_new(8, 1);        // elem_size=8 bytes (i64-sized elements), 1 initial element
array_set_i64(list, 0, 10);
let v: i64 = array_get_i64(list, 0);
```

Exact constructor and accessor names are listed in [Arrays](../20-api/50-arrays.md).

## Type Summary

| Category | Examples | Value or Handle | Indirection |
|---|---|---|---|
| Boolean | `bool` | value | none |
| Integer | `i8`...`i64`, `u8`...`u64` | value | none |
| Floating point | `f32`, `f64` | value | none |
| Struct | script-declared `struct Name { ... }` | value | none, and cannot cross call boundaries by value |
| Fixed array | `T[N]` | value | none (contiguous inline storage) |
| Slice | `T[]` | value (pointer + length) | one level, the only indirection in Ember |
| String literal | `"..."`, `f"...{}..."` | converts to slice or handle by context | n/a |
| String handle | `string` | opaque handle | host-owned |
| Vector/matrix handles | `vec2`, `vec3`, `vec4`, `quat`, `mat4` | opaque handle | host-owned |
| Array handle | generic array handle | opaque handle | host-owned |

---

Continue to [Declarations](20-declarations.md) for how these types appear in `fn`, `struct`, `let`, `auto`, and
`const` declarations, or jump straight to [Math and Vectors](../20-api/40-math-vectors.md) for the vector and
matrix handle API.
