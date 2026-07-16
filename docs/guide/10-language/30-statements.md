# Statements

## Blocks and scope

A braced block creates a lexical scope. Inner bindings may shadow outer bindings; redeclaring a name in the same scope is an error.

```ember
fn scope_demo() -> i64 {
    let x: i64 = 1;
    {
        let x: i64 = 2;
        if (x != 2) { return 0; }
    }
    return x;
}
```

## `if` / `else`

```ember
fn report_health(health: i64) -> void {
    if (health <= 0) {
        println("dead");
    } else if (health < 25) {
        println("critical");
    } else {
        println("ok");
    }
}
```

The condition must be `bool`; integers are not truthy.

## `while` and `do-while`

```ember
fn loop_demo() -> i64 {
    let mut i: i64 = 0;
    while (i < 3) {
        i += 1;
    }

    do {
        i -= 1;
    } while (i > 0);
    return i;
}
```

`while` checks before the body. `do-while` runs the body at least once and requires the trailing semicolon.

## C-style `for`

```ember
fn print_ten() -> void {
    for (let mut i: i64 = 0; i < 10; i += 1) {
        print_i64(i);
    }
}
```

Initialization runs once, the `bool` condition runs before each iteration, and the step runs after the body. Any clause may be empty; `for (;;) { ... }` is an infinite loop. A `continue` executes the step before rechecking the condition.

## For-each

For-each accepts a slice or an array handle whose element type can be inferred from `array_new`:

```ember
fn sum_slice(values: i64[]) -> i64 {
    let mut total: i64 = 0;
    for (value in values) {
        total += value;
    }
    return total;
}

fn sum_handle() -> i64 {
    let values = array_new(8, 2);
    array_set_i64(values, 0, 20);
    array_set_i64(values, 1, 22);
    let mut total: i64 = 0;
    for (value in values) {
        total += value;
    }
    return total;
}
```

Iterate a fixed array by converting it to a slice (`values[..]`).

## `switch`

```ember
fn report_kind(kind: i64) -> void {
    switch (kind) {
        case 0:
            println("none");
            break;
        case 1:
        case 2:
            println("active");
            break;
        default:
            println("unknown");
            break;
    }
}
```

The subject is integer or `bool`. Case labels are unique compile-time constants, including constants produced by `constexpr fn` calls.

**Implicit fallthrough is forbidden.** Every nonempty case must end in `break`, `continue`, or `return`; sema rejects a case that falls into the next one. Empty adjacent labels may share a body as shown above. Return-path analysis remains conservative, so a non-void function ending in a `switch` still needs a trailing return.

## `match`

`match` uses no-fallthrough arms. It supports integer, `bool`, enum, wildcard, and struct-destructure patterns; an optional `if` guard follows a pattern.

```ember
struct Point { x: i64; y: i64; }

fn classify(p: Point) -> i64 {
    match (p) {
        Point{x, y} if x > 0 => { return 1; },
        Point{x, y} if y > 0 => { return 2; },
        _ => { return 0; }
    }
    return 0;
}
```

For scalar subjects, write literal or enum patterns:

```ember
enum State { Idle, Running }

fn state_value(state: State) -> i64 {
    let mut result: i64 = -1;
    match (state) {
        State::Idle => { result = 0; },
        State::Running => { result = 1; },
        _ => { result = -1; }
    }
    return result;
}
```

A match need not be exhaustive; without a matching arm it simply exits. Consequently, return-path analysis requires a trailing return even when every written arm returns.

## Exceptions: `try`, `catch`, and `throw`

```ember
fn parse_code(value: i64) -> i64 {
    try {
        if (value < 0) { throw 42; }
        return value;
    } catch (error) {
        return error;
    }
}
```

Thrown values are `i64`. `catch (name)` binds the thrown value in the catch block. A throw reaches the nearest active catch; an uncaught throw traps to the host with `TrapReason::UnhandledThrow`. Try/catch requires the normal per-context execution model and is forbidden in `@realtime` functions.

## Coroutines and `yield`

A function containing `yield` is a coroutine. Start it from a function handle, resume it with `coroutine_next`, and query completion with `coroutine_done`:

```ember
fn counter(start: i64) -> i64 {
    yield start;
    yield start + 1;
    return -1;
}

fn coroutine_demo() -> i64 {
    let c = coroutine_start(&counter, 20);
    let a: i64 = coroutine_next(c);
    let b: i64 = coroutine_next(c);
    let done_value: i64 = coroutine_next(c);
    return a + b + done_value;
}
```

The first `yield` establishes the coroutine's yield type; later yields must match it. `yield;` establishes/uses `void`. The coroutine runtime is implemented with Windows fibers in the current build; non-Windows builds register a stub.

## `defer`

`defer expression;` schedules an expression for lexical block exit. Defers run in last-in, first-out order on normal fallthrough, `break`, `continue`, and `return`.

```ember
fn deferred() -> i64 {
    let mut value: i64 = 40;
    {
        defer value += 1;
        defer value += 1;
    }
    return value;
}
```

A defer inside a reached loop iteration runs on that iteration's exit. Runtime traps and exception `longjmp` unwinds bypass pending defers; `defer` is not a `finally` mechanism.

## `break` and `continue`

`break` exits the nearest loop or `switch`. `continue` advances the nearest loop; a surrounding `switch` does not intercept it. Both are rejected outside a valid enclosing construct.

## `return`

A non-void function uses `return expression;`. A `void` function may use `return;` or fall off its end. Sema verifies type compatibility and that every non-void path returns.

## `static_assert`

```ember
static_assert(2 * 3 == 6, "constant arithmetic failed");
```

The condition is evaluated during sema and must be a compile-time `bool`. The statement emits no runtime code.

## Expression statements

Calls, assignments, compound assignments, and increment/decrement commonly appear as statements:

```ember
fn expression_statements() -> i64 {
    let mut counter: i64 = 0;
    let mut values: i64[1] = [0];
    let index: i64 = 0;
    println("working");
    counter += 1;
    values[index]++;
    return counter + values[index];
}
```

See [Expressions and Operators](40-expressions-operators.md) for their value and type rules.
