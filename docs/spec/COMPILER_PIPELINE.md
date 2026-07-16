# ember - compiler pipeline spec

**Status: IMPLEMENTED and re-audited 2026-07-15.** The production compiler has
two backends: the complete typed-AST tree walker and the `ThinFunction` backend
(`thin_lower` → optional pass pipeline → optional linear-scan `regalloc` →
`thin_emit`). The tree walker remains the default/fallback when no IR pipeline
is requested or a function is not representable in Thin IR. Thin IR
serialization (v5), keyed v6 modules, 18 optimization passes, and seven
obfuscation passes are implemented; the richer illustrative
`IrFunction`/full-SSA design in §§5/8 is still future.

The lexer/parser/AST/sema additionally implement `let`/`mut`, source-import
preprocessing, namespaces, parameterized function types, lambdas and captures,
`try`/`catch`/`throw`, `yield` coroutines, and managed `new`/`delete`. Match
supports struct-destructure patterns and guards. Slice escape Stage 2 is
complete. Historical Tier labels below describe when features landed, not open
implementation state.

Detail doc for ../planning/DESIGN.md Section 3. Lexer tokens, grammar, AST/sema,
and both shipped lowering paths.

## 1. Lexer token set

```
Literals:   INT_LIT (e.g. 42 — no integer width suffix; the lexer REJECTS
            `42u`/`42i64`-style suffixes with "integer literal width suffixes
            are unsupported; use an explicit `as` cast"), FLOAT_LIT (e.g. 1.0,
            1.0f — the `f` suffix forces f32), STRING_LIT ("..."),
            RAW_STRING_LIT (r"""...""" — triple-quoted raw string, no escape
            processing, may span lines), BOOL_LIT (true/false)
Identifiers: IDENT
Keywords:   fn struct global enum if else while do for switch case
            default break continue return as auto defer const
            link true false bool i8 i16 i32 i64 u8 u16 u32 u64
            f32 f64 void sizeof offsetof in match namespace
            try catch throw yield new delete let mut
            constexpr static_assert priv
            (v1.0: `enum` + `link`/`switch`/`case`/`default`/`do`/`defer`/
            `const`/`sizeof`/`offsetof`/`in`/`match` were all added as the frontend
            grew past the v0.1 spec this list was written against; see the
            v0.2–v1.0 milestones in `../planning/DESIGN.md`. The `fn` keyword is
            overloaded: a *declaration* (`fn name(...) -> ret`) starts at
            `parse_top`, a *type* (`fn`, in `parse_type`) is the v1.0
            function-handle type — i64 with `is_fn_handle=true`, a bare
            handle that accepts any fn — see Section 2's `&fn`/`handle(args)`.
            `constexpr` is a **function-declaration modifier** (shipped
            2026-07-11): `constexpr fn name(...) -> i64 { ... }` declares a fn
            that **can** be const-evaluated at sema time when called with
            all-constant args (see `TYPE_SYSTEM.md` §11.5 + Section 2a/4/6
            below). `priv` is a function-declaration visibility modifier
            (F1, `priv fn` is module-private — not exported cross-module).
            `constexpr` and `priv` may appear in either order before `fn`.
            `static_assert` is a compile-time assertion statement/decl
            (shipped 2026-07-11): `static_assert(cond, "msg");` — see
            Section 2a/4/6. `const` (immutable local/global) ships.)
Punctuation: ( ) { } [ ] , ; : -> . .. :: =>
            (`::` is the v0.5 cross-module selector `mod::fn` AND the v1.0
            enum-variant access `E::A`; the one-token lookahead split is in
            Section 2. `=>` is the Tier 1 match-arm separator.)
Operators:  + - * / % = == != < <= > >= && || ! & | ^ ~ << >> += -=
            *= /= %=  (v1.0: prefix `&` in `parse_unary` is
            function-handle-take `&fn_name`, distinct from the bitwise-AND
            `&` binary operator in `parse_band`; see Section 2.)
Annotation: @ (starts an annotation, followed by IDENT and optional
            parenthesized literal-list, TYPE_SYSTEM.md Section 10)
```
- No `class`, `goto`, `do`-while-top-level, `typedef`/`using`, `template`,
  `#include`/preprocessor - none exist in the grammar. `namespace` now ships
  as a flat qualified-name grouping feature. (This supersedes the old v1
  no-namespace claim and otherwise matches
  ../planning/DESIGN.md non-goals). (`switch`/`case`/`default`/`do` shipped v0.2–v0.5;
  `enum` shipped v1.0 — the original v0.1 spec's "no enum/switch" note is
  superseded, see ../planning/DESIGN.md non-goals.)
- Comments: `//` line and `/* */` block, stripped by the lexer,
  never reach the parser (no doc-comment/attribute-comment special
  handling in v1).
- String literals: `"..."` supports the minimal escape set `\n \t \\ \"`
  (no unicode escapes). Raw strings `r"""..."""` (triple-quoted, no escape
  processing, may span lines) are also supported — the lexer produces a
  `RawStringLit` token whose `.text` is the raw bytes between the delimiters; a
  raw string is the way to embed literal newlines/quotes/backslashes without
  escaping. These are the only two string forms in v1; a script string literal's
  bytes are used exactly as the host would want for e.g. an event name or log
  message (TYPE_SYSTEM.md Section 6).
- Lexer error (invalid character, unterminated string/comment):
  produces one `Diagnostic` (Section 7) and the lexer synchronizes by
  skipping to the next whitespace/statement-terminator-looking
  character - best-effort continuation so a single typo doesn't hide
  every other error in the file, but no guarantee of avoiding
  cascade errors (not worth building a sophisticated error-recovery
  lexer for a v1 scripting language - YAGNI, most real usage will fix
  the first reported error and recompile anyway given sub-millisecond
  compile times, ../planning/DESIGN.md Section 1).

## 2. Grammar (informal BNF)

