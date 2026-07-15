# ember — Full Self-Hosting Feature Parity Matrix

**Purpose.** This document enumerates **every** feature of the ember language and
records the **current** status of the self-hosted compiler
(`self_hosted/{lex,parse,sema,codegen}.ember` + `emberc.ember` + `full_pipeline.ember`)
for each. It is the authoritative roadmap for the self-hosting completion effort:
every row is either *supported*, *rejected-with-code* (a stable `-3xx`), *partial*,
or *untested*, and the closing roadmap assigns every still-open row to one of the
five implementation phases.

**How to read a row.**

| Column | Meaning |
|---|---|
| **Native (C++)** | What `src/{lexer,parser,sema,codegen}.cpp` does today. The reference. |
| **Self-hosted sema** | What `sema.ember`'s dispatch does with the node: *supported* (type-checked), *rejected-`-3NN`* (explicit `unsupported(...)` call), *partial* (checked for a restricted shape only), or *untested* (falls to a catch-all). |
| **Self-hosted codegen** | What `codegen.ember`'s dispatch does: *supported* (emitted), *rejected* (generic `cg_err=1`, **no** `-3xx` code), *partial* (emits a restricted/wrong form), or *untested* (catch-all). |
| **Notes** | The precise restriction, the reject code, or the gap. |

**Status legend:** ✅ supported · 🟡 partial · ❌ rejected-with-code · ⛔ untested/catch-all · ➖ not-applicable (no AST node).

**Scope reminder.** The lexer (`lex.ember`) and parser (`parse.ember`) are
**complete**: the `Tok` enum mirrors every C++ token, and the `NK` enum (parse.ember
lines 54–117) covers every node kind the parser builds. The gap is in **sema +
codegen**, which type-check / lower a small i64/bool/void subset and reject
~18 feature categories with `unsupported(-3NN)` codes (sema) or generic
`cg_err=1` messages (codegen). The reject-test corpus
(`self_hosted/correctness_tests/reject_*.ember`, 17 files) is **not** exhaustive —
this matrix is derived from the C++ sources directly.

**One lexer/parser gap worth stating up front.** `new` / `delete` are keywords in
the C++ lexer (`Tk::Kw_new`, `Tk::Kw_delete`) and first-class operators in the C++
AST (`NewExpr`, `DeleteExpr`). The self-hosted **lexer has no `Kw_new`/`Kw_delete`
tokens** and the self-hosted **`NK` enum has no `NewExpr`/`DeleteExpr` node kinds**,
so `new`/`delete` currently lex as `Ident` and never parse as operators. This is a
lexer+parser gap (Phase 4 prerequisite), not just a sema/codegen gap.

---

## 1. Types

The `Prim` enum has 12 primitives. The C++ `Type` struct adds: named struct,
slice `T[]`, fixed array `T[N]`, typed enum (`enum E : T`), fn handle (`fn` /
`fn(Args)->Ret`), cross-module fn handle, lambda-with-capture, and managed pointer
(`new T`). The self-hosted `Prim` enum mirrors all 12; the self-hosted `Type` node
encodes `prim`/`struct-name-tok`/`elem`/`array_len`/`is_slice` but does **not**
encode `enum_name`, `is_fn_handle`, `has_recorded_sig`, `recorded_params/ret`,
`is_cross_module_handle`, `is_lambda`, or `is_managed_ptr`.

| Type | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| `i8` | ✅ full (8-bit, sign/zero-ext on load/store) | ❌ `-315` ("non-i64 integer…") | ⛔ (sema rejects first) | `resolve_type` only accepts I64/Bool/Void. |
| `i16` | ✅ full | ❌ `-315` | ⛔ | as above |
| `i32` | ✅ full | ❌ `-315` | ⛔ | as above |
| `i64` | ✅ full | ✅ supported | ✅ supported | the one integer width in the subset |
| `u8` | ✅ full | ❌ `-315` | ⛔ | |
| `u16` | ✅ full | ❌ `-315` | ⛔ | |
| `u32` | ✅ full | ❌ `-315` | ⛔ | |
| `u64` | ✅ full | ❌ `-315` | ⛔ | |
| `f32` | ✅ full (SSE, `xmm0` return, `xmm0-3` args) | ❌ `-303` ("floating-point type") | ⛔ | |
| `f64` | ✅ full (SSE) | ❌ `-303` | ⛔ | |
| `bool` | ✅ full (i8-sized, `0/1`) | ✅ supported | ✅ supported | |
| `void` | ✅ full | ✅ supported | ✅ supported | |
| named struct (`Point`) | ✅ full (layout table, by-value copy, field access) | ❌ `-301` (decl + value + field) | ⛔ | `StructDecl` rejected at decl pass; `StructLit`/`FieldExpr` rejected in `check_expr`. |
| slice `T[]` | ✅ full (`{ptr,len}` 2-word ABI) | ❌ `-310` ("slice type"/"array/slice expression") | ⛔ | `resolve_type` rejects `is_slice`; `IndexExpr`/`ViewExpr` rejected. |
| fixed array `T[N]` | ✅ full (contiguous, `N` compile-time-checked) | ❌ `-310` ("array type") | ⛔ | `resolve_type` rejects `array_len>0`. |
| typed enum (`enum E : T`) | ✅ full (backing-int repr + `enum_name` tag) | ❌ `-302` (enum decl/value) | ⛔ | `EnumDecl` rejected; no `enum_name` encoding in self-hosted `Type`. |
| fn handle (`fn` / `fn(Args)->Ret`) | ✅ full (i64 slot, recorded-sig checks) | ⛔ catch-all `-313`/`-315` | ⛔ | `FnHandleExpr` falls to `-313`; a `fn` type hits `-315`. No `is_fn_handle` in self-hosted `Type`. |
| cross-module fn handle (`&mod::fn`) | ✅ full (`bit63` tag, per-module guard) | ⛔ `-313` | ⛔ | parsed (`FnHandleExpr` cross-module form), rejected at sema. |
| lambda-with-capture | ✅ full (`{slot,env_ptr}` 16-byte, by-ref capture, GC env) | ❌ `-307` ("lambda") | ⛔ | `LambdaExpr` rejected. |
| managed pointer (`new T`) | ✅ full (one-word GC ptr, `is_managed_ptr`) | ➖ (no AST node — lexer gap) | ➖ | `new`/`delete` not lexed as keywords; no `NewExpr`/`DeleteExpr` in `NK`. |

