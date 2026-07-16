# ember — Tier 2 first-class function references (implementation plan)

**DONE — expanded beyond the original Tier-2 plan.** First-class refs (`&fn`,
`handle(args)`, and `fn`) shipped in v1.0. Parameterized
`fn(Args)->Ret` types subsequently closed the bare-signature hole, and
cross-module handles now carry tagged module/slot identity with target-module
runtime validation. Lambdas reuse the guarded dispatch model with an env
pointer. Focused CTests: `function_refs`, `fn_types`, and
`cross_module_handles`. The text below is the historical implementation plan;
its “two open items” and “plan only” statements are superseded by this block.

**Status of body: historical plan; source is implemented.** This document is the design for the
fork the user resolved: **first-class call syntax `handle(args)`** — the first
indirect call through a *runtime* i64 value in ember. Every call today
(`CallExpr` in `src/codegen.cpp` ~1463/1564) is to a *compile-time-known* slot
(`c->script_slot`, stamped by sema from the host-provided `script_slots` map)
or a *compile-time-known* host native fn ptr (`c->native_fn`). This plan adds
a third kind: a call whose target is an i64 that the program produced at
runtime, validated against a registered-fn allowlist before dispatch.

**Why the guard is load-bearing.** The call-target-provenance invariant
("call-target provenance, forward guard") fires the instant this
feature lands: the i64-as-call-target vector is currently STOPPED at sema only
because there is *no* syntax to call an i64 and *no* `call rax`-by-value path.
Adding `handle(args)` opens exactly that path. If the JIT'd indirect call were
a raw `call [dispatch_base + handle*8]` with the handle taken on faith, a
script that learned or forged a slot index out of range could read past the
dispatch table into adjacent host memory, load an arbitrary 8-byte value as a
function pointer, and `call rax` — the exact "raw call rax of a script value"
the guard forbids. So the new codegen path **must** prove the i64 is a
registered slot before indexing the table with it, and **must** trap (via the
v0.4 `emit_trap` stub, `src/codegen.cpp` ~233) if it isn't. This is the single
most important part of the plan and the part that is genuinely new.

---

## 1. Surface-syntax decision: `&fn` to take, `handle(args)` to call

### The choice

Two ways to obtain a handle were on the table, both names-checked by
`../ROADMAP.md` Tier 2 and `GAP_ANALYSIS.md` §6:

| Form | Example | Status in ember today |
|---|---|---|
| `cast(fn)` | `let h = cast(fib)` | **Does not exist.** `../ROADMAP.md` Tier 2 and `GAP_ANALYSIS.md` §6 use this *spelling*, but it is aspirational text — `GAP_ANALYSIS.md` §2 row "Function references / `cast(fn)`" marks it ✗ v1 (deliberate), and ember **diverged** from the surveyed language's `cast<T>()` to `expr as T` (`GAP_ANALYSIS.md` §2: "ember uses `expr as T` (C/Rust style)"). There is no `Kw_cast` token (`src/lexer.hpp` enum `Tk` has no `Kw_cast`); `src/parser.cpp` ~322 `parse_cast` only handles `as`. So "cast(fn)" would require either (a) adding a `cast` keyword that contradicts the established `as` divergence, or (b) treating `cast` as an ordinary native name — which is exactly what the dynamic-registration host would do anyway, but then it's not first-class syntax, it's a native call, and you cannot *type* a `fn` parameter that way. |
| `&fn` | `let h = &fib` | **New, but cheap.** `&` already lexes (`Tk::Amp`, `src/lexer.cpp` ~344). A prefix `&` on an `Ident` that resolves to a script function is unambiguous: ember has no reference types today (`GAP_ANALYSIS.md` §2 row "Reference `&`/`out` params ✗ v1"), so prefix `&` is currently used only... nowhere as a prefix operator. `parse_unary` (`src/parser.cpp` ~336) handles `Minus`/`Not`/`Tilde` only. Adding an `&` prefix case is one branch. |

**Decision: `&fn_name` to take a handle, `handle(args)` to call it.**

### Rationale

1. **`&` is the established C/Rust/Golang address-of operator.** ember's
   syntax is C-family (`../spec/COMPILER_PIPELINE.md` §2 grammar, C precedence).
   `&fib` reads to a C/Rust programmer as "a reference to fib." The handle is
   literally the slot index — a small integer that *stands for* the function
   the way a C function pointer stands for the function. `&` is the honest
   spelling.

2. **`cast(fn)` would be a contradiction.** The whole `GAP_ANALYSIS.md` §2 row
   for `cast<T>()` says ember deliberately *diverged* to `as`. Introducing
   `cast(fn)` now would either (a) reverse that divergence for one special
   case with no rationale, or (b) force `cast` to be a magic name in the
   parser that isn't a real native — a special case worse than `&`. The
   ROADMAP's `cast(fn)` wording is a leftover from the surveyed-language
   comparison; the implementation should follow ember's own divergence, not
   the surveyed language's spelling.

3. **`&fn` is a pure compile-time operation, so the operator's prefix position
   is correct.** The handle is produced at the point of `&fib` (sema resolves
   `fib` to its slot, emits an i64 literal = slot). It is *not* a runtime
   dereference. Putting it at prefix-unary level matches that: it's a
   compile-time reification, like `sizeof` (`src/parser.cpp` ~593) which is
   also prefix-only.

4. **`handle(args)` is the user's chosen fork (Tier 2 first-class form, option
   B).** The alternative would have been a host-native `call_fn(handle, args)`
   — but that pushes the indirect call into a host C function, hiding the
   dispatch behind a native boundary, and it would *not* be "ember's first
   indirect call through a runtime i64" (the task's framing). The
   `handle(args)` form keeps the call in the JIT, where the guard lives.

### What this leaves out (deliberate)

- **No `fn` type literal in `parse_type`.** A handle is just an `i64` with a
  provenance tag (see §2). You do not write `let h: fn = &fib;` — you write
  `let h = &fib;` and sema types `h` as `i64` (or `fn` as a *display alias*
  for i64, see §2). If we want a `fn` keyword for *parameter declarations*
  (`register_routine(fn, data)`), it is a type-alias spelling, not a new
  type representation. This keeps `Type` (`src/ast.hpp` ~14) unchanged at the
  representation level.
- **No `cast<fn>`/`as fn`.** `as` (`src/parser.cpp` ~322) casts a *value*; a
  function name is not a value you cast, it's a name you take a handle of. The
  two operations are different and should look different.

---

## 2. The `fn` type representation — reuse i64, add a provenance flag, no new Prim

### Why not a new `Prim::Fn`

`Type` (`src/ast.hpp` ~16-24) is `Prim prim; string struct_name; bool
is_slice; uint32_t array_len; shared_ptr<Type> elem;`. Adding `Prim::Fn`
would touch: `Type::is_int()` / `byte_size()` / `align()` / `same()` /
`to_string()` in `src/types.cpp`; the parser's `parse_type` switch
(`src/parser.cpp` ~52); the codegen value-width logic
(`words_for_type`, `local_width_bytes`); the native-binding ABI (a `fn`
param would need a different Win64 marshalling — but it's just an i64 in a
GP register, so this would be a special case for no gain); and the `.em`
serialization name-table (`src/em_file.hpp`) which stores `Prim` values.
That is a wide blast radius for a feature whose *value* is provably just an
i64 = slot index.

### The representation

A function handle is `Prim::I64` with a new optional tag on `Type`:

```cpp
// src/ast.hpp, struct Type — ADD one field:
bool is_fn_handle = false;   // true iff this i64 is a function handle
                             // (provenance tag for the call-target guard; see §5)
```

