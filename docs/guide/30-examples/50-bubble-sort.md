# Bubble Sort

> **STALENESS NOTICE (2026-07-11):** the file `examples/scripts/bubble_sort.ember`
> **does not exist** in the tree. The "Full Source" below is illustrative. It
> uses `assert_eq_i64`, a **prism-host** native not registered by the
> standalone `ember` CLI, and repeats two pre-v1.0 claims that are now
> **false**: that indexing is not bounds-checked (it **is** —
> `TrapReason::BoundsCheck`), and that `for` has no for-each form (`for (x in
> slice)` shipped). The algorithm walkthrough itself is still accurate.

This page walks through `examples/scripts/bubble_sort.ember`, a small script that sorts a fixed-size array of
eight `i64` values in place using the classic bubble sort algorithm. It is a compact demonstration of three
things at once: fixed arrays with indexed read and write, C-style nested `for` loops, and the `arr[..]` view
syntax used to pass a fixed array to a function that expects a slice.

## Full Source

```ember
fn is_sorted(a: i64[]) -> i64 {
    let mut i: i64 = 0;
    while (i + 1 < 8) {
        if (a[i] > a[i + 1]) { return 0; }
        i = i + 1;
    }
    return 1;
}

fn main() -> i64 {
    let arr: i64[8];
    arr[0] = 5; arr[1] = 2; arr[2] = 9; arr[3] = 1;
    arr[4] = 7; arr[5] = 3; arr[6] = 8; arr[7] = 4;

    assert_eq_i64(is_sorted(arr[..]), 0);

    for (let mut pass: i64 = 0; pass < 8 - 1; pass = pass + 1) {
        for (let mut j: i64 = 0; j < 8 - 1 - pass; j = j + 1) {
            if (arr[j] > arr[j + 1]) {
                let tmp: i64 = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }

    assert_eq_i64(is_sorted(arr[..]), 1);
    assert_eq_i64(arr[0], 1);
    assert_eq_i64(arr[7], 9);
    return 0;
}
```

## Declaring and Filling the Array

`let arr: i64[8];` declares a fixed-size array of eight `i64` elements. This is a value type: the storage for
all eight elements lives inline, right where `arr` lives, not behind a handle or a pointer. The `8` in `i64[8]`
must be a compile-time constant integer literal, and it must be greater than 0.

Unlike a `let` with an initializer expression, this declaration has no `= ...` on the right side. The array
starts with its elements in an unspecified state, so the script fills every slot explicitly before reading any
of them:

```ember
arr[0] = 5; arr[1] = 2; arr[2] = 9; arr[3] = 1;
arr[4] = 7; arr[5] = 3; arr[6] = 8; arr[7] = 4;
```

Each `arr[i] = value` is an indexed write: an assignment whose left-hand side is an indexing expression instead
of a plain variable name. Reading an element back out, as in `a[i]` inside `is_sorted`, is the same indexing
syntax used as an expression instead of an assignment target. The index literals here (`0`, `1`, `2`, and so on)
are plain `i64` in this snippet; sema only requires an index to be *some* integer type, signed or unsigned, so a
plain `i64` literal (or variable) works directly as an index. Note also that indexing **is bounds-checked at
runtime**: an out-of-range index traps via `TrapReason::BoundsCheck` (a recoverable non-local unwind to the
host), rather than reading/writing past the end. The earlier "indexing is not bounds-checked" claim in this
guide is false — see `docs/spec/CODEGEN_SPEC.md` §9.
For the full rule on index types, see [Types](../10-language/10-types.md).

## The Nested-Loop Sort

The sort itself is the textbook bubble sort, written as two nested C-style `for` loops:

```ember
for (let mut pass: i64 = 0; pass < 8 - 1; pass = pass + 1) {
    for (let mut j: i64 = 0; j < 8 - 1 - pass; j = j + 1) {
        if (arr[j] > arr[j + 1]) {
            let tmp: i64 = arr[j];
            arr[j] = arr[j + 1];
            arr[j + 1] = tmp;
        }
    }
}
```

The outer loop variable `pass` counts sweeps over the array. Each sweep pushes the largest remaining unsorted
element to its final position at the tail, so the inner loop's upper bound shrinks by one element per pass
(`8 - 1 - pass`): the last `pass` elements at the tail are already known to be sorted and do not need to be
re-examined.

The inner loop variable `j` walks adjacent pairs. When `arr[j]` is greater than `arr[j + 1]`, the two elements
are out of order, and the body swaps them using a temporary local:

```ember
let tmp: i64 = arr[j];
arr[j] = arr[j + 1];
arr[j + 1] = tmp;
```

This is the standard three-step swap: without `tmp` holding the original `arr[j]`, the second line would
overwrite `arr[j]` before its old value could be written into `arr[j + 1]`. `tmp` is declared fresh inside the
`if` block, so a new binding is created on every swap and none of it survives past that block's closing brace.

Both `for` loops here desugar internally to `while` loops; Ember's `for` is C-style three-clause (init,
condition, step), **and** also ships a `for (x in slice)` / `for (x in array_handle)` for-each form (see
[Statements](../10-language/30-statements.md)). The manual index loop here is used because the sort needs the
index to shrink the inner bound. See [Statements](../10-language/30-statements.md) for the full
loop and control-flow reference.

## Checking Order With a Slice Parameter

`is_sorted` walks the array once and returns `0` as soon as it finds an adjacent pair out of order, or `1` if it
reaches the end without finding one:

```ember
fn is_sorted(a: i64[]) -> i64 {
    let mut i: i64 = 0;
    while (i + 1 < 8) {
        if (a[i] > a[i + 1]) { return 0; }
        i = i + 1;
    }
    return 1;
}
```

Its parameter is declared `a: i64[]`, a slice, not `a: i64[8]`, a fixed array. `main` never constructs a slice
value by hand; it hands `is_sorted` a view over the array it already has, using the `arr[..]` syntax:

```ember
assert_eq_i64(is_sorted(arr[..]), 0);
```

`arr[..]` converts the whole fixed-size array `arr` into a slice over the same underlying elements. This
conversion is required here because `arr` has type `i64[8]` and `is_sorted` expects `i64[]`: a fixed array and a
slice are different types in Ember, and there is no implicit conversion from one to the other. `arr[..]` is the
only slicing syntax that exists in this version of the language; there is no partial-range form like `arr[1..3]`,
only the full-array view.

The script calls `is_sorted(arr[..])` twice: once before sorting, asserting the result is `0` (the initial data
is not sorted), and once after the nested loops complete, asserting the result is `1`. It then double-checks the
concrete endpoints directly on the array itself:

```ember
assert_eq_i64(arr[0], 1);
assert_eq_i64(arr[7], 9);
```

After the sort, the smallest value (`1`) has settled at index `0` and the largest (`9`) has settled at index `7`,
confirming the eight elements are in ascending order.

## See Also

- [Types](../10-language/10-types.md) for the full distinction between fixed arrays (`T[N]`, value type, inline
  storage) and slices (`T[]`, pointer-and-length view), including the rules on index types.
- [Expressions and Operators](../10-language/40-expressions-operators.md) for the complete `arr[..]` view syntax
  reference, alongside indexing, assignment, and the other operators used in this script.
