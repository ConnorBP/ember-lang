# ember - type system spec

Detail doc for ../planning/DESIGN.md Section 2. Formalizes the shipped v1 types, Ember
layout, conversions, and ABI limits; future extensions are labeled inline.

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
  in Ember source, no host backing): fields are **tightly packed in
  declaration order**, with no alignment gaps or trailing padding. Nested
  structs and fixed arrays contribute their recursively resolved Ember size.
  This is Ember layout, not MSVC/C host layout. Native by-value aggregate
  arguments are supported only through 8 bytes; sema rejects larger native
  aggregate arguments. Large aggregate returns use the tested hidden-return
  path. Hosts needing arbitrary C layouts must use explicit bindings/handles;
  a host-layout builder remains deferred.
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
  (`sizeof(T)` is the tightly packed Ember size for a script struct).
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
there is no separate raw-pointer type (../planning/DESIGN.md Section 5 requirement,
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
  first-class functions, no closures - ../planning/DESIGN.md non-goals) - a
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

> **Deprecation note (2026-07-10, commit `d852160`):** `auto` is
> **deprecated** (soft - emits a non-fatal sema warning; the program still
> compiles and runs). It is a redundant spelling of `let x = expr;`
> inference (both share the same `is_auto` inference path). The canonical
> forms are `let x = expr;` (inference) and `let x: T = expr;` (explicit).
> `auto` is slated for removal after a grace period - migrate any
> `auto x = e;` to `let x = e;`. See `COMPILER_PIPELINE.md` §2 (which records
> the deprecation in the grammar note) and `../ROADMAP.md` "Slated for
> removal (deprecated)". The inference rule below is preserved for reference
> and because `let`-inference shares it.

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
needed (matches "monomorphic types only so simple," ../planning/DESIGN.md Section 3).

## 10. Annotation argument types

`@event("player_hit")` / `@on_tick` style annotations (../planning/DESIGN.md Section 2)
take a fixed, small argument grammar: zero args, or a comma-separated
list of literal values only (string/int/float/bool literals - no
expressions, no identifiers). Annotations are metadata consumed by
`get_annotations`/`get_annotated_functions` (../planning/DESIGN.md Section 8) at the
sema/introspection level; they carry **no** runtime type-checking
requirement against how the host later interprets them (e.g. the host
deciding `@event("player_hit")` means "call with a `HitInfo` slice
arg" is a host-side convention, not something ember's type system
verifies) - this mirrors the surveyed native-JIT language's `annotation_info` being untyped
name+args (../RESEARCH_NOTES.md), and keeps the annotation feature purely
declarative metadata, not a second type-checked calling-convention
layer.

## 11. v1 feature additions (this spec pass)

The small C-style conveniences added in the completeness pass
(`../planning/GAP_ANALYSIS.md` Section 5). Each is cheap and C-expected; semantics here
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
  `constexpr fn`s, `static_assert`) is v2 (`../ROADMAP.md` Tier 1).

### 11.6 `do-while` / `switch` / `defer` (semantics, grammar in
COMPILER_PIPELINE.md Section 2)

- `do { ... } while (cond);` - body runs at least once, condition
  checked at the bottom. Same bool-operand rule as `if`/`while` (`cond`
  must be `bool`). Budget check placed at the back-edge to the top of
  the body, same as `while` (SAFETY_AND_SANDBOX.md Section 3).
- `switch (e) { case L: ...; break; ... }` - `e` integer-typed, `L`
  constexpr integer literals, unique within the switch, in-range for
  `e`'s type. **No fallthrough**: each case must end in
  `break` or `return` (sema rejects falling off a case
  body - eliminates the entire C-fallthrough footgun class at the
  grammar level, ../planning/GAP_ANALYSIS.md Section 5). `default` optional. Lowering
  and jump-table vs cascade decision in CODEGEN_SPEC.md Section 12.
- `defer expr;` - the shipped v1 implementation is **lexical-block-exit LIFO**;
  deferred expressions run in reverse declaration/reach order on block
  fallthrough, `break`, `continue`, `return`, and ordinary function return.
  A defer inside a loop body runs on every reached iteration (reset-on-entry);
  trap/`longjmp` bypass is unchanged (no exception unwinding through JIT
  frames). See `CODEGEN_SPEC.md` Section 13 for the full emission contract.

### 11.7 String interpolation `f"...{expr}..."`

