# Immutable Pre-Cycle Baseline — DIRTY-READ-ONLY — 2026-07-15

> Worker chunk deliverable. Captured **before** any change or deletion, exactly as
> required by the pre-cycle baseline protocol. The tree is **DIRTY** (recursive
> submodule dirt in `thirdparty/vst3sdk`), so this run is **read-only audit only**:
> build + tests + validation were executed and evidenced below, but **no source file
> was edited, nothing was staged/committed/pushed, no stash/checkout/restore/clean,
> no `git reset --hard`, no force-push, no history rewrite.** All pre-existing
> tracked, untracked, and recursive-submodule dirt is preserved untouched.
>
> The planning-time inventory (branch `self-hosting-completion` @ `7c0a05f` with
> modified `self_hosted/codegen.ember` + `self_hosted/sema.ember` and untracked
> `self_hosted/correctness_tests` files) is **STALE**. HEAD has since advanced to
> `864652d`; those modifications were committed and the `correctness_tests` files
> are now tracked. A fresh baseline was captured below rather than trusting the
> planning-time snapshot.

## 1. Git identity / branch / HEAD / upstream

| field | value |
|---|---|
| repo | `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember` |
| branch (checked out) | `self-hosting-completion` |
| HEAD (oid) | `864652db58e0063396935a7b43d0f32b7482ea64` |
| `git describe --always --dirty` | `v1.3.0-24-g864652d-dirty` (dirty via submodule) |
| upstream | **none** — `git rev-parse --abbrev-ref @{u}` → `fatal: no upstream configured for branch 'self-hosting-completion'` (exit 128) |
| ahead/behind | **N/A** (no upstream configured; cannot compute) |

`git status -sb --branch` output:
```
## self-hosting-completion
 m thirdparty/vst3sdk
```

## 2. Remotes

```
origin  https://github.com/ConnorBP/ember-lang.git (fetch)
origin  https://github.com/ConnorBP/ember-lang.git (push)
```

## 3. Worktree registrations (`git worktree list`)

```
E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember                  864652d [self-hosting-completion]   <-- THIS repo (primary)
C:/Users/connor/AppData/Local/Temp/ember_head                     26b2263 (detached HEAD)
E:/DEVELOPER/PROJECTS/sus/ember_ir_pass_audit                     571f1fc [ir-pass-correctness-audit]
E:/DEVELOPER/PROJECTS/sus/ember_ir_ser_audit                      bfd22a3 [audit/ir-ser-2026-07-13]
E:/DEVELOPER/PROJECTS/sus/ember_pass_audit_v2                     8519f93 [ir-pass-correctness-audit-v2]
E:/DEVELOPER/PROJECTS/sus/ember_perf_audit_358b590d               358b590 (detached HEAD)
E:/DEVELOPER/PROJECTS/sus/ember_thin_ir_audit_8519f93             a8dae6b [thin-ir-emission-audit]
E:/DEVELOPER/PROJECTS/sus/ember_thin_ir_lower_audit               9ce749a [audit/thin-ir-lowering-2026-07-13]
E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember_regalloc_audit    dc5a16c [regalloc-audit-fix]
```
9 worktrees registered. Only the primary (`.../ember` @ 864652d) is in scope. No worktree
pruning or modification performed.

## 4. Staged changes (index)

```
git diff --cached --name-only   →  (empty)
git diff --cached --check       →  exit 0 (clean)
```
**Nothing staged.**

## 5. Tracked modifications (working tree)

```
git diff --name-only            →  thirdparty/vst3sdk
git diff --check                →  exit 0 (no whitespace errors)
```

`git status --porcelain=v2 --branch --untracked-files=all` (complete, one line):
```
# branch.oid 864652db58e0063396935a7b43d0f32b7482ea64
# branch.head self-hosting-completion
1 .M S.M. 160000 160000 160000 9fad9770f2ae8542ab1a548a68c1ad1ac690abe0 9fad9770f2ae8542ab1a548a68c1ad1ac690abe0 thirdparty/vst3sdk
```
- `XY = .M` (worktree modified, no index change)
- `sub = S.M.` (submodule gitlink commit unchanged at HEAD; submodule **content** modified)
- gitlink commit `9fad977…` matches HEAD `9fad977…` — **no gitlink drift**, only dirty content inside the submodule.

**The sole tracked modification is the `thirdparty/vst3sdk` submodule's modified content.**

## 6. Untracked paths

