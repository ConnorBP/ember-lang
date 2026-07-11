# ember — Documentation & Test Coverage Audit (2026-07-11)

**Auditor:** read-only deep audit (no source edited)
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** `bf76217`
**Build:** `buildt/` — MinGW g++ 15.2.0, C++17, Release
**Baseline confirmed:** `ctest` → **32/32 PASS** (85.26s); `tests/run_lang_tests.sh buildt` → **274 passed, 0 failed, 0 skipped**
**Scope:** every doc in `docs/` (incl. `docs/spec/`, `docs/planning/`, `docs/audit/`) + every test in `tests/lang/` + every `examples/*_test.cpp` ctest target. Scratch space `tmp_edit/` is gitignored and excluded.

Findings are tagged **STALE** (doc claim contradicts current source/reality), **INACCURATE** (doc is wrong/self-contradictory), **MISSING** (feature exists in source but is undocumented or untested), or **VERIFIED CURRENT** (checked against source, accurate). Exact `file:line` citations throughout.

The new features the task named — for-each, match, map, passes, LICM, SubstitutionPass — were each located in source and traced against every doc and every test. The headline result: **all six ship and build green, but the spec/grammar docs and the GAP_ANALYSIS were not updated for them, the map extension has zero test coverage, and the pass-system spec still marks LICM/SubstitutionPass as future.**

A process note that explains the pattern: `docs/MAINTENANCE_CONSTRAINTS.md` ("No changes to `docs/spec/` design docs — these are design decisions, not maintenance items. Fix typos/cross-refs but do not change the design") forbids the hourly maintenance cron from updating spec docs, so the spec layer systematically lags shipped features. That is a policy choice, not a finding against the cron — but it is *why* most STALE/MISSING findings below land in `docs/spec/` and `docs/planning/`.

---

## A. STALE findings (doc claim contradicts current source)

### A1. ROADMAP marks LICM + SubstitutionPass as FUTURE; both are shipped
- `docs/ROADMAP.md` (the CODEGEN_OPTIMIZATION_DESIGN §8 reproduction inside the "Codegen optimization" candidate-changes entry) and, authoritatively, `docs/spec/PASS_SYSTEM_DESIGN.md:312-313`:
  - `docs/spec/PASS_SYSTEM_DESIGN.md:312` — "Step 4 — FUTURE. `EmberAnalysisManager` (when a pass needs it) + `LICMPass` (the first pass that needs a CFG/loop analysis)."
  - `docs/spec/PASS_SYSTEM_DESIGN.md:313` — "Step 5 — FUTURE. Obfuscation passes (`extensions/obf/ext_obf.cpp`) — `SubstitutionPass`, `FlatteningPass`, `MBAPass`, with `is_required = true`."
- **Source reality:** `LICMPass` is implemented and registered as `"licm"` in `extensions/opt/ext_opt.cpp` (`LICMPass::run` + `register_passes` at file bottom: `reg.add<LICMPass>("licm");`). `SubstitutionPass` is implemented and registered as `"subst"` in `extensions/obf/ext_obf.cpp` (`SubstitutionPass::run`, `is_required = true`, `reg.add<SubstitutionPass>("subst");`). Both are exercised by `examples/ir_passes_test.cpp` (ctest `ir_passes`, PASS): registry `has("licm")`/`has("subst")`, value-preservation on all four workloads, and instr-count change checks.
- **Verdict:** STALE. (Note: `FlatteningPass` and `MBAPass` named in Step 5 are NOT shipped — only `SubstitutionPass` is — so Step 5 is *partially* stale: SubstitutionPass shipped, the other two remain future.)

### A2. CODEGEN_OPTIMIZATION_DESIGN §8 says Steps 4-5 remain FUTURE and lists "the first three" passes
- `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md:982` — "the first three IR optimization passes:" followed at `:983-989` by `ConstPropPass`, `DeadCodeElimPass`, `CSEPass` only.
- `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md:995` — "Steps 4-5 (AnalysisManager + LICM, obfuscation passes) remain FUTURE."
- **Source reality:** there are now **four** opt passes (`ConstPropPass`, `DeadCodeElimPass`, `CSEPass`, `LICMPass`) plus the obf `SubstitutionPass` — see `extensions/opt/ext_opt.hpp` (four struct decls) and `extensions/obf/ext_obf.hpp`.
- **Verdict:** STALE. Line 982 "first three" and line 995 "remain FUTURE" both predate the LICM + SubstitutionPass shipment.

### A3. PASS_SYSTEM_DESIGN.md header Status line is stale
- `docs/spec/PASS_SYSTEM_DESIGN.md:3` — "**Status:** design (pre-implementation)."
- **Source reality:** §8 of the same file says "Step 1 — SHIPPED (2026-07-11)", "Step 2 — SHIPPED (2026-07-11)", "Step 3 — SHIPPED (2026-07-11)" (`:307`, `:309`, `:311` in the §8 migration list). The infrastructure (`src/ember_pass.{hpp,cpp}`, `src/ember_pass_registry.hpp`, `src/ember_pass_pipeline.hpp`) and the `ember_ext_opt`/`ember_ext_obf` libs all ship and are ctest-gated (`ember_pass`, `ir_passes`).
- **Verdict:** STALE. The header says pre-implementation while the body says shipped.

