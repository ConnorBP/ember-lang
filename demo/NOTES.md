# Multi-file language demo — notes & kinks surfaced

A large multi-file Ember program (`demo/main.ember` → 4 modules, recursive + dotdot imports, structs by value across modules, slices, f64, unsigned/narrow, defer per-iteration, fn-reference handles, switch, recursion, f-strings, the string extension) built to stress-test the bundler, modules, and language features together.

## Status: works end-to-end

- **Interpreter**: `ember run demo/main.ember` → returns `777` (exit 9 = 777 mod 256). Every feature assertion passes.
- **Bundler / cross-process** (`demo/main_em.ember` — a fn-ref-free variant): `ember emit-em demo/main_em.ember demo/main_em.em` writes an 8851-byte v2 module (22 fns, 4 globals, entry slot 21); `ember run --load-em demo/main_em.em` loads and runs it from a fresh process → returns `888` (exit 120). The portable subset (structs, slices, f64, unsigned, defer, recursion, f-strings, initialized globals, the string extension) round-trips through `.em` and re-executes cross-process. H12 portability + H14 signatures hold.

## Kinks surfaced by the demo (the real deliverable)

### Worked around in the demo (language limitations, not bugs)

1. **No aggregate global initialization** (audit M11, deliberately rejected). Globals accept only scalar initializers (`global pi : f32 = 3.14f`), not struct/array literals. Worked around by initializing structs in fns (`v3_up()`, `make_config()`) and scalars at global scope. *A reasonable future feature but tied to aggregate-global storage allocation; left as the audit's open M11.*
   **RESOLVED (first-class struct / aggregate pass, 2026-07-10):** aggregate globals now ship — `global cfg : Config = Config { name_id: 42, ... };`, `global arr : i64[4] = [1, 2, 3, 4];`, and slice globals all type-check, initialize, load, and store. The globals table now carries typed per-global offsets/sizes (slices 16 bytes ptr+len, structs/arrays their full layout). The in-fn init workaround still compiles but is no longer required. Pinned by the non-circular `aggregate_global_test` ctest probes [1]-[8] (struct/array/slice global read + by-value arg/return + `.em` round-trip with relative-ptr slice relocation). Documented in `../docs/spec/TYPE_SYSTEM.md` §12.2 and `../docs/spec/CODEGEN_SPEC.md` §16.

2. **No fixed-array literals** (`[0, 0, 0]`). Arrays must be initialized element-by-element (`arr[0] = 10;`). Surfaced cleanly; worked around in `make_arr_and_sum`.
   **RESOLVED (first-class struct / aggregate pass, 2026-07-10):** array literals are now first-class expressions — `[a, b, c]` constructs a fixed array (`let arr: i64[3] = [10, 20, 30];`) or a slice (`let s: i64[] = [1, 2, 3];` allocates a backing store and yields ptr/len). The declared type is required (no inference); count and element type are checked; empty `[]` is rejected. The element-by-element workaround still compiles but is no longer required. Pinned by the non-circular `array_lit_test` ctest probes [1]-[8] (fixed-array + slice construction, full-i64 storage pinned via the direct C read path). Documented in `../docs/spec/TYPE_SYSTEM.md` §12.1 and `../docs/spec/CODEGEN_SPEC.md` §16.

3. **Struct-return ABI restriction**: a fn cannot `return V3 { x:..., y:..., z:... };` directly — sema requires "a plain local variable or a call to a function with the same struct return type." Worked around by storing the literal in a local first (`let r: V3 = V3 {...}; return r;`).
   **RESOLVED (first-class struct / aggregate pass, 2026-07-10):** a fn may now `return V3 { ... };` directly. Sema accepts a StructLit of the return type as a `return` value (in addition to a bare local and a same-type forwarding call); codegen materializes the literal into a compiler-hidden temp frame slot, then copies it through the hidden return pointer. The via-local workaround still compiles but is no longer required. Pinned by the non-circular `binding_abi_test` probe [2c] (struct-literal return, host reads V3 fields directly). Documented in `../docs/spec/CODEGEN_SPEC.md` §16.