**Summary:** 3 of 12 primitives supported (`i64`/`bool`/`void`); all other widths
`-315`; floats `-303`; every composite type rejected (`-301`/`-302`/`-310`/`-307`) or
untyped-catch-all (`-313`); managed pointers unreachable (lexer gap).

---

## 2. Declarations

| Declaration | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| `fn` (free function) | ✅ full (params, ret, body, dispatch slot) | ✅ supported | ✅ supported | sema `register_fn` + `check_func_body`; codegen lays fns out in decl order with per-fn entry labels. |
| `fn` default params (`b: i64 = 10`) | ✅ full (literal defaults spliced at call site) | 🟡 partial | ⛔ | sema stores `Param` but does **not** model `DefaultValue`; no default-arg splicing at call sites. |
| `priv fn` (module-private) | ✅ full (opts out of export surface) | ⛔ not modeled | ⛔ | `FD_PRIV` flag parsed onto `FnDecl` but sema ignores `is_exported`. |
| `constexpr fn` | ✅ full (const-eval at sema, rewrite to `IntLit`) | ⛔ not modeled | ⛔ | `FD_CEXPR` flag parsed but no const-eval interpreter. |
| `struct` | ✅ full | ❌ `-301` | ⛔ | rejected at decl pass 1. |
| `enum` (untyped + typed) | ✅ full | ❌ `-302` | ⛔ | rejected at decl pass 1. |
| `global` / `const` (top-level) | ✅ full (mutable globals + relocations) | ❌ `-317` | ⛔ | rejected at decl pass 1 (`-317`); pass 2 never runs. |
| `link "foo.em" as foo;` | ✅ full (live module load/register) | ⛔ catch-all `-316` | ⛔ | `LinkDecl` parsed; hits the `-316` "top-level declaration" else-branch. |
| `static_assert(cond, "msg");` (top-level) | ✅ full (folded at sema; true elided, false = error) | ⛔ catch-all `-316` | ⛔ | `StaticAssertDecl` parsed; hits `-316`. |
| `namespace Name { … }` | ✅ full (prefix flattening, `Name::member` resolution) | ⛔ catch-all `-316` | ⛔ | `NamespaceDecl` parsed; hits `-316`. |
| `import "path";` | ✅ full (**pre-lex** textual include via `resolve_imports`) | ➖ host step | ➖ host step | Not an AST node / not a keyword (`import` lexes as `Ident`). The **C++ host** (`ember_cli`) inlines `import` lines before lexing. The self-hosted 4-file compiler receives a single already-inlined source string from `full_pipeline.ember`; it does **no** import resolution itself. |
| annotations (`@realtime`, `@obf(...)`, `@on_tick`) | ✅ full (sema reads them) | ❌ `-318` | ⛔ | `reject_annotations()` scans the token stream for `Tok::At` and returns `-318` before any decl pass. |

**Summary:** only `fn` (plain, no modifiers) is supported; globals `-317`, structs
`-301`, enums `-302`, annotations `-318`, and `link`/`namespace`/`static_assert`
fall to the `-316` catch-all; `import` is a host pre-lex step outside the 4 files.

---

## 3. Statements

| Statement | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| `let` (immutable) | ✅ full | ✅ supported | ✅ supported | |
| `let mut` / `auto mut` | ✅ full | ✅ supported | ✅ supported | `LF_MUT` flag; codegen allocates an 8-byte slot. |
| `auto` (infer from init) | ✅ full | ✅ supported | ✅ supported | |
| `const` (immutable local/global) | ✅ full | ✅ supported (local) | ✅ supported (local) | globals rejected (`-317`); local constness checked in `check_assignexpr`. |
| `if` / `else` / `else if` | ✅ full | ✅ supported | ✅ supported | bool cond checked; codegen emits `jcc`. |
| `while` | ✅ full | ✅ supported | ✅ supported | |
| `do-while` | ✅ full | ⛔ catch-all `-314` | ⛔ | `DoWhileStmt` is **not** in `check_stmt`'s handled list → falls to `-314` "statement kind". |
| `for` (C-style) | ✅ full | ✅ supported | ✅ supported | `check_for`; codegen `cg_emit_for`. |
| `for-each` (`for (x in iter)`) | ✅ full (slice + array-handle iterables) | ❌ `-311` | ⛔ | |
| `return` / `return expr;` | ✅ full | ✅ supported | ✅ supported | type checked against `s_cur_ret`. |
| `break` | ✅ full | ✅ supported | ✅ supported | `s_loop_depth` guard (rejects outside loop). |
| `continue` | ✅ full | ✅ supported | ✅ supported | as above. |
| `defer expr;` | ✅ full (lexical-block-exit LIFO) | ❌ `-312` | ⛔ | |
| expr stmt (`f();`, `x = 1;`) | ✅ full | ✅ supported | ✅ supported | |
| block `{ … }` | ✅ full | ✅ supported | ✅ supported | |
| `switch` / `case` / `default` | ✅ full (no fallthrough; jump-table or cmp cascade) | ❌ `-305` | ⛔ | |
| `match` / arms (`pat => body`, `_`, struct destructure, guards) | ✅ full | ❌ `-304` | ⛔ | |
| `yield expr;` (coroutine) | ✅ full (lowered to `__ember_coro_yield` native) | ❌ `-308` | ⛔ | |
| `throw expr;` | ✅ full (longjmp to nearest catch / host trap) | ❌ `-306` (shared with try/catch) | ⛔ | |
| `try { } catch (name) { }` | ✅ full (inline setjmp/longjmp over `catch_bufs`) | ❌ `-306` | ⛔ | |
| `static_assert(cond, "msg");` (in-body) | ✅ full (folded; true elided, false = error) | ⛔ catch-all `-314` | ⛔ | `StaticAssertStmt` not in `check_stmt`'s handled list → `-314`. |

