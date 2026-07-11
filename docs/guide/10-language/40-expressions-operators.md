# Expressions and Operators

This page covers every operator and expression form in Ember: arithmetic, comparison, logical, and bitwise operators, the ternary operator, assignment and compound assignment, increment and decrement, casts, indexing, field access, method-call sugar, the array-to-slice view syntax, `sizeof`/`offsetof`, operator overloading, string concatenation, and f-string interpolation.

> **NOTE:** Ember does not publish a single precise operator-precedence table in this guide. Operators are grouped by category below. When in doubt about how two different categories of operator combine in one expression, use parentheses to make the grouping explicit rather than relying on memorized precedence.

## The Same-Type Rule

Most binary operators in Ember (arithmetic, comparison, bitwise) require both operands to be the exact same type. There is no implicit mixing of, for example, `i32` and `i64`, or `i32` and `f32`, inside a single operator expression. If you need to combine two different types, cast one of them explicitly with `as` (or rely on implicit widening where it applies; see below). This rule is checked by sema and a mismatch is a compile error, not a runtime conversion.

```ember
let a: i32 = 10;
let b: i64 = 20;
// a + b is a compile error: i32 and i64 are different types
let c: i64 = (a as i64) + b;
```

## Arithmetic Operators

`+`, `-`, `*`, `/`, `%`

Both operands must be the exact same type. The result type equals the operand type. Arithmetic is checked for overflow in debug builds (an overflowing add, subtract, or multiply traps) and wraps silently in release builds.

```ember
fn add_two(a: i32, b: i32) -> i32 {
    return a + b;
}

fn average(a: f64, b: f64) -> f64 {
    return (a + b) / 2.0;
}
```

`%` is integer remainder; it requires integer operands of the same type, same as the other arithmetic operators.

## Comparison Operators

`==`, `!=`, `<`, `<=`, `>`, `>=`

Both operands must be the same type. The result is always `bool`. There is no "truthy" integer: Ember has no implicit conversion from an integer to `bool`, so you must write the comparison out explicitly.

```ember
let x: i32 = 0;
// if (x) { ... }        // compile error: i32 is not bool
if (x != 0) {             // correct
    print_i32(x);
}
```

## Logical Operators

`&&`, `||`, `!`

Logical operators take `bool` operands and produce a `bool` result. `&&` and `||` short-circuit: the right operand is not evaluated if the left operand already determines the result.

```ember
fn in_range(v: i32, lo: i32, hi: i32) -> bool {
    return v >= lo && v <= hi;
}

fn should_skip(ready: bool, cancelled: bool) -> bool {
    return !ready || cancelled;
}
```

Because `&&` and `||` short-circuit, it is safe to guard a call behind a condition on the left side:

```ember
fn is_positive_and_even(v: i32) -> bool {
    return v > 0 && (v % 2) == 0;
}
```

## Bitwise Operators

`&`, `|`, `^`, `~`, `<<`, `>>`

Bitwise operators only accept integer operands. The two operands of a binary bitwise operator (`&`, `|`, `^`) must be the same type, following the same-type rule. `~` is unary bitwise-not. For the shift operators `<<` and `>>`, the value being shifted and the shift amount do not need to match types: the shift amount can be any unsigned integer type.

```ember
fn set_flag(flags: u32, bit: u32) -> u32 {
    return flags | (1u << bit);
}

fn clear_flag(flags: u32, bit: u32) -> u32 {
    return flags & ~(1u << bit);
}

fn toggle_flag(flags: u32, bit: u32) -> u32 {
    return flags ^ (1u << bit);
}
```

## Ternary Operator

`cond ? a : b`

The condition must be `bool`. Both arms must be the same type, there is no implicit widening between the two arms even if one would normally widen to the other in isolation, and neither arm may be `void`. The ternary operator is right-associative.

```ember
fn clamp_low(v: i32, lo: i32) -> i32 {
    return v < lo ? lo : v;
}

let mut label: i64 = score > 100 ? label_win : label_lose;
```

## Assignment and Compound Assignment

`=`, `+=`, `-=`, `*=`, `/=`, `%=`

