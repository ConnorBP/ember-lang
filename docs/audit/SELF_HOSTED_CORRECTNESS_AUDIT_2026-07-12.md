# Self-Hosted Ember Compiler Correctness Audit — 2026-07-12

## Executive summary

The Ember-written compiler is **strong on scalar expression evaluation, direct calls, recursion, and large straight-line functions**, but it is not yet output-equivalent to the native compiler across the complete advertised subset.

A new corpus under `self_hosted/correctness_tests/` executed **47 audit cases**:

| Class | Cases | Pass | Fail | Hang/crash |
|---|---:|---:|---:|---:|
| Positive native-vs-self-hosted differential programs | 30 | 24 | 6 | 0 |
| Negative/out-of-subset rejection programs | 17 | 14 | 3 | 0 |
| **Total** | **47** | **38** | **9** | **0** |

The six positive failures reduce to one high-impact self-hosted sema scope defect. The three negative failures are silently accepted constructs. No generated code mismatch was observed after a program successfully passed the self-hosted frontend: all 24 successful positive programs returned exactly the native result.

The existing six self-hosted CTest targets also passed (lexer, parser, sema, codegen, full pipeline, and preview smoke).

## Scope and constraints

This was a read-only implementation audit:

- no C++ source was modified;
- no existing self-hosted compiler source was modified;
- no access to `G:` was performed;
- generated audit artifacts are limited to `.ember` fixtures, PowerShell/docs, and this report on `E:`.

The tested subset was the documented self-hosted subset: `i64`/`bool`/`void`, typed and inferred lets, assignment, `if`, `while`, C-style `for`, returns, scalar operators, direct calls of up to four arguments, multiple functions, forward calls, and recursion.

## Methodology

### Differential execution

Each positive fixture has a documented `// expect: N` value and was run in two modes:

```text
buildt/ember_cli.exe run self_hosted/correctness_tests/<case>.ember --fn main
```

and, via the silent file adapter:

```text
buildt/ember_cli.exe run self_hosted/correctness_tests/file_pipeline_runner.ember \
  --fn run_file --ffi
```

The second invocation reads the test path from stdin, calls `compile_file_and_run`, and therefore performs:

```text
source -> self-hosted lex -> parse -> sema -> codegen
       -> make_executable -> call_raw -> i64 result
```

`run_correctness_audit.ps1` uses `.NET Process` directly. This is important on Windows because MSYS/Bash exposes only an eight-bit exit status, while Ember uses the full process return value as its `i64` result channel. The script applies the CLI's 31-bit negative-result representation when checking self-hosted `-3xx` results.

### Rejection testing

`reject_*.ember` fixtures exercise parsed but unsupported declarations, statements, expressions, and ABI boundaries. Stable codes were checked for:

- `-301` struct
- `-302` enum
- `-303` float
- `-304` match
- `-305` switch
- `-306` try/catch or throw
- `-307` lambda
- `-308` coroutine/yield
- `-309` string
- `-310` array/slice
- `-311` for-each
- `-312` defer
- `-317` global

A five-argument call was required to fail negatively; it returned pipeline code `-401` at codegen, as expected for the current implementation.

### Timeout and crash handling

Every native and self-hosted process had a 30-second timeout. The runner records native/self-hosted timeouts independently and forcibly terminates a timed-out process. There were no hangs or crashes in this run, including `fib(30)`.

### Existing regression gate

The following was rerun:

```text
ctest --test-dir buildt -R "self_hosted|selfhost" --output-on-failure --timeout 120
```

Result: **6/6 passed** in approximately 0.29 seconds.

## Positive differential results

### Passing cases (24/30)

| Fixture | Coverage | Result |
|---|---|---:|
| `arithmetic_all.ember` | `+ - * / % & | ^ << >>`, signed shift | 163 |
| `assignment_expression.ember` | assignment result/right association | 28 |
| `block_local_only.ember` | isolated nested block/local | 42 |
| `block_partition_a.ember` | flat control-flow spelling | 42 |
| `bool_logic.ember` | bool literals, `&&`, `||`, `!` | 11 |
| `comparisons_all.ember` | `== != < <= > >=`, true/false | 63 |
| `complex_expression.ember` | precedence and nested expression trees | 65 |
| `divmod_negative.ember` | signed division/remainder | 67 |
| `fib20.ember` | recursive call volume | 6,765 |
| `fib30.ember` | deep/high-volume recursion | 832,040 |
| `for_no_outer_use.ember` | C-style `for` without outer capture | 4 |
| `forward_backward_calls.ember` | forward/backward direct calls | 44 |
| `four_args_nested.ember` | four-register Win64 ABI, nested args | 100 |
| `if_outer_assignment.ember` | outer local visible in one if block | 42 |
| `int64_boundaries.ember` | max/min and two's-complement overflow | 1 |
| `many_functions.ember` | 20-function forward call chain | 40 |
| `many_locals.ember` | 50 locals, long body/frame | 1,275 |
| `mutual_recursion.ember` | bool returns and mutual recursion | 42 |
| `nested_if_else.ember` | deeply nested branch tree | 42 |
| `short_circuit.ember` | RHS side effects skipped by `&&`/`||` | 33 |
| `typed_auto_lets.ember` | typed/inferred i64 and bool lets | 42 |
| `unary_all.ember` | unary `-`, `!`, `~` | 5 |
| `while_simple.ember` | simple while mutation | 5 |
| `zero_args_void.ember` | zero-arg calls, void return/fallthrough | 42 |

