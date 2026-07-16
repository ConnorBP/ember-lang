# Declarations

Ember declarations introduce functions, locals, globals, structs, enums, and namespaces. Bindings are immutable unless marked `mut`.

## Functions

```ember
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn announce(message: string) -> void {
    println(message);
}
```

The return clause may be omitted for `void`. Every reachable path of a non-void function must return a compatible value.

Parameters use `name: type`. Parameters themselves are immutable; copy one into a `let mut` local when mutation is needed. Structs, slices, typed function handles, and other supported values can cross function boundaries.

### Default arguments

Trailing parameters may have literal defaults:

```ember
fn scale(value: i64, factor: i64 = 2) -> i64 {
    return value * factor;
}

fn default_demo() -> i64 {
    return scale(21) + scale(2, 3);
}
```

Defaults must be literals, and every parameter after the first defaulted parameter must also have a default.

### `priv` and `constexpr`

`priv fn` remains callable within its module but is omitted from the exported function surface.

`constexpr fn` is a normal runtime-callable function that is also evaluated by sema when all arguments are compile-time constants:

```ember
constexpr fn square(n: i64) -> i64 {
    return n * n;
}

priv fn helper() -> i64 { return square(7); }
```

Constexpr evaluation is bounded. A call with nonconstant arguments falls back to an ordinary runtime call.

## Local bindings

| Form | Meaning |
|---|---|
| `let x: T = expr;` | immutable, explicit type |
| `let mut x: T = expr;` | mutable, explicit type |
| `let x = expr;` | immutable, inferred type |
| `let mut x = expr;` | mutable, inferred type |
| `auto x = expr;` | immutable, inferred type |
| `auto mut x = expr;` | mutable, inferred type |
| `const x: T = expr;` | immutable compile-time constant |

An explicitly typed `let` may omit its initializer; storage is zero-initialized. Inferred declarations require an initializer.

```ember
fn locals() -> i64 {
    let immutable: i64 = 10;
    let mut counter = 0;
    auto mut doubled = immutable * 2;
    const LIMIT: i64 = 3;

    while (counter < LIMIT) {
        doubled += 1;
        counter += 1;
    }
    return doubled;
}
```

A bare string literal with no expected type infers `string`. Use an explicit `u8[]` annotation when a byte slice is wanted.

## Globals and top-level constants

A mutable global uses `global`; an immutable compile-time global uses `const`:

```ember
global requests: i64 = 0;
const MAX_REQUESTS: i64 = 100;

fn record_request() -> i64 {
    requests += 1;
    return requests;
}
```

Global storage lives for the loaded module's lifetime. Scalar, struct, fixed-array, slice, and handle-typed globals are supported. Initializers are materialized into the module's global data/load initialization; use compile-time expressions for deterministic baked values and initialize runtime handles from an entry function when construction requires a native call.

## Struct declarations

```ember
struct Header {
    kind: u8;
    length: u32;
}

fn make_header() -> Header {
    return Header { kind: 1, length: 64 };
}
```

Fields end in semicolons. Struct literals name every initialized field as `field: expression`. Struct types are nominal and use Ember's tightly packed layout.

## Enum declarations

```ember
enum Mode {
    Off,
    Warmup = 10,
    Running
}

enum Error : u16 {
    None = 0,
    Invalid = 7
}

fn enum_demo() -> i64 {
    let e: Error = Error::Invalid;
    return e as i64;
}
```

Typed enums require an integer backing type. Explicit variant values must be compile-time integer expressions; omitted values auto-increment.

## Namespaces

A namespace qualifies its declarations with `Name::`:

```ember
namespace Math {
    const Answer: i64 = 42;

    fn twice(x: i64) -> i64 {
        return x * 2;
    }
}

fn namespace_demo() -> i64 {
    return Math::twice(5) + Math::Answer;
}
```

Namespaces can contain functions, globals/constants, structs, enums, and static assertions. They are compile-time qualification, not runtime objects.

## Static assertions

`static_assert` is valid at top level or in a function and emits no runtime code. Its current constant folder covers literal integer/boolean arithmetic, comparisons, logic, and foldable `constexpr fn` calls; `sizeof`/`offsetof` are compile-time-valued elsewhere but are not accepted directly by this assertion folder:

```ember
static_assert(1 + 1 == 2, "constant folding must work");

fn checked() -> i64 {
    static_assert(offsetof(Header, length) == 1, "Header is tightly packed");
    return 0;
}
```

The condition must fold to a compile-time `bool`; a false condition reports the supplied message.

## Function annotations

Annotations are metadata attached to functions:

```ember
@entry
fn start() -> i64 { return 1; }

@on_tick
fn tick() -> void { }

@event("player_hit")
fn on_player_hit() -> void { }
```

Lifecycle annotations (`entry`, `on_tick`, `event`, `on_reload`) are queried and dispatched by a host. `@realtime` asks sema to enforce the realtime-safe subset: allocation, blocking/threading, I/O, exceptions, and unproven calls are rejected. Optimization/obfuscation annotations such as `@obf("mba")` and `@obf_keyed` are consumed by hosts or configured compiler pipelines that enable those facilities.

Annotations accept literal arguments and apply only to functions.

## Imports and links

`import "path.ember";` is a pre-lex textual source inclusion handled by the host's import resolver.

`link "module.em" as module;` links a compiled module and enables qualified calls and handles such as `module::run()` and `&module::run`. Link availability depends on the embedding host's module registry.

Continue with [Statements](30-statements.md).
