# ember - gap analysis & completeness audit

This pass re-audits the spec against (a) the original request and (b)
the now-fully-known feature surface of the surveyed native-JIT language (RESEARCH_NOTES.md updated).
Goal: confirm nothing the user actually asked for is missing, and
make every deferral an explicit, justified decision rather than an
oversight.

## 1. Original-request requirements → where satisfied

| Requirement (paraphrased from original request) | Status | Where |
|---|---|---|
| C-style scripting language | ✓ v1 | `COMPILER_PIPELINE.md` Section 2 grammar (C-family syntax, C precedence) |
| Compiles to **composable actual native x86-64 chunks** | ✓ v1 | `CODEGEN_SPEC.md` (per-function JIT into exec-memory arena, dispatch-table composition) |
| Bundling / pre-compile to a loadable module | ✓ v0.1 (added) | `BUNDLING_AND_EM_MODULES.md`, `CODEGEN_SPEC.md` Section 15 test 7  -  the `.em` format (`em_file`/`em_writer`/`em_loader`); JIT → serialize → load → run round-trip proven |
| Fairly safe | ✓ v1 | `SAFETY_AND_SANDBOX.md` (budgets, bounds, depth, `PERM_FFI`, non-local unwind) |
| Extremely fast (JIT, not bytecode) | ✓ v1 | `CODEGEN_SPEC.md` (baseline JIT, no interpreter); honest caveat in Section 3 below re: "MUCH faster than AngelScript" |
| Expose scripting API bindings like AngelScript | ✓ v1 | `BINDING_API.md` (`TypeBuilder`, `NativeFn`, descriptor style) |
| For game engine + game modding | ⚠ partial | core JIT+bindings v1; modding-relevant data structures (arrays/maps/strings) deferred  -  see Section 3 |
| "AngelScript but MUCH faster" | ✓ expected | baseline JIT vs AS's bytecode interpreter (RESEARCH_NOTES.md)  -  see Section 3 caveat |
| "(sort of like a native-JIT scripting language)" | ✓ scoping | the surveyed native-JIT language's *embedding API* mirrored; its *language breadth* deliberately scoped down  -  `ROADMAP.md` |
| Hot reload | ✓ v1 (single fn) / v2 (whole-module workflow) | `HOT_RELOAD.md` |
| GC | deferred v1 (arena/host-owned), v2 plan | `MEMORY_AND_GC.md` Section 8, `ROADMAP.md` |

No original-request requirement is unaddressed. The one ⚠
(game-modding data structures) is the real gap and is addressed in
Section 3 below + `ROADMAP.md`.

## 2. Surveyed native-JIT language feature surface → v1 decision per feature

