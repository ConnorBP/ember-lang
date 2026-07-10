# ember - compiler pipeline spec

Detail doc for DESIGN.md Section 3. Lexer token set, grammar, AST, sema

> **Implementation status: v0.1** - this is the v1.0 design spec. The
> current repo implements the JIT codegen proof (encoder, label/patch,
> exec-mem, `.em` format). See `README.md` for what's shipped; see
> `CODEGEN_SPEC.md` Section 12 + Section 15 for the acceptance suite. This doc's
> content is the target design, not a claim of current implementation.
passes, IR shape, lowering rules, error reporting.

## 1. Lexer token set

```
Literals:   INT_LIT (e.g. 42, 42u, 42i64 - optional type suffix,
            see Section 6 default-type rule), FLOAT_LIT (e.g. 1.0, 1.0f),
            STRING_LIT ("..."), BOOL_LIT (true/false)
Identifiers: IDENT
Keywords:   fn struct global enum if else while do for switch case
            default break continue return as auto defer const constexpr
            link true false bool i8 i16 i32 i64 u8 u16 u32 u64
            f32 f64 void sizeof offsetof
            (v1.0: `enum` + `link`/`switch`/`case`/`default`/`do`/`defer`/
            `const`/`constexpr`/`sizeof`/`offsetof` were all added as the frontend
            grew past the v0.1 spec this list was written against; see the
            v0.2–v1.0 milestones in `DESIGN.md`. The `fn` keyword is
            overloaded: a *declaration* (`fn name(...) -> ret`) starts at
            `parse_top`, a *type* (`fn`, in `parse_type`) is the v1.0
            function-handle type — i64 with `is_fn_handle=true`, a bare
            handle that accepts any fn — see Section 2's `&fn`/`handle(args)`.)
Punctuation: ( ) { } [ ] , ; : -> . .. ::
            (`::` is the v0.5 cross-module selector `mod::fn` AND the v1.0
            enum-variant access `E::A`; the one-token lookahead split is in
            Section 2.)
Operators:  + - * / % = == != < <= > >= && || ! & | ^ ~ << >> += -=
            *= /= %=  (v1.0: prefix `&` in `parse_unary` is
            function-handle-take `&fn_name`, distinct from the bitwise-AND
            `&` binary operator in `parse_band`; see Section 2.)
Annotation: @ (starts an annotation, followed by IDENT and optional
            parenthesized literal-list, TYPE_SYSTEM.md Section 10)
```
- No `class`, `goto`, `do`-while-top-level, `typedef`/`using`, `template`,
  `namespace`, `#include`/preprocessor - none exist in v1 grammar (matches
  DESIGN.md non-goals). (`switch`/`case`/`default`/`do` shipped v0.2–v0.5;
  `enum` shipped v1.0 — the original v0.1 spec's "no enum/switch" note is
  superseded, see DESIGN.md non-goals.)
- Comments: `//` line and `/* */` block, stripped by the lexer,
  never reach the parser (no doc-comment/attribute-comment special
  handling in v1).
- String literals: no escape-sequence processing beyond `\n \t \\ \"`
  (the minimal set needed for readable script source) - no unicode
  escapes, no raw strings; a script string literal's bytes are used
  exactly as the host would want for e.g. an event name or log
  message, which is the only realistic v1 use case (TYPE_SYSTEM.md Section 6
  - no runtime string manipulation library in v1, strings are opaque
  byte slices handed to native functions).
- Lexer error (invalid character, unterminated string/comment):
  produces one `Diagnostic` (Section 7) and the lexer synchronizes by
  skipping to the next whitespace/statement-terminator-looking
  character - best-effort continuation so a single typo doesn't hide
  every other error in the file, but no guarantee of avoiding
  cascade errors (not worth building a sophisticated error-recovery
  lexer for a v1 scripting language - YAGNI, most real usage will fix
  the first reported error and recompile anyway given sub-millisecond
  compile times, DESIGN.md Section 1).

## 2. Grammar (informal BNF)