### A4. ext_opt.hpp header comment says "Three IR→IR optimization passes" — there are four
- `extensions/opt/ext_opt.hpp:3` — "Three IR→IR optimization passes over ThinFunction, registered by name via register_passes."
- **Source reality:** the same file declares four pass structs (`ConstPropPass`, `DeadCodeElimPass`, `CSEPass`, `LICMPass`) and `register_passes` registers four names. `extensions/opt/ext_opt.cpp` ends with `reg.add<ConstPropPass>("constprop"); reg.add<DeadCodeElimPass>("dce"); reg.add<CSEPass>("cse"); reg.add<LICMPass>("licm");`.
- **Verdict:** STALE. The "Three" count was not bumped when `LICMPass` was added. (Also the file-header bullet list documents only ConstProp/DCE/CSE, omitting LICM.)

### A5. GAP_ANALYSIS marks for-each and match as "✗ v1" — both shipped
- `docs/planning/GAP_ANALYSIS.md:46` — "| `for-each` | ✗ v1 | needs iterable protocol; add with `iterable` TypeBuilder hook in v2, `../ROADMAP.md` |"
- `docs/planning/GAP_ANALYSIS.md:48` — "| `match` (pattern) | ✗ v1 | `switch` covers v1; pattern match is v2+, `../ROADMAP.md` |"
- **Source reality:** for-each shipped 2026-07-11 (`src/ast.hpp:219` `ForEachStmt`, `src/parser.cpp:774` builds it, `tests/lang/valid_for_each.ember` PASS, ROADMAP Tier 1 ✓ shipped). match shipped 2026-07-11 (`src/ast.hpp:231-235` `MatchStmt`/`MatchArm`, `src/parser.cpp:831`, `tests/lang/valid_match.ember` PASS, ROADMAP Tier 1 ✓ shipped).
- **Verdict:** STALE. The "✗ v1" entries should be "✓ v1" (or removed). The `iterable` hook framing is also outdated — the shipped for-each is slice-specific and does NOT use an `iterable` protocol (ROADMAP says so), so "needs iterable protocol" mis-describes what shipped.

### A6. SPEC_AUDIT_2026-07-10 F2 says no serializable IR exists; the v5 IR .em shipped
- `docs/spec/SPEC_AUDIT_2026-07-10.md:93` — "Does ember have a serializable IR today? **No.** … there is no `IrFunction`/`IrInstr`/`IrValue`/`BasicBlock`/`run_linear_scan`/`emit_x64` in the codegen source (verified by grep)."
- `docs/spec/SPEC_AUDIT_2026-07-10.md:78` (F2 STATUS) — "F2 secondary (IL-`.em` re-lowering) remains deferred until the SSA-lite IR ships AND a concrete threat justifies the larger loader surface."
- `docs/spec/SPEC_AUDIT_2026-07-10.md:227` (cross-check) — "the SSA-lite IR + linear-scan regalloc is marked deferred … `src/codegen.hpp` line 1-2 confirms codegen is a tree-walking stack-spilling emitter with no `IrFunction`/`run_linear_scan`/`emit_x64`. Accurate."
- **Source reality:** the thin three-address IR shipped 2026-07-10 (Stage A): `src/thin_ir.{hpp,cpp}` (`ThinFunction`/`ThinBlock`/`ThinInstr`/`ThinOp`), `src/thin_lower.{hpp,cpp}` (`lower_function`), `src/thin_emit.{hpp,cpp}` (`emit_x64` — the exact symbol the audit said does not exist). The v5 IR `.em` serialization shipped (Stage B): `src/thin_ir_ser.{hpp,cpp}` (`serialize_thin_function`), `EmFunctionRecord::ir_blob` (`src/em_loader.cpp:110-115,287-332`), `write_em_file_v5`/`load_em_file` v5 path, gated by `examples/em_v5_ir_test.cpp` (ctest `em_v5_ir`, PASS).
- **Verdict:** STALE. The "no serializable IR today" and "IL-`.em` re-lowering remains deferred" claims are superseded by the Stage A + Stage B shipment. The cross-check's "Accurate" verdict at :227 is no longer accurate. (Context: this audit is dated 2026-07-10 at revision `8062195`; the thin-IR and v5 work landed the same day / after, so the audit's point-in-time claims were not refreshed.)

