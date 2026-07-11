# Arrays

The `array_` functions manage a host-owned, dynamically-sized array. This is a separate facility from the language-level array types described in [10-language/10-types.md](../10-language/10-types.md): it is not a fixed array `T[N]` and not a slice `T[]`. There is no syntax sugar for it in the language at all. You allocate it, index it, resize it, and free it (implicitly, on GC/host cleanup) purely through the functions on this page.

> **NOTE:** If you just need a compile-time-sized block of scalars local to a function, use a fixed array `T[N]` instead, see [10-language/10-types.md](../10-language/10-types.md). Reach for `array_` only when the element count is not known until runtime, or needs to grow after creation.

## Why a Separate Handle Type

A fixed array `T[N]` is a value type: its size is baked in at compile time, and it lives inline wherever it is declared (a local, a struct field). A slice `T[]` is a two-word view (pointer plus length) over some existing memory, it cannot grow, and it cannot exist without something backing it.

Neither of those can express "an array whose length is decided at runtime and may change later." The `array_` handle fills that gap. Like `string`, `vec2`, and the other handle types, an array handle is an opaque `i64` naming a host-owned buffer. The script never sees or manipulates its bytes directly, only through the accessor functions below.

## Handle Lifetime

`array_new` returns an `i64` handle. Treat it exactly like any other handle type (`string`, `vec3`, and so on): store it in a `let` or `auto` binding, pass it around by value, and let the host manage the backing memory. There is no explicit free function; the host reclaims the buffer when the handle is no longer reachable.

## Function Reference

| Function | Signature | Description |
|---|---|---|
| `array_new` | `(elem_size: i64, count: i64) -> i64` | Allocates a new array handle with `count` elements of `elem_size` bytes each. |
| `array_length` | `(h: i64) -> i64` | Returns the current element count of the array. |
| `array_resize` | `(h: i64, new_count: i64) -> void` | Resizes the array to `new_count` elements. Newly added elements are zero-filled. |
| `array_set_u8` | `(h: i64, i: i64, v: i64) -> void` | Writes a `u8` element at index `i`. |
| `array_get_u8` | `(h: i64, i: i64) -> i64` | Reads a `u8` element at index `i`. |
| `array_set_f32` | `(h: i64, i: i64, v: f32) -> void` | Writes an `f32` element at index `i`. |
| `array_get_f32` | `(h: i64, i: i64) -> f32` | Reads an `f32` element at index `i`. |
| `array_set_i64` | `(h: i64, i: i64, v: i64) -> void` | Writes an `i64` element at index `i`. |
| `array_get_i64` | `(h: i64, i: i64) -> i64` | Reads an `i64` element at index `i`. |
| `array_push_u8` | `(h: i64, v: i64) -> void` | Appends a `u8` element, growing the array by one. |
| `array_push_f32` | `(h: i64, v: f32) -> void` | Appends an `f32` element, growing the array by one. |
| `array_push_i64` | `(h: i64, v: i64) -> void` | Appends an `i64` element, growing the array by one. |
| `array_pop_u8` | `(h: i64) -> i64` | Removes and returns the last `u8` element. |
| `array_pop_f32` | `(h: i64) -> f32` | Removes and returns the last `f32` element. |
| `array_pop_i64` | `(h: i64) -> i64` | Removes and returns the last `i64` element. |
| `array_clear` | `(h: i64) -> void` | Removes all elements (length → 0). |
| `array_remove` | `(h: i64, i: i64) -> void` | Removes the element at index `i`, shifting later elements down. |

## Choosing `elem_size`

`array_new` takes `elem_size` in bytes, and it is up to you to pass a value consistent with the accessor family you intend to use afterward: `1` for `array_set_u8`/`array_get_u8`, `4` for `array_set_f32`/`array_get_f32` or `array_set_i64`/`array_get_i64` truncated forms, `8` for full-width `array_set_i64`/`array_get_i64`. Mixing accessor families on the same handle (say, writing with `array_set_i64` into an array created with `elem_size = 1`) is not checked by the language and will corrupt or misread adjacent elements. Pick one element type per handle and stay with its matching accessor pair.

## Indexing

The index parameter `i` on every `array_` accessor is a plain `i64`. Slice and fixed-array indexing (`arr[i]`) accepts any integer type for `i`, signed or unsigned, since sema only requires *some* integer type there too. Out-of-range indices on the `array_` handle are a host-side concern (the extension's own bounds handling); `arr[i]` on a slice or fixed array **is bounds-checked at runtime** (a `TrapReason::BoundsCheck` trap, not UB — see `docs/spec/CODEGEN_SPEC.md` §9). Keep `i` within `[0, array_length(h))` (or the array/slice's own length) yourself for the `array_` handle accessors.

## Basic Usage

Allocate an array of four `i64` elements, fill it, and read it back:

```ember
fn sum_array(h: i64) -> i64 {
    let count: i64 = array_length(h);
    let mut total: i64 = 0;
    for (let mut i: i64 = 0; i < count; i += 1) {
        total += array_get_i64(h, i);
    }
    return total;
}

fn build_and_sum() -> i64 {
    let h: i64 = array_new(8, 4);
    array_set_i64(h, 0, 10);
    array_set_i64(h, 1, 20);
    array_set_i64(h, 2, 30);
    array_set_i64(h, 3, 40);
    return sum_array(h);
}
```

`build_and_sum` returns `100`.

## Growing an Array

`array_resize` changes the element count in place; existing elements below the new count are preserved, and any newly added slots are zero-filled. `array_push_u8` is a convenience for the common case of appending single bytes one at a time without computing the new length yourself:

```ember
fn build_byte_buffer() -> i64 {
    let h: i64 = array_new(1, 0);
    array_push_u8(h, 1);
    array_push_u8(h, 2);
    array_push_u8(h, 3);
    return array_length(h);
}
```

`build_byte_buffer` returns `3`.

## Relationship to Fixed Arrays and Slices

| Aspect | `T[N]` fixed array | `T[]` slice | `array_` handle |
|---|---|---|---|
| Kind | value type | view (pointer + length) | opaque `i64` handle |
| Size known at | compile time | runtime, but never grows | runtime, resizable |
| Indexing | `arr[i]`, any integer index | `slice[i]`, any integer index | `array_get_*(h, i)`, `i64` index |
| Backing storage | inline (stack/struct) | somewhere else, borrowed | host-owned, managed for you |

See [10-language/10-types.md](../10-language/10-types.md) for the full treatment of fixed arrays, slices, and the `arr[..]` conversion between them. The `array_` handle family does not participate in that conversion, it is a wholly separate mechanism for cases where compile-time sizing is not an option.
