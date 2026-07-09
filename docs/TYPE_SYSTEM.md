# ember - type system spec

Detail doc for DESIGN.md Section 2. Formalizes types, layout, conversions.

> **Implementation status: v0.1** - this is the v1.0 design spec. The
> current repo implements the JIT codegen proof (encoder, label/patch,
> exec-mem, `.em` format). See `README.md` for what's shipped; see
> `CODEGEN_SPEC.md` Section 12 + Section 15 for the acceptance suite. This doc's
> content is the target design, not a claim of current implementation.

## 1. Primitive types

| Type | Width | Align | Notes |
|---|---|---|---|
| `bool` | 1 byte | 1 | Stored as 0/1. Any nonzero read from host memory via a binding is *not* implicitly truthy  -  only script-produced bools are guaranteed 0/1; a `bool` field read from a host struct is the host's responsibility to keep canonical (documented requirement in BINDING_API.md, not enforced by codegen  -  enforcing it would cost a normalize-on-every-read branch we don't want). |
| `i8/i16/i32/i64` | 1/2/4/8 | = width | Two's complement, signed ops (checked overflow in debug, see CODEGEN_SPEC.md Section 10). |
| `u8/u16/u32/u64` | 1/2/4/8 | = width | Wraparound always defined. |
| `f32` | 4 | 4 | IEEE-754 binary32. |
| `f64` | 8 | 8 | IEEE-754 binary64. |
| `void` | 0 |  -  | Return-type only. Not a value type  -  cannot declare a `void` variable/field/param. |

No `char`/`wchar` distinct type in v1 - text is `u8` byte slices
(Section 4). No arbitrary-width integers, no `usize`/pointer-sized integer
type exposed to script (slices carry length as `i64` explicitly,
avoids "is usize 32 or 64 bit" ambiguity entirely).

## 2. Struct layout

Two layout modes, chosen per struct at declaration:

- **Host-mapped struct** (declared via `TypeBuilder`/`StructBuilder`
  from the native side, BINDING_API.md Section 4): every field has an
  explicit byte offset supplied by the host (`offsetof` in the real
  C++ struct). Script sees exactly the host's layout, including any
  padding - script cannot declare a *new* field on a host-mapped
  struct, only reference the ones registered. Total size is also
  supplied by the host (`sizeof` of the real struct), not computed by
  ember.
- **Script-declared struct** (a `struct Foo { ... }` written directly
  in ember source, no host backing): layout computed by ember using
  the same rules as a standard C compiler on the target platform
  (MSVC x64 struct layout, since Windows is the v1 target):
  1. Fields laid out in declaration order.
  2. Each field's offset rounded up to its own alignment (from Section 1's
     table, or a nested struct's alignment = max of its members').
  3. Struct's own alignment = max alignment of any member (minimum 1).
  4. Struct's total size rounded up to a multiple of its alignment
     (trailing padding, matches C rules, needed for correct array-of-
     struct stride).
  - No `#pragma pack`/custom alignment attributes in v1 - one layout
    rule, no exceptions (YAGNI; add if a host integration genuinely
    needs a packed layout later).
- **Nested structs**: allowed, computed recursively with the rule
  above. **Self-referential structs** (a struct containing a field of
  its own type, or a cycle through multiple structs) are a **compile
  error** at struct-declaration sema time - v1 has no pointer/handle
  type a struct field could use to break the cycle indirectly (see
  Section 5), so a cycle is always infinite size and always wrong; detect via
  a simple DFS over the not-yet-finalized struct dependency graph and
  report the cycle path in the error message.
- **Empty struct** (`struct Empty {}`): size 0, align 1 - allowed
  (useful as a marker/tag type for annotations or generic-free
  polymorphism substitutes), but a size-0 struct as a *value* passed
  by-value to a native function is documented as UB-free-but-
  implementation-defined (Win64 ABI doesn't define zero-size
  by-value params meaningfully) - sema rejects passing an empty struct
  by value across the native boundary specifically (BINDING_API.md
  Section 6), script-to-script is fine (it's just a zero-size frame slot,
  no call-boundary ambiguity).

