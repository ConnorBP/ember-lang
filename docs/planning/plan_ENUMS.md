# Implementation Plan — Tier 1: Script-Side `enum`

> Research / planning document. **No source is written here.** Every claim
> below is anchored to a firsthand read of the current tree; the seam each
> decision hangs on is quoted with a file:line so the implementer can verify
> the plan against the code before touching it.

## 0. What this is

ROADMAP Tier 1's first bullet (`../ROADMAP.md:34-38`):

> **`enum`** — script-side `enum E { A, B, C }` declaring named i32
>  constants. Grammar + `EnumBuilder` (already noted as deferred in
>  `../spec/BINDING_API.md` Section 5). Trigger: a real script wants more than ~5
>  related constants and the global-flat hurts readability. Dep: none.

Today ember has no `enum`. The constant mechanism is `const` locals +
`global`/`global const` decls (`tests/lang/valid_globals_calls.ember:2-4`,
`const PI : f32 = 3.14f;` / `global g_proc : i64 = 0;`). This plan adds a
script-side enum as **named i32 compile-time constants** with the minimum
surface that survives the existing pipeline unchanged downstream of sema.

The decisive constraint that shapes the whole design is in `sema.cpp:1057`
and `codegen.cpp:2010` — see §5. Everything else falls out of honoring it.

---

## 1. Syntax decisions (with rationale)

### 1.1 Access form: `E::A`, not bare `A`

**Decision:** variants are referenced as `E::A`. Bare `A` is **not** in scope.

**Rationale (from the code):**
- `Tk::DoubleColon` already exists as a token (`src/lexer.cpp:40`,
  `tok_spelling`), and the parser already special-cases it in
  `parse_postfix` (`src/parser.cpp`, the `if (k==Tk::DoubleColon)` block).
  So `::` is a known, lexed, parsed operator in the language today — it is
  *not* a new token to introduce. The only work is to **extend** the
  existing `::` postfix handling (which currently hard-requires the base to
  be a module alias and builds a `CallExpr` expecting a following `(`) to
  also accept `EnumName::Variant` as a value expression. See §3.5.
