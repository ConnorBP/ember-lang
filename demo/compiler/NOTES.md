# demo/compiler — a compiler/interpreter written IN ember, hosting ember

A tiny arithmetic/let/print language ("µ") whose lexer, parser, and
tree-walking evaluator are all ember source. The point is to stress the
pure-compute language surface — enum, struct, switch, slices, recursion,
fn-ref handles, fixed arrays — and show ember can host itself.

## The language

µ is a classic expression language:

```
program  := stmt { ';' stmt }
stmt     := 'let' ident '=' expr | expr
expr     := term { ('+' | '-') term }
term     := factor { ('*' | '/') factor }
factor   := number | ident | '(' expr ')'
```

Identifiers are a single lowercase letter (`a`–`z`), so the symbol table
is a fixed `i64[26]` indexed by `ascii - 'a'`. Statements are `;`-separated.
The last expression statement's value is the program's result.

Supported: integer literals, `+ - * /` with correct precedence and
left-associativity, parentheses, `let` bindings, identifier lookup,
divide-by-zero detection (surfaced as an eval error, not a host trap).

## Files

- `lex.ember`   — the streaming lexer. `enum Tok` for token kinds, `struct
  Token` returned by value (struct-return-via-local ABI), `switch` for
  single-char dispatch, scalar globals for the cursor (aggregate globals
  are rejected — audit M11), `fn`-typed helpers (`is_digit`/`is_alpha`/
  `is_space`). Strings are opaque handles; bytes are read via
  `string_char_at`.
- `parse.ember`  — the recursive-descent parser. `enum NodeK` for AST node
  kinds. The AST is a flat node pool: five parallel slices
  (kinds/vals/left/right/next) plus a 2-element `meta` slice
  (`[count, err]`). All six slices are fn-local fixed arrays in `main`'s
  `run_src` and passed **by value** (ptr,len) to every `parse_*` fn;
  element writes through a by-value slice hit the backing array, so appended
  nodes are visible to the caller. This is the supported shape — slice
  globals are aggregate globals (rejected, M11), and slices derived from
  local arrays can't escape (return/global-store), so the pool must live in
  the fn that drives parse + eval.
- `eval.ember`   — the tree-walking evaluator. `switch` on node kind for
  dispatch. For `BinOp`, it evaluates both children (recursion) and then
  calls a **fn-ref handle** bound to `apply_binop` — the node-eval dispatch
  table (Tier 2 first-class fn-refs: `let h: fn = &apply_binop; h(op, l, r)`).
  The symbol table is a fn-local `i64[26]` passed as a slice.
- `main.ember`   — the driver. Owns the fn-local pools (5 fixed `i64[256]`
  arrays + a `i64[2]` meta + a `i64[26]` symbol table — all fn-local because
  aggregate globals are rejected). Drives lex → parse → eval and asserts
  deterministic results.

## The deterministic result

`main` runs six programs and asserts each:

| program | result |
|---|---|
| `let a = 1 + 2 * 3; a + 10` | 17 (precedence: `a = 7`, then `+10`) |
| `let a = 2; let b = (a + 10) * 3; b / 4 - 1` | 8 (parens, multi-let, div, sub) |
| `7 * 6 - 1` | 41 (bare expr, no lets) |
| `(2 + 3) * 4` | 20 (parens override precedence) |
| `100 / 5 / 2` | 10 (left-assoc division) |

`main` returns `0` on full success, `100 + r1` / `200 + r2` / etc. on a
mismatch (so a wrong result is harness-observable, not a silent 0). Run:

```
./buildt/ember_cli.exe run demo/compiler/main.ember   # → exit 0
```

## Kinks surfaced, and the decisions made

### Worked around in the demo (language surface, not bugs)

