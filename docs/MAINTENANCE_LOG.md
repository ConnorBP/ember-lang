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
