# Gotchas

This page collects the places where Ember will surprise someone coming from C, C++, or a similar systems language. None of these are bugs. Each one is a deliberate design decision, usually made for safety or to keep the v1 implementation tractable. Read this page once before you write your first non-trivial script, then come back to it whenever the compiler tells you something you did not expect.

## No Raw Pointers

Ember has no pointer type at all. There is no `T*`, no address-of operator, no pointer arithmetic. The only form of indirection in the language is the slice, `T[]`, a two-word value of pointer and length that the compiler and runtime manage for you.

> **WARNING:** If you reach for `&x` or try to declare a `T*` parameter out of habit, there is nothing to reach for. Ember has no address-of operator and no pointer type. Redesign the function to take a slice (`T[]`) or separate scalar parameters instead.

```ember
fn sum(values: i64[], count: u64) -> i64 {
    let mut total: i64 = 0;
    for (let mut i: u64 = 0; i < count; i += 1) {
        total = total + values[i];
    }
    return total;
}
```

There is no way to take the address of a local, a field, or an array element. If you need to share mutable state across calls, pass a slice into the function and let it write through the slice; that is the only indirection primitive you get.

## Indexing Is Not Bounds-Checked

Despite the slice's `{pointer, length}` shape, neither slice indexing nor fixed-array indexing is bounds-checked at runtime. `arr[i]` compiles to a direct address computation, `base + i * sizeof(T)`, with no comparison against the array's or slice's length anywhere in the generated code.

> **WARNING:** An out-of-range index is not a trap and is not caught at compile time (even when the index is a literal constant that sema could in principle check). It silently reads or writes whatever memory happens to sit past the end of the array or slice, exactly like unchecked pointer arithmetic in C. There is nothing else guarding this: keeping every index inside `[0, length)` is entirely the script's own responsibility.

```ember
let mut buf: i64[4];
buf[0] = 1; buf[1] = 2; buf[2] = 3; buf[3] = 4;
let bad: i64 = buf[10]; // compiles fine; reads past the end of buf at runtime, undefined result
```

An index can be any integer type, signed or unsigned, sema only requires it to be *some* integer type. There is no requirement to use an unsigned type for indexing, that is a convention some example scripts follow, not a rule the compiler enforces.

## Handle Types Are Opaque

`vec2`, `vec3`, `vec4`, `quat`, `mat4`, `string`, and the generic array handle are not structs you can peek inside. Each one is an opaque `i64` handle to host-owned memory. The script never sees the bytes behind the handle and never manages its lifetime; that is entirely the host's job.

> **WARNING:** You cannot read or write the fields of a handle type directly, there is no `.x` on a `vec3` unless a host-registered accessor function or operator overload exposes it. Treat every handle as a black box and go through the registered API.

```ember
let v: vec3 = vec3_new(1.0, 2.0, 3.0);
let x: f32 = vec3_x(v);
```

Because a handle is just an `i64` under the hood, do not cast it `as i64` expecting to inspect a memory address or reinterpret it. The value is meaningless to the script; only the host side knows what it points to.

## Defer Does Not Run on Trap-Unwind

`defer` runs at scope exit, in LIFO order, on `return`, `break`, `continue`, or normal fallthrough out of the block where it was declared. It does not run when the script traps or aborts.

> **WARNING:** Do not rely on `defer` for cleanup that absolutely must happen even on a hard trap (for example releasing a host-side lock or handle). A trap unwinds without running any pending `defer` statements. This is a deliberate v1 limitation.

```ember
fn risky(divisor: i64) -> i64 {
    defer print_string("cleanup ran");
    return 100 / divisor;
}
```

If `divisor` is `0` this traps on the divide, and `cleanup ran` never prints. `defer` is fine for the normal-path cleanup you would write in a destructor or a `finally` block, but it is not a safety net against traps.

## Switch Falls Through Just Like C, With No Compile-Time Check

Case bodies in a `switch` are emitted back to back with no jump inserted between them, so execution falls through from one case into the next exactly like C, unless the case ends in `break`, `return`, or `continue`. Unlike C, there is no `[[fallthrough]]`-style annotation needed to opt in, fallthrough is just the default. There is also no sema check that flags a case missing one of those three terminators: an accidentally-omitted `break` compiles cleanly and silently falls through at runtime.

> **WARNING:** A `case` that just runs its statements and reaches the closing brace falls through into the next case's statements at runtime. This is not caught at compile time. Always add an explicit `break`, `return`, or `continue` at the end of every case that should not fall through, including `default` if it is not last.