**Summary:** 11 statement forms supported (let/mut/auto/const, if/else, while,
for, return, break, continue, expr stmt, block); 8 rejected with explicit codes
(`-304`/`-305`/`-306`/`-308`/`-311`/`-312`); `do-while` and in-body
`static_assert` fall to the `-314` catch-all (no dedicated code).

---

## 4. Expressions

### 4.1 Literals & identifiers

| Expression | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| int literal (`42`, `0x..`, negative) | ✅ full (contextual width adaptation) | ✅ supported (`-> i64`) | ✅ supported (`mov rax, imm`) | literals are always `i64` in the subset (no adaptation). |
| float literal (`3.14`, `2.0f`) | ✅ full (f32/f64) | ❌ `-303` | ⛔ | |
| bool literal (`true`/`false`) | ✅ full | ✅ supported | ✅ supported | |
| string literal (`"…"`) | ✅ full (rodata + optional XOR encrypt + implicit `string` conv) | ❌ `-309` | ⛔ | |
| f-string (`f"…"`) | ✅ full (desugared to `StringLit` + `__fstring_to_string` `+` chain) | ❌ `-309` (via `StringLit`) | ⛔ | **parser partial**: records the `FStringLit` token as a flagged `NK::StringLit` (`d=1`) but does **not** desugar the interpolation; sema then rejects it as a string (`-309`). |
| raw string (`` `…` `` / `r"…"`) | ✅ full (`StringLit`) | ❌ `-309` (via `StringLit`) | ⛔ | lexer produces `RawStringLit`; parser maps to `NK::StringLit`; sema rejects. |
| ident | ✅ full (local/global/param lookup) | ✅ supported | ✅ supported | locals+params only (globals `-317`); no namespace-qualified lookup. |

### 4.2 Binary operators (all 18)

The `BinOp` enum: `Add Sub Mul Div Mod And Or Xor Shl Shr Eq Neq Lt Le Gt Ge LAnd LOr`.

| Op | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| `+` Add | ✅ (int + float + overload) | ✅ i64 only | ✅ `add rax,rcx` | |
| `-` Sub | ✅ | ✅ i64 | ✅ `sub` | |
| `*` Mul | ✅ | ✅ i64 | ✅ `imul` | |
| `/` Div | ✅ (signed/unsigned, float) | ✅ i64 | ✅ `cqo; idiv rcx` | signed i64 only. |
| `%` Mod | ✅ | ✅ i64 | ✅ `cqo; idiv; mov rax,rdx` | signed i64 only. |
| `&` And | ✅ | ✅ i64 | ✅ `and` | |
| `\|` Or | ✅ | ✅ i64 | ✅ `or` | |
| `^` Xor | ✅ | ✅ i64 | ✅ `xor` | |
| `<<` Shl | ✅ | ✅ i64 | ✅ `shl rax, cl` | |
| `>>` Shr | ✅ (arithmetic for signed) | ✅ i64 | ✅ `sar rax, cl` | signed shift (i64 is signed in subset). |
| `==` Eq | ✅ (-> bool) | ✅ | ✅ `cmp; setcc; movzx` | |
| `!=` Neq | ✅ | ✅ | ✅ | |
| `<` Lt | ✅ (signed) | ✅ | ✅ `COND_L` | |
| `<=` Le | ✅ | ✅ | ✅ `COND_LE` | |
| `>` Gt | ✅ | ✅ | ✅ `COND_G` | |
| `>=` Ge | ✅ | ✅ | ✅ `COND_GE` | |
| `&&` LAnd | ✅ (short-circuit, -> bool) | ✅ | ✅ short-circuit w/ labels | |
| `\|\|` LOr | ✅ (short-circuit) | ✅ | ✅ short-circuit | |

**All 18 binary ops are supported** in the i64/bool subset. Float/overload variants
are not (floats `-303`). Unsigned comparisons (`COND_B/AE/BE/A`) exist in codegen
globals but are unused (no unsigned types).

### 4.3 Unary, cast, assignment, call, access