For every row above, native result = self-hosted result = documented expected result.

### Failing cases (6/30)

All six fail in self-hosted sema with ordinary sema status `1`, which `full_pipeline.ember` maps to `-302`. This `-302` is the historical stage mapping (`-301 - sr`), not the stable enum subset code.

| Fixture | Native | Self-hosted | Self-hosted diagnostic |
|---|---:|---:|---|
| `block_partition_b.ember` | 42 | `-302` | line 7: `undefined name 'x'` |
| `for_basic.ember` | 10 | `-302` | line 6: `undefined name 'sum'` |
| `for_steps.ember` | 72 | `-302` | first loop body loses outer `sum` |
| `nested_loops.ember` | 9 | `-302` | line 14: `undefined name 'i'` |
| `variable_shadowing.ember` | 130 | `-302` | line 13: `undefined name 'r'` |
| `while_complex.ember` | 16 | `-302` | line 13: `undefined name 'sum'` |

#### Root cause: self-hosted scope marks are append-only but read as a stack

`self_hosted/sema.ember` represents scope marks in `g_marks` and the logical symbol length in `s_scope_len`.

- `push_scope()` appends a mark with `array_push_i64`.
- `pop_scope()` clears the last physical element but cannot shrink `g_marks`.
- A later `push_scope()` appends after the cleared slot.
- `pop_scope()` and `declare()` continue to read `array_length(g_marks) - 1` as if it were the live stack top.

After sibling or nested scope activity, stale/cleared marks become part of the apparent stack. A pop can therefore restore `s_scope_len` to the wrong value—often zero—discarding enclosing locals from lookup.

This explains the pattern:

- simple `while` and a single `if` body can pass;
- a `for` checks its init/condition/step and then enters another body scope, triggering the problem before the outer accumulator is read;
- extra braces can change acceptance despite identical behavior;
- shadowed variables do not restore correctly after exiting the inner block;
- nested loops lose outer loop variables.

### Block partition invariance

This audit explicitly compared:

- `block_partition_a.ember`: flat spelling, result 42 in both compilers;
- `block_partition_b.ember`: equivalent extra lexical blocks, native result 42, self-hosted rejection `-302`.

Therefore **block partition invariance fails** in the self-hosted frontend. The failure is semantic scope bookkeeping, not emitted machine-code basic-block layout.

## Boundary results

### Limits that worked

- `fib(20)` and `fib(30)` completed with exact native results.
- A 50-local function generated and executed correctly.
- A 20-function forward call chain generated and executed correctly.
- Four arguments, including nested calls while preparing arguments, obeyed the Win64 register ABI.
- Direct and mutual recursion, bool-returning functions, forward references, and backward references worked.
- `INT64_MAX`, `INT64_MIN`, wraparound addition/subtraction, and arithmetic right shift matched native output.

### At/over the declared ABI boundary

A five-parameter/five-argument program is native-valid but outside the self-hosted four-register subset. The self-hosted pipeline rejected it with `-401` during codegen. Rejection is correct, though a dedicated stable sema subset code would be clearer and would prevent the unsupported AST from reaching codegen.

## Out-of-subset rejection results

### Correctly rejected (14/17)

All named unsupported language families produced the expected stable code: struct, enum, float, match, switch, try/catch, lambda, coroutine/yield, string, array, for-each, defer, and global declarations. The five-argument ABI boundary also failed negatively (`-401`).

No tested out-of-subset family was sent to machine-code execution after one of these rejections.

### Silently accepted (3/17)

1. **`break` outside a loop** — accepted by self-hosted sema. Codegen sees loop depth zero and emits no jump, so execution continues and returns 42.
2. **`continue` outside a loop** — same behavior: accepted, emitted as a no-op, returns 42.
3. **`@realtime` annotation** — accepted and discarded. The parser's `parse_top_decl()` consumes annotations but does not attach them to the function; sema cannot reject or enforce them. The annotated program compiles and returns 42.

