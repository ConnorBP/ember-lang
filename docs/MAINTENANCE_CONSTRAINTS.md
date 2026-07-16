# Ember maintenance constraints

This file defines the rules for the automated maintenance job. Interactive,
explicitly requested work may have a wider scope, but it must still preserve
other users' changes and obey repository safety rules.

## 1. Never overwrite active work

Before any edit, run `git status --short --branch` in the Ember repository and
inspect submodules recursively when relevant.

- If unrelated tracked files are dirty, perform a read-only audit unless the
  owner explicitly assigned a non-overlapping file set.
- A dirty `thirdparty/vst3sdk` submodule is unrelated external state. Do not
  modify, stage, reset, clean, or commit it.
- `tmp_edit/` and `buildt/` are scratch/build directories. Do not edit them as
  source and do not stage them.
- Recheck status before staging because concurrent work may have appeared.
- Stage named files, not `git add -A`, when unrelated dirt exists.

The maintenance log may be appended in read-only mode only when that append is
itself expected; otherwise report findings without creating new dirt.

## 2. Priority order

1. Build and test regressions
2. Security and loader/trap/ownership correctness
3. Compiler warnings and undefined behavior
4. Documentation accuracy and broken links
5. Focused test coverage for confirmed gaps
6. Small duplication fixes
7. Style only when it directly improves correctness/readability

Do not replace a correctness fix with a benchmark-only containment, and do not
call an experimental/compatibility path secure by default.

## 3. Allowed automated changes on a clean tree

- Small warning, validation, ownership, or documentation fixes
- Focused tests for a confirmed defect
- Narrow low-risk duplication removal
- Maintenance-log summary

A change that alters serialization, calling convention, dispatch topology,
collector semantics, or public pass interfaces needs a dedicated human-reviewed
session even if the diff is small.

## 4. Prohibited automated changes

- No force push, history rewriting, amend, squash, or destructive reset.
- No unrequested new language features, passes, extensions, or architecture.
- No sweeping refactor (roughly more than three files or 50 behavior-changing
  lines) under the cron.
- No edits to `thirdparty/`, `buildt/`, or `tmp_edit/`.
- No silent changes to stable serialization boundaries:
  - `src/em_file.hpp` EMBL layouts
  - `src/thin_ir.hpp` serialized `ThinOp` numbering
  - `src/thin_ir_ser.*` blob layout
  - `self_hosted/MODULE_IMAGE_FORMAT.md` / EMBM loader contract
- No public pass-registry/interface changes without dedicated review.
- No weakening secure defaults, loader limits, W^X, permission checks, or trap
  recovery to make a test pass.

## 5. Build and test baseline

Supported configuration:

```bash
cd E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember
cmake -S . -B buildt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe
cmake --build buildt -j 8
ctest --test-dir buildt --output-on-failure
```

The current full supported inventory is **94 CTest tests**. Counts may differ in
a stale build directory or when optional dependencies are absent, so compare
`ctest --test-dir buildt -N` with a freshly configured tree before declaring
documentation stale.

For self-hosting changes also run:

```powershell
powershell -ExecutionPolicy Bypass `
  -File self_hosted/correctness_tests/run_parity_audit.ps1 `
  -EmberCli buildt/ember_cli.exe
```

Expected current result: **188/188**, 0 unsupported, 0 mismatch, 0 hangs.
Bootstrap changes must additionally exercise `self_hosted/bootstrap.ember`.

For optimization changes run the relevant pass/IR tests and the validation
sentinel. Benchmarks are evidence, not correctness gates unless a test checks a
specific value-equivalence invariant.

## 6. Commit protocol

1. Recheck `git status` and inspect the diff.
2. Run `git diff --check`.
3. Stage only intended `.md`/source/test files by explicit path.
4. Verify the staged diff and staged path list.
5. Use a descriptive commit message. Commit subjects and bodies must contain no
   at sign.
6. Do not push unless the task explicitly requests it.
7. Do not update the parent workspace gitlink unless explicitly requested and
   the parent tree can be changed safely.

For hourly maintenance, prefer:

```text
Hourly audit: concise description
```

For an explicit user task, describe the actual work rather than pretending it
was an hourly audit.

## 7. Documentation rules

- Distinguish the tagged release from unreleased branch state.
- Distinguish current contracts from historical plans/audit snapshots.
- Distinguish public EMBL `.em` versions from self-hosted EMBM versions.
- Verify examples by compiling/running them when practical.
- Verify relative links against real paths.
- State optional build/test conditions near counts.
- Do not advertise a capability solely because a planning file proposed it.
- When a feature is complete, remove “planned/subset/not yet implemented” from
  top-level current-status docs while preserving dated historical records as
  history.

## 8. Maintenance log format

Append a concise entry containing:

- date/time and branch/HEAD;
- initial tree state and any unrelated dirt;
- files changed;
- claims corrected;
- build/test/link-check results;
- remaining issues and their reason;
- resulting commit hash, or an explicit no-commit reason.