| Expression | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| unary `-` (Neg) | ✅ | ✅ i64 | ✅ `neg rax` | |
| unary `!` (Not, logical) | ✅ (bool) | ✅ | ✅ | |
| unary `~` (BitNot) | ✅ | ✅ i64 | ✅ `not rax` | |
| `expr as T` (cast) | ✅ full matrix (see cast kinds below) | ⛔ catch-all `-313` | ⛔ | `CastExpr` is **not** in `check_expr`'s handled list → `-313` "expression kind". |
| assign `=` | ✅ full (lvalue check, constness, type compat) | ✅ supported (Ident lvalue) | 🟡 partial | sema allows Ident/Field/Index lvalues; **codegen only handles Ident targets** — field/index assign → `cg_err` "target must be an identifier". |
| compound `+=` | ✅ full | ✅ supported (Ident; compound via `check_binop_result`) | 🟡 **partial/buggy** | codegen `cg_eval_assignexpr` **ignores the compound op** (`nk_d`) — it emits a plain store of the rhs, dropping the read-modify-write. `x += 5` miscompiles to `x = 5`. |
| compound `-=` `*=` `/=` `%=` | ✅ full | ✅ supported (Ident) | 🟡 partial/buggy | same bug as `+=` (op dropped). |
| compound `&=` `\|=` `^=` `<<=` `>>=` | ✅ full | ✅ supported (Ident) | 🟡 partial/buggy | same bug (op dropped). |
| postfix `x++` / `x--` | ✅ full (returns old value) | ✅ supported (integer target) | 🟡 partial/buggy | codegen ignores `postfix` flag + compound op; emits plain store of the implicit `1` → `x++` miscompiles to `x = 1` and returns the new value, not the old. |
| prefix `++x` / `--x` | ✅ full (returns new value) | ✅ supported (desugared to assign) | 🟡 partial/buggy | same compound-assign bug. |
| call `f(args)` | ✅ full (native/script/indirect/lambda/method) | ✅ supported (script, ≤4 args checked) | 🟡 partial | codegen: **≤4 reg args only** (`>4` → `cg_err` "at most 4 call args"); direct `call rel32` to script fn label; **no native calls, no indirect, no lambda, no method sugar, no stack args, no aggregates**. |
| field access `a.b` | ✅ full | ❌ `-301` ("struct value") | ⛔ | |
| index `a[i]` | ✅ full (array/slice, bounds-checked) | ❌ `-310` | ⛔ | |
| view `a[..]` | ✅ full (whole-array slice) | ❌ `-310` | ⛔ | |
| ternary `c ? t : e` | ✅ full (bool cond, same-type branches) | ⛔ catch-all `-313` | ⛔ | `TernaryExpr` not handled → `-313`. |
| `sizeof(T)` | ✅ full (compile-time constant) | ⛔ catch-all `-313` | ⛔ | `SizeofExpr` not handled → `-313`. |
| `offsetof(T, field)` | ✅ full (compile-time constant) | ⛔ catch-all `-313` | ⛔ | `OffsetofExpr` not handled → `-313`. |
| struct literal `Point{x: 1, y: 2}` | ✅ full | ❌ `-301` | ⛔ | |
| array literal `[1, 2, 3]` | ✅ full (fixed-array or slice) | ❌ `-310` | ⛔ | |
| enum access `E::A` | ✅ full (rewritten to `IntLit` at sema) | ❌ `-302` | ⛔ | `EnumAccess` rejected. |
| fn handle `&fn` | ✅ full (bakes slot as i64 literal) | ⛔ catch-all `-313` | ⛔ | `FnHandleExpr` not handled → `-313`. |
| cross-module handle `&mod::fn` | ✅ full (`bit63` encoding) | ⛔ `-313` | ⛔ | |
| lambda `fn(params)->ret { body }` | ✅ full (synthetic fn + env) | ❌ `-307` | ⛔ | |
| `new T` | ✅ full (`__ember_gc_alloc_object`) | ➖ (no AST node) | ➖ | lexer/parser gap (`new` lexes as `Ident`). |
| `delete expr` | ✅ full (`__ember_gc_delete_object`) | ➖ (no AST node) | ➖ | lexer/parser gap. |

### 4.4 Cast kinds (the explicit `as` matrix, from `sema.cpp`)

| Cast | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| same-type no-op | ✅ | ⛔ `-313` | ⛔ | whole `CastExpr` node rejected. |
| same-type aggregate (local init only) | ✅ | ⛔ `-313` | ⛔ | |
| int → int (widening/narrowing, same signedness) | ✅ | ⛔ `-313` | ⛔ | |
| typed enum → backing int | ✅ | ⛔ `-313` | ⛔ | |
| float → float (f32↔f64) | ✅ | ⛔ `-313` | ⛔ | |
| signed int ↔ float | ✅ | ⛔ `-313` | ⛔ | |
| unsigned int ↔ float | ❌ deliberately (v1) | — | — | not in native matrix. |
| int → typed enum | ❌ deliberately | — | — | not in native matrix. |
| slice/aggregate/bool/fn-handle reinterpret | ❌ (same-type only) | — | — | |

**Summary:** literals — int/bool/ident ✅, float `-303`, all string forms `-309`
(f-string parser-partial); all 18 binops ✅ (i64/bool); unary ✅; **cast/ternary/
sizeof/offsetof/fn-handle all fall to the `-313` catch-all** (no dedicated code);
assign is 🟡 partial in codegen (Ident-only targets); **compound assign + pre/postfix
++/-- are type-checked in sema but miscompiled in codegen** (op dropped); call is 🟡
(≤4 reg args, script-only); struct/array/enum/lambda/index/field/view rejected with
dedicated codes; `new`/`delete` unreachable (lexer gap).

---

## 5. Runtime / ABI

| Mechanism | Native (C++) | Self-hosted sema | Self-hosted codegen | Notes |
|---|---|---|---|---|
| Calling convention: ≤4 register args | ✅ Win64 (`rcx/rdx/r8/r9` GP, `xmm0-3` float) | ✅ (arity checked) | ✅ (GP only, `rcx/rdx/r8/r9`) | float regs unused (no floats). |
| Calling convention: >4 stack args | ✅ full (outgoing stack args past shadow) | ✅ (arity NOT limited at sema) | ❌ rejected | codegen hard-rejects `argc>4` ("at most 4 call args"). |
| Calling convention: aggregate args (by value) | ✅ full (`ceil(size/8)` words, `copy_bytes`) | ➖ | ❌ | no aggregates in subset. |
| Calling convention: aggregate returns | ✅ full (Win64 hidden-pointer) | ➖ | ❌ | |
| Calling convention: 16-byte stack alignment | ✅ full (`win64_call_frame_size`, `require_win64_call_alignment`) | ➖ | ✅ (`cg_round16`, 32-byte shadow) | maintained for the ≤4-arg path. |
| Shadow space (32 bytes) | ✅ | ➖ | ✅ | |
| rodata (string literals) | ✅ full (`FunctionRodataBase` AbsFixup + inline XOR decrypt) | ➖ | ❌ | no string support (`-309`); no rodata segment emitted. |
| Mutable globals + relocations | ✅ full (`GlobalsBase` AbsFixup, typed offsets) | ❌ `-317` | ❌ | globals rejected at sema. |
| Native / extension calls | ✅ full (symbolic `NativeFixup`, allowlisted by name) | ➖ (no native call concept) | ❌ | codegen only emits `call rel32` to **script** fn labels; no native table, no `mov_reg_native`. |
| `import` / module resolution | ✅ full (pre-lex `resolve_imports`) | ➖ host step | ➖ host step | done by the C++ host before the 4 files run. |
| `link` + cross-module `mod::fn()` | ✅ full (`ModuleRegistryBase` AbsFixup, keyed/legacy dispatch) | ⛔ `-316` (link decl) | ❌ | |
| Namespace resolution (`Name::member`) | ✅ full (`namespace_names`, `current_ns`) | ⛔ `-316` | ❌ | |
| Function handles + indirect calls | ✅ full (allowlist bit-test guard + `call [dispatch_base+slot*8]`) | ⛔ `-313` | ❌ | |
| Cross-module calls | ✅ full (per-module records table + guard) | ⛔ `-316`/`-313` | ❌ | |
| GC allocation (`new`/`delete`) | ✅ full (`__ember_gc_alloc_object`/`__ember_gc_delete_object`, write barriers, shadow-stack roots) | ➖ (no AST node) | ➖ | lexer gap. |
| Lambda env allocation | ✅ full (`__ember_gc_alloc_env`, by-ref capture) | ❌ `-307` | ❌ | |
| Coroutine runtime | ✅ full (Windows fibers: `CreateFiber`/`SwitchToFiber`, `__ember_coro_yield`) | ❌ `-308` | ❌ | Windows-only (see §6). |
| try/catch runtime | ✅ full (inline setjmp/longjmp over `context_t::catch_bufs`, gated on `use_context_reg`) | ❌ `-306` | ❌ | |
| throw runtime | ✅ full (restore saved state + longjmp; uncaught → host `TrapReason::UnhandledThrow`) | ❌ `-306` | ❌ | |
| Annotations (`@realtime`, `@obf`, `@on_tick`) | ✅ full (sema-enforced) | ❌ `-318` | ❌ | token-stream scan rejects any `Tok::At`. |
| Operator overloads (`vec3_add`, string concat) | ✅ full (sema resolves, codegen emits native call) | ⛔ (no overload table) | ❌ | |
| Dispatch table (script fn slots) | ✅ full (`DispatchTableBase` AbsFixup) | ✅ (fn slots modeled) | ✅ (in-buffer `call rel32` to entry labels) | self-hosted uses direct rel32 calls within one RX page, not a relocated dispatch base. |
| String encryption (XOR rodata) | ✅ full (per-compile key) | ➖ | ❌ | no strings. |