```
git ls-files --others --exclude-standard   →  (empty)
```
**No meaningful untracked source/test files.** (`git ls-files --others` without exclude
lists only gitignored build artifacts under `build/`, `buildt/`, `Testing/Temporary/`,
`analysis/`, `bench/results_*`, etc. — all covered by `.gitignore`; none are source/test.)

### `.gitignore` (verbatim)
```
/build
/build_my
/build_msvc
/tmp_edit
/build_ts
/buildt
/Testing
demo/**/*.exe
*.em
bench/*.dll
bench/results_codegen_paths.*
analysis/
build_cov/
build_asan/
```

### Status of paths the planning-time snapshot called "untracked"
- `self_hosted/correctness_tests/` — **now TRACKED** (111 files under `git ls-files self_hosted/correctness_tests/`; `git check-ignore` exit 1 = not ignored). Committed at HEAD.
- `self_hosted/codegen.ember`, `self_hosted/sema.ember` — **now TRACKED and committed** (part of 127 tracked files under `self_hosted/`). Not modified vs HEAD.

## 7. Submodule gitlinks + recursive submodule status

`.gitmodules`:
```
[submodule "thirdparty/vst3sdk"]
    path = thirdparty/vst3sdk
    url = https://github.com/steinbergmedia/vst3sdk.git
```
Top-level `git ls-tree HEAD | grep 160000` → no top-level gitlink line returned by that
grep (the submodule is recorded at path `thirdparty/vst3sdk`; `git submodule status`
confirms it). Gitlink commit at HEAD: `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`.

`git submodule status --recursive` (leading char = status: space = at-recorded-commit, `+` = checked-out differs, `-` = not init, `U` = merge conflict):
```
 9fad9770f2ae8542ab1a548a68c1ad1ac690abe0 thirdparty/vst3sdk (v3.7.3_build_20-15-g9fad977)
 3d2e82f8e6bff59c1d8b7a27491a29c2286b5206 thirdparty/vst3sdk/base
 de6e54eeaaab35b7145f5c32c279b5e892146e04 thirdparty/vst3sdk/cmake
 6d4737c9e70750056e731d88d49aa06eefc8a1a4 thirdparty/vst3sdk/doc
 31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c thirdparty/vst3sdk/pluginterfaces
 a3911a4615dabbfdfd9d181ee26b05c70c289a95 thirdparty/vst3sdk/public.sdk
 33b73dfbb87f3fde3bce8c0a10cae934dc66ad34 thirdparty/vst3sdk/tutorials
 76823bdbe286e4bdb9f79ab8986af5ce7202336c thirdparty/vst3sdk/vstgui4
```
All gitlinks at their recorded commits (leading space, no `+`/`-`/`U`). **No gitlink drift.**

### Recursive submodule DIRT (the single dirty item in the whole tree)
Inside `thirdparty/vst3sdk`, the nested `public.sdk` submodule has **modified content**
(one file). The vst3sdk submodule itself is in detached HEAD (`HEAD (no branch)`).

`thirdparty/vst3sdk` porcelain:
```
1 .M S.M. 160000 160000 160000 a3911a46… a3911a46… public.sdk
```
`thirdparty/vst3sdk/public.sdk` porcelain:
```
1 .M N... 100644 100644 100644 0ce85e72… 0ce85e72… source/vst/utility/alignedalloc.h
```
`git -C thirdparty/vst3sdk/public.sdk diff source/vst/utility/alignedalloc.h`:
```diff
@@ -29,6 +29,10 @@
 #include <malloc.h>
 #endif

+#if defined(__MINGW32__) || defined(__MINGW64__)
+#include <malloc.h>
+#endif
+
@@ -53,7 +57,7 @@
-#elif defined(_MSC_VER)
+#elif defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
 	data = _aligned_malloc (numBytes, alignment);
@@ -68,7 +72,7 @@
-#if defined(_MSC_VER)
+#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
 		_aligned_free (addr);
```
**Classification: PRE-EXISTING owner/environment work** — a MinGW portability patch to the
vendored VST3 SDK so `_aligned_malloc`/`_aligned_free` and the `malloc.h` include apply
under MinGW g++ (the documented build toolchain: MinGW g++ 15.2.0). This is the
"dirty thirdparty/vst3sdk" noted at planning time; it persists at HEAD `864652d`. It lives
under `thirdparty/`, which the maintenance constraints forbid modifying, and is **not**
cycle-generated. **Left untouched.**

`git diff --check` inside both the submodule and `public.sdk` → exit 0 (no whitespace errors).

## 8. Path classification summary (pre-existing owner vs cycle-generated)

