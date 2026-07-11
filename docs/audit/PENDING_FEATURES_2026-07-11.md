# ember — Pending / Incomplete Language-Feature Audit

**Repo:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Audited commit (HEAD):** `d25cc8c` ("Deep audit 2026-07-11: 5-dimension parallel audit + critical security fixes")
**Scope:** read-only audit of `docs/ROADMAP.md` against the actual source (`src/`, `extensions/`, `examples/ember_cli.cpp`) and the design docs (`docs/spec/`, `docs/planning/`). No source files were edited.
**Method:** every claim below was verified by reading the cited source file/line. The four ROADMAP status categories are used verbatim — **SHIPPED**, **PARTIALLY SHIPPED**, **DEFERRED**, **MISSING** — plus a fifth, **CONFIRMED DEFECT** (parses + sema-accepts but miscompiles or crashes), which is the "old do-while bug" category the task names.

---

## 0. Headline

ember at `d25cc8c` is a **v1.0 language with a coherent, mostly-shipped surface**. Every ROADMAP tier has either shipped or is an explicit, trigger-gated deferral. The two things that most need attention are **not roadmap deferrals** — they are (a) six confirmed codegen/sema correctness defects (C1–C6, carried forward unfixed from the `bf76217`/`7c73361` audit; only the two *security* findings S1/S2 landed in `d25cc8c`) and (b) a cluster of **spec-vs-implementation drift** where `docs/spec/` describes behavior the source does not provide (the `constexpr` keyword being the clearest case: lexed, documented as partially working, with zero parser/sema support). The single biggest *roadmap* gap for a real game modder is the **missing `io` extension** (no `print`/`println`/`read_line`/`read_file`/`write_file`) — a script cannot produce any output except its integer exit code.

---

## 1. ROADMAP feature-by-feature status

### Tier 0 — standard addon set (host C++ side)

| Addon | Status | Evidence |
|---|---|---|
| `array` | **SHIPPED** (full v1 API) | `extensions/array/ext_array.cpp:139-155` — 17 natives: new/length/resize, u8/f32/i64 get/set, push_u8/f32/i64, pop_u8/f32/i64, clear, remove. ROADMAP's "full v1 API shipped 2026-07-11" is accurate. |
| `map<K,V>` | **SHIPPED** | `extensions/map/ext_map.cpp:70-76` — 7 natives: map_new/set/get/contains/length/remove/clear, i64-keyed host `unordered_map`. Lang tests: `tests/lang/valid_map_*.ember` (4 files). Note: no ctest-level test target for `map` (only lang-suite coverage) — see §5. |
| `string` | **PARTIALLY SHIPPED** | `extensions/string/ext_string.cpp:144-154` — 11 natives incl. `string_find`/`string_substr` (added 2026-07-11). **Missing:** general `string_format`/`__fmt` (f-strings lower through `__fstring_to_string` → `string_from_*` only; no arbitrary format spec). ROADMAP flags this honestly. |
| `math` | **PARTIALLY SHIPPED** | `extensions/math/ext_math.cpp:37-50` — f32 sqrt/sin/cos/tan; f64 sqrt_f64/sin_f64/cos_f64/tan_f64/floor_f64/ceil_f64/abs_f64/pow_f64; abs_i64. **Missing:** f32 floor/ceil/abs/pow, atan2/log/exp/min/max, any integer min/max/clamp. "Broader math still deferred" per ROADMAP. |
| `vec2/vec3/vec4`, `quat`, `mat4` | **SHIPPED** | `extensions/{vec,quat,mat}/` — opaque i64 handles, constructors/accessors, registered `+`/`==` overloads. |
| `sync` | **SHIPPED** | `extensions/sync/` — atomics (aint8/16/32/64), swap buffer, SPSC/MPSC/MPMC queues. Pinned by `ext_sync_test`. |
| `lifecycle` | **SHIPPED** | `extensions/lifecycle/` — `register_routine`/`unregister_routine`/`host_routines()`. Pinned by `ext_lifecycle_test`. |

**Tier 0 verdict: SHIPPED.** All nine native addons exist and register. The partial ones (`string`, `math`) are partial in the *breadth* the ROADMAP already names as deferred, not in correctness.

### Tier 1 — small language extensions

| Feature | Status | Evidence |
|---|---|---|
| `enum` | **SHIPPED** | `src/parser.cpp` `parse_enum`; `src/sema.cpp` `resolve_enums` + `lower_enum_access_expr` (rewrites `E::A` → `IntLit` in place). Untyped i32 constants; typed enums (`enum E : i32`) and `enum`-from-expr remain deferred per ROADMAP. Pinned by `tests/lang/{valid_enums,sema_*_enum_*}.ember`. |
| `for-each` | **PARTIALLY SHIPPED** | Parses (`src/parser.cpp` `for (x in slice)`), sema-checks (`src/sema.cpp` ForEachStmt), codegens in the **tree-walker** (`src/codegen.cpp:3184`, desugars to a while loop). **IR backend does NOT lower it** — `src/thin_lower.cpp:949` marks the function `non_serializable` and falls back to the tree-walker. ROADMAP documents this exactly. **Caveat:** the tree-walker for-each has a confirmed correctness bug for non-{1,2,4,8}-byte elements (C2, §4). The general `iterable()` TypeBuilder hook stays deferred. |
| `match` (pattern) | **PARTIALLY SHIPPED** | Parses (`src/parser.cpp` `match (expr) { pat => body, _ => default }`), sema-checks (`src/sema.cpp` MatchStmt), codegens in the **tree-walker** (`src/codegen.cpp:3321`). **IR backend does NOT lower it** — falls back with for-each (`src/thin_lower.cpp:956`). Patterns are int/bool literals + `_` only; struct destructure + guards remain deferred per ROADMAP. |
| `static_assert` | **DEFERRED** | Not implemented. ROADMAP: "Dep: `constexpr` evaluation broadened beyond literals." |
| `constexpr` function evaluation | **DEFERRED / PARTIALLY-CLAIMED** | Not implemented — but see §3: the `constexpr` *keyword* is lexed and the specs claim a restricted form works, while the source has zero support. |