**Summary:** the self-hosted codegen implements a **minimal Win64 ABI slice** —
≤4 GP register args, 32-byte shadow, 16-byte alignment, direct script-to-script
`call rel32`. Everything else (stack args, aggregates, floats-in-xmm, rodata,
globals, natives, indirect calls, cross-module, GC, coroutines, try/catch) is
absent.

---

## 6. Platform-conditioned behavior

| Behavior | Native (C++) | Self-hosted | Notes |
|---|---|---|---|
| Windows coroutine execution | ✅ via fibers (`CreateFiberEx`/`SwitchToFiber`, `FIBER_FLAG_FLOAT_SWITCH`); `coroutine_init` converts the thread to a fiber; `__ember_coro_yield` switches back to the caller's fiber. | ❌ `-308` (rejected before any runtime) | The fiber runtime lives in `extensions/coroutine/ext_coroutine.cpp` (Windows-only; keyed-mode coroutine start returns a typed unsupported-mode failure because the r15/generation invariant across a fiber yield/resume is not yet guaranteed). |
| Linux coroutine execution | ⛔ stub / not built | ❌ `-308` | `platform.hpp` notes the coroutine fiber abstraction is `ucontext` on Linux — **TODO, unimplemented**. `platform.cpp`'s Linux JIT-memory path (mmap/mprotect) is written but **UNTESTED**. No Linux coroutine runtime exists. |
| try/catch execution | ✅ inline setjmp/longjmp; `EMBER_SETJMP`/`EMBER_LONGJMP` resolve to `__builtin_setjmp`/`__builtin_longjmp` on MinGW (full callee-saved restore) and standard `setjmp`/`longjmp` elsewhere. Gated on `ctx.use_context_reg` (B1 per-context model); without it, try/catch emits a trap. | ❌ `-306` | |
| JIT memory (W^X) | ✅ Windows `VirtualAlloc`/`VirtualProtect`; Linux `mmap`/`mprotect` (untested). | ➖ host | the self-hosted codegen emits bytes into an `array<u8>`; `make_executable` (a host native in `full_pipeline.ember`) does the W^X seal. |
| Native re-entry guard / call budgets | ✅ full (`context_t` call-depth, trap stub longjmp) | ➖ host | the self-hosted pipeline calls one `main` via `call_raw`; no budget enforcement in the 4 files. |

---

## 7. Reject-code index (self-hosted sema `unsupported(-3NN)`)

Every stable code the self-hosted sema emits, with the feature(s) it covers. Codes
are emitted from `sema.ember`'s `unsupported(code, feature, node)` helper.

| Code | Feature string | Covers |
|---|---|---|
| `-301` | "struct declaration" / "struct value" | `StructDecl`; `StructLit`; `FieldExpr` |
| `-302` | "enum declaration" / "enum value" | `EnumDecl`; `EnumAccess` |
| `-303` | "floating-point type" / "float literal" | `f32`/`f64` types; `FloatLit` |
| `-304` | "match" | `MatchStmt` |
| `-305` | "switch" | `SwitchStmt` |
| `-306` | "try/catch or throw" | `TryCatchStmt`; `ThrowStmt` |
| `-307` | "lambda" | `LambdaExpr` |
| `-308` | "coroutine/yield" | `YieldStmt` |
| `-309` | "string literal" | `StringLit` (incl. f-string + raw-string forms) |
| `-310` | "slice type" / "array type" / "array literal" / "array/slice expression" | slice & fixed-array types; `ArrayLit`; `IndexExpr`; `ViewExpr` |
| `-311` | "for-each" | `ForEachStmt` |
| `-312` | "defer" | `DeferStmt` |
| `-313` | "expression kind" (catch-all) | `CastExpr`; `TernaryExpr`; `SizeofExpr`; `OffsetofExpr`; `FnHandleExpr` (intra + cross-module); any other unhandled expr |
| `-314` | "statement kind" (catch-all) | `DoWhileStmt`; in-body `StaticAssertStmt`; any other unhandled stmt |
| `-315` | "non-i64 integer or function-handle type" | `i8/i16/i32/u8/u16/u32/u64` prims; bare `fn` type |
| `-316` | "top-level declaration" (catch-all) | `LinkDecl`; `NamespaceDecl`; `StaticAssertDecl` |
| `-317` | "global declaration" | `GlobalDecl` |
| `-318` | "annotation" | any `@…` annotation (`Tok::At`) |