### A7. CODEGEN_SPEC §5 "the shipped backend walks the AST directly" omits the thin-IR backend
- `docs/spec/CODEGEN_SPEC.md:232-236` — "## 5. Deferred register allocation design: linear scan on SSA-lite IR > **Not implemented in v1.0.** The shipped backend walks the AST directly and stack-spills expression intermediates. Everything in this section is a future, benchmark-gated design, not a description of current codegen."
- **Source reality:** there are now TWO shipped backends — the tree-walker (`compile_func` in `src/codegen.cpp`) AND the thin three-address IR backend (`lower_function` → `emit_x64`, behind `CodeGenCtx::enable_ir_backend`). The full SSA-lite + linear-scan (what §5 actually specifies) is still deferred (ROADMAP Stage 3 future), so "Not implemented in v1.0" for the *full* regalloc is accurate. But "the shipped backend walks the AST directly" is now incomplete — it omits the second shipped backend.
- **Verdict:** STALE (partial). The sibling `docs/spec/COMPILER_PIPELINE.md:376-393` §5 WAS updated with a "Stage A, 2026-07-10" implementation note documenting the thin IR; CODEGEN_SPEC §5 was not. The cross-ref at CODEGEN_SPEC §5 ("see COMPILER_PIPELINE.md Section 5 for the deferred IR definition") now points to a section that carries a Stage-A shipped note, making CODEGEN_SPEC §5's silence inconsistent.

### A8. BUNDLING_AND_EM_MODULES.md has no Version 5 section
- `docs/BUNDLING_AND_EM_MODULES.md` — Part 2 "`.em` pre-compile (Option B: serialized native code)" covers Versions 1-4 only (`:470` "Version 4 — signed raw-x86 `.em`"). Part 3 "Implemented status" (`:549-556`) lists only ".em v2 serializer + loader", "Textual imports", "Live modules" — no v3/v4/v5. No occurrence of "v5", "Version 5", "is_ir", "ir_blob", "Stage B", or "IR .em" anywhere in the file (grep-confirmed).
- **Source reality:** the v5 IR `.em` format ships — `EM_VERSION_V5 = 5` (`src/em_file.hpp:168`), per-function `is_ir` byte + `ir_blob` (`src/em_loader.cpp:287-332`, `src/em_writer.cpp`), `write_em_file_v5`/`load_em_file` v5 path. The v5 security model (re-emit from deserialized IR at load, reject malformed before any exec page) is documented only in source comments + the `em_v5_ir_test` comments, not in the bundling doc.
- **Verdict:** STALE/MISSING. The bundling doc — the canonical `.em` format reference — is two format versions behind and omits the IR-`.em` path entirely. MODULES.md is the same: no v5/ir_blob/is_ir mentions (grep-confirmed).

### A9. extensions/README.md extension table omits map/opt/obf and under-lists array/string/math APIs
- `extensions/README.md` (the "What lives here" table) lists 8 extensions: `vec`, `quat`, `mat`, `string`, `array`, `math`, `sync`, `lifecycle`. No `map`, `opt`, or `obf` row.
  - `string` row lists "`from_slice`/`from_i64`/`from_f32`/`from_f64`/`from_bool`/`identity`/`length`/`char_at`" — omits `find` and `substr` (shipped 2026-07-11, ROADMAP Tier 0 "✓ find/substr shipped 2026-07-11"; `extensions/string/ext_string.cpp:105,113`).
  - `array` row lists "`new`/`length`/`resize`/`set_u8`/`get_u8`/`set_f32`/`get_f32`/`set_i64`/`get_i64`/`push_u8` + the `GetArrayBytes` accessor" — omits `push_f32`/`push_i64`/`pop_u8`/`pop_f32`/`pop_i64`/`clear`/`remove` (shipped 2026-07-11, ROADMAP Tier 0 "✓ full v1 API shipped"; `extensions/array/ext_array.cpp` registers all of these).
  - `math` row lists "`sqrt`/`sin`/`cos`/`tan` (f32)" — omits the f64 variants and `abs_i64`/`floor_f64`/`ceil_f64`/`abs_f64`/`pow_f64` (shipped 2026-07-11, ROADMAP Tier 0).
- **Source reality:** `extensions/map/` ships (`ext_map.cpp`, CMake `ember_add_extension(map …)` at `CMakeLists.txt:153`). `extensions/opt/` and `extensions/obf/` ship (`CMakeLists.txt:156-157`). The full array/string/math native sets are in source (see above).
- **Verdict:** STALE. (Counterpart: `extensions/AUDIT.md:15` DOES name `map<K,V>` in the Tier 0 set, so the AUDIT.md is more current than the README table — but the README table is the user-facing index.)
- **Note:** `extensions/README.md` also frames extensions as "NOT a language grammar or type-system change" — accurate for vec/quat/mat/string/array/math/sync/lifecycle/map, but the `opt`/`obf` pass extensions are a different kind (IR→IR transforms, not `NativeSig` addons), so they arguably do not fit the doc's "what an extension is" definition. The doc does not address this category.

### A10. README extensions list says "eight extensions" — there are nine addon extensions + two pass extensions
- `README.md:162` — "The eight extensions (`vec`/`quat`/`mat`/`string`/`array`/`math`/`sync`/`lifecycle`) register their `NativeSig` + `OpOverloadTable` entries the same way."
- **Source reality:** nine `NativeSig` extensions (add `map`) plus `opt` + `obf` pass extensions (which register via `register_passes`, not `register_natives` — so the "register their NativeSig" framing does not cover them).
- **Verdict:** STALE. The count "eight" and the list omit `map`; the pass extensions are a separate category the sentence doesn't cover.