Semantics:

- `is_fn_handle = true` ⇒ `prim == Prim::I64` (invariant, enforced by sema;
  setting it on a non-i64 is a sema internal error).
- `byte_size() == 8`, `align() == 8`, lives in a GP register, marshals as a
  Win64 i64 — **identical to a plain i64 in every ABI respect.** The tag is
  metadata, not a representation change. Codegen's `words_for_type` /
  `local_width_bytes` / the call-arg stash in `src/codegen.cpp` ~1466-1540
  all see a normal i64 and behave identically.
- `same()`: a `fn` handle type is `same` to another `fn` handle type iff
  their *signatures* match (see §4); a `fn` handle is **not** `same` to a plain
  i64 (this is what makes `handle(args)` typecheck against the recorded
  signature, and what makes assigning a forged i64 to a `fn`-typed variable a
  compile error — closing V3-style forging at the type level where possible).
- `to_string()`: prints `fn` (display alias) so diagnostics read
  `let h: fn = ...`, not `let h: i64 = ...`.

### The `fn` keyword in `parse_type`

Add `Kw_fn` handling to `parse_type` (`src/parser.cpp` ~52-63):

```cpp
case Tk::Kw_fn: {
    prim(Prim::I64);
    t->is_fn_handle = true;
    // signature is NOT parsed here — a bare `fn` is "function of unknown
    // signature" (the dynamic-registration case: register_routine(fn, data)
    // takes any fn). A parameterized `fn(i64)->i64` form is a v2+ refinement
    // (see §9); for Tier 2 the bare `fn` suffices because the call-target
    // guard validates against the *actual* registered fn's signature at the
    // call site, not at the variable's declared type.
    break;
}
```

`Kw_fn` already exists (`src/lexer.hpp` ~16) — it lexes `fn` today for
`fn name(...)` declarations (`src/parser.cpp` ~135). It is currently only
consumed in declaration position; reusing it in `parse_type` is unambiguous
because `parse_type` is only called where a type is expected (param/ret/let
annotations, `as`/`sizeof` operands), never where a `fn` *declaration* could
start.

### Display alias `fn` vs. i64-with-tag

A variable typed `fn` and a variable typed `i64` holding a handle are the
same representation. The difference is purely whether the tag is set, which
controls (a) whether `handle(args)` is legal on it (§4), and (b) diagnostics.
This mirrors how `string` is already `Prim::I64` with `struct_name ==
"string"` (`src/binding_builder.hpp` `bind_handle`, `src/sema.cpp` ~383's
`StringLit` `implicit_to_string` path) — an i64 with a tag, not a distinct
`Prim`. The `is_fn_handle` flag is the `fn` analogue of that pattern.

---

## 3. Parser changes

### 3.1 Prefix `&` to take a handle — `src/parser.cpp` `parse_unary` (~336)

Add a prefix `&` case in `parse_unary`, *before* the postfix fallback:

```cpp
if (k == Tk::Amp) {
    adv();
    ExprPtr e = parse_unary();   // the operand must be an Ident resolving
                                 // to a script function (sema checks §4);
                                 // we allow parse_unary so `&&fib`-style
                                 // nesting is structurally parseable then
                                 // rejected by sema, not by a parser crash.
    auto h = std::make_unique<FnHandleExpr>();
    h->loc = e->loc;
    h->operand = std::move(e);
    return h;
}
```

This needs a new AST node:

```cpp
// src/ast.hpp — ADD:
struct FnHandleExpr : Expr { ExprPtr operand; int slot = -1; };
```

(`slot` is filled by sema — §4.2.) Why a new node and not a `UnaryExpr`
variant: `UnaryExpr::Op` is `{Neg, Not, BitNot}` and its codegen case
(`src/codegen.cpp`) is purely arithmetic; a fn-handle take is a *compile-time*
slot lookup with zero runtime arithmetic, and it needs its own sema path
(resolve the name to a slot, check it's a script fn not a native or a
variable). A dedicated node keeps that path clean and keeps `UnaryExpr`
honest about "runtime unary op."

### 3.2 First-class call target — `src/parser.cpp` `parse_postfix` (~361-418)

Today, the `LParen` case in `parse_postfix` only accepts three call-target
shapes (`src/parser.cpp` ~385-397):
- `name(args)` — base is an `Ident`
- `obj.method(args)` — base is a `FieldExpr`
- `mod::fn(args)` — base is a half-built `CallExpr` with `module_alias` set

Everything else throws `"call target must be a function name or obj.method"`
(`src/parser.cpp` ~398). **This is the line that V2 currently turns into a
sema error and that we must lift.**

Change: accept *any* expression as a call target when none of the three
special cases match. The fallback builds a `CallExpr` with the target
expression carried alongside `name`:

```cpp
} else {
    // First-class call: <expr>(args). The target is a runtime i64 handle.
    // Sema types the target as a fn handle and stamps an indirect-call flag;
    // codegen validates the handle against the registered-fn allowlist then
    // emits `call [dispatch_base + handle*8]` (§5).
    c = std::make_unique<CallExpr>();
    c->loc = e->loc;
    c->name.clear();                  // empty name = not a named call
    c->indirect_target = std::move(e); // new field on CallExpr (§3.3)
}
```

### 3.3 `CallExpr` AST field — `src/ast.hpp` (~CallExpr)

Add one field to `CallExpr`:

```cpp
// First-class call (Tier 2): if non-null, this is `indirect_target(args)` —
// a call through a RUNTIME i64 handle, not a compile-time-known name. Sema
// types indirect_target as a fn handle, records its signature for the
// call-site arg check, and sets `is_indirect = true`. Codegen validates the
// handle against the registered-fn allowlist (the call-target-provenance guard) before
// dispatching (§5). Mutually exclusive with is_native/script_slot/module_alias
// being set (sema asserts this).
ExprPtr indirect_target;
bool is_indirect = false;
```

`name` is empty (`c->name.clear()`) when `is_indirect`; the existing sema
resolution (`src/sema.cpp` ~491 `natives->find(c->name)`) must skip the
`name.empty()` case and route to the indirect path.

### 3.4 Grammar summary after changes

```
unary   := '-' unary | '!' unary | '~' unary | '&' unary | ('++'|'--') unary | postfix
postfix := primary ( '::' ident | '(' args? ')' | '[' ... ']' | '.' ident | '++'|'--' )*
primary := lit | ident | '(' expr ')' | sizeof '(' type ')'
type    := prim | ident | 'fn'   (type suffixes unchanged)
```

The new productions are `& unary` (prefix handle-of) and the relaxed
`postfix` call target (any expression before `(`).

---

## 4. Sema changes

### 4.1 New allowlist: the registered-fn table — `src/sema.hpp` + `src/sema.cpp`

The guard needs, at codegen time, the set of slot indices that are *registered
script functions* and, for each, its signature (for the call-site arg check).
This already exists in two places that sema currently reads separately:

- `script_slots` (host-provided `name -> slot` map, passed to `sema()` as
  `const std::unordered_map<std::string,int>&`, `src/sema.hpp` ~111) — gives
  the slot numbers.
- `Program::funcs` (`src/ast.hpp` ~Program) — gives each fn's `params`/`ret`
  and its `slot` (stamped by the host before sema, `examples/ember_cli.cpp`
  ~208: `fn.slot = slots[fn.name]`).

**Build a derived table at sema entry** and carry it on the `Checker`:

```cpp
// src/sema.hpp — ADD to the sema() signature (or a new param):
struct RegisteredFn {
    int slot = -1;
    Type ret;
    std::vector<Type> params;
    bool is_native = false;   // false = script fn (intra-module); the
                              //   allowlist only contains script fns for
                              //   Tier 2 (native fn handles are a v2+ question,
                              //   see §9). true reserved.
};
using RegisteredFnTable = std::unordered_map<int, RegisteredFn>;  // slot -> sig
```

