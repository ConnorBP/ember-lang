# ember - gap analysis & completeness audit

This pass re-audits the spec against (a) the original request and (b)
the now-fully-known feature surface of the surveyed native-JIT language (../RESEARCH_NOTES.md updated).
Goal: confirm nothing the user actually asked for is missing, and
make every deferral an explicit, justified decision rather than an
oversight.

## 1. Original-request requirements → where satisfied

| Requirement (paraphrased from original request) | Status | Where |
|---|---|---|
| C-style scripting language | ✓ v1 | `../spec/COMPILER_PIPELINE.md` Section 2 grammar (C-family syntax, C precedence) |
| Compiles to **composable actual native x86-64 chunks** | ✓ v1 | `../spec/CODEGEN_SPEC.md` (per-function JIT into exec-memory arena, dispatch-table composition) |
| Bundling / pre-compile to a loadable module | ✓ v0.1 (added) | `../BUNDLING_AND_EM_MODULES.md`, `../spec/CODEGEN_SPEC.md` Section 15 test 7  -  the `.em` format (`em_file`/`em_writer`/`em_loader`); JIT → serialize → load → run round-trip proven |
| Fairly safe | ✓ v1 | `../spec/SAFETY_AND_SANDBOX.md` (budgets, bounds, depth, `PERM_FFI`, non-local unwind) |
| Extremely fast (JIT, not bytecode) | ✓ v1 | `../spec/CODEGEN_SPEC.md` (baseline JIT, no interpreter); honest caveat in Section 3 below re: "MUCH faster than AngelScript" |
| Expose scripting API bindings like AngelScript | ✓ v1 floor | `BindingBuilder` + `NativeSig`; fluent type/engine builders deferred (`../spec/BINDING_API.md`) |
| For game engine + game modding | ⚠ partial | core JIT+bindings plus limited array/string addons; map and richer collection/string APIs deferred — see Section 3 |
| "AngelScript but MUCH faster" | ✓ expected | baseline JIT vs AS's bytecode interpreter (../RESEARCH_NOTES.md)  -  see Section 3 caveat |
| "(sort of like a native-JIT scripting language)" | ✓ scoping | the surveyed native-JIT language's *embedding API* mirrored; its *language breadth* deliberately scoped down  -  `../ROADMAP.md` |
| Hot reload | ✓ v1 (single fn) / v2 (whole-module workflow) | `../HOT_RELOAD.md` |
| GC | deferred v1 (arena/host-owned), v2 plan | `../spec/MEMORY_AND_GC.md` Section 8, `../ROADMAP.md` |

No original-request requirement is unaddressed. The one ⚠
(game-modding data structures) is the real gap and is addressed in
Section 3 below + `../ROADMAP.md`.

## 2. Surveyed native-JIT language feature surface → v1 decision per feature