| path | status | classification |
|---|---|---|
| `thirdparty/vst3sdk/public.sdk/source/vst/utility/alignedalloc.h` | modified (uncommitted) inside nested submodule | **PRE-EXISTING** owner/environment (MinGW port patch) — in `thirdparty/`, untouched |
| `self_hosted/codegen.ember`, `self_hosted/sema.ember` | tracked, committed @ HEAD, clean | pre-existing owner work (committed) |
| `self_hosted/correctness_tests/**` (111 files) | tracked, committed @ HEAD, clean | pre-existing owner work (committed; was untracked at planning time, now committed) |
| all other tracked source/tests | clean vs HEAD | pre-existing owner work (committed) |
| gitignored build artifacts (`build/`, `buildt/`, `Testing/Temporary/`, `analysis/`, `bench/results_*`) | untracked, ignored | generated (not owner work, not in scope) |
| `docs/audit/BASELINE_DIRTY_READONLY_2026-07-15.md` (this file) | new, untracked | **CYCLE-GENERATED** audit artifact |
| `docs/MAINTENANCE_LOG.md` (append) | tracked, appended this run | **CYCLE-GENERATED** audit append (sanctioned by MAINTENANCE_CONSTRAINTS for dirty-tree runs) |

**No cycle-generated source/test changes were made.** The only cycle-generated writes are
this manifest and the maintenance-log append (both docs, both sanctioned for a dirty-tree
read-only audit).

## 9. Baseline declaration

**DIRTY-READ-ONLY.** Pre-existing recursive submodule dirt exists
(`thirdparty/vst3sdk/public.sdk/source/vst/utility/alignedalloc.h`, modified). Per the
protocol and `docs/MAINTENANCE_CONSTRAINTS.md`, a dirty tree triggers **read-only audit
only**: build + tests + validation are run and evidenced, findings recorded, but **no
source edits, no stash/checkout/restore/clean, no stage/commit/push, no
`git reset --hard`, no force-push, no history rewriting.** The dirty submodule is in
`thirdparty/` (off-limits) and is preserved exactly. A `CLEAN-MAY-FIX` manifest is **not**
recorded because the tree is not genuinely clean.

**HEAD of record for later comparison: `864652db58e0063396935a7b43d0f32b7482ea64`.**

## 10. Success-gate execution (read-only audit) — exact commands + return codes

All three gates run after the baseline was captured. The build writes only to gitignored
`buildt/`; no tracked source, no `thirdparty/` source, no `tmp_edit/` was modified.

### Gate 1 — Build
```
cd E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember
cmake --build buildt -j 8
```
- stdout (tail): `ninja: no work to do.`
- **return code: 0** ✅
- The buildt is current with HEAD's `CMakeLists.txt` (`.ninja_log`/`.ninja_deps` refreshed
  2026-07-15 14:22, after the 09:11 `CMakeLists.txt` edit; Ninja reports no stale outputs).
  Build toolchain: MinGW g++ 15.2.0, C++17, Release `-O3 -DNDEBUG`, Ninja, in `buildt/`.

### Gate 2 — Full unfiltered CTest
```
cd buildt && ctest --output-on-failure
```
- **return code: 8** ❌ (CTest non-zero = at least one test failed)
- result: **73/74 passed, 1 failed**
- total test time: 130.57 s
- **FAILED test: `#39 self_hosted_preview_smoke`**
  - `tests/selfhost_preview_smoke.cmake:17` FATAL_ERROR:
    `self-host preview returned -201, expected 42`
  - stdout excerpt: `ember self-host preview: enter an ASCII .ember path` / `subset:
    scalar/string ops, f-strings, floats, flat structs, enums/match, calls/recursion` /
    `self-host compile failed with code -201`
  - Mechanism: `add_test self_hosted_preview_smoke` runs the bundled
    `EMBER_SELFHOST_PREVIEW` = `buildt/ember_selfhost_preview.exe` (custom target bundling
    `self_hosted/emberc.ember`) with stdin = path to
    `self_hosted/preview_smoke_input.ember` (`fn twice(n)=n*2; fn main()=twice(21)` →
    expected exit 42). The bundled self-host compiler fails to compile the trivial input
    and exits with code -201 instead of computing `twice(21)=42`.
  - Manual repro confirms: `printf 'self_hosted/preview_smoke_input.ember\n' |
    ./buildt/ember_selfhost_preview.exe` → banner + `self-host compile failed with code -201`.
  - **Disposition: UNFIXED (read-only).** This is a regression/defect in the self-hosted
    compiler (`self_hosted/emberc.ember` preview path) surfaced at HEAD `864652d`.
    Likely related to recent commits advancing HEAD past the planning-time `7c0a05f`
    (`84702c7` Aggregate args/returns ABI: struct by-value; `7c0a05f` String globals; etc.).
    Not fixed in this chunk — DIRTY-READ-ONLY prohibits source edits. Handed off for a
    clean-tree (or owner-sanctioned) chunk to diagnose `emberc.ember`'s preview compile
    path and the -201 error code.

