# Assertions

> **STALENESS NOTICE (2026-07-11):** `assert_eq_i64`/`assert_eq_f32`/
> `assert_eq_bool`/`assert_eq_str` are **prism-host** natives, not a standard
> ember extension (per `extensions/AUDIT.md` + `extensions/README.md` "What
> stayed in prism"). They route through prism's host print sink + assert-
> failure counter. The standalone `ember` CLI does **not** register them; a
> script calling `assert_eq_*` against the CLI gets "unknown function". The
> standard `ember test` runner (the `ember test [dir]` CLI action) classifies
> `.ember` files by expected outcome instead (a `// expect: N` comment → run,
> expect exit N; `invalid_*` → parse-only, expect fail; etc.). This page
> documents the prism assertion surface for historical context; a host that
> wants in-script assertions registers its own `assert_eq_*` natives.

Assertions are how Ember scripts check their own work while running. There is no separate test runner, no assertion library to import, and no test discovery step: `assert_eq_*` is a small family of native functions, always available, that compare two values and trap the script if they do not match.

> **NOTE:** A trap is not a normal error. When an `assert_eq_*` call fails, it prints `ASSERT FAIL` along with the two values, then halts script execution immediately. There is no catch, no recovery, and no continuing past it.

## The Idiom

Look through any of the example scripts and you will see the same pattern over and over: compute a value, then immediately assert what it should be.

```ember
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn main() -> void {
    let result: i64 = add(2, 3);
    assert_eq_i64(result, 5);
    print_i64(result);
}
```

This is not a unit test in the conventional sense. There is no separate `test_add.ember` file, no test framework attaching to a runner, and no report of "N passed, M failed" at the end. The assertion is inline, in the same function, right next to the computation it is checking. It exists to answer one question as early as possible: did that last step actually do what I expected?

Because of this, the pass/fail signal for an Ember script is binary and structural, not a summary:

- **A script that runs every `assert_eq_*` call it reaches and keeps going has passed.** Execution reaching the end of `main` (or wherever the script's entry point finishes) with no trap is the success condition.
- **A script that traps on any assertion has failed**, full stop, at that exact line. Nothing after the failing assertion runs.

This is why you will see assertions used freely throughout the example scripts even outside of anything that looks like a "test." They document intent inline: "this is what I, the script author, believe to be true at this point," enforced by the host rather than left as a comment.

## Reference

| Function | Signature | Behavior |
|---|---|---|
| `assert_eq_i64` | `(a: i64, b: i64) -> void` | Traps if `a != b`. Exact integer comparison. |
| `assert_eq_f32` | `(a: f32, b: f32) -> void` | Traps if `a != b`. Exact bitwise-equal comparison, no epsilon or tolerance. |
| `assert_eq_bool` | `(a: bool, b: bool) -> void` | Traps if `a != b`. |
| `assert_eq_str` | `(a: u8[], b: u8[]) -> void` | Traps if the two byte slices differ in length or content. |

All four functions return `void`. None of them produce a usable value, so they are always called as a bare statement, never assigned or used in an expression.

## assert_eq_i64

```
assert_eq_i64(a: i64, b: i64) -> void
```

Compares two `i64` values for exact equality. This is the assertion you reach for most often, since integers are the default numeric type for counters, indices, sizes, and most computed results in example scripts.

```ember
let n: i64 = 10;
let mut total: i64 = 0;
for (let mut i: i64 = 1; i <= n; i += 1) {
    total += i;
}
assert_eq_i64(total, 55);
```

## assert_eq_f32

```
assert_eq_f32(a: f32, b: f32) -> void
```

Compares two `f32` values for exact equality, bit for bit. There is no epsilon, no tolerance window, and no rounding applied before the comparison.

> **WARNING:** Be careful asserting on computed floats. Floating-point arithmetic accumulates rounding error, so two values that are "mathematically" equal (the same expression written two different ways) can differ in their last bit and fail this assertion. Reserve `assert_eq_f32` for values that are exact by construction, such as a literal round-tripped through a function, or a result derived only from operations that do not introduce rounding (integer-valued additions, multiplications by powers of two). If you need to compare a genuinely computed float, compute your own tolerance check with ordinary comparison operators instead of reaching for `assert_eq_f32`.

```ember
let half: f32 = 1.0 / 2.0;
assert_eq_f32(half, 0.5);
```

## assert_eq_bool

```
assert_eq_bool(a: bool, b: bool) -> void
```

Compares two `bool` values. Commonly used to pin down the result of a comparison or logical expression rather than repeating the condition inline.

```ember
let x: i64 = 7;
let is_odd: bool = (x % 2) != 0;
assert_eq_bool(is_odd, true);
```

## assert_eq_str

```
assert_eq_str(a: u8[], b: u8[]) -> void
```

Compares two byte slices for equality, checking both length and content. Since `string` is itself a handle over an underlying byte buffer, this is the assertion used to check string results, typically after a `string_*` operation such as concatenation or slicing.

```ember
let greeting: string = "hello, " + "world";
assert_eq_str(greeting, "hello, world");
```

See [Strings](30-strings.md) for the full `string_*` reference and how `string` relates to `u8[]`.

## Placement and Style

Assertions read best placed immediately after the value they check, not batched at the end of a function. This keeps the failure line close to the computation that produced the bad value, so a trap message points you straight at the relevant step instead of a summary line far away from the cause.

```ember
fn factorial(n: i64) -> i64 {
    let mut result: i64 = 1;
    for (let mut i: i64 = 2; i <= n; i += 1) {
        result *= i;
    }
    return result;
}

fn main() -> void {
    assert_eq_i64(factorial(0), 1);
    assert_eq_i64(factorial(1), 1);
    assert_eq_i64(factorial(5), 120);
    print_i64(factorial(5));
}
```

If `factorial(5)` ever regresses to returning the wrong value, this script traps on that exact `assert_eq_i64` call before it ever reaches `print_i64`, and the printed `ASSERT FAIL` output shows both the expected and actual values at the point of failure.

---

See [I/O and Debug](10-io-debug.md) for the `print_*` functions used alongside assertions to surface values before a script exits, and [API Overview](00-overview.md) for how this page fits into the rest of the native API surface.