- ember already uses `::` for modules (`mod::fn()`, `../MODULES.md §6`,
  `sema.hpp ModuleExportTable`). Using `::` for enum variants keeps one
  scoped-name operator for both — a reader who knows `mod::fn` immediately
  reads `Color::Red` the same way. Bare `A` (C-style) would collide with
  the existing `Ident` resolution path (`sema.cpp:398`, `lookup_var`) and
  would inject every variant name into the flat global namespace, which is
  exactly the readability problem enums exist to avoid (ROADMAP: "the
  global-flat hurts readability"). Rust-style `E::A` is the form that
  matches ember's existing scoped-operator convention.
- Bare `A` would also force every variant to be registered into `Checker`'s
  `globals` map (`sema.cpp:213`, `std::unordered_map<std::string,const Type*>`),
  polluting the global namespace across all enums and making name collisions
  between two enums' variants a real (and common) error. `E::A` namespaces
  per-enum, so `Color::Red` and `Light::Red` coexist — the normal case for
  flags/state enums in real handler code.

### 1.2 Typed vs untyped: **untyped (C-style), v1**

**Decision:** an enum-typed value is just `i32`. `let x: Color = Color::Red;`
is **not** supported in v1 — write `let x: i32 = Color::Red;`. The enum
exists to produce i32 constants, not to introduce a new type.

**Rationale:**
- The `Type` struct (`src/ast.hpp:22-32`) has `Prim` + `struct_name` +
  `is_slice`/`array_len`/`elem` and **no enum variant**. The spec
  (`../spec/TYPE_SYSTEM.md §1`) deliberately lists exactly the primitives that
  exist; `../spec/BINDING_API.md §1`'s `TypeId` enum comment explicitly says
  "no `t_enum`... enums are deferred." Adding a typed enum means a new
  `Type` shape, new `TypeId`, sema enforcement that a `let x: Color` only
  accepts `Color::` variants (a per-enum-type compatibility rule that
  doesn't exist today — `../spec/TYPE_SYSTEM.md §6` only has nominal struct
  equality and numeric widening), and an "is this i32 or is it a tag"
  question at every integer operation site. That's a real subsystem, not a
  Tier-1 nicety.
- The ROADMAP spec line is unambiguous: "declaring named **i32
  constants**." An enum value IS an i32. The whole point of Tier 1 is the
  cheapest thing that removes the global-flat readability problem.
  Typed enums are explicitly deferred to Tier 2+ (see §8).
- This keeps `parse_type` unchanged in behavior: an enum name used in a
  type position is **not** a type in v1 (see §3.4 — error, same as an
  unknown struct name today, rather than silently becoming an i64 handle).

### 1.3 Values: auto-increment from 0, optional explicit, C-shape

**Decision:** `enum E { A, B = 5, C, D = 10, E }` — first variant defaults to
0; each variant without an explicit `= value` is `prev + 1`; an explicit
`= constexpr_int` sets the next base. Mirrors C exactly.

**Rationale:**
- "named i32 constants" + "auto-increment + optional explicit" is the
  C shape the ROADMAP implies, and it's the shape that covers the two real
  use cases (sequential state IDs, and packed flag bits `A=1,B=2,C=4`).
- The explicit-value expression is restricted to a **compile-time integer
  constant** in v1 — i.e. what `try_eval_const_i64` already folds
  (`sema.cpp:127`: IntLit / UnaryExpr-Neg-of-literal / BinExpr-of-literals).
  This reuses the existing const-folder verbatim; no new evaluator. (A
  value like `A = PREV + 1` referencing another variant is a *nicety* —
  defer to Tier 2's `constexpr` function evaluation; see ROADMAP line 53.
  v1 explicit values are literals and literal-arithmetic only, same as
  array sizes / `const` initializers today.)
- Values are `i32`-range-checked at sema (the ROADMAP says i32; the const
  folder yields i64, so sema rejects `= 3000000000` against an i32 enum
  with the existing range check shape from `adapt_int_lit`,
  `sema.cpp:311`: `i32` is `[-2147483648, 2147483647]`).
- Duplicate values **within one enum** are a compile error (sema, with the
  offending pair in the message). This is the one place v1 enums are
  stricter than C — C allows it and it's a classic footgun; rejecting it
  costs one `unordered_set<int32_t>` per enum and matches ember's
  "same-type-required, no silent footguns" stance (`../spec/TYPE_SYSTEM.md §6/§7`).
  (If a real flags enum later wants deliberate aliasing, that's the typed
  enum Tier 2 work; v1 untyped enums rejecting duplicates is the safe
  default.)

### 1.4 Underlying type: `i32`, fixed

The ROADMAP says i32. The resolved `IntLit` an enum variant folds to
therefore gets `Prim::I32` as its `ty` (set by sema during the rewrite in
§5, using the same `adapt_int_lit`/intern path `check_expr`'s IntLit case
already uses at `sema.cpp:325-328`). No `i64`/`u32`/configurable backing in
v1 — one underlying type, matching the spec, matching how `switch` case
values are compared today (`codegen.cpp:2010`, plain `cmp r13,rax` over
64-bit `rax` loaded from `mov rax, imm64` — an i32 value sign-extends
cleanly into the i64 `rax` the existing comparison uses, zero new codegen).

---

## 2. AST — `EnumDecl`

A new top-level declaration, parallel to `StructDecl`/`GlobalDecl`
(`src/ast.hpp:120-130`). Goes into `Program` as a new vector.

```cpp
// in src/ast.hpp, alongside StructDecl/GlobalDecl/LinkDecl
struct EnumVariant {
    std::string name;
    // nullptr iff no explicit `= value` (auto-increment). A constexpr-
    // foldable integer expression (IntLit / -IntLit / BinExpr-of-literals),
    // same restricted set try_eval_const_i64 already handles (sema.cpp:127).
    // Resolved by sema; NOT resolved by the parser.
    ExprPtr explicit_value;
    Loc loc;
    int64_t resolved = 0;   // sema fills: the variant's final i32 value
};

struct EnumDecl {
    std::string name;
    std::vector<EnumVariant> variants;
    Loc loc;
};

// in struct Program (src/ast.hpp:175-189), add alongside structs/globals/funcs/links:
struct Program {
    std::vector<StructDecl> structs;
    std::vector<GlobalDecl> globals;
    std::vector<FuncDecl>   funcs;
    std::vector<LinkDecl>   links;
    std::vector<EnumDecl>   enums;   // NEW (Tier 1)
    // ... rest unchanged
};
```

**Why an `ExprPtr explicit_value` and not a bare `int64_t`:** keeping it an
expression means the parser does zero value resolution (consistent with how
it treats `GlobalDecl::init` — `src/parser.cpp` `parse_global` stores the
raw `parse_expr()`, sema folds it). This keeps "parser builds shape, sema
folds values" — the existing layering (`../spec/COMPILER_PIPELINE.md §4` pass
ordering). `resolved` is filled by sema in a dedicated enum-resolution
sub-pass before any function body is checked (see §4.1), so that
`E::A`-as-a-value resolution in function bodies already has a finalized
value table to consult.

**No new `Type` shape.** An enum value is an `IntLit` after sema; the
`Type` it carries is `Prim::I32`. `../spec/TYPE_SYSTEM.md` gets one new
sub-section (§11.8 or a §12 "enum") stating "enums are named i32
constants; an enum-typed value is an i32" — no `Type`/`Prim` change.

Also add a new expression node for the access form (§3.5):

