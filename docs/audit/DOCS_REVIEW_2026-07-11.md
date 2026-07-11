# Ember Docs Review — 2026-07-11 (accuracy pass + recent-feature documentation)

**Scope:** read-only accuracy review of the user-facing + spec + planning docs
against the current source (`src/`, `extensions/`, `examples/ember_cli.cpp`,
`tests/lang/`), with fixes applied to **docs only** (no `.cpp`/`.hpp` touched).
The review also backfilled documentation for the four recent Tier 1 features
that had shipped in source but were still described as TODO/deferred in the docs:
**`constexpr fn` evaluation**, **`static_assert`**, **typed enums (`enum E : T`)**
+ enum-from-constexpr, **`ember test` CLI**, and confirmed the
**iterable() for-each-over-array** and **8 IR optimization passes** docs are
current.

**Baseline gate (confirmed green before AND after, unmodified source):**
`ctest` → **42/42 passed** in `buildt/` (the task brief said 40/40; the actual
configured total is 42 — 40 excluding the two bench targets `bench_codegen_paths`
and `bench_ember_vs_as`; without the AngelScript SDK the total is 41). The lang
suite via `ember test tests/lang` → **200/200 passed**. The README example
compiles + runs (exit 192 = 704 mod 256, the correct `total + len`).

**Source files NOT touched.** The working tree had pre-existing uncommitted
edits to `src/sema.cpp`, `extensions/opt/ext_opt.cpp`,
`examples/constexpr_test.cpp`, and a new `tests/lang/valid_type_stress.ember`
(present at the start of this session as in-progress work from a concurrent
code-review/correctness agent — a `f32→f64` implicit-widening sema gate + a
typed-enum literal re-type guard + opt-pass additions). This review did **not**
modify any of those; only the 8 docs listed in §A were edited. The pre-existing
source edits are left for the code-review agent. ctest stays 42/42 green with
them present.

---

## A. Docs reviewed + edited

