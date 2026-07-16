# Bubble Sort

Save this complete example as `bubble_sort.ember`.

```ember
fn is_sorted(values: i64[]) -> bool {
    let mut i: i64 = 0;
    while (i + 1 < 8) {
        if (values[i] > values[i + 1]) { return false; }
        i += 1;
    }
    return true;
}

fn main() -> i64 {
    let mut values: i64[8] = [5, 2, 9, 1, 7, 3, 8, 4];
    if (is_sorted(values[..])) { return 1; }

    for (let mut pass: i64 = 0; pass < 7; pass += 1) {
        for (let mut i: i64 = 0; i < 7 - pass; i += 1) {
            if (values[i] > values[i + 1]) {
                let temporary: i64 = values[i];
                values[i] = values[i + 1];
                values[i + 1] = temporary;
            }
        }
    }

    if (!is_sorted(values[..])) { return 2; }
    if (values[0] != 1 || values[7] != 9) { return 3; }

    for (value in values[..]) {
        print_i64(value);
        println("");
    }
    return 0;
}
```

Run it:

```console
build\ember_cli.exe run bubble_sort.ember --fn main --ffi
```

It prints `1, 2, 3, 4, 5, 7, 8, 9`, one value per line.

## Fixed array and slice view

`values` owns eight inline `i64` elements. `values[..]` borrows all of them as an `i64[]` for `is_sorted` and the final for-each. Dynamic indexing is bounds-checked. The loop limits guarantee that `i + 1` never exceeds index 7.

## Nested passes

Each outer pass moves the largest remaining unsorted value to its final position. The inner upper bound shrinks by one because the tail is already sorted. Swapping requires a temporary so the first assignment does not destroy the old left value.

This example uses explicit `if` checks rather than `assert_eq_*`, because those assertion natives are host-specific and not registered by standalone `ember_cli`.