```
program      := (annotation* func_decl | struct_decl | global_decl)*

annotation   := '@' IDENT ('(' (literal (',' literal)*)? ')')?

struct_decl  := 'struct' IDENT '{' field_decl* '}'
field_decl   := IDENT ':' type ';'

global_decl  := 'global' IDENT ':' type '=' literal ';'

func_decl    := 'fn' IDENT '(' param_list? ')' ('->' type)? block
param_list   := param (',' param)*
param        := IDENT ':' type

type         := prim_type | IDENT              // IDENT = struct name
               | type '[' INT_LIT ']'          // fixed array
               | type '[' ']'                  // slice
prim_type    := 'bool'|'i8'|'i16'|'i32'|'i64'
               |'u8'|'u16'|'u32'|'u64'|'f32'|'f64'|'void'

block        := '{' stmt* '}'
stmt         := 'let' IDENT (':' type)? '=' expr ';'      // local decl
               | 'const' IDENT ':' type '=' expr ';'       // immutable local
               | 'defer' expr ';'                          // scope-exit cleanup (Section 6, CODEGEN_SPEC.md Section 14)
               | 'auto' IDENT '=' expr ';'                 // TYPE_SYSTEM.md Section 9
               | 'if' '(' expr ')' block ('else' (block|stmt))?
               | 'while' '(' expr ')' block
               | 'do' block 'while' '(' expr ')' ';'
               | 'for' '(' for_init? ';' expr? ';' for_step? ')' block
               | 'switch' '(' expr ')' '{' case_clause* '}'    // CODEGEN_SPEC.md Section 13
               | 'break' ';' | 'continue' ';'
               | 'return' expr? ';'
               | expr ';'                                   // expr-statement (calls, assignments)
               | block                                      // nested scope
case_clause  := 'case' literal ':' stmt* ('break' ';')?       // break required, no fallthrough (Section 2)
               | 'default' ':' stmt* ('break' ';')?

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
unary_expr   := ('++'|'--'|'-'|'!'|'~')? postfix_expr      // prefix ++/--; postfix in postfix_expr
postfix_expr := primary (call_suffix | index_suffix | field_suffix | view_suffix | '++' | '--')*
call_suffix  := '(' arg_list? ')'
index_suffix := '[' expr ']'
field_suffix := '.' IDENT
view_suffix  := '[' '.' '.' ']'                              // arr[..] whole-array view, MEMORY_AND_GC.md Section 3
primary      := INT_LIT | FLOAT_LIT | STRING_LIT | BOOL_LIT
               | IDENT | '(' expr ')' | struct_literal
struct_literal := IDENT '{' (IDENT ':' expr (',' IDENT ':' expr)*)? '}'
```

Standard C precedence climb, matching TYPE_SYSTEM.md Section 7's operator
rules exactly (the grammar enforces *syntactic* structure only; the
same-type-required rule and no-int-as-bool rule are sema checks, not
grammar restrictions - grammar accepts `1 + 1.0` syntactically, sema
rejects it, which is the right layering since "same type required" is
a semantic property, not a parseable one).

- **`let` vs `auto`**: `let x: i32 = expr;` (explicit type, checked
  against `expr`'s type - implicit-conversion rules from
  TYPE_SYSTEM.md Section 6 apply, e.g. `let x: i64 = some_i32_expr;` is fine
  via implicit widening, `let x: i32 = some_i64_expr;` is a compile
  error requiring `as`). `auto x = expr;` infers per TYPE_SYSTEM.md
  Section 9. `let x = expr;` (no type, no `auto` keyword) is **not valid
  syntax** - deliberately requiring the explicit `auto` keyword rather
  than making type omission implicitly mean inference, so "did the
  author mean to infer or forget a type" is never ambiguous to a
  reader (small ergonomic/readability choice, zero implementation
  cost either way, chosen for clarity).
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
struct Program { vector<StructDecl> structs; vector<GlobalDecl> globals; vector<FuncDecl> funcs; };
struct FuncDecl { string name; vector<Param> params; TypeRef ret; vector<Annotation> annotations; Block body; SourceLoc loc; };
struct StructDecl { string name; vector<FieldDecl> fields; SourceLoc loc; };
struct GlobalDecl { string name; TypeRef type; Expr init; SourceLoc loc; };