```ember
switch (code) {
    case 1: print_string("one"); break;
    case 2: print_string("two"); break;
    default: print_string("other"); break;
}
```

If you are used to C's fallthrough-by-default, Ember behaves the same way, minus any keyword or attribute to make the intent explicit. Omitting `break` is legal and silent, so double-check every case when writing or reviewing a `switch`.

## Switch Is Never Proven Exhaustive

Sema's return-path analysis is deliberately conservative for `switch`. Even if a `switch` has a `default` clause and every single case returns a value, sema still will not treat the switch alone as covering every path through a non-void function.

> **WARNING:** A function whose body ends in only a `switch`, even one with a `default` and a `return` in every case, still needs a trailing `return` statement after the switch. Without it, sema reports a missing-return error on an otherwise complete function.

```ember
fn classify(code: i64) -> i64 {
    switch (code) {
        case 0: return 10;
        case 1: return 20;
        default: return -1;
    }
    return -1;
}
```

That final `return -1;` after the closing brace is unreachable at runtime, since `default` already covers every remaining value, but sema requires it anyway. Always add a trailing return after a switch in a non-void function.

## Operator Overloads Resolve Statically

Operator overloads for handle and struct types are registered host-side through `TypeBuilder`, and sema resolves them statically at compile time based on the operand types. There is no runtime virtual dispatch, and no way for a script to intercept or override an operator at runtime.

> **WARNING:** Do not expect operator resolution to change based on runtime values. `a + b` is bound to one specific host function at compile time, chosen purely from the compile-time types of `a` and `b`. If no matching overload is registered for that exact type pair, it is a compile error, not a fallback to some default behavior.

```ember
let result: string = "score: " + score_to_string(points);
```

The `+` above is resolved at compile time to whichever overload was registered for `(string, string)`. If you pass a type pair that was never registered, for example adding a `vec3` to a `quat` when no such overload exists, the compiler rejects it before the script ever runs.

## Structs Cannot Cross a Function Call Boundary by Value

This is a deliberate scope limit for v1, enforced as a sema error. A script-declared struct cannot be passed by value into a function, and a function cannot return one by value. Functions that logically operate on a struct pass its fields as separate scalar parameters instead.

> **WARNING:** `fn f(p: Point) -> void` does not compile if `Point` is a script-declared struct. Split the struct into its scalar fields at the call site and take those fields as separate parameters.

```ember
struct Point { x: f32; y: f32; }

fn distance(x1: f32, y1: f32, x2: f32, y2: f32) -> f32 {
    let dx: f32 = x2 - x1;
    let dy: f32 = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

let a: Point = Point { x: 0.0, y: 0.0 };
let b: Point = Point { x: 3.0, y: 4.0 };
let d: f32 = distance(a.x, a.y, b.x, b.y);
```

You can still declare and use structs freely for local bookkeeping, `sizeof` and `offsetof` work as expected, but the moment you want to hand one to a function, unpack it into scalars first.

## No Implicit Int-to-Bool

An integer is never a valid condition on its own. `if (x)`, `while (x)`, and the ternary condition all require an actual `bool` expression; there is no "0 is falsy, anything else is truthy" rule anywhere in the language.

> **WARNING:** Writing `if (count)` where `count` is an integer does not compile. You must write the comparison out explicitly, `if (count != 0)`, every time.

```ember
let count: i64 = get_item_count();
if (count != 0) {
    print_string("has items");
}
```

The same applies to loop conditions and the ternary operator's condition. There is an explicit `as bool` cast available for int-to-bool when you really do need that conversion, but it is never inferred for you.

## No Implicit Int-Float Conversion

Arithmetic and comparison operators require both operands to be the exact same type. Mixing an integer and a float in the same expression, even something as small as `i64` plus `f64`, is a compile error, not a silent promotion.

> **WARNING:** `1 + 2.0` does not compile if the `1` is inferred as an integer type and `2.0` as a float type. Cast one side explicitly with `as`, for example `(1 as f64) + 2.0`.

```ember
let count: i64 = 5;
let average: f64 = (count as f64) / 3.0;
```

This also applies to comparisons: comparing an `i64` against an `f64` directly does not compile, cast first. Only the numeric widening conversions (`i8` to `i32` to `i64`, `f32` to `f64`, and the unsigned equivalents) happen implicitly; crossing between the integer and floating-point families always needs an explicit `as` cast.