Plain assignment requires the right-hand side to match the left-hand side's type (subject to the same implicit-widening rules that apply everywhere else). Each compound assignment form follows the same-type rule of its underlying binary operator: `x += y` requires `x` and `y` to be the same type, exactly as `x + y` would.

```ember
let mut total: i64 = 0;
total += 10;
total -= 3;
total *= 2;
total /= 4;
total %= 5;
```

## Increment and Decrement

`++`, `--` (prefix and postfix)

These apply only to integer lvalues: locals, struct fields, and indexed array/slice elements. Prefix returns the new (post-modification) value; postfix returns the old (pre-modification) value. Overflow is checked in debug builds.

```ember
let mut i: u32 = 0u;
i++;        // i is now 1, expression value is 0 (old value)
++i;        // i is now 2, expression value is 2 (new value)

let mut arr: i32[4] = [0, 0, 0, 0];
arr[0u]++;  // increments arr[0] in place
```

## Casts and Implicit Widening

Ember distinguishes explicit narrowing casts from implicit safe widening.

**Explicit casts** use the `as` keyword. Use `as` for:

- Narrowing an integer type (`i64 as i32`, `u32 as u16`)
- Converting between signed and unsigned, same or different width (`i32 as u32`, `u8 as i64`)
- Converting between integer and float (`i32 as f64`, `f64 as i32`)
- Converting integer to `bool` or `bool` to integer

```ember
let big: i64 = 300;
let small: i32 = big as i32;

let signed: i32 = -1;
let unsigned: u32 = signed as u32;

let flag: bool = (1 as bool);
let as_int: i32 = flag as i32;
```

**Implicit widening** happens automatically, no `as` needed, for safe same-kind conversions that cannot lose information:

- Signed integer widening: `i8` to `i16` to `i32` to `i64`
- Unsigned integer widening: `u8` to `u16` to `u32` to `u64`
- Float widening: `f32` to `f64`

```ember
fn takes_i64(v: i64) -> i64 {
    return v;
}

let small: i32 = 5;
// takes_i64(small) would be an error if i32->i64 were not implicit
let result: i64 = takes_i64(small as i64); // explicit is always fine too
```

Never implicit: int-to-float or float-to-int in either direction, int-to-bool or bool-to-int in either direction, any narrowing conversion, and struct-to-struct conversion (Ember uses nominal typing; there is no structural coercion between different struct types).

## Indexing

`arr[i]`, `slice[i]`

The index expression must be an integer type - signed or unsigned both work (`i32`, `u32`, `u64`, ...); sema only requires *some* integer type, not specifically an unsigned one.

```ember
let mut values: i32[8] = [1, 2, 3, 4, 5, 6, 7, 8];
let idx: u32 = 2u;
let v: i32 = values[idx];
```

> **WARNING:** Indexing is **not** bounds-checked, at compile time or at runtime. An out-of-range index is undefined behavior: a small stray index silently reads or writes whatever memory happens to sit next to the array on the stack, and a large enough one raises a hardware access violation. There is no compile-time check either, even when both the array's size and the index are constants known at compile time. Double-check index arithmetic yourself; the language will not catch a mistake here for you.

```ember
const N: u64 = 4;
let arr: i32[4] = [10, 20, 30, 40];
let bad: i32 = arr[N];   // compiles without error, despite N == arr's own length - undefined behavior at runtime
```

## Field Access

`struct_val.field`

Plain dot access reads or writes a field on a struct value.

```ember
struct Point {
    x: f32;
    y: f32;
}

fn move_point(mut p: Point, dx: f32, dy: f32) -> f32 {
    p.x += dx;
    p.y += dy;
    return p.x;
}
```

> **NOTE:** Structs **can** be passed by value across a function call
> boundary (shipped 2026-07-10): ≤8 bytes in one register, >8 bytes via the
> Win64 hidden-pointer by-value path, native by-value args up to 128 bytes.
> The earlier "Structs cannot be passed by value in this version" note in
> this guide is **false** — see `docs/spec/TYPE_SYSTEM.md` §12 +
> `docs/spec/CODEGEN_SPEC.md` §16.

## Method-Call Sugar

`obj.method(args)`