```
program      := (annotation* func_decl | struct_decl | enum_decl
               | global_decl | link_decl | namespace_decl
               | static_assert_decl)*
// `import "path";` is resolved by src/import.cpp as a source pre-pass before
// tokenization; it is intentionally not a lexer/parser token or AST declaration.

annotation   := '@' IDENT ('(' (literal (',' literal)*)? ')')?

struct_decl  := 'struct' IDENT '{' field_decl* '}'
field_decl   := IDENT ':' type ';'

enum_decl    := 'enum' IDENT (':' prim_type)? '{' (variant (',' variant)* ','?)? '}'   // Tier 1 (Section 2a). The optional ': T' is the TYPED-enum form (TYPE_SYSTEM §15.2).
variant      := IDENT ('=' constexpr_int_expr | '=' constexpr_fn_call)?  // auto-increment from 0 or last explicit value; the explicit value may be a constexpr fn call (enum-from-constexpr, TYPE_SYSTEM §15.2)

global_decl  := ('global'|'const') IDENT ':' type '=' expr ';'        // const = immutable global (Section 11.5)

link_decl    := 'link' STRING_LIT ('as' IDENT)? ';'                   // v0.5 cross-module: link "foo.em" as foo; / link "foo" as foo;
namespace_decl := 'namespace' IDENT '{' (func_decl | struct_decl | enum_decl
                  | global_decl | static_assert_decl | ';')* '}'
                  // parser flattens members into Program with qualified names/ns metadata

static_assert_decl := 'static_assert' '(' expr ',' STRING_LIT ')' ';'  // Tier 1 top-level compile-time assertion (Section 2a + TYPE_SYSTEM §11.5)

func_decl    := ('priv'|'constexpr')* 'fn' IDENT '(' param_list? ')' ('->' type)? block   // 'priv' and 'constexpr' may appear in either order before 'fn' (both optional, both fn-only)
param_list   := param (',' param)*
param        := IDENT ':' type ('=' literal)?      // optional trailing default value (a bare int/float/bool/string literal); defaulted params must trail non-defaulted ones (trailing-defaults-only, enforced in the parser + sema). At a call site, sema splices the synthesized literal for any missing trailing arg, so arity is a range [required_count, param_count]. See Section 4 + `ast.hpp` `DefaultValue`/`Param::default_val`.

// F1 visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): an optional leading
// `priv` on a func_decl opts the function out of the module's EXPORTED surface
// (the .em name directory / the JIT export table). A bare `fn` is EXPORTED by
// default (backward compat); `priv fn` is module-private (still callable within
// its own module, not published cross-module). `pub` is the implicit default and
// has no keyword. This is a BUNDLING/linking concern (what the loader/linker
// honors), distinct from in-module name scoping (namespaces, ROADMAP Tier 6).

type         := prim_type | IDENT              // IDENT = struct/enum/host type
               | type '[' INT_LIT ']'          // fixed array
               | type '[' ']'                  // slice
               | 'fn' ('(' type_list? ')' '->' type)?
                  // bare erased handle or parameterized function-handle type
prim_type    := 'bool'|'i8'|'i16'|'i32'|'i64'
               |'u8'|'u16'|'u32'|'u64'|'f32'|'f64'|'void'

block        := '{' stmt* '}'
stmt         := 'let' 'mut'? IDENT (':' type)? ('=' expr)? ';' // immutable by default; mut opts into writes
               | 'const' IDENT (':' type)? '=' expr ';'          // immutable local
               | 'defer' expr ';'                          // lexical-block-exit LIFO cleanup (Section 6, CODEGEN_SPEC.md Section 13)
               | 'auto' IDENT '=' expr ';'                 // TYPE_SYSTEM.md Section 9 (deprecated)
               | 'static_assert' '(' expr ',' STRING_LIT ')' ';'  // Tier 1 in-body compile-time assertion (Section 2a + TYPE_SYSTEM §11.5)
               | 'if' '(' expr ')' block ('else' (block|stmt))?
               | 'while' '(' expr ')' block
               | 'do' block 'while' '(' expr ')' ';'
               | 'for' '(' for_init? ';' expr? ';' for_step? ')' block   // C-style for
               | 'for' '(' IDENT 'in' expr ')' block                    // for-each over a slice (Tier 1, Section 2a + CODEGEN_SPEC.md Section 17)
               | 'switch' '(' expr ')' '{' case_clause* '}'    // CODEGEN_SPEC.md Section 12
               | 'match' '(' expr ')' '{' match_arm* '}'       // Tier 1 pattern match (Section 2a + CODEGEN_SPEC.md Section 18)
               | 'break' ';' | 'continue' ';'
               | 'return' expr? ';'
               | 'try' block 'catch' '(' IDENT ')' block
               | 'throw' expr ';'
               | 'yield' expr? ';'
               | expr ';'                                   // expr-statement (calls, assignments)
               | block                                      // nested scope
case_clause  := 'case' literal ':' stmt* ('break' ';')?       // break required, no fallthrough (Section 2)
               | 'default' ':' stmt* ('break' ';')?
match_arm    := (pattern | '_') '=>' (block | stmt) (',')?     // no fallthrough, no break; trailing comma optional (Tier 1)
pattern      := INT_LIT | BOOL_LIT                            // literal constant; struct destructure + guards are a later refinement

for_init     := 'let' IDENT (':' type)? '=' expr
for_step     := expr

expr         := assign_expr
assign_expr  := ternary_expr (('='|'+='|'-='|'*='|'/='|'%=') assign_expr)?
ternary_expr := logic_or_expr ('?' expr ':' ternary_expr)?
logic_or_expr:= logic_and_expr ('||' logic_and_expr)*
logic_and_expr:=bit_or_expr ('&&' bit_or_expr)*
bit_or_expr  := bit_xor_expr ('|' bit_xor_expr)*
bit_xor_expr := bit_and_expr ('^' bit_and_expr)*
bit_and_expr := eq_expr ('&' eq_expr)*
eq_expr      := rel_expr (('=='|'!=') rel_expr)*
rel_expr     := shift_expr (('<'|'<='|'>'|'>=') shift_expr)*
shift_expr   := add_expr (('<<'|'>>') add_expr)*
add_expr     := mul_expr (('+'|'-') mul_expr)*
mul_expr     := cast_expr (('*'|'/'|'%') cast_expr)*
cast_expr    := unary_expr ('as' type)*
               | 'sizeof' '(' type ')'
               | 'offsetof' '(' type ',' IDENT ')'
unary_expr   := ('++'|'--'|'-'|'!'|'~'|'&')? postfix_expr   // prefix ++/--; prefix '&' is Tier 2 fn-handle-take (Section 2a)
postfix_expr := primary (call_suffix | index_suffix | field_suffix | enum_access | view_suffix | '++' | '--')*
call_suffix  := '(' arg_list? ')'
index_suffix := '[' expr ']'
field_suffix := '.' IDENT
enum_access  := '::' IDENT                                   // Tier 1: E::A enum-variant access (value); also the v0.5 mod::fn cross-module selector
view_suffix  := '[' '.' '.' ']'                              // arr[..] whole-array view, MEMORY_AND_GC.md Section 3
primary      := INT_LIT | FLOAT_LIT | STRING_LIT | RAW_STRING_LIT | BOOL_LIT
               | IDENT | '(' expr ')' | struct_literal | array_literal
               | lambda_expr
unary_expr   := ... | 'new' type | 'delete' unary_expr
lambda_expr  := 'fn' ('[' capture_list? ']')? '(' param_list? ')' ('->' type)? block
struct_literal := IDENT '{' (IDENT ':' expr (',' IDENT ':' expr)*)? '}'
array_literal  := '[' (expr (',' expr)*)? ']'                  // Tier 1: fixed-array or slice literal at PRIMARY position (TYPE_SYSTEM.md Section 12.1)
```

`=>` (FatArrow) is the match-arm separator (Tier 1). `::` (ColonColon) is both
the Tier 1 enum-variant selector `E::A` and the v0.5 cross-module call selector
`mod::fn` — the one-token lookahead split (Section 2a) keeps `mod::fn(args)` on
the cross-module call path and `E::A` (second IDENT not followed by `(`) on the
enum-access path.