| surveyed-language feature | ember v1 | Rationale / where |
|---|---|---|
| Primitives (bool/i*/u*/f*) | ✓ v1 | `TYPE_SYSTEM.md` Section 1 |
| `char`/`wchar` | ✗ v1 | byte slices (`slice<u8>`) instead, `TYPE_SYSTEM.md` Section 1 |
| `aint*` (atomic ints) | ✗ v1 | no threading in v1, `SAFETY_AND_SANDBOX.md` Section 8 |
| `null` / `nullable` | ✗ v1 | no pointers; optional via i64 handle w/ 0=invalid idiom, `BINDING_API.md` Section 7 |
| `const` / `constexpr` | ✓ v1 (added this pass) | `TYPE_SYSTEM.md` Section 11, `COMPILER_PIPELINE.md` Section 2 |
| `auto` inference | ✓ v1 | `TYPE_SYSTEM.md` Section 9 |
| Ternary `?:` | ✓ v1 (added this pass) | `COMPILER_PIPELINE.md` Section 2, `TYPE_SYSTEM.md` Section 11 |
| `++` / `--` | ✓ v1 (added this pass) | `TYPE_SYSTEM.md` Section 11 |
| `cast<T>()` | ✗ v1 (diverged) | ember uses `expr as T` (C/Rust style); equivalent power, `TYPE_SYSTEM.md` Section 6 |
| `sizeof` / `offsetof` | ✓ v1 (added this pass) | `TYPE_SYSTEM.md` Section 11 |
| Compound assign (`+=` etc.) | ✓ v1 (semantics added this pass) | `TYPE_SYSTEM.md` Section 11 |
| `if`/`else`/`while`/`for`/`break`/`continue`/`return` | ✓ v1 | `COMPILER_PIPELINE.md` Section 2 |
| `do-while` | ✓ v1 (added this pass) | `COMPILER_PIPELINE.md` Section 2 |
| `for-each` | ✗ v1 | needs iterable protocol; add with `iterable` TypeBuilder hook in v2, `ROADMAP.md` |
| `switch` | ✓ v1 (added this pass) | `COMPILER_PIPELINE.md` Section 2, `CODEGEN_SPEC.md` Section 13 |
| `match` (pattern) | ✗ v1 | `switch` covers v1; pattern match is v2+, `ROADMAP.md` |
| `goto` | ✗ v1 (deliberate) | structured control flow only; goto complicates liveness/scope, no need |
| `defer` | ✓ v1 (added this pass) | `CODEGEN_SPEC.md` Section 14, `COMPILER_PIPELINE.md` Section 6 |
| Default args | ✗ v1 | host registers overloads or takes slice; `BINDING_API.md` Section 2 |
| Reference `&` / `out` params | ✗ v1 (idiom instead) | pass `slice<T>` of len 1, `TYPE_SYSTEM.md` Section 5 |
| Variadic fns | ✗ v1 | `BINDING_API.md` Section 2 (fixed arity; variadic via slice) |
| `extern` | ✗ v1 | FFI via `PERM_FFI`-gated natives, not extern decls |
| Function references / `cast(fn)` | ✗ v1 (deliberate) | host-side name/slot lookup instead (HOT_RELOAD.md Section 7); dynamic registration is v2, `ROADMAP.md` |
| Lambdas | ✗ v1 | `DESIGN.md` non-goal; v2+ with GC, `ROADMAP.md` |
| Dynamic arrays / maps / strings (rich) | ✗ v1 builtin | exposed as **native addons** on host-owned memory, Section 3 below |
| Structs (ctors/dtor/methods/op-overload/bitfield/packed) | partial v1 | fields/methods/properties/op-overload ✓; ctor via named literal; dtor/bitfield/packed ✗, `ROADMAP.md` |
| Classes / inheritance / vtables | ✗ v1 | nominal structs + static dispatch; v2+, `ROADMAP.md` |
| Interfaces / mixins | ✗ v1 | v2+ |
| Properties (get/set) | ✓ v1 | `BINDING_API.md` Section 3 |
| Templates / monomorphization | ✗ v1 | `DESIGN.md` non-goal; v2+, `ROADMAP.md` |
| Enums | ✗ v1 | named i32 globals; v2 grammar, `BINDING_API.md` Section 5 |
| Typedefs / `using` | ✗ v1 | YAGNI |
| Delegates | ✗ v1 | needs function refs (v2) |
| Namespaces | ✗ v1 | flat module scope; v2+ |
| Coroutines / `yield` | ✗ v1 | v2+, needs suspended-frame storage, `ROADMAP.md` |
| Exceptions try/catch/throw | ✗ v1 | `runtime_error`/`runtime_exception` host-signal instead, `SAFETY_AND_SANDBOX.md` Section 7; v2+ |
| Heap `new`/`delete` | ✗ v1 | arena-only, `MEMORY_AND_GC.md`; GC v2+ |
| Inline asm intrinsics | ✗ v1 | `PERM_FFI`-gated native can wrap `__rdtsc` etc.; v2 if real need |
| `[[annotations]]` | ✓ v1 (`@` syntax) | `TYPE_SYSTEM.md` Section 10 |
| Modules / `import` | ✗ v1 (single module) | v2+, `ROADMAP.md` |
| Preprocessor | ✗ v1 (deliberate) | `engine.define(name,value)` for compile-time defines instead, `DESIGN.md` Section 8 |
| FFI `[[dll(...)]]` | ✓ v1 (different mechanism) | `PERM_FFI`-gated `NativeFn`, `SAFETY_AND_SANDBOX.md` Section 6 |
| `static_assert` | ✗ v1 | YAGNI; add with const-expr eval in v2 |
| Lifecycle `main()` + routines | ✓ v1 (equivalent) | `LIFECYCLE.md` (new this pass)  -  annotation-based, same semantics |