1. **No aggregate globals** (audit M11 — deliberately rejected). Globals
   accept only scalar initializers; a `string` global with a literal is now
   supported (see kink #4), but a `struct`/`array`/`slice` global still
   cannot exist. The node pool, the symbol table, and the meta slice are
   all fn-local fixed arrays in `run_src`, passed by value as slices to
   every helper. This is also why the lexer cursor is scalar globals
   (`lx_src`/`lx_pos`/`lx_len`/`lx_err`) and not a struct global.

2. **Slice globals are aggregate globals** — the same M11 rejection. An
   `i64[]` global can't be initialized (or even declared). So the pool
   can't live in a global; it must be threaded as by-value slice args.
   (The first draft tried `global ps_kinds : i64[]` and was rewritten.)

3. **`struct`-return-via-local ABI** (the documented Win64 hidden-pointer
   restriction). `lex_one`/`lex_word`/`blank_token`/`tok`/`tok_err` each
   build the `Token` literal in a local and `return` the local — never
   `return Token { ... };` directly. Same pattern `demo/math/vec.ember`
   uses for `V2`/`V3`.

4. **Switch cases are statement form, not block form.** `case N: return x;`
   is supported; `case N: { return x; }` is rejected with "nonempty switch
   case must end with break, continue, or return" (the block body's
   terminating `return` isn't recognized as the case's terminator). All
   switches in `lex.ember`/`eval.ember` use the statement form. A `tok(kind)`
   helper was added to `lex.ember` so the punctuation switch can be
   `case 43: return tok(Tok::Plus as i64);` rather than a block that builds
   a `Token` inline. (Same kink the multi-file demo's `flag_label` switch
   hits — see `demo/NOTES.md` #7.)

5. **Switch `default:return` is not recognized exhaustive.** Every switch
   that returns from every case still needs a trailing `return` after the
   switch to satisfy sema ("function not all paths return a value"). All
   switches here have one (e.g. `return blank_token();`, `return
   eval_err_sentinel;`).

6. **`let mut` for reassigned locals.** `let ch: i64 = 0; ch = ...;` is
   rejected ("cannot assign to const variable"). The lexer's digit/word
   loops use `let mut ch`. (ember's default is binding-const; `mut` opts
   out.)

7. **`import` lines cannot have trailing comments.** The import resolver's
   regex is `^\s*import\s+"([^"]+)"\s*;\s*$` — it requires only whitespace
   after the `;`. A `// comment` after the `;` makes the regex miss the
   line, so the import is silently NOT inlined and the rest of the file
   parses as if the import line were a top-level statement → "expected
   fn/struct/global at top level". All `import` lines in this demo are
   bare (`import "lex.ember";` with nothing after the `;`). *This is a real
   sharp edge — see kink #5 in "language fixes" below for the candidate
   fix.*

8. **fn-ref calls are kept to ≤ 4 args.** `apply_binop(op, l, r)` takes
   three i64s so the `h(op, l, r)` dispatch stays within the 4-register
   Win64 convention. The first draft's `eval_binop` took 8 args (seven
   slices + an index = 15 words) and the fn-ref call segfaulted. See kink
   #6 in "language fixes" — fn-ref calls with 5+ args are a real broken
   codegen path.

### Language fixes made (with regression tests)

4. **Global string initializers were never baked at load.**
   `eval_global_initializers` (`src/globals.hpp`) folded int/float/bool
   global initializers into the globals block at load, but **not
   `string`-typed globals** — a `string` is an opaque i64 handle (prim I64
   + struct_name "string"), which is none of `is_int`/`is_float`/`is_bool`,
   so it fell through to "left zero". The result: `global g : string =
   "hello";` compiled, but the first script read saw a null handle —
   `string_length(g)` returned 0, `string_char_at(g, i)` returned 0. The
   compiler demo surfaced this (its source programs are global strings).
   **Fix:** added an optional `string_alloc_fn` callback to
   `GlobalInitCtx`; when set, a `string` global with a bare `StringLit`
   initializer is materialized via the callback (the host wires it to
   `ember::ext_string::alloc`) and the returned handle is baked into the
   slot. The callback defaults to null, so hosts that don't wire it keep
   the pre-fix behavior (no regression). Wired in `examples/ember_cli.cpp`.
   **Regression test:** `tests/lang/runtime_global_string_init.ember`
   (asserts length, char-at, empty-string edge, multiple globals, local
   vs global equality), added to `tests/run_lang_tests.sh`'s runtime spec
   list. Gate: 222 passed, 0 failed.

### Candidate language fixes (documented, NOT applied — out of scope for this demo)

5. **`import` lines with trailing comments silently fail to inline.** The
   import regex should tolerate a trailing `// ...` (strip it before
   matching, or relax `\s*$` to `\s*(//.*)?$`). One-line fix in
   `src/import.cpp`. Left for a considered pass — it's a sharp edge that
   every author hits once.

6. **fn-ref calls with 5+ args are broken.** A `fn`-typed handle called
   with 5+ args (past the 4-register Win64 boundary, into the stack-arg
   region) returns nondeterministic garbage. Verified: `fn add5(a,b,c,d,e:
   i64) -> i64` called via `h(1,2,3,4,5)` returns varying wrong values (10,
   202, 106, ...) across runs instead of 15; 1-, 2-, 3-, and 4-arg fn-ref
   calls all work; `add5` called *directly* (not via a handle) returns 15.
   The bug is in the indirect-call path's stack-argument handling (the
   `handle_word` shift in `src/codegen.cpp`'s CallExpr `is_indirect`
   branch likely miscomputes the outgoing-arg offset for word 5+, since the
   direct-call path for 5+ args works). The existing `function_refs_test`
   only covers 1- and 2-arg handle calls, so this was untested. Worked
   around in this demo by keeping the fn-ref call to 3 args. A fix would
   add a 5+-arg fn-ref regression to `function_refs_test` and correct the
   indirect-call stack-arg offset.

## What this proves

ember can host itself: a lexer (enum + struct + switch + string-byte
access), a recursive-descent parser (struct AST + by-value slice pools +
recursion), and a tree-walking evaluator (switch dispatch + recursion +
fn-ref dispatch table + fixed-array symbol table) — all pure ember source,
multi-file, deterministic. A known input program (`let a = 1 + 2 * 3; a +
10`) produces a known result (17) asserted at runtime. The work also
surfaced and fixed one real language bug (global string initializers) and
documented two more (trailing-comment imports; 5+-arg fn-ref calls).