```cpp
// in src/ast.hpp, alongside the other Expr nodes
// E::A — resolved by sema to an IntLit carrying the variant's i32 value.
// Exists only between parse and sema's check_expr; after sema the owning
// ExprPtr points at an IntLit, not an EnumAccessExpr (see §5 for why the
// in-place rewrite is mandatory, not optional).
struct EnumAccessExpr : Expr { std::string enum_name; std::string variant; };
```

---

## 3. Parser changes

### 3.1 New keyword `enum`

`src/lexer.cpp:9-24` keyword map: add `{"enum", Tk::Kw_enum}`.
`src/lexer.hpp` `Tk` enum (line 16-24 region): add `Kw_enum` to the
`Kw_*` group. `tok_spelling` (`src/lexer.cpp:31+`): add a
`case Tk::Kw_enum: return "enum";`.

This is the **only** new token. `::` (`Tk::DoubleColon`) already exists.
No new punctuation.

### 3.2 `parse_program` top-level dispatch

`src/parser.cpp` `parse_program` (the `while (!at(Tk::Eof))` loop) currently
dispatches on `Kw_fn`/`Kw_struct`/`Kw_global`/`Kw_const`/`Kw_link`/`;`.
Add:

```text
} else if (at(Tk::Kw_enum)) {
    if (!anns.empty()) throw ParseError("annotations on enums not supported v1", ...);
    prog.enums.push_back(std::move(*parse_enum()));
}
```

(Mirrors the `Kw_struct` branch exactly, including the annotations-not-
supported guard that `struct`/`global`/`link` already enforce — consistent
with v1 not annotating anything but functions, `../spec/TYPE_SYSTEM.md §10`.)

### 3.3 New `parse_enum()`

```text
enum_decl := 'enum' IDENT '{' (variant (',' variant)* ','?)? '}'
variant   := IDENT ('=' constexpr_int_expr)?
```

- Trailing comma allowed (matches `struct` field lists in spirit, and is
  the C shape people expect for multi-line enums).
- `constexpr_int_expr` is parsed as a **full `parse_expr()`** — the
  *restriction* to "compile-time integer constant" is enforced by sema
  (`try_eval_const_i64`), not the parser, identical to how
  `GlobalDecl::init` and array sizes are handled (parser accepts an expr,
  sema rejects a non-constant). This keeps the parser dumb.
- Stored as `EnumVariant{name, explicit_value=parse_expr() or nullptr, loc}`.
  `resolved` left at 0 for sema.

### 3.4 `parse_type`: enum name is **not** a type in v1

`src/parser.cpp` `parse_type` `case Tk::Ident:` currently treats any
identifier as a named struct/handle type (`prim=I64`, `struct_name=tk.text`).
In v1 an enum name appearing in a type position (`let x: Color = ...`)
should be an error, not a silent i64 handle.

**Decision:** leave `parse_type`'s `Ident` case **as-is** (it can't know
enum-vs-struct at parse time — enums aren't registered until sema). Instead,
sema rejects a `let`/`param`/`field`/`global` whose declared type's
`struct_name` matches a registered **enum** name (not a registered struct)
with "enum 'E' is not a type in v1; use i32" — a cheap check in sema's
existing type-resolution pass. This keeps the parser untouched on the type
side and makes the "typed enum" upgrade (§1.2, §8) a pure sema+AST change
later, with the parser already doing the right thing.

(Alternative considered: resolve enum names in `parse_type`. Rejected —
the parser has no symbol table today; struct-name resolution is deferred to
sema by design, `../spec/COMPILER_PIPELINE.md §4` pass 1. Enum names should follow
the same rule.)

### 3.5 `parse_postfix` `::` — extend for enum variant values

The existing `::` block (`src/parser.cpp`, the `if (k==Tk::DoubleColon)`
case in `parse_postfix`) requires base to be an `Ident`, calls it a
module alias, and builds a `CallExpr` with `module_alias` set, expecting
`(` to follow. `E::A` (a value, no parens) is **not** currently parseable.

**Decision:** introduce a new postfix `::` outcome that is *not* a call.
Two clean options:

- **Option A (preferred): the `EnumAccessExpr` node from §2.**
  parse_postfix's `::` case, when the token after `::` is an `Ident` and
  the token after *that* is **not** `(`, builds an `EnumAccessExpr`. When
  the token after the variant IS `(`, it stays the existing `mod::fn(args)`
  `CallExpr` path (so `mod::fn()` keeps working unchanged — a module alias
  followed by `::fn(` is unambiguously a cross-module call). This is a
  one-token lookahead split, no backtracking.
- Option B (rejected): overload `CallExpr` with a sentinel. Rejected — it
  muddies `CallExpr` (which has 9 fields already, `src/ast.hpp:75-100`)
  with a "this is actually an enum access" mode, and forces sema to
  special-case it before doing call resolution. A distinct node is cleaner
  and matches how the AST already separates concerns (`ViewExpr` vs
  `IndexExpr`).

