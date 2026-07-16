# I/O and Debug

The standalone `io` extension registers all functions on this page with `PERM_FFI`. Run CLI programs that use them with `--ffi`.

## Console

| Function | Signature | Behavior |
|---|---|---|
| `print` | `(text: string) -> void` | writes text without a newline |
| `println` | `(text: string) -> void` | writes text followed by `\n` |
| `print_i64` | `(value: i64) -> void` | decimal integer, no automatic newline |
| `print_f64` | `(value: f64) -> void` | `%g`-style floating output, no automatic newline |
| `read_line` | `() -> string` | reads one stdin line; strips LF and a preceding CR |

`read_line` returns a valid empty string for a blank line and handle `0` for EOF before any bytes are read.

```ember
fn main() -> i64 {
    print("value = ");
    print_i64(42);
    println("");
    print_f64(3.5);
    println("");
    return 0;
}
```

The standalone extension does **not** register `print_str`, `print_f32`, or `print_string`; those are host-specific names. Use `print`/`println` for `string`, and cast/format other values as needed.

## File operations

| Function | Signature | Behavior |
|---|---|---|
| `file_read_bytes` | `(path: string) -> i64` | whole file as an `array<u8>` handle; `0` on failure |
| `file_read_text` | `(path: string) -> string` | whole file as a string, preserving UTF-8 bytes; `0` on failure |
| `file_write_bytes` | `(path: string, data: i64) -> i64` | truncate/create and write an array handle; `1` success, `0` failure |
| `file_exists` | `(path: string) -> i64` | `1` only for an existing regular file |

A zero-byte file returns a valid empty handle from either read function; failure returns handle `0`.

```ember
fn read_source_length(path: string) -> i64 {
    let text: string = file_read_text(path);
    if (text == "") {
        return file_exists(path) == 1 ? 0 : -1;
    }
    return string_length(text);
}

fn copy_bytes(input: string, output: string) -> i64 {
    let bytes: i64 = file_read_bytes(input);
    if (bytes == 0) { return 0; }
    return file_write_bytes(output, bytes);
}
```

`file_write_bytes` expects a dynamic array handle, normally from `file_read_bytes` or `array_new(1, count)`. It does not accept a language-level `u8[]` slice.

## Path operations

| Function | Signature | Behavior |
|---|---|---|
| `path_exists` | `(path: string) -> i64` | `1` for an existing file or directory |
| `path_basename` | `(path: string) -> string` | component after the final `/` or `\` |
| `path_dirname` | `(path: string) -> string` | directory component; `"."` when absent |

The basename/dirname functions are lexical string operations; they do not require the path to exist.

```ember
fn path_demo() -> i64 {
    let path: string = "docs/guide/00-index.md";
    println(path_basename(path));
    println(path_dirname(path));
    return path_exists(path);
}
```

## Running examples

```console
build\ember_cli.exe run script.ember --fn main --ffi
```

A host may omit the I/O extension or replace stdout/stdin behavior. The signatures above describe `extensions/io/ext_io.cpp` as registered by the standalone CLI.