| surveyed-language feature | ember v1 | Rationale / where |
|---|---|---|
| Primitives (bool/i*/u*/f*) | ✓ v1 | `../spec/TYPE_SYSTEM.md` Section 1 |
| `char`/`wchar` | ✗ v1 | byte slices (`slice<u8>`) instead, `../spec/TYPE_SYSTEM.md` Section 1 |
| Atomic integers | ✓ v1 addon | `sync` exposes `atomic_new/load/store/fetch_add/fetch_sub/cas/swap/free`; script-created in-context threads remain deferred |
| `null` / `nullable` | ✗ v1 | no pointers; optional via i64 handle w/ 0=invalid idiom, `../spec/BINDING_API.md` Section 7 |
| `const` / `constexpr` | ✓ v1 (added this pass) | `../spec/TYPE_SYSTEM.md` Section 11, `../spec/COMPILER_PIPELINE.md` Section 2 |
| `auto` inference | ✓ v1 | `../spec/TYPE_SYSTEM.md` Section 9 |
| Ternary `?:` | ✓ v1 (added this pass) | `../spec/COMPILER_PIPELINE.md` Section 2, `../spec/TYPE_SYSTEM.md` Section 11 |
| `++` / `--` | ✓ v1 (added this pass) | `../spec/TYPE_SYSTEM.md` Section 11 |
| `cast<T>()` | ✗ v1 (diverged) | ember uses `expr as T` (C/Rust style); equivalent power, `../spec/TYPE_SYSTEM.md` Section 6 |
| `sizeof` / `offsetof` | ✓ v1 (added this pass) | `../spec/TYPE_SYSTEM.md` Section 11 |
| Compound assign (`+=` etc.) | ✓ v1 (semantics added this pass) | `../spec/TYPE_SYSTEM.md` Section 11 |
| `if`/`else`/`while`/`for`/`break`/`continue`/`return` | ✓ v1 | `../spec/COMPILER_PIPELINE.md` Section 2 |
| `do-while` | ✓ v1 (added this pass) | `../spec/COMPILER_PIPELINE.md` Section 2 |
| `for-each` | ✓ v1 (shipped 2026-07-11) | `for (x in slice)` over a slice `T[]` — slice-specific, no `iterable` protocol; the general `iterable()` TypeBuilder hook stays v2. `tests/lang/valid_for_each.ember`, `../ROADMAP.md` |
| `switch` | ✓ v1 (added this pass) | `../spec/COMPILER_PIPELINE.md` Section 2, `../spec/CODEGEN_SPEC.md` Section 12 |
| `match` (pattern) | ✓ v1 (shipped 2026-07-11) | `match (expr) { pat => body, _ => default }` — integer/bool literal patterns + `_` wildcard, no fallthrough. `tests/lang/valid_match.ember`, `../ROADMAP.md` |
| `goto` | ✗ v1 (deliberate) | structured control flow only; goto complicates liveness/scope, no need |
| `defer` | ✓ v1 (added this pass) | `../spec/CODEGEN_SPEC.md` Section 13, `../spec/COMPILER_PIPELINE.md` Section 6 |
| Default args | ✗ v1 | host registers overloads or takes slice; `../spec/BINDING_API.md` Section 2 |
| Reference `&` / `out` params | ✗ v1 (idiom instead) | pass `slice<T>` of len 1, `../spec/TYPE_SYSTEM.md` Section 5 |
| Variadic fns | ✗ v1 | `../spec/BINDING_API.md` Section 2 (fixed arity; variadic via slice) |
| `extern` | ✗ v1 | FFI via `PERM_FFI`-gated natives, not extern decls |
| Function references | ✓ v1 | `&fn`, `fn` handles, guarded indirect calls, and recorded-signature checks; bare-`fn` parameter signatures and cross-module handles remain deferred |
| Lambdas | ✗ v1 | `DESIGN.md` non-goal; v2+ with GC, `../ROADMAP.md` |
| Dynamic arrays / maps / strings (rich) | ✗ v1 builtin | exposed as **native addons** on host-owned memory, Section 3 below |
| Structs (ctors/dtor/methods/op-overload/bitfield/packed) | partial v1 | fields/methods/properties/op-overload ✓; ctor via named literal; dtor/bitfield/packed ✗, `../ROADMAP.md` |
| Classes / inheritance / vtables | ✗ v1 | nominal structs + static dispatch; v2+, `../ROADMAP.md` |
| Interfaces / mixins | ✗ v1 | v2+ |
| Properties (get/set) | ✓ v1 | `../spec/BINDING_API.md` Section 3 |
| Templates / monomorphization | ✗ v1 | `DESIGN.md` non-goal; v2+, `../ROADMAP.md` |
| Enums | ✓ v1 (untyped constants) | `enum E { ... }` + `E::A`; variants lower to i32 literals. Typed enums and host `EnumBuilder` remain deferred |
| Typedefs / `using` | ✗ v1 | YAGNI |
| Delegates | ✗ v1 | needs function refs (v2) |
| Namespaces | ✗ v1 | flat module scope; v2+ |
| Coroutines / `yield` | ✗ v1 | v2+, needs suspended-frame storage, `../ROADMAP.md` |
| Exceptions try/catch/throw | ✗ v1 | `runtime_error`/`runtime_exception` host-signal instead, `../spec/SAFETY_AND_SANDBOX.md` Section 7; v2+ |
| Heap `new`/`delete` | ✗ v1 | arena-only, `../spec/MEMORY_AND_GC.md`; GC v2+ |
| Inline asm intrinsics | ✗ v1 | `PERM_FFI`-gated native can wrap `__rdtsc` etc.; v2 if real need |
| `[[annotations]]` | ✓ v1 (`@` syntax) | `../spec/TYPE_SYSTEM.md` Section 10 |
| Modules / imports | ✓ v1 | textual `import`, `.em` bundles, and live `link`/`mod::fn`; bare-name search paths and cross-module function handles remain deferred |
| Preprocessor | ✗ v1 (deliberate) | `engine.define(name,value)` for compile-time defines instead, `DESIGN.md` Section 8 |
| FFI `[[dll(...)]]` | ✓ v1 (different mechanism) | `PERM_FFI`-gated `NativeFn`, `../spec/SAFETY_AND_SANDBOX.md` Section 6 |
| `static_assert` | ✗ v1 | YAGNI; add with const-expr eval in v2 |
| Lifecycle `main()` + routines | ✓ v1 (equivalent) | `../LIFECYCLE.md` (new this pass)  -  annotation-based, same semantics |

