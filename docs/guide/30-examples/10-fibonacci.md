# Example: fibonacci.ember

This example lives at `examples/scripts/fibonacci.ember`. It is a good first non-trivial script to read end to end: it defines two versions of the same computation, cross-checks them against each other with assertions, and uses a global counter plus `defer` to prove that a piece of cleanup logic ran exactly once. Nothing here touches the host application beyond printing; the whole script runs on plain Ember control flow and arithmetic.

## Full Source

```ember
fn fib_recursive(n: i64) -> i64 {
    if (n <= 1) { return n; }
    return fib_recursive(n - 1) + fib_recursive(n - 2);
}

fn fib_iterative(n: i64) -> i64 {
    if (n <= 1) { return n; }
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    for (let mut i: i64 = 2; i <= n; i = i + 1) {
        let next: i64 = a + b;
        a = b;
        b = next;
    }
    return b;
}

global g_sequences_printed: i64 = 0;
fn note_sequence_done() -> i64 {
    g_sequences_printed = g_sequences_printed + 1;
    return 0;
}

fn print_sequence(count: i64) -> i64 {
    defer note_sequence_done();
    let mut i: i64 = 0;
    while (i < count) {
        print_i64(fib_iterative(i));
        i = i + 1;
    }
    return 0;
}

fn main() -> i64 {
    let mut i: i64 = 0;
    while (i <= 15) {
        assert_eq_i64(fib_recursive(i), fib_iterative(i));
        i = i + 1;
    }
    print_sequence(10);
    assert_eq_i64(g_sequences_printed, 1);
    let fib30: i64 = fib_iterative(30);
    print_i64(fib30);
    return 0;
}
```

## Two Implementations, One Definition

`fib_recursive` is the textbook definition translated directly into Ember: `fib(0) = 0`, `fib(1) = 1`, and every later term is the sum of the two before it. The base case `if (n <= 1) { return n; }` handles both `fib(0)` and `fib(1)` in one branch, and the recursive case adds the two smaller calls together.

`fib_iterative` computes the same sequence with a running pair of accumulators instead of recursion. `a` and `b` start as `fib(0)` and `fib(1)`, and the `for` loop walks forward from index `2` up to `n`, sliding `b` into `a` and the new sum into `b` on each step. Because Ember's `for` is C-style three-clause only (see [Statements](../10-language/30-statements.md)), the loop variable `i` is declared right in the init clause and scoped to the loop.

The two functions exist side by side deliberately. `fib_recursive` is easy to read and obviously correct against the definition, but it does redundant work, recomputing the same sub-terms many times, and its call depth grows with `n`. `fib_iterative` does a fixed amount of work per term and never recurses, so it is the version `print_sequence` actually calls in a loop. Keeping both in the same script gives you a slow-but-obviously-right oracle to check the fast-but-less-obviously-right version against, which is exactly what `main` does.

> **NOTE:** Neither function memoizes. `fib_recursive` on a large `n` will recompute lower terms exponentially many times; it is included here for clarity and cross-checking, not as the version you would call in a hot loop.

## The Counter and Defer Pattern

`g_sequences_printed` is a `global`, meaning it has a single instance for the lifetime of the script and a compile-time constant initializer (`0`). `note_sequence_done` is a small function whose only job is to increment that global and return `0`.

`print_sequence` ties the two together with `defer`:

```ember
fn print_sequence(count: i64) -> i64 {
    defer note_sequence_done();
    let mut i: i64 = 0;
    while (i < count) {
        print_i64(fib_iterative(i));
        i = i + 1;
    }
    return 0;
}
```

The `defer note_sequence_done();` line does not call `note_sequence_done` on the spot. It schedules the call to run when the current scope exits, and `print_sequence`'s body is that scope. Whether the function exits by falling off the end, by an early `return`, by `break`, or by `continue` out of an enclosing loop, the deferred call still fires, exactly once, right before control actually leaves the function. Here there is only one exit path (the final `return 0;`), so the effect is simple: every call to `print_sequence` bumps `g_sequences_printed` by exactly one, no matter how many terms it printed along the way.

This is the pattern to reach for whenever you want "this must happen once, no matter how the function leaves" without duplicating the cleanup call at every `return`. See [Statements](../10-language/30-statements.md) for the full rules on `defer`, including LIFO ordering when a scope has more than one deferred call, and the trap-unwind exception (a hard trap skips pending defers, which does not come up in this script since nothing here can trap).

## Cross-Checking With assert_eq_i64

`main` opens with a loop that walks `i` from `0` through `15` inclusive and checks the two implementations agree at every one of those first sixteen terms:

```ember
let mut i: i64 = 0;
while (i <= 15) {
    assert_eq_i64(fib_recursive(i), fib_iterative(i));
    i = i + 1;
}
```

`assert_eq_i64` compares its two `i64` arguments and traps the script if they differ, rather than returning a `bool` you would have to check yourself. That makes it a compact way to say "these two values must be equal, or stop right here" without any `if` around it. Because the loop covers `i` from `0` to `15`, both `fib(0) = 0` and `fib(15) = 610` (and everything between) get checked, which is enough range to catch an off-by-one in either implementation's base case or loop bound.

The same pattern shows up again right after `print_sequence(10)` returns:

```ember
print_sequence(10);
assert_eq_i64(g_sequences_printed, 1);
```

This assertion is the actual proof that the `defer` pattern above did what it claims: after exactly one call to `print_sequence`, the counter has moved by exactly one. If `note_sequence_done` were called from inside the `while` loop instead of deferred once at the scope level, this assertion would fail, since `print_sequence(10)` prints ten terms and would bump the counter ten times instead. For the full list of `assert_eq_*` variants and how they trap, see [Assertions](../20-api/20-assertions.md).

## Reading main Top to Bottom

Putting it together, `main` runs in three phases: first it proves `fib_recursive` and `fib_iterative` agree on sixteen terms, then it exercises the `defer` counter pattern by printing a sequence of ten terms and asserting the counter moved by exactly one, and finally it computes and prints a single larger term, `fib_iterative(30)`, which is `832040`. Nothing in this script depends on host state or a GUI; it is a pure demonstration of control flow, globals, and `defer`, which is why it is a good first example to step through in a debugger or trace by hand before moving on to scripts that touch host memory or vectors.