## 3. Fixed arrays

`T[N]` where `N` is a compile-time constant (integer literal or a
`const` - v1 has no `const` expressions beyond literals, so `N` must
literally be a literal in v1; named compile-time constants are a
post-v1 nicety, not needed yet).
- Layout: `N` contiguous elements of `T`, each at `i * sizeof(T)`
  (`sizeof(T)` already includes T's own trailing padding if T is a
  struct - this gives correct C-compatible array stride automatically
  from Section 2's struct sizing rule).
- `N == 0` is a compile error (a zero-length fixed array has no
  sensible use - if a variable-length empty case is needed, that's
  what slices are for).
- Fixed arrays are always **value types**: assigning a fixed-array
  variable to another copies all `N` elements (memcpy-equivalent, one
  `rep movsb`-style loop or a sequence of plain loads/stores if `N` is
  small and known - codegen picks a fixed unrolled copy for `N <= 8`
  elements-of-machine-word-size-or-less, else emits a compact copy
  loop; see CODEGEN_SPEC.md, this is a lowering detail not a new
  instruction class). Passing a fixed array *by value* to a function
  (script or native) follows the same rule as any other >8-byte
  value type: passed by hidden pointer to a copy (CODEGEN_SPEC.md Section 8)  - 
  script semantics still say "value passed by value," codegen's
  pointer-passing is just the mechanism.
- Indexing a fixed array with a compile-time-constant index is
  bounds-checked at compile time, zero runtime cost
  (CODEGEN_SPEC.md Section 9). Runtime index still bounds-checked at runtime
  against the known-at-compile-time `N` (the `len_reg` in the
  bounds-check sequence is simply the immediate `N`, so this compiles
  to `cmp i_reg, N_imm; jae trap` - one immediate compare, cheaper
  than the slice case which needs an actual length load).

## 4. Slices

`slice<T>` (spelled `T[]` in source, e.g. `i32[]`, `Vec3[]`) is a
two-word value: `{ T* ptr; i64 len; }`, always passed and stored as
that pair - never a single pointer. This is the **only** way script
code can reference a variable-length or host-owned range of memory;
there is no separate raw-pointer type (DESIGN.md Section 5 requirement,
enforced structurally here: the grammar has no pointer-dereference or
pointer-arithmetic syntax at all, so there's nothing to type-check
against - it's not "pointers exist but are checked," pointers as a
script-visible concept simply don't exist).

- **Construction**: a slice value can only be produced by:
  1. A native binding handing one back (return type `slice<T>` from a
     `NativeFn`, or an `iterable`-style TypeBuilder registration  - 
     BINDING_API.md Section 4).
  2. Taking a slice-view of a script-local fixed array via `arr[..]`
     syntax (produces `{ &arr[0], N }`) - this is the *only*
     address-of-like operation in the language, and it's restricted to
     exactly this form (whole-array view; no arbitrary sub-range
     slicing syntax in v1 - `arr[a..b]` partial slicing is a
     post-v1 nicety, add only if a real use case shows up, YAGNI).
  3. A field of a host-mapped struct declared with slice type
     (host lays out `{ptr, len}` at a known offset matching this
     exact 16-byte representation - documented ABI requirement on the
     host side, BINDING_API.md Section 4).
- **Lifetime**: a slice is *never* an owning reference. Script cannot
  outlive the memory it points into by construction (a fixed-array
  view's underlying array is either a still-live local in the current
  call chain, or came from a native call whose contract is "valid for
  at least the duration of this call" - v1 does **not** attempt
  static lifetime checking à la Rust borrow-checking; this is
  explicitly a documented trust boundary, not a soundness hole we're
  pretending doesn't exist: BINDING_API.md and SAFETY_AND_SANDBOX.md
  both state that native functions returning slices are trusted to
  return memory that outlives its use, same trust level as any
  AngelScript/the surveyed native-JIT language native binding).
- **Bounds check**: always at runtime on index (Section 3/CODEGEN_SPEC.md Section 9)
  since `len` is a runtime field, never compile-time-known even if
  `ptr` happens to be.
- **Element type restrictions**: `slice<slice<T>>` (nested slice) is
  **disallowed** in v1 - adds no real capability for game-scripting
  use cases yet and complicates the "exactly two machine words" ABI
  story (a slice-of-slices would need the inner slices contiguous,
  which is a host layout decision, not something ember can assume).
  If needed later, it's an additive change (YAGNI).

## 5. References

No standalone reference/pointer type beyond what's embedded in a
slice. A script function parameter that needs "pass this struct by
reference so the callee can mutate it" is expressed as a `slice<T>`
of length 1 (idiomatic pattern, not a separate language feature) - no
new type or syntax needed, matches YAGNI stance and keeps exactly one
memory-indirection concept in the whole language.

## 6. Type compatibility & implicit conversion

- **Numeric widening**: implicit only in the "safe and lossless"
  direction: `i8->i16->i32->i64`, `u8->u16->u32->u64`, `f32->f64`.
  Signed-to-unsigned or unsigned-to-signed of the *same* width is
  **never implicit** (classic bug source) - requires an explicit
  cast expression `x as u32`. Int-to-float and float-to-int are
  **never implicit** either, even widening-looking ones like
  `i32->f64` - always `as`. This is stricter than C on purpose: C's
  implicit-conversion matrix is a well-known footgun source and
  costs nothing to forbid here since `as` is one token.
  - Narrowing (`i64->i32`, `f64->f32`, etc.) is **always** explicit
    (`as`), never implicit, no exceptions.
- **Explicit cast (`as`) semantics**:
  - int narrowing: truncation (bit-pattern truncate, matches C).
  - int widening signed: sign-extend. unsigned: zero-extend.
  - int<->float: uses `cvtsi2sd`/`cvttsd2si` family (CODEGEN_SPEC.md
    Section 3) - float-to-int is *truncating toward zero* (matches
    `cvttsd2si`, not round-to-nearest); out-of-range float-to-int cast
    (e.g. `1e30 as i32`) is **checked in debug, implementation-defined
    saturation-or-garbage in release** - same debug/release split
    philosophy as arithmetic overflow (CODEGEN_SPEC.md Section 10), since
    checking costs a compare+branch per cast.
  - bool<->int: `bool as i32` gives 0/1. `x as bool` (int) is
    **disallowed** - no implicit "nonzero is true," must write
    `x != 0` explicitly (removes an entire class of "did they mean
    truthiness or did they mean the C++0-and-nonzero convention"
    ambiguity from the language; zero cost to require it).
- **Struct types**: never implicitly convert to one another, even
  with identical field layouts (nominal typing, not structural)  - 
  matches host-mapped-struct identity expectations (BINDING_API.md).
- **Slice element type**: `slice<T>` and `slice<U>` are never
  compatible even if `T`/`U` have the same size - nominal, no
  "reinterpret slice" operation in v1 (no `bytes-as-T` reinterpret
  cast - if a host binding needs that, it does the reinterpret on the
  native side and hands back a properly-typed slice).

## 7. Operator result types & the index-expression type rule

- Arithmetic (`+ - * / %`) on two operands of the same numeric type
  produces that type. Mixed-type arithmetic (`i32 + i64`, `f32 +
  f64`) is a **compile error**, not an implicit promotion - script
  must cast explicitly. (Differs from C's usual-arithmetic-conversions
  on purpose - same footgun-avoidance rationale as Section 6; game-script
  authors are not expected to reason about C's promotion rules, and
  removing them removes a class of silent-precision-loss bugs.)
- Comparison (`< <= > >= == !=`) requires same-type operands (same
  rule as arithmetic), produces `bool`.
- Logical (`&& || !`) requires `bool` operands, produces `bool`. No
  int-as-bool coercion (Section 6) reinforces this - `if (x)` where `x` is
  `i32` is a **compile error**, must write `if (x != 0)`.
- Bitwise (`& | ^ ~ << >>`) only defined on integer types (any
  width/signedness), same-type-required rule for binary forms.
  `<<`/`>>` right-hand operand may be any unsigned integer type
  (shift amount doesn't need to match the shifted value's type)  - 
  the one deliberate exception to "same type required," because
  requiring `x << (3 as i64)` for `x: i32` would be pure friction with
  no safety benefit (shift-amount type doesn't affect result type or
  introduce precision loss).
- **Index expression type rule**: `arr[i]` / `slice[i]` requires `i`
  to be an unsigned integer type (`u8/u16/u32/u64`) - signed integer
  indices are a **compile error**, must cast explicitly (`arr[i as
  u64]`). This is what makes CODEGEN_SPEC.md Section 9's claim "no negative
  index reaches codegen" true by construction rather than by a runtime
  check: it's impossible to type-check a negative-index expression as
  well-typed in the first place, since a signed value used as an
  index is rejected before codegen ever sees it, and an unsigned value
  cannot represent a compile-time-literal-negative number (the lexer
  never produces a negative unsigned literal - `-1u` is a unary-minus
  applied to `1u`, which for unsigned types wraps at *runtime* per Section 6,
  i.e. it's a huge positive number, correctly caught by the ordinary
  runtime bounds check, not a special case).
- Operator overloading for struct types (`bin_add` etc. registered via
  `TypeBuilder`, BINDING_API.md Section 3) resolves to a native function call
  - sema resolves the operator to the registered `NativeFn` at
  compile time (same "no runtime dispatch" principle as everything
  else here: overload resolution is a compile-time lookup keyed on
  `(operator, lhs_type, rhs_type)`, producing a direct call exactly
  like any other native call, CODEGEN_SPEC.md Section 8). **Ambiguous or
  missing operator overload** is a compile error with the attempted
  `(op, lhs_type, rhs_type)` triple in the message.

## 8. Function signature types & `void` enforcement

- A function's type is `(param_types...) -> return_type`. No function
  values / function pointers as script-visible values in v1 (no
  first-class functions, no closures - DESIGN.md non-goals) - a
  function name in an expression position other than a direct call is
  a compile error. (Callback-style native APIs, e.g. "register this
  script function as an event handler," are handled via the
  annotation mechanism Section 2/DESIGN.md Section 6 dispatch-table slot lookup by
  name from the host side, not by passing a function value through
  script expressions.)
- **Non-void function must return on every path**: sema performs a
  straightforward structured-control-flow reachability check (every
  `if` needs an `else` that also returns, `while`/`for` bodies don't
  count as "always executes" for this analysis since the condition
  might be false immediately, a `return` statement makes its block
  terminate) - if any path falls off the end of a non-void function
  body, compile error "not all paths return a value." This is what
  lets CODEGEN_SPEC.md Section 6's epilogue generation assume the guarantee
  holds, no runtime fallback needed.
- **`void` function with `return expr;`**: compile error (return
  expression on a void function). **`void` function with bare
  `return;`**: fine, early-exit, compiles to the normal per-return
  epilogue (CODEGEN_SPEC.md Section 6) with no value moved into `rax`/`xmm0`.
- **Unreachable code after `return`/`break`/`continue`** in the same
  block: not an error in v1 (a warning would be nice, not required  - 
  YAGNI on a full unreachable-code diagnostic pass; codegen simply
  never emits it since lowering stops at the terminator).

## 9. `auto` - the one type-inference rule

`auto` is allowed **only** as a local-variable declaration's type,
and only when the declaration has an initializer: `auto x = expr;`
infers `x`'s type as exactly the type of `expr` (no widening, no
"choose a sensible default," just literally the initializer
expression's already-computed static type). Not allowed for:
function parameters, function return types, struct fields, or a
variable declaration without an initializer. This is deliberately the
smallest possible inference rule - full bidirectional type inference
(Mun/Rust-style) is explicitly out of scope; `auto` here is pure
syntactic sugar to avoid repeating a type name, resolved trivially
during sema's single left-to-right pass with no unification/solver
needed (matches "monomorphic types only so simple," DESIGN.md Section 3).

## 10. Annotation argument types

`@event("player_hit")` / `@on_tick` style annotations (DESIGN.md Section 2)
take a fixed, small argument grammar: zero args, or a comma-separated
list of literal values only (string/int/float/bool literals - no
expressions, no identifiers). Annotations are metadata consumed by
`get_annotations`/`get_annotated_functions` (DESIGN.md Section 8) at the
sema/introspection level; they carry **no** runtime type-checking
requirement against how the host later interprets them (e.g. the host
deciding `@event("player_hit")` means "call with a `HitInfo` slice
arg" is a host-side convention, not something ember's type system
verifies) - this mirrors the surveyed native-JIT language's `annotation_info` being untyped
name+args (RESEARCH_NOTES.md), and keeps the annotation feature purely
declarative metadata, not a second type-checked calling-convention
layer.

## 11. v1 feature additions (this spec pass)

The small C-style conveniences added in the completeness pass
(`GAP_ANALYSIS.md` Section 5). Each is cheap and C-expected; semantics here
keep them consistent with the rules above (same-type-required, no
implicit promotion, debug-checked overflow).

### 11.1 Ternary `?:`

`cond ? a : b` - `cond` must be `bool` (no int-as-bool coercion, Section 6);
`a` and `b` must be the **same type** (the no-implicit-promotion rule
applies here too - `cond ? 1i32 : 2i64` is a compile error, both
arms must be identical type). Result type = that common type.
Right-associative (chains as `cond ? a : (cond2 ? b : c)`), matching
C. **Both arms are always type-checked**; a common C/C++ wart
(short-circuit means only one runs at runtime, so the unused arm's
side effects don't happen) is *not* a type-rule concern - both arms
must typecheck regardless of which executes, identical to `if`/`else`
arms. `cond ? a : b` where one arm is `void` (e.g.
`cond ? return x : return y`) is a **compile error** - ternary
produces a value, use `if`/`else` for void branches.

### 11.2 `++`/`--` (prefix and postfix)

Integer operands only (`i*`/`u*`, any width); applying to `bool`/`f*`/
struct is a compile error (no `++` on a float, matching C - many
footguns removed). `++x` returns the new value; `x++` returns the old
value (postfix needs a temp - lowered to load, increment, store,
yield-old). **Checked overflow in debug** (same flag as arithmetic,
`CODEGEN_SPEC.md` Section 10) - `x++` past `INT64_MAX` traps in debug,
wraps in release. Operand must be an lvalue (assignable: local,
field, index) - `++(x + 1)` is a compile error. Acts on the operand
**once** (not twice - relevant only if the lvalue has side effects in
its address computation, e.g. `arr[f()]++` calls `f()` exactly once,
matching the compound-assignment once-evaluation rule, Section 11.6).

### 11.3 Compound assignment (`+=` etc.)

`x += y` typechecks exactly as `x = x + y` would, with the
same-type-required rule (Section 7) applying - `i32 += i64` is a compile
error. **Operand `x` evaluated once**, not twice - important when `x`
is `obj.field` or `arr[i]` with a side-effecting `obj`/`arr`/`i`
subexpression: `arr[f()] += 1` calls `f()` exactly once (load base,
compute index once, load, add, store). Lowered to: load target →
compute `target op value` → store back, with the address of `target`
held in a temp across the value computation. `<<=`/`>>=`/`&=`/`|=`/
`^=`/`%=` follow the same rule with their respective operator
constraints (Section 7: shift-amount may be any unsigned type, bitwise is
integer-only).

### 11.4 `sizeof` / `offsetof`

`sizeof(T)` and `offsetof(T, field_name)` are **compile-time
constants** of type `u64`, usable wherever a `constexpr` is (array
sizes, `switch` case labels, `constexpr` initializers, Section 11.7).
`sizeof` works on any type (primitives, structs, arrays, slices  - 
slice `sizeof` is 16, the two-word `{ptr,len}` representation,
TYPE_SYSTEM.md Section 4). `offsetof` works only on struct types
(script-declared or host-mapped) and the field must exist (sema
rejects unknown field names). These never emit code - they're folded
to literals at sema using the struct-layout pass results (Section 2 / sema
pass 1, COMPILER_PIPELINE.md Section 4).

### 11.5 `const` / `constexpr`

- `const x: T = expr;` - an **immutable local** (or global, when at
  module scope: `global const x: T = expr;`). Assignment after
  declaration is a compile error (sema-only enforcement, zero runtime
  cost - storage is identical to `let`, just with write-after-init
  rejected). `expr` is typechecked normally (implicit conversions per
  Section 6 apply).
- `constexpr` - a marker that an expression/value is **compile-time-
  evaluable**. v1 restricts what can be `constexpr`: literal values,
  `sizeof`/`offsetof`, and integer arithmetic on other `constexpr`
  values (no function calls, no runtime values). Used in: array sizes
  (`let buf: [u8; constexpr 256]` - though v1 array sizes already
  must be literals, `constexpr` lets a *named* compile-time value be
  used, `const N: u32 = 256; let buf: [u8; N]`), `switch` case
  labels, `offsetof`/`sizeof` arg contexts. Full const-eval (recursive
  `constexpr fn`s, `static_assert`) is v2 (`ROADMAP.md` Tier 1).

### 11.6 `do-while` / `switch` / `defer` (semantics, grammar in
COMPILER_PIPELINE.md Section 2)

- `do { ... } while (cond);` - body runs at least once, condition
  checked at the bottom. Same bool-operand rule as `if`/`while` (`cond`
  must be `bool`). Budget check placed at the back-edge to the top of
  the body, same as `while` (SAFETY_AND_SANDBOX.md Section 3).
- `switch (e) { case L: ...; break; ... }` - `e` integer-typed, `L`
  constexpr integer literals, unique within the switch, in-range for
  `e`'s type. **No fallthrough**: each case must end in
  `break`/`return`/`continue`/`trap` (sema rejects falling off a case
  body - eliminates the entire C-fallthrough footgun class at the
  grammar level, GAP_ANALYSIS.md Section 5). `default` optional. Lowering
  and jump-table vs cascade decision in CODEGEN_SPEC.md Section 13.
- `defer expr;` - `expr` (typically a call, e.g.
  `defer release_resource(handle);`) runs at scope exit. Semantics:
  LIFO order if multiple `defer`s in one scope; runs on every scope-
  exit path (return/break/continue/fallthrough) **except the trap-
  unwind path** (Section 11 of COMPILER_PIPELINE.md, GAP_ANALYSIS.md Section 5  - 
  traps abort, don't gracefully unwind locals). `defer`'s `expr` is
  typechecked at the `defer` statement's location (so it sees the
  in-scope variables at that point), not at scope exit - meaning if a
  variable the deferred expression references is later reassigned, the
  deferred call sees the *final* value at exit, not the value at
  `defer` time (matches C++ `defer`/Go `defer` semantics - defer
  captures the *expression*, evaluated lazily, not a snapshot of
  variable values).

### 11.7 String interpolation `f"...{expr}..."`

An `f"..."` literal is syntactic sugar lowered at sema to a call to
the host `__fmt` addon (`GAP_ANALYSIS.md` Section 3) - not a language
builtin. `f"v={x} and y={y}"` desugars to roughly
`__fmt(__sl("v="), x, __sl(" and y="), y)`, returning an `array<u8>`
handle. Hole expressions (`{expr}`) are typechecked normally against
whatever `__fmt`'s signature accepts (the standard `__fmt` addon
accepts `slice<u8>` for the literal parts and any of `i64`/`u64`/
`f64`/`bool`/`slice<u8>` for holes - finer integer widths require an
explicit `as` cast to `i64`/`u64` first). If `__fmt` isn't
registered, sema errors: "string interpolation requires the `__fmt`
addon" - the feature simply doesn't exist in a host that doesn't ship
the format addon, no silent fallback. The result is an `array<u8>`
handle (host-owned memory, MEMORY_AND_GC.md Section 1 category 2), not a
slice - interpolation produces fresh owned bytes, the addon manages
their lifetime.