| Doc | Edited? | Summary of changes |
|---|---|---|
| `README.md` | ✓ | test count 37→42 (35→40 excl.); "baseline JIT, no inlining or loop opts" → notes the IR backend + 8 opt passes shipped behind flags (default path still baseline); stale "no typed enums" example comment → typed/untyped enum note; added `constexpr fn` + `static_assert` + typed enums to the Language spec; added `ember test` + `--passes` to the CLI reference; "1.00×" → "~1.0×" |
| `docs/ROADMAP.md` | ✓ | test count 37→42 (35→40); Tier 1 `static_assert`/`constexpr`/typed-enums TODO → ✓ shipped entries; self-hosting path step 1 (constexpr) → ✓ done; `ember test` (Family A) TODO → ✓ shipped; Family A header "built" → "shipped"; Tier 1 enum entry "typed enums remain a later refinement" → "shipped 2026-07-11" |
| `docs/spec/TYPE_SYSTEM.md` | ✓ | §11.5 "constexpr is a RESERVED keyword... parse error" → `constexpr fn` + `static_assert` shipped documentation; §11.4/§11.6/§12.2 stale "constexpr reserved" cross-refs removed; §8 "no first-class functions" → `&fn` shipped; new §15 (Enums — untyped + typed) documenting typed-enum type rules (enum→int widening, int→enum rejection, enum-from-constexpr) |
| `docs/spec/COMPILER_PIPELINE.md` | ✓ | §1 keyword list added `constexpr`/`static_assert`/`priv`; §1 "constexpr reserved keyword" → shipped fn modifier; §2 grammar: `program` + `static_assert_decl`, `enum_decl` typed form, `variant` constexpr-fn-call, `func_decl` `constexpr` modifier, `stmt` `static_assert`; §2a added constexpr fn + static_assert + typed-enum bullets + fixed for-each bullet (array case); §3 AST: `StaticAssertStmt`, `EnumDecl.backing`, `FuncDecl.is_constexpr`, `ForEachStmt.array_elem_ty`; §4 Pass 1.4 typed-enum registration + constexpr-call pre-pass + static_assert check, Pass 1.6 "typed enums later flip" → shipped; §6 "constexpr reserved" → constexpr-fn/static_assert produce no runtime code |
| `docs/spec/BINDING_API.md` | ✓ | "eight standard extensions" → ten (added map, io); §5 EnumBuilder note: typed-enum form mentioned |
| `docs/planning/DESIGN.md` | ✓ | Non-goals "Self-hosting" → "NOT a non-goal (north star, work started)"; "Explicitly skipped" "no self-hosting" + "modules in v1" corrected; v1.0 milestone test count 37→42 (35→40) |
| `docs/planning/GAP_ANALYSIS.md` | ✓ | §2 table: `static_assert` ✗→✓ shipped; `const`/`constexpr` note "no full compile-time fn eval"→shipped; Enums "typed enums deferred"→shipped; for-each "no iterable protocol"→array case shipped; §5 constexpr bullet fixed |
| `docs/planning/v1.0_INTEGRATION_NOTES.md` | ✓ | test count 37→42 (35→40); §2 "enum name is not a type... typed enums later flip" → typed enums shipped; "six→eight extensions" → ten |
| `extensions/README.md` | ✗ (accurate) | Verified: 10 addon extensions + 2 pass extensions, all 8 opt passes (constprop/dce/cse/licm/forward/copyprop/instcombine/dse) + subst listed, io row present, "links all ten addon extensions" — current, no changes needed |
| `docs/spec/CODEGEN_SPEC.md` | ✗ (accurate) | §17 (for-each, incl. array case 17b) + §18 (match) + §16 (aggregate) current; §10.1 const fold current. No stale recent-feature claims found. |
| `docs/spec/PASS_SYSTEM_DESIGN.md` | ✗ (accurate) | §8 lists all 8 opt passes + subst shipped; FlatteningPass/MBAPass/EmberAnalysisManager future. Current. |
| `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md` | ✗ (accurate) | §8 Stage A/B/C shipped, all 8 passes listed, status current. |
| `docs/spec/MEMORY_AND_GC.md` | ✗ (accurate) | Aggregate globals, string encryption, slice-escape Stage 1/2, fn handles all current. |
| `docs/spec/SAFETY_AND_SANDBOX.md` | ✗ (accurate) | §7a call-target provenance, §8a context thread-safety, §6 PERM_FFI all current. |
| `docs/spec/SPEC_AUDIT_2026-07-10.md` | ✗ (historical) | Dated audit at commit `8062195`; findings F1-F7 marked DONE inline. Historical record — left as-is. |
| `docs/spec/BENCHMARK_SYSTEM_DESIGN.md` | ✗ (accurate) | 6 prototype paths, findings §8 current. |
| `docs/BUNDLING_AND_EM_MODULES.md` | ✗ (accurate) | v2/v3/v4/v5 formats, signing, pub/priv, live link all current. |
| `docs/MODULES.md` | ✗ (accurate) | Registry, cross-module call, link grammar, pub/priv, v5 all current. |
| `docs/HOT_RELOAD.md` | ✗ (accurate) | HotReloadDomain epoch reclamation, migration recipe current. |
| `docs/LIFECYCLE.md` | ✗ (accurate) | @entry/@on_tick + dynamic register_routine current. |

---

## B. Stale claims found + fixed

### B1. `constexpr` described as a "RESERVED keyword with no parser/sema support" — FIXED (Critical staleness)

The most material stale claim. `TYPE_SYSTEM.md` §11.5, `COMPILER_PIPELINE.md`
§1/§6, and the `§11.4`/`§11.6`/`§12.2` cross-refs all stated `constexpr` "lexes
as `Kw_constexpr` but has NO parser or sema support — using it is a parse error"
and "Full const-eval (recursive `constexpr fn`s, `static_assert`...) is v2."
This was true at the 2026-07-10 audit but **false now**: commit `3b8a8d7`
shipped `constexpr fn` evaluation (a bounded tree-walking interpreter
`eval_constexpr_fn` + a constexpr-call pre-pass `lower_constexpr_calls_expr`
that rewrites a constexpr call with all-constant args to an `IntLit` before
`check_expr`). Verified in `src/sema.cpp:932-959` (`eval_constexpr_fn`,
`lower_constexpr_calls_*`, `try_fold_constexpr_call`) + `src/parser.cpp:950-975`
(`is_constexpr` modifier) + `tests/lang/valid_constexpr*.ember` + ctest
`constexpr`. **Fixed** in TYPE_SYSTEM §11.5 (full `constexpr fn` + `static_assert`
documentation with bounds: max 100000 loop iters, max 256 recursion depth, i64
integer fns only, runtime fallback), COMPILER_PIPELINE §1/§2a/§3/§4/§6, and the
§11.4/§11.6/§12.2 cross-refs.

### B2. `static_assert` listed as TODO — FIXED