**codegen** does **not** emit `-3xx` codes — it sets `cg_err=1` with a generic
"… unsupported in v1" message. The pipeline (`full_pipeline.ember`) maps a sema
`-3xx` straight through (`if (sr < 0) return sr;`) and a codegen failure to
`-401`. So the **stable, machine-readable subset boundary lives entirely in
sema**; codegen is a pure emit-or-flag layer.

---

## 8. NK-enum coverage check (parse.ember lines 54–117)

Every node kind the parser builds, and where sema/codegen route it. Confirms no
node kind is silently dropped.

| NK kind | Parser builds? | sema route | codegen route |
|---|---|---|---|
| `Type` (1) | ✅ | `resolve_type` (i64/bool/void ✅; else `-303`/`-310`/`-315`) | (consumed via slot types) |
| `FnDecl` (10) | ✅ | ✅ register + check body | ✅ emit fn |
| `Param` (11) | ✅ | ✅ (no default modeling) | ✅ (8-byte slot) |
| `StructDecl` (12) | ✅ | ❌ `-301` | ⛔ |
| `Field` (13) | ✅ | (with struct) ⛔ | ⛔ |
| `EnumDecl` (14) | ✅ | ❌ `-302` | ⛔ |
| `Variant` (15) | ✅ | (with enum) ⛔ | ⛔ |
| `GlobalDecl` (16) | ✅ | ❌ `-317` | ⛔ |
| `LinkDecl` (17) | ✅ | ❌ `-316` | ⛔ |
| `StaticAssertDecl` (18) | ✅ | ❌ `-316` | ⛔ |
| `NamespaceDecl` (19) | ✅ | ❌ `-316` | ⛔ |
| `LetStmt` (20) | ✅ | ✅ | ✅ |
| `IfStmt` (21) | ✅ | ✅ | ✅ |
| `WhileStmt` (22) | ✅ | ✅ | ✅ |
| `ForStmt` (23) | ✅ | ✅ | ✅ |
| `ForEachStmt` (24) | ✅ | ❌ `-311` | ⛔ |
| `DoWhileStmt` (25) | ✅ | ❌ `-314` (catch-all) | ⛔ |
| `ReturnStmt` (26) | ✅ | ✅ | ✅ |
| `BreakStmt` (27) | ✅ | ✅ | ✅ |
| `ContinueStmt` (28) | ✅ | ✅ | ✅ |
| `DeferStmt` (29) | ✅ | ❌ `-312` | ⛔ |
| `ExprStmt` (30) | ✅ | ✅ | ✅ |
| `BlockStmt` (31) | ✅ | ✅ | ✅ |
| `SwitchStmt` (32) | ✅ | ❌ `-305` | ⛔ |
| `Case` (33) | ✅ | (with switch) ⛔ | ⛔ |
| `MatchStmt` (34) | ✅ | ❌ `-304` | ⛔ |
| `MatchArm` (35) | ✅ | (with match) ⛔ | ⛔ |
| `YieldStmt` (36) | ✅ | ❌ `-308` | ⛔ |
| `ThrowStmt` (37) | ✅ | ❌ `-306` | ⛔ |
| `TryCatchStmt` (38) | ✅ | ❌ `-306` | ⛔ |
| `StaticAssertStmt` (39) | ✅ | ❌ `-314` (catch-all) | ⛔ |
| `IntLit` (50) | ✅ | ✅ | ✅ |
| `BoolLit` (51) | ✅ | ✅ | ✅ |
| `FloatLit` (52) | ✅ | ❌ `-303` | ⛔ |
| `StringLit` (53) | ✅ | ❌ `-309` | ⛔ |
| `Ident` (54) | ✅ | ✅ | ✅ |
| `BinExpr` (55) | ✅ | ✅ (all 18 ops, i64/bool) | ✅ (all 18 ops) |
| `UnaryExpr` (56) | ✅ | ✅ | ✅ |
| `AssignExpr` (57) | ✅ | ✅ (compound + postfix type-checked) | 🟡 partial (Ident-only; compound/postfix op dropped) |
| `CallExpr` (58) | ✅ | ✅ (arity) | 🟡 partial (≤4 reg, script-only) |
| `FieldExpr` (59) | ✅ | ❌ `-301` | ⛔ |
| `IndexExpr` (60) | ✅ | ❌ `-310` | ⛔ |
| `CastExpr` (61) | ✅ | ❌ `-313` (catch-all) | ⛔ |
| `TernaryExpr` (62) | ✅ | ❌ `-313` (catch-all) | ⛔ |
| `StructLit` (63) | ✅ | ❌ `-301` | ⛔ |
| `FieldInit` (64) | ✅ | (with struct lit) ⛔ | ⛔ |
| `ArrayLit` (65) | ✅ | ❌ `-310` | ⛔ |
| `ViewExpr` (66) | ✅ | ❌ `-310` | ⛔ |
| `FnHandleExpr` (67) | ✅ | ❌ `-313` (catch-all) | ⛔ |
| `LambdaExpr` (68) | ✅ | ❌ `-307` | ⛔ |
| `EnumAccess` (69) | ✅ | ❌ `-302` | ⛔ |
| `SizeofExpr` (70) | ✅ | ❌ `-313` (catch-all) | ⛔ |
| `OffsetofExpr` (71) | ✅ | ❌ `-313` (catch-all) | ⛔ |
| `Program` (90) | ✅ | ✅ (decl passes) | ✅ |

**C++ AST nodes with no self-hosted `NK` kind** (lexer/parser gap): `NewExpr`,
`DeleteExpr` — because `Kw_new`/`Kw_delete` are absent from the self-hosted `Tok`
enum and `keyword_kind`. These are Phase 4 prerequisites.

---

