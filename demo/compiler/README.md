# Compiler demo: a language implemented in Ember

This directory implements a tiny arithmetic/`let` language (µ) in Ember. Its
lexer, recursive-descent parser, flat AST, and tree-walking evaluator are all
`.ember` source. It is a language-feature demo, distinct from the production
self-hosted compiler under `self_hosted/`.

## µ grammar

```text
program  := stmt { ';' stmt }
stmt     := 'let' ident '=' expr | expr
expr     := term { ('+' | '-') term }
term     := factor { ('*' | '/') factor }
factor   := number | ident | '(' expr ')'
```

Identifiers are one lowercase letter (`a`-`z`). The symbol table is an
`i64[26]`. The implementation supports integer literals, precedence,
left-associative arithmetic, parentheses, bindings, identifier lookup, and
explicit divide-by-zero reporting.

## Files

- `lex.ember`: `Tok`, `Token`, string-backed source scanning, and tokenization.
- `parse.ember`: recursive descent into five parallel `i64[256]` node arrays
  plus a two-cell metadata slice.
- `eval.ember`: recursive evaluator and a typed function-handle call to
  `apply_binop`.
- `main.ember`: allocates the pools, runs five deterministic programs, and
  returns 0 only when every result matches.

## Run

From the repository root:

```bash
./buildt/ember_cli.exe run demo/compiler/main.ember
# exit 0
```

No `--ffi` permission is needed. The stock CLI registers the string extension
used by the source globals.

Expected cases:

| Program | Result |
|---|---:|
| `let a = 1 + 2 * 3; a + 10` | 17 |
| `let a = 2; let b = (a + 10) * 3; b / 4 - 1` | 8 |
| `7 * 6 - 1` | 41 |
| `(2 + 3) * 4` | 20 |
| `100 / 5 / 2` | 10 |

A mismatch returns a distinct nonzero `100 + result`, `200 + result`, and so
on, making failure visible to a harness.

## Implementation choices and current constraints

The local fixed-array pools are an intentional reentrant design, not a current
aggregate-global workaround. Ember now supports aggregate globals and array
literals, direct struct-literal returns, and struct-returning calls used as
aggregate arguments.

The source still demonstrates several current rules:

- locals are immutable unless declared `let mut`;
- slices are passed by value as pointer/length pairs while writes affect their
  backing arrays;
- typed function handles use `fn(Args) -> Ret` and can cross the four-register
  Win64 boundary; stack-argument handling is covered by current codegen tests;
- switch case bodies in this demo use direct statement form, and the functions
  retain a trailing return because switch exhaustiveness is not yet used by
  the missing-return analysis;
- import directives must occupy their own line. The current resolver regex does
  not accept a trailing `//` comment on an `import` line.

The prior global-string initialization bug is fixed by module initialization;
`mu_program` and `mu_program2` are initialized before `main` executes.
Function-handle calls with 5+ arguments are also no longer a documented broken
path: current tree and ThinIR codegen both marshal stack arguments, with
coverage in `codegen_coverage_test` and keyed-dispatch tests.

## What this proves

The demo exercises enums, packed structs returned by value, switch, strings,
fixed arrays, slices, recursion, global initialization, imports, and typed
function handles in one deterministic program. For the complete self-hosting
and two-generation bootstrap work, see `../../self_hosted/` and the root
README.