`ROADMAP.md` Tier 1 listed `static_assert(cond, msg)` as "TODO (blocked on
constexpr)"; `GAP_ANALYSIS.md` §2 listed it "✗ v1 — YAGNI; add with const-expr
eval in v2." Both stale: commit `a7adce7` shipped `static_assert` (parser
`parse_static_assert`, sema `check_static_assert`, top-level
`prog.static_asserts` + in-body `StaticAssertStmt`, true→elided / false→compile
error / non-const→compile error). Verified in `src/parser.cpp:278-303` +
`src/sema.cpp:1909-1940` + `tests/lang/{valid_static_assert,valid_static_assert_constexpr,sema_invalid_static_assert_*}.ember` + ctest `static_assert`. **Fixed** in ROADMAP (✓ shipped entry), GAP_ANALYSIS (✓ v1), TYPE_SYSTEM §11.5, COMPILER_PIPELINE §1/§2a/§3/§4/§6.

### B3. Typed enums (`enum E : T`) listed as TODO/deferred — FIXED

`ROADMAP.md` Tier 1 listed "Typed enums (`enum E : i32`) + enum-from-expr" as
"TODO (blocked on constexpr)"; the Tier 1 `enum` entry said "typed enums remain
a later refinement"; `GAP_ANALYSIS.md` §2 said "Typed enums... remain deferred";
`TYPE_SYSTEM.md` had no typed-enum documentation; `v1.0_INTEGRATION_NOTES.md` §2
said "an enum name is not a type in v1... the hook typed enums later flip to
accept"; `COMPILER_PIPELINE.md` §4 Pass 1.6 said "typed enums later flip to
accept." All stale: commit `7f00a5f` shipped typed enums (`enum E : T` makes `E`
a real type backed by `T`; enum→int implicit widening allowed; int→enum
rejected; enum-from-constexpr-expr via the constexpr-call pre-pass). Verified
in `src/parser.cpp:243` (typed enum grammar), `src/sema.cpp:337-347,503-523`
(`register_typed_enums`, `typed_enum_backing`/`typed_enum_types`), `src/ast.hpp:27-28`
+ `tests/lang/{valid_typed_enum,valid_typed_enum_match,valid_enum_from_constexpr,sema_invalid_int_to_enum}.ember` + ctest `typed_enum`. **Fixed** in ROADMAP (✓ shipped), GAP_ANALYSIS (✓ v1 typed), TYPE_SYSTEM (new §15), COMPILER_PIPELINE §2a/§3/§4, v1.0_INTEGRATION_NOTES §2.

### B4. `ember test` CLI listed as TODO — FIXED

`ROADMAP.md` Family A listed `ember test` as "TODO (NEXT)... blocked on a small
refactor." Stale: `examples/ember_cli.cpp:695-904` ships `run_test_command` +
`TestClassifier` (classifies by `// expect: N` / `runtime_trap_*` / `invalid_*`
/ `sema_invalid_*` / `sema_valid_*` / else parse-only), wired as the
`ember_test_cli` ctest target. Verified: `ember test tests/lang` → 200/200
passed. **Fixed** in ROADMAP (✓ shipped) + README CLI reference (`ember test [dir]`).

### B5. CTest count "37 (35 excluding two benchmarks)" — FIXED (5 locations)

The tree grew from 37 to **42** ctest targets (the 2026-07-11 Tier 1 follow-ons
added `constexpr`, `static_assert`, `typed_enum`, `codegen_opt`, plus the
Stage A/B/C tests `thin_ir*`/`em_v5_*`/`ember_pass`/`ir_passes`/`host_struct`).
The "37 (35 excl.)" count appeared in `README.md`, `ROADMAP.md` (×2: the v1.0
batch header + the prose), `DESIGN.md` (v1.0 milestone), and
`v1.0_INTEGRATION_NOTES.md`. Verified via `ctest -N` → "Total Tests: 42".
**Fixed** to "42 (40 excluding the two benchmarks)" in all 5 locations (the
no-SDK count is 41, noted in ROADMAP).

### B6. "No first-class functions" in TYPE_SYSTEM §8 — FIXED

