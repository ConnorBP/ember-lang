# Type Detection Stress Review — 2026-07-11

> **Scope.** A full-suite stress test of ember's auto type detection over the
> complete usage chain (declaration → assignment → binary op → arg passing →
> return → cast → array index → match pattern → native call), exercising every
> integer width/signedness (i8/i16/i32/i64/u8/u16/u32/u64), typed enums
> (`enum E : T`), floats (f32/f64), constexpr fn folding, and `let` inference.
> The suite was commissioned specifically to find places where enum and integer
> values "fail to upgrade or downgrade to the correct usage."

## Artifacts

| artifact | role |
|---|---|
| `tests/lang/valid_type_stress.ember` | positive corpus — 12 blocks, runs `main` → `123`, exercises the full chain for every standard (should-work) case |
| `tests/lang/sema_invalid_type_narrow.ember` | negative corpus — 44 fns, 42 sema errors: every narrowing / signedness-change / int↔float / int→enum / out-of-matrix cast / non-int index / int-as-bool the spec rejects |
| `tests/lang/sema_invalid_type_edge.ember` | negative corpus — 12 fns, 11 sema errors: compound-assign, ternary, shift-on-non-int, enum+int binary, logical-on-int, bitwise-on-float corners |
| `examples/type_stress_test.cpp` | ctest — 55 programmatic probes (40 positive value-assertions + 15 sema-rejection assertions) through the full parse→sema→codegen→finalize→call pipeline |
| `tests/run_lang_tests.sh` | registers `valid_type_stress.ember:123` in the executable expected-rc list (the two `sema_invalid_type_*` files are auto-classified by the existing `sema_invalid_*` globs) |
| `CMakeLists.txt` | registers `type_stress_test` → `add_test(NAME type_stress ...)` |

All 41 ctest tests pass (40 prior + `type_stress`); the 40/40 baseline is preserved.

## Bugs found and fixed

The suite was built to find type-detection bugs. Three were found; all three
were real spec-vs-implementation divergences, all three were fixed in
`src/sema.cpp`, and all three are pinned by regression probes in
`examples/type_stress_test.cpp` (groups 11, 13, 14) plus the `.ember` files.

### B1 — `f32 → f64` implicit widening was rejected (spec requires it)

**Symptom.** `let y: f64 = x;` where `x: f32` produced `let type mismatch
(f64 = f32)`. The same applied at the return and arg-passing sites.

**Spec.** TYPE_SYSTEM.md §6 lists `f32->f64` as the one lossless implicit float
widening (alongside the integer widening chains). The implementation's
`can_implicitly_convert` only handled plain integers and enum→int — it had no
float-widening case at all, so `f32 → f64` fell through to the same-type
check and failed.

**Fix.** Added a float-widening arm to `can_implicitly_convert` (f32→f64 only;
f64→f32 stays explicit because it is narrowing; int↔float stays never-implicit).
Codegen already emitted `cvtss2sd` for the explicit `f32 as f64` path, so the
synthesized CastExpr uses the identical instruction — the fix is purely the
sema gate. Pinned by `[11a]`–`[11c]` (positive) and `[11d]` (f64→f32 still
rejected).

### B2 — typed-enum variant literal silently narrowed / changed signedness

**Symptom.** `let x: i16 = Color::Red;` (Color is i32-backed) and `let x: u8 =
Color::Red;` both **compiled and ran**, discarding the enum's backing-width and
signedness constraint. The same happened for enum variant literals inside an
array literal: `let a: i16[2] = [Color::Red, Color::Green];` compiled. An
enum *variable* (`let c: Color = Color::Red; let x: i16 = c;`) was correctly
rejected — so the behavior was inconsistent between a variant literal and a
variable of the same enum type.

**Root cause.** `adapt_int_lit` re-types an `IntLit` to a target integer type
when the raw value fits, but it only guarded the *target* being a typed enum
(`if (!target->enum_name.empty()) return;`). It did not guard the *source*
literal already carrying a typed-enum type. A variant literal `Color::Red` is
rewritten by `lower_enum_access_expr` to an `IntLit` with `ty = Color` (prim
i32, enum_name "Color"); when that literal flowed to an `i16` or `u8` target,
`adapt_int_lit` re-typed it by raw value fit (0 fits i16; 0 fits u8),
discarding the enum's i32 backing and the signed/unsigned distinction.