### A11. README syntax Control list omits for-each and match
- `README.md:138-139` — "Control: `if`/`else`, `while`, `for (init; cond; step)`, `do { } while(cond);`, `switch`/`case`/`default`/`break`, `continue`, `return`"
- **Source reality:** for-each (`for (x in slice)`) and match (`match (expr) { pat => body, _ => default }`) ship (see A5) and are exercised by `tests/lang/valid_for_each.ember` and `tests/lang/valid_match.ember`.
- **Verdict:** STALE/MISSING. Two shipped control-flow forms are absent from the README's language-syntax summary.

---

## B. INACCURATE findings (doc is wrong / self-contradictory)

### B1. README contradicts itself on whether `--load-em` is a CLI action
- `README.md:86` (the CLI reference block) lists `ember run --load-em <file.em> [--fn NAME]` as a supported action.
- `README.md:100-101` — "There is no CLI `--load-em` *action* in v1 — loading is via the host `load_em_file`/`link_em_file` API and the `link "mod.em" as m;` source directive."
- **Source reality:** `examples/ember_cli.cpp:272-314` implements `--load-em` (parses the flag at :272, calls `load_em_file` at :314; usage string at :159). Verified live: `ember_cli.exe emit-em readme_ex.ember readme_ex.em` then `ember_cli.exe run --load-em readme_ex.em --fn main` exits 20, matching the JIT result of the same script.
- **Verdict:** INACCURATE. Line 100's "There is no CLI `--load-em` action in v1" directly contradicts line 86 and the implementation. One of the two must be fixed (the action exists, so line 100 is the wrong one).

### B2. README "Docs" spec list omits two spec docs that exist and are referenced
- `README.md` "Docs → Spec" list enumerates: `TYPE_SYSTEM`, `COMPILER_PIPELINE`, `CODEGEN_SPEC`, `SAFETY_AND_SANDBOX`, `BINDING_API`, `MEMORY_AND_GC`, `BENCHMARK_SYSTEM_DESIGN`.
- **Source reality:** `docs/spec/` also contains `PASS_SYSTEM_DESIGN.md` and `CODEGEN_OPTIMIZATION_DESIGN.md`, both referenced by the ROADMAP and by source comments (`extensions/opt/ext_opt.hpp:9` "See ext_opt.hpp for the design … docs/spec/PASS_SYSTEM_DESIGN.md §8"; `docs/ROADMAP.md` cites `CODEGEN_OPTIMIZATION_DESIGN.md` repeatedly). `docs/spec/SPEC_AUDIT_2026-07-10.md` is also unlisted (that one is an audit record, more defensible to omit).
- **Verdict:** INACCURATE (incomplete index). A reader following the README's Docs section cannot discover the pass-system or codegen-optimization design docs that the code references.

### B3. ROADMAP match entry omits the IR-backend fallback that for-each's entry states
- `docs/ROADMAP.md:100-101` (for-each entry) — "The IR backend marks functions using for-each as non_serializable (falls back to the tree-walker)." Correct.
- `docs/ROADMAP.md:103-108` (match entry) — says match shipped, patterns, arms, no fallthrough; **no mention of the IR-backend fallback.**
- **Source reality:** `src/thin_lower.cpp:949-971` — a function containing `ForEachStmt` OR `MatchStmt` is marked `non_serializable` with reason "for-each or match is not yet lowered to ThinFunction IR; falling back to tree-walker at Stage A" (`:942-944` + `:971`). So match has the SAME IR-fallback behavior as for-each, but the ROADMAP documents it only for for-each.
- **Verdict:** INACCURATE (asymmetric/omitted). The match entry should carry the same non_serializable note the for-each entry carries.

---

## C. MISSING documentation (feature ships, doc absent)

### C1. for-each and match are absent from the canonical grammar + AST docs
- `docs/spec/COMPILER_PIPELINE.md:61-138` §2 "Grammar (informal BNF)" — the `stmt` production lists `while`, `do…while`, `for(init;cond;step)`, `switch`, `break`, `continue`, `return`, etc. There is **no** `for (IDENT in expr) block` production and **no** `match (expr) { arms }` production.
- `docs/spec/COMPILER_PIPELINE.md:167-191` §3 "AST node types" — lists `ForStmt`, `WhileStmt`, `IfStmt`, `ReturnStmt`, `BreakStmt`, `ContinueStmt`, `ExprStmt`, `BlockStmt`. There is **no** `ForEachStmt` and **no** `MatchStmt`/`MatchArm`.
- `docs/spec/COMPILER_PIPELINE.md:214-256` §2a "New v1.0 surface syntax" — documents enum + function refs only; no for-each, no match.
- **Source reality:** `src/ast.hpp:219` `ForEachStmt { string var; ExprPtr iter; Block body; }`; `src/ast.hpp:231-235` `MatchArm { ExprPtr pattern; bool is_wildcard; Block body; }` + `MatchStmt { ExprPtr subject; vector<MatchArm> arms; }`. `src/parser.cpp:774` parses for-each, `:831` parses match.
- **Verdict:** MISSING. Two shipped statement forms have no grammar production and no AST-node entry in the canonical pipeline doc. (The ROADMAP Tier 1 entries describe them informally, but the spec grammar/AST is the referenced contract.)