Populate it from `Program::funcs` (slot -> {ret, params}) at the top of
`sema()`. This is the **runtime allowlist** the codegen guard checks against
(§5): the set of integers that are legal call-target handles. A handle not in
this table is, by construction, not a registered script fn of this module —
and the guard traps.

**Why a slot-keyed table and not a name-keyed one:** the handle is a slot
index, and the guard validates an i64 against the table at runtime. Looking
up by slot is O(1) in a `unordered_map<int, RegisteredFn>` or, better, O(1)
with a direct `vector<bool>` indexed by slot (§5.2). Name-keyed lookup would
require the JIT'd code to carry a string, which it cannot.

### 4.2 `FnHandleExpr` resolution — `src/sema.cpp` `check_expr`

New case in `check_expr` (the `dynamic_cast<Expr*>(&e)` ladder in
`src/sema.cpp` ~97+):

```cpp
if (auto* h = dynamic_cast<FnHandleExpr*>(&e)) {
    // The operand must be an Ident naming a script function.
    auto* id = dynamic_cast<Ident*>(h->operand.get());
    if (!id) {
        err("'&' may only be applied to a function name, not an expression",
            h->loc.line, h->loc.col);
        e.ty = intern(type_void()); return e.ty;
    }
    auto sit = script_slots->find(id->name);
    if (sit == script_slots->end()) {
        err("'" + id->name + "' is not a script function in this module "
            "(cannot take a handle of a native or an unknown name)",
            h->loc.line, h->loc.col);
        e.ty = intern(type_void()); return e.ty;
    }
    // The handle IS the slot index, as an i64 with the fn-handle tag.
    // We stash the slot on the AST node so codegen emits it as a literal
    // (a compile-time-known i64 — this is NOT a runtime value, despite the
    // first-class call being one). The first-class-ness is at the CALL site,
    // not at the handle-of site.
    h->slot = sit->second;
    Type t = type_i64();
    t.is_fn_handle = true;
    // Carry the signature too, so a fn-typed variable keeps its sig for the
    // later handle(args) check (§4.4):
    //   (populate t.recorded_params/recorded_ret/has_recorded_sig from the
    //    matching FuncDecl in Program::funcs — find by name, copy params/ret)
    e.ty = intern(t);
    return e.ty;
}
```