## 9. Dependency-ordered implementation roadmap (5 phases)

The phases below match the advisor's five-phase plan. Each phase lists the features
that fall in it (drawn from the open rows above) and the prerequisite that the
prior phase establishes. Work within a phase is roughly independent; work across
phases is strictly ordered.

### Phase 1 — Module-image / runtime ABI foundation

**Goal:** the self-hosted codegen can emit a loadable module image with rodata,
mutable globals, relocations, imports, and the full calling convention (stack
args, alignment, aggregates). Without this, every later phase that touches
data or calls is built on sand.

Features in this phase:
- **rodata segment** for string literals (`FunctionRodataBase` AbsFixup) — unblocks
  `-309` (string/raw-string/f-string).
- **mutable globals + `GlobalsBase` relocation** — unblocks `-317` (globals).
- **import / module resolution** as a self-hosted pre-lex pass (or an explicit
  host contract) — the 4 files currently rely on the C++ host for `import`.
- **`link` declarations + cross-module `mod::fn()` registry hop**
  (`ModuleRegistryBase` AbsFixup) — unblocks the `-316` `LinkDecl` path.
- **calling convention: >4 stack args** (outgoing stack past the 32-byte shadow) —
  unblocks the `reject_five_args` boundary.
- **calling convention: 16-byte alignment + shadow space** for all call shapes
  (already partial; generalize).
- **calling convention: aggregate args/returns** (`ceil(size/8)` words,
  `copy_bytes`, Win64 hidden-pointer returns) — prerequisite for structs-by-value
  (Phase 3) and lambdas (Phase 4).
- **native / extension call path** (symbolic `NativeFixup`, allowlisted by name) —
  prerequisite for operator overloads (Phase 3), GC (Phase 4), coroutines/try
  (Phase 5).

Prerequisite: none (this is the foundation).

### Phase 2 — Type / layout / lvalue foundation

**Goal:** the self-hosted sema/codegen handle the full scalar type system, all
casts, the layout helpers, general lvalues, and the assignment operators
correctly. Depends on Phase 1 (globals/relocations for global lvalues; native
calls for some helpers).

Features in this phase:
- **all integer widths** `i8/i16/i32/u8/u16/u32/u64` — unblocks `-315`
  (sign/zero-ext on load/store, width-aware arithmetic).
- **all cast kinds** (int↔int widening/narrowing, enum→int, float↔float,
  signed-int↔float; reject the deliberately-excluded kinds) — unblocks the
  `-313` `CastExpr` catch-all.
- **sizeof / align / offset helpers** — unblocks the `-313` `SizeofExpr` /
  `OffsetofExpr` catch-all.
- **general lvalues** (Ident + FieldExpr + IndexExpr + globals) — fixes the
  codegen "target must be an identifier" restriction.