#### Note on the "67/67" expectation vs actual count
The task's success criterion states `ctest 67/67`. The **actual full unfiltered suite at
HEAD `864652d` is 74 tests** (`ctest -N` → `Total Tests: 74`; `CMakeLists.txt` has 75
`add_test` calls). `docs/ROADMAP.md` records an older count (73 total / 68 filtered
excluding the four bench tests; full unfiltered 73/73) — the ROADMAP is one test behind
the current tree. The "67/67" figure predates the current tree (the count has grown
49 → 54 → 67 → 70 → 73 → 74 across sessions). **Neither the count (74, not 67) nor the
all-pass property (1 failure) is met.** This count drift + the one failure are recorded
as findings; nothing was altered to force a 67.

### Gate 3 — Validation 177 (value-preservation)
Required pass list (canonical, per `scripts/prepare_release.sh` and
`docs/audit/DOCS_AUDIT_REVALIDATION_2026-07-13.md`):
`constprop,forward,copyprop,instcombine,dce,licm,dse`. Test header
(`tests/lang/optimization_validation.ember`) declares `// expect: 177` and requires the
same exit both with and without passes.

With required pass list:
```
cd buildt && ./ember_cli.exe run ../tests/lang/optimization_validation.ember --fn main --passes constprop,forward,copyprop,instcombine,dce,licm,dse
```
- **return code: 177** ✅ (PASS)

Without passes (value-preservation cross-check):
```
cd buildt && ./ember_cli.exe run ../tests/lang/optimization_validation.ember --fn main
```
- **return code: 177** ✅ (PASS — passes are value-preserving; identical result with and without the 7-pass pipeline)

## 11. Gate summary

| gate | command | result | rc | status |
|---|---|---|---|---|
| Build | `cmake --build buildt -j 8` | no work to do (current) | 0 | ✅ PASS |
| CTest (full, unfiltered) | `cd buildt && ctest --output-on-failure` | 73/74, 1 failed (`self_hosted_preview_smoke`) | 8 | ❌ FAIL |
| Validation (with passes) | `ember_cli.exe run … --passes constprop,forward,copyprop,instcombine,dce,licm,dse` | exit 177 | 177 | ✅ PASS |
| Validation (no passes) | `ember_cli.exe run …` | exit 177 | 177 | ✅ PASS |

## 12. Findings / regression disposition (read-only — nothing fixed)

1. **`self_hosted_preview_smoke` FAIL (regression).** Bundled self-host preview
   (`ember_selfhost_preview.exe` ← `self_hosted/emberc.ember`) returns -201 instead of 42
   on the trivial `preview_smoke_input.ember`. **Unfixed** — DIRTY-READ-ONLY. Candidate
   root-cause area: `self_hosted/emberc.ember` preview/compile path vs recent
   struct-by-value ABI + string-globals commits at `864652d`. Hand off to a clean-tree
   chunk.
2. **CTest count drift.** Actual 74 (not the criterion's 67; ROADMAP says 73). The
   criterion's "67/67" is stale relative to HEAD `864652d`. No action taken (read-only).
3. **Stale planning-time inventory.** The `7c0a05f` snapshot (modified
   `codegen.ember`/`sema.ember`, untracked `correctness_tests`) no longer holds; HEAD is
   `864652d`, those files are committed, `correctness_tests` is tracked. Fresh baseline
   captured here.
4. **Persistent `thirdparty/vst3sdk` submodule dirt** (MinGW port patch to
   `alignedalloc.h`) — pre-existing, in `thirdparty/`, preserved untouched.

## 13. Constraints honored

- No `git reset --hard`, no `git clean`, no force-push, no history rewriting. ✔
- No stash / checkout / restore / stage / commit / push. ✔
- No access to `G:`. ✔
- No modifications to `thirdparty/` (the dirty `alignedalloc.h` left exactly as found). ✔
- No modifications to `tmp_edit/`. ✔
- No `NUL` file created. ✔
- No `temp_*.cpp` file created. ✔
- Build/test/validation ran read-only against the dirty tree per
  `docs/MAINTENANCE_CONSTRAINTS.md` (dirty-tree rule: run build + tests, record findings,
  do not edit source / commit / push). ✔
