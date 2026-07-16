# Expressions and Operators

## Precedence

From lowest to highest:

1. assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` (right-associative)
2. ternary: `?:` (right-associative)
3. logical OR: `||`
4. logical AND: `&&`
5. bitwise XOR: `^`
6. bitwise OR: `|`
7. bitwise AND: `&`
8. equality: `==`, `!=`
9. relational: `<`, `<=`, `>`, `>=`
10. shifts: `<<`, `>>`
11. additive: `+`, `-`
12. multiplicative: `*`, `/`, `%`
13. casts: `as`
14. prefix: `-`, `!`, `~`, `++`, `--`, `&function`, `new`, `delete`
15. postfix: calls, indexing/view, field access, `++`, `--`

Use parentheses when mixed operators would obscure intent.

## Arithmetic and comparison

`+ - * / %` operate on compatible numeric types; `%` is integer remainder. Most binary operations require exactly matching operand types. Safe widening occurs at assignment/call boundaries but is not C-style mixed-expression promotion.

```ember
fn arithmetic(a: i32, b: i64) -> i64 {
    let total: i64 = (a as i64) + b;
    return total / 2;
}
```

`== != < <= > >=` return `bool`. Division or remainder by zero traps. Signed minimum divided by `-1` also traps. Integer overflow behavior follows the selected checked/release compilation mode.

## Logical operators

`!`, `&&`, and `||` require `bool`. `&&` and `||` short-circuit.

```ember
fn safe_check(index: i64, count: i64) -> bool {
    return index >= 0 && index < count;
}
```

## Bitwise and shift operators

`& | ^ ~ << >>` operate on integers. Binary bitwise operands have matching types. The shift amount may be an unsigned integer type. Right shift is arithmetic for signed values and logical for unsigned values.

```ember
fn flags(value: u32, bit: u32) -> u32 {
    return value | ((1 as u32) << bit);
}
```

## Assignment and compound assignment

The complete compound family is:

```text
+=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
```

The target must be a mutable lvalue: a mutable local/global, mutable struct field, or indexed mutable array/slice element. Complex lvalue subexpressions are evaluated once.

```ember
fn assignments() -> i64 {
    let mut x: i64 = 3;
    x *= 4;
    x <<= 1;
    x |= 1;
    return x;
}
```

## Increment and decrement

Prefix and postfix `++`/`--` work on mutable integer lvalues. Prefix returns the new value; postfix returns the old value.

```ember
fn increments() -> i64 {
    let mut x: i64 = 5;
    let old: i64 = x++;
    let current: i64 = ++x;
    return old * 100 + current;
}
```

## Ternary

`condition ? then_value : else_value` requires a `bool` condition and compatible non-void arms.

```ember
fn absolute(v: i64) -> i64 {
    return v < 0 ? -v : v;
}
```

## Casts

Explicit casts use `as`:

```ember
fn cast_demo(wide: i64, count: i64) -> i64 {
    let narrow: i16 = wide as i16;
    let real: f64 = count as f64;
    let integer: i64 = real as i64;
    let flag: bool = count != 0;
    return (narrow as i64) + integer + (flag ? 1 : 0);
}
```

Supported categories include integer width/signedness changes, `f32`/`f64`, signed-integer/float conversions, `bool`-to-integer conversion, and typed-enum-to-backing-integer conversion. Integer-to-`bool` (write `value != 0`), unsigned integer/float casts, and integer-to-enum casts are deliberately rejected in the current type matrix.

## Literals

- decimal/hex integer literals adapt to an expected integer type; integer width suffixes are rejected, so use `as`
- floating literals default to `f64`; an `f` suffix selects `f32`
- `true` and `false` are `bool`
- `"text"` adapts to `string` or `u8[]`
- `[a, b, c]` constructs an explicitly typed fixed array or slice
- `Type { field: value }` constructs a struct
- `Enum::Variant` names an enum value

```ember
struct Point { x: i64; y: i64; }

fn literal_demo() -> i64 {
    let bytes: u8[3] = [10, 20, 30];
    let point: Point = Point { x: 1, y: 2 };
    return (bytes[0] as i64) + point.x;
}
```

Array literals require an explicit target type. Fixed-array literals must have exactly the declared element count.

## Indexing and views

`value[index]` indexes fixed arrays and slices. Every dynamic access is bounds-checked; an invalid index traps. `array[..]` creates a whole-array slice view.

```ember
fn first(values: i64[]) -> i64 {
    return values[0];
}
```

There is no partial range syntax. A slice view borrows backing storage and may not escape a local that would die first.

## Field and method syntax

`value.field` accesses a struct field. `receiver.method(args)` is compile-time sugar for `method(receiver, args)` and is commonly used for handle accessors:

```ember
fn method_demo() -> f32 {
    let v: vec3 = vec3_new(1.0f, 2.0f, 3.0f);
    let x: f32 = v.vec3_x();
    return x;
}
```

Script structs do not declare methods inside their definitions.

## Function handles and indirect calls

`&function_name` creates a logical dispatch handle. Typed handles use `fn(Args) -> Return`; bare `fn` is the backward-compatible untyped form.

```ember
fn double_it(x: i64) -> i64 { return x * 2; }

fn invoke(f: fn(i64) -> i64) -> i64 {
    return f(21);
}

fn handle_use() -> i64 {
    return invoke(&double_it);
}
```

`&module::function` creates a cross-module handle after a `link`. Handles are validated against dispatch tables; they are not arbitrary native addresses.

## Lambdas

A lambda uses function syntax in expression position. Unlisted outer locals are captured by value. An optional capture list marks explicit by-reference captures with `&`:

```ember
fn lambda_demo() -> i64 {
    let base: i64 = 40;
    let add = fn(x: i64) -> i64 { return base + x; };

    let mut current: i64 = 1;
    let read_current = fn[&current]() -> i64 { return current; };
    current = 2;

    return add(read_current());
}
```

A closure is a slot/environment pair and can be called directly. Captures requiring lifetime beyond the creating frame use the GC environment execution mode; do not return a stack-backed closure from a host configuration that has not enabled GC environments.

## F-strings

`f"...{expression}..."` evaluates to `string`. Supported holes include integer, float, `bool`, and `string` expressions; each is lowered to the appropriate `string_from_*` conversion and concatenation.

```ember
fn description(name: string, hp: i64, alive: bool) -> string {
    return f"{name}: hp={hp}, alive={alive}";
}
```

Double braces represent literal braces. Plain/raw literal segments and interpolation expressions are parsed with source-location-aware diagnostics.

## `sizeof` and `offsetof`

Both are compile-time `u64` expressions:

```ember
struct Packet { tag: u8; value: i64; }

fn packet_layout() -> i64 {
    const PACKET_SIZE: u64 = sizeof(Packet);
    const VALUE_OFFSET: u64 = offsetof(Packet, value);
    return (PACKET_SIZE + VALUE_OFFSET) as i64;
}
```

`offsetof` requires a struct type and an existing field.

## `new` and `delete`

`new T` allocates a GC-managed object and returns an opaque managed pointer. `delete expression` destroys a managed object and has `void` type. There is no dereference or pointer arithmetic.

## Operator overloading

Hosts may register operators for named handle types. Resolution is static and based on compile-time operand types. The standard extensions provide:

- `string`: `+`, `==`
- `vec2`/`vec3`/`vec4`: `+`, `-`, `*`, `==`
- `quat`: `+`, `-`, Hamilton `*`, `==`
- `mat4`: matrix `*`, `==`

No script-defined operator declaration syntax exists.