## 3. Standard addon reality and deferred data structures

The eight shipped host-side extensions are `vec`, `quat`, `mat`, `string`,
`array`, `math`, `sync`, and `lifecycle`. Their actual script APIs are:

- vec2/vec3/vec4 constructors and component get/set; vec add/sub/mul/equality;
  quat construction/access plus add/sub/mul/equality; mat4 construction,
  identity, get/set, multiplication, and equality;
- string construction, `string_from_slice`, length/character access,
  scalar-to-string conversions, identity, and concatenation/equality overloads;
- array creation, length, resize, typed u8/f32/i64 get/set, and `array_push_u8`;
- math `sqrt`/`sin`/`cos`/`tan` for f32;
- sync `atomic_*`, `swapbuf_*`, `spsc_*`, `mpsc_*`, and `mpmc_*` APIs;
- lifecycle `register_routine`/`unregister_routine`.

There is no map extension. General array pop/remove/clear and generic push,
string find/substr/format APIs, and a broader math surface are deferred. These
are addon gaps, not hidden language builtins. The host owns addon storage and
scripts carry nominal opaque handles.

The `.em` bundling format shipped in v0.1 (`em_file`/`em_writer`/`em_loader`,
`../spec/CODEGEN_SPEC.md` Section 15 test 7). v0.5 then shipped live cross-module
`link "foo.em" as foo;` plus `foo::bar()` through `ModuleRegistry`; textual
`import` remains source inclusion. v1 `.em` native code is ABI/process trusted
and exports carry `unknown_sig` (no canonical signatures, no build/ABI identity —
the v1 compatibility contract). v2 `.em` ships canonical `Type` signatures plus
build/ABI identity, and sema/loader verify arity, ordered parameter types, and
return type at link before page publication; portability and signatures are now
shipped v2 guarantees (`../MODULES.md` Section 5), not H12/H14 redesigns.

## 4. Honest performance caveat ("MUCH faster than AngelScript")

The original request says "AngelScript but MUCH faster (sort of like
a native-JIT scripting language)." ember v1 is a **baseline** JIT with a
tree-walking, stack-spilling emitter (no SSA IR, no linear-scan allocator, no
inlining or loop optimization). SSA-lite/linear scan are deferred. AngelScript is a bytecode interpreter. Even baseline native
code beats a bytecode interpreter on tight loops by typically 5-50×
(one dispatch-decode loop removed, real registers vs a virtual stack),
which comfortably satisfies "MUCH faster" for the hot-game-logic target.

**An optimizing native-JIT language is faster than ember v1 will be**, because such a language runs an
"optimizing pipeline" (per its docs, ../RESEARCH_NOTES.md updated). ember
v1 will *not* match an optimizing native-JIT language's speed - only its *category*
(native, not bytecode). Matching an optimizing native-JIT language's speed is a v2+ goal
(after the v0.5 benchmark harness exists to prove where ember is slow
and justify adding opt passes, `DESIGN.md` Section 9 - no speculative
optimization). This is stated honestly here rather than oversold in
the README.

## 5. Features added to v1 in this spec pass (concrete)

