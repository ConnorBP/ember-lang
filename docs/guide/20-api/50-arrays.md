# Dynamic Arrays

The array extension stores resizable host-owned byte buffers behind opaque `i64` handles. It is separate from fixed arrays (`T[N]`) and borrowed slices (`T[]`).

## Function reference

| Function | Signature | Description |
|---|---|---|
| `array_new` | `(elem_size: i64, count: i64) -> i64` | allocate zero-filled elements; `0` on invalid size/failure |
| `array_length` | `(handle: i64) -> i64` | element count; invalid handle returns `0` |
| `array_resize` | `(handle: i64, count: i64) -> void` | resize; new bytes are zero-filled |
| `array_set_u8` | `(handle: i64, index: i64, value: i64) -> void` | write low byte |
| `array_get_u8` | `(handle: i64, index: i64) -> u8` | read byte |
| `array_set_f32` | `(handle: i64, index: i64, value: f32) -> void` | write `f32` |
| `array_get_f32` | `(handle: i64, index: i64) -> f32` | read `f32` |
| `array_set_i64` | `(handle: i64, index: i64, value: i64) -> void` | write `i64` |
| `array_get_i64` | `(handle: i64, index: i64) -> i64` | read `i64` |
| `array_push_u8/f32/i64` | `(handle, value) -> void` | append one element |
| `array_pop_u8/f32/i64` | `(handle) -> element` | remove/return final element |
| `array_clear` | `(handle: i64) -> void` | set length to zero |
| `array_remove` | `(handle: i64, index: i64) -> void` | erase one element and shift following elements |

## Element-size discipline

Use exactly one accessor family per handle:

| Element | `elem_size` | Accessors |
|---|---:|---|
| byte | `1` | `_u8` |
| float | `4` | `_f32` |
| integer/managed word | `8` | `_i64` |

The i64 accessors require `elem_size == 8`; they do not truncate into smaller elements. The f32 accessors require `4`, and push/pop functions similarly validate the family.

## Bounds and failure behavior

Dynamic array natives are forgiving rather than trapping:

- invalid/out-of-range reads return zero
- invalid/out-of-range writes and removes do nothing
- popping an invalid or empty array returns zero
- invalid resizes do nothing
- allocation/size-cap failure returns `0` or leaves the original array unchanged

This differs from language fixed-array/slice indexing, which raises a bounds trap.

## i64 example

```ember
fn main() -> i64 {
    let values: i64 = array_new(8, 0);
    array_push_i64(values, 10);
    array_push_i64(values, 20);
    array_push_i64(values, 30);

    array_remove(values, 1);
    let last: i64 = array_pop_i64(values);
    return array_length(values) * 100 + last;
}
```

The result is `130`: removal leaves `[10, 30]`, pop returns `30`, and one element remains.

## Byte buffer and file I/O

```ember
fn write_three_bytes(path: string) -> i64 {
    let bytes: i64 = array_new(1, 0);
    array_push_u8(bytes, 65);
    array_push_u8(bytes, 66);
    array_push_u8(bytes, 67);
    return file_write_bytes(path, bytes);
}
```

`file_read_bytes` returns the same array-handle representation. File operations require `--ffi` in the standalone CLI.

## For-each

Sema tracks handles produced by `array_new` and infers `u8`, `f32`, or `i64` from the element-size argument, enabling for-each:

```ember
fn sum() -> i64 {
    let values = array_new(8, 3);
    array_set_i64(values, 0, 10);
    array_set_i64(values, 1, 20);
    array_set_i64(values, 2, 30);

    let mut total: i64 = 0;
    for (value in values) {
        total += value;
    }
    return total;
}
```

A plain unrelated `i64` is not accepted as an iterable; the compiler must be able to prove the handle's array origin/type.

## Lifetime and GC interaction

The extension owns the backing storage and exposes no script-visible free function. Stores and pushes into 8-byte arrays participate in the GC trace/write-barrier integration, so managed pointers placed in i64 slots can remain rooted while present. Removing/clearing the entries removes those roots.