4. **Struct-by-value argument restriction**: a struct-by-value argument must be a plain local variable — you can't pass a struct literal or a struct-returning fn call directly as an argument. Worked around by introducing locals (`let up: V3 = v3_up(); ... make_config(7, 3.0f, up, 255)`).
   **RESOLVED (first-class struct / aggregate pass, 2026-07-10):** a struct-by-value arg may now be a struct literal, a struct-returning fn call, or a bare local — codegen materializes a general-expr struct arg into a compiler-hidden distinct temp frame slot (one distinct temp per arg, never reused within a call) and copies bytes from it into the arg-stash slot. So `v3_dot(v3_up(), v3_up())` works. The introduce-a-local workaround still compiles but is no longer required. Pinned by the non-circular `binding_abi_test` probes [2d] (`v3_dot(v3_up(), v3_up())` — struct-ret-call as arg) and [2e] (nested `v3_shift(v3_up())` — struct-ret-call as arg to a struct-returning call). Documented in `../docs/spec/CODEGEN_SPEC.md` §16.

   Restrictions 3 & 4 were **Win64 hidden-pointer struct-return/arg ABI restrictions** (the named slot was the hidden pointer). They made a vector-math-lib style API verbose. The pass let codegen allocate a hidden temp for struct-literal returns and struct-by-value arg temporaries, so `return V3{...}` and `v3_dot(v3_up(), v3_up())` work — the real codegen change to the struct ABI surface the original entry described (M10-adjacent).

5. **`defer` takes a statement, not a block**: `defer mark(x);` is supported; `defer { ... }` is not. The M5 lexical-defer work shipped the statement form. Worked around by using a side-effecting fn (`defer bump_defer();`) that mutates a global — which is also the pattern the existing M5 runtime tests use.

6. **`defer` can't mutate a captured local by reference**: the defer statement can call a fn, but that fn can't reach the enclosing local (no ref params). So defer cleanup that needs to accumulate uses a **global** (`defer_counter`), not a local. This is consistent with the M5 design but limits `defer`'s ergonomics for the Go-style `defer close(f)` pattern where `f` is a local.

7. **Switch exhaustiveness not recognized**: sema does not treat `default: return ...` as covering all paths — "function not all paths return a value." Worked around with a trailing `return` after the switch (dead code that satisfies sema). *Candidate language fix: sema should treat a switch with a `default` that returns as exhaustive (M7-adjacent).*

8. **fn-reference handles are a distinct `fn` type, not `i64`**: `let h: fn = &inc;` (not `let h: i64`); the handle is called as `h(args)`; a fn-taking-a-fn param is typed `f: fn`. `let h: i64 = &inc` is correctly rejected (a fn is not interchangeable with i64 — verified by `function_refs_test` A2). This is **correct behavior**, not a kink — just a surface I had to learn.

### Real bundler kink found

9. **`.em` writer leaves a 0-byte file on rejection.** When the writer rejects a module as non-portable (e.g. `apply_twice` uses a fn-reference call, which requires process-local allowlist storage — correctly rejected per H12), it prints the error but leaves a **0-byte `demo/main.em`** behind. The loader then fails on it ("file shorter than header"). *Fix: on `em_writer` rejection, delete the output file (or never create it / truncate only on success). Minor, but a real kink the demo surfaced.*

10. **`.em` writer rejects the whole module if ANY fn in the import graph is non-portable** — even an uncalled fn. This is **correct** (a loaded `.em`'s fns are all callable), but it means a module that *contains* a fn-reference handle anywhere can't be serialized, even for the parts that don't use it. Worked around by a separate `buffer_em.ember` without the fn-ref fn. *Not a bug; just a constraint to document.*

## Features confirmed working (cross-module, cross-process)

struct-by-value return + arg (via locals) · slices + views · f64 precision · u8/u64 unsigned + narrow normalization · defer per-iteration (M5) · fn-reference handles (H10) · switch (M7 no-fallthrough) · recursion (depth guard) · f-strings (D5) · string extension · recursive + dotdot imports · `.em` v2 emit/load/run cross-process · H12 initialized globals (`pi`) surviving serialization · H14 export signatures · bounds-check trap (H2-style) catching a length mismatch in dev.