`EnumAccessExpr` is a **leaf for the const-folder**: sema's `check_expr`
case for it resolves `(enum_name, variant)` against the enum table built
in §4.1, then **rewrites the node in place to an `IntLit`** carrying the
resolved i32 value (see §5). After that rewrite, neither the const-folder
(`try_eval_const_i64`, `sema.cpp:127`) nor codegen ever see an
`EnumAccessExpr` — they see an `IntLit`. This is the central design move;
see §5 for why it's forced.

---

## 4. Sema changes

### 4.1 New pass: enum resolution (before function-body checks)

`sema()` (`src/sema.cpp:1109`) currently: register globals → register
script sigs → check each fn body. Insert a new pass between
"register globals" and "register script sigs":

```text
// pass 1.5: resolve enums (fills EnumVariant::resolved, builds lookup tables)
for (auto& e : prog.enums) {
    int64_t next = 0;
    std::unordered_set<int32_t> seen;
    // also: record the enum name in a set of "registered enum names"
    //   for the parse_type-rejects-enum-names check (§3.4)
    for (auto& v : e.variants) {
        if (v.explicit_value) {
            int64_t ev;
            if (!try_eval_const_i64(*v.explicit_value, ev))
                err("enum variant explicit value must be a compile-time integer constant",
                    v.loc);
            // i32 range check (reuse the bound from adapt_int_lit, sema.cpp:311)
            if (ev < -2147483648LL || ev > 2147483647LL)
                err("enum variant value out of i32 range", v.loc);
            next = ev;
        }
        v.resolved = next;
        if (!seen.insert(int32_t(next)).second)
            err("enum '" + e.name + "' has duplicate value " + to_string(next) +
                " (variant '" + v.name + "')", v.loc);
        // also: duplicate variant NAME within an enum is an error
        ++next;
    }
    // build (enum_name -> (variant_name -> i32 value)) table on the Checker
    //   for §4.2's EnumAccessExpr resolution
}
```

This reuses `try_eval_const_i64` (`sema.cpp:127`) verbatim — it already
folds `IntLit` / `UnaryExpr{Neg,BitNot}` / `BinExpr{Add,Sub,Mul,And,Or,Xor,
Shl,Shr}` of literals, which is exactly the "constexpr_int_expr" subset v1
allows for explicit values. No new evaluator.

**Duplicate enum names** across two `enum` decls: error (mirrors how struct
name collision would be handled; ember has one global namespace per decl
kind today). **Duplicate variant names within one enum**: error.

### 4.2 `check_expr`: the `EnumAccessExpr` case → rewrite to `IntLit`

Add to `Checker::check_expr` (`src/sema.cpp:323`, the dispatch chain):

```text
if (auto* ea = dynamic_cast<EnumAccessExpr*>(&e)) {
    auto eit = enum_values.find(ea->enum_name);   // the table built in §4.1
    if (eit == enum_values.end())
        err("unknown enum '" + ea->enum_name + "'", ea->loc);
    else {
        auto vit = eit->second.find(ea->variant);
        if (vit == eit->second.end())
            err("enum '" + ea->enum_name + "' has no variant '" + ea->variant + "'", ea->loc);
        else {
            // REWRITE IN PLACE: replace this node's identity with an IntLit.
            // See §5 for why this is mandatory, not optional.
            // (Mechanism: since check_expr takes Expr& and the AST is
            //  owned by unique_ptr up the tree, the clean way is to have
            //  the PARENT re-point its ExprPtr. Concretely: check_expr
            //  returns the resolved type, but the rewrite needs the
            //  owning slot. Two viable mechanics — pick at impl time:
            //   (a) check_expr takes ExprPtr& instead of Expr& for the
            //       children it recurses into (small signature change,
            //       localized), and the EnumAccessExpr branch does
            //       e = make_unique<IntLit>(v.resolved); e.ty = i32;
            //   (b) keep Expr& but stamp a sentinel on the EnumAccessExpr
            //       and have the const-folder + a tiny pre-codegen pass
            //       lower it. Rejected — see §5: the switch case-value
            //       check is a dynamic_cast<IntLit*> at sema time, so
            //       the rewrite MUST be visible to sema's own switch
            //       check, not deferred to codegen. So (a).)
        }
    }
    e.ty = intern(make_prim(Prim::I32));
    return e.ty;
}
```