**Fix.** Added a symmetric guard at the top of `adapt_int_lit`: if the
literal's current type already carries a typed-enum tag
(`lit.ty && !lit.ty->enum_name.empty()`), do not re-type it. The value then
flows through `check_value`'s enum→int widening gate
(`can_implicitly_convert`: backing width ≤ target width AND same signedness),
so a widening target (e.g. `let x: i64 = Color::Red`) still accepts via a
synthesized enum→int CastExpr, and a narrowing / signedness-change target
correctly errors with `let type mismatch (i16 = Color)`. Codegen already
handled the enum→int CastExpr via the same `normalize_rax` path a plain int
cast uses. Pinned by `[13a]`–`[13c]` (now rejected) and `[13d]` (widening still
works).

### B3 — binary bitwise (`& | ^`) on floats was silently accepted

**Symptom.** `let c = a & b;` where `a, b: f32` compiled and ran without
error. Codegen emitted an integer `and` over the float bit patterns in `rax`
— a value-wrong, no-trap miscompile. The unary form `~x` on a float was
already correctly rejected (`sema_invalid_bitnot_non_int.ember`); only the
binary forms were missing the integer-only check.

**Spec.** TYPE_SYSTEM.md §7: "Bitwise (`& | ^ ~ << >>`) only defined on
integer types." The binary-op sema path's general "arithmetic/bitwise" branch
only checked `is_int() || is_float()` (the numeric guard), with no
bitwise-specific integer requirement, so floats slipped through.

**Fix.** Added an integer-only check for the bitwise binary ops
(`And | Or | Xor`) before the numeric guard: if the (already same-typed)
operand type is not integer, error `bitwise operator requires integer
operands`. Arithmetic (`+ - * / %`) on floats is unaffected (it still reaches
the `is_float()` branch). Pinned by `[14a]`–`[14c]` (now rejected) and
`[14d]`/`[14e]` (int bitwise and float arithmetic still work).

## Matrix results (post-fix, all correct)

### Integer assignment (variable → variable), 8×8

`let x: <from> = 5; let y: <to> = x;` — `.` = compiles (widening / same-type),
`X` = sema error (narrowing / signedness change). Columns are the target width
(signed then unsigned).

```
       i8 i16 i32 i64 u8 u16 u32 u64
i8      .   .   .   .   X   X   X   X
i16     X   .   .   .   X   X   X   X
i32     X   X   .   .   X   X   X   X
i64     X   X   X   .   X   X   X   X
u8      X   X   X   X   .   .   .   .
u16     X   X   X   X   X   .   .   .
u32     X   X   X   X   X   X   .   .
u64     X   X   X   X   X   X   X   .
```

Same-signedness widening (`i8→i16→i32→i64`, `u8→u16→u32→u64`) and same-type
compile; narrowing and same-width signedness flips are sema errors. Matches
§6 exactly.

### Integer literal adaptation (`let x: <T> = 5;`)

A raw integer literal adapts to any integer target whose range the value fits
(`adapt_int_lit`): `let x: u8 = 5` ✓, `let x: u8 = 300` ✗ (300 stays i64,
i64→u8 signedness/width mismatch), `let x: i8 = 5` ✓, `let x: i8 = 200` ✗.
This is the literal-contextual-adaptation rule (§6) and is distinct from the
variable widening matrix above — a literal is re-typed by value fit; a
variable obeys widening-only.

### Cast matrix (explicit `as`)