### C2. for-each and match codegen/lowering are absent from CODEGEN_SPEC
- `docs/spec/CODEGEN_SPEC.md` — grep for "for-each"/"match"/"ForEach"/"MatchStmt" finds only English occurrences ("for each reg", "matches", "mismatch"); no codegen-section documentation of how for-each lowers (the slice {ptr,len} → while-loop-with-indexing described in ROADMAP:100) or how match lowers (per-arm branches).
- **Verdict:** MISSING. The codegen doc has switch lowering (`§12`) but no match lowering and no for-each lowering.

### C3. match is absent from TYPE_SYSTEM
- `docs/spec/TYPE_SYSTEM.md` — grep for "match (" yields only "mismatch" (false positives at `:442,:460`); no documentation of match as a language feature, its subject type rule (integer or bool — `tests/lang/sema_invalid_match_not_int.ember` pins f32 rejection), or its pattern types.
- **Verdict:** MISSING. The match subject-type rule ("must be integer or bool") is enforced in `src/sema.cpp` and pinned by `sema_invalid_match_not_int.ember` but is not in the type-system spec.

### C4. The map<K,V> extension has no spec documentation
- `docs/spec/TYPE_SYSTEM.md`, `docs/spec/BINDING_API.md` — no mention of `map<K,V>`, `map_new`, or the map extension. The map extension appears only in `docs/ROADMAP.md` (Tier 0), `docs/MAINTENANCE_LOG.md`, `docs/audit/AUDIT_2026-07-09.md`, `docs/planning/GAP_ANALYSIS.md`, and `extensions/AUDIT.md:15`.
- **Source reality:** `extensions/map/ext_map.cpp` ships a 7-native API (`map_new`/`map_set`/`map_get`/`map_contains`/`map_length`/`map_remove`/`map_clear`) on `unordered_map<int64_t,int64_t>`, registered in the CLI (`examples/ember_cli.cpp:135`).
- **Verdict:** MISSING. The map extension's API, its i64/i64 key/value convention, and its bounds behavior (invalid handle → 0, missing key → 0) are undocumented in the spec layer.

### C5. The v5 IR `.em` format is undocumented in BUNDLING + MODULES (also A8)
- See A8. The `is_ir` per-function byte, the `ir_blob` record, the re-emit-at-load security model (deserialize → validate → re-emit via `emit_x64` BEFORE `alloc_executable_rw`), the malformed-rejection guarantees, and the v5 version itself are all absent from `docs/BUNDLING_AND_EM_MODULES.md` and `docs/MODULES.md`. The only documentation is source comments (`src/em_loader.cpp:110-115,287-332,651+`) and the `em_v5_ir_test` comments.

---

## D. MISSING tests (feature/behavior ships, no test)

### D1. The map extension has ZERO test coverage — no ctest, no runtime exercise, no registration smoke test
- `CMakeLists.txt` — `ember_add_extension(map extensions/map/ext_map.cpp)` at `:153`, but there is **no** `add_test` for any map target (ctest list confirmed: 32 tests, none map-named).
- `examples/ext_registration_test.cpp` — registers 8 extensions (`vec/quat/mat/string/array/math/sync/lifecycle` at `:67-74`) but does **NOT** `#include "ext_map.hpp"` or call `ext_map::register_natives(m)`. No `map_*` native is invoked anywhere in `examples/` or `tests/lang/` (grep-confirmed: `map_new|map_set|map_get|map_contains` → 0 hits in `examples/*.cpp` and `tests/lang/*.ember`).
- **Source reality:** `extensions/map/ext_map.cpp` implements 7 natives with real bounds behavior: invalid handle (`h<1 || h>size`) → nullptr → `map_get` returns 0 / `map_set` no-op / `map_contains` 0 / `map_length` 0; missing key → `map_get` returns 0; `MAX_MAPS=100000` cap; `n_map_new` returns 0 on cap/exception. None of this is verified by any test.
- **Verdict:** MISSING (highest-impact test gap). The map extension shipped 2026-07-11 (ROADMAP Tier 0 ✓) with no test of any kind — not registration, not happy-path, not bounds. Compare: `array`/`string`/`sync` all have registration-smoke + boundary tests in `ext_registration_test.cpp`; map has none.

### D2. No edge-case tests for for-each (nested for-each)
- `tests/lang/valid_for_each.ember` — single-level `for (x in slice) sum = sum + x;`. That is the only for-each positive test. `sema_invalid_for_each_not_slice.ember` is the only negative.
- Grep for a second `for (… in)` inside any for-each body across `tests/lang/`, `examples/`, `demo/`: none. No nested for-each test exists.
- **Verdict:** MISSING. Nested for-each (a `for (x in outer) { for (y in inner) … }` shape) is not tested anywhere.