## 3. The one real gap: modding-relevant data structures

The original request's use case is **game modding**. Useful mods need
dynamic arrays, maps, and string manipulation. v1 has **no builtins**
for these - deliberate (no script-managed heap, `MEMORY_AND_GC.md` Section 1).

**Resolution (already consistent with the spec, now made explicit):**
these are **native addons**, not language builtins. The host registers
`NativeFn`s operating on host-owned memory the host lifetime-manages,
exactly like the surveyed native-JIT language's addons (RESEARCH_NOTES.md) and AngelScript's
`scriptarray`/`scriptdictionary`/`scriptstdstring` add_on libraries
(surveyed). Concretely, a v1.0 ship includes a small **standard addon
set** (host C++ side, not in the JIT):

- `array<T>` - exposed as a host-allocated buffer + length handle
  (i64), with `push`/`get`/`set`/`len`/`remove` natives. Script holds
  the i64 handle, never the raw buffer - fits the host-owned-memory
  ownership category (`MEMORY_AND_GC.md` Section 1 category 2) perfectly,
  zero GC needed, bounds-checked inside the natives
  (`SAFETY_AND_SANDBOX.md` Section 1 - natives are trusted, but the addon
  implements its own internal bounds check on every indexing native).
- `map<K,V>` - same pattern, opaque i64 handle.
- `string` - a `slice<u8>` view is the *literal* representation
  (`TYPE_SYSTEM.md` Section 6, rodata); a *mutable growable* string is the
  `array<u8>` addon. String interpolation `f"v={x}"` (added this pass,
  `TYPE_SYSTEM.md` Section 11) produces a fresh `array<u8>` via the addon's
  format native.

This keeps the language core small (YAGNI) while making mods writable.
`ROADMAP.md` lists the addon set as a v1.0 deliverable, not a v2
language feature - the distinction matters: addons are *host C++ code
using the stable `NativeFn` API*, they don't expand the JIT or the
type system.

Note: the `.em` bundling format shipped in v0.1 (`em_file`/`em_writer`/`em_loader`, `CODEGEN_SPEC.md` Section 15 test 7) provides a single-module pre-compile + load path. Its name-table and dispatch-table-reloc infrastructure is a foundation for the Tier 6 live-`import` modules work (`ROADMAP.md`), but it does not satisfy that - `.em` is single-module, no live multi-module linking. The modding-data-structure gap (arrays/maps/strings as native addons) is unchanged: still a v1.0 Tier 0 deliverable, not a language feature.

## 4. Honest performance caveat ("MUCH faster than AngelScript")

The original request says "AngelScript but MUCH faster (sort of like
a native-JIT scripting language)." ember v1 is a **baseline** JIT (linear-scan regalloc, no opt
passes beyond peephole, no inlining, no loop opts) - `CODEGEN_SPEC.md`
Section 3/Section 5/Section 10. AngelScript is a bytecode interpreter. Even baseline native
code beats a bytecode interpreter on tight loops by typically 5-50×
(one dispatch-decode loop removed, real registers vs a virtual stack),
which comfortably satisfies "MUCH faster" for the hot-game-logic target.

**An optimizing native-JIT language is faster than ember v1 will be**, because such a language runs an
"optimizing pipeline" (per its docs, RESEARCH_NOTES.md updated). ember
v1 will *not* match an optimizing native-JIT language's speed - only its *category*
(native, not bytecode). Matching an optimizing native-JIT language's speed is a v2+ goal
(after the v0.5 benchmark harness exists to prove where ember is slow
and justify adding opt passes, `DESIGN.md` Section 9 - no speculative
optimization). This is stated honestly here rather than oversold in
the README.

## 5. Features added to v1 in this spec pass (concrete)

Each is small, C-expected, and cheap to specify/implement. Detail
patches landed in the noted docs:

