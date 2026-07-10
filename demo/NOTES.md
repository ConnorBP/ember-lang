# Multi-file language demo — notes & kinks surfaced

A large multi-file Ember program (`demo/main.ember` → 4 modules, recursive + dotdot imports, structs by value across modules, slices, f64, unsigned/narrow, defer per-iteration, fn-reference handles, switch, recursion, f-strings, the string extension) built to stress-test the bundler, modules, and language features together.

## Status: works end-to-end

- **Interpreter**: `ember run demo/main.ember` → returns `777` (exit 9 = 777 mod 256). Every feature assertion passes.
- **Bundler / cross-process** (`demo/main_em.ember` — a fn-ref-free variant): `ember emit-em demo/main_em.ember demo/main_em.em` writes an 8851-byte v2 module (22 fns, 4 globals, entry slot 21); `ember run --load-em demo/main_em.em` loads and runs it from a fresh process → returns `888` (exit 120). The portable subset (structs, slices, f64, unsigned, defer, recursion, f-strings, initialized globals, the string extension) round-trips through `.em` and re-executes cross-process. H12 portability + H14 signatures hold.

## Kinks surfaced by the demo (the real deliverable)

### Worked around in the demo (language limitations, not bugs)

1. **No aggregate global initialization** (audit M11, deliberately rejected). Globals accept only scalar initializers (`global pi : f32 = 3.14f`), not struct/array literals. Worked around by initializing structs in fns (`v3_up()`, `make_config()`) and scalars at global scope. *A reasonable future feature but tied to aggregate-global storage allocation; left as the audit's open M11.*

2. **No fixed-array literals** (`[0, 0, 0]`). Arrays must be initialized element-by-element (`arr[0] = 10;`). Surfaced cleanly; worked around in `make_arr_and_sum`.

3. **Struct-return ABI restriction**: a fn cannot `return V3 { x:..., y:..., z:... };` directly — sema requires "a plain local variable or a call to a function with the same struct return type." Worked around by storing the literal in a local first (`let r: V3 = V3 {...}; return r;`).

4. **Struct-by-value argument restriction**: a struct-by-value argument must be a plain local variable — you can't pass a struct literal or a struct-returning fn call directly as an argument. Worked around by introducing locals (`let up: V3 = v3_up(); ... make_config(7, 3.0f, up, 255)`).

   Restrictions 3 & 4 are **Win64 hidden-pointer struct-return/arg ABI restrictions** (the named slot is the hidden pointer). They make a vector-math-lib style API verbose. *Candidate language change: let codegen allocate a hidden temp for struct-literal returns and struct-by-value arg temporaries, so `return V3{...}` and `v3_dot(v3_up(), v3_up())` work. This is a real codegen change to the struct ABI surface — should be a considered pass, not a drive-by fix. The audit's M10 covers the adjacent area.*

5. **`defer` takes a statement, not a block**: `defer mark(x);` is supported; `defer { ... }` is not. The M5 lexical-defer work shipped the statement form. Worked around by using a side-effecting fn (`defer bump_defer();`) that mutates a global — which is also the pattern the existing M5 runtime tests use.

6. **`defer` can't mutate a captured local by reference**: the defer statement can call a fn, but that fn can't reach the enclosing local (no ref params). So defer cleanup that needs to accumulate uses a **global** (`defer_counter`), not a local. This is consistent with the M5 design but limits `defer`'s ergonomics for the Go-style `defer close(f)` pattern where `f` is a local.

7. **Switch exhaustiveness not recognized**: sema does not treat `default: return ...` as covering all paths — "function not all paths return a value." Worked around with a trailing `return` after the switch (dead code that satisfies sema). *Candidate language fix: sema should treat a switch with a `default` that returns as exhaustive (M7-adjacent).*

8. **fn-reference handles are a distinct `fn` type, not `i64`**: `let h: fn = &inc;` (not `let h: i64`); the handle is called as `h(args)`; a fn-taking-a-fn param is typed `f: fn`. `let h: i64 = &inc` is correctly rejected (a fn is not interchangeable with i64 — verified by `function_refs_test` A2). This is **correct behavior**, not a kink — just a surface I had to learn.

### Real bundler kink found

9. **`.em` writer leaves a 0-byte file on rejection.** When the writer rejects a module as non-portable (e.g. `apply_twice` uses a fn-reference call, which requires process-local allowlist storage — correctly rejected per H12), it prints the error but leaves a **0-byte `demo/main.em`** behind. The loader then fails on it ("file shorter than header"). *Fix: on `em_writer` rejection, delete the output file (or never create it / truncate only on success). Minor, but a real kink the demo surfaced.*

10. **`.em` writer rejects the whole module if ANY fn in the import graph is non-portable** — even an uncalled fn. This is **correct** (a loaded `.em`'s fns are all callable), but it means a module that *contains* a fn-reference handle anywhere can't be serialized, even for the parts that don't use it. Worked around by a separate `buffer_em.ember` without the fn-ref fn. *Not a bug; just a constraint to document.*

## Features confirmed working (cross-module, cross-process)

struct-by-value return + arg (via locals) · slices + views · f64 precision · u8/u64 unsigned + narrow normalization · defer per-iteration (M5) · fn-reference handles (H10) · switch (M7 no-fallthrough) · recursion (depth guard) · f-strings (D5) · string extension · recursive + dotdot imports · `.em` v2 emit/load/run cross-process · H12 initialized globals (`pi`) surviving serialization · H14 export signatures · bounds-check trap (H2-style) catching a length mismatch in dev.