Ember desugars method-call syntax at sema time into an ordinary function call, with the receiver inserted as the first argument:

```ember
obj.method(args)
// is exactly equivalent to:
method(obj, args)
```

This is how handle types expose behavior. For example, a `string` handle's `string_length()` method call:

```ember
let report: string = "build finished";
let len: u64 = report.string_length();
// desugars to: string_length(report)
```

is really a call to the free function `string_length(report)`. There is no hidden `this` pointer or separate method-dispatch mechanism; it is purely a syntactic rewrite resolved at compile time.

## View Syntax (Array to Slice)

`arr[..]`

`arr[..]` converts a whole fixed-size array (`T[N]`) into a slice (`T[]`) view over the same elements. This is the only slicing syntax in this version of Ember: there is no partial-range slicing (no `arr[1..3]`), only the full-array view conversion.

```ember
fn sum_slice(s: i32[]) -> i32 {
    let mut total: i32 = 0;
    let mut i: u32 = 0u;
    while (i < s.slice_length()) {
        total += s[i];
        i++;
    }
    return total;
}

fn sum_array() -> i32 {
    let values: i32[5] = [1, 2, 3, 4, 5];
    return sum_slice(values[..]);
}
```

## sizeof and offsetof

`sizeof(type)` and `offsetof(struct_type, field)` are both compile-time constants of type `u64`, folded by the compiler.

```ember
struct Entity {
    id: u64;
    health: f32;
    flags: u32;
}

const ENTITY_SIZE: u64 = sizeof(Entity);
const HEALTH_OFFSET: u64 = offsetof(Entity, health);
```

`offsetof` only works on struct types, and the named field must actually exist on that struct; referencing a nonexistent field is a compile error.

## Operator Overloading

Handle types and struct types can have operators registered against them on the host side (via `TypeBuilder`), covering the arithmetic, comparison, and bitwise operators, plus unary minus and unary `!`. From script code these overloads are used exactly like the built-in operators:

```ember
let a: vec3 = vec3_new(1.0, 2.0, 3.0);
let b: vec3 = vec3_new(4.0, 5.0, 6.0);
let sum: vec3 = a + b;      // resolves to the registered vec3 + vec3 overload
let eq: bool = a == b;      // resolves to the registered vec3 == vec3 overload
```

> **WARNING:** Operator overload resolution is static, decided by sema at compile time from the operand types, never a runtime virtual dispatch. There is no way for two different runtime "kinds" of the same handle type to resolve an operator differently; the overload used is fixed once the types are known at compile time. If no overload is registered for a given operator and pair of operand types, it is a compile error, not a fallback to some default behavior.

## String Concatenation

String literals and string handles both support `+` through the registered `plus` overload on the string type. You can mix literals and handles freely on either side.

```ember
let name: string = "PRISM";
let greeting: string = "Hello, " + name + "!";
let both_literals: string = "foo" + "bar";
```

A bare string literal implicitly converts to a string handle when used where a string handle is expected, such as either side of a `+` against a string, so you never need to manually construct a handle from a literal before concatenating.

## F-String Interpolation

`f"...{expr}..."`

An f-prefixed string literal can contain `{expr}` placeholders. At compile time, an f-string desugars into a chain of formatting calls and evaluates to a `string` handle.

```ember
fn describe(name: string, hp: i32) -> string {
    return f"{name} has {hp} HP remaining";
}

let count: u32 = 3u;
let msg: string = f"retry {count} of {3u}";
```

Any expression valid in that scope can appear inside the braces, including field access, method calls, and arithmetic:

```ember
struct Vec2 {
    x: f32;
    y: f32;
}

fn report_position(p_x: f32, p_y: f32) -> string {
    return f"position: ({p_x}, {p_y})";
}
```

## See Also

- [Types](10-types.md) for the primitive, struct, array, slice, and handle types that these operators act on.
- [Declarations](20-declarations.md) for `let`, `auto`, `const`, and `mut`, which control what an lvalue's mutability rules are for assignment and increment/decrement.
- [Gotchas](50-gotchas.md) for common mistakes with the same-type rule, unsigned indexing, and operator overload resolution.