// statements
struct LetStmt { string name; optional<TypeRef> type; Expr init; bool is_auto; SourceLoc loc; };
struct IfStmt { Expr cond; Block then_block; optional<variant<Block, unique_ptr<Stmt>>> else_branch; };
struct WhileStmt { Expr cond; Block body; };
struct ForStmt { optional<LetStmt> init; optional<Expr> cond; optional<Expr> step; Block body; };
struct ReturnStmt { optional<Expr> value; };
struct BreakStmt {}; struct ContinueStmt {};
struct ExprStmt { Expr value; };
struct BlockStmt { Block block; };  // nested scope

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
struct AssignExpr  { unique_ptr<Expr> target; optional<BinOp> compound_op; unique_ptr<Expr> value; };
```

(`EnumDecl` + `EnumVariant` — `enum E { A, B, C }` / `variant = constexpr_int` —
live on `Program` (Tier 1, `src/ast.hpp`); `CallExpr::indirect_target`/
`is_indirect` and the standalone `FnHandleExpr` node are Tier 2.)

### 2a. New v1.0 surface syntax (Tier 1 enums + Tier 2 function refs)

The grammar grew two top-level / expression forms in the v1.0 concurrency +
Tier 2 batch, all verified in `src/parser.cpp`:

- **Enum declaration** (top-level, `parse_enum`): `enum IDENT { (variant (',' variant)* ','?)? }`
  where `variant := IDENT ('=' constexpr_int_expr)?`. Auto-increment from 0
  (or from the last explicit value); explicit-value expr is parsed as a full
  `parse_expr()` and restricted to a compile-time integer by sema's
  `try_eval_const_i64`. Trailing comma allowed.
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

Sema: `EnumAccessExpr` is rewritten **in place** to an `IntLit` (value i32,
sign-extended into the `IntLit`'s i64 `v` field) by `lower_enum_access_expr`
before any function body is checked, so codegen, the const-folder, the switch
case-value check, and the globals initializer evaluator all see an ordinary
integer literal. `FnHandleExpr` bakes the slot as an i64 literal and records
the source fn's signature on the type for arg checking. The indirect
`CallExpr` path checks args against the recorded signature when present
(bare `fn`-typed targets accept any args at the type level — the runtime
guard is the backstop; documented as an open item in `ROADMAP.md` Tier 2).
**i64 ↔ fn assignment is forbidden either direction** (closes forging at the
type level). Codegen validates the runtime i64 against a host-built bitset
allowlist before dispatch (REDSHELL guard #6, `SAFETY_AND_SANDBOX.md` §7a).

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
   (HOT_RELOAD.md Section 1), and insert into a module-wide symbol table  - 
   **before** any function body is examined, so forward calls
   (calling a function declared later in the file) resolve correctly
   (CODEGEN_SPEC.md Section 7's forward-reference handling depends on this
   ordering).
   - **Pass 1.5 — enum resolution (v1.0 Tier 1, `resolve_enums`):** resolve
     every `EnumDecl`'s variant values (auto-increment from 0 or from the
     last explicit `= constexpr_int_expr`, evaluated with the existing
     `try_eval_const_i64`; duplicate enum names / duplicate variant names
     within one enum / duplicate explicit values are errors, stricter than C).
     Builds the `(enum_name -> (variant -> i32 value))` table used by
     `lower_enum_access_expr` and the `enum_names` set used by pass 1.6.
   - **Pass 1.6 — type-position enum rejection (`check_declared_types_not_enum`):**
     an enum name is **not** a type in v1 — `let x: Color = ...`, a struct
     field, a fn param, a return type, a global declared with an enum name
     is a clean sema error (the hook typed enums later flip to accept).
   - **EnumAccessExpr lowering (`lower_enum_access_*`, runs as a sema-internal
     pass over every ExprPtr slot, NOT deferred to codegen):** rewrite each
     `E::A` to an `IntLit` carrying the variant's i32 value (sign-extended
     into the i64 `v` field), so by the time `check_expr` runs there are no
     `EnumAccessExpr` nodes left anywhere — codegen, the const-folder, the
     switch case-value literal check, and the globals initializer evaluator
     all see an ordinary integer literal. Unknown enum / unknown variant are
     errors (and the node is still rewritten to a placeholder `IntLit` so
     `check_expr` doesn't double-report via its catch-all).
3. **Per-function body check** (name resolution + type check,
   single pass, left-to-right, no unification/solver - matches
   "monomorphic types only so simple," DESIGN.md Section 3):
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
     statements specifically (the two escape routes) - implemented as
     a small backward walk from the returned/assigned expression
     through the chain of `Expr` nodes that produced it, stopping at
     (and flagging) a `ViewExpr` whose base is a local, or passing
     cleanly if the value came from a call or a global/param instead.
   - Index-expression unsigned-type check (TYPE_SYSTEM.md Section 7).
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
> regression: `DESIGN.md` Section 9 says no speculative optimization is
> added before the v0.5 benchmark harness exists to prove where it
> matters. The SSA-lite IR below is the **target design** the tree-walker
> lowers toward conceptually; the formal IR + regalloc refactor is the
> v0.5 milestone. The spec text is preserved unchanged.

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
    SourceLoc loc;   // preserved for runtime error messages / debug hook (DESIGN.md Section 8)
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
  index (HOT_RELOAD.md Section 1) attached - never a direct-address call IR
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
- **`defer` statement**: pushed onto a per-block "deferred-actions"
  stack during lowering (one stack per enclosing block, pushed when
  the `defer` stmt is lowered, popped at block exit). Every scope-exit
  path that lowers *into* the block (normal fallthrough off the end,
  explicit `return`, `break`/`continue` out of an enclosing loop, a
  trap-unwind) must emit the deferred actions in reverse order. **v1
  limitation, documented** (`GAP_ANALYSIS.md` Section 5): the trap-unwind
  path (non-local jump, `SAFETY_AND_SANDBOX.md` Section 2) does **not** run
  `defer` bodies - traps abort the whole `ember_call`, jumping past
  them; only the structured-exit paths (`return`/`break`/`continue`/
  fallthrough) run defers. This matches the "traps abort, don't
  gracefully unwind locals" stance and avoids needing defer-cleanup
  awareness in the non-local-unwind machinery. If a script needs
  cleanup on trap too, it must do it via a host-side
  `runtime_error`/`runtime_exception` handler, not `defer`.
- **`switch` statement**: scrutinee is a compile-time-known integer
  type (signed or unsigned, any width) - non-integer scrutinee is a
  compile error. Case labels must be `constexpr` integer literals
  (TYPE_SYSTEM.md Section 11), unique within the switch, and within the
  scrutinee type's range. Lowering: if the case-label set is dense
  (max-min < ~2×count, heuristic) emit a jump table (rodata array of
  rel32 targets, indexed by `scrutinee - min`); else emit a
  `cmp`+`je`-per-case cascade. Each case body's `break` lowers to
  `Jmp(switch_exit_bb)`; falling off a case body without `break`/
  `return`/`continue`/trap is a **compile error** (no fallthrough,
  `GAP_ANALYSIS.md` Section 5) - checked at sema, so lowering never emits a
  fallthrough path. `default` missing + no case matched = fall
  through to `switch_exit_bb` (no-op, equivalent to an empty
  switch). See `CODEGEN_SPEC.md` Section 13 for the jump-table encoding.
- **`f"...{expr}..."` interpolation**: sema-lowered to a sequence of
  string-literal `IrValue`s (the static parts, rodata per
  `MEMORY_AND_GC.md` Section 6) plus the hole expressions, all passed to a
  host-provided `__fmt` native addon (`GAP_ANALYSIS.md` Section 3) returning
  an `array<u8>` handle. Concretely `f"v={x}"` desugars to
  `__fmt(__sl("v="), x)` where `__sl` constructs a `slice<u8>` from a
  literal and `__fmt` is a `PERM_FFI`-free standard-addon native.
  If the `__fmt` addon isn't registered, sema errors with "string
  interpolation requires the `__fmt` addon" - the feature is syntactic
  sugar over a host native, not a language builtin, so it's absent
  when the host doesn't ship the format addon.
- **`const`/`constexpr` locals**: `const x: T = expr;` lowers like
  an ordinary local whose `StoreFrameSlot` happens once and whose
  subsequent assignment is rejected at sema (compile error, not a
  runtime check - `const`-ness is purely a sema-level property, zero
  runtime cost, the storage is identical to a `let` local).
  `constexpr` (when used as an array size, `offsetof`/`sizeof` arg,
  or `switch` case label) is **folded at sema** to a literal - v1
  `constexpr` expressions are restricted to literals, `sizeof`/
  `offsetof`, and integer arithmetic on other `constexpr` values; no
  function calls, no full const-eval interpreter (`ROADMAP.md` Tier 1
  for the broadened version).

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
  `error_info`/`last_error`/`last_error_message` (DESIGN.md Section 8,
  native-JIT-language-shaped surface per RESEARCH_NOTES.md) - `ember_compile` returns
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
  (module/context-level), RESEARCH_NOTES.md.
- **Parser error recovery**: on a syntax error, the parser
  synchronizes to the next statement boundary (`;` or matching `}`) at
  the current nesting depth and continues parsing, so a typo in one
  function doesn't prevent reporting errors in every other function in
  the same compile - same best-effort-continuation philosophy as the
  lexer (Section 1), not a guarantee of zero cascade errors.

## 8. Regalloc / codegen interface (module boundary between passes)

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
This three-stage split (lower -> regalloc -> emit) is what makes each
of CODEGEN_SPEC.md's acceptance-criteria cases (Section 12) independently
testable: a spill-heavy test case only needs to check `RegAllocResult`
correctness, an encoding-correctness test only needs to check
`emit_x64` against known byte sequences for a hand-built
`IrFunction`+`RegAllocResult` pair without exercising the parser at
all.
