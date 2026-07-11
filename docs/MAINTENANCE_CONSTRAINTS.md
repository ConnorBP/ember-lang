# ember maintenance constraints — rules for the hourly audit cron

> **This document is the rulebook for the automated hourly maintenance cron.**
> The cron reads this file at the start of each run and follows these rules.
> Edit this file to adjust the cron's behavior without editing the schedule.

## The prime directive: do not interrupt ongoing work

**Before making ANY changes, check `git status` in the ember repo.**

- **Dirty working tree** (uncommitted modifications to tracked files, excluding `tmp_edit/` which is gitignored): someone is actively working. The cron must do **read-only audit only** — run the build + tests, read the docs, identify findings, but do NOT edit any source files, do NOT commit, do NOT push. Append findings to `docs/MAINTENANCE_LOG.md` and stop.
- **Clean working tree** (no uncommitted changes): nobody is editing. The cron may make fixes, commit, and push as described below.

This ensures the cron never conflicts with a live work session. If you're in the middle of a focused edit and the cron fires, it will see your uncommitted changes, do a read-only pass, and leave your working tree untouched.

## Priority order (highest to lowest)

1. **Build + test baseline**: if the build is broken or any ctest fails, fix it first. Nothing else matters until the build is green.
2. **Security**: unfixed confirmed defects from `docs/audit/AUDIT_2026-07-09.md` with clear one-line fixes.
3. **Compiler warnings**: each `-Wunused-variable`, `-Wsign-compare`, etc. is a fix.
4. **Doc accuracy**: stale banners, broken cross-references, inaccurate claims.
5. **Code duplication**: extract into a shared helper ONLY if the duplication is clear and low-risk.
6. **Style**: do NOT change working code for style preferences.

## What the cron may do (clean tree only)

- Fix compiler warnings (unused variables, sign-compare, etc.).
- Fix doc inaccuracies (stale banners, broken links, wrong claims).
- Fix unfixed audit defects with clear, small, safe fixes.
- Extract obvious code duplication into a shared helper (low-risk only).
- Append a run summary to `docs/MAINTENANCE_LOG.md`.

## What the cron must NOT do (ever)

- **No new features.** No new language constructs, no new passes, no new extensions.
- **No architecture changes.** No refactoring of the pass interface, the .em format, the IR, the codegen pipeline.
- **No sweeping refactors.** If a refactor touches more than ~3 files or ~50 lines, it's too big for an automated cron — note it in the maintenance log and leave it for the human.
- **No force-push.** Never `git push --force`. If the push fails, `git pull --rebase` first.
- **No history rewriting.** Never rebase, amend, or squash existing commits.
- **No changes to `tmp_edit/`** — it's gitignored scratch space for ad-hoc probes.
- **No changes to `thirdparty/`** — external dependencies.
- **No changes to the .em format spec** (`src/em_file.hpp` format comments) or the `ThinOp` enum (`src/thin_ir.hpp`) — these are stable serialization boundaries.
- **No changes to the pass interface** (`src/ember_pass.hpp`, `src/ember_pass_registry.hpp`) — the interface is stable.
- **No changes to `docs/spec/` design docs** — these are design decisions, not maintenance items. Fix typos/cross-refs but do not change the design.

## Commit + push protocol

1. After all fixes, `git add -A` (in the ember repo).
2. `git commit -m "Hourly audit: <summary of fixes>"`.
3. `git push origin master`.
4. If the push fails (remote has advanced), `git pull --rebase origin master` and `git push origin master`.
5. Then update the parent repo: `cd E:/DEVELOPER/PROJECTS/sus/hyper_workspace && git add ember && git commit -m "Update ember submodule: hourly audit" && git push`.
6. If no changes were needed, skip the commit entirely (don't create empty commits).

## Build + test commands

```bash
cd E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember
cmake --build buildt -j 8
cd buildt && ctest --output-on-failure
```

The build is MinGW g++ 15.2.0, C++17, Release `-O3 -DNDEBUG`, Ninja, in `buildt/`.
The project is LAZY MODE (no unrequested abstractions, minimum that works). But never cut security, validation, or correctness.

## Maintenance log

The cron appends a summary of each run to `docs/MAINTENANCE_LOG.md` (see that file for the format). This is the cron's memory between runs — it prevents re-fixing the same thing and gives you a visible audit trail.