The parser wraps every literal/hole segment in the internal
`__fstring_to_string` sentinel. During sema that sentinel is replaced by a
real registered string conversion: `string_from_slice` for literal `u8[]`
segments, `string_from_i64`, `string_from_f32`, `string_from_f64`, or
`string_from_bool` for scalar holes, and `string_identity` for an existing
string handle. Segments are combined through the string extension's `+`
overload. `__fstring_to_string` is an internal lowering marker, not a native;
there is no `__fmt` API. The result is the host-owned opaque `string` handle.

## 12. First-class struct / aggregate pass (2026-07-10)

Four features shipped together to remove the biggest ergonomic blocker to
writing real ember programs (the Win64 hidden-pointer struct ABI restrictions
that forced every struct through a named local, plus the missing array-literal
and aggregate-global surface). Each is a real codegen change to the struct
ABI / globals surface, not a parser tweak; each is pinned by a non-circular
regression (would fail with the fix reverted) and documented in
`CODEGEN_SPEC.md` §16. This section formalizes the type-system half; the
codegen half is in `CODEGEN_SPEC.md` §16.

### 12.1 Array literals `[a, b, c]`

An `ArrayLit` is a first-class expression: `let arr: i64[3] = [10, 20, 30];`
(fixed array) and `let s: i64[] = [1, 2, 3];` (slice — the literal allocates a
backing store and yields `{ptr, len}`). It is checked against the **declared
target type** (`expected`), exactly as a struct literal is checked against its
declared struct type:

- **Declared-type required (no inference).** A bare `let x = [1, 2, 3];` with
  no annotation is a **compile error** ("needs an explicit type annotation").
  v1 does not infer the element type or the array/slice kind from the literal —
  the target type must be present. This matches the no-bidirectional-inference
  stance of §9 (`auto` is the one inference rule, and `auto` here would infer
  the initializer's type, which for an ArrayLit is the *baked* type after sema,
  so `let x: i64[3] = [1,2,3]` is the canonical form; a future refinement could
  let `auto x = [1,2,3]` infer from element types, but that is additive, not
  required for the feature to be useful).
- **Fixed-array construction** (`T[N]`): requires **exactly N** elements. A
  count mismatch (`let arr: i64[3] = [1, 2]`) is a compile error
  ("array literal has 2 elements, expected 3"). `N == 0` fixed-array literals
  are unreachable here (a zero-length fixed array is itself a §3 compile error);
  the empty literal is handled by the slice rule below.
- **Slice construction** (`T[]`): accepts **any count** `>= 0`. The literal
  allocates a backing store of `count` elements and yields the two-word slice
  `{&backing[0], count}`. An **empty slice literal `[]` is a compile error**
  ("empty array literal needs an explicit type annotation") — without a
  declared element type there is nothing to type-check the (absent) elements
  against, and v1 does not infer the element type from an empty literal. A
  zero-length slice must instead be produced by a native or by slicing a
  zero-length fixed array (§4). (A future refinement could allow `let s: i64[]
  = []` once the declared `T[]` is recognized as carrying the element type `T`
  with no elements to check; additive, not required.)
- **Element type checking.** Each element is checked against the declared
  element type via the ordinary value-check path, so the existing int-literal
  coercion applies: `let arr: i32[3] = [1, 2, 3]` adapts the `i32`-typed int
  literals into the `i32` slots, and `let s: f32[] = [1.0, 2.0]` is fine. A
  type mismatch (`let arr: i64[3] = [1, 2, 3.0]` — float element in an int
  array) is a compile error.
- **Where an ArrayLit may appear.** A fixed-array ArrayLit is **only** valid as
  a `let`'s direct initializer (the let-init position), because v1's call/return
  convention does not carry fixed arrays by value (`words_for_type` returns 1
  for a fixed array, which would truncate the array to a single word — a
  fixed-array ArrayLit passed as a call arg or returned would miscompile). A
  **slice** ArrayLit, by contrast, is fine as an arg or a return (it yields the
  two-word `{ptr, len}` ABI, which the existing slice-arg / slice-return paths
  handle). This is a v1 call-convention limit on fixed arrays, not a new
  restriction — it's the same `words_for_type` limit §3's "fixed arrays are value
types" note already documents (a >8-byte value is passed by hidden pointer;
  fixed arrays specifically are not wired through that path in v1). A
  fixed-array ArrayLit reaching a general expression position (arg/return) is a
  **compile error** ("fixed-array array literal must be a let initializer").
