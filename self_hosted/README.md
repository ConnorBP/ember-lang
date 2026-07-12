# Ember self-host preview

`ember_selfhost_preview.exe` is generated in the build directory from
`emberc.ember` using the normal `ember_bundle` tool. It reads one input path
from standard input, compiles the file with the Ember-written frontend and x64
emitter, then executes its `main` function.

```console
echo examples\program.ember | buildt\ember_selfhost_preview.exe
```

## Supported subset

- scalar types: `i64`, `bool`, and `void`
- `let` / `let mut`, `if` / `else`, `while`, C-style `for`, and `return`
- arithmetic, bitwise, comparison, and logical operators
- direct calls with up to four `i64` arguments
- multiple functions, forward calls, and recursion
- an input entrypoint shaped as `fn main() -> i64`
- ASCII input files (the preview converts `file_read_bytes` through the current
  IO-extension byte surface)

Structs, enums, floating point, match, switch, try/catch, lambdas, coroutines
and `yield`, strings, arrays, slices, for-each, defer, function handles,
cross-module calls, and other full-language features are outside this preview.
The self-hosted sema rejects parsed out-of-subset syntax with a clear message
and stable `-3xx` result instead of allowing incorrect code generation.
