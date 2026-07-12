# ember maintenance log — hourly cron audit trail

> **This file is appended to by the hourly maintenance cron.**
> Each run adds a timestamped summary of what it checked, what it fixed, and
> what it deferred. This is the cron's memory between runs.

## Format

Each entry:

```
## YYYY-MM-DD HH:MM (timezone)
- **Tree state:** clean / dirty (read-only audit)
- **Build:** PASS (N/N tests) / FAIL (details)
- **Findings:**
  - [FIXED] <what was fixed and where>
  - [DEFERRED] <what was found but too complex for automated fix>
  - [NONE] <no issues found>
- **Commit:** <hash or "none">
```

---

## 2026-07-11 00:45 (initial — created by the human, not the cron)
- **Tree state:** clean
- **Build:** PASS (32/32 tests)
- **Findings:**
  - [NONE] Initial maintenance log created. The cron will append entries here on each hourly run.
- **Commit:** none (this is the seed entry)

## 2026-07-11 (manual review — 2-minute cron triggered, >2h since last entry)
- **Tree state:** clean
- **Build:** PASS (32/32 tests, 0 failures)
- **Findings:**
  - [NONE] No new compiler warnings. No stale docs found. Build is green.
  - 21 commits since last maintenance log entry (Stage C LICM+SubstitutionPass,
    audit revalidations, array/string/math/map extensions, for-each, match,
    nested for-each fix, CLI --passes, bench paired CI, ROADMAP updates).
  - The hourly audit cron (s2) has not yet appended an entry — it may not have
    fired during this session (orchestrate mode, fires while pi is running).
- **Commit:** none (no changes needed — tree is clean, build is green, docs are current)

