# Self-hosted compiler correctness and parity suites

These tests compare the Ember-written compiler with the native C++ compiler. The current completion baseline is **188/188 parity with 0 unsupported cases**.

## Full parity audit: 188 cases

Run from the repository root on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_parity_audit.ps1 -EmberCli build/ember_cli.exe
```

The 188 positive programs are assembled by the script from:

- **87** `tests/lang/valid_*.ember` files containing a `// expect: N` marker
- **101** non-infrastructure, non-`reject_` programs in this directory containing `// expect: N`

For every positive program, the runner executes:

1. `ember_cli.exe run <program> --fn main` using the native compiler, and
2. `ember_cli.exe run self_hosted/correctness_tests/file_pipeline_runner.ember --fn run_file --ffi`, with the target path on stdin.

A passing result requires both implementations to equal the declared expectation. The completion baseline is:

```text
full parity:        188/188
unsupported subset: 0
real mismatches:    0
hangs:              0
```

The script also records the general invalid-language corpus and checks the permanent `reject_*.ember` contracts. Those categories validate diagnostics and deliberate rejections; they are not unsupported-feature counts.

## Local differential audit: 101 cases

```powershell
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -EmberCli build/ember_cli.exe
```

Current result:

```text
pass=101 fail=0 hangs=0
```

Add `-IncludeNegative` to include the local permanent rejection tests:

```powershell
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -EmberCli build/ember_cli.exe -IncludeNegative
```

## Why PowerShell?

The CLI maps an Ember `i64` result into a 31-bit process result. The audit scripts decode negative values back into their signed form. PowerShell/.NET process APIs preserve the Windows result channel reliably; POSIX compatibility shells may truncate it to eight bits.

## Infrastructure files

- `file_pipeline_runner.ember` — reads a target path from stdin and invokes the self-hosted file pipeline
- `diagnose_file.ember` — prints a self-hosted diagnostic for one stdin path
- `run_correctness_audit.ps1` — focused 101-case local differential run
- `run_parity_audit.ps1` — 188-case language + local parity run, plus invalid/rejection reporting

## Adding a test

1. Add `// expect: N` near the top.
2. Make `main() -> i64` return that exact value on success.
3. Avoid observing behavior only through stdout; the result code is the differential oracle.
4. Add a `reject_*.ember` file only for a permanent semantic/diagnostic contract, with `// selfhost-reject: CODE` (or `negative`).
5. Run both audit scripts and the bootstrap smoke test.

## Bootstrap smoke test

```console
echo tests/lang/valid_try_catch.ember | build/ember_cli.exe run self_hosted/bootstrap.ember --fn main --ffi
```

Expected result: `42`, produced by the second-generation compiler.