| from → to | result |
|---|---|
| int → int (any width/signedness) | ✓ (truncate / sign-extend / zero-extend) |
| `256 as u8` | ✓ → 0 (truncation) |
| `-1 as u8` | ✓ → 255 (sign + trunc) |
| f32 ↔ f64 | ✓ (`cvtss2sd` / `cvtsd2ss`) |
| signed int ↔ float | ✓ (`cvtsi2sd` / `cvttsd2si`, truncates toward zero) |
| **unsigned int → float** (u8/u16/u32/u64 → f32/f64) | ✗ out of v1 matrix |
| **float → unsigned int** (f32/f64 → u8/u16/u32/u64) | ✗ out of v1 matrix |
| enum → int | ✓ (explicit spelling of the implicit widening) |
| int → enum | ✗ (not in matrix; int→enum is never available, implicit or explicit) |

### Binary ops (result type + literal adaptation)

| operands | result |
|---|---|
| same-type int / float arithmetic | ✓ result is that type |
| literal + variable (`5 + u64var`) | ✓ literal adapts to the variable's type |
| i32 + i64 (mixed width) | ✗ `operator requires same-type operands` |
| u32 + i32 (mixed signedness) | ✗ same |
| f32 + f64 (mixed float) | ✗ same |
| i32 == i64 (mixed cmp) | ✗ `comparison requires same-type operands` |
| typed enum == plain int | ✓ (the §6.3 enum↔int comparison allowance) |
| typed enum == different typed enum | ✗ (`Color == Hue` rejected) |
| same typed enum + same typed enum | ✓ result is the enum type |
| shift rhs any integer (signed or unsigned) | ✓ (§7 relaxation; signed shift amounts accepted) |
| shift on float | ✗ `shift requires integer operands` |
| **float & float / \| / ^** | ✗ `bitwise operator requires integer operands` (B3 fix) |
| int & / \| / ^ | ✓ |
| float + - * / | ✓ (B3 fix does not over-reach) |

### Return / arg-passing (widening gate)

Same widening/narrowing/signedness rules as assignment, applied via
`check_value` + `types_compatible` at both the `ReturnStmt` and `CallExpr`
arg sites. `i32 → i64` return ✓, `i64 → i32` return ✗, `u8 → i64` arg ✗
(signedness), literal `300 → u8` param ✗ (out of range). All correct.

### Array index

`arr[i]` requires `i` integer (any width, signed or unsigned). `arr[u8]`,
`arr[i8]`, `arr[u32]`, `arr[i64]`, `arr[typed-enum-variable]`,
`arr[enum-variant-literal]` all ✓ (a typed enum `is_int()` because
`Type::is_int()` excludes only `struct_name`, not `enum_name`). `arr[f32]` ✗,
`arr[bool]` ✗. Matches §7's index rule.

### Match pattern

Pattern must be same type as subject OR both integer (`pt->is_int() &&
subj_ty->is_int()`). `match (c: Color) { Color::Green => ... }` ✓,
`match (n: i64) { 5 => ... }` ✓ (both int), `match (c: Color) { 0 => ... }` ✓
(enum subject, int pattern, both int), `match (m: i32) { Color::Red => ... }`
✓ (int subject, enum pattern, both int). A pattern must be a bare `IntLit` /
`BoolLit` — a constant expression like `-1` (a `UnaryExpr::Neg` of a literal)
is rejected as `match pattern must be a literal constant` (a known v1
limitation, not a type bug).

### Enum ↔ integer (the design rule)

| form | result |
|---|---|
| `enum E : i32` → `i64` (widening) | ✓ implicit |
| `enum E : i32` → `i32` (backing) | ✓ implicit (width-equal, same signedness) |
| `enum E : i32` → `i16` (narrowing) | ✗ (variable and variant literal both, post-B2) |
| `enum E : i32` → `u8` (signedness) | ✗ (both, post-B2) |
| `enum E : u8` → `u8` | ✓ |
| `enum E : u8` → `i64` | ✗ (u8 unsigned, i64 signed — signedness change) |
| `enum E : i64` large value (`1 << 40`) | ✓ |
| int → enum (`let c: Color = 5`) | ✗ |
| int → enum via `as` (`0 as Color`) | ✗ (not in cast matrix) |
| different typed enum (`Color = Hue::A`) | ✗ (nominal typing) |
| enum variant in array literal, widening target | ✓ |
| enum variant in array literal, narrowing target | ✗ (post-B2) |