### D3. No edge-case test for match with no wildcard arm
- `tests/lang/valid_match.ember` — has `_ => { return 99; }` (wildcard present).
- `tests/lang/sema_invalid_match_not_int.ember` — has `_ => { return 0; }` (wildcard present).
- The only other `match (` occurrences in lang tests are false positives (`sema_invalid_multiple_errors.ember` and `sema_invalid_undefined_name.ember` do not use match — grep matched "mismatch"/unrelated).
- **Source reality:** match with no `_` wildcard is a legal form (ROADMAP:103-108 says patterns are "integer/bool literals + `_` wildcard" — the wildcard is optional, not required). A match where no arm matches and there is no wildcard falls off the end (behavior: falls through to the next statement / returns default). This behavior is untested.
- **Verdict:** MISSING. No test covers match without a `_` arm (the no-match fallthrough path).

### D4. No bounds-checking tests for array get/set (negative index, OOB index)
- `examples/ext_registration_test.cpp:128-136` tests `array_new` rejection (negative count, zero elem size, overflow) and invalid `array_resize` (negative, over-cap) — but does NOT test `array_get_u8`/`array_set_u8`/`array_get_i64`/etc. with a negative index or an out-of-bounds index.
- **Source reality:** `extensions/array/ext_array.cpp` guards every get/set with `i>=0 && i<int64_t(s->bytes.size())` (u8) / `i>=0 && size_t(i)*N+N<=s->bytes.size()` (f32/i64) — a negative or OOB index is a no-op (set) or returns 0 (get). This bounds contract is unverified.
- **Verdict:** MISSING. The array negative-index and OOB-index bounds behavior is not tested.

### D5. No bounds-checking tests for string char_at / substr (OOB index, OOB substr)
- `examples/ext_registration_test.cpp:140-142` tests `string_from_slice` rejection (negative length, over-cap) — but does NOT test `string_char_at` with negative/OOB index, nor `string_substr` with out-of-bounds `start`/`len`.
- **Source reality:** `extensions/string/ext_string.cpp:50-52` `n_string_char_at` — `if (!s || i < 0 || size_t(i) >= s->size()) return 0;`. `:113-119` `n_string_substr` — `if (!x || start < 0) return str_new(""); if (s >= x->size()) return str_new(""); actual_len = (len<0) ? (size-s) : min(len, size-s)` (clamps). These bounds contracts are unverified.
- **Verdict:** MISSING. The string char_at OOB and substr out-of-bounds (negative start, start≥length, negative len, len past end) behaviors are not tested.

### D6. No test for IR optimization passes on functions with no optimizable patterns / empty functions
- `examples/ir_passes_test.cpp` — runs each pass on one of four workloads, each deliberately constructed to contain the pass's target pattern (`constprop_fold` has `b+4`; `dce_dead_store` has a dead `i*13`; `cse_redundant` has `a+a`; `licm_invariant` has `100*200` in a loop). All four workloads have non-trivial bodies.
- Not tested: (a) a pass on a function with NO pattern it can transform (e.g. `fn main()->i64 { return 7; }` for constprop/dce/cse/licm — should return `Preserved::all()` and change nothing); (b) a pass on an empty-body function (e.g. `fn f() -> void {}` or a function that lowers to zero/no instrs); (c) a pass on a function with only side-effecting instrs (calls/stores) that DCE/CSE must not remove.
- **Source reality:** the passes have explicit "nothing changed → `Preserved::all()`" paths (`ConstPropPass` final returns, `DeadCodeElimPass` `return changed ? none : all`, `CSEPass` same, `LICMPass` `if (loops.empty()) return all` + `if (to_hoist.empty()) continue`). These no-op paths are not exercised by any test.
- **Verdict:** MISSING. The passes are tested only on their happy-path (pattern-present) workloads; the no-op and empty-function paths are unverified.

### D7. LICM is tested only for value-preservation, not for actually hoisting anything
- `examples/ir_passes_test.cpp` — the LICM entry in `passes[]` is `{"licm", "licm_invariant", false}` with `check_instr_reduction=false`, and the comment says "LICM which moves, not removes." So the test asserts `rb == rp` (value-preservation) but does NOT assert that any instruction actually moved from a loop body to a pre-header, nor that the hoist produced any measurable effect (e.g. the invariant `100*200` is computed once, not per iteration).
- **Source reality:** `LICMPass::run` (`extensions/opt/ext_opt.cpp`) does real hoisting (finds back-edges, builds loop bodies, finds pre-header, moves invariant instrs to pre-header end). The hoisting logic (back-edge detection, predecessor mapping, `is_invariant_instr`, the `to_hoist` collection + reverse-order erase) is the load-bearing part and is not directly verified.
- **Verdict:** MISSING (edge-case / behavioral). LICM is gated only by "didn't break the result," not by "did the optimization." A regression where LICM silently hoists nothing (or hoists wrongly but happens to preserve the result on this workload) would pass.

### D8. No test for the .em v5 mixed-mode (some IR, some raw-x86 functions in one module)
- `examples/em_v5_ir_test.cpp` — builds a single-function all-IR v5 module (`fib`, `ir_blob` populated for the one function, `:67-81`). `examples/em_roundtrip_test.cpp` tests v1/v3/v4 (all raw-x86). No test builds a v5 module with multiple functions where SOME have `is_ir=1` (IR blob) and SOME have `is_ir=0` (raw-x86 body).
- **Source reality:** the loader explicitly supports mixed mode — `src/em_loader.cpp:110-115` ("when non-empty, this function is an IR function … otherwise the function is a raw-x86 fallback") and `:287-332` (per-function `is_ir` byte: 1 → read ir_blob, 0 → fall through to raw-x86 body). This is a first-class supported combination with no test.
- **Verdict:** MISSING. Mixed-mode v5 modules (IR + raw-x86 in one bundle) are supported by the loader but untested.

