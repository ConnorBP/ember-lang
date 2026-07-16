# Fibonacci

This complete example uses recursion, iteration, `defer`, a global, and standalone I/O. Save it as `fibonacci.ember`.

```ember
global sequences_completed: i64 = 0;

fn fib_recursive(n: i64) -> i64 {
    if (n <= 1) { return n; }
    return fib_recursive(n - 1) + fib_recursive(n - 2);
}

fn fib_iterative(n: i64) -> i64 {
    if (n <= 1) { return n; }

    let mut previous: i64 = 0;
    let mut current: i64 = 1;
    for (let mut i: i64 = 2; i <= n; i += 1) {
        let next: i64 = previous + current;
        previous = current;
        current = next;
    }
    return current;
}

fn mark_complete() -> i64 {
    sequences_completed += 1;
    return 0;
}

fn print_sequence(count: i64) -> i64 {
    defer mark_complete();
    for (let mut i: i64 = 0; i < count; i += 1) {
        print_i64(fib_iterative(i));
        println("");
    }
    return 0;
}

fn main() -> i64 {
    for (let mut i: i64 = 0; i <= 15; i += 1) {
        if (fib_recursive(i) != fib_iterative(i)) {
            return 1;
        }
    }

    print_sequence(10);
    if (sequences_completed != 1) { return 2; }

    print_i64(fib_iterative(30));
    println("");
    return 0;
}
```

Run it:

```console
build\ember_cli.exe run fibonacci.ember --fn main --ffi
```

It prints the first ten Fibonacci values and then `832040` (`fib(30)`).

## Why two implementations?

`fib_recursive` mirrors the mathematical definition and is easy to inspect. `fib_iterative` avoids exponential recomputation and unboundedly growing recursion for practical calls. `main` compares both implementations over the first sixteen inputs without relying on host-specific assertion natives.

## The deferred completion marker

`defer mark_complete();` runs when `print_sequence`'s block exits normally. It increments the global exactly once, regardless of how many values the loop prints. The subsequent explicit `if` verifies that behavior.

A runtime trap or exception unwind would skip the defer; this example has no such path during valid execution.
