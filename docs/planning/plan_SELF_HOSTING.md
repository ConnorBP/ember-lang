# Self-Hosting Plan — ember compiler written in ember

**Status:** IN PROGRESS (foundation laid: demo/compiler/ is a µ-language lexer/parser/evaluator in ember)
**Date:** 2026-07-11

## The north star

An ember compiler written in ember — the language hosting itself. This is NOT a
non-goal (corrected from an earlier roadmap revision). The path is: port the
ember frontend (lexer → parser → sema → codegen) to ember, one stage at a time,
validating each against the C++ reference.

## Foundation already laid

`demo/compiler/` is a complete µ-language lexer/parser/evaluator written entirely
in ember source (`lex.ember`, `parse.ember`, `eval.ember`, `main.ember`). It
proves ember can host a compiler: it lexes, parses, and evaluates a tiny
arithmetic/let/print language with correct precedence, left-associativity,
parens, and divide-by-zero detection. It stresses enum, struct, switch, slices,
recursion, fn-ref handles, fixed arrays, and import — the pure-compute surface
a real compiler needs.

## The path (each stage is a TODO, validated against the C++ reference)

### Stage 1: Port the ember lexer to ember (most shovel-ready)

The C++ lexer is `src/lexer.cpp` + `src/lexer.hpp` — a hand-rolled state machine
that produces a token stream. The demo `lex.ember` already lexes a simple
language. The task: extend it to handle ember's FULL token set (all keywords,
all operators, f-strings, raw strings, char escapes, number literals with
suffixes, comments). The output: a `Token` struct array (using ember's struct +
array extension) that the C++ lexer produces identically.

Validation: run the ember-lexer on the same source → compare the token stream
to the C++ lexer's output. They must match.

Dependencies: the `string` extension (for token text), the `array` extension
(for the token array), the `map` extension (for the keyword lookup). All shipped.

### Stage 2: Port the ember parser to ember

The C++ parser is `src/parser.cpp` — a recursive-descent parser that produces
an AST. The demo `parse.ember` already parses a simple grammar. The task:
extend it to handle ember's FULL grammar (all declarations, statements,
expressions, types, annotations, match, for-each, struct destructure, etc.).

The AST representation in ember: use ember structs for AST nodes (StructDecl,
FuncDecl, Expr, etc.) + a tagged union (enum + struct) for the expression
hierarchy. The output: an AST that the C++ parser produces identically.

Dependencies: Stage 1 (the lexer). The `map` extension (for the operator
precedence table). The struct + enum + fn-ref features (all shipped).

### Stage 3: Port the ember sema to ember

The C++ sema is `src/sema.cpp` — a multi-pass type checker. The task: port the
type-checking passes (resolve types, check exprs, check stmts, enum resolution,
constexpr folding, etc.) to ember.

Dependencies: Stage 2 (the parser/AST). The `constexpr` feature (shipped) for
compile-time evaluation. The type system (struct + enum + fn types — all shipped).

### Stage 4: Port the ember codegen to ember

The C++ codegen is `src/codegen.cpp` (tree-walker) + `src/thin_lower.cpp` +
`src/thin_emit.cpp` (IR backend). The task: port the x64 emitter to ember —
either the tree-walker (direct AST → x64) or the thin-IR path (AST → ThinFunction
→ x64).

This is the hardest stage — ember needs to emit raw x64 bytes (via the array
extension or a new native) + manage the JIT memory (via the jit_memory natives).
The host provides the `alloc_executable_rw` + `free_executable` natives; the
ember codegen fills the buffer with x64 bytes + calls the host to make it
executable.

Dependencies: Stage 3 (sema). The `array` extension (for the byte buffer). The
JIT memory natives (host-provided). The struct layout (for frame planning).

### Stage 5: The self-hosting milestone

Once all 4 stages are ported, the ember compiler written in ember can:
1. Lex ember source → token stream
2. Parse tokens → AST
3. Type-check AST → validated AST
4. Codegen AST → x64 bytes → JIT-execute

At this point, the ember compiler can compile ITSELF — the self-hosting
milestone. The C++ compiler bootstraps the ember compiler (compiles it once),
then the ember compiler can take over.

## What's needed before each stage (gaps to fill first)

### Before Stage 1 (lexer port):
- The string extension needs `string_from_bytes` + `string_char_at` (shipped).
- The array extension needs to hold struct elements (shipped — array<T>).
- ember needs a `Token` struct with a `kind` enum + `text` string + `line`/`col`
  (struct + enum + string handle — all shipped).
- **No gaps — Stage 1 can start now.**

### Before Stage 2 (parser port):
- ember needs a proper AST representation. The struct + enum + array features
  are sufficient, but a tagged-union (enum kind + struct payload) is needed for
  the expression hierarchy. ember doesn't have inheritance (OOP is a non-goal),
  so the AST uses a flat struct with a kind enum + optional fields.
- **No gaps — Stage 2 can start after Stage 1.**

### Before Stage 3 (sema port):
- The type system representation (Type struct with prim/struct_name/is_slice/
  array_len/elem) maps directly to ember structs.
- The constexpr interpreter (shipped) is needed for compile-time folding.
- **No gaps — Stage 3 can start after Stage 2.**

### Before Stage 4 (codegen port):
- ember needs to emit raw bytes: the array<u8> extension (shipped) + a way to
  cast an array<u8> to a function pointer + call it. The host provides
  `alloc_executable_rw` + `free_executable` natives (in the engine). ember
  needs a native to call a raw function pointer (the `fn` handle + the
  call-target guard could be extended to accept raw pointers, OR a new
  `call_raw(ptr, arg) -> i64` native).
- **Gap: a `call_raw` native (call a raw x64 function pointer from ember).**
  This is a small extension — register a native that takes an i64 (the ptr) +
  an i64 (the arg) + calls the ptr as `int64_t(*)(int64_t)`.

## Estimated effort

- Stage 1 (lexer port): ~1-2 days. The demo lex.ember is a start; extending to
  ember's full token set is mechanical.
- Stage 2 (parser port): ~3-5 days. ember's grammar is larger than µ's, but the
  recursive-descent pattern is the same.
- Stage 3 (sema port): ~5-7 days. The type checker is the most complex piece.
- Stage 4 (codegen port): ~7-10 days. The x64 emitter is the hardest — raw byte
  emission + JIT memory management.
- Stage 5 (self-hosting milestone): ~1 day (once all stages work, the bootstrap
  is straightforward).

Total: ~3-4 weeks of focused effort. Each stage is independently valuable
(an ember-written lexer/parser/sema/codegen is useful even without full
self-hosting — it's a reference implementation in the language itself).