## 2026-07-11 06:49 (EDT)
- **Tree state:** clean (c1 baseline) → dirty after c2/c3 fixes (src/ + docs/), now committed
- **Build:** PASS (32/32 tests, 0 warnings)
- **Findings:**
  - [FIXED] src/codegen.cpp:580 — moved `return;` onto its own line (prescan_stmt IfStmt has_else, -Wmisleading-indentation)
  - [FIXED] src/codegen.cpp:651 — moved `return;` onto its own line (count_struct_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/codegen.cpp:723 — moved `return;` onto its own line (count_arr_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/codegen.cpp:790 — moved `return;` onto its own line (count_str_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/codegen.cpp:847 — moved `return;` onto its own line (count_pin_refs_stmt, -Wmisleading-indentation)
  - [FIXED] src/codegen.cpp:1835 — removed unused `Label z` declaration (-Wunused-but-set-variable)
  - [FIXED] src/codegen.cpp:3547 — removed unused `int32_t locals_bytes` line + orphaned trailing comment (-Wunused-variable)
  - [FIXED] src/thin_lower.cpp:279 — moved `return;` onto its own line (prescan_stmt, -Wmisleading-indentation)
  - [FIXED] src/thin_lower.cpp:327 — moved `return;` onto its own line (count_struct_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/thin_lower.cpp:374 — moved `return;` onto its own line (count_arr_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/thin_lower.cpp:423 — moved `return;` onto its own line (count_str_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/thin_lower.cpp:476 — moved `return;` onto its own line (count_logical_temps_stmt, -Wmisleading-indentation)
  - [FIXED] src/thin_lower.cpp:541 — moved `return;` onto its own line (count_pin_refs_stmt, -Wmisleading-indentation)
  - [FIXED] src/peephole.cpp:34 — removed unused `static int32_t read_le_i32(...)` function (-Wunused-function; siblings read_le_i64/write_le_i32 remain in use)
  - [FIXED] src/parser.cpp:459 — dropped unused `existing` binding name in `if (auto existing = dynamic_cast<CallExpr*>(e.get()))` (-Wunused-variable; condition unchanged)
  - [FIXED] docs/ROADMAP.md — updated stale CTest count 22/23 → 31/32
  - [FIXED] docs/planning/DESIGN.md — updated stale CTest count 20/19 → 32/31
  - [FIXED] docs/planning/v1.0_INTEGRATION_NOTES.md — updated stale CTest count 20/19 → 32/31
  - [FIXED] docs/planning/plan_CONTEXT_THREADSAFETY.md — refreshed all stale src/codegen.cpp, src/codegen.hpp, src/context.hpp, src/dispatch_table.hpp, examples/ember_cli.cpp line cites throughout §1.1, §1.2, §1.3, Options A/B/D, §3, §5, and Appendix evidence index to current source lines (resolves M-docs-1 residual from 07-10 revalidation)
  - [NONE] security: all 07-09/07-10 confirmed defects verified fixed (c4 spot-checks + revalidation probes PASS; M-docs-1 partial — the only non-fully-fixed item — was the plan_CONTEXT_THREADSAFETY.md stale line cites, now resolved by the doc fix above)
  - [DEFERRED] Duplication extraction: 11 identical `if (is->has_else) <call>(...); return;` sites across codegen.cpp (5) and thin_lower.cpp (6) follow the same pattern but extracting a shared helper would require templating over different call targets/signatures (struct/arr/str/logical/pin-refs variants) and touch 2 files with well over 50 lines of scaffolding. Per conservative constraints (>50 lines), deferred for a human refactor.
- **Commit:** see git log

## 2026-07-11 09:50 (EDT)
- **Tree state:** clean (c1 baseline at HEAD `ab49898`) → dirty after c2/c3 fixes (4 docs/ + 1 src/), committed this run
- **Build:** PASS (34/34 tests, 0 failures; 0 warnings in src/ + extensions/; full rebuild of src/engine.cpp clean)
- **Findings:**
  - [FIXED] src/engine.cpp:170 — P2 from `docs/audit/AUDIT_CODE_CORRECTNESS_2026-07-11.md`: `finalize()` pass 2 (post-allocation rodata relocation) was missing the bounds check that pass 1 has; added `&& uint64_t(af.code_offset)+8 <= image.size()` to the pass-2 loop condition (comment at :168), preventing an OOB write on the executable page if a malformed fixup with `code_offset+8 > image.size()` ever reaches the second pass. Behavior-preserving for all valid fixups (which always satisfy the check). (c3)
  - [FIXED] docs/ROADMAP.md:43 — updated stale CTest count "31 CTest targets by default … 32 total with the SDK" → "33 CTest targets by default … 34 total with the SDK", matching the actual 34 `add_test` entries (c2)
  - [FIXED] docs/audit/AUDIT_2026-07-11_SYNTHESIS.md:78 — appended a dated "Update 2026-07-11 (post-audit fix pass)" note recording that all six correctness findings C1-C6 are now fixed (with fix location + commit hash for each: C1 `381a616`/field_of_index_test, C2 `ab49898`, C3 `3b7c8df`, C4 `4d4036e`, C5 sema.cpp U64 arm, C6 `3b7c8df`/try_eval_const_f64); original finding text preserved as historical record (c2)
  - [FIXED] docs/spec/SAFETY_AND_SANDBOX.md:259 — fixed a malformed cross-reference `../RESEARCH_NOTES.md/DESIGN.md Section 8` → `../RESEARCH_NOTES.md "Prior native-JIT scripting language"` (RESEARCH_NOTES.md is a file, not a directory) (c2)
  - [FIXED] docs/planning/GAP_ANALYSIS.md:80 — corrected stale addon status: "eight shipped host-side extensions" → "nine" (added `map`); added a `map` API bullet (map_new/set/get/contains/length/remove/clear); updated the array bullet (pop/clear/remove shipped 2026-07-11), string bullet (find/substr shipped), math bullet (f64 octet + abs_i64 shipped); replaced the false "There is no map extension" sentence with "A `map` extension now ships (2026-07-11)"; corrected the Section-1 table row "map … deferred" → "map addons; richer … still deferred" (c2)
  - [FIXED] (concurrent live session, NOT this pass — recorded for accuracy) C2 for-each non-power-of-2 element-size defect: fixed in commit `ab49898` at src/codegen.cpp:3418-3427 (imul-based element addressing for esz ∉ {1,2,4,8}); verified holding. The task expected this as DEFERRED, but a concurrent work session fixed it before this run.
  - [NONE] compiler warnings: full rebuild of src/ + extensions/ produced 0 warnings in src/ or extensions/. The only build warnings are in examples/ files (out of scope). The 15 warning fixes from the prior 06:49 run hold.
  - [NONE] security: all 07-09/07-10/07-11 confirmed defects (C1-C6, S1, S2, H1-H15, M1-M13) verified fixed and holding (c3 spot-checked C1/C2/C4/H2/S1/S2/C5/C6 at cited source lines). The P2 engine.cpp fix above is the one new defect addressed this run.
  - [DEFERRED] 11-site `has_else` duplication extraction: 11 identical `if (is->has_else) <call>(...); return;` sites across codegen.cpp (5) and thin_lower.cpp (6); extracting a shared helper would require templating over different call targets/signatures and touch 2 files with well over 50 lines of scaffolding. Per conservative constraints (>50 lines), deferred for a human refactor. (carried over from the 06:49 run; c3 confirmed still applicable)
  - [DEFERRED] `b.build()` + merge boilerplate duplication across 10 extension files — would touch >3 files; deferred (c3)
  - [DEFERRED] P1: src/types.cpp:28-42 `byte_size()` returns 8 for structs (dead `if (!struct_name.empty())` branch); latent footgun, not a clear security/correctness defect — a fix would change behavior (moving the struct check before the switch). Deferred (c3)
  - [DEFERRED] P3: sema does not flag unreachable code — documented v1 scope choice, not a defect. Deferred (c3)
  - [DEFERRED] P4: cascading/duplicate sema errors on undefined names — cosmetic/diagnostic only. Deferred (c3)
  - [DEFERRED] P5: `get_entry_function` returns a thread_local static pointer — footgun, no current misuse; deferred (c3)
  - [DEFERRED] map extension has zero test coverage — true gap; no test added (beyond the scope of a conservative doc-fix pass) (c2)
  - [DEFERRED] for-each and match absent from the canonical grammar/codegen specs (COMPILER_PIPELINE §2 grammar / CODEGEN_SPEC) — no spec sections added (would be new design content, out of scope) (c2)
  - [DEFERRED] docs/spec/BUNDLING_AND_EM_MODULES.md "Part 3 - Implemented status" lists the `.em` v2 serializer as the implemented version, but the tree has shipped v5 (Stage B IR serialization); v2 is still supported/loadable so the claim is technically not false — left under the conservative constraint; a future pass may want to note v3/v5 there (c2)
- **Notes:**
  - c1 baseline: build PASS, ctest 34/34 at HEAD `ab49898` (2026-07-11 09:41 EDT). The ctest denominator is 34, not 32 (some prior entries stated 32/32): `field_of_index` (commit `381a616`) and `aggregate_global` are among the additions.
  - Concurrent live work session activity was present during this run (rapid commits C1/C2/C3/C4/C6 in git log: `381a616`, `ab49898`, `4d4036e`, `3b7c8df`, plus the C5 sema.cpp U64 arm). The for-each C2 defect was fixed by that session (`ab49898`), not by this maintenance pass.
  - `examples/field_of_index_test.cpp` was committed by the concurrent session in `381a616` (no longer untracked; it is tracked and its test `field_of_index` #25 passes). No `git add` of an untracked file was needed this run — `git add -A` stages only the 5 modified tracked files + this log entry.
  - One transient environmental issue: a file-lock on `buildt/bench_codegen_paths.exe` (linker "Permission denied") was cleared by deleting the gitignored artifact and rebuilding — not a source/build error (same class of issue c1 hit with `function_refs_test.exe`).
  - Parent submodule bump **skipped**: the parent repo (`hyper_workspace`, branch `main`) has extensive unrelated uncommitted work (modified `CLAUDE.md`, `VULN_DRIVER_SEARCH_STATUS.md`, 16 `prism_loader/*` files, plus 30+ untracked research docs / python scripts / new sources) — a major active live work session. The parent's `ember` submodule pointer records `ab49898` but ember's HEAD is now `2223b3a` (this commit advanced it). Per the prime directive (do not interrupt ongoing work) and the conservative-skip guidance, the parent commit was skipped rather than pushing a submodule-bump commit onto `origin/main` mid-session. `git add ember && git commit -m "Update ember submodule: hourly audit" && git push origin main` is the sanctioned scoped action (stages only the submodule gitlink) and can be run later by the parent session owner when the tree is clean. The parent is not broken — `ab49898` is a valid green commit.
  - Follow-up note: `git add -A` also surfaced three untracked files from concurrent activity (`docs/LLVM_ADDITIONAL_PASSES_RESEARCH.md`, a 568-line research doc; and `results_codegen_paths.csv` / `results_codegen_paths.md` at the repo root — transient bench artifacts, machine-specific, regenerated each run, only partially covered by `.gitignore` which ignores `bench/results_codegen_paths.*` but not the root copies). These were **unstaged and left untracked** — not part of this maintenance pass. A future pass may want to extend `.gitignore` with `/results_codegen_paths.*` to cover the root-location bench artifacts.
- **Commit:** see git log (this run)

## 2026-07-12 03:49 (EDT)
- **Tree state:** dirty (read-only audit) at HEAD `2341ea0ff82a7de1c30f5fb9b136db968fd4aca4`; initial pre-existing paths: modified `src/regalloc.cpp`, `src/thin_emit.cpp`, `src/thin_ir.hpp`; untracked `bench_after_loadframe.txt`; no staged changes. Per the prime directive, no source/doc fixes, staging, cleanup, commit, or push were attempted (this required log append is the only intentional edit).
- **Build:** PASS (`cmake --build buildt -j 8`: no work, no warnings; `ctest --output-on-failure -E bench`: 54/54 tests on the confirming full run)
- **Findings:**
  - [NONE] No compiler warnings were emitted by the build.
  - [DEFERRED] The first full CTest run reported transient launch failures for `em_v5_ir` and `ember_pass`, plus an interrupted/failing `regalloc` run; an immediate targeted retry passed 3/3 and the confirming full run passed 54/54. No source diagnosis or fix was attempted because the initial tree was dirty.
  - [DEFERRED] The test run created an untracked repository-root path `NUL` (absent from the initial status). It was left untouched under the no-clean/no-edit directive.
  - [DEFERRED] Previously recorded larger audit/refactor items remain outside the constrained dirty-tree audit scope.
- **Commit:** none (initial tree dirty; commit/push prohibited)

## 2026-07-12 04:05 (EDT)
- **Tree state:** dirty (read-only audit) at HEAD `58c0bda1587c16e25ec52302a083fa2885a37c48`; c1's initial inventory and the final pre-append check contain only the pre-existing modification to `docs/MAINTENANCE_LOG.md`, with no staged or untracked files. Per the prime directive, c5 made no maintenance changes and this appended entry is the sole tracked edit permitted by this audit.
- **Build:** PASS (c2 read-only baseline: `cmake --build buildt -j 8`; `ctest --output-on-failure -E bench --timeout 60`: 54/54 non-benchmark tests passed)
- **Findings:**
  - [NONE] No Ember-owned compiler warnings were emitted; no fixes were made on this dirty run.
  - [DEFERRED] Confirmed small maintenance candidates, identified without modification: v5 empty-IR raw-x86 policy bypass (`src/em_loader.cpp` and its mixed-v5 test); unchecked frame-plan offsets (`src/thin_ir_ser.cpp` and validation tests); catch-stack overflow beyond `MAX_CATCH_DEPTH` (`src/codegen.cpp` and try/catch test); missing raw/f-string literal size limits (`src/lexer.cpp` and lexer test); and bundle-footer arithmetic overflow (`examples/ember_stub_main.cpp` and bundler test).
  - [DEFERRED] Larger security/design work remains outside the constrained audit: broader v5 semantic validation, coroutine checkpoint ownership, safe-call/default-policy APIs, allowlist behavior, and comprehensive frame budgeting.
  - [DEFERRED] Documentation inaccuracies remain for a future clean-tree pass, including stale test totals, CLI exit-code/options, `.em` version/policy and extension/status claims, broken cross-references, and the incorrect no-bounds-check claims in `docs/guide/10-language/30-statements.md` and `docs/guide/10-language/40-expressions-operators.md`.
- **Commit:** none (initial tree dirty; staging, commit, pull, and push prohibited)

## 2026-07-12 04:10 (EDT)
- **Tree state:** dirty (read-only audit) at HEAD `58c0bda1587c16e25ec52302a083fa2885a37c48`; the exact initial inventory was modified `docs/MAINTENANCE_LOG.md`, with no staged or untracked paths. `tmp_edit/` is gitignored and excluded by rule. No source or documentation maintenance fixes were made; this required log append is the only action exempted from read-only auditing.
- **Build:** PASS (`cmake --build buildt -j 8`; `ctest --test-dir buildt --output-on-failure -E bench`: 54/54 non-benchmark tests passed)
- **Findings:**
  - [NONE] No compiler warnings were emitted and all non-benchmark tests passed.
  - [DEFERRED] All source/doc maintenance remains deferred because the initial tree was dirty.
  - [NOTE] The required maintenance-log append is exempt from the clean-tree success criterion on a dirty run. A log-only commit was not made because the dirty-run rule explicitly prohibits staging, commit, and push; the pre-existing live-work path remains uncommitted and untouched except for this required append.
- **Commit:** none (initial tree dirty; staging, commit, and push prohibited)