- **Disambiguation with the index/view operator.** `[` at **primary** position
  constructs an ArrayLit; the existing `[` in the postfix position is the
  index/view operator and only fires *after* a primary is parsed. So `arr[0]`
  still indexes, `[1, 2, 3]` at an expression start constructs a literal, and
  `[1, 2, 3][0]` is an ArrayLit primary followed by a postfix index. `[i]` at
  primary position is a 1-element ArrayLit (not an index) — the intended
  disambiguation. An unclosed `[1, 2, 3;` produces a clean "expected `]`" parse
  error.

### 12.2 Aggregate globals (struct / array / slice)

A `struct`, fixed-`array`, or `slice` typed global now type-checks,
initializes, loads, and stores. This lifts the audit-M11 rejection that
limited globals to scalar initializers and required aggregates to be
initialized in a fn (`v3_up()`, `make_config()`).

- **Typed global-table layout.** The globals block is no longer a flat array of
  8-byte scalar slots; the per-global table carries an **offset and size** per
  global, with **8-byte alignment** for every slot. A scalar global occupies 8
  bytes at its offset (unchanged from v1). A **slice** global occupies **16
  bytes** — the two-word `{ptr, len}` representation of §4 — at its offset. A
  **struct** global occupies its tightly-packed Ember size (§2) at its offset
  (script-declared struct: packed field order; host-mapped struct: the host's
  supplied `sizeof`). A **fixed-array** global occupies `N * sizeof(T)` at its
  offset. Global load/store addresses the slot at `[globals_base + offset]`
  (the typed per-global offset), not the old `[globals_base + i*8]` flat-index
  form. (See `CODEGEN_SPEC.md` §16 for the codegen details; this section covers
  the type-system / layout contract.)
- **Const-initializer-folding rule.** A global's initializer is evaluated at
  **load time** by the host's `eval_global_initializers`, which folds the
  initializer into the typed globals block. A **struct** global initializer is
  folded field-by-field; a **fixed-array** global initializer element-by-element;
  a **slice** global initializer materializes the backing store and folds
  `{ptr, len}`. The folding rule is: a **non-const** field/element initializer
  (one that is not a literal or a constexpr-foldable expression) folds to
  **zero** for that field/element, and the rest fold normally. Concretely,
  `global cfg : Config = Config { name_id: 42, scale: 0.0f };` bakes `name_id
  = 42` and `scale = 0.0f` into the struct global's slot; a field whose
  initializer is a call or a runtime expression folds to zero. This is the
  same const-fold restriction the v1 scalar-globals path already enforced
  (§11.5: v1 restricts `constexpr` to literals, `sizeof`/`offsetof`, and
  integer arithmetic on other `constexpr` values), extended to aggregate
  fields/elements. A handle-typed global (e.g. a sync `spsc` handle) still
  needs a call initializer for the type-check and still starts at 0 until
  `@entry` reassigns it (the aggregate-globals pass did not add a handle
  literal — see `../../demo/concurrency/NOTES.md` K4 for that adjacent, still-open
  shape).
- **`.em` round-trip behavior.** A struct or fixed-array global round-trips
  through `.em` serialize → load → run identically: the typed globals block is
  serialized with its per-global offsets/sizes, the const-folded initializer is
  baked into the block, and the loaded module's globals block is laid out at
  the same offsets so a `[globals_base + offset]` load/store in the loaded code
  reads the same field. A **slice** global's 16-byte `{ptr, len}` carries a
  **relative pointer** into the module's own rodata/globals backing; on `.em`
  load the loader **relocates** that relative pointer to the loaded module's
  own backing store (the `EmReloc` kind for a slice-global's ptr field), so a
  slice global's `ptr` is valid in the loaded process. This is the slice `.em`
  relocation path. **Gap note:** if c3 flagged any slice-`.em`-relocation edge
  as a gap, it is recorded in `aggregate_global_test` as probe [8] (slice global
  `.em` round-trip with relative-ptr relocation) — that probe is green, so the
  relocation is verified; no open gap was flagged for the slice `.em` path in
  c3's final report. (A future host that loads a `.em` into a process whose
  address space differs from the serializer's would still need the standard
  relative-ptr relocation the loader already performs; no further gap.)
- **By-value use of an aggregate global.** A struct global may be passed by
  value as an arg (`useit(cfg)`) and returned by value (`getit().name_id`),
  copying from `[globals_base + offset]` — the same struct-by-value machinery
  c1 relaxed for local struct args/returns, now reading from the typed global
  slot instead of a local frame slot. This is the c1↔c3 interaction: c1 made a
  general-expr struct arg work (materializing a temp); c3 made a struct global
  readable as that arg's source. Pinned by `aggregate_global_test` probes [4]
  (struct global by-value arg) and [5] (struct global by-value return).