The handle is a **compile-time constant** (the slot is known at the `&`
site). This is the key honesty: `&fib` is not a runtime computation, it's
sema baking the slot into an i64 literal. The runtime-ness is only at
`handle(args)` — the *target* is runtime (the handle may have flown through a
variable, an array, a host native's return value), but the *value* is always
a slot index that sema can prove came from a `&` of a registered fn.

### 4.3 `CallExpr` with `indirect_target` — `src/sema.cpp` (~403+)

Add the indirect path at the *top* of the `CallExpr` case, before the
`__fstring_to_string` and cross-module and named-resolution branches:

```cpp
if (c->indirect_target) {
    // First-class call: target(args). Type the target; it must be a fn handle.
    const Type* tt = check_expr(*c->indirect_target);
    if (!tt || !tt->is_fn_handle) {
        err("call target must be a function handle (fn), got '" +
            (tt ? tt->to_string() : std::string("?")) + "'",
            c->loc.line, c->loc.col);
        e.ty = intern(type_void()); return e.ty;
    }
    c->is_indirect = true;
    // Signature for the arg check: if the target's type carries a recorded
    // signature (from a &fn that kept it), check args against it; if the
    // target is a bare `fn` (unknown sig, e.g. a register_routine param),
    // skip the arg check — the runtime guard (§5) validates the handle is a
    // registered slot, and the call uses the target fn's OWN signature at
    // dispatch (the dispatch table entry is the fn's entry; the Win64 ABI
    // args are whatever the caller passes). See §9 for the unsoundness this
    // opens and why it's acceptable for Tier 2.
    if (tt->has_recorded_sig) {
        size_t nargs = c->args.size() + (c->receiver ? 1 : 0);
        if (nargs != tt->recorded_params.size()) { err(/*arity*/); }
        for (size_t i = 0; i < c->args.size(); ++i)
            check_expr(*c->args[i], &tt->recorded_params[i]);  // with hint
        e.ty = intern(tt->recorded_ret);
    } else {
        for (auto& a : c->args) check_expr(*a);   // no hint; type each arg
        e.ty = intern(type_i64());                 // default ret for bare fn
    }
    return e.ty;
}
```

### 4.4 Carrying the signature on a `fn`-typed variable

For `let h = &fib; h(5);` to type-check `5` against `fib`'s `(i64)` param,
the `fn` type on `h` must carry `fib`'s signature. Extend `Type`:

```cpp
// src/ast.hpp, struct Type — ADD:
std::vector<std::shared_ptr<Type>> recorded_params;  // empty unless is_fn_handle
std::shared_ptr<Type> recorded_ret;                  // null unless is_fn_handle
bool has_recorded_sig = false;
```

`FnHandleExpr`'s sema path (§4.2) populates these from `Program::funcs`'s
matching `FuncDecl::params`/`ret`. A `fn`-typed *parameter*
(`register_routine(fn h, i64 data)`) has `has_recorded_sig = false` (the
param accepts any fn); a `fn`-typed *local* initialized from `&fib` has
`has_recorded_sig = true` (it knows it's `fib`).

`Type::same` (`src/types.cpp`): two `fn` handle types are `same` iff both
`is_fn_handle` and either (a) both `has_recorded_sig` and the recorded
sigs match elementwise, or (b) neither has a recorded sig (bare `fn`).
A recorded-sig `fn` and a bare `fn` are `same` (a bare `fn` param accepts a
specific-fn arg) — this is the one direction of subtyping we allow, and it's
safe because the *call site* still validates the handle (§5).

### 4.5 The allowlist is closed under `&` — defense-in-depth rule at the type level

Every `&fn_name` that sema accepts is a script fn in `script_slots`, so its
slot is in the `RegisteredFnTable` (§4.1). A handle therefore always
*originated* from a registered slot. The guard's job (§5) is to validate the
handle *still* is one at the call site — i.e. that nothing between the `&` and
the `handle(args)` corrupted or replaced the i64 with a non-slot value. This
is the gap the type tag alone cannot close (a `fn`-typed variable could be
assigned a forged i64 if the type system let an i64-to-fn assignment through),
so the runtime guard is mandatory regardless of type-tagging.

**Sema rule that tightens the type path (defense-in-depth, does NOT replace
the runtime guard):** an assignment / `let` init from a plain `i64` to a
`fn`-typed variable is a *compile error* (closes the type-forging vector at the type
level, the same way `let s: u8[] = forged_ptr;` is already a sema error).
Only `&fn` and a `fn`-typed source produce a
`fn`-typed value. A host native may *return* a `fn`-typed i64
(`register
_routine` returns the handle it stored; §6) — that's the one
runtime source, and it's trusted because the native is host-trusted
(`../spec/SAFETY_AND_SANDBOX.md` §1: natives are trusted by design). This still does
not relieve the runtime guard: a host native is trusted, but the *script*
could pass a forged i64 to a `fn`-typed variable via a yet-unregistered
native — the guard is the last line, not the first.

---

## 5. Codegen changes — the indirect call + the provenance guard (the hard part)

### 5.1 The new emission path — `src/codegen.cpp` `CallExpr` case (~1463-1576)

Today the final dispatch branch (`src/codegen.cpp` ~1567-1575) is:

```
emit_depth_check();
e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);  // kind-0
e.call_mem(Reg::r11, int32_t(c->script_slot) * 8);  // compile-time-known slot
emit_depth_leave();
```

Add a fourth branch, when `c->is_indirect`. The arg stash (sub rsp + stash +
place-into-regs, ~1462-1560) is **identical** to a named call — a fn handle is
an i64 in a GP register, marshalled like any i64 — except the handle is the
*target*, not an arg, so the target is evaluated separately. The sequence
skeleton (the precise register juggling and frame-accounting caveats are in
§9.1/§9.7 — this skeleton is the shape, not a verified-correct byte sequence):

1. Evaluate the target into rax: `eval(*c->indirect_target)`.
   - A `&fn` is a compile-time slot literal -> `mov rax, imm64(slot)`.
   - A `fn`-typed variable -> load from frame.
   - A native-returned handle -> rax already holds it from the native call.
2. **Guard before the arg stash** (see §9.1 ordering hazard): validate rax
   is a registered slot (§5.2). Trap if not. This must run before the arg
   stash places args into rcx/rdx/r8/r9 so the guard's scratch (rcx, r11) is
   free and the args are not clobbered.
3. Reserve the handle: the guard consumes rax; the dispatch needs the handle
   again as the lea index. Stash it in a callee-saved register (rbx, already
   saved by the prologue) OR — cleaner per §9.7 — into a dedicated word in the
   arg-stash region (bump `total` by 8, write the handle to `[rsp +
   stash_size]` before the arg loop, reload into rax after args are placed).
4. The existing arg stash: `sub rsp, total`; eval each arg into its word
   slot; place words 0..3 into rcx/rdx/r8/r9; words 4+ to outgoing stack.
5. `mov rax, rbx` (or reload from the stash slot) — rax = handle.
6. `emit_depth_check()` (the indirect call is a script-to-script call; the
   depth budget applies, `src/codegen.cpp` ~296).
7. Dispatch: `mov r11, dispatch_base` (kind-0 reloc); `lea r11,[r11+rax*8]`;
   `mov r11,[r11]`; `call r11` (exact bytes §5.3).
8. `emit_depth_leave()`.
9. `add rsp, total` — the existing arg-stash cleanup.

The precise register choice and the rbx-push-vs-stash-slot decision is the
highest-risk implementation detail; §9.1 and §9.7 cover it. The plan's job
here is to fix the *order* (guard before stash) and the *encoding* (lea via a
new helper), both flagged as must-verify at implementation time.

### 5.2 The provenance guard — `emit_call_target_guard()` (new codegen helper)

This is the load-bearing new part. The guard validates that the i64 in `rax`
is a registered slot before it is used as a dispatch-table index. Two
representations considered:

#### 5.2.1 Option A — a runtime bitset over slots (chosen)

The allowlist is a host-allocated byte array of length
`ceil(slot_count / 8)`, one bit per slot, bit set iff that slot is a
registered script fn of this module. Stored on `CodeGenCtx` as
`int64_t fn_allowlist_base` and `int64_t fn_slot_count` (the count is the
range bound; the bitset is the membership test).

The guard, emitted inline before the dispatch (after the handle is in rax):

```
// rax = handle (i64). Validate: 0 <= handle < slot_count AND bit set.
// 1. Range: cmp rax, slot_count; jae trap. (unsigned — catches negatives too,
//    same trick as emit_bounds_check_imm, src/codegen.cpp ~217.)
// 2. Bit test: mov r11, allowlist_base; compute byte = handle>>3, bit = handle&7;
//    bt [r11 + byte], bit; jnc trap.
// 3. Fall through to dispatch.
//
// bt (0F AB /5 for bt r/m, r) tests bit (reg & 63) of the r/m64. We want bit
// (handle & 7) of byte (handle >> 3). So we address the byte with the byte
// offset and put the bit index in a register:
//   mov r11, allowlist_base          ; imm64 (raw, not a reloc — §9.2)
//   mov rcx, rax                     ; copy handle (preserve rax for the dispatch)
//   shr rcx, 3                       ; byte offset
//   add r11, rcx                     ; r11 = &allowlist[handle>>3]
//   mov rcx, rax                     ; bit index
//   and rcx, 7                       ; rcx = handle & 7
//   bt [r11], rcx                    ; 0F AB /r: bt r/m64, r64; CF = bit
//   jnc .trap                        ; bit clear -> not a registered fn
//   ; fall through: rax still = handle
// .trap:
//   emit_trap(int(TrapReason::BadCallTarget),
//             "call-target provenance: handle is not a registered function");
//   ; (emit_trap longjmps; bind .trap before it so the fall-through path
//   ;  reaches the dispatch when a stub is set, matching emit_cpuid_gate's
//   ;  pattern, src/codegen.cpp ~686-696)
```

Range check first: `cmp rax, slot_count; jae .trap` (unsigned compare — a
negative handle reinterprets as a huge unsigned and fails the range check, so
no separate `handle < 0` test is needed, the same one-shot trick
`emit_bounds_check_imm` uses, `src/codegen.cpp` ~217). This also bounds the
`shr rcx, 3` / `add r11, rcx` addressing: the byte offset is at most
`slot_count/8`, well within the allowlist allocation.

The bit-test branch labels use the same `Label alloc_label` / `bind` / `jcc`
machinery as the bounds check (`src/codegen.cpp` ~200-220).

**`TrapReason` extension:** add `BadCallTarget` to `enum class TrapReason`
(`src/context.hpp` ~35) and to `trap_reason_str` (~82). The guard's trap
detail string is a rodata literal (the same mechanism `emit_bounds_check`
uses for its "bounds check: index out of range" string).

**The allowlist base address:** a host pointer, baked as an imm64 via raw
`mov_reg_imm64(Reg::r11, int64_t(ctx.fn_allowlist_base))` (the same form
native fn ptrs use, `src/codegen.cpp` ~649). It is *not* relocatable: the
allowlist is per-context, allocated once when the module is compiled, and is
stable for the module's lifetime (the same lifetime as the dispatch table,
which the `DispatchTableBase` reloc handles). If hot-reload ever needs to
rebuild it, add an `AbsFixup::Kind::FnAllowlistBase` — but Tier 2 does not
need that; see §9.2.

#### 5.2.2 Option B — a host-provided validation native (rejected)

Alternative: call a host native `__validate_fn_handle(handle) -> bool` and
trap if false. This is the `__str_decrypt` pattern (`src/codegen.cpp` ~1612).
Rejected because:

- It turns a per-call-site guard into a host call (Win64 shadow space,
  register clobbering, a real function call frame) on *every* indirect call.
  The bitset version is ~8 inline instructions with one branch.
- It reintroduces the "host native called from JIT'd code" path at a point
  where the JIT'd code is about to make an untrusted indirect call — the
  native's return value would be trusted to decide whether to trap, adding a
  trust hop where the bitset has none.
- It is exactly what the call-target-provenance invariant explicitly
  does *not* want: "dispatch must validate an i64 used as a call target
  against a registered-fn allowlist; never raw call rax of a script value."
  A bitset the JIT reads directly *is* the allowlist; a native is an
  indirection away from it.

The bitset (Option A) is the honest implementation of the guard.

#### 5.2.3 Why a bitset and not a hash table / sorted vector

The slot space is dense `[0, slot_count)` (assigned by the host as
`slots = si++`, `examples/ember_cli.cpp` ~208). A bitset is O(1) space per
slot (1 bit), O(1) validation (range check + bt), and the dense property
makes the range check `handle < slot_count` sufficient. A hash table would be
needed only if handles were sparse or large (they're not). The bitset is also
what a defender auditing the JIT reads most easily: "the call site checks a
bit in a host bitmap before indexing the dispatch table" is a one-line
invariant.

### 5.3 The indirect call encoding — exact bytes (must be re-derived at implementation time)

`call_mem(base, disp)` (`src/x64_emitter.hpp` ~call_mem) emits `FF /2` with
`mod=10` (disp32) — a 4-byte displacement. For a zero displacement after the
`lea r11,[r11+rax*8]`, we want `call [r11]` with `mod=00` (no disp), which
`call_mem` does not currently emit. Two options:

1. **Add a zero-disp `call_mem` variant** (`call_mem_zero_disp(base)`) to
   `src/x64_emitter.hpp` emitting `FF /2` mod=00. Cleanest.
2. **`lea` into a scratch, `load` the entry, `call_reg`:** after the guard,
   `mov r11, dispatch_base; lea r11,[r11+rax*8]; mov r11,[r11]; call r11`.
   This reuses `load_reg_mem` (which handles the SIB/rsp case) and
   `call_reg`. Two extra instructions (the load) but no new emitter primitive.

**Chosen: option 2** — it reuses existing emitter primitives
(`load_reg_mem`, `call_reg`, both present in `src/x64_emitter.hpp`) and adds
no new encoding path to audit. The cost is one extra `mov r11,[r11]` per
indirect call, negligible (indirect calls are not the hot path; the
named-call path is, and it's unchanged).

The full final sequence after the guard (the lea SIB/REX bytes here are a
*sketch to be verified* — see §9.1, the highest-risk item):

```
e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);  // kind-0
// lea r11, [r11 + rax*8]:  REX.W + 8D /r + SIB(scale=8,index=rax,base=r11)
//   SIB = (scale=3<<6)|(index=rax=0<<3)|(base=r11=3) = 0xC3
//   ModRM = (mod=00<<6)|(reg=r11<<3)|(rm=SIB=4) -- but r11 as dest needs REX.R
//   REX = 0100 W R X B; W=1, R=1(r11 dest ext), X=0(rax not ext), B=1(r11 base ext) = 0x4D
//   => 4D 8D 1C C3   (leaked; VERIFY — §9.1)
e.byte(0x4D); e.byte(0x8D); e.byte(0x1C); e.byte(0xC3);
// mov r11, [r11]:  load_reg_mem(r11, r11, 0)  -> 49 8B 1B
e.load_reg_mem(Reg::r11, Reg::r11, 0);
// call r11:  call_reg(r11)  -> 41 FF D3
e.call_reg(Reg::r11);
```

The `lea` SIB byte (0xC3) encodes scale=8 (scale field=3), index=rax (index
field=0), base=r11 (base field=3, with REX.B=1 for r11). This MUST be
verified against `src/x64_emitter.hpp`'s rex/modrm conventions; §9.1 lists it
as the top risk. **The mitigation is to add a `lea_reg_mem_sib` helper to
`x64_emitter.hpp` with the rex/modrm/SIB logic centralized and unit-tested in
isolation** (like `load_reg_mem` already handles the rsp/SIB case), rather
than hand-emitting bytes in codegen.

---

## 6. Dynamic registration — the `register_routine` host native

`../LIFECYCLE.md` §2: "Dynamic registration (script decides at runtime what
to hook, unregister later) is not v1 — it needs function references
(`../ROADMAP.md` Tier 2). This is it."

### 6.1 The native's signature

`register_routine` is a **regular host native** registered via
`BindingBuilder::add` (`src/binding_builder.hpp` ~add):

```cpp
b.add("register_routine", type_i64(),
      { /* fn handle */ Type{Prim::I64, .is_fn_handle=true}, type_i64() /* data */ },
      &host_register_routine);
```

Its Win64 signature (host C++): `int64_t host_register_routine(int64_t handle, int64_t data)`.

It receives the handle (an i64 = slot index, produced by `&fn` in the script)
and an opaque `data` value. It stores `(handle, data)` in a host-side table
keyed by whatever the host wants (a routine id, a name, an event key). It
returns an opaque routine id (an i64) the script can use to unregister later
(a companion `unregister_routine(id)` native).

### 6.2 What the host does with the handle

The host stores the handle (slot index). To *call* the routine per frame,
the host does **exactly what `ember_cli` does today**
(`examples/ember_cli.cpp` ~408): `void* entry = table.get(slot);
reinterpret_cast<F>(entry)(args...)`. The handle is the slot; the host looks
up the entry via the dispatch table and calls it. **No new host-side call
path is needed** — the dynamic-registration native reuses the existing
dispatch-table-by-slot lookup that the static-annotation path already uses
(`examples/game_host.cpp` ~106: `m.table->get(m.slots[fn])`).

This is the elegance of the slot-as-handle design: the dynamic-registration
case is *the same call mechanism* as the static case, just discovered by the
script passing `&fn` instead of the host querying `@on_tick`. The guard
(§5.2) is what makes the script-supplied slot safe to use this way.

### 6.3 The host calling a registered routine — does it need the guard?

The guard (§5.2) is in the **JIT'd code path** (script → script via
`handle(args)`). When the *host* calls a routine it stored from
`register_routine`, the host does `table.get(slot)` directly in C++ — the host
is trusted (`../spec/SAFETY_AND_SANDBOX.md` §1) and the slot came from a `&fn` that
sema validated, so the host's `table.get(slot)` is safe by the same
reasoning that `table.get(entry_slot)` is safe today. The guard is not
duplicated on the host side. (If a host wanted to call a handle whose
provenance it did not control, it would check `slot < table.slots.size()` and
`table.get(slot) != nullptr` — a one-line check the host can do; not a JIT
concern.)

### 6.4 Recursion via handle — `fib` calling itself through a handle

`let f = &fib; return f(n - 1) + f(n - 2);` — this is a script-to-script call
through a handle. The guard runs on each `f(...)` call (validates the handle
= `fib`'s slot, which is in the allowlist), then dispatches. The depth check
(`emit_depth_check`, `src/codegen.cpp` ~296) wraps it as for any script call,
so recursion via handle is bounded by `max_call_depth` exactly like direct
recursion. No new recursion hazard.

---

## 7. Call-target-provenance guard compliance — how the guard prevents raw-call-rax-of-script-value

The guard's job, stated as the call-target-provenance invariant: "when first-class function references are ever added,
dispatch must validate an i64 used as a call target against a registered-fn
allowlist; never raw call rax of a script value."

### How the guard satisfies it, concretely

The threat model (the i64-as-call-target vector): a script produces an i64, treats
it as a call target, and the JIT'd code `call rax` (or `call [dispatch + rax*8]`)
with no validation. The i64 could be:

1. **Out of range** (`handle >= slot_count`): the `cmp rax, slot_count; jae
   trap` catches it. Without the guard, `call_mem(r11, handle*8)` with a huge
   `handle` reads `[dispatch_base + huge*8]` — past the dispatch table, into
   adjacent host memory, loading an arbitrary 8-byte value as a function
   pointer. The range check closes this.

2. **In range but not a registered fn** (a slot index that happens to be in
   `[0, slot_count)` but was never `&`'d — e.g. a future slot reserved for a
   not-yet-compiled fn, or a slot the script computed by arithmetic on a
   valid handle): the `bt [allowlist + handle>>3], handle&7; jnc trap` catches
   it. Only slots whose bits are set in the allowlist (set by sema from
   `script_slots` at compile time) pass. Without the guard, an in-range
   unregistered slot would read `dispatch_table[handle]`, which is `nullptr`
   for an unregistered slot (`DispatchTable` ctor stores nullptr,
   `src/dispatch_table.hpp` ~DispatchTable) — and `call [nullptr]` is a
   null-deref crash, *worse* for the sandbox than a trap (it's the V6-DoS
   "process crash" class, not the V2 "controlled trap" class). The guard turns
   it into a graceful `BadCallTarget` trap via `emit_trap` (longjmp to the
   `ember_call` checkpoint, `src/context.hpp` ~checkpoint).

3. **A forged i64 that the type system should have rejected** (assigned to a
   `fn`-typed variable through a path sema didn't catch — §4.5's defense-in-
   depth rule closes the *type* path, but the guard is the *runtime* backstop):
   the guard validates regardless of how the i64 got into `rax`. This is the
   "never raw call rax of a script value" guarantee: even if a future native
   or a future type-system hole lets a forged i64 reach a `fn`-typed variable,
   the call site still checks it against the allowlist before dispatching.

### What the guard does NOT prevent (and why that's correct)

- **Calling the *wrong* registered fn** (a handle that is a valid slot but
  for a different fn than the caller intended, because the handle was
  corrupted to a *different valid* slot): the guard permits this — both
  slots' bits are set. This is "calling a valid but wrong function," not
  "raw call rax of a script value." It is the same threat level as a
  use-after-free in C: you call real, valid code, just not the code you
  meant. The guard's scope is *provenance* (is this a registered fn at all?),
  not *intent* (is this the registered fn the caller meant?). Intent is
  unsolvable at the JIT level without a per-call-site capability model,
  which is out of scope for Tier 2 (§9). The sandbox property preserved: the
  called code is a *registered ember script fn* (it went through sema, it's
  in the dispatch table, it obeys all budgets/bounds/traps), so even the
  "wrong" call stays inside the sandbox. The sandbox boundary (V2's actual
  concern) is not crossed.

- **A host native returning a `fn` handle the script then calls:** the host
  is trusted (`../spec/SAFETY_AND_SANDBOX.md` §1). The handle the host returns is
  trusted by the same trust that `array_new`'s returned handle is trusted.
  The guard still runs on it (it's a runtime i64 at the call site) and
  validates it — defense-in-depth, not a trust shortcut.

### The one-line invariant for ../spec/SAFETY_AND_SANDBOX.md

Add to `../spec/SAFETY_AND_SANDBOX.md` (the call-target-provenance forward guard,
now realized):

> **Call-target provenance (Tier 2).** A first-class function handle is an
> i64 = dispatch-table slot index. At every indirect call site (`handle(args)`),
> the JIT'd code validates the handle against a host-built bitset allowlist
> (one bit per registered script-fn slot, set by sema at compile time) before
> indexing the dispatch table. A handle that is out of range or whose bit is
> clear traps via `emit_trap(BadCallTarget)` (longjmp to the `ember_call`
> checkpoint). The JIT'd code never executes `call rax` / `call [table + h*8]`
> with an unvalidated script-supplied i64. This is the runtime backstop for
> the V2 ("i64-as-call-target") surface that first-class function references
> open; the type-level rule (§4.5: no i64-to-fn assignment) is the compile-time
> first line, the bitset guard is the runtime last line.

---

## 8. Test matrix

Model on `examples/v0_5_live_modules_test.cpp` / `v0_4_hardening_test.cpp`
(hand-build a `Program`, run sema, run codegen, call the entry, assert exit
code / trap reason). New file: `examples/v0_7_function_refs_test.cpp`
(Tier 2 follows the version cadence; or co-locate with the hardening tests).

### A. Handle creation and the `fn` type

| # | Script | Asserts |
|---|---|---|
| A1 | `let h = &fib;` then `return h;` | exit code = `fib`'s slot index (proves `&` bakes the slot as an i64 literal; the handle IS the slot). |
| A2 | `let h: i64 = &fib;` | **sema error**: `&fib` is `fn`, not bare `i64` — §4.4 says recorded-sig `fn` is `same` to bare `fn` but NOT to bare `i64`. Assert sema rejects with "let type mismatch (i64 = fn)". Proves the type tag distinguishes fn from i64. |
| A3 | `let h = 42; let f = &fib; h = f;` | **sema error**: assigning a `fn` to a bare `i64` (the reverse of A2). Assert rejected. |
| A4 | `let forged: i64 = 5; let f: fn = forged;` | **sema error** (§4.5): no i64-to-fn assignment. Assert rejected. Closes the type-level forging path. |
| A5 | `&&fib` | **sema error**: `&` of a non-name (`&` of a `FnHandleExpr`). Assert "may only be applied to a function name." |
| A6 | `&some_native` | **sema error**: `&` of a native fn, not a script fn. Assert "not a script function in this module." |
| A7 | `&undefined_name` | **sema error**: name not in `script_slots`. Assert "is not a script function." |

### B. First-class call — the happy path

| # | Script | Asserts |
|---|---|---|
| B1 | `fn id(x: i64) -> i64 { return x; } fn main() -> i64 { let h = &id; return h(42); }` | exit 42. Proves `handle(args)` dispatches to the right fn. |
| B2 | `fn add(a: i64, b: i64) -> i64 { return a+b; } ... let h = &add; return h(3, 4);` | exit 7. Two-arg handle call, Win64 ABI args (rcx, rdx) correct. |
| B3 | B1 but `let h: fn = &id;` (explicit `fn` type) | exit 42. The `fn` type annotation accepts a recorded-sig `&fn`. |
| B4 | A float-returning fn called through a handle | exit proves a float-returning fn is callable through a handle (xmm0 result path). |
| B5 | Recursion via handle: `fn fib(n: i64) -> i64 { if (n <= 1) return n; let f = &fib; return f(n-1) + f(n-2); }` | exit = `fib(N)` for small N. Proves the depth check + guard compose on recursive handle calls. |

### C. The guard — invalid handles trap (the call-target-provenance test)

| # | Script | Asserts |
|---|---|---|
| C1 | Out-of-range handle forced through a `fn`-typed var via a test-only host native `forge_fn_handle(slot)` returning `Type{is_fn_handle=true}` with slot `99999`: `let h: fn = forge_fn_handle(99999); h(1);` | **runtime trap**: `TrapReason::BadCallTarget`, detail "call-target provenance: handle is not a registered function". Proves the range check fires. |
| C2 | In-range unregistered slot: `forge_fn_handle(<a slot in [0,slot_count) whose bit is clear>)` — e.g. a slot reserved for a not-yet-compiled fn, or `slot_count - 1` if that slot's fn was never defined. | **runtime trap**: `BadCallTarget`. Proves the bit test fires (the in-range-but-unregistered case, which without the guard would be a null-deref crash, §7 case 2). |
| C3 | The lowest valid slot: `let h = &main; h();` if `main` is slot 0 — should work (main is a registered script fn). | exit = whatever `main` returns recursively (bounded by depth). Assert it works (not a trap). Proves the guard doesn't over-fire on the lowest valid slot. |
| C4 | Trap reason is `BadCallTarget`, not a raw SIGSEGV: the test sets `ctx.trap_stub` and checks `ctx.last_trap == TrapReason::BadCallTarget` after the call returns to the checkpoint. | Asserts the guard routes through `emit_trap` (longjmp), NOT `ud2`-only process death. This is the V6-DoS / V7 lesson: a trap must be recoverable, not a crash. |

### D. Dynamic registration + host drives it

| # | Setup | Asserts |
|---|---|---|
| D1 | Script: `@entry fn main() -> i64 { register_routine(&tick, 0); return 1; } fn tick(data: i64) -> i64 { return data + 1; }`. Host: after `main` returns, the host's routine table has one entry `(slot=slot_of_tick, data=0)`. Host calls it per-frame via `table.get(slot)` with arg `data`. | The host's call returns `1`. Proves the full dynamic-registration round-trip: `&tick` -> `register_routine` native -> host stores slot -> host calls via dispatch table. |
| D2 | D1 plus `unregister_routine(id)` where `id` is what `register_routine` returned; host then has no routine; calling it is a host-side no-op. | Asserts unregister works; the host's table is empty. |
| D3 | A routine whose `data` arg is non-trivial: `register_routine(&tick, 42)`; host calls with `data=42`; tick returns `data + 1` = 43. | Proves the `data` arg passes through (the handle is arg0, data is arg1; Win64 rcx=handle, rdx=data at the `register_routine` call site; the host then calls tick with rcx=data). |

### E. Cross-feature interactions

| # | Setup | Asserts |
|---|---|---|
| E1 | A handle passed cross-module: `mod::fn` returning a handle, the importer calls it. (Cross-module fn handles are §9's open question; this test may be deferred.) | TBD — see §9. |
| E2 | Hot-reload: a module with a `&fib` handle is reloaded; the handle (slot index) stays valid (slot indices never change, `../HOT_RELOAD.md` §7). `fib`'s new body runs on the next `handle(args)`. | Asserts slot stability lifts to handles (it should, free of charge). |
| E3 | Budget trap during a handle call: a handle call to a fn that loops forever hits the budget trap. | `TrapReason::BudgetExceeded` (the budget check is at loop back-edges inside the callee; the handle call's depth check + the callee's budget check compose). |

---

## 9. What's hard / what could go wrong

### 9.1 The `lea r11,[r11+rax*8]` SIB encoding — must be hand-verified

The dispatch uses `lea` with a SIB byte for `base=r11, index=rax, scale=8`.
The bytes I derived in §5.3 must be checked against `src/x64_emitter.hpp`'s
rex/modrm/SIB conventions. The emitter does not currently have a `lea`
primitive (grep `x64_emitter.hpp` for `lea` — none), so this is a raw byte
emission in `src/codegen.cpp`, not a helper. Risks:

- **REX bits wrong** (W/R/X/B): `lea` is a 64-bit address calc so W=1; the
  dest is r11 so the ModRM *reg* field needs R=1 (r11 is extended in the reg
  field); index rax is not extended (X=0); base r11 is extended (B=1). So
  REX = `0100 W R X B` = `0100 1 1 0 1` = `0x4D`, not the `0x49` a casual
  reader might reach for. **This is the kind of off-by-one that silently
  corrupts the call target** — a wrong REX bit makes the lea target a
  different register, and the subsequent `mov r11,[r11]` / `call r11` then
  jumps to wherever that wrong register points. **Mitigation: add a
  `lea_reg_mem_sib` helper to `src/x64_emitter.hpp` with the rex/modrm/SIB
  logic centralized and unit-tested in isolation** (like `load_reg_mem`
  already handles the rsp/SIB case), rather than hand-emitting bytes in
  codegen. Then in the test, dump the JIT'd bytes and disassemble the lea to
  confirm it is `lea r11,[r11+rax*8]`. **This is the single highest-risk
  implementation detail.**

- **`rax` clobbered between the guard and the lea:** the guard (§5.2) uses
  rax, rcx, r11 as scratch. The handle must survive into the lea. §5.1 stashes
  it in rbx and reloads `mov rax, rbx` before the lea — but the guard sequence
  in §5.2 uses rcx and r11 freely, which is fine (rcx/r11 are caller-saved and
  the arg stash already moved arg0 out of rcx into the stash and back; rcx is
  arg0 at the *call*, so the guard must NOT clobber rcx after the stash places
  arg0 into rcx). **Ordering hazard:** the guard must run *before* the arg
  stash places args into rcx/rdx/r8/r9, OR the guard must not clobber those.
  Cleanest: run the guard *before* the arg stash, with the handle already in
  rbx (from the target's eval-into-rax-then-mov-rbx at the very top), then do
  the arg stash, then `mov rax, rbx` + lea + load + call. The guard's scratch
  (rcx, r11) is free before the arg stash. This reordering must be done
  carefully; the plan's §5.1 order (guard after stash) is *a sketch and must
  be fixed to guard-before-stash* at implementation time. **This is the
  second highest-risk detail.**

### 9.2 The allowlist's lifetime and relocation

The allowlist base is baked as a raw imm64 (`mov r11, ctx.fn_allowlist_base`,
§5.2.1). This is correct iff the allowlist is allocated once and never moves
for the module's lifetime. If a future hot-reload or module-linking change
reallocates the allowlist, the baked address dangles. **Mitigation:** the
allowlist is allocated per-`sema()` call (it's derived from `Program::funcs`
+ `script_slots`, both stable per compile), stored on the context, and lives
as long as the dispatch table (which the `DispatchTableBase` reloc already
handles for the table). If hot-reload ever needs to rebuild the allowlist,
add an `AbsFixup::Kind::FnAllowlistBase` and patch it at reload — but that's
v2+, not Tier 2. The plan documents this as a known limitation, not a bug.

### 9.3 The "bare `fn` parameter" signature hole (§4.3)

A `fn`-typed *parameter* (`register_routine(fn h, i64 data)`) has no recorded
signature. The script can then `h(any_args)` and sema does NOT type-check the
args against the target's actual signature. This means:
- `let f: fn = &add; f(1);` (one arg, but `add` takes two) — sema passes (no
  recorded sig), the guard passes (the handle is valid), and the call
  dispatches into `add` with rcx=1, rdx=garbage. `add` returns `1 + garbage`.
- This is **not a sandbox violation** (the called code is a registered fn,
  obeys all budgets/bounds), but it is a **type-soundness hole**: the script
  gets a garbage result instead of a sema error.

**Why it's acceptable for Tier 2:** the alternative (requiring all `fn`
params to carry a signature — `fn(i64,i64)->i64` syntax) is a v2+ type-system
expansion (parameterized types, signature equality). Tier 2's goal is the
first-class call + the guard, not a full function-type system. The hole is
documented in `../ROADMAP.md` as the v2+ refinement: "`fn(i64)->i64` parameterized
function types for compile-time arg checking." The guard ensures the *call
target* is always safe; the *args* are the caller's responsibility at the
bare-`fn` type, same as a C function pointer with no prototype.

**Risk:** a script author writes `let f: fn = &add; f(1);` and gets a
mysterious garbage return with no sema error. **Mitigation:** sema emits a
*warning* (not an error) when a bare-`fn`-typed variable is called with args
that don't match *any* registered fn's signature — a best-effort lint, not a
soundness check. This is optional for Tier 2; the test matrix (B1-B5) uses
recorded-sig handles, which DO check args.

### 9.4 The `FnHandleExpr`-as-compile-time-literal vs. first-class-ness tension

`&fib` bakes the slot as a compile-time i64 literal (§4.2). This is honest
(the slot IS known at the `&` site), but it means a handle is *not* a runtime
value at creation — only at *use* (when it's been stored in a variable, passed
to a native, etc.). A test like A1 (`return h;` returns the slot) confirms
this. The tension: if someone expects `&fib` to be a runtime "function
pointer load," they'll be surprised it's a literal. **Mitigation:** the plan
documents this clearly (§4.2: "the handle IS the slot index, as an i64 ...
this is NOT a runtime value, despite the first-class call being one"). The
first-class-ness is real at the call site (the target is a runtime i64); the
handle-of is a compile-time reification, which is more, not less, efficient.

### 9.5 Cross-module function handles — explicitly deferred

`mod::fn` returning a handle, or `&mod::fn`, is NOT in scope for Tier 2. The
allowlist (§4.1) is per-module (built from `Program::funcs` + `script_slots`
of *this* module). A handle from another module would be that module's slot
index, which is meaningless against this module's dispatch table. Cross-module
handles need either (a) a global handle space (a `(module_id, slot)` pair
encoded into one i64), or (b) per-module allowlists and a guard that knows
which module a handle is for. This is real work and is **Tier 2.5 / v0.8**,
documented in `../ROADMAP.md` as the cross-module extension. Tier 2 ships
intra-module handles only. The `register_routine` native's handle is always
this module's (the script passes `&my_fn`, not `&mod::their_fn`).

### 9.6 The `string`-handle precedent and whether `fn` should mirror it exactly

`string` is `Prim::I64` with `struct_name == "string"` (`src/sema.cpp` ~383).
`fn` is `Prim::I64` with `is_fn_handle == true` (§2). The two are parallel
patterns. The risk: if a future feature needs *both* tags on one type (a
`string` of `fn`?), the two tag fields conflict. **Mitigation:** they don't —
`struct_name` and `is_fn_handle` are independent fields; a type with both set
is nonsensical and sema rejects it (a `fn` is not a `string`). The parallel is
safe. The only divergence is that `string`'s tag is a name (`struct_name`)
and `fn`'s is a bool (`is_fn_handle`) — `fn` has no "name" because all `fn`
types are the same representation (unlike `string`/`vec3`/`mat4` which are
distinct handle types). This is fine.

### 9.7 Register pressure at the call site

The first-class call site uses: rax (target eval + guard + lea index), rbx
(handle stash, callee-saved), rcx (guard scratch + arg0), r11 (dispatch base
+ lea + load), plus the standard arg regs. This is within x86-64's 16 GP
registers, but rbx is callee-saved and the prologue already saves it
(`src/codegen.cpp` frame prologue). The §5.1 `push rbx`/`pop rbx` around the
call adds a balanced stack pair — but this interacts with the `sub rsp,
total` / `add rsp, total` arg-stash bookkeeping (the push must be accounted in
`total` or done outside the `sub rsp` region). **Risk:** rsp misalignment at
the `call` (Win64 requires 16-aligned rsp at call + 32-byte shadow space; the
existing arg stash already handles this via `round16`, `src/codegen.cpp`
~1553). The extra `push rbx` shifts rsp by 8, breaking the 16-alignment. The
cleanest fix: do NOT push rbx; instead, save the handle into the stash region
itself (a scratch word in the already-`sub rsp, total`'d region), or evaluate
the target *after* the `sub rsp, total` and stash it at a known offset.
**This needs careful frame-layout accounting at implementation time** — the
plan's `push rbx`/`pop rbx` is a sketch, not a verified-correct sequence. The
honest implementation likely evaluates the target into a dedicated stash slot
(reserved by bumping `total` by 8 and writing the handle to `[rsp + stash_size]`
before the arg eval loop), then loads it into rax after the args are placed.
This avoids any push/pop and any rbx usage entirely. **This is the third
highest-risk detail** and is the one most likely to "work in the simple test
and break in a nested-call test."

### 9.8 The trap-stub-null fallback path

`emit_trap` (`src/codegen.cpp` ~233) falls back to `ud2` when `trap_stub` is
null (pre-v0.4 hosts). The guard's trap uses `emit_trap(BadCallTarget, ...)`,
so on a null-stub host the guard's trap is a `ud2` -> process death. This is
**acceptable** (a null-stub host has chosen "traps kill the process" globally;
the guard's trap is no worse than any other). But a test must set
`ctx.trap_stub` to verify the recoverable path (test C4). The `ud2` fallback
is not a regression — it matches how bounds/budget traps behave on null-stub
hosts today.

### 9.9 What I did NOT verify firsthand and flag as needing implementation-time check

- The exact `lea` SIB/REX bytes (§9.1) — derived, not disassembled.
- The register-pressure / rsp-alignment sequence (§9.7) — sketched, not
  assembled.
- Whether `Type::same`'s existing callers tolerate the new `is_fn_handle`
  field (the field defaults false, so existing types are unaffected, but a
  grep for `->same(` call sites to confirm no silent behavior change is
  warranted at implementation time).
- Whether `words_for_type` / `local_width_bytes` need any change for a
  `is_fn_handle` i64 (they should not — the type is `Prim::I64` with a flag,
  and those functions branch on `prim`/`struct_name`/`is_slice`/`array_len`;
  the flag is invisible to them). Confirm by reading them at implementation
  time.

---

## Appendix: files touched (summary for the implementer)

| File | Change | New? |
|---|---|---|
| `src/ast.hpp` | `FnHandleExpr` node; `CallExpr::indirect_target`/`is_indirect`; `Type::is_fn_handle`/`recorded_params`/`recorded_ret`/`has_recorded_sig` | new fields + 1 node |
| `src/lexer.hpp` | none (`Kw_fn`, `Tk::Amp` exist) | — |
| `src/parser.cpp` | `parse_unary` prefix `&` case; `parse_postfix` relaxed call target (the lifted throw); `parse_type` `Kw_fn` case | edits |
| `src/sema.hpp` | `RegisteredFn`/`RegisteredFnTable`; `sema()` signature (allowlist or build-from-Program) | new types |
| `src/sema.cpp` | `FnHandleExpr` case; `CallExpr` indirect case (top of the ladder); allowlist build at entry; i64-to-fn assignment rejection | new branches |
| `src/types.cpp` | `Type::same` (fn-handle rules); `to_string` (`fn` alias); confirm `byte_size`/`align`/`is_int` unchanged | edits |
| `src/x64_emitter.hpp` | **`lea_reg_mem_sib` helper** (§9.1 mitigation — centralize the SIB lea); optionally a `call_mem_zero_disp` variant | new helper(s) |
| `src/codegen.hpp` | `CodeGenCtx::fn_allowlist_base` / `fn_slot_count` | new fields |
| `src/codegen.cpp` | `CallExpr` `is_indirect` branch (eval target, guard, lea, load, call); `emit_call_target_guard` helper; the `TrapReason::BadCallTarget` trap | new branch + helper |
| `src/context.hpp` | `TrapReason::BadCallTarget`; `trap_reason_str` case | edits |
| host (`examples/ember_cli.cpp` / `game_host.cpp` / a new `ext_lifecycle`) | build the allowlist (bitset from `script_slots`); `register_routine`/`unregister_routine`/`forge_fn_handle`(test-only) natives via `BindingBuilder::add` | new natives |
| `examples/v0_7_function_refs_test.cpp` | the test matrix (§8) | new file |
| `../spec/SAFETY_AND_SANDBOX.md` | the call-target-provenance invariant (§7 one-liner) | edit |
| `../ROADMAP.md` | mark Tier 2 function references shipped; note the bare-`fn` signature hole (§9.3) and cross-module deferral (§9.5) as v2+ | edit |

---

**Historical plan (source now implemented).** Every claim about the
current code — `CallExpr` emission at `src/codegen.cpp` ~1463/1564, the
kind-0 `DispatchTableBase` reloc at ~1573, the `emit_trap` stub at ~233,
the `parse_postfix` throw at `src/parser.cpp` ~398, the `script_slots` /
`Program::funcs` provenance of slot numbers, the `DispatchTable`
nullptr-init, the absence of `Kw_cast`, and the `as`-divergence — was read
firsthand from the source in `ember/src/` before writing this. The one
piece NOT read — the exact `lea` SIB/REX bytes — is flagged as a §9.1
implementation-time verification, not a claim.
