# Getting Started

This page walks through the smallest possible Ember program, how a script is run by a host application, and a slightly larger example that introduces `let`, `if`, and `while`.

## Hello, World

Every Ember script needs a `main` function that returns `i64`. When a host runs a script, it calls `main` and treats the returned value as the script's exit code, `0` conventionally means success.

```ember
fn main() -> i64 {
    print_i64(42);
    return 0;
}
```

Save this as `hello.ember`. `print_i64` is a built-in native that writes a 64-bit integer to the host's output. There is nothing else to declare and nothing to import: `main` is found automatically as the script's entry point.

You can print other things just as easily. `print_f32` prints a 32-bit float, and `print_str` prints a raw `u8[]` byte slice (a plain string literal converts to one automatically):

```ember
fn main() -> i64 {
    print_str("Hello, world!");
    return 0;
}
```

Both functions are covered in detail on the I/O and Debug reference page. For now, the important shape to remember is this: a function named `main`, a return type of `i64`, a call to some `print_*` function, and a final `return 0;`.

## Running a Script

How you run a `.ember` script depends on the host application that embeds Ember. Each host provides its own way to load and run scripts, a CLI runner, an editor panel, a hot-reload watcher, or something else, and each host decides where printed output is sent (a console, a log view, stdout, and so on). See your host's documentation for the exact workflow.

Everything else on this page and the rest of this guide is about the script side of that workflow, the language and the API you call into from your `.ember` file, which is the same no matter which host runs it.

## A Slightly Bigger Example

Here is a program that introduces three of the most common statements you will write: `let` for local variables, `if` for branching, and `while` for looping.

```ember
fn main() -> i64 {
    let mut total: i64 = 0;
    let mut i: i64 = 1;

    while (i <= 10) {
        if (i % 2 == 0) {
            total = total + i;
        }
        i = i + 1;
    }

    print_i64(total);
    return 0;
}
```

Walking through it:

- `let mut total: i64 = 0;` declares a mutable local variable named `total` with explicit type `i64`, initialized to `0`. Without `mut`, `let` gives you an immutable binding, reassigning it later would be a compile error.
- `let mut i: i64 = 1;` declares the loop counter the same way.
- `while (i <= 10) { ... }` repeats its body as long as the condition holds. The condition must be a `bool` expression; Ember has no "truthy" integers, so `while (i)` is not legal, you always write an explicit comparison like `i <= 10`.
- `if (i % 2 == 0) { ... }` branches on a `bool` expression the same way `while` does. This example has no `else`, but `else` and `else if` chains work exactly as you would expect.
- `total = total + i;` is a plain assignment to the already-declared `total`. Ember also supports compound forms like `total += i;`.
- `i = i + 1;` advances the loop counter by hand, since this is a `while` loop rather than a `for` loop.

Running this script prints `30`, the sum of the even numbers from 1 to 10.

> **NOTE:** `if` and `while` conditions in Ember are always `bool`. There is no implicit conversion from `i64` (or any other type) to `bool`, so a condition like `i % 2 == 0` must use an explicit comparison operator rather than relying on zero/non-zero truthiness.

## Next Steps

- [10-language/10-types.md](10-language/10-types.md) covers Ember's primitive types, structs, arrays, slices, and handle types like `string` in full.
- [30-examples/10-fibonacci.md](30-examples/10-fibonacci.md) walks through a complete example script that combines recursion, `for` loops, `while` loops, and `defer`.
