# Declarations

Ember has seven kinds of declaration: `fn`, `struct`, `global`, `let`, `auto`, `const`, and the `mut`
modifier that attaches to `let`. This page is the full reference for each of them, plus the built-in
function annotations (`entry`, `on_tick`, `event`, `on_reload`, `obf`, `obf_keyed`) and the
default-parameter-values feature. For what can go inside a function body (statements, control flow,
`defer`), see [30-statements.md](30-statements.md).

## Functions (`fn`)

```ember
fn name(param: type, param2: type) -> rettype {
    // body
}
```

The `-> rettype` clause is optional. Omit it for a void function:

```ember
fn log_startup() {
    print_string("starting up");
}
```

Every path through a non-void function must return a value; sema checks this for you at compile time.

```ember
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}
```

Parameters are comma-separated `name: type` pairs. Structs **can** be passed by value across a function call boundary (shipped 2026-07-10). A struct ≤8 bytes is passed in one register; a struct >8 bytes uses the Win64 hidden-pointer by-value path. A native by-value arg is rejected only if its registered struct size exceeds 128 bytes. A fn can `return V3 { ... };` directly and pass a struct literal / struct-returning call as a by-value arg. See `docs/spec/TYPE_SYSTEM.md` §12 + `docs/spec/CODEGEN_SPEC.md` §16.

```ember
struct Rect {
    width: f32;
    height: f32;
}

fn rect_area(r: Rect) -> f32 {
    return r.width * r.height;
}
```

### Default parameter values

A parameter can carry a default value:

```ember
fn greet(name: string, times: i64 = 1) -> string {
    return name;
}
```

A default value must be a bare literal (an int, float, bool, or string literal), not an arbitrary
expression. Writing `b: i64 = a + 1` is a compile error; only `b: i64 = 1` is allowed.

**Trailing-only rule:** once a parameter has a default, every parameter after it must also have one. A
required parameter cannot follow a defaulted parameter.

```ember
fn f(a: i64, b: i64 = 10) -> i64 { return a + b; }   // ok
fn g(a: i64 = 10, b: i64) -> i64 { return a + b; }   // compile error: 'b' has no default
                                                       // but follows a parameter that does
```

At a call site you may omit any trailing run of defaulted arguments; the compiler fills in the
missing ones for you.

```ember
f(5);       // b defaults to 10, same as f(5, 10)
f(5, 20);   // b = 20
```

## Structs

```ember
struct Name {
    field: type;
    field2: type;
}
```

Fields are separated by semicolons, not commas. Struct layout follows **Ember's tight-packed layout**: fields are placed in declaration order at consecutive offsets with no alignment padding and no trailing padding (the offset of each field is the sum of the previous fields' Ember sizes; `build_struct_layouts` in `src/sema.cpp`). This is **not** MSVC/C layout; a host that needs a C-layout struct uses an explicit host-mapped struct (`docs/spec/BINDING_API.md`). Structs cannot
be self-referential (a field whose type is, directly or indirectly, the struct itself is a compile
error), and an empty struct is legal (size 0, align 1).

```ember
struct Point {
    x: i64;
    y: i64;
}

struct Pair {
    a: Point;
    b: Point;
}
```

Structs do not carry methods in the declaration itself; methods only come from host-registered
`TypeBuilder` operator overloads on handle types. Annotations are not supported on structs.

## Globals

```ember
global name: type = literal;
```

A global's initializer must be a compile-time constant literal, evaluated once at load time.

```ember
global g_proc: i64 = 0;
global g_addr: u64 = 0;
```

Annotations are not supported on globals.

## Locals: `let`, `mut`, `auto`, `const`

Inside a function body you have several ways to introduce a local:

| Form | Mutability | Type | Initializer |
|---|---|---|---|
| `let x: type = expr;` | immutable | explicit | required |
| `let x = expr;` | immutable | inferred from `expr` | required |
| `let mut x: type = expr;` | mutable | explicit | required |
| `let mut x = expr;` | mutable | inferred from `expr` | required |
| `auto x = expr;` | immutable | inferred from `expr` | required |
| `const x: type = expr;` | immutable, compile-time constant | explicit | required |
| `const x = expr;` | immutable, compile-time constant | inferred from `expr` | required |

```ember
fn scratch() -> i64 {
    let mut a: i64 = 10;
    auto b = a * 2;        // auto infers i64 from a * 2
    let c = a * 3;         // bare let with no ': TYPE' infers exactly like auto does
    const C: i64 = 100;

    a = a + 1;              // ok, a is mut
    a += 5;                 // ok, compound assignment is still assignment

    return a + b + c + C;
}
```

Omitting `: TYPE` on a `let`, `let mut`, or `const` local falls back to the exact same
type-inference-from-initializer behavior as `auto` - internally these are indistinguishable once
parsed. There is no meaningful difference between `auto x = expr;` and a type-less `let x = expr;`
other than which keyword you typed; both need an initializer to infer from, and both give you an
immutable binding (add `mut` on the `let` form if you need to reassign it - `auto mut` is not valid
syntax, so reach for `let mut x = expr;` when you want a mutable, type-inferred local).