Standard C precedence climb, matching TYPE_SYSTEM.md Section 7's operator
rules exactly (the grammar enforces *syntactic* structure only; the
same-type-required rule and no-int-as-bool rule are sema checks, not
grammar restrictions - grammar accepts `1 + 1.0` syntactically, sema
rejects it, which is the right layering since "same type required" is
a semantic property, not a parseable one).

- **`let` inference vs explicit**: `let x = expr;` (no type) **infers** the type
  from the initializer (Rust-style optional annotation). `let x: T = expr;`
  (explicit type) is checked against `expr`'s type - implicit-conversion
  rules from TYPE_SYSTEM.md Section 6 apply, e.g. `let x: i64 = some_i32_expr;`
  is fine via implicit widening, `let x: i32 = some_i64_expr;` is a compile
  error requiring `as`). `auto x = expr;` is a **deprecated** redundant
  spelling of `let x = expr;` (both share the same inference path; `auto`
  is kept working but emits a deprecation warning and is slated for
  removal). Use `let x = expr;` for inference, `let x: T = expr;` for
  explicit.
- **Dangling-else**: standard resolution (`else` binds to the nearest
  unmatched `if`) - no special grammar rule needed beyond the
  recursive-descent parser's natural greedy behavior.
- **`for` loop desugaring**: lowered to a `while`-equivalent IR shape
  at the lowering stage (Section 5), not a distinct AST node needing its own
  codegen path - `for(init; cond; step) body` becomes `{ init; while
  (cond) { body; step; } }` in IR terms (with `continue` inside `body`
  correctly jumping to the step, not the top of the loop - a lowering
  detail: the loop's "continue target" is the step block, established
  once per loop when lowering the `for`, distinct from a plain `while`
  where continue's target is the condition check).

## 3. AST node types (representative, not exhaustive C++ decl)

```cpp
struct Program { vector<StructDecl> structs; vector<GlobalDecl> globals; vector<FuncDecl> funcs; vector<EnumDecl> enums; vector<LinkDecl> links; vector<StaticAssertStmt> static_asserts; };   // Tier 1 enums + v0.5 link decls + Tier 1 top-level static_asserts live on Program
struct FuncDecl { string name; vector<Param> params; TypeRef ret; vector<Annotation> annotations; Block body; SourceLoc loc; bool is_exported=true; bool is_constexpr=false; };  // F1: is_exported=false for `priv fn`; Tier 1: is_constexpr=true for `constexpr fn`
struct StructDecl { string name; vector<FieldDecl> fields; SourceLoc loc; };
struct GlobalDecl { string name; TypeRef type; Expr init; SourceLoc loc; };
struct EnumDecl { string name; optional<Prim> backing; vector<EnumVariant> variants; SourceLoc loc; };  // Tier 1: `backing` set => typed enum `enum E : T` (TYPE_SYSTEM §15.2); unset => untyped (§15.1)
struct EnumVariant { string name; optional<ExprPtr> explicit_value; };  // Tier 1: explicit_value may be a constexpr fn call (enum-from-constexpr)
struct StaticAssertStmt { ExprPtr cond; string msg; SourceLoc loc; };  // Tier 1: `static_assert(cond, "msg")` — top-level (on Program) or in-body

// statements
struct LetStmt { string name; optional<TypeRef> type; Expr init; bool is_auto; SourceLoc loc; };
struct IfStmt { Expr cond; Block then_block; optional<variant<Block, unique_ptr<Stmt>>> else_branch; };
struct WhileStmt { Expr cond; Block body; };
struct ForStmt { optional<LetStmt> init; optional<Expr> cond; optional<Expr> step; Block body; };
struct ForEachStmt { string var; Expr iter; Block body; SourceLoc loc; const Type* array_elem_ty = nullptr; };   // Tier 1: `for (x in slice)` or `for (x in array_handle)` — array_elem_ty set by sema for the array-handle case (the iterable() hook, CODEGEN_SPEC.md Section 17)
struct ReturnStmt { optional<Expr> value; };
struct BreakStmt {}; struct ContinueStmt {};
struct ExprStmt { Expr value; };
struct BlockStmt { Block block; };  // nested scope
struct MatchArm { Expr pattern; bool is_wildcard = false; Block body; };   // Tier 1: one match arm — pattern is a literal constant or `_` (is_wildcard)
struct MatchStmt { Expr subject; vector<MatchArm> arms; SourceLoc loc; };  // Tier 1: `match (expr) { pat => body, _ => default }` — per-arm branches, no fallthrough (CODEGEN_SPEC.md Section 18)

// expressions - every Expr node carries a SourceLoc and, post-sema,
// a resolved static Type (filled in during the type-check pass, Section 4 -
// AST is mutated in place with resolved types rather than building a
// second typed-AST tree, since v1 has no need to keep an untyped AST
// around after sema succeeds)
struct BinaryExpr { BinOp op; unique_ptr<Expr> lhs, rhs; };
struct UnaryExpr  { UnOp op; unique_ptr<Expr> operand; };
struct CastExpr    { unique_ptr<Expr> operand; TypeRef target; };
struct CallExpr    { string name; vector<Expr> args; };            // free fn or method (method desugared at sema, BINDING_API.md Section 3)
                   // v1.0 Tier 2: also the indirect call `handle(args)` —
                   //   `indirect_target` (ExprPtr) + `is_indirect` (bool),
                   //   `name` empty; built by `parse_postfix` when the call
                   //   target is none of the named forms (a runtime i64
                   //   handle). `FnHandleExpr` (`&fn_name`) is a separate node.
struct EnumAccessExpr { string enum_name; string variant; };    // v1.0 Tier 1: `E::A`, exists only parse→sema; sema rewrites it in place to an IntLit.
struct FnHandleExpr   { ExprPtr operand; int slot = -1; };      // v1.0 Tier 2: `&fn_name` — sema bakes the slot as an i64 literal (no codegen case needs to know it's a handle).
struct IndexExpr   { unique_ptr<Expr> base; unique_ptr<Expr> index; };
struct FieldExpr   { unique_ptr<Expr> base; string field; };
struct ViewExpr    { unique_ptr<Expr> base; };                     // arr[..]
struct IdentExpr   { string name; };
struct LiteralExpr { LiteralValue value; };
struct StructLiteralExpr { string type_name; vector<pair<string,Expr>> fields; };
struct ArrayLit { vector<Expr> elements; };                            // Tier 1: `[a, b, c]` at PRIMARY position — fixed-array or slice literal (TYPE_SYSTEM.md Section 12.1)
struct AssignExpr  { unique_ptr<Expr> target; optional<BinOp> compound_op; unique_ptr<Expr> value; };
```

(`EnumDecl` + `EnumVariant` — `enum E { A, B, C }` / `variant = constexpr_int` —
live on `Program` (Tier 1, `src/ast.hpp`); `LinkDecl` — `link "foo.em" as foo;`
— lives on `Program` (v0.5, `src/ast.hpp`); `ArrayLit` — `[a, b, c]` — is a
first-class expression (Tier 1, `src/ast.hpp`, `TYPE_SYSTEM.md` §12.1);
`ForEachStmt`/`MatchStmt`/`MatchArm` — `for (x in slice)` / `match (e) { pat => body }`
— are Tier 1 statements (`src/ast.hpp`, `CODEGEN_SPEC.md` §17/§18);
`CallExpr::indirect_target`/`is_indirect` and the standalone `FnHandleExpr` node are Tier 2.)

### 2a. New v1.0 surface syntax (Tier 1 enums/match/for-each/constexpr/static_assert + Tier 2 function refs)

The grammar grew several top-level / expression / statement forms across the
v1.0 batch and the 2026-07-11 Tier 1 follow-ons, all verified in
`src/parser.cpp`:

- **Enum declaration** (top-level, `parse_enum`): `enum IDENT (':' prim_type)? '{' (variant (',' variant)* ','?)? '}'`
  where `variant := IDENT ('=' constexpr_int_expr | '=' constexpr_fn_call)?`.
  Auto-increment from 0 (or from the last explicit value); explicit-value expr
  is parsed as a full `parse_expr()` and restricted to a compile-time integer
  by sema's `try_eval_const_i64` (or folded from a `constexpr fn` call by the
  constexpr-call pre-pass — enum-from-constexpr, `TYPE_SYSTEM.md` §15.2).
  Trailing comma allowed. The optional `: prim_type` is the **typed-enum** form
  (`enum Color : i32`, shipped 2026-07-11, `TYPE_SYSTEM.md` §15.2) — it makes
  the enum name a real type backed by that integer. Without it the enum is
  **untyped** (variants are plain i32 literals, `TYPE_SYSTEM.md` §15.1).