**Tier 1 verdict: SHIPPED for the three constructs (enum/for-each/match) with the documented IR-backend fallback caveat; DEFERRED for static_assert + full constexpr.** The do-while bug the task references is **fixed** — `DoWhileStmt` parses, sema-checks, codegens (`src/codegen.cpp:3169`), IR-lowers (`src/thin_lower.cpp:2528`), and is exercised in `tests/lang/runtime_{audit_semantics,language_features}.ember`.

### Tier 2 — function references + dynamic registration

| Feature | Status | Evidence |
|---|---|---|
| Function references (`&fn` / `handle(args)` / `fn` type) | **PARTIALLY SHIPPED** | `src/parser.cpp` prefix `&` → `FnHandleExpr`; `parse_type` bare `fn` → i64+`is_fn_handle`; indirect `CallExpr`; `src/sema.cpp` FnHandleExpr + indirect-call checks; `src/codegen.cpp` `emit_call_target_guard` (bitset allowlist). Pinned by `examples/function_refs_test.cpp` (ctest `function_refs`). **Two documented open items (ROADMAP §Tier 2):** (1) the bare-`fn` signature hole — a `fn`-typed *parameter* with no recorded signature accepts any args (type-soundness hole, not a sandbox violation; the runtime guard still validates the handle); (2) **cross-module handles deferred to v2+** — `&mod::fn` / `mod::fn`-as-handle is not in scope (allowlist is per-module). **No `fn(i64)->i64` parameterized function types** (v2+). |
| `register_routine` dynamic registration | **SHIPPED** | `extensions/lifecycle/` — `register_routine(fn h, i64 data) -> id` / `unregister_routine(id)` + `host_routines()`. The `fn` param is typed so sema rejects a forged plain i64. Pinned by `examples/ext_lifecycle_test.cpp`; demo `examples/scripts/dynamic_registration.ember`. |

**Tier 2 verdict: SHIPPED** with the two ROADMAP-documented open items (bare-fn arg hole, cross-module handles) explicitly deferred.

### Tier 3 — OOP / polymorphism

| Feature | Status | Evidence |
|---|---|---|
| Classes + single inheritance + vtables | **DEFERRED** | Not in grammar (`src/parser.cpp` has no `class`). ROADMAP: "Dep: heap/GC (Tier 4)." |
| Interfaces | **DEFERRED** | Dep: classes. |
| Mixins | **DEFERRED** | Dep: classes. |
| Templates / monomorphization | **DEFERRED** | No `<T>` syntax. ROADMAP: "Dep: monomorphization pass." |

**Tier 3 verdict: DEFERRED (all).** None have a re-entry trigger fired; structs + free functions + operator overloads cover the current need.

### Tier 4 — heap + GC

| Feature | Status | Evidence |
|---|---|---|
| Tracing GC | **DEFERRED** | No heap/GC. `docs/spec/MEMORY_AND_GC.md` §8 has the rationale. ROADMAP: "only build when Tier 3 forces it." |
| `new`/`delete` + lambdas with capture | **DEFERRED** | No `new`/lambda syntax. Dep: GC. |

**Tier 4 verdict: DEFERRED (all).** Deliberate; no script construct creates a heap reference today.

### Tier 5 — concurrency + exceptions

| Feature | Status | Evidence |
|---|---|---|
| Sync queue primitives + context thread-safety | **SHIPPED** | `extensions/sync/` (atomics, swap buffer, SPSC/MPSC/MPMC queues); `src/context.hpp` per-thread `context_t`; `CodeGenCtx::use_context_reg` (r14 indirection, `src/codegen.cpp`). Pinned by `examples/thread_safety_test.cpp` + `examples/ext_sync_test.cpp`. CLI `--tick` consumes B1 (`examples/ember_cli.cpp`). |
| In-context threads (`thread` addon) | **DEFERRED** | Non-goal; one-context-per-thread is the v1 model. ROADMAP: "defer as long as possible." |
| Coroutines / `yield` | **DEFERRED** | No `yield` syntax. Dep: heap/GC for suspended-frame storage. |
| Exceptions `try`/`catch`/`throw` | **DEFERRED** | No try/catch syntax. Host-side `runtime_error`/`runtime_exception` (non-local unwind, `docs/spec/SAFETY_AND_SANDBOX.md` §7) covers host→script abort. |

**Tier 5 verdict: the two pieces short of in-context threading SHIPPED; the rest DEFERRED** exactly as the ROADMAP's "Shipped v1.0" block states.

### Tier 6 — language ecosystem

| Feature | Status | Evidence |
|---|---|---|
| Modules / live `link` | **SHIPPED** | `link "foo.em" as foo;` parses (`src/parser.cpp` `parse_link`); `foo::bar()` cross-module calls resolve (`src/sema.cpp` CallExpr `module_alias` path); `ModuleRegistry` + kind-2 reloc (`src/module_registry.{hpp,cpp}`, `src/em_loader.cpp`); `--emit-em` CLI mode. Pinned by `examples/v0_5_live_modules_test.cpp`. **Open:** bare-name `link "foo" as foo;` to an already-registered module is host-driven in v0.5 (a future linker could resolve bare names against a search path). |
| Namespaces | **DEFERRED** | No `namespace` syntax. ROADMAP: "Dep: modules (now shipped)." Re-entry trigger: module size makes flat scope crowded. |
| Preprocessor | **NEVER** (hard non-goal) | `engine.define(name,value)` + `const`/`constexpr` + modules cover the legitimate needs. |

