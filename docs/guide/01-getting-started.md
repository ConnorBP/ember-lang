# Getting Started

This page uses the standalone CLI built by this repository. Commands are shown from the Ember repository root.

## Build

A typical Windows build is:

```console
cmake -S . -B build -G Ninja
cmake --build build
```

The resulting runner is `build/ember_cli.exe`. Run `build/ember_cli.exe --help` for every command and option.

## Hello, world

Save this as `hello.ember`:

```ember
fn main() -> i64 {
    println("Hello, world!");
    return 0;
}
```

Run it with I/O permission enabled:

```console
build\ember_cli.exe run hello.ember --fn main --ffi
```

`println` belongs to the I/O extension, whose natives require `PERM_FFI`; the CLI grants that permission with `--ffi`. Omitting `--ffi` causes sema to reject the call. A plain computation that uses no gated native does not need the flag.

`main` is a convention, not a reserved entry point. `--fn NAME` selects any compatible zero-argument function; it defaults to `main`. The selected function's `i64` result becomes the CLI process result.

## A complete calculation

```ember
fn main() -> i64 {
    let mut total: i64 = 0;

    for (let mut i: i64 = 1; i <= 10; i += 1) {
        if (i % 2 == 0) {
            total += i;
        }
    }

    print_i64(total);
    println("");
    return 0;
}
```

Run it with the same command. It prints `30` and a newline.

Important rules visible here:

- `let` is immutable; `let mut` permits assignment.
- Conditions must be `bool`. Write `i != 0`, not `if (i)`.
- Numeric operands normally have to be the same type. Use an explicit `as` cast when needed.
- Statements end with semicolons; blocks use braces.

## Useful CLI workflows

```console
# Compile and run one function
build\ember_cli.exe run hello.ember --fn main --ffi

# Compile every classified language test
build\ember_cli.exe test tests\lang

# Precompile to an .em module and load it
build\ember_cli.exe emit-em hello.ember hello.em
build\ember_cli.exe run --load-em hello.em --fn main

# Run with an optimization profile
build\ember_cli.exe run hello.ember --fn main --ffi --profile balanced
```

An `.em` module records its required native bindings and permissions. The load host must register compatible natives and grant the required permissions.

## Self-hosted compiler

The Ember-written compiler can compile and run a source path through one stage:

```console
echo tests/lang/valid_try_catch.ember | build/ember_cli.exe run self_hosted/correctness_tests/file_pipeline_runner.ember --fn run_file --ffi
```

The two-generation bootstrap, where the Ember-written compiler compiles the compiler and the resulting compiler compiles the input, is:

```console
echo tests/lang/valid_try_catch.ember | build/ember_cli.exe run self_hosted/bootstrap.ember --fn main --ffi
```

Both commands return `42` for that test. See [`self_hosted/README.md`](../../self_hosted/README.md) for details.

## Next steps

- [Types](10-language/10-types.md)
- [Declarations](10-language/20-declarations.md)
- [Fibonacci example](30-examples/10-fibonacci.md)