- **Enum-variant access** (postfix `::`, `parse_postfix`): `IDENT :: IDENT`
  where the second IDENT is **not** followed by `(` → `EnumAccessExpr`
  (a value). The one-token lookahead split keeps `mod::fn(args)` (the v0.5
  cross-module call, second IDENT followed by `(`) on its existing path.
- **Function-handle-take** (prefix `&` in `parse_unary`): `& IDENT` →
  `FnHandleExpr`. The operand is parsed as unary so `&&fib`-style nesting is
  structurally parseable then rejected by sema (which only accepts an
  `Ident` naming a script function of this module).
- **Indirect call** (`parse_postfix` `(` case): `<expr>(args)` where `<expr>`
  is none of the named forms (a bare `Ident` fn name, a `FieldExpr`
  method, or a `mod::fn` cross-module call) → `CallExpr` with `indirect_target`
  set + `name` empty. A bare `Ident(args)` that resolves at sema to a
  *local fn-typed variable* (the `let h = &fn; h(args);` case) is promoted to
  the indirect path at sema.
- **`fn` type keyword** (`parse_type`): a bare `fn` is the function-handle
  type — `Prim::I64` with `is_fn_handle=true` (a bare handle that accepts any
  fn). A parameterized `fn(i64)->i64` form is v2+.
- **`for-each`** (`parse_for`, Tier 1, 2026-07-11): `for (IDENT 'in' expr)
  block` — detected by `at(Ident) && peek(1).kind == Kw_in` inside the `for`
  paren. Builds a `ForEachStmt { var, iter, body }`. The iterable is a slice
  `T[]` **or** an `array<T>` handle (the `iterable()` hook, array case —
  `TYPE_SYSTEM.md` §13.2); `var` gets the element type. Lowers to a
  while-loop-with-indexing over the slice's `{ptr, len}` or the array's
  `array_length` + typed `array_get_*` (`CODEGEN_SPEC.md` §17). The IR backend
  marks a function using for-each as `non_serializable` (falls back to the
  tree-walker). See `TYPE_SYSTEM.md` §13 + `ROADMAP.md` Tier 1.
- **`match`** (`parse_stmt` `Kw_match` case, Tier 1, 2026-07-11):
  `match (expr) '{' (pattern | '_') '=>' (block | stmt) (',')? '}'` — builds a
  `MatchStmt { subject, arms }` where each `MatchArm` is `{ pattern, is_wildcard,
  body }`. Patterns are `IntLit`/`BoolLit` literals (sema rejects non-literal
  patterns); `_` is the wildcard (at most one, sema rejects a second). Each arm
  is a separate branch with no fallthrough and no `break` (the body jumps to the
  exit). `=>` is the `FatArrow` token. Lowers to a per-arm compare-and-branch
  cascade (`CODEGEN_SPEC.md` §18). The subject must be an integer or bool
  (`TYPE_SYSTEM.md` §13). The IR backend marks a function using match as
  `non_serializable` (falls back to the tree-walker). See `ROADMAP.md` Tier 1.
- **Array literals** (`parse_primary` `[` case, Tier 1): `[` at **primary**
  position constructs an `ArrayLit` (distinct from the postfix `[` index/view
  operator); `[a, b, c]` is a fixed-array or slice literal depending on the
  declared target type (`TYPE_SYSTEM.md` §12.1).
- **`constexpr fn`** (function-declaration modifier, Tier 1, 2026-07-11):
  `constexpr fn name(...) -> i64 { ... }` — a `constexpr` modifier before `fn`
  (may combine with `priv` in either order: `priv constexpr fn` /
  `constexpr priv fn`). Declares a fn that **can** be const-evaluated at sema
  time when called with all-constant args (`TYPE_SYSTEM.md` §11.5). Parser:
  `parse_top` reads an optional `priv` then an optional `constexpr` (in either
  order) before requiring `fn`; `is_constexpr` is recorded on the `FuncDecl`.
  `constexpr` on a non-`fn` decl is a parse error. See Section 4 (the
  constexpr-call pre-pass) + Section 6 (no runtime code for a folded call).
- **`static_assert`** (top-level decl + in-body stmt, Tier 1, 2026-07-11):
  `static_assert(cond, "msg");` — a compile-time assertion. At top level it
  is a `static_assert_decl` on `Program` (`prog.static_asserts`); in a body it
  is a `StaticAssertStmt`. Parser: `parse_static_assert` (shared by both
  positions); the message must be a string literal. See Section 4
  (`check_static_assert`) + Section 6 (no runtime code — true is elided) +
  `TYPE_SYSTEM.md` §11.5.

Sema: `EnumAccessExpr` is rewritten **in place** to an `IntLit` (value i32,
sign-extended into the `IntLit`'s i64 `v` field) by `lower_enum_access_expr`
before any function body is checked, so codegen, the const-folder, the switch
case-value check, and the globals initializer evaluator all see an ordinary
integer literal. `FnHandleExpr` bakes the slot as an i64 literal and records
the source fn's signature on the type for arg checking. The indirect
`CallExpr` path checks args against the recorded signature when present
(bare `fn`-typed targets accept any args at the type level — the runtime
guard is the backstop; documented as an open item in `../ROADMAP.md` Tier 2).
**i64 ↔ fn assignment is forbidden either direction** (closes forging at the
type level). Codegen validates the runtime i64 against a host-built bitset
allowlist before dispatch (the call-target-provenance guard, `SAFETY_AND_SANDBOX.md` §7a).

- **Annotation** node: `struct Annotation { string name; vector<LiteralValue> args; };`  - 
  attached to `FuncDecl` only (TYPE_SYSTEM.md Section 10/Section 2 grammar - struct
  and global decls don't take annotations in v1, no host use case
  identified for annotating those yet).

## 4. Sema passes (order matters, each pass assumes the previous
succeeded)