The implementer picks the exact re-pointing mechanism during impl, but the
**requirement** is non-negotiable: by the time `check_expr` returns, the
expression node an `EnumAccessExpr` occupied must BE an `IntLit` (so that
sema's own switch case-value check at `sema.cpp:1057` sees an `IntLit`).
Mechanism (a) — threading `ExprPtr&` through the child-recurse sites — is
the one that satisfies this without a second AST pass. (This is the same
"sema mutates AST in place" contract `../spec/COMPILER_PIPELINE.md §3` already
states; the only wrinkle is that an in-place identity swap needs the owning
`unique_ptr`, not just a `Node&`.)

### 4.3 `try_eval_const_i64`: no change needed

Because §4.2 rewrites `EnumAccessExpr` → `IntLit` **before** any const-fold
site runs, `try_eval_const_i64` (`sema.cpp:127`, which returns `false` for
`Ident`/`CallExpr`/etc.) needs no enum awareness: by the time it sees the
expression, the enum access is already an `IntLit` it folds normally. This
is why the rewrite (not a "teach the folder about enums") is the right
design — it fixes the const-folder, the switch case-value check, AND
codegen in one move. (§5.)

### 4.4 Switch scrutinee + case values: no change needed

`sema.cpp:1046-1062` (the `SwitchStmt` case): the subject must be int/bool
(an enum-typed value is an `i32` IntLit after §4.2, so `case Color::Red:`'s
subject-position enum access is already an i32 — passes the `is_int()`
check). The case-value check at `sema.cpp:1057`:

```cpp
if (!dynamic_cast<IntLit*>(c.value.get()) && !dynamic_cast<BoolLit*>(c.value.get()))
    err("case value must be a literal constant", ...);
```

— after the §4.2 rewrite, `case Color::Red:`'s `c.value` IS an `IntLit`, so
this check passes with no change. This is the seam that forces the rewrite
design; see §5. **No edit to this block.**

### 4.5 Type-position enum-name rejection (§3.4 follow-through)

In sema's type-resolution (wherever a `let`/`param`/`field`/`global`/`return`
type's `struct_name` is interned), if the name matches a registered enum
(not a registered struct), error: "enum 'E' is not a type in v1; declare
the binding as `i32`." Cheap: one set lookup at the points that already
inspect `struct_name`. (This is what makes `let x: Color = ...` a clean
error instead of a silent i64 handle, and is the single hook the Tier-2
typed-enum upgrade flips from "reject" to "accept as a typed i32".)

### 4.6 Global initializer & array-size contexts

Because `EnumAccessExpr` → `IntLit` at §4.2, an enum variant used as a
`global const MASK : i32 = Flags::A | Flags::B;` initializer works for
free: the `BinExpr` over two rewritten `IntLit`s folds via
`try_eval_const_i64`, and `globals.hpp`'s `eval_global_initializers`
bakes it into the globals block at load — no change to `globals.hpp`.
Same for a fixed-array size `let buf: [u8; Color::Count]` if `Count` is a
variant (the array-size path already requires a foldable constant; the
rewrite makes the enum access foldable). Both fall out of §4.2 + §4.3 for
free and are noted in the test matrix (§7) as coverage, not new code.

---

## 5. Codegen: **no change** (confirmed)

This is the headline confirmation the task asked for. Three firsthand
anchors make it airtight:

1. **`sema.cpp:1057`** — switch case values must be `IntLit`/`BoolLit` or
   sema errors. So an enum variant in a `case` position MUST be an IntLit
   by the time sema is done, or it's a compile error. This is what forces
   §4.2's rewrite to happen *in sema*, not as a codegen pre-pass.

2. **`codegen.cpp:2010`** — switch emit does `eval(*sw->cases[i].value)`
   and compares `r13` (subject) to `rax` (case value) at runtime. `eval`'s
   `IntLit` case (`codegen.cpp:904`) is `mov rax, imm64` from `lit->v`. An
   `i32` value sign-extends cleanly into the 64-bit `rax` the existing
   `cmp r13,rax` already uses. An enum variant rewritten to
   `IntLit{v=resolved_i32}` hits this exact path. **Zero codegen edit.**

3. **`codegen.cpp:923`** — `eval`'s `Ident` case looks up `locals` then
   `g_globals_for_codegen`, and falls through (emits *nothing*) if neither
   matches. If enum variants were exposed as bare `Ident`s (the rejected
   §1.1 option), codegen would silently emit a broken comparison. The
   `EnumAccessExpr` → `IntLit` rewrite means codegen never sees an enum
   access at all — it sees the literal. This is the second reason the
   rewrite (not "register enum variants as globals") is correct:
   registering them as codegen globals would require a new
   `g_globals_for_codegen` population path AND a const-folding path AND a
   switch-case-value special case; the rewrite needs none of those.

**Therefore: codegen is untouched.** An enum variant IS an `IntLit` at
codegen time. The only files that change are `src/lexer.{hpp,cpp}`,
`src/parser.cpp`, `src/ast.hpp`, `src/sema.{hpp,cpp}` (the new pass + the
`check_expr` case + the enum-name-not-a-type check), and the docs
(`../spec/COMPILER_PIPELINE.md §1/§2` grammar, `../spec/TYPE_SYSTEM.md` new enum section,
`../spec/BINDING_API.md §5` un-defer `EnumBuilder` if §6 is in scope).

---

## 6. Host-side `EnumBuilder` (../spec/BINDING_API.md §5) — **defer to v2-of-enums**

`../spec/BINDING_API.md §5` currently says:

> `EnumBuilder`: **dropped from v1** — no `enum` keyword in the v1
>  grammar... Host-side named constants use `set_global`. Revisit if
>  script-side enum syntax is added later (would need both a grammar
>  addition and this builder — YAGNI to add the builder alone first).

The script-side enum this plan adds is exactly the "grammar addition"
half of that revisit condition. The question is whether to also land the
host-side `EnumBuilder` in the same change.

**Decision: defer. Ship script-only enums first.**

**Rationale:**
- The script-side path (§2-§5) needs no `EnumBuilder` — the enum table is
  built by sema from `Program::enums` (§4.1). A host that wants an enum
  today can still expose named constants via `set_global` (the existing
  mechanism, `../spec/BINDING_API.md §5`'s own fallback), and a script can
  `import`/`link` a module that declares the enum if it must be shared.
  Neither needs a host-side builder.
- `EnumBuilder` is a *host→script* direction: a host registers an enum so
  scripts can use its variants (e.g. a host flags enum a script switches
  on). That requires: (a) a host API to declare the enum + variants, (b)
  a way for that declaration to enter sema's enum table (today the table
  is built purely from `Program::enums` — a host enum would need a
  parallel input channel into the `Checker`), and (c) a decision on
  whether host enums and script enums share a namespace / can collide.
  Each of those is a real design question, not a mechanical addition.
- The trigger in the ROADMAP is "a real script wants >5 related
  constants" — script-authored enums. Host-authored enums are a separate
  trigger (a host wanting to expose a flags enum to scripts), not named in
  the Tier 1 bullet, and not yet demonstrated by a real binding.
- Deferring keeps the change small and the surface matched 1:1 to a
  reachable feature (the `../spec/BINDING_API.md §3` rationale pattern: "a builder
  method with no corresponding language feature to drive it is dead API
  surface"). The script feature is the language feature; `EnumBuilder` is
  the binding feature — they can land together but don't have to, and the
  smaller change is easier to validate against the test matrix in §7.

**What the doc edit looks like instead:** `../spec/BINDING_API.md §5`'s note is
updated from "dropped from v1 — no `enum` keyword" to "script-side `enum`
lands in Tier 1 (see `plan_ENUMS.md`); `EnumBuilder` (host→script enum
registration) remains deferred — host enums use `set_global` until a real
binding demonstrates the need." Honest, no overclaim.

---

## 7. Test matrix

All under `tests/lang/`, run by `tests/run_lang_tests.sh` against
`ember_check` (parse) + `sema_check` (sema) + `ember_cli` (end-to-end),
following the existing `sema_valid_*` / `sema_invalid_*` / `invalid_*` /
`valid_*` classification (see `tests/run_lang_tests.sh:23-37`).

### 7.1 Parse-only (must lex+parse OK) — `valid_enum_*.ember`

| File | Covers |
|---|---|
| `valid_enum_basic.ember` | `enum E { A, B, C }` minimal; auto-increment 0,1,2 |
| `valid_enum_explicit.ember` | `enum F { A = 1, B = 2, C = 4 }` packed flags |
| `valid_enum_mixed.ember` | `enum M { A, B = 5, C, D = 10, E }` (C=6, E=11) |
| `valid_enum_trailing_comma.ember` | `enum E { A, B, C, }` trailing comma accepted |
| `valid_enum_empty.ember` | `enum E {}` (zero variants) — decide: allow or reject? **Recommend allow** (matches empty struct `struct Empty{}` in `../spec/TYPE_SYSTEM.md §2`; a zero-variant enum is just a name with no constants, harmless) |
| `valid_enum_use.ember` | `E::A` in expression position, in a `let`, in a `return`, as a `global const` initializer, as a `switch` case value, in a `BinExpr` (`Flags::A \| Flags::B`) |

### 7.2 Sema-valid (must sema OK) — `sema_valid_enum_*.ember`

| File | Covers |
|---|---|
| `sema_valid_enum_autoincr.ember` | declares enum, asserts (via the existing `assert_eq_*` native folding, `sema.cpp:554`) that `E::B == 1` at compile time — pins auto-increment |
| `sema_valid_enum_switch.ember` | `switch (x) { case Color::Red: ...; case Color::Green: ...; default: ... }` — the §5 seam: enum case values reach sema as IntLits and codegen as `mov rax, imm` |
| `sema_valid_enum_flags_expr.ember` | `let m: i32 = Flags::A \| Flags::B \| Flags::D;` — BinExpr over rewritten IntLits folds (codegen const-folds two IntLits, `codegen.cpp:997-1023`) |
| `sema_valid_enum_global_init.ember` | `global const MASK : i32 = Flags::A \| Flags::C;` — bakes via `globals.hpp eval_global_initializers` (§4.6); assert at runtime the global holds the right value |
| `sema_valid_enum_array_size.ember` | `let buf: [i32; Color::Count]` where `Count` is the last variant — enum access as array size folds (§4.6) |
| `sema_valid_enum_shadow.ember` | two enums with the same variant name (`Color::Red`, `Light::Red`) coexist — pins the §1.1 namespacing decision |

### 7.3 Sema-invalid (must sema ERROR) — `sema_invalid_enum_*.ember`

| File | Covers |
|---|---|
| `sema_invalid_enum_dup_value.ember` | `enum E { A, B = 0 }` — duplicate value 0 (A=0, B=0) → error (§1.3 stricter-than-C rule) |
| `sema_invalid_enum_dup_name.ember` | `enum E { A, A }` — duplicate variant name → error |
| `sema_invalid_enum_unknown_variant.ember` | `E::Z` where `Z` not in `E` → error |
| `sema_invalid_enum_unknown_enum.ember` | `NoSuch::A` → error |
| `sema_invalid_enum_value_not_const.ember` | `enum E { A = some_global }` → "explicit value must be a compile-time integer constant" (§4.1 reuses `try_eval_const_i64`) |
| `sema_invalid_enum_value_out_of_i32.ember` | `enum E { A = 3000000000 }` → out of i32 range (§4.1) |
| `sema_invalid_enum_as_type.ember` | `let x: Color = 0;` → "enum 'Color' is not a type in v1; use i32" (§3.4/§4.5) |
| `sema_invalid_enum_dup_enum_name.ember` | two `enum Color {...}` decls → error |

### 7.4 Parse-invalid (must parse ERROR) — `invalid_enum_*.ember`

| File | Covers |
|---|---|
| `invalid_enum_no_name.ember` | `enum { A, B }` — missing name |
| `invalid_enum_no_brace.ember` | `enum E A, B }` — missing `{` |
| `invalid_enum_variant_no_name.ember` | `enum E { = 1 }` — variant missing name |
| `invalid_enum_annotation.ember` | `@entry enum E { A }` — annotations on enums rejected (§3.2) |

### 7.5 End-to-end (run via `ember_cli`, soft-fail if absent) — reuses §7.2 files

The `sema_valid_enum_switch.ember` and `sema_valid_enum_flags_expr.ember`
files should also be runnable end-to-end (a `@entry fn main() -> i64`
that returns a value derived from enum variants), confirming the §5
"codegen untouched" claim empirically: the JIT'd code produces the right
integer. This is the load-bearing test for the whole design — if codegen
needed a change, this is where it would surface.

---

## 8. Scope cut — v1-of-enums vs deferred

### v1 (this change)

- Script-side `enum E { ... }` at top level, alongside `fn`/`struct`/`global`/`link`.
- Auto-increment from 0 + optional explicit `= constexpr_int` (literal/literal-arithmetic only — what `try_eval_const_i64` already folds).
- `E::A` access via a new `EnumAccessExpr`, **rewritten by sema to an `IntLit`** carrying the resolved i32.
- Underlying type fixed at `i32`. Enum-typed value IS an i32; no `Type`/`Prim` change.
- Duplicate values within one enum → error (stricter than C).
- Enum name used in a type position → error (untyped v1).
- Use in: expressions, `switch` cases, `global const` initializers, fixed-array sizes, anywhere an i32 literal is valid — all via the single rewrite.
- Codegen, the const-folders, and `globals.hpp`'s initializer evaluator: **unchanged**.

### Deferred (explicit non-goals, listed so they don't get silently assumed)

- **Typed enums** (`let x: Color = Color::Red;` rejecting raw `i32`). Needs a `Type` shape + a per-enum compatibility rule + an `as`-cast hook. Tier 2+. Hook left at §4.5 (the type-position enum-name check flips from reject to accept).
- **`EnumBuilder`** (host→script enum registration). §6. Deferred; host uses `set_global` meanwhile.
- **`enum`-from-expr / `constexpr fn` in variant values** (`A = compute()`). ROADMAP line 53: depends on the `constexpr` function-eval subsystem. v1 explicit values are literal/literal-arithmetic only.
- **Variant referencing another variant** (`A = 1, B = A + 1`). Same dependency as above — `try_eval_const_i64` doesn't know about enum values, and §4.2's rewrite happens *during* the per-function check, after the enum-resolution pass in §4.1. Would need enum values available to the const-folder during the enum-resolution pass itself; defer.
- **Methods / associated functions on enums** (`E::A.something()`). Not in the language for any type yet (no methods on non-struct types); out of scope.
- **`enum` as a switch scrutinee with exhaustiveness checking** ("you didn't handle `Color::Blue`"). That's `match` (ROADMAP Tier 1's pattern-match bullet), not `switch`. v1 `switch` stays the existing int/bool scrutinee with optional `default`.
- **Repr / custom underlying type** (`enum E : u8`, `enum E : u64`). ROADMAP says i32; one underlying type in v1.

---

## 9. Doc edits (no source, but the spec docs must move with the code)

- `../spec/COMPILER_PIPELINE.md §1` token set: add `enum` to Keywords. §2 grammar: add `enum_decl`/`variant` productions; add `enum_decl` to `program`. §3 AST: add `EnumDecl`/`EnumVariant`/`EnumAccessExpr`. §4 sema passes: insert the §4.1 enum-resolution pass between pass 1 (struct layout) and pass 2 (function sig registration); note the §4.2 in-place rewrite. (Note: the spec's `program := (annotation* func_decl | struct_decl | global_decl)*` line and the "No `enum`... none exist in v1 grammar" line both need updating — they currently *deny* this feature by name.)
- `../spec/TYPE_SYSTEM.md`: add a §11.8 (or §12) "enum" subsection stating the §1.2/§1.3/§1.4 decisions — named i32 constants, auto-increment + optional explicit, untyped, underlying i32. Cross-link to the §11.6 `switch` semantics (enum case values are i32 literals).
- `../spec/BINDING_API.md §5`: un-defer the *note* but keep `EnumBuilder` deferred per §6 — update the text from "dropped from v1 — no `enum` keyword" to "script-side `enum` lands in Tier 1 (see `plan_ENUMS.md`); `EnumBuilder` remains deferred."
- `../ROADMAP.md` Tier 1: mark the `enum` bullet as "planned — see `plan_ENUMS.md`" (or, after implementation, "done").
- `../MODULES.md`: no change (enums are per-module like structs; a `link`ed module's enum is reachable as `alias::EnumName::Variant` via the existing `::` + `module_alias` machinery — but confirm during impl whether cross-module enum access needs the same deferred-trap handling `mod::fn` uses, `sema.hpp ModuleExport`. **Open question for impl**: does `mod::Enum::V` parse under the §3.5 lookahead split (base `Ident` `mod`, `::`, `Ident` `Enum`, then `::` again `V`)? The current `::` handler only does one level. Likely needs the `EnumAccessExpr` base to also accept an existing `EnumAccessExpr`/module-qualified base — verify, possibly defer cross-module enum access to v2-of-enums and error on it in v1.)

---

## 10. Open questions for the implementer (not blocking the plan)

1. **In-place rewrite mechanism (§4.2):** the cleanest is threading `ExprPtr&`
   through the child-recurse sites in `check_expr`'s callers (the `BinExpr`/
   `UnaryExpr`/`CallExpr`/`AssignExpr`/`IndexExpr`/`SwitchCase::value`/
   `ReturnStmt::value`/`LetStmt::init` cases). Survey how many call sites
   pass an `Expr&` vs could take `ExprPtr&` — the goal is the §4.2 rewrite
   being visible to `sema.cpp:1057`'s `dynamic_cast<IntLit*>` without a
   second pass. If the survey shows the threading is too invasive, the
   fallback is a tiny dedicated pre-switch-check lowering walk over
   `SwitchCase::value` only (the one place that dynamic_casts), but that's
   uglier than the general rewrite. Decide at impl.
2. **Cross-module enum access (`mod::Enum::V`, §9):** does it parse? Does it
   need the deferred-trap pattern? Likely defer to v2-of-enums with a clean
   error in v1; confirm the §3.5 lookahead doesn't accidentally accept it
   and produce a broken AST.
3. **Empty enum (`enum E {}`, §7.1):** allow or reject? Recommend allow for
   parity with empty struct; confirm no downstream breakage (an enum with no
   variants can't be used, so it's inert).
4. **`enum` in a `link`ed module's export surface:** does the
   `ModuleExportTable` (`sema.hpp`) need an enum-export entry, or are
   cross-module enums purely v2? (Ties to open question 2.)

---

## 11. Summary (one paragraph for the implementer)

Add `Kw_enum` + an `EnumDecl{name, variants[{name, explicit_value?, resolved}]}` 
to `Program`; parse `enum E { A, B = 5, C, }` at top level and `E::A` as a new 
`EnumAccessExpr` in `parse_postfix`'s existing `::` handler (one-token lookahead: 
`::` + `Ident` + not-`(` = enum access; `::` + `Ident` + `(` = the existing 
`mod::fn()` call, unchanged). In sema, a new pass resolves every variant's 
i32 value (auto-increment from 0, optional explicit `= constexpr_int`, 
duplicate-value/name → error, i32-range-checked) and builds an 
`(enum,variant)→i32` table; `check_expr`'s `EnumAccessExpr` case rewrites the 
node **in place to an `IntLit{v=resolved}` carrying `Prim::I32`**. From that 
point the existing const-folder, the `switch` case-value literal-check 
(`sema.cpp:1057`), codegen's `IntLit` eval (`codegen.cpp:904`), and 
`globals.hpp`'s initializer evaluator all treat the enum variant as an 
ordinary i32 literal — **codegen, the folders, and the global-init helper 
are untouched**. Typed enums and the host-side `EnumBuilder` are deferred 
to Tier 2; `enum` used as a type position is a clean error in v1 (the hook 
the typed upgrade later flips).