- **Ternary `?:`** - `COMPILER_PIPELINE.md` Section 2 grammar, `TYPE_SYSTEM.md`
  Section 11 (result type = common type of branches, both must be
  same-type per the no-implicit-promotion rule, Section 6).
- **`++`/`--`** (prefix+postfix) - `TYPE_SYSTEM.md` Section 11 (integer
  operands only, returns the new/old value per prefix/postfix, checked
  overflow in debug like `+=`).
- **`switch`** - `COMPILER_PIPELINE.md` Section 2, `CODEGEN_SPEC.md` Section 13
  (integer/enum-ident scrutinee only; jump-table if dense, else
  cmp/je cascade; `break`/fallthrough-via-`goto`? - v1: **no fallthrough**,
  each case must `break`/`return`/`trap`, eliminates the entire
  C-fallthrough footgun class at the grammar level).
- **`do-while`** - `COMPILER_PIPELINE.md` Section 2 (lowered like `while`
  with the condition check at the bottom).
- **`const`/`constexpr`** - `TYPE_SYSTEM.md` Section 11 (`const` = immutable
  local; `constexpr` = compile-time-evaluable, usable as array sizes
  and case labels; v1 `constexpr` is restricted to literal-and-`sizeof`/
  `offsetof` expressions only, no full compile-time fn eval - that's
  v2 `static_assert` territory).
- **`defer`** - `CODEGEN_SPEC.md` Section 14, `COMPILER_PIPELINE.md` Section 6
  (scope-exit cleanup; lowering maintains a per-block deferred-action
  stack, emits them at every scope exit path including `return`/
  `break`/`continue`/trap-unwind - note: trap-unwind does **not** run
  `defer` bodies in v1, documented limitation, since the non-local
  unwind jumps past them; matches "traps abort, don't gracefully
  unwind locals" stance, `SAFETY_AND_SANDBOX.md` Section 7).
- **`sizeof`/`offsetof`** - `TYPE_SYSTEM.md` Section 11 (compile-time
  constants, usable in `constexpr` contexts; `offsetof` only on
  registered/script struct fields).
- **Compound assignment semantics** - `TYPE_SYSTEM.md` Section 11 (`x += y`
  typechecks as `x = x + y` would, same-type-required rule applies;
  `x` evaluated once, not twice - important if `x` is a field/index
  expression with side-effecting subexpressions).
- **String interpolation `f"..."`** - `TYPE_SYSTEM.md` Section 11, `COMPILER_-
  PIPELINE.md` Section 6 (lowered to a call to the host `__fmt` native addon
  per `{expr}` hole; produces an `array<u8>` via that addon, Section 3; the
  interpolation is syntactic sugar, not a language builtin type).

## 6. Verified not-missing (things that looked like gaps but aren't)

- **Constructor call syntax `color(255,255,255,255)`** (the surveyed native-JIT language uses
  this heavily): ember v1 uses named-field struct literals
  `Color{r:255, g:255, b:255, a:255}` instead (`COMPILER_PIPELINE.md`
  Section 3 `StructLiteralExpr`). Positional constructors need a registered
  `factory` native (dropped from `TypeBuilder` v1, `BINDING_API.md` Section 3
  YAGNI rationale). For host types where positional ctor syntax is
  ergonomic, the host registers a plain `NativeFn` named `make_color`
  and script calls `make_color(255,255,255,255)` - same call cost, no
  new language feature. Documented here as the explicit v1 answer.
- **`main()` / routine registration lifecycle**: ember uses
  annotation-based discovery (`@on_tick`, `@event(...)`,
  `DESIGN.md` Section 2/Section 8) + host name lookup (`HOT_RELOAD.md` Section 7) instead
  of the surveyed native-JIT language's `register_routine(cast(fn))` dynamic call. Static
  discovery is adequate for v1 game-loop integration; *dynamic*
  registration (script decides at runtime what to hook) is a v2
  feature requiring function references - `ROADMAP.md`. See the new
  `LIFECYCLE.md` for the full equivalence.
- **String literals + escape processing**: already in v1
  (`COMPILER_PIPELINE.md` Section 1 - `\n \t \\ \"`).
- **Comment handling**: already v1 (`//`, `/* */`).
