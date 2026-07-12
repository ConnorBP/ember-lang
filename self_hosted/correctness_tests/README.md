# Self-hosted compiler correctness regression suite

This directory contains the read-only audit corpus created on 2026-07-12. No C++ source is modified by the suite.

## Run

From the Ember repository root on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -IncludeNegative
```

The first command runs positive differential tests. Each `.ember` program is executed by:

1. the native compiler (`ember_cli.exe run <test> --fn main`), and
2. the Ember-written pipeline through `file_pipeline_runner.ember` and `compile_file_and_run`.

The runner compares both full process return values with the `// expect: N` comment. PowerShell/.NET process APIs are used because MSYS truncates Windows process return values to eight bits.

`-IncludeNegative` also runs `reject_*.ember` and verifies stable self-hosted `-3xx` subset rejection (or any negative pipeline failure where no stable subset code exists).

`diagnose_file.ember` is a utility for printing the self-hosted sema error for one path supplied on stdin.

## Current expected audit result

The suite intentionally preserves discovered regressions:

- Positive differential corpus: **24 pass / 6 fail**.
- Negative/out-of-subset corpus: **14 pass / 3 fail**.
- Combined: **38 pass / 9 fail**, no hangs.

The six positive failures all expose the same scope-stack defect: after the self-hosted sema exits an inner block, an enclosing local may disappear. This breaks block-partition invariance, for-loop bodies that use outer accumulators, nested loops, shadow restoration, and complex while bodies.

The three rejection failures are silently accepted syntax:

- `break` outside a loop,
- `continue` outside a loop,
- `@realtime` (indeed annotations generally, because Stage 2 consumes and discards them).

See `docs/audit/SELF_HOSTED_CORRECTNESS_AUDIT_2026-07-12.md` for results and recommendations.
