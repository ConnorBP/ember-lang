# ember — Final Audit Synthesis (2026-07-11)

**Date:** 2026-07-11
**Scope:** Synthesis of the 7 final audit reports (docs, quality, speed, completeness, .em red team, sandbox red team, general red team).
**Repository HEAD:** `d1ad8bf`
**Test status:** 52/52 ctest pass (excluding 2 bench), 249/249 lang tests pass, validation 177.

---

## Audit reports (all committed)

| # | Report | Size | Key finding |
|---|---|---|---|
| 1 | FINAL_DOCS_AUDIT_2026-07-11.md | 22KB | docs accurate; stale claims fixed in-place |
| 2 | FINAL_QUALITY_AUDIT_2026-07-11.md | 19KB | 11 warnings → 0; 1 production bug fixed (uninit read) |
| 3 | FINAL_SPEED_AUDIT_2026-07-11.md | 11KB | 6-7x faster than AngelScript; 5-8x slower than g++-O2 |
| 4 | FINAL_COMPLETENESS_AUDIT_2026-07-11.md | 49KB | 16/17 features complete; 1 partial (iterable, by design) |
| 5 | FINAL_EM_REDTEAM_2026-07-11.md | 39KB | Finding A/B fixes verified solid; raw x86 drop verified |
| 6 | FINAL_SANDBOX_REDTEAM_2026-07-11.md | 50KB | sandbox correctly implemented; no new escape vectors |
| 7 | FINAL_GENERAL_REDTEAM_2026-07-11.md | 41KB | 1 HIGH (compiler stack overflow DoS), 2 MEDIUM |

---

## Executive summary

ember is in **strong shape** for a research-stage JIT scripting language. The audits found no critical security vulnerabilities (the .em + sandbox fixes from the earlier audit are verified solid), no data-corruption bugs, and no correctness regressions (52/52 tests, 249/249 lang tests, validation 177). The findings are:

### Bugs fixed during the audits

1. **Quality audit:** 11 GCC warnings → 0 (fixed in `7d38714`). One production bug: uninitialized variable read in the constexpr compound-assignment evaluator (`src/sema.cpp`) — fixed.
2. **End-to-end pipeline (t151):** codegen epilogue-label bug (implicit `mov rax, 0` emitted after the epilogue label, clobbering return values to 0) — fixed. Only visible when actually executing the emitted code (the byte-pattern test missed it).

### Open findings (TODO, not blocking)

#### Security (from general red team)

| Severity | Finding | Status |
|---|---|---|
| **HIGH** | Compiler stack overflow via unbounded recursion on crafted source (DoS — crashes the compiler process, not a code-execution bug) | TODO: add recursion depth limit in parser/sema |
| MEDIUM | frame-size `int32_t` accumulation without overflow check | TODO: check for overflow in sema |
| MEDIUM | `call_raw` deliberately crashes on null/garbage pointer (by design — PERM_FFI-gated) | Document as a risk |
| LOW | Uncapped string-literal allocation (memory-exhaustion DoS) | TODO: cap string literal size |
| LOW | Self-hosted compiler produces wrong code on out-of-scope input (can't crash the host) | TODO: expand the self-hosted compiler's subset |
| LOW | Bundler footer integer-wrap edge case (caught downstream) | TODO: defensive check |

#### Performance (from speed audit)

| Finding | Status |
|---|---|
| Tree-walker 5-8x slower than g++-O2 on most paths (stack-spill, no regalloc) | The SSA-IR + regalloc (Stage 3, implemented) is designed to close this — benchmark its impact (TODO) |
| constprop_fold: g++-O2 folds to 0ns; ember computes at runtime (1.5ms) | The constprop pass exists + is value-preserving; run passes by default for compute-heavy scripts (TODO: --opt flag) |
| Pass runtime impact not benchmarked (bench harness lacks --passes) | TODO: add --passes to bench_codegen_paths |
| Regalloc impact not benchmarked | TODO: benchmark Stage 3 regalloc on call/loop overhead |

#### Completeness (from completeness audit)

| Finding | Status |
|---|---|
| 16/17 features COMPLETE (full impl + passing tests) | ✅ |
| iterable() is PARTIAL (slice + array-handle only; map/host deferred by design) | By design |
| GC has unit tests but is NOT wired into the engine/codegen (standalone core) | TODO: wire GC into the engine for lambda/coroutine heap management |
| in-context threads: ctest passes but not linked into ember_cli (ember test/run can't exercise it) | TODO: link thread extension into the CLI |
| lambdas + coroutines tested via ember test but not in the bash lang_suite RUN list | TODO: add to lang_suite |

#### Documentation (from docs audit)

| Finding | Status |
|---|---|
| Stale claims fixed in-place (test counts, feature status, cross-refs) | ✅ Fixed |
| 54 ctest targets (52 excluding bench), 249 lang tests | ✅ Documented |

#### Platform support

| Finding | Status |
|---|---|
| Windows x64 only | Current |
| Linux x64 / macOS / 32-bit / ARM64 | TODO (#36-38) |

---

## Security posture (consolidated)

The three red-team audits (.em, sandbox, general) together cover the full attack surface:

1. **.em format** (FINAL_EM_REDTEAM): The Finding A (frame_size=0 stack-smash) + Finding B (PERM_FFI bypass) fixes are verified solid. Raw x86 v1-v4 are refused by default. v5 IR validator bounds frame_off unconditionally. v4 requires Ed25519 verification. No new .em issues found.

2. **Sandbox** (FINAL_SANDBOX_REDTEAM): The per-frame byte budget, call-depth guard, call-target provenance guard, and trap/checkpoint unwind are all correctly implemented. PERM_FFI is enforced at three independent gates (sema, .em bind, .em v5 rebind). The new features (lambdas, coroutines, exceptions, threads) do NOT introduce sandbox escape vectors. The sandbox is sound where it is enabled.

3. **General** (FINAL_GENERAL_REDTEAM): The C++ runtime is well-engineered. One HIGH finding (compiler stack overflow on crafted source — a DoS, not code execution). Two MEDIUM (frame-size overflow, call_raw null crash). The residual issues are DoS-class (crash the compiler process), not code-execution-class (inject/escape). The sandbox prevents a malicious SCRIPT from escaping; the compiler-crash DoS is a host-process robustness issue, not a sandbox breach.

**Bottom line:** ember's security posture is strong. The .em + sandbox attack surfaces are closed. The residual findings are DoS-class robustness issues (crafted input crashing the compiler process), not security breaches. The HIGH finding (recursion depth) is the most important to fix.

---

## Recommended next steps (priority order)

1. **Fix HIGH: compiler recursion depth limit** (parser/sema) — prevent stack overflow on deeply-nested crafted source.
2. **Wire GC into the engine** — the tracing GC core exists but isn't connected to the codegen; lambdas + coroutines need it for heap management.
3. **Link thread extension into ember_cli** — in-context threads have a ctest but aren't exercisable via `ember test`/`ember run`.
4. **Benchmark the regalloc + passes impact** — add `--passes` to the bench harness; measure how much Stage 3 regalloc closes the 5x gap to g++-O2.
5. **Platform ports** — Linux x64 first (#36), then macOS/ARM64 (#37), then 32-bit (#38).
6. **Expand the self-hosted compiler** — the current 4-stage self-hosted compiler handles a subset of ember; expanding to the full language is the path to the bootstrap milestone.