> **NOTE:** `auto` never accepts an explicit `: TYPE` clause at all - `auto x: i64 = 5;` is a parse
> error, not a redundant-but-legal annotation. `auto` only ever has the bare `auto x = expr;` form.
> If you want a declared type visible at the declaration site, use `let x: TYPE = expr;` instead of
> reaching for `auto` with a type annotation bolted on.

`auto` and bare `let`/`const` all require an initializer; there is nothing to infer or bind
otherwise. Leaving one off is a compile error asking you to add either `: TYPE` or an initializer.

### Type Inference Defaults for String Literals

When the initializer is a bare string literal (`"..."` or an f-string) and there is no explicit
`: TYPE` to adapt to - that is, exactly the `auto x = "literal";` / type-less `let x = "literal";`
case - the inferred type defaults to the `string` handle type, not `u8[]`. This is true even though
a string literal used in an explicitly-typed context can also convert to `u8[]` (see
[Types](10-types.md#string-literals-and-the-string-handle)); with no expected type at all to guide
that choice, `string` is the default because it is the far more generally useful type for "a
variable holding a string literal" - a `u8[]`-inferred local could not be passed to any native
expecting `string` (for example `print_string`) without an explicit `: string` annotation added
after the fact.

```ember
auto name = "PRISM";           // inferred as string, not u8[]
print_string(name);            // works directly, no annotation needed

let greeting = "hello";        // same inference, via bare let instead of auto
let combined: string = greeting + " world";
```

If you actually want the `u8[]` slice form from a type-less local, give it an explicit type instead
of relying on inference: `let raw: u8[] = "raw bytes";`.

`const` locals are checked as compile-time constants, distinct from `let` (which just means
"immutable after init," not necessarily foldable). Use `const` when the value needs to be a
compile-time constant, such as a fixed array size or a value you want sema to fold.

```ember
const SIZE: i64 = 8;
let buf: u8[SIZE] = ...;
```

Loop-scoped locals follow the same rules and are common in `for` init clauses:

```ember
for (let mut i: i64 = 0; i < 10; i = i + 1) {
    // ...
}
```

See [30-statements.md](30-statements.md) for `for`, `while`, `do while`, `switch`, and the rest of
what can appear in a function body.

## Annotations

An annotation is an at-sign followed by a name, optionally followed by parenthesized literal
arguments, attached directly above a function declaration:

```ember
@name
fn f() { }

@name("arg1", "arg2")
fn g() { }
```

Annotation arguments must be literals (int, float, bool, or string), same restriction as default
parameter values. Multiple annotations can stack on one function, one per line:

```ember
@obf("mba")
@obf_keyed
fn add_obf(a: i64, b: i64) -> i64 {
    return a + b;
}
```

Annotations attach to functions only; they are not accepted on `struct` or `global` declarations.

### Built-in annotations

| Annotation | Meaning |
|---|---|
| `@entry` | Marks the function that runs once when the script loads. |
| `@on_tick` | Marks a function dispatched once per frame. |
| `@event(name)` | Marks a function as a handler for the named host event. |
| `@on_reload` | Marks a function that runs when the script is reloaded. |
| `@obf(...)` | Requests an obfuscation pass on the function body. |
| `@obf_keyed` | Emits a CPUID-keyed gate at function entry. |

`@entry` and `@on_tick` are the two you will use in nearly every script:

```ember
global g_proc: i64 = 0;

@entry
fn main() -> i64 {
    g_proc = ref_process(1234);
    return 1;
}

@on_tick
fn render() {
    let v: u64 = ru64(g_proc, 0);
    print_u64(v);
}
```

`@entry` runs once on load; returning a nonzero value signals success, zero signals abort. `@on_tick`
runs once per frame for as long as the script is active.

`@event(name)` binds a function to a specific host-fired event by name, for host integrations that
push discrete events instead of a per-frame tick. `@on_reload` binds a function that the host calls
when the script is hot-reloaded, the counterpart to setup done in `@entry`, useful for releasing or
re-acquiring resources across a reload.

`@obf` takes string literal arguments naming which obfuscation passes to apply to that function's
generated code:

```ember
@obf("mba")
fn mul_obf(a: i64, b: i64) -> i64 {
    return a * b + a;
}
```

- `@obf("mba")`: substitutes arithmetic with an equivalent mixed-boolean-arithmetic identity (for
  example, rewriting `add` as `x - (-y)`).
- `@obf("opaque")`: emits an always-taken opaque predicate wrapped around junk instructions.

`@obf_keyed` emits a CPUID-keyed gate at the start of the function, independent of `@obf`:

```ember
@obf_keyed
fn licensed_path() -> i64 {
    return 1;
}
```

`@obf` and `@obf_keyed` can be combined on the same function, and `@obf` can be repeated with
different arguments to stack passes:

```ember
@obf("mba")
@obf("opaque")
@obf_keyed
fn protected_calc(a: i64, b: i64) -> i64 {
    return a * b + a;
}
```

> **NOTE:** Annotation argument literals are still parsed as string tokens, so pass pass names as
> quoted strings: `@obf("mba")`, not `@obf(mba)`.