### Constexpr + type interactions

| form | result |
|---|---|
| `constexpr fn f() -> i32` used as `i64` (fold + widen) | ✓ |
| `constexpr fn f() -> i32` used in `i64` expression | ✓ |
| `constexpr fn gc() -> Color` used as `i64` | ✓ (fold to enum literal, widen) |
| `constexpr fn f() -> i32` → `let c: Color` (int→enum via fold) | ✗ (int→enum rejected even via fold) |
| `constexpr fn f() -> i64` → `i32` param | ✓ — **intentional leniency**: the constexpr call folds to an `IntLit(5)`, which then adapts to `i32` via `adapt_int_lit` (5 fits i32). This is the literal-contextual-adaptation rule inheriting through the fold, NOT a widening-rule bypass. A non-constexpr `i64` value → `i32` param is still correctly rejected. Documented as a subtle interaction, not a bug. |
| `static_assert(Color::Red == 0, ...)` | ✓ (enum in constexpr context) |
| `static_assert(cexpr_color() == Color::Green, ...)` | ✓ |
| `static_assert(cexpr_color() as i64 == 1, ...)` | ✗ `static_assert condition must be a compile-time constant` — the constexpr folder does not fold through a `CastExpr` on a typed-enum literal. A known v1 limitation (the constexpr const-check is `try_eval_const_i64`, which does not descend through enum→int casts); the runtime widening is covered separately. Not a type-detection bug. |

### Float widening/narrowing (post-B1)

| form | result |
|---|---|
| `f32 → f64` (assign / return / arg / array-literal) | ✓ implicit (B1 fix) |
| `f64 → f32` | ✗ (narrowing, explicit-only) |
| `int var → f32` | ✗ (int→float never implicit) |
| `f32 literal → i64` | ✗ (float→int never implicit) |
| `f32 + f64` | ✗ (mixed-type) |

### Inference (`let x = expr;`)

| literal | inferred |
|---|---|
| `5` | `i64` |
| `5.0f` | `f32` (f suffix authoritative) |
| `5.0` | `f64` |
| `true` | `bool` |
| `Color::Red` (typed enum) | `Color` |
| `-5` | `i64` (unary neg of i64 literal) |

Integer-width suffixes (`5u32`, `5i64`) are not supported (parse error, PF-D3);
use an explicit `as` cast.

## Spec-vs-implementation divergences noted but NOT changed

These are cases where the implementation is more lenient than the spec text.
They were left as-is because tightening them could break existing code and the
leniency is in the safe/enient direction (no silent miscompile).

- **Shift rhs signedness.** TYPE_SYSTEM.md §7 says the shift rhs "may be any
  unsigned integer type." The implementation accepts any integer type
  (signed or unsigned) — `i32 << i64` and `u32 >> i8` compile. A signed shift
  amount is value-equivalent to an unsigned one for the in-range case, and an
  out-of-range signed amount (negative) is masked by `& 63` at codegen, same
  as a huge unsigned amount. No silent miscompile; documented divergence.
- **Constexpr-fold narrowing at call site.** See the constexpr table above —
  a constexpr `i64` result folded to a literal adapts to an `i32` param. This
  is literal adaptation inheriting through the fold, not a widening bypass.

## Validation

- `ctest -E bench --timeout 30` → **41/41 pass** (40 prior + `type_stress`).
- `examples/type_stress_test.exe` → **55/55 probes pass** (40 positive
  value-assertions + 15 sema-rejection assertions).
- `tests/lang/valid_type_stress.ember` → parses, semas, runs `main` → `123`.
- `tests/lang/sema_invalid_type_narrow.ember` → parses, **42 sema errors**
  (every expected rejection fires).
- `tests/lang/sema_invalid_type_edge.ember` → parses, **11 sema errors**.
- `ember_cli test tests/lang` classifies all three new files `ok`.
- The 40/40 baseline was re-confirmed after each of the three sema fixes
  (B1/B2/B3) — no existing test regressed.