- **globals as lvalues / rvalues** (read + write through `GlobalsBase`).
- **compound assignment** (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`) —
  fixes the codegen bug that drops the op (read-modify-write with single eval of
  the target).
- **postfix `++`/`--`** (return old value) and **prefix `++`/`--`** (return new
  value) — fixes the codegen bug.
- **ternary `?:`** (bool cond, same-type branches) — unblocks the `-313`
  `TernaryExpr` catch-all.
- **`static_assert` (in-body + top-level)** with a const-folding interpreter
  (true → elided, false → error, non-const → error) — unblocks `-314` (in-body)
  and the `-316` `StaticAssertDecl` path; also enables `constexpr fn` later.

Prerequisite: Phase 1 (globals/relocations; native-call plumbing for helpers).

### Phase 3 — Ordinary data + control flow

**Goal:** structs, arrays, slices, strings, indexing, literals, enums, match,
switch, do-while, defer, for-each (after arrays), and floats with SSE + the float
ABI. Depends on Phase 2 (layout/sizeof/offset, general lvalues, casts) and Phase 1
(aggregates ABI, rodata, natives for string/array overloads).

Features in this phase:
- **named structs** (layout table, field access, by-value copy) — unblocks `-301`
  (`StructDecl`, `StructLit`, `FieldExpr`).
- **fixed arrays `T[N]`** (contiguous layout, const-index elision, runtime bounds
  checks) — unblocks part of `-310`.
- **slices `T[]`** (`{ptr,len}` 2-word ABI) + **view `a[..]`** — unblocks the rest
  of `-310` (`IndexExpr`, `ViewExpr`, slice types).
- **string literals / raw strings / f-strings** (rodata from Phase 1; f-string
  desugaring in the parser; `string` handle + concat overload via native calls) —
  unblocks `-309`.
- **array literals `[1,2,3]`** (fixed-array + slice targets) — unblocks the
  `ArrayLit` `-310`.
- **typed + untyped enums** (variant folding, `enum_name` tag in `Type`,
  enum→int widening, int→enum rejection) — unblocks `-302` (`EnumDecl`,
  `EnumAccess`).
- **`match`** (literal/bool/`_` patterns, struct destructure, guards) — unblocks
  `-304` (depends on structs for destructure).
- **`switch`/`case`/`default`** (no fallthrough; cmp cascade or jump table) —
  unblocks `-305`.
- **`do-while`** — unblocks the `-314` `DoWhileStmt` catch-all.
- **`defer`** (lexical-block-exit LIFO; per-iteration reset in loops) — unblocks
  `-312`.
- **`for-each`** (slice first, then array-handle iterable) — unblocks `-311`
  (depends on slices/arrays).
- **floats `f32`/`f64`** (SSE moves `movss/movsd`, `xmm0` return, `xmm0-3` args,
  cvtss2sd/cvtsi2ss/etc., float compare) — unblocks `-303` (depends on Phase 1
  float ABI + Phase 2 int↔float casts).

Prerequisite: Phases 1 + 2.

### Phase 4 — Callable / runtime

**Goal:** fn handles, indirect calls, cross-module calls, `new`/`delete` via GC,
and lambdas (after structs + aggregates + fn handles + GC). Depends on Phase 1
(native calls, cross-module registry), Phase 2 (general lvalues), Phase 3
(structs + aggregate ABI for lambda env).

Features in this phase:
- **lexer + parser: `new`/`delete` keywords + `NewExpr`/`DeleteExpr` `NK` kinds**
  — closes the lexer/parser gap (prerequisite for the rest of this phase).
- **function handles `&fn`** (bake slot as i64; `is_fn_handle` in `Type`) —
  unblocks the `-313` `FnHandleExpr` catch-all.
- **recorded-signature `fn(Args)->Ret` types** (arg checks at call sites) +
  bare-`fn` "any fn" escape hatch.
- **indirect calls** (allowlist bit-test guard + `call [dispatch_base+slot*8]`) —
  runtime validation of handles.
- **cross-module handles `&mod::fn`** (`bit63` encoding, per-module records table)
  + **cross-module calls** (resolve via `ModuleRegistryBase`) — unblocks the
  `-316`/`-313` cross-module paths (ties into Phase 1 `link`).
- **`new T` / `delete expr`** via `__ember_gc_alloc_object` /
  `__ember_gc_delete_object` (GC-traced managed pointers; shadow-stack roots;
  write barriers) — unblocks the `new`/`delete` gap.
- **lambdas `fn(…){…}`** (synthetic `__lambda_N` fn, by-value + by-ref capture,
  `__ember_gc_alloc_env` 16-byte `{slot,env_ptr}`) — unblocks `-307` (depends on
  structs + aggregate ABI + fn handles + GC env).
- **operator overloads** (sema overload table → native call) — unblocks
  vec/string/mat arithmetic (depends on Phase 1 native calls + Phase 3 structs).
- **`priv fn` / `constexpr fn`** (export-surface opt-out; bounded const-eval
  interpreter reusing the Phase 2 static_assert folder) — closes the `FnDecl`
  modifier gaps.

Prerequisite: Phases 1 + 2 + 3.

### Phase 5 — Nonlocal control

**Goal:** try/catch/throw and coroutines/yield, both via host thunks so the
self-hosted codegen only lowers to runtime calls. Depends on Phase 1 (native
calls) and Phase 4 (fn handles for coroutine_start/next).

Features in this phase:
- **`try`/`catch`/`throw`** — lower to the host's setjmp/longjmp runtime
  (`context_t::catch_bufs`); the self-hosted codegen emits the inline setjmp
  save + catch-label binding + `thrown_value` read, gated on `use_context_reg`.
  Unblocks `-306`.
- **`yield` + coroutines** — lower `yield` to the `__ember_coro_yield` native
  (host fiber switch); mark containing fn `is_coroutine`; wire
  `coroutine_start`/`coroutine_next`/`coroutine_done`. Unblocks `-308`.
  **Windows-only** at runtime (fibers); the Linux `ucontext` path is a host
  stub (see §6) — the self-hosted codegen is platform-agnostic, it only emits
  the native call.
- **`@realtime` / annotations generally** (sema-enforced annotation metadata on
  decls, e.g. the realtime alloc/IO/new-delete/try blocklist) — unblocks `-318`
  (currently a blanket token-scan reject).

Prerequisite: Phases 1 + 4 (native calls; fn handles for coroutine_start).

---

## 10. At-a-glance phase assignment (all open rows)

| Open feature | Phase |
|---|---|
| rodata / string literals / raw strings / f-strings (desugar) | 1 (rodata) + 3 (string type) |
| mutable globals + relocations | 1 |
| `import` / module resolution | 1 |
| `link` + cross-module `mod::fn()` | 1 (+ 4 handles) |
| >4 stack args | 1 |
| aggregate args/returns ABI | 1 |
| native / extension calls | 1 |
| `i8/i16/i32/u8/u16/u32/u64` | 2 |
| all cast kinds | 2 |
| `sizeof` / `offsetof` | 2 |
| general lvalues (field/index/global) | 2 |
| compound assign (all 10 ops) | 2 |
| postfix / prefix `++`/`--` | 2 |
| ternary `?:` | 2 |
| `static_assert` (in-body + top-level) + `constexpr fn` | 2 (+ 4) |
| named structs | 3 |
| fixed arrays `T[N]` | 3 |
| slices `T[]` + view `a[..]` | 3 |
| array literals | 3 |
| typed + untyped enums | 3 |
| `match` (incl. struct destructure + guards) | 3 |
| `switch` / `case` / `default` | 3 |
| `do-while` | 3 |
| `defer` | 3 |
| `for-each` (slice, then array) | 3 |
| `f32` / `f64` (SSE + float ABI) | 3 |
| operator overloads (vec/string/mat) | 4 (needs 1+3) |
| `new`/`delete` lexer+parser+GC | 4 |
| fn handles `&fn` + recorded sigs | 4 |
| indirect calls (allowlist guard) | 4 |
| cross-module handles `&mod::fn` | 4 |
| lambdas (by-value + by-ref capture) | 4 |
| `priv fn` | 4 |
| `try`/`catch`/`throw` | 5 |
| `yield` + coroutines | 5 |
| annotations (`@realtime` etc.) | 5 |

**Supported today (no phase needed):** `i64`/`bool`/`void`; plain `fn`; `let`/
`let mut`/`auto`/`const` (local); `if`/`else`; `while`; C-style `for`; `return`;
`break`; `continue`; expr stmt; block; int/bool literals; ident; all 18 binary
ops (i64/bool); unary `-`/`!`/`~`; plain `=` assign (Ident lvalue); script calls
(≤4 reg args); the minimal Win64 ABI slice (≤4 GP args, 32-byte shadow, 16-byte
alignment, direct `call rel32`).

---

*Derived from `src/ast.hpp`, `src/parser.cpp`, `src/sema.cpp`, `src/codegen.cpp`,
`src/lexer.{hpp,cpp}`, `src/platform.{hpp,cpp}`, `src/x64_emitter.hpp`,
`src/context.hpp`, `extensions/coroutine/ext_coroutine.cpp`, and the self-hosted
`lex.ember` / `parse.ember` / `sema.ember` / `codegen.ember` / `full_pipeline.ember`.
No source files were modified.*