1. **Struct layout pass**: resolve every `StructDecl` (script-declared
   and host-registered via `StructBuilder`/`TypeBuilder`) into a
   finalized `TypeInfo` (name, size, align, field offsets) per
   TYPE_SYSTEM.md Section 2. Detects self-referential/cyclic struct
   definitions here (Section 2's DFS check) before anything else runs, since
   every later pass needs a *finished* size/align for every type.
2. **Function signature registration pass**: for every `FuncDecl` (and
   every registered `NativeFn`), resolve param/return `TypeRef`s to
   finalized types, assign a dispatch-table slot index
   (../HOT_RELOAD.md Section 1), and insert into a module-wide symbol table  - 
   **before** any function body is examined, so forward calls
   (calling a function declared later in the file) resolve correctly
   (CODEGEN_SPEC.md Section 7's forward-reference handling depends on this
   ordering).
   - **Pass 1.4 — typed-enum registration (2026-07-11, `register_typed_enums`):**
     BEFORE `resolve_type` runs, scan every `EnumDecl` and register the typed
     ones (those with a `: prim_type` backing). Records `enum_name -> backing
     prim` in `typed_enum_backing` and interns the enum type in
     `typed_enum_types` (`TYPE_SYSTEM.md` §15.2). An untyped enum (no backing)
     is skipped here — it stays a plain i32-literal source, not a type.
   - **Pass 1.5 — enum resolution (v1.0 Tier 1, `resolve_enums`):** resolve
     every `EnumDecl`'s variant values (auto-increment from 0 or from the
     last explicit `= constexpr_int_expr`, evaluated with the existing
     `try_eval_const_i64`; an explicit value may also be a `constexpr fn`
     call, folded first by the constexpr-call pre-pass — enum-from-constexpr,
     `TYPE_SYSTEM.md` §15.2; duplicate enum names / duplicate variant names
     within one enum / duplicate explicit values are errors, stricter than C).
     Builds the `(enum_name -> (variant -> i32 value))` table used by
     `lower_enum_access_expr` and the `enum_names` set used by pass 1.6.
   - **Pass 1.6 — type-position untyped-enum rejection
     (`check_declared_types_not_enum`):** an **untyped** enum name is **not**
     a type — `let x: Color = ...` (for an untyped `Color`), a struct field,
     a fn param, a return type, a global declared with an untyped enum name
     is a clean sema error. A **typed** enum name (`enum E : T`, registered in
     Pass 1.4) IS a valid type in these positions (`TYPE_SYSTEM.md` §15.2).
   - **EnumAccessExpr lowering (`lower_enum_access_*`, runs as a sema-internal
     pass over every ExprPtr slot, NOT deferred to codegen):** rewrite each
     `E::A` to an `IntLit` carrying the variant's value. For an **untyped**
     enum the `IntLit` is i32-typed (sign-extended into the i64 `v` field);
     for a **typed** enum (`enum E : T`) the `IntLit` is stamped with the enum
     type from `typed_enum_types` (so `let c: Color = Color::Red` type-checks
     — `TYPE_SYSTEM.md` §15.2). By the time `check_expr` runs there are no
     `EnumAccessExpr` nodes left anywhere — codegen, the const-folder, the
     switch case-value literal check, and the globals initializer evaluator
     all see an ordinary integer literal. Unknown enum / unknown variant are
     errors (and the node is still rewritten to a placeholder `IntLit` so
     `check_expr` doesn't double-report via its catch-all).
   - **constexpr-call pre-pass (2026-07-11, `lower_constexpr_calls_*`):** a
     bottom-up pass over every `ExprPtr` slot (fn bodies, globals, enum
     variant explicit values, `static_assert` conditions) that folds a call
     to a `constexpr fn` with all-constant args into an `IntLit` carrying the
     evaluated result, BEFORE `check_expr` runs (`TYPE_SYSTEM.md` §11.5). A
     constexpr call with a non-constant arg, or a non-i64-integer constexpr
     fn, is left as a normal call (runtime fallback). This is what lets
     `static_assert(square(7) == 49, ...)` and `enum E : i64 { X = base() }`
     work — the constexpr call is folded to a literal before the
     const-check.
   - **`static_assert` check (2026-07-11, `check_static_assert`):** folds the
     condition (after the constexpr-call pre-pass) via `try_eval_const_bool`;
     a **false** result is a compile error carrying the message, a **true**
     result is elided (no runtime code), a non-constant condition is a
     compile error. Runs both for top-level `prog.static_asserts` (after
     signatures + globals are registered) and for in-body `StaticAssertStmt`
     nodes (during the per-function body check). Produces NO runtime code in
     either position (`TYPE_SYSTEM.md` §11.5).
3. **Per-function body check** (name resolution + type check,
   single pass, left-to-right, no unification/solver - matches
   "monomorphic types only so simple," ../planning/DESIGN.md Section 3):
   - Scope stack: one scope per block (`Block`), locals resolved by
     walking the scope stack innermost-out; shadowing a name from an
     outer scope is **allowed** (matches common C-family expectation,
     not an error) but shadowing *within the same block* (redeclaring
     the same name twice in one `Block`'s direct statement list) is a
     compile error.
   - Every `Expr` node gets its resolved `Type` written back onto it
     (Section 3's "mutate AST in place" note) as the pass descends, so
     lowering (Section 5) never needs to re-derive types.
   - Operator resolution: for primitive operands, direct rule
     application (TYPE_SYSTEM.md Section 7); for struct operands, look up a
     registered operator overload by `(op, lhs_type, rhs_type)` in the
     `TypeBuilder`-populated operator table (BINDING_API.md Section 3)  - 
     missing/ambiguous overload is a compile error here.
   - **v1.0 Tier 2 function refs:** `FnHandleExpr` (`&fn_name`) is checked
     here — the operand must be an `Ident` naming a script function of this
     module (a native or unknown name is an error); the slot is baked onto
     the node as an i64 literal and the source fn's signature is recorded on
     the type. An indirect `CallExpr` (`indirect_target` set) type-checks the
     target as a fn handle and checks args against the recorded signature
     when present (a bare-`fn`-typed target accepts any args at the type
     level — the runtime allowlist guard is the backstop). A named call
     `name(args)` that resolves to a *local fn-typed variable* is promoted
     to the indirect path. **i64 ↔ fn assignment is forbidden either
     direction** (assignment + `let` init), closing V3-style forging at the
     type level; the runtime guard (`SAFETY_AND_SANDBOX.md` §7a) is the last line.
   - Method/property call desugaring (`obj.method(args)` ->
     synthesized native-call-with-implicit-self, BINDING_API.md Section 3)
     happens here, rewriting the `FieldExpr`+`CallExpr` combination
     into a plain `CallExpr` with the receiver prepended to `args`
     before lowering ever sees it - lowering has no special
     "method call" IR shape, only ordinary calls.
   - Return-path-coverage check (TYPE_SYSTEM.md Section 8) runs at the end of
     each function body's check.
   - Slice-escape provenance check (MEMORY_AND_GC.md Section 3) runs as part
     of type-checking `return` statements and global-assignment
     statements specifically - the **Stage-1 escape routes**: C1 (return),
     C2a (global `Ident`-store), and C2b (global-rooted `FieldExpr`/
     `IndexExpr`-store), for both the `ViewExpr`-over-fixed-array class and
     the `StringLit`-derived-`slice<u8>` class (commit `8062195`). It is
     implemented as a small backward walk from the returned/assigned
     expression through the chain of `Expr` nodes that produced it,
     stopping at (and flagging) a `ViewExpr` whose base is a local (or a
     `StringLit` whose resolved type is `slice<u8>`), or passing cleanly
     if the value came from a call or a global/param instead.
     **Stage 2 — DONE.** C3 is enforced through `NativeSig::retains`: a
     stack-backed slice cannot be passed to a retaining native, while copying
     natives remain legal. C5 is enforced for direct script calls through
     fixed-point per-function `borrowed_params`/`retained_params` summaries;
     retained arguments are rejected and borrowed results preserve provenance
     to the caller's eventual escape point. Opaque indirect/cross-module calls
     are handled conservatively. See `src/sema.cpp`'s
     `compute_borrow_retain` paths.
   - Index-expression integer-type check (TYPE_SYSTEM.md Section 7 — the
     implementation accepts any signed or unsigned integer index, not
     unsigned-only; the spec's "unsigned index" claim is relaxed to match).
     A constant index into a fixed array is bounds-checked at compile time.
   - Annotation argument grammar check (TYPE_SYSTEM.md Section 10 - literals
     only, no identifiers/expressions).
   - `PERM_FFI` gating check on every native call site
     (SAFETY_AND_SANDBOX.md Section 6).
4. **Whole-module validation**: at least one function should probably
   exist (not a hard error if zero - an empty module compiles fine,
   just useless) - this step is mostly a hook point for future
   module-level invariants, v1 has none beyond what's already covered
   per-function above.

## 5. IR ("SSA-lite") design

> **Implementation note (v0.2):** the shipped codegen (`src/codegen.cpp`)
> is a correctness-first **tree-walking stack-spilling emitter** that walks
> the AST directly and emits x86-64, not the SSA-lite IR + linear-scan
> regalloc specified in this section. This is a deliberate deferral, not a
> regression: `../planning/DESIGN.md` Section 9 says no speculative optimization is
> added before the v0.5 benchmark harness exists to prove where it
> matters. The SSA-lite IR below is the **target design** the tree-walker
> lowers toward conceptually; the formal IR + regalloc refactor is the
> v0.5 milestone. The spec text is preserved unchanged.
>
> **Implementation note (Stage A, 2026-07-10):** a thin three-address IR — a
> deliberate subset of this SSA-lite design (three-address `dst = op src1 src2`
> form, basic blocks, but NO SSA renaming, NO phi, and reassigned locals stay
> slot-backed via `LoadFrame`/`StoreFrame` exactly as the "would-be-phi" note
> below describes) — is now implemented behind `CodeGenCtx::enable_ir_backend`
> as the Stage-2 stepping stone. `src/thin_ir.{hpp,cpp}` ship the
> `ThinFunction`/`ThinBlock`/`ThinInstr` + the stable `ThinOp` enum;
> `src/thin_lower.{hpp,cpp}` ship the AST→`ThinFunction` lowering
> (`lower_function`); `src/thin_emit.{hpp,cpp}` ship the `ThinFunction`→x64 emit
> (`emit_x64`). Default off → the tree-walker above runs unchanged
> (byte-identical); on → value-equivalent (not byte-identical). It is the
> foundation for the formal SSA-lite + linear-scan refactor (§8 below): the
> `ThinOp` instruction set is a subset of the `IrInstr::Op` vocabulary, so the
> Stage-3 upgrade (SSA rename + slot-back + liveness + linear-scan) is
> additive, not a rewrite. See `CODEGEN_OPTIMIZATION_DESIGN.md` §4.3/§4.6 (the
> hybrid thin-IR option + the `ThinFunction` representation) and §8 (Stage A
> status), and `src/thin_ir.hpp` (the serialization-ready contract).

Already characterized at a high level in CODEGEN_SPEC.md Section 5. Concrete
shape:
```cpp
struct IrValue { uint32_t id; Type type; };  // a "virtual register", single-assignment

struct IrInstr {
    enum Op { Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr, Neg, Not,
              Cmp /* produces a condition, consumed by branch */,
              Load, Store, LoadGlobal, StoreGlobal,
              LoadFrameSlot, StoreFrameSlot,     // for the "would-be-phi" merge vars, MEMORY_AND_GC.md/CODEGEN_SPEC.md Section 5
              Lea, BoundsCheck, DivCheck, OverflowCheck,
              CallNative, CallScript /* via dispatch slot */,
              Cast, MakeSlice, FieldAddr, IndexAddr,
              Move, ConstInt, ConstFloat, ConstStringRef } op;
    vector<IrValue> operands;
    optional<IrValue> result;
    SourceLoc loc;   // preserved for runtime error messages / debug hook (../planning/DESIGN.md Section 8)
};

struct BasicBlock {
    uint32_t id;
    vector<IrInstr> instrs;
    // terminator is always the last instr: Jmp, Branch(cond, true_bb, false_bb), Return, Trap
};

struct IrFunction {
    vector<BasicBlock> blocks;   // blocks[0] is entry
    vector<Type> vreg_types;     // indexed by IrValue::id
    uint32_t frame_slot_count;   // pre-allocated by lowering for merge vars/struct temporaries
};
```
- **No phi nodes** (restated from CODEGEN_SPEC.md Section 5): a local
  variable that's reassigned inside more than one control-flow path
  reaching a common point (loop bodies, if/else arms both assigning
  the same outer-scope variable) is lowered to a dedicated
  `LoadFrameSlot`/`StoreFrameSlot` pair at each definition/use instead
  of a fresh SSA value + phi - decided once per source-level variable
  during lowering (Section 6) by a simple pre-pass: does this variable get
  assigned inside a block nested more than zero levels below its
  declaration, and is it used after the nested block? If yes, it's
  slot-backed for its whole lifetime, never register-candidate,
  simplifying regalloc's job (it never has to be told about the
  variable at all - it only sees the explicit load/store instructions
  operating on that frame slot exactly like any other memory access).
  If a variable is assigned exactly once and used only in a single
  straight-line path, it's a normal single-assignment `IrValue`
  (register-candidate).
- **Trap terminator**: a basic block ending in a bounds/div/budget/
  depth trap call is marked `Trap`-terminated rather than falling
  through to a normal successor in the CFG - liveness analysis
  (CODEGEN_SPEC.md Section 5 step 2) treats a `Trap` block as having no
  successors (the non-local unwind means control never returns to
  this function, SAFETY_AND_SANDBOX.md Section 2), which correctly shrinks
  live ranges that would otherwise appear to extend through
  never-actually-reached fallthrough code.

## 6. Lowering rules (AST -> IR), representative cases

- **Literal**: `ConstInt`/`ConstFloat` IR instr producing a fresh
  `IrValue` of the literal's resolved type (INT_LIT default type is
  `i64` unless suffixed or unless context/an explicit `let`-type
  forces a narrower type at sema time - sema already resolved and
  stamped the final type on the AST node per Section 4, so lowering just
  reads it off, no re-deriving).
- **BinaryExpr**: lower `lhs`/`rhs` first (recursively), then emit the
  matching `IrInstr::Op` (Add/Sub/etc.) consuming their result
  `IrValue`s. For struct-operand operator-overload calls (resolved to
  a `NativeFn` at sema), lowers to `CallNative` instead of an
  arithmetic op - the AST's `BinaryExpr` doesn't distinguish "primitive
  op" vs "overloaded operator call" itself; that distinction was
  already resolved and recorded during sema (Section 4) as an attached
  "resolved-to" reference sema stashes on the node, and lowering
  branches on that.
- **IndexExpr on a slice/array**: emits `BoundsCheck(index, len)`
  (len is either a loaded runtime field for slices, or a compile-time
  constant for fixed arrays with a constant index - sema already
  determined which case applies and can mark the `IndexExpr` node
  "check elided" when the index is a compile-time constant within
  bounds, per TYPE_SYSTEM.md Section 3/CODEGEN_SPEC.md Section 9, so lowering simply
  skips emitting `BoundsCheck` for that marked case) followed by
  `IndexAddr` (computes element address) and a `Load`/produces an
  address for `Store` if this is an assignment target.
- **IfStmt**: lower condition to a value, emit `Branch(cond, then_bb,
  else_bb_or_join_bb)`, lower `then_block` into `then_bb` ending with
  `Jmp(join_bb)`, likewise for `else_branch` if present (or branch
  false-target is the join block directly if no `else`).
- **WhileStmt**: `cond_bb` (evaluates condition, `Branch(cond, body_bb,
  exit_bb)`), `body_bb` (lowers body, ends with a budget-check-then-
  `Jmp(cond_bb)` back-edge - the budget decrement described in
  SAFETY_AND_SANDBOX.md Section 3 is inserted right here, at every loop
  back-edge, by lowering, not by codegen - codegen just sees an
  ordinary `Sub`+`Branch` pair like any other IR), `exit_bb` (join
  point, continues with whatever follows the loop). `break`/`continue`
  inside `body_bb` lower to `Jmp(exit_bb)`/`Jmp(cond_bb)` respectively
  (loop context - target blocks - tracked on a small stack during
  lowering, pushed on loop entry, popped on exit, standard recursive-
  descent-lowering bookkeeping).
- **ForStmt**: desugared per Section 2's note before lowering even starts
  (AST-level desugar into an equivalent `{init; while(cond){body;
  step;}}` shape, with `continue`'s target specifically pointed at a
  small `step_bb` inserted between body and the condition recheck  - 
  the one place `for`'s lowering differs from a hand-written
  `while`-with-manual-step, exactly to get `continue` semantics right).
- **CallExpr (native)**: lower each arg expression, emit `CallNative`
  with the resolved `NativeFn` pointer/descriptor attached (sema
  already resolved which native function by name+arity+types, Section 4)  - 
  slice args lower to two consecutive IR operands (ptr value, len
  value) matching BINDING_API.md Section 2's two-C++-param convention.
- **CallExpr (script)**: emit `CallScript` with the resolved slot
  index (../HOT_RELOAD.md Section 1) attached - never a direct-address call IR
  shape, keeping the "only one way to call a script function" rule
  (CODEGEN_SPEC.md Section 7) true starting from the IR level, not just as a
  codegen-time decision.
- **ReturnStmt**: lower the value expression (if any) into an
  `IrValue`, emit a `Return` terminator carrying it - codegen turns
  this into "move into rax/xmm0 + full epilogue" per CODEGEN_SPEC.md
  Section 6, one `Return` terminator per source-level `return` statement (no
  shared-epilogue-via-jmp in v1, matches that section's stated
  simplicity tradeoff).
- **StructLiteralExpr**: allocates a dedicated frame slot
  (CODEGEN_SPEC.md Section 5 step 7 - struct temporaries are never
  register-candidates), emits a `StoreFrameSlot` per field at its
  known offset within the slot.
- **ViewExpr (`arr[..]`)**: emits `MakeSlice(lea_of_arr, const_len_N)`
  - a slice-typed `IrValue` whose provenance tag (Section 4's escape check)
  marks it as "local-array-derived" for the sema pass that already
  ran before lowering to have flagged any illegal escape; lowering
  itself doesn't re-check this (sema already rejected bad cases, Section 4),
  it just needs the tag preserved through to this point conceptually
  - in practice this means the *sema* pass, not lowering, is where the
  rejection actually happens (stated once here for clarity: lowering
  never sees an escaping case because sema already turned it into a
  compile error before lowering runs).
- **`defer` statement (implemented M5)**: every lexical `Block` owns a
  cleanup scope. Reached defer sites execute once in reverse declaration/reach
  order when that block exits normally. Codegen also emits cleanup edges before
  `break`, `continue`, and `return`: break cleans only scopes crossed to its
  selected loop/switch target; continue skips switch frames and cleans through
  the nearest loop-body scope before its condition/step target; return cleans
  every active scope through the function body while preserving all return ABI
  forms. Direct site flags reset whenever a block is entered, so loop-body
  defers activate once per iteration, and cleanup clears a flag before running
  its expression to prevent a later fallthrough edge from running it twice.
  Cleanup runs while lexical locals remain live, so ordinary scope and
  declaration-before-use checks permit local references. Trap/longjmp remains
  non-local host traps still bypass defer cleanup. In-language
  `try`/`catch`/`throw` is implemented separately with the per-context catch
  stack; it unwinds to the nearest language catch, while safety traps remain
  uncatchable host-level aborts.
- **`switch` statement**: scrutinee is a compile-time-known integer
  type (signed or unsigned, any width) - non-integer scrutinee is a
  compile error. Case labels must be compile-time-constant integer
  literals (`try_eval_const_i64`-foldable; TYPE_SYSTEM.md Section 11),
  unique within the switch, and within the
  scrutinee type's range. Lowering: if the case-label set is dense
  (max-min < ~2×count, heuristic) emit a jump table (rodata array of
  rel32 targets, indexed by `scrutinee - min`); else emit a
  `cmp`+`je`-per-case cascade. Each case body's `break` lowers to
  `Jmp(switch_exit_bb)`; falling off a case body without `break`/
  `return`/`continue`/trap is a **compile error** (no fallthrough,
  `../planning/GAP_ANALYSIS.md` Section 5) - checked at sema, so lowering never emits a
  fallthrough path. `default` missing + no case matched = fall
  through to `switch_exit_bb` (no-op, equivalent to an empty
  switch). See `CODEGEN_SPEC.md` Section 12 for the jump-table encoding.
- **`f"...{expr}..."` interpolation**: the parser wraps each segment in
  the internal `__fstring_to_string` sentinel. Sema replaces each sentinel
  with a real string-extension conversion: literal `u8[]` segments use
  `string_from_slice`; integer, `f32`, `f64`, and bool holes use the matching
  `string_from_i64`/`string_from_f32`/`string_from_f64`/`string_from_bool`;
  existing string handles use `string_identity`. Concatenation is lowered via
  the registered string `+` overload. `__fstring_to_string` is not a native
  API and no `__fmt` native exists.
- **`const` locals**: `const x: T = expr;` lowers like
  an ordinary local whose `StoreFrameSlot` happens once and whose
  subsequent assignment is rejected at sema (compile error, not a
  runtime check - `const`-ness is purely a sema-level property, zero
  runtime cost, the storage is identical to a `let` local).
- **`constexpr fn` calls**: a `constexpr fn` call with all-constant args is
  folded to an `IntLit` by the constexpr-call pre-pass (Section 4) BEFORE
  lowering runs, so lowering sees an ordinary integer literal — **no call
  instruction is emitted** for the folded call (zero runtime cost). A
  `constexpr fn` called with a non-constant arg falls back to a normal
  `CallScript` (the fn is a real fn that is also emitted normally for the
  runtime-call case). The `constexpr` modifier is only valid on a function
  declaration (`constexpr fn`); on a non-`fn` decl it is a parse error.
- **`static_assert(cond, "msg");`**: produces **NO runtime code** in either
  position (top-level or in-body). Sema folds the condition (after the
  constexpr-call pre-pass); a true result is elided, a false result is a
  compile error (the assertion never reaches lowering), a non-constant
  condition is a compile error. Lowering never emits a `static_assert` —
  there is no IR shape for it.
- **`sizeof`/`offsetof`** DO fold at sema to literals; array sizes must
  be integer literals; `switch` case labels must be `try_eval_const_i64`-
  foldable integer expressions (or a `constexpr fn` call folded by the
  pre-pass) — none of these require the `constexpr` keyword on the
  expression itself (the keyword is the fn modifier).

## 7. Error reporting

```cpp
struct SourceLoc { uint32_t line; uint32_t column; const char* file; };

struct Diagnostic {
    enum Severity { Error, Warning } severity;
    string message;
    SourceLoc loc;
    string code;   // stable short code e.g. "E0203" for tooling/tests to match on, not just message-string-matching
};
```
- **Compile errors** (lexer/parser/sema) accumulate into a
  `vector<Diagnostic>` on the `engine_t`'s last-compile result, mirrors
  `error_info`/`last_error`/`last_error_message` (../planning/DESIGN.md Section 8,
  native-JIT-language-shaped surface per ../RESEARCH_NOTES.md) - `ember_compile` returns
  `nullptr` for the `module_t` if any `Error`-severity diagnostic was
  produced (warnings don't block compilation).
- **Runtime errors** (bounds/budget/depth/div-zero/overflow/
  `runtime_error`/`runtime_exception`) are a **completely separate**
  reporting path from compile-time `Diagnostic`s (SAFETY_AND_SANDBOX.md
  Section 7's `exception_pending`/`exception_value`/`exception_type` /
  `runtime_error` surface) - never unified into the same struct,
  since one is "your script text has a problem" (fixable by editing
  source, discovered at `ember_compile`) and the other is "your
  running script hit a guarded condition" (discovered at `ember_call`
  time, tied to a `context_t` not a `module_t`). Keeping them
  structurally distinct matches how a surveyed native-JIT language's
  API separates `error_info` (engine/compile-level) from the exception functions
  (module/context-level), ../RESEARCH_NOTES.md.
- **Parser error recovery**: on a syntax error, the parser
  synchronizes to the next statement boundary (`;` or matching `}`) at
  the current nesting depth and continues parsing, so a typo in one
  function doesn't prevent reporting errors in every other function in
  the same compile - same best-effort-continuation philosophy as the
  lexer (Section 1), not a guarantee of zero cascade errors.

## 8. Deferred full-SSA regalloc/codegen interface (a linear-scan subset shipped)

> **Status update (2026-07-11):** a **linear-scan register allocator over the
> thin IR has shipped** as Stage 3 (`src/regalloc.{hpp,cpp}`, `run_regalloc`,
> wired into `compile_func` at `src/codegen.cpp` behind `CodeGenCtx::
> enable_regalloc` — default off, only effective when `enable_ir_backend` is
> also true). It assigns scalar int/bool VRegs to Win64 callee-saved
> registers (rbx/rsi/rdi/r12/r13/r15) with linear-scan spilling to frame slots
> under pressure, and the emit consumes the regalloc map. Pinned by
> `examples/regalloc_test.cpp` (ctest `regalloc`). **What remains future** is
> the full SSA-lite design below — SSA renaming, phi nodes, and the
> `IrFunction`/`run_linear_scan` interface this section specifies (a different,
> richer IR than the shipped `ThinFunction`). The shipped regalloc is the
> linear-scan-over-thin-IR subset of this target; the `ThinOp` instruction set
> is a subset of the `IrInstr::Op` vocabulary, so the full-SSA upgrade is
> additive. See `codegen.hpp` (`enable_regalloc` + the Stage-3 note) and
> `../ROADMAP.md` "Codegen optimization" Stage 3 (TODO).

```cpp
// lowering produces this; regalloc consumes it and produces physical assignments;
// codegen consumes both to emit bytes. Kept as three separate, independently
// testable stages rather than one monolithic "IR to machine code" pass -
// each stage has a narrow, checkable contract.
struct RegAllocResult {
    unordered_map<uint32_t /*vreg id*/, PhysLoc> assignment; // PhysLoc = reg or frame-slot-offset
    uint32_t frame_size;
    vector<PhysReg> used_callee_saved;
};
RegAllocResult run_linear_scan(const IrFunction&);
vector<uint8_t> emit_x64(const IrFunction&, const RegAllocResult&); // CODEGEN_SPEC.md Section 3-Section 11
```
If implemented, this three-stage split (lower -> regalloc -> emit) would make each
of CODEGEN_SPEC.md's acceptance-criteria cases (Section 12) independently
testable: a spill-heavy test case only needs to check `RegAllocResult`
correctness, an encoding-correctness test only needs to check
`emit_x64` against known byte sequences for a hand-built
`IrFunction`+`RegAllocResult` pair without exercising the parser at
all.