The native compiler rejects the two loop-control fixtures, as it should. The native compiler accepts the safe `@realtime` fixture and enforces its contract; the self-hosted compiler merely ignores the annotation, creating a semantic divergence at the subset boundary.

The annotation behavior is broader than `@realtime`: Stage 2 currently consumes arbitrary annotations and keeps only a temporary token value that is not attached to the declaration. Unsupported annotations should not be silently erased.

## Crashes and hangs

- Native crashes: **0**
- Self-hosted compiler crashes: **0**
- Native hangs/timeouts: **0**
- Self-hosted hangs/timeouts: **0**

`fib(30)` is the highest-volume runtime case and completed within the 30-second process timeout.

## Recommendations

### P0 — Repair logical scope-mark stack handling

Add a logical `s_mark_len`, parallel to `s_scope_len`:

- `push_scope`: resize only when needed, write at `s_mark_len`, increment it;
- `pop_scope`: decrement `s_mark_len`, read that live mark, restore `s_scope_len`;
- `declare`: use the mark at `s_mark_len - 1`, never physical `array_length(g_marks) - 1`.

Do not use append-only physical array length as live stack depth. Add all six failing fixtures to the permanent gate, particularly the A/B block-partition pair.

### P0 — Validate loop-control context in sema

Track loop depth in `check_while`/`check_for` and reject `break` or `continue` when depth is zero. Codegen should also treat zero-depth loop control as an error rather than silently emitting nothing, providing defense in depth.

### P1 — Reject or preserve annotations

Until annotation semantics are self-hosted:

- attach annotation metadata to the declaration and return a stable unsupported-subset code; or
- reject annotations directly during parsing/sema.

Silently discarding `@realtime` is unsafe from a correctness-contract perspective because the native compiler assigns it observable semantic validation.

### P1 — Move the four-argument ceiling into sema

Reject functions/calls above four parameters with a stable `-3xx` subset code. Current `-401` codegen failure is safe but late and less diagnostic.

### P1 — Improve stage error-code disambiguation

Ordinary sema failure currently maps to `-302`, which is also the stable enum unsupported code. Use distinct non-overlapping ranges or preserve a structured stage/status pair so reports can distinguish “enum unsupported” from “ordinary sema error.”

### P1 — Wire this corpus into CTest after triage

The existing six self-hosted tests all pass but do not expose the scope defect. Once intended outcomes are accepted, register the PowerShell runner as a Windows CTest target. Keep negative fixtures in a separate mode so expected-known failures cannot be mistaken for a green release gate.

### P2 — Add deterministic generated differential testing

Generate bounded, well-typed scalar ASTs and equivalent block-partition variants, then run both compilers. Prioritize:

- scope push/pop sequences;
- sibling blocks and nested loops;
- calls nested in each argument position;
- signed division/modulo combinations;
- random operator precedence trees;
- branch/loop transformations that should preserve output.

## Confidence assessment

### High confidence

The tested implementation is correct for:

- i64 arithmetic, bitwise, shifts, comparisons, and unary operations;
- bool values and short-circuit logical behavior;
- expression precedence and assignment expressions;
- direct calls through four arguments;
- declaration-order-independent calls;
- direct and mutual recursion;
- substantial call volume (`fib(30)`);
- large local frames and many function labels;
- simple `if` and simple `while` control flow;
- the tested integer boundary/wrap behavior.

### Low confidence / known incorrect

The advertised subset is not currently reliable for:

- general lexical scoping after repeated/nested scope push/pop;
- variable shadow restoration;
- C-style `for` bodies that reference enclosing locals;
- nested loops;
- block partition invariance;
- validation of out-of-loop `break`/`continue`;
- annotation-bearing functions.

### Overall

**Confidence: moderate for the scalar/codegen core, low-to-moderate for the full advertised statement/scoping subset.**

A useful quantitative view is 24/30 positive differential programs passing (80%), but raw percentage understates the issue: the single scope defect affects several central language constructs. Conversely, among programs that passed self-hosted frontend validation, no x64 output discrepancy was observed in this corpus. Fixing scope bookkeeping and adding loop/annotation rejection should materially raise confidence without requiring a codegen redesign.

## Reproduction

```powershell
# Positive differential corpus: expected current audit result 24 pass / 6 fail
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1

# Positive + rejection corpus: expected current result 38 pass / 9 fail
powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -IncludeNegative

# Existing self-hosted gates: expected 6/6 pass
ctest --test-dir buildt -R "self_hosted|selfhost" --output-on-failure --timeout 120

# Print a self-hosted sema diagnostic for one fixture
echo self_hosted/correctness_tests/for_basic.ember | \
  buildt/ember_cli.exe run self_hosted/correctness_tests/diagnose_file.ember --fn diagnose --ffi
```