`TYPE_SYSTEM.md` §8 stated "No function values / function pointers as
script-visible values in v1 (no first-class functions, no closures) — a function
name in an expression position other than a direct call is a compile error."
Stale: `&fn` / `handle(args)` / the `fn` type keyword shipped in v1.0 (ROADMAP
Tier 2 ✓). Verified in `src/parser.cpp` (prefix `&` → `FnHandleExpr`, indirect
`CallExpr`) + `src/sema.cpp` (slot baking, i64↔fn forbidden) + ctest
`function_refs`. **Fixed** in TYPE_SYSTEM §8 (documents `&fn`/`handle(args)`/`fn`
type + the call-target-provenance guard + the bare-`fn` signature hole + closures
still a non-goal).

### B7. README "baseline JIT, no inlining or loop opts" framing — FIXED

The README framed ember as purely baseline with "closing that gap is a
benchmark-gated v2+ goal, not a v1 claim." Partially stale: Stage A (thin IR
backend), Stage B (`.em` v5 IR serialization), and Stage C (8 IR opt passes incl.
**LICM**, which is a loop opt) all shipped behind flags (`enable_ir_backend`,
`--passes`), default-off. The default path is still the baseline tree-walker, so
the spirit is right, but "no inlining or loop opts" is technically false (LICM
exists) and "v2+ goal" understates that the infrastructure shipped. **Fixed** to
state the default path is baseline, the IR backend + 8 passes shipped behind
flags as the staged path, and full SSA-lite + linear-scan remains future.

### B8. README example comment "Enums are untyped i32 constants (v1: no typed enums, no tag)" — FIXED

Stale (typed enums shipped). **Fixed** to describe both the untyped and typed
forms. The "What this example shows" bullet "Enums with auto-increment..." →
notes both forms + constexpr-fn variant values. Example still compiles + runs
(verified, exit 192).

### B9. DESIGN.md non-goals "Self-hosting" + "no self-hosting, hard non-goal" + "modules in v1" — FIXED

`DESIGN.md` Non-goals listed "Self-hosting" and the Explicitly-skipped list said
"no self-hosting - hard non-goals, never added" and "No templates/classes/.../
modules in v1." All stale: the ROADMAP was corrected (commit `3134561`) to make
self-hosting the north star (work started, `demo/compiler/`), and live modules
shipped v0.5. **Fixed**: Non-goals → "Self-hosting is NOT a non-goal";
Explicitly-skipped → self-hosting corrected + modules noted as shipped v0.5
(namespaces still Tier 6).

### B10. BINDING_API.md "eight standard extensions" — FIXED

Said "the eight standard extensions (vec/quat/mat/string/array/math/sync/lifecycle)."
Stale: `map` + `io` shipped 2026-07-11 (10 addon extensions now). **Fixed** →
"ten standard extensions (...map/io)." The §5 EnumBuilder note was also updated
to mention the typed-enum form.

---

## C. Recent-feature documentation backfill (the 7 items the task required)

1. **constexpr fn evaluation** — was undocumented (specs said "reserved keyword").
   **Now documented** in TYPE_SYSTEM §11.5 (keyword, eval interpreter, bounds:
   max 100000 loop iters / max 256 recursion / i64 integer fns only / runtime
   fallback), COMPILER_PIPELINE §1/§2a/§3/§4/§6 (grammar, AST `FuncDecl.is_constexpr`,
   the constexpr-call pre-pass, no-runtime-code lowering), ROADMAP Tier 1 (✓
   shipped entry), README (syntax + language spec).

2. **static_assert** — was listed TODO. **Now documented** in TYPE_SYSTEM §11.5
   (syntax, compile-time semantics: true→elided / false→compile error /
   non-const→compile error, top-level + in-body), COMPILER_PIPELINE §1/§2a/§3/§4/§6,
   ROADMAP Tier 1 (✓ shipped), GAP_ANALYSIS §2 (✓ v1), README (syntax).

3. **typed enums (`enum E : T`)** — was undocumented + listed deferred. **Now
   documented** in TYPE_SYSTEM §15 (new section: typed-enum registration, enum→int
   widening rule, int→enum rejection, comparison, enum-from-constexpr-expr),
   COMPILER_PIPELINE §2a/§3/§4 (grammar `enum_decl` typed form, AST
   `EnumDecl.backing`, Pass 1.4 `register_typed_enums`, Pass 1.6 untyped-only
   rejection), ROADMAP Tier 1 (✓ shipped), GAP_ANALYSIS §2 (✓ v1 typed), README
   (type system + example comment).