**Tier 6 verdict: modules SHIPPED; namespaces DEFERRED; preprocessor NEVER.**

### Investigation-backed candidates

| Item | Status | Evidence |
|---|---|---|
| Runtime string encryption | **SHIPPED (DONE)** | Inline-stack-XOR lowering; `Program::string_xor_key` (CLI default `0xA5`, ON). `src/codegen.cpp` StringLit eval / `alloc_str_temp`. The const/non-const classification the original analysis recommended was NOT implemented (every encrypted literal takes the same path) — documented as safe. |
| Slice-of-stack-local escape safety | **PARTIALLY SHIPPED (Stage 1 DONE, Stage 2 DEFERRED)** | `src/sema.cpp` `is_local_array_view` covers ViewExpr + StringLit→slice; guards close C1 (return), C2a (global-Ident-store), C2b (global-rooted FieldExpr/IndexExpr-store). **C3 (slice passed to a native that may retain) and C5 (slice passed to a script fn / fn-handle / cross-module call that may retain) are NOT guarded** — the residual live hole. Needs a real borrow/escape analysis + a `borrows`/`retains` annotation on `NativeSig`. |
| Codegen optimization | **PARTIALLY SHIPPED** | Stage 1 (peephole + local regalloc) DONE; Stage 2 Stage A (thin three-address IR, `src/thin_ir.{hpp,cpp}` + `thin_lower` + `thin_emit`) SHIPPED behind `enable_ir_backend` (default off); Stage 2 full (carrying peephole/regalloc over as ThinPasses + cross-block CSE/LICM) and Stage 3 (full SSA-lite + linear-scan) DEFERRED. Pass system (Stage C Steps 1-5) SHIPPED: constprop/dce/cse/licm (opt) + subst (obf); Steps 6 (FlatteningPass/MBAPass/bogus-CF) + `EmberAnalysisManager` FUTURE. |

**Investigation-backed verdict: all three are tracked decisions, exactly as the ROADMAP records.** The benchmark gate (5-9× on 5/6 paths) is confirmed; the optimization is evidence-gated, not speculative.

### CLI tooling

| Family / command | Status | Evidence |
|---|---|---|
| `ember run` | **SHIPPED** | `examples/ember_cli.cpp` — `--fn`, `--dump`, `--emit-em`, `--tick`/`--tick-count`/`--tick-interval`, `--passes`, `--load-em`, `--verify-em-key`. |
| `ember emit-em` | **SHIPPED** | Positional `<input> <output>` precompile. |
| `ember bench` | **SHIPPED** | Family A — warmup + N timed iters, min/median/mean/p99/max/stddev/CV% + return value + machine/compiler/date metadata. |
| `ember test` | **MISSING** (ROADMAP: "NEXT") | No `test` subcommand in `ember_cli.cpp`. ROADMAP: "Replaces the bash harness with a fast, parallel, TAP-ish runner. Blocked on a small refactor: extracting the compile-to-entry flow from `main` into a reusable helper." The bash harness `tests/run_lang_tests.sh` is the current runner. |
| Family B (`io` extension: print/println/read_line/read_file/write_file + argv) | **DEFERRED** | No `io` extension exists. `ember_cli.cpp` comment: "There is no print_i64 in the standard extension set... so YAGNI: keep it to exit-code-as-signal." ROADMAP: deferred because "it's a language feature + a security decision... re-entry trigger: a demo or real use genuinely blocked on output beyond the exit code." **This is the biggest day-to-day gap for a modder** (see §6). |
| Family C (`ember pipe`, `ember live`) | **DEFERRED** | Neither exists. ROADMAP: "Build after those demos prove the underlying APIs are pleasant enough to expose as a tool." |

### Slated for removal

| Item | Status | Evidence |
|---|---|---|
| `auto x = expr;` | **PARTIALLY SHIPPED (deprecated, not yet removed)** | Still parses (`src/parser.cpp` `parse_let_stmt` handles `Kw_auto`), still sema-checks, emits a **non-fatal warning** (`src/sema.cpp:1625`). `let x = expr;` is the canonical inference form. Removed from in-tree tests per ROADMAP; removal from the language after a grace period. |

### Hard non-goals (never)

`goto`, C-preprocessor (`#include`/`ifdef`/`pragma`), multiple inheritance/diamond, self-hosting — all confirmed absent from the grammar and ROADMAP.

---

## 2. SHIPPED ahead of / with the v1.0 batch (ROADMAP's own markers, verified)

- **`.em` binary bundling** — SHIPPED v0.1→v0.2. `src/em_file.hpp`/`em_writer.cpp`/`em_loader.cpp`. Now at **v5** (IL-`.em` IR blob) with **v4 Ed25519 signed bundles** (`thirdparty/ed25519/`, `EmVerifyPolicy` keyring, `--verify-em-key`). Pinned by `em_roundtrip`, `em_signed`, `em_v5_ir`, `em_loader_hardening` ctests.
- **First-class struct/aggregate** (struct-literal return, struct-by-value arg temps, array literals, aggregate globals) — SHIPPED 2026-07-10. `src/codegen.cpp` §16; `docs/spec/TYPE_SYSTEM.md` §12, `docs/spec/CODEGEN_SPEC.md` §16. Pinned by `binding_abi`, `array_lit`, `aggregate_global` ctests. **Caveat:** three of these four sub-features have confirmed edge-case defects (C1, C2, C3 — see §4).

---

