# Types

Ember is statically typed. Every expression has a compile-time type, and implicit conversions are intentionally limited.

## Primitive types

| Type | Size | Notes |
|---|---:|---|
| `i8`, `i16`, `i32`, `i64` | 1, 2, 4, 8 bytes | signed integers |
| `u8`, `u16`, `u32`, `u64` | 1, 2, 4, 8 bytes | unsigned integers |
| `f32`, `f64` | 4, 8 bytes | IEEE-754 floating point |
| `bool` | 1 byte | `true` or `false`; no integer truthiness |
| `void` | — | function return type; no value |

There are no `char`, `usize`, or raw-pointer types. A byte is `u8`; text normally uses `string` or `u8[]`.

```ember
fn primitive_demo() -> i64 {
    let a: i8 = (-12) as i8;
    let b: u16 = 65000;
    let c: i32 = 100000;
    let d: u64 = 4000000000;
    let x: f32 = 1.25f;
    let y: f64 = 2.5;
    let ok: bool = x < (y as f32);
    return (a as i64) + (b as i64) + (c as i64) + (d as i64) + (ok ? 1 : 0);
}
```

### Conversion rules

Implicit widening is available within the same family:

- `i8 -> i16 -> i32 -> i64`
- `u8 -> u16 -> u32 -> u64`
- `f32 -> f64`

Narrowing, signedness changes, integer/float conversion, and integer/`bool` conversion require `as`.

```ember
fn conversions(x: i32) -> f64 {
    let wide: i64 = x;
    let unsigned: u32 = x as u32;
    return (wide as f64) + ((unsigned as i64) as f64);
}
```

## Structs

Structs are nominal value types. Fields are laid out in declaration order with Ember's tight-packed layout: no alignment or trailing padding is inserted.

```ember
struct Point {
    x: i32;
    y: i32;
}

fn translate(p: Point, dx: i32, dy: i32) -> Point {
    let mut out: Point = p;
    out.x += dx;
    out.y += dy;
    return out;
}
```

Structs may contain other structs and fixed arrays but cannot form recursive by-value cycles. They can be passed and returned by value. `sizeof(Point)` and `offsetof(Point, y)` expose Ember layout, not C or MSVC layout. These are compile-time-valued `u64` expressions, though the current `static_assert` folder only accepts its documented integer/boolean expression subset; bind layout queries to `const` or use them in ordinary compile-time contexts.

## Enums

An untyped enum uses integer values. Variants auto-increment after the previous value; explicit values may be compile-time integer expressions.

```ember
enum State {
    Idle,
    Running = 10,
    Stopped
}
```

A typed enum has an explicit integer backing type and remains nominally distinct:

```ember
enum Status : u8 {
    Ok = 0,
    Failed = 1
}

fn status_code(s: Status) -> i64 {
    return s as i64;
}
```

Use `EnumName::Variant` to name a variant. Integer-to-enum conversion is deliberately not implicit.

## Fixed arrays

A fixed array is `T[N]`, where `N` is a positive compile-time integer. Storage is inline and the length is part of the type.

```ember
fn array_demo() -> i64 {
    let mut values: i32[4] = [10, 20, 30, 40];
    values[2] = 99;
    return values[2] as i64;
}
```

Array and slice indices may use any integer type. Literal out-of-range fixed-array indices are rejected at compile time; dynamic indices are checked at runtime and trap with `TrapReason::BoundsCheck` when out of range.

## Slices

A slice is `T[]`, represented as a pointer-and-length pair. It is a borrowed view, not an owning growable collection. Convert a complete fixed array with `array[..]`:

```ember
fn sum(values: i64[]) -> i64 {
    let mut total: i64 = 0;
    for (value in values) {
        total += value;
    }
    return total;
}

fn slice_demo() -> i64 {
    let values: i64[4] = [1, 2, 3, 4];
    return sum(values[..]);
}
```

Only whole-array view syntax is available; there is no `array[begin..end]` range syntax. Slice indexing is runtime bounds-checked. Borrow checking rejects local slice views that escape their backing storage through returns, globals, or retaining calls.

## Strings and byte slices

A plain literal can adapt to either `string` or `u8[]` according to context:

```ember
fn text_demo() -> i64 {
    let text: string = "hello";
    let bytes: u8[] = "hello";
    return string_length(text) + (bytes[0] as i64);
}
```

`string` is an opaque host-owned handle registered by the string extension. It supports content `==`, concatenation with `+`, conversion natives, search, and substring operations. An f-string evaluates to `string`:

```ember
fn label(n: i64, ready: bool) -> string {
    return f"item {n}, ready={ready}";
}
```

Raw multiline strings use `r"""..."""`; escapes are not processed within them.

## Function types and handles

A bare `fn` accepts any validated function handle. A parameterized function type records its signature:

```ember
fn add_one(x: i64) -> i64 { return x + 1; }

fn apply(f: fn(i64) -> i64, value: i64) -> i64 {
    return f(value);
}

fn handle_demo() -> i64 {
    let h: fn(i64) -> i64 = &add_one;
    return apply(h, 41);
}
```

`&name` produces a logical dispatch-slot handle, not a raw code address. Linked modules also support `&module::name`. Calls through typed handles are checked against the recorded signature and guarded by the runtime dispatch table.

A lambda has a closure type represented by a function slot and environment. See [Expressions and Operators](40-expressions-operators.md#lambdas).

## Managed pointers

`new T` returns an opaque GC-managed pointer. Ember intentionally provides no dereference or pointer-arithmetic syntax; managed pointers are used for lifetime/rooting and host-native interoperability. `delete value` requests deterministic deletion.

```ember
fn allocation_count() -> i64 {
    let p = new i64;
    let before: i64 = gc_live();
    delete p;
    return before;
}
```

## Host-defined opaque handles

The standalone host registers named opaque handle types including `string`, `vec2`, `vec3`, `vec4`, `quat`, and `mat4`. Their ABI representation is host-owned; script code uses constructors, accessors, method-call sugar, and registered operator overloads.

The dynamic array, map, synchronization, thread, coroutine, lifecycle, module, and audio APIs primarily expose opaque `i64` handles. An `i64` handle is not interchangeable with a typed `string` or vector handle unless a native signature explicitly says so.

## Summary

| Category | Examples | Representation |
|---|---|---|
| scalar | `i8`…`u64`, `f32`, `f64`, `bool` | value |
| struct | `Point` | tightly packed value |
| enum | `State`, `Status : u8` | nominal integer value |
| fixed array | `i32[4]` | inline value storage |
| slice | `i32[]` | borrowed pointer + length |
| string/vector handles | `string`, `vec3`, `mat4` | typed host-owned handle |
| function handle | `fn(i64) -> i64` | logical dispatch slot |
| lambda | inferred closure type | slot + environment |
| managed pointer | inferred from `new T` | opaque GC pointer |

Continue with [Declarations](20-declarations.md).