4. **iterable() for-each over arrays** — was already documented (commit `98dbd3c`
   added CODEGEN_SPEC §17 + TYPE_SYSTEM §13.2). **Verified current**: §17a (slice)
   + §17b (array handle, `array_get_*` dispatch) + §13.2 (iterable hook, array
   case) match `src/codegen.cpp` ForEachStmt + `src/sema.cpp` `infer_*_array_elem_ty`
   + `src/ast.hpp` `ForEachStmt::array_elem_ty`. The COMPILER_PIPELINE §2a for-each
   bullet was stale ("iterable must be a slice T[], sema rejects non-slice") —
   **fixed** to note the array-handle case.

5. **ember test CLI** — was listed TODO. **Now documented** in ROADMAP Family A
   (✓ shipped, classification rules) + README CLI reference.

6. **8 IR optimization passes** — **verified current** in PASS_SYSTEM_DESIGN §8
   (all 8: constprop/dce/cse/licm/forward/copyprop/instcombine/dse + subst) and
   CODEGEN_OPTIMIZATION_DESIGN §8 (Stage C shipped, all 8 listed). extensions/README.md
   opt row lists all 8. A prior agent fixed these; they are accurate. No changes.

7. **enum-from-constexpr (variant value = constexpr fn call)** — **now documented**
   in TYPE_SYSTEM §15.2 (enum-from-constexpr-expr subsection), COMPILER_PIPELINE
   §2a (variant grammar `= constexpr_fn_call`) + §4 (resolve_enums folds via the
   constexpr-call pre-pass), ROADMAP Tier 1 (typed-enum entry mentions it). Verified
   in `src/sema.cpp:581-584` + `tests/lang/valid_enum_from_constexpr.ember`.

---

## D. Verified-accurate items (checked against source, no changes needed)

- **extensions/README.md** — 10 addon + 2 pass extensions; opt row lists all 8
  passes; io row present; "links all ten addon extensions"; build list complete.
- **CODEGEN_SPEC.md §17/§18** — for-each (slice 17a + array 17b) + match lowering
  match the source.
- **PASS_SYSTEM_DESIGN.md §8** — 8 opt passes + subst, EmberAnalysisManager +
  FlatteningPass/MBAPass future.
- **CODEGEN_OPTIMIZATION_DESIGN.md §8** — Stage A/B/C shipped, 8 passes, status.
- **MEMORY_AND_GC.md** — aggregate globals (§4), string encryption (§6),
  slice-escape Stage 1/2 (§3), fn handles — all current.
- **SAFETY_AND_SANDBOX.md** — §7a call-target provenance, §8a context thread-safety,
  §6 PERM_FFI, §1 .em attack surface + v4 signing — all current.
- **BUNDLING_AND_EM_MODULES.md** — v2/v3/v4/v5 formats, Ed25519 signing,
  re-emit-at-load, pub/priv — all current.
- **MODULES.md** — registry, cross-module call, link grammar, pub/priv, v5 — current.
- **HOT_RELOAD.md** — HotReloadDomain epoch reclamation, migration recipe — current.
- **LIFECYCLE.md** — @entry/@on_tick + dynamic register_routine — current.
- **README example** — compiles + runs (exit 192 = 704 mod 256, correct
  `total + len` where total = 700 = (100+200+50)×2, len = 4 = "0150"). The
  example exercises type inference, enums, structs, for-each over a slice,
  match, operator overloads, explicit casts, opaque string handles, slice views,
  native calls — all accurate.
- **README MSVC claim** — accurate (`CMakeLists.txt:21-22` `FATAL_ERROR "MSVC x64
  not yet supported; use MinGW"`).
- **README extension count** — "ten NativeSig extensions" + "two pass extensions"
  accurate.

---

## E. Notes (not fixed — within jitter / out of scope)

### E1. Benchmark numbers are run-to-run jittery (not a doc error)