## 3. Constructs that parse but don't work (the "do-while bug" category)

> The task asks specifically about "constructs that parse but don't work (like the old do-while bug)." The old do-while bug is **fixed** (§1 Tier 1). What follows are the constructs that *currently* parse and sema-accept but miscompile or crash. These are the live "parses but doesn't work" surface.

### 3.1 `constexpr` — a keyword that lexes but has no grammar (MISSING, spec-claimed)

**The clearest "parses into a token but can't be used" gap.** `constexpr` is a lexed keyword (`src/lexer.cpp:13` → `Kw_constexpr`) but:

- `parse_let_stmt` (`src/parser.cpp`) handles `Kw_let`/`Kw_auto`/`Kw_const` — **not** `Kw_constexpr`.
- `parse_program` handles `Kw_fn`/`Kw_struct`/`Kw_global`/`Kw_const`/`Kw_link`/`Kw_enum` — **not** `Kw_constexpr`.
- `parse_stmt`'s default case would call `parse_expr` → `parse_primary`, which hits its default and throws `unexpected token 'constexpr'`.

So **using `constexpr` in source produces a parse error.** No `.ember` file in the tree uses it as a keyword (verified: `grep -rn "constexpr " tests/lang/*.ember examples/scripts/*.ember` → zero hits; the only `tests/lang` reference is a *comment* in `runtime_language_features.ember:46` describing the concept).

**Why this matters (spec drift):** the specs claim a restricted `constexpr` works:
- `docs/spec/COMPILER_PIPELINE.md` §1 lists `constexpr` in the keyword set; §6 says "`constexpr` (when used as an array size, `offsetof`/`sizeof` arg, or `switch` case label) is **folded at sema** to a literal."
- `docs/spec/TYPE_SYSTEM.md` §11.5 says "`constexpr` ... Used in: array sizes (`let buf: [u8; constexpr 256]` ... `const N: u32 = 256; let buf: [u8; N]`), `switch` case labels..."