### D9. No test that for-each/match functions correctly fall back to the tree-walker under enable_ir_backend
- `examples/thin_ir_test.cpp` — the IR-path corpus (`:516-545`) runs only `runtime_audit_semantics.ember` and `runtime_division_forms.ember` with `enabled=true` (neither uses for-each or match). The rest are `enabled=false` SKIP (known gaps: slices/structs/strings/defer). The known-gaps SKIP list (`:507-510`) does NOT mention for-each or match.
- **Source reality:** `src/thin_lower.cpp:949-971` — a function using `ForEachStmt` or `MatchStmt` is marked `non_serializable` and `lower_function` falls back (the IR path returns a flag that makes the caller use the tree-walker). This fallback is the only way for-each/match functions execute when `enable_ir_backend=true`.
- **Verdict:** MISSING. No test compiles a for-each or match function with `enable_ir_backend=true` and asserts the fallback produces the correct result (and does not crash or silently miscompile). If the fallback path regressed (e.g. emitted wrong code instead of falling back), no test would catch it.

### D10. em_loader_hardening_test does not cover v5 IR malformations
- `examples/em_loader_hardening_test.cpp` — tests v1-v4 raw-x86 malformations only (huge functions/globals/rodata/code, bad relocations, duplicate slots, bad entry, name-directory errors). No `EM_VERSION_V5`, `ir_blob`, or `is_ir` cases (grep-confirmed).
- **Source reality:** v5 malformations (bad ir_blob magic, truncated ir_blob, empty ir_blob, invalid is_ir byte, is_ir=1 with ir_blob_len=0, unknown native name, natives==nullptr + CallNative) ARE tested — but in `examples/em_v5_ir_test.cpp` Part 2, not in the hardening test.
- **Verdict:** PARTIAL (not a clean MISSING — `em_v5_ir_test` covers v5 malformed rejection). The hardening test is v1-v4 only; v5 hardening lives in a different test. Acceptable, but the split means the hardening test's "the loader rejects all malformed input" framing is implicitly v1-v4-scoped. Noting for completeness.

---

## E. VERIFIED CURRENT (checked against source, accurate)

These were specifically checked because the task or prior audits flagged them, and they hold:

