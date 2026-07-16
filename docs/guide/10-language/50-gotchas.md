# Gotchas

## Conditions are `bool`

There is no integer truthiness. Write `if (count != 0)`, not `if (count)`.

## Numeric expressions do not use C promotion

Operands generally must have the same type. Safe widening helps at declarations, arguments, and returns, but mixed arithmetic should use an explicit cast:

```ember
fn add_mixed_widths(small: i32, large: i64) -> i64 {
    let total: i64 = (small as i64) + large;
    return total;
}
```

Unsigned-integer/float casts are not in the current cast matrix; route through a supported signed type only when the value range makes that safe.

## Indexing traps instead of producing undefined behavior

Fixed-array and slice indexing is checked. Literal fixed-array mistakes are compile errors; dynamic out-of-range or negative indices trap with `TrapReason::BoundsCheck`. Pending `defer` expressions do not run during trap unwind.

The dynamic `array_*` extension uses different behavior: invalid reads return zero and invalid writes/removes are ignored. It does not raise the language bounds trap.

## Slices are borrowed

`T[]` does not own storage. A local array view cannot be returned, stored globally, or passed to a native/script path that may retain it. The sema escape analysis rejects known unsafe shapes. Use host-owned dynamic arrays or another owning representation when data must outlive a frame.

## `switch` does not fall through

Unlike C, a nonempty case must end in `break`, `continue`, or `return`. Accidental fallthrough is a compile error. Empty case labels may share the next body.

Return analysis is conservative: even a `switch` with a `default` and a return in every case needs a trailing return in a non-void function.

## `match` is not proven exhaustive

A scalar match may omit `_`; no matching arm means the statement does nothing. A function ending in an all-returning match still needs a trailing return.

## `defer` is lexical, but not exception-safe cleanup

Defers run LIFO when their block exits normally or via `return`, `break`, or `continue`. Runtime traps and try/catch's nonlocal unwind skip them. Use host-side ownership/RAII for resources that must survive every failure mode.

## Exceptions carry `i64`

`throw` values and catch bindings are `i64` in the current language. An uncaught throw is a host trap, not an automatically printed exception.

## Coroutines are platform-conditioned

The production coroutine runtime uses Windows fibers. Non-Windows builds currently expose a stub rather than an equivalent `ucontext` implementation. A function containing `yield` is treated as a coroutine and must be driven with `coroutine_start`/`coroutine_next`.

## Function handles are slots, not pointers

`&fn` is a dispatch slot validated at runtime. Do not cast arbitrary integers and expect them to be callable. Prefer parameterized `fn(Args) -> Return` types; bare `fn` deliberately retains weaker compile-time checking for compatibility.

## Capturing lambdas have lifetime constraints

By-value captures copy values into an environment; `fn[&x]` stores a reference to `x`. A by-reference capture cannot safely outlive the captured storage. Escaping closure environments require the GC environment mode configured by the host; immediate local use works with the stack environment path.

## Struct layout is packed Ember layout

Ember inserts no C ABI padding. `struct Packet { tag: u8; value: i64; }` has size 9 and `value` offset 1. Use host-mapped structs through the binding API when interoperating with a C layout.

## Opaque handles are not interchangeable

`string`, vectors, matrices, synchronization objects, and other host types may all use machine-word handles internally, but sema tracks their named types. Do not treat a `string` as an arbitrary `i64` or mix handles from different stores.

## I/O requires permission

The standalone I/O extension is `PERM_FFI`-gated. Pass `--ffi` to CLI runs that call `print`, `println`, `read_line`, `file_*`, or `path_*`. A production host may decline to register the extension entirely.

## `string` and `u8[]` are distinct

A literal adapts according to context, but an existing string handle is not a byte slice. Use `string_from_slice` to create an owned string from bytes. The API intentionally exposes no borrowed pointer into a string handle.

## Dynamic arrays require a consistent element size

Create arrays with `1` for `u8`, `4` for `f32`, or `8` for `i64`, then stay with that accessor family. `array_set_i64` only accepts an 8-byte-element array; it is not a generic truncating write.

## `constexpr` is an optimization opportunity, not a separate runtime language

A `constexpr fn` call folds only when arguments are compile-time constants and the bounded interpreter can evaluate it. Calls with runtime values execute the normal function body.

## Imports and links are different

`import` textually includes Ember source before lexing. `link` connects to a compiled `.em` module through the module registry. Their path, visibility, permission, and handle behavior are not interchangeable.