Neither is true. The array-size path is the most concrete: `src/parser.cpp:88` is `expect(Tk::IntLit, "integer array size")` — a named `const` used as an array size is a parse error (it's an `Ident`, not an `IntLit`). `docs/spec/TYPE_SYSTEM.md` §3 is honest ("N must literally be a literal in v1; named compile-time constants are a post-v1 nicety"), but §11.5 contradicts it.

**Classification:** the *keyword* is a **MISSING** feature with a **stale/overstated spec**. The ROADMAP correctly lists `constexpr` function evaluation as DEFERRED (Tier 1) — but the *lexed keyword + the spec claims* make this worse than a pure deferral: a user reading the spec would expect `const N: u32 = 256; let buf: [u8; N]` to work.

### 3.2 Confirmed codegen/sema defects (C1–C6) — parse + sema-accept, miscompile or crash

These six were filed as **CONFIRMED DEFECT** in `docs/audit/AUDIT_CODE_CORRECTNESS_2026-07-11.md` (audited at `7c73361`, one cosmetic commit behind `bf76217`). The `SYNTHESIS` doc says S1/S2 (security) were fixed in `d25cc8c` but **C1–C6 were "not yet fixed — tracked for follow-up."** I re-verified each against the `d25cc8c` source: **all six are still present at HEAD.** Each is a construct that **parses and sema-accepts** but produces silent wrong-code or a crash.

| ID | Construct | Symptom | Files (verified at `d25cc8c`) | Status |
|---|---|---|---|---|
| **C1** | `arr[i].field` on an array/slice of structs | Tree-walker: emits nothing (exit=44 for expected 10). IR: returns VReg 0 (exit=80). **Silent wrong-code.** | `src/codegen.cpp:2773` FieldExpr eval only handles `Ident` base (comment at :2770: "v1 scope: base must be a bare local Ident"); `src/thin_lower.cpp:1887` same. Sema accepts it (`src/sema.cpp` FieldExpr case). | **CONFIRMED DEFECT — unfixed** |
| **C2** | `for (x in slice)` with element size ∉ {1,2,4,8} | Wrong SIB scale (`else scale = 3` → ×8 for esz=16) + truncated 8-byte element copy. exit=130 for expected 60. **Silent wrong-code.** | `src/codegen.cpp:3218-3223` (tree-walker; IR falls back to tree-walker for for-each, `src/thin_lower.cpp:949`). `tests/lang/valid_for_each.ember` only exercises `i64[]` (esz=8), so the suite passes. | **CONFIRMED DEFECT — unfixed** |
| **C3** | `g[..]` view of a global fixed array | Tree-walker: emits nothing (exit=15 for expected 2, garbage slice). IR (`--passes dce`): **SIGSEGV** (exit=139). | `src/codegen.cpp:2758` ViewExpr only handles local `Ident` base (misses `locals.find` for a global); `src/thin_emit.cpp:1275-1278` MakeSlice always emits `lea rax, [rbp+frame_off]`, ignoring the `base_kind=GlobalsBase` the lowering set (`src/thin_lower.cpp:1838` et al.). Sema accepts it. `aggregate_global_test` never tests `g[..]`. | **CONFIRMED DEFECT — unfixed** |
| **C4** | IR-backend int compare with an IntLit > 32 bits | `emit_cmp` truncates the immediate to `int32_t`. `if (x == 0x1234567890)` → exit=0 (wrong) under `--passes`. **IR-only; tree-walker correct.** | `src/thin_emit.cpp:1554` `e.cmp_reg_imm32(Reg::rax, int32_t(in.imm.i))`. The binop immediate path (`src/thin_emit.cpp:1360`) is NOT affected (it range-checks). | **CONFIRMED DEFECT — unfixed** (IR backend, default off) |
| **C5** | u64 literals in `[2^63, 2^64-1]` | Lexer accepts the full u64 range (bit-cast to int64), but `adapt_int_lit`'s U64 arm gates on `v >= 0` → a ≥2^63 literal (negative as int64) is not adapted → "let type mismatch (u64 = i64)" compile error. | `src/lexer.cpp:271` (`stoull` → `int64_t` bit-cast); `src/sema.cpp:681` `case Prim::U64: if (v >= 0)`. The two halves disagree. | **CONFIRMED DEFECT — unfixed** |
| **C6** | f64 global initializers | Folded through `try_eval_const_f32` (no `try_eval_const_f64` exists), then widened via `double dv = double(v)` — **round-trips through f32, losing precision.** `global g : f64 = 3.141592653589793;` bakes a truncated value. | `src/globals.hpp:88` (`fold_scalar_init`) + `:197` (`eval_global_initializers`) — both use `try_eval_const_f32` for all float globals, widening to f64 via `double dv = double(v)`. | **CONFIRMED DEFECT — unfixed** |

**Why these escaped the suite:** each is exercised by a construct no in-tree test hits — `arr[i].field` on a struct array, for-each over a non-8-byte element, `g[..]` on a global, an IR-backend >32-bit literal compare, a ≥2^63 u64 literal, and an f64 global with a precision-sensitive initializer. The lang suite and ctests are green (the SYNTHESIS reports 274/0/0 lang + 32/32 ctest at `bf76217`), but green because the bugs are in untested corners.

**These are the real "parses but doesn't work" surface at HEAD.** They are not ROADMAP deferrals — they are defects against already-shipped features. C1 and C3 are the most dangerous (silent wrong-code on constructs the type system admits; C3 is a crash under `--passes`).

---

## 4. Features mentioned in design docs (`docs/spec/`) that aren't accurately reflected in the source

These are spec-vs-implementation drifts — places where a spec describes behavior the source doesn't provide (or forbids behavior the source ships). Most are stale text predating a shipped feature; a few overstate a deferred feature.

| # | Spec claim | Source reality | Drift type |
|---|---|---|---|
| D1 | `docs/spec/COMPILER_PIPELINE.md` §1 + `docs/spec/TYPE_SYSTEM.md` §11.5: `constexpr` is a working keyword (folded at sema; usable as array sizes / case labels) | `constexpr` has no parser/sema support; using it is a parse error; named const as array size is rejected (`parser.cpp:88` `expect(IntLit)`) | **Overstates a MISSING feature** (see §3.1) |
| D2 | `docs/spec/TYPE_SYSTEM.md` §7 + `docs/spec/COMPILER_PIPELINE.md` §4: "arr[i] requires i to be an unsigned integer type — signed integer indices are a compile error" | `src/sema.cpp:1416` only checks `it->is_int()` — **any** signed or unsigned int is accepted | **Spec stricter than implementation** (implementation more permissive) |
| D3 | `docs/spec/COMPILER_PIPELINE.md` §1: "INT_LIT (e.g. 42, 42u, 42i64 - optional type suffix)" | `src/lexer.cpp:245` rejects integer width suffixes: "integer literal width suffixes are unsupported; use an explicit `as` cast" | **Spec claims a feature the lexer rejects** (float `f` suffix works; int suffixes don't) |
| D4 | `docs/spec/COMPILER_PIPELINE.md` §1 + `docs/spec/TYPE_SYSTEM.md` §6: "no raw strings" | `src/lexer.cpp:155-177` ships `r"""..."""` triple-quoted raw strings (no escape processing) | **Spec says a feature doesn't exist; it does** (stale, predates the raw-string addition) |
| D5 | `docs/spec/MEMORY_AND_GC.md` §6: "Strings are `slice<u8>` — no distinct `string` type, no owned/heap string type in v1" | `extensions/string/` ships an owned opaque `string` handle; README says "`string` (owned, via the string extension)"; sema implicitly converts literals to `string` | **Stale** (predates the string extension) |
| D6 | `docs/planning/GAP_ANALYSIS.md` §2: "Default args ✗ v1" | Default params ship: `src/parser.cpp` `parse_param` handles `= literal`; `src/sema.cpp` synthesizes missing trailing args; tests `tests/lang/{invalid_default_*,sema_invalid_default_*}.ember` | **Stale** (default args shipped) |
| D7 | `docs/planning/GAP_ANALYSIS.md` §3: "There is no map extension. General array pop/remove/clear and generic push, string find/substr/format APIs, and a broader math surface are deferred" | `map` shipped (`extensions/map/`); array pop/clear/remove shipped; string find/substr shipped; math f64 + abs_i64 shipped | **Stale** (predates the 2026-07-11 addon expansion) |
| D8 | `docs/spec/COMPILER_PIPELINE.md` §2 BNF | The main BNF does not list `match`, `for-each` (`in`), `enum`, `fn`-type, `&fn`, `link`, `priv`, array literals, or `=>`/`::` — all shipped v1.0 forms. §2a documents them separately, but the BNF block itself is stale. | **Stale BNF** (doc-maintenance gap, not a code gap) |
| D9 | `docs/spec/SPEC_AUDIT_2026-07-10.md` F3/F5/F6/F7 | These four were already remediated in the spec docs per the SPEC_AUDIT's own "DONE" markers (F3 aggregate-globals text, F5 `auto` deprecation note, F6 CTest count, F7 string-encryption path). F1/F2 (pub/priv, signed `.em`) implemented. **F4 (slice-escape C3/C5) is the one still-open spec-vs-impl gap** — and it's documented as Stage-2-deferred in both the spec and the ROADMAP. | **Mostly remediated; F4 tracked** |

**Note:** `docs/spec/SPEC_AUDIT_2026-07-10.md` already did this spec-vs-impl exercise and remediated F1/F2/F3/F5/F6/F7 (doc fixes) + F1/F2 (impl). The drifts D1–D8 above are **additional** items the SPEC_AUDIT did not flag (D1 `constexpr`, D2 unsigned-index, D3 int-suffixes, D4 raw-strings, D5 string-type, D6 default-args, D7 map/addons, D8 BNF) — they are the residual spec-staleness surface.

---

## 5. TODO / FIXME / HACK comments in the source

**There are zero strict `TODO`/`FIXME`/`HACK`/`XXX`/`UNIMPLEMENTED` comments in `src/`** (verified: `grep -rn -E "TODO|FIXME|HACK\b|XXX\b|UNIMPLEMENTED|NOT IMPLEMENTED|KLUDGE" src/` → no matches). The codebase uses "deferred" exclusively as **intentional design documentation** (e.g. `// STAGE 2 (deferred): C3...` in `src/sema.cpp:612`, `// ForEachStmt fallback: for-each is not yet lowered to ThinFunction IR` in `src/thin_lower.cpp:949`), not as unfinished-work markers.

The `extensions/` tree has two "deferred" notes, both already ROADMAP-tracked:
- `extensions/math/ext_math.cpp:20,41` — "f64 variants (deferred Tier 0 — broader f32/f64 math)" — then shipped (the f64 variants are present at lines 42-50; the comment is stale within the file).
- `extensions/sync/ext_sync.hpp:41` — "No script-visible mutex -- deferred per the [ROADMAP]."

**The one genuine "incomplete-work" marker** is `src/ember_pass.hpp:150`: `// ─── Analysis manager (stub for the first Stage C cut) ───` — the `EmberAnalysisManager` is an empty stub (PASS_SYSTEM_DESIGN §6/§8 mark it FUTURE). This is documented, not hidden.

**Verdict:** the source is notably clean of drive-by TODOs. Incomplete work is either (a) a confirmed defect filed in the audit docs (C1–C6) or (b) an explicitly ROADMAP-tracked deferral with a named re-entry trigger.

---

## 6. The gap between what ember CAN do and what a game modder would NEED

ember's shipped surface covers the **structural** needs of a game modder well: structs + arrays + slices + enums + vec/quat/mat + a real sandbox + hot-reload + `.em` bundling + cross-module linking + a tick lifecycle + per-thread contexts + sync queues. The README's archer example compiles and runs. The gaps are in **ergonomics, output, and a few correctness corners**.

### What a modder needs that ember CAN'T do today (ordered by day-to-day friction)

1. **Output / I/O (the biggest gap).** A script can only signal via its integer exit code. There is no `print`/`println`/`read_line`/`read_file`/`write_file` — the `io` extension (CLI Family B) is deferred. A modder cannot log, cannot read a config file, cannot write a result. Debugging is "return a different number and check the exit code." *This is the single feature whose absence makes ember unusable as a standalone scripting language.* (ROADMAP re-entry trigger: "a demo or real use genuinely blocked on output beyond the exit code" — that trigger is arguably already fired for anyone trying to write a real mod.)

2. **String formatting.** F-strings work (`f"hp={hp}"`) but only through `string_from_i64`/`string_from_f32`/etc. — no width/precision/specifier control, no `__fmt`. A modder building a HUD or log line gets no padding, no hex format, no float precision control. (`string` extension is "PARTIALLY SHIPPED" — find/substr yes, general format no.)

3. **`arr[i].field` and `g[..]` actually working (C1, C3).** A game modder iterating entities stored in a struct array and reading a field — `entities[i].health` — gets **silent garbage**. A modder slicing a global config array — `config_ranges[..]` — gets garbage (tree-walker) or a **crash** (`--passes`). These are not exotic constructs; they are the first things a modder writes. (C1/C3 are confirmed defects, not deferrals — see §3.2.)

4. **`for (p in entities)` over a slice of structs (C2).** The natural idiom for iterating entities gives the wrong element and a truncated copy for any struct >8 bytes. The modder's fallback (C-style `for` with manual indexing) works, but the ergonomic form is a trap.

5. **No `map` over non-i64 keys/values.** `map<K,V>` is i64-only (ROADMAP: "typed keys/values are a v2 concern"). A modder wanting `map<string, Entity>` or `map<vec3, Cell>` can't express it; they key by integer id.

6. **No classes / interfaces / virtual dispatch (Tier 3, deferred).** A modder with >1 entity type sharing a base interface falls back to structs + free functions + (now) `fn` handles for callbacks. This is workable for a small mod, unwieldy for a large one. The ROADMAP's re-entry trigger ("a real game has >1 entity types sharing a base interface") has not fired in-tree.

7. **No exceptions / try-catch (Tier 5, deferred).** A modder who wants local recovery (catch a bad index, retry) can't; a trap unwinds the whole call to the host. The host-side `runtime_error`/`runtime_exception` is the only error channel.

8. **No coroutines / `yield` (Tier 5, deferred).** A modder writing a cutscene or AI sequence-style can't; they must drive it from a tick callback. Dep: heap/GC.

9. **No lambdas with capture (Tier 4, deferred).** Callback-style APIs use `&fn` handles (no capture) or `register_routine(fn, data)`. A modder wanting `sort(arr, |a, b| a.hp < b.hp)` can't. Dep: GC for by-reference capture.

10. **No named compile-time constants as array sizes / case labels (D1/§3.1).** `const N: u32 = 256; let buf: [u8; N];` is a parse error — the modder must inline the literal. The spec says this works; it doesn't.

### What a modder needs that ember CAN do (the shipped strengths)

- Structs (value type, MSVC-compatible layout), fixed arrays, slices, array literals, struct literals, aggregate globals.
- Full control flow: if/else, while, do-while, for, for-each (over slices), switch (no fallthrough), match (literal patterns), break/continue/return, defer (lexical-block-exit LIFO).
- Enums (untyped i32 constants, `E::A`), `fn` handles + indirect calls (guarded), operator overloads (vec/quat/mat/string `+`/`==`).
- A real sandbox: per-frame byte budget, stack-depth guard, bounds checks (always on), div/overflow traps, `PERM_FFI` gating, recoverable non-local unwind, call-target-provenance guard.
- Hot-reload (single-fn, stable slots), `.em` bundling (signed v4), cross-module `link` + `mod::fn()`, `@entry`/`@on_tick`/`@event` lifecycle, dynamic `register_routine`.
- Per-thread `context_t` + sync queues for multi-context coordination.
- String encryption (inline-stack-XOR, on by default), obfuscation passes (`subst` MBA), optimization passes (constprop/dce/cse/licm) behind `--passes`.

---

## 7. Prioritized "what to build next" list

Ordered by the ratio of (user value × risk-if-absent) to (implementation cost). The top of this list is **not** roadmap deferrals — it's the confirmed defects and the I/O gap.

### P0 — Fix the confirmed defects (C1, C2, C3) [correctness, not features]

These are silent wrong-code / crashes on constructs sema admits. They are the highest priority because they violate the language's own type-soundness contract and a modder hits them immediately.

1. **C1 — `arr[i].field` on struct arrays/slices.** Codegen the `IndexExpr` base for `FieldExpr` (load the element to a temp, read the field at its offset). Both backends. Add a lang test: `struct P { a: i64; b: i64; } ... return arr[0].a;` → 10.
2. **C3 — `g[..]` view of a global fixed array.** Codegen the global-`Ident` base for `ViewExpr` (lea the global's typed offset, not a frame slot). Fix `MakeSlice` in `thin_emit.cpp` to honor `base_kind=GlobalsBase`. Add a lang test: `global g: i64[3] = [1,2,3]; ... return g[..][1];` → 2.
3. **C2 — for-each over non-{1,2,4,8}-byte elements.** Replace the `else scale = 3` SIB-scale branch with an `imul`-based element-address computation (the IndexExpr path next door already does this correctly — reuse it), and fix `load_elem_to_rax`/`store_rax_elem` to copy `esz` bytes for widths >8. Add a lang test: for-each over a 16-byte struct slice, sum a field.
4. **C5 — u64 literals ≥ 2^63.** Fix `adapt_int_lit`'s U64 arm (`src/sema.cpp:681`) to recognize the bit-cast negative as a valid u64 (compare the unsigned bit pattern, not `v >= 0`). Add a lang test: `let x: u64 = 18446744073709551615;`.
5. **C6 — f64 global initializers.** Add `try_eval_const_f64` (mirror `try_eval_const_f32` in `double`) and use it in `globals.hpp` for `Prim::F64` globals. Add a lang test: `global g : f64 = 3.141592653589793;` read back with a precision check.
6. **C4 — IR-backend int compare with >32-bit literal.** Add the `mov_reg_imm64` + `cmp_reg_reg` fallback to `emit_cmp` (`src/thin_emit.cpp:1554`) for literals outside imm32 range (the binop path already does this). Add an IR-path test. Lowest priority of the six (IR backend is default-off; only bites under `--passes`).

### P1 — The `io` extension (CLI Family B) [biggest day-to-day gap]

Ship a small `io` extension registered in the CLI: `print`/`println` (ungated), `read_line`/`read_file`/`write_file` (`PERM_FFI`-gated), + argv access. This is the re-entry trigger the ROADMAP names ("a demo or real use genuinely blocked on output beyond the exit code") — and that trigger has effectively already fired for any modder who can't debug via exit codes. Pure host C++ against the v1 `NativeFn` API; zero JIT/type-system changes. Unblocks debugging, logging, config loading, and code-gen/templating CLI tools.

### P2 — `ember test` CLI subcommand (CLI Family A) [test ergonomics]

Replace the bash harness (`tests/run_lang_tests.sh`) with a fast, parallel, TAP-ish runner. ROADMAP: "Blocked on a small refactor: extracting the compile-to-entry flow from `main` into a reusable helper so `test` can call it per-file." Low risk; the bash harness already defines the classification convention (`valid_*`/`invalid_*`/`sema_valid_*`/`sema_invalid_*`/expected exit codes).

### P3 — Spec reconciliation (D1–D8) [documentation correctness]

Fix the spec docs to match the source. This is low-cost, zero-risk, and prevents users from relying on behavior that doesn't exist:
- **D1:** remove the `constexpr`-works claims from `COMPILER_PIPELINE.md` §1/§6 and `TYPE_SYSTEM.md` §11.5 (or, better, implement the restricted form — see P4). Either way, the spec must stop claiming it.
- **D2:** decide — enforce the unsigned-index rule in sema (matching the spec) or relax the spec to match the implementation. The implementation (any-int index) is the more ergonomic choice; the spec (unsigned-only) is the safer one. Pick one.
- **D3:** remove the int-suffix claim from `COMPILER_PIPELINE.md` §1 (the lexer rejects them; keep the float `f`-suffix note).
- **D4:** add raw strings (`r"""..."""`) to `COMPILER_PIPELINE.md` §1 + `TYPE_SYSTEM.md` §6.
- **D5:** update `MEMORY_AND_GC.md` §6 to reflect the owned `string` handle (or note the two representations: `slice<u8>` literal vs owned `string` handle).
- **D6/D7:** update `GAP_ANALYSIS.md` §2 (default args shipped) + §3 (map shipped; array/string/math expanded).
- **D8:** refresh the `COMPILER_PIPELINE.md` §2 BNF to include `match`/`for-each`/`enum`/`fn`-type/`&fn`/`link`/`priv`/array literals/`=>`/`::`.

### P4 — Restricted `constexpr` (named const as array size / case label) [small language extension]

If P3-D1 chooses "implement" rather than "remove the claim": wire the lexed `Kw_constexpr` into the parser + sema so that `const N: u32 = 256; let buf: [u8; N];` and `switch` case labels can reference a named `const`. This is a small extension to the existing `try_eval_const_i64` folder (already used for enum variant values + assert folding) — resolve a `const` Ident to its literal value at the array-size / case-label position. ROADMAP Tier 1 lists `static_assert` + full `constexpr` fn eval as deferred; this is the *narrow* form the spec already claims, not the full const-eval interpreter.

### P5 — `string_format` / richer formatting [addon breadth]

Add a `string_format` native (or a `__fmt` lowering) for width/precision/hex control. The f-string pipeline already threads through `__fstring_to_string` → `string_from_*`; a format spec would extend the sentinel resolution. Medium cost; unblocks HUD/log-line formatting (P1 item 2).

### P6 — Broaden `math` (f32 floor/ceil/abs/pow, atan2/log/exp, min/max/clamp) [addon breadth]

The f64 surface shipped; the f32 surface is just sqrt/sin/cos/tan. A game modder wants `floor_f32`, `clamp`, `min`/`max`, `atan2`. Pure host C++ additions to `extensions/math/`. Low cost.

### P7 — `map` test coverage [test gap]

`map` has only lang-suite coverage (4 `valid_map_*.ember` files) and **no ctest-level test target** (the SYNTHESIS flagged "map extension has zero test coverage" — now partially addressed by lang tests, but no ctest). Add an `ext_map_test.cpp` ctest mirroring `ext_sync_test`/`ext_lifecycle_test`. Low cost; closes a coverage gap on a shipped addon.

### Deferred-but-tracked (do NOT build until the ROADMAP trigger fires)

- Classes/interfaces/mixins/templates (Tier 3) — trigger: >1 entity types sharing a base interface.
- GC / `new` / lambdas-with-capture (Tier 4) — trigger: Tier 3 lands.
- Coroutines/`yield`, try/catch/throw, in-context threads (Tier 5) — trigger: real demand; coroutines dep GC.
- Namespaces (Tier 6) — trigger: module size makes flat scope crowded.
- Codegen Stage 2 full + Stage 3 (SSA-lite + linear-scan) — gated on Stage A insufficiency / cross-block evidence.
- Slice-escape Stage 2 (C3 native-arg + C5 script-fn-arg) — needs borrow/escape analysis + `NativeSig::borrows`/`retains`.
- CLI Family C (`ember pipe`, `ember live`) — gated on demo ergonomics.
- `auto` removal — after a grace period.
- Bare-`fn` parameterized function types (`fn(i64)->i64`) + cross-module function handles — v2+ (ROADMAP Tier 2 open items).

---

## 8. Summary table

| Category | Count | Examples |
|---|---:|---|
| ROADMAP features SHIPPED | 18 | enum, for-each*, match*, fn-refs*, dynamic reg, all Tier 0 addons, modules, sync+thread-safety, string encryption, struct/aggregate, `.em` bundling, CLI run/emit-em/bench, codegen Stage 1 + Stage A, pass system Steps 1-5 |
| ROADMAP features PARTIALLY SHIPPED | 6 | for-each (IR fallback), match (IR fallback), fn-refs (bare-fn hole + no cross-module), string (no format), math (f32 thin), slice-escape (Stage 1 only) |
| ROADMAP features DEFERRED (trigger-gated) | 13 | classes/interfaces/mixins/templates, GC/new/lambdas, coroutines/yield, try/catch/throw, in-context threads, namespaces, static_assert, full constexpr, codegen Stage 2 full + Stage 3, slice-escape Stage 2, CLI Family B + C |
| ROADMAP features MISSING (not deferred, not scheduled) | 1 | `ember test` (ROADMAP marks it "NEXT" — intended but not yet built; the only item in that category) |
| Hard non-goals (never) | 4 | goto, preprocessor, multiple inheritance, self-hosting |
| CONFIRMED DEFECTS (parses, miscompiles/crashes) | 6 | C1 arr[i].field, C2 for-each non-pow2, C3 g[..] global view, C4 IR >32-bit cmp, C5 u64 ≥2^63 literal, C6 f64 global precision |
| Spec-vs-impl drift (D1–D8) | 8 | constexpr claimed, unsigned-index claimed, int-suffixes claimed, raw-strings denied, string-type denied, default-args denied, map/addons denied, stale BNF |
| Strict TODO/FIXME/HACK in `src/` | 0 | — (incomplete work is in audit docs or ROADMAP-tracked deferrals) |

**The two things to do first:** fix C1/C2/C3 (P0 — silent wrong-code on constructs a modder writes immediately), and ship the `io` extension (P1 — the only thing making ember unusable as a standalone scripting language). Everything else is either a smaller correctness fix (C4–C6), a doc reconciliation (P3), or a ROADMAP-tracked deferral with a named re-entry trigger that hasn't fired.

---

*Audit method: read-only. Every source claim verified against `src/`, `extensions/`, `examples/ember_cli.cpp` at HEAD `d25cc8c`. ROADMAP cross-referenced line-by-line. Defects C1–C6 cross-checked against `docs/audit/AUDIT_CODE_CORRECTNESS_2026-07-11.md` and re-verified present at HEAD. No source files were edited.*