- **README code example** (`README.md:21-56`) — runs clean. `ember_cli.exe run /tmp/readme_ex.ember --fn main` exits 20 (compiles + executes; struct literal, enum `Stance::Aggressive`, `sqrt` native, while loop, cross-fn call all work). VERIFIED CURRENT.
- **ROADMAP "31 CTest targets by default, 32 with SDK"** (`docs/ROADMAP.md:43-45`) — accurate. `CMakeLists.txt` has 32 `add_test` directives; `bench_ember_vs_as` is conditional on `if(EXISTS …/as_atomic.cpp)` (`CMakeLists.txt:401`) and the AngelScript SDK is present (`thirdparty/sdk/angelscript/include/angelscript.h`), yielding the 32 ctest actually ran. Default (no SDK) would be 31. VERIFIED CURRENT.
- **ROADMAP Tier 0 array/string/math/map "✓ shipped 2026-07-11" claims** — `array` full API (push_f32/i64, pop_*, clear, remove) in `extensions/array/ext_array.cpp`; `string` find/substr in `extensions/string/ext_string.cpp:105,113`; `math` f64 variants + abs_i64 in `extensions/math/ext_math.cpp`; `map` i64/i64 in `extensions/map/ext_map.cpp` (`unordered_map<int64_t,int64_t>`). All match the ROADMAP claims. VERIFIED CURRENT.
- **ROADMAP for-each "IR backend marks functions as non_serializable"** (`docs/ROADMAP.md:100-101`) — `src/thin_lower.cpp:949-962` confirms `ForEachStmt` triggers `non_serializable`. VERIFIED CURRENT.
- **ROADMAP Tier 2 function-refs + call-target-provenance guard** — `examples/function_refs_test.cpp` (ctest `function_refs`, PASS) pins handle creation, multi-arg dispatch, recursion via `&fib`, and the guard (out-of-range + in-range-unregistered both trap). VERIFIED CURRENT.
- **ROADMAP Tier 5 sync queues + context thread-safety** — `examples/ext_sync_test.cpp` (ctest `ext_sync`, PASS, 16 tests incl. multi-thread stress) and `examples/thread_safety_test.cpp` (ctest `thread_safety`, PASS) pin both. VERIFIED CURRENT.
- **SPEC_AUDIT F3 (MEMORY_AND_GC §4 "sema rejects aggregate globals" was wrong)** — remediated: `docs/spec/MEMORY_AND_GC.md:104` now reads "v1 global storage supports scalar/handle/struct/fixed-array/slice globals." VERIFIED CURRENT (fixed).
- **SPEC_AUDIT F5 (TYPE_SYSTEM §9 `auto` no deprecation note)** — remediated: `docs/spec/TYPE_SYSTEM.md:260-268` carries the deprecation note (2026-07-10, commit `d852160`). VERIFIED CURRENT (fixed).
- **SPEC_AUDIT F6 (ROADMAP "20/19 CTest" stale)** — remediated: ROADMAP now says 31/32 (see above). VERIFIED CURRENT (fixed). (The SPEC_AUDIT's own "current tree is 22" at `:27,:187` and "ctest 22/22" at `:5` are historical snapshots at revision `8062195` — acceptable as an audit record, not re-flagged.)
- **COMPILER_PIPELINE §5 IR deferred note** (`docs/spec/COMPILER_PIPELINE.md:367-393`) — updated with a "Stage A, 2026-07-10" implementation note documenting the thin IR. VERIFIED CURRENT (unlike CODEGEN_SPEC §5 — see A7).
- **ROADMAP historical ctest counts in DONE/SHIPPED entries** (`:340` "ctest 22/22" for string-encryption DONE 2026-07-10; `:412` "26/26" for Stage 1 peephole SHIPPED 2026-07-10; `:432` "27/27" for Stage A SHIPPED 2026-07-10) — these are point-in-time verification records inside historical entries ("Verified: ctest X/X … at the time this shipped"), not claims about the current count. VERIFIED CURRENT as historical records.
- **Cross-process loading is documented as unsupported (and not tested, correctly)** — `docs/MODULES.md:367` "No cross-process modules. The registry is per-process."; `README.md:101-102,157-158` ".em is ABI/process-trusted, not a portable interchange format … cross-process portability is a versioned-relocation v2+ item." No test claims cross-process works. VERIFIED CURRENT.
- **`ext_registration_test.cpp` array/string/sync allocation-boundary rejection tests** (`:128-145`) — negative count/size, zero elem size, overflow, invalid resize, negative/over-cap string length, invalid swapbuf/spsc/mpsc/mpmc capacities all tested and match source guards. VERIFIED CURRENT.
- **`em_v5_ir_test.cpp` v5 roundtrip + malformed rejection** — fib(10)==55 via re-emitted IR, JIT ground-truth match, and 5 malformed cases (bad magic, truncated, empty, unknown native, nullptr natives) all rejected with no exec page. VERIFIED CURRENT (this is the one .em v5 path test that does exist; the gap is mixed-mode D8).
- **`ember_pass_test.cpp` pass-system infrastructure** — registry/manager/PreservedAnalyses/instrumentation/is_required/pipeline-parser/run_to_fixpoint all pinned (25 checks, ctest `ember_pass`, PASS). VERIFIED CURRENT.
- **MAINTENANCE_CONSTRAINTS.md** — the rulebook itself is internally consistent and current (it even explains why spec docs lag: "No changes to `docs/spec/` design docs"). VERIFIED CURRENT.
- **MAINTENANCE_LOG.md** — entries are timestamped and consistent with the build state (32/32 PASS recorded). VERIFIED CURRENT.

---

## F. Summary by category

| Category | Count | IDs |
|---|---|---|
| STALE | 11 | A1–A11 |
| INACCURATE | 3 | B1–B3 |
| MISSING (docs) | 5 | C1–C5 |
| MISSING (tests) | 10 | D1–D10 (D10 is PARTIAL) |
| VERIFIED CURRENT | 16 | listed in §E |

**Highest-impact findings:**
1. **D1** — map extension has zero test coverage (shipped, untested entirely).
2. **A1/A2/A3/A4** — the pass-system spec layer (PASS_SYSTEM_DESIGN header + §8, CODEGEN_OPTIMIZATION_DESIGN §8, ext_opt.hpp header) still marks LICM + SubstitutionPass as future / says "three" passes; both shipped and tested.
3. **A5** — GAP_ANALYSIS marks for-each and match "✗ v1"; both shipped.
4. **C1/C2/C3** — for-each and match are absent from the canonical grammar, AST, codegen, and type-system specs.
5. **A8/C5** — the v5 IR `.em` format is undocumented in BUNDLING + MODULES.
6. **B1** — README self-contradicts on whether `--load-em` is a CLI action (it is).
7. **D8** — .em v5 mixed-mode (IR + raw-x86 in one module) is supported by the loader but untested.
8. **D6/D7** — IR optimization passes are tested only on happy-path workloads; no-op/empty-function paths and LICM's actual hoist behavior are unverified.

**Root-cause note:** the cluster of STALE/MISSING findings in `docs/spec/` and `docs/planning/` (A1–A8, C1–C5) is consistent with `docs/MAINTENANCE_CONSTRAINTS.md` forbidding the maintenance cron from editing spec docs. The spec layer therefore does not auto-track shipped features; it requires a human doc pass per feature shipment. The features that DID get spec updates (enum, function refs in COMPILER_PIPELINE §2a; aggregate globals in TYPE_SYSTEM §12; thin-IR Stage A in COMPILER_PIPELINE §5) show the intended workflow — for-each, match, map, LICM, SubstitutionPass, and v5 IR simply did not get that pass.