Each is small, C-expected, and cheap to specify/implement. Detail
patches landed in the noted docs:

- **Ternary `?:`** - `../spec/COMPILER_PIPELINE.md` Section 2 grammar, `../spec/TYPE_SYSTEM.md`
  Section 11 (result type = common type of branches, both must be
  same-type per the no-implicit-promotion rule, Section 6).
- **`++`/`--`** (prefix+postfix) - `../spec/TYPE_SYSTEM.md` Section 11 (integer
  operands only, returns the new/old value per prefix/postfix, checked
  overflow in debug like `+=`).
- **`switch`** - `../spec/COMPILER_PIPELINE.md` Section 2, `../spec/CODEGEN_SPEC.md` Section 12
  (integer/enum-ident scrutinee only; jump-table if dense, else
  cmp/je cascade; `break`/fallthrough-via-`goto`? - v1: **no fallthrough**,
  each case must `break`/`return`/`trap`, eliminates the entire
  C-fallthrough footgun class at the grammar level).
- **`do-while`** - `../spec/COMPILER_PIPELINE.md` Section 2 (lowered like `while`
  with the condition check at the bottom).
- **`const`/`constexpr`** - `../spec/TYPE_SYSTEM.md` Section 11 (`const` = immutable
  local; `constexpr` = compile-time-evaluable, usable as array sizes
  and case labels; v1 `constexpr` is restricted to literal-and-`sizeof`/
  `offsetof` expressions only, no full compile-time fn eval - that's
  v2 `static_assert` territory).
- **`defer`** - `../spec/CODEGEN_SPEC.md` Section 13, `../spec/COMPILER_PIPELINE.md` Section 6
  (implemented M5 is lexical-block-exit LIFO: normal fallthrough and cleanup
  edges before `break`, `continue`, and `return`; block-entry activation reset
  gives loop sites per-iteration behavior; lexical locals are valid in defer
  expressions. Existing trap/longjmp unwind still bypasses defers).
- **`sizeof`/`offsetof`** - `../spec/TYPE_SYSTEM.md` Section 11 (compile-time
  constants, usable in `constexpr` contexts; `offsetof` only on
  registered/script struct fields).
- **Compound assignment semantics** - `../spec/TYPE_SYSTEM.md` Section 11 (`x += y`
  typechecks as `x = x + y` would, same-type-required rule applies;
  `x` evaluated once, not twice - important if `x` is a field/index
  expression with side-effecting subexpressions).
- **String interpolation `f"..."`** - `../spec/TYPE_SYSTEM.md` Section 11 and
  `../spec/COMPILER_PIPELINE.md` Section 6. Parser sentinel
  `__fstring_to_string` is sema-lowered to `string_from_*` or
  `string_identity`, then concatenated through the string overload.

## 6. Verified not-missing (things that looked like gaps but aren't)

- **Constructor call syntax `color(255,255,255,255)`** (the surveyed native-JIT language uses
  this heavily): ember v1 uses named-field struct literals
  `Color{r:255, g:255, b:255, a:255}` instead (`../spec/COMPILER_PIPELINE.md`
  Section 3 `StructLiteralExpr`). Positional constructors need a registered
  `factory` native (dropped from `TypeBuilder` v1, `../spec/BINDING_API.md` Section 3
  YAGNI rationale). For host types where positional ctor syntax is
  ergonomic, the host registers a plain `NativeFn` named `make_color`
  and script calls `make_color(255,255,255,255)` - same call cost, no
  new language feature. Documented here as the explicit v1 answer.
- **`main()` / routine registration lifecycle**: ember ships static
  annotation discovery (`@on_tick`, `@event(...)`) and dynamic registration
  through lifecycle's `register_routine(&fn, data)` / `unregister_routine(id)`.
  Hosts retain stable slot indices and fetch the current dispatch entry before
  invoking it. See `../LIFECYCLE.md` and `../HOT_RELOAD.md` Section 7.
- **String literals + escape processing**: already in v1
  (`../spec/COMPILER_PIPELINE.md` Section 1 - `\n \t \\ \"`).
- **Comment handling**: already v1 (`//`, `/* */`).