`ROADMAP.md`'s "Codegen optimization" section cites an earlier bench run
(int_div 1.00×, call_overhead 5.23×, loop 5.69×, slice_bounds 5.60×, string_decrypt
5.58×, struct_by_value 3.00×). The current `bench/results_codegen_paths.md` run
differs (int_div 1.04×, call 4.49×, loop 5.29×, slice 9.17×, string 6.13×, struct
4.00×). `BENCHMARK_SYSTEM_DESIGN.md` §8.1 itself documents this as run-to-run
jitter on a shared dev machine ("Ranges reflect run-to-run jitter; the ranking is
stable"). The qualitative ranking (5-9× on the slow paths, ~1× on int_div) holds.
**Not fixed** — the ROADMAP numbers are a valid earlier run; the current run is a
valid later run; both are within the documented jitter. The README's "~1.0×" on
int_div is accurate to within jitter (was "1.00×", softened to "~1.0×"). The
README's "five of six codegen paths are 5-9× slower" is roughly right for the
slowest paths (loop 5.29, slice 9.17, string 6.13 clearly in 5-9×; call_overhead
4.49 safety-off / 5.98 safety-on; struct_by_value 4.00 noisy at 400 vs 100 ns).

### E2. `docs/audit/` audit-tracking docs are stale relative to current HEAD (out of review scope)

`docs/audit/PENDING_FEATURES_2026-07-11.md` and `docs/audit/PENDING_ACTIONS_2026-07-11.md`
are dated audit snapshots at commit `d25cc8c` (an earlier HEAD). They state
`static_assert` "DEFERRED — Not implemented," `constexpr` "DEFERRED /
PARTIALLY-CLAIMED — Not implemented," "typed enums remain deferred," and "the
single biggest roadmap gap is the missing `io` extension." **All four are now
false** at current HEAD (the features shipped after that audit: commits
`3b8a8d7`, `a7adce7`, `7f00a5f`, `b836a5a`). These docs are NOT in the review
list and are dated audit artifacts (clearly headered "Audited commit (HEAD):
d25cc8c"), so they are honestly snapshots-at-a-commit. **Not modified** — editing
dated audit records would misrepresent the audit's findings-at-the-time. Flagged
here so a reader knows the audit docs predate the Tier 1 / io shipment. A fresh
audit against current HEAD would clear these. (The main docs, which ARE in scope
and which the audit docs reference, are now corrected.)

### E3. Pre-existing uncommitted source edits (not this review's work)

The working tree has uncommitted edits to `src/sema.cpp` (a `f32→f64` implicit-
widening sema gate + a typed-enum literal re-type guard), `extensions/opt/ext_opt.cpp`
(opt-pass additions), `examples/constexpr_test.cpp` (test additions), and a new
`tests/lang/valid_type_stress.ember`. These were present at the start of this
session (in-progress work from a concurrent code-review/correctness agent). This
review did **not** touch them (docs-only per the task constraint). ctest stays
42/42 green with them present. They are left for the code-review agent to commit
or discard. **Note for the code-review agent:** the `f32→f64` widening gate in
`sema.cpp` is a real sema change (TYPE_SYSTEM §6 documents `f32→f64` as the one
lossless implicit float conversion — the gate implements that spec claim, which
was previously spec-only); the typed-enum literal guard prevents
`let x: i16 = Color::Red` from silently narrowing an enum value. Both look
correct and align with the (now-corrected) specs.

---

## F. Method

- Each doc was read in full; every current-state claim was grepped against
  `src/`, `extensions/`, `examples/ember_cli.cpp`, and `tests/lang/`, and
  verified against the ctest/lang baseline.
- The 7 recent-feature items (§C) were each checked: keyword in lexer
  (`src/lexer.cpp`), parse path (`src/parser.cpp`), sema path (`src/sema.cpp`),
  AST node (`src/ast.hpp`), test coverage (`tests/lang/*.ember` + ctest targets).
- README code example extracted to `/tmp/readme_example.ember` and run via
  `./buildt/ember_cli.exe run ... --fn main` (exit 192, correct).
- `ctest` run before and after → 42/42 green both times (docs don't affect tests;
  confirmed no source file was accidentally edited by this review).
- `git diff --name-only | grep -E '\.(cpp|hpp)$'` confirmed only pre-existing
  source edits (not this review's) are in the working tree.
- `git add` will stage **only** the 8 edited docs + this report (not the source
  files).

---

## G. Commit

`git add docs/ README.md extensions/README.md && git commit -m 'Docs review:
accuracy pass + recent-feature documentation (constexpr, static_assert, typed
enums, iterable, ember test)'` — stages the 8 edited docs (`README.md`,
`docs/ROADMAP.md`, `docs/spec/TYPE_SYSTEM.md`, `docs/spec/COMPILER_PIPELINE.md`,
`docs/spec/BINDING_API.md`, `docs/planning/DESIGN.md`,
`docs/planning/GAP_ANALYSIS.md`, `docs/planning/v1.0_INTEGRATION_NOTES.md`) +
this report (`docs/audit/DOCS_REVIEW_2026-07-11.md`). `extensions/README.md` is
staged too (verified accurate, no content change, but included in the add per
the task). The pre-existing source edits (`src/sema.cpp`, etc.) are **excluded**
from the commit.
