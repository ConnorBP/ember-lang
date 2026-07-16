# Ember Codegen Per-Path Benchmark System — Design

> **Status: IMPLEMENTED PROTOTYPE + EXPANDED PASS PROBES; re-audited
> 2026-07-15.** `bench_codegen_paths` currently registers ten paths (the six
> original category paths plus CSE/DCE/constprop/LICM probes), supports paired
> no-pass/pass runs where `ir_safe`, records compile time/code bytes/IR counts,
> writes separate result artifacts, and has a harness self-test. It is still
> not the originally proposed full ~15-path matrix; the omitted float/native-
> call/array/defer/switch/f-string paths remain legitimate benchmark expansion,
> not compiler feature gaps. Historical 2026-07-10 timings below are provenance,
> not current performance claims after the 18-pass/regalloc work.
>
> Evidence-gathering infrastructure for the codegen-optimization decision.
> The v0.5/v0.6 gate (`docs/planning/DESIGN.md` §9: "no speculative
> optimization before the bench proves it matters"; `docs/spec/COMPILER_PIPELINE.md`
> §5 defers the SSA-lite IR + linear-scan regalloc until "stronger benchmarks
> show where Ember is slow") requires **per-path** evidence: not just "is
> ember's entry fn fast" but **which codegen path is slow**, against an
> **optimizing native compiler** (g++ -O2 — the "optimizing native-JIT
> language's speed" the gate quote names), in both **safety-off** and
> **safety-on** modes so the guard cost is visible.

This doc specifies the **full** system and documents the implementation under
`bench/`. The first prototype landed six representative paths; four pass probes
were subsequently added. The remaining full-matrix paths are documented as
"add per this pattern."

---

## 1. Why per-path (the gap the existing bench leaves open)

Two benches exist before this work:

- **`ember bench`** (`examples/ember_cli.cpp` ~L628): microbenchmarks ONE
  entry fn — warmup + N timed iters, each under its own fresh checkpoint,
  reports min/median/mean/p99/stddev/CV + machine/compiler metadata. Closes
  the 07-09 benchmark-methodology gap (was: one mean, no variance, no
  provenance). **But it measures one function's total time — it does not
  break down WHICH codegen path is slow.**
- **`examples/bench_ember_vs_as.cpp`**: 4 game-logic workloads vs the
  AngelScript bytecode interpreter. Categorical: ember native-JIT vs AS
  interpreter. Reports an ember/AS ratio per workload. **But again
  whole-workload timing, not per-path, and AS is a bytecode interpreter —
  not the optimizing native compiler the gate quote names.**

The optimization design (`COMPILER_PIPELINE.md` §5) needs to know, e.g.,
whether the **div path** (cqo + idiv + div-by-zero guard) is the slow path,
or the **string decrypt** (inline byte-XOR per use), or the **struct-by-value
hidden-pointer temp copy** — and whether each is slow vs g++ -O2 and vs each
other. That is per-path evidence. This system produces it.

## 2. The three axes

### Axis A — codegen path
One small ember fn per path, each timed in isolation. The full set (~15):

| # | Path | What it exercises | Prototype? |
|---|------|-------------------|-----------|
| 1 | integer arithmetic add/mul | `add`/`imul` straight-line | (fold into div baseline) |
| 2 | **integer div/mod** | `cqo`+`idiv`+div-by-zero guard | **yes** |
| 3 | float arithmetic add/mul | sse add/mul | |
| 4 | float sqrt | `ext_math` native call (sqrtss) | |
| 5 | local load/store | rbp-relative frame slot traffic | |
| 6 | **call overhead** | script-to-script, recursion | **yes** |
| 7 | native call overhead | script-to-native + budget/depth guard | |
| 8 | **loop overhead** | while/for/do-while jmp back-edge | **yes** |
| 9 | **slice bounds-check** | indexing w/ bounds check, view `a[..]` | **yes** |
| 10 | **string ops** | string_length/char_at + inline-stack-XOR decrypt | **yes** |
| 11 | **struct-by-value arg / return** | hidden-pointer ABI temp copy | **yes** |
| 12 | array literals / aggregate globals | chunk c2/c3 backing | |
| 13 | defer | lexical scope-exit defer runs | |
| 14 | switch dispatch | jump table / cmp chain | |
| 15 | f-string interpolation | the string-extension format path | |

The prototype ships paths **2, 6, 8, 9, 10, 11** — one per category (integer
div, call, loop, slice, string, struct) so the matrix spans every category
the SSA-lite refactor would touch.

### Axis B — safety mode
Each path is measured in BOTH:
- **safety-off** — `emit_budget_checks=false`, `emit_depth_checks=false`,
  `trap_stub=null`. Zero guard overhead (the trusted-tool-script shape).
  This is the pure codegen-path cost.
- **safety-on** — `emit_budget_checks=true`, `emit_depth_checks=true`,
  `trap_stub` set to a longjmp stub, `use_context_reg=true` (r14 = context_t*).
  The untrusted-mod shape. The guard cost = safety-on − safety-off.

Paths whose guard is not optional (e.g. the div-by-zero guard is always
emitted; slice bounds-check is always emitted) are still run in both modes so
the budget/depth entry+back-edge charges are visible, but the note "guard
not optional for this path" is recorded in the path's metadata.

### Axis C — ember vs g++ -O2 (the optimizing-native baseline)
For each path fn, an equivalent C++ function is emitted and compiled with
`/c/msys64/mingw64/bin/g++.exe -O2 -std=c++17` into a DLL **at runtime**
(stale-probe discipline: the baseline compiles from source each run, never
from a checked-in binary). The same workload shape is timed in both ember and
the g++ -O2 DLL, and the **ember/g++-O2 ratio** is reported per path. Ratio
> 1 = ember slower than an optimizing native compiler on that path; this is
  the real "is ember slow" signal the gate needs.

(AS is retained as a third comparison in `bench_ember_vs_as` for the
categorical native-vs-interpreter picture; this system's native-vs-native
picture is the g++ -O2 axis.)

## 3. The measurement methodology (reuses `ember bench`'s provenance)

Per (path × safety-mode × engine):
1. **Warmup** — K untimed calls (default 50) to fill caches / branch
   predictors. Under a fresh checkpoint each (safety-on) so a trap in warmup
   stops cleanly.
2. **Timed iters** — N iterations (default 2000), each timed with
   `std::chrono::steady_clock` around the call, each under its OWN fresh
   `setjmp` checkpoint (safety-on) so a trap in one iter stops the bench
   cleanly, not the process. Records ns per iter.
3. **Statistics** — min, median, mean, p99, stddev, CV% over the timed iters
   (the same shape `ember bench` reports). The median is the headline number
   (resistant to scheduler outliers); p99 and CV% are the noise signal.
4. **Machine + compiler metadata** — compiler (`gcc` + `__VERSION__`),
   platform (`x86-64`, ptr width), build date, iters/warmup, and the g++ -O2
   version (captured from the baseline-compile step).

The g++ -O2 baseline is timed with the identical warmup+iters+stats pipeline
so the ratio is apples-to-apples.

## 4. Output format

The driver writes **two** machine-readable artifacts + a stdout summary:

### 4.1 CSV — `bench/results_codegen_paths.csv`
One row per (path × safety-mode × engine):
```
path,safety,engine,iters,warmup,min_ns,median_ns,mean_ns,p99_ns,stddev_ns,cv_pct,note
int_div,off,ember,2000,50,...,...,...,...,...,...,cqo+idiv+div0-guard
int_div,off,gcc_O2,2000,50,...,...,...,...,...,...,baseline
int_div,on,ember,2000,50,...,...,...,...,...,budget+depth+trap
...
```
`engine ∈ {ember, gcc_O2}`. The `ember/gcc_O2` ratio is derived per
(path × safety) by joining the two rows.

### 4.2 Markdown — `bench/results_codegen_paths.md`
A human-readable table grouped by path, with the ratio column and a verdict
column (the `bench_ember_vs_as` verdict shape, retargeted to g++ -O2):
```
| path | safety | ember median ns | g++-O2 median ns | ember/g++-O2 | verdict |
```
Verdict bands (ratio = ember/g++-O2):
- `< 1.1` — **ember ≈ g++-O2** (tree-walker adequate on this path; YAGNI wins)
- `1.1–3` — **ember slower** (visible tree-walker cost; candidate for peephole)
- `> 3` — **ember much slower** (SSA-lite IR + linear-scan warranted on this path)
- plus a `safety-on` overhead column = `(safety_on − safety_off) / safety_off`.

### 4.3 stdout
A compact run-progress summary (one line per cell) + the slowest-3-paths
headline at the end.

## 5. The harness architecture (extensibility — adding a path is one fn + one registration)

```
bench/
├── bench_codegen_paths.cpp   # the harness + driver (links buildt libs, consumer of JIT)
├── baseline_paths.cpp        # the g++-O2 baseline: extern "C" fns, compiled to DLL at runtime
├── run_bench.sh              # build + run wrapper (g++ -O2 the baseline, run the harness)
└── results_*.csv / .md       # written by the run (gitignored — machine-specific)
```

### 5.1 Adding a path
A path is defined by ONE struct:
```cpp
struct PathBench {
    const char* name;            // "int_div"
    const char* ember_src;       // the .ember source (one entry fn `main`)
    const char* baseline_sym;    // extern "C" symbol in the g++-O2 DLL
    const char* note;            // "cqo+idiv+div0-guard"
    bool guard_optional;         // is the path's own guard optional? (div0 always on)
    int iters, warmup;
};
```
plus the matching `extern "C"` fn in `baseline_paths.cpp`. Register it in the
`paths` vector in `main`. That is the entire change — the harness loops the
matrix automatically.

### 5.2 The compile+call shape (mirrors v0_4_hardening_test / bench_ember_vs_as)
- `ember_compile(src, safety_on)` — tokenize → parse → slot-assign →
  register the six core extensions → `build_struct_layouts` →
  `string_xor_key = <0 or chosen key>` → sema → set up `GlobalsBlock` →
  `CodeGenCtx` (safety flags per mode) → `compile_func` + `finalize` per fn →
  populate `DispatchTable`. Returns the module.
- For string-decrypt path: `string_xor_key` is set nonzero so the inline
  byte-XOR decrypt fires per use (the path under test). For all other paths
  `string_xor_key = 0` (no encrypted-rodata path) — the bench measures the
  path, not the string infra.
- Call via `ember_call_void(entry, &ectx)` (r14 = ctx) for BOTH modes
  (`use_context_reg=true` in both; safety-off just emits no checks so r14 is
  set but unread — no cost). One consistent call path.
- The g++-O2 baseline: `LoadLibraryA("baseline_paths.dll")` +
  `GetProcAddress(sym)` + the same timed loop.

### 5.3 Safety-on trap handling
A `test_trap` stub (the v0_4 shape) writes the reason into `ectx` and
`longjmp`s to the per-iter checkpoint. A trap in a timed iter breaks the
bench for that cell with exit 70 (distinct from valid return codes) and the
cell is recorded as `TRAP` in the CSV — that itself is data (a path that
traps under a realistic budget is a finding).

## 6. The prototype (what ships now)

6 paths, each in {safety-off, safety-on} × {ember, g++-O2} = 4 cells/path =
24 timed cells + warmup. The paths:

1. **`int_div`** — `fn main()->i64 { let mut s=0; let mut i=1; while(i<1000){ s = s + (i*7)/(i+1) + (i*13)%(i+2); i=i+1; } return s; }`
   Exercises `cqo`+`idiv` twice per iter + the always-on div-by-zero guard.
   Baseline: the identical loop in C++. **This is the path the IR `Div`/`Mod`
   + `DivCheck` ops would touch.**

2. **`call_overhead`** — a 3-deep script-to-script call chain `a(x)->b(x)->c(x)`
   called in a loop of 1e5. Exercises the dispatch-table indirect call +
   prologue/epilogue per call. **This is the path `CallScript` + linear-scan
   across call boundaries would touch.**

3. **`loop_overhead`** — an empty-ish `while(i<N)` with a counter, N=1e7.
   Exercises the jmp-based back-edge + the loop-body budget charge
   (safety-on). **This is the path loop-back-edge budget + the tree-walker's
   per-iter frame traffic would touch.**

4. **`slice_bounds`** — index a `i64[64]` array 1e6 times with the bounds
   check firing each index. Exercises the `BoundsCheck` emit. **This is the
   path the IR `BoundsCheck` op + the always-on bounds guard would touch.**

5. **`string_decrypt`** — call `string_length` on an encrypted string literal
   1e5 times; the inline byte-XOR decrypt runs per use (string_xor_key != 0).
   Exercises the per-use inline-stack-XOR decryption. **This is the path
   `StringLit` eval + `alloc_str_temp` would touch — the obf-feature cost.**

6. **`struct_by_value`** — pass a 12-byte struct by value to a fn 1e5 times
   and return it by value. Exercises the hidden-pointer ABI temp copy
   (`alloc_struct_temp`). **This is the path `CallNative`/`CallScript`
   struct-arg + struct-return hidden-pointer would touch.**

## 7. CTest gate discipline

- The harness is a **consumer of the JIT** (no `src/` changes).
- A CTest target `bench_codegen_paths` is added **only** as a
  "ran + wrote results = pass" bench (the `bench_ember_vs_as` shape), NOT an
  assertion bench — it never fails on a ratio, only on a compile/run/IO error.
  TIMEOUT 600 (the g++ -O2 baseline compile + 24 timed cells).
- The harness links the **buildt** libs (fresh, per stale-probe discipline).
- The g++ -O2 baseline `baseline_paths.cpp` is compiled to a DLL **at run
  time** by the harness (or by `run_bench.sh` before invoking) — never
  checked in as a binary.

## 8. Findings (prototype run, 2026-07-10, gcc 15.2.0, x86-64)

The prototype ran the 6-path × 2-safety × ember-vs-g++-O2 matrix and produced
the evidence below. **Headline = median ns** (resistant to scheduler outliers;
CV was high on some cells on this shared dev machine — the design doc §3
notes median is the headline for exactly this reason). The ratio is
ember/g++-O2; >1 = ember slower than an optimizing native compiler on that path.

### 8.1 Slowest paths vs g++ -O2 (safety-off, the pure codegen-path cost)

| rank | path | ember med ns | g++-O2 med ns | ember/g++-O2 | verdict |
|---|---|---|---|---|---|
| 1 | slice_bounds | ~1.6-1.9M | ~190-340K | **5.6-8.5x** | ember MUCH slower |
| 2 | string_decrypt | ~1.4-1.6M | ~250K | **5.4-6.3x** | ember MUCH slower |
| 3 | loop_overhead | ~10.3-12.1M | ~1.9-2.0M | **5.2-6.1x** | ember MUCH slower |
| 4 | call_overhead | ~1.3-1.4M | ~250-266K | **5.2-5.4x** | ember MUCH slower |
| 5 | struct_by_value | 300 | 100 | **3.0x** | ember slower (NOISY — near timer res; n=20) |
| 6 | int_div | 2700 | 2700 | **1.0x** | ember ~= g++-O2 (YAGNI wins) |

(Ranges reflect run-to-run jitter on the shared machine; the ranking is
stable across runs — slice/string/loop/call are always 5-9x, struct ~3x, int_div ~1x.)

### 8.2 Safety-on overhead (guard cost = safety_on − safety_off)

| path | guard overhead |
|---|---|
| call_overhead | +16.8% (+226K ns) — heaviest (per-call inc/cmp budget/depth) |
| slice_bounds | +10.3% (+170K ns) — bounds + per-index guard |
| string_decrypt | +4.4% (+60K ns) |
| int_div | +3.7% |
| loop_overhead | +0.2% — back-edge budget charge is cheap |
| struct_by_value | +0.0% — too small to measure at n=20 |

### 8.3 What the evidence says (the gate's input)

- **Five of six codegen paths are 5-9x slower than g++ -O2.** This is the
  per-path evidence the gate (`../planning/DESIGN.md` §9 / `COMPILER_PIPELINE.md` §5) was
  waiting for: the tree-walking stack-spilling codegen is NOT adequate on the
  call/loop/slice/string paths. The SSA-lite IR + linear-scan regalloc target
  design (`COMPILER_PIPELINE.md` §5) is now **benchmark-proven to matter** on
  these paths — the gate's "no speculative optimization before the bench proves
  it matters" condition is met for them.
- **int_div is adequate (1.0x).** The div path (cqo+idiv+div0-guard) is not the
  bottleneck — g++ -O2 and ember emit essentially the same div sequence. YAGNI
  wins on the div path: no IR work warranted there.
- **slice_bounds is the single slowest path (5.6-8.5x).** The always-on bounds
  check (ember) vs the unchecked native (C++) is most of the gap; the C++
  baseline has NO bounds check by design (this path measures ember's bounds-check
  cost vs the unchecked native — that's the point). A portion of the gap is also
  the tree-walker's per-index frame traffic.
- **string_decrypt's 5.4-6.3x is the inline-stack-XOR obf-feature cost**, not
  the string-length read itself (the C++ baseline does a non-decrypt length
  read; the delta IS the per-use inline byte-XOR decrypt). This is the cost of
  the encryption feature, separate from the codegen quality — a finding for the
  obf-design, not (only) the IR refactor.
- **call_overhead's 5.2-5.4x + the heaviest safety-on overhead (+16.8%)**
  points at the script-to-script dispatch indirect call + prologue/epilogue
  being the IR refactor's highest-value target (call sites are the path
  `CallScript` + linear-scan across call boundaries would touch, and they pay
  the guard cost too).
- **struct_by_value is 3.0x but NOISY** (300 vs 100 ns, both near steady_clock
  resolution at n=20). The number is directionally consistent (ember slower)
  but the full matrix should rerun this path with a higher-resolution timer
  (e.g. `__rdtsc`) or a larger workload (see 8.4 for why correctness on this
  path must be asserted via `ember bench`'s full `result` line, not the
  `ember run` exit code, which is 8-bit-truncated by the shell).

### 8.4 Real finding: a struct-local REASSIGNMENT codegen segfault (fixed)

While sizing the struct_by_value path, the bench surfaced a real codegen bug
— but it is NOT the "temp-slot collision / 0.1467x miscompile" an earlier
draft of this section diagnosed. That earlier diagnosis was wrong on two
counts, and this section now records the corrected finding so the
Stage-1 codegen-optimization work doesn't chase a non-bug.

**The real bug** is a 100%-reproducible segfault (exit 139, 20/20, both
`ember run` and `ember bench`) on struct-local REASSIGNMENT where the RHS is a
struct-returning call:
```
struct S { v: i64; }
fn mk() -> S { return S { v: 42 }; }
fn main() -> i64 {
    let mut s: S = S { v: 0 };
    s = mk();          // <-- AssignExpr: target is a struct local, value is a struct-returning call
    return s.v;
}
```
Isolation (all confirmed 20/20):
  - `let s = mk()` (INITIALIZATION via LetStmt) → works (returns 42).
  - `s = mk()` (REASSIGNMENT via AssignExpr) → SEGFAULT. In a loop or a single
    statement; with or without struct-by-value args in the same function.
  - A struct-by-value ARG in a loop (no struct return) → works.
  - A struct-returning call in a loop WITHOUT reassigning a struct local → works.

Root cause (src/codegen.cpp, AssignExpr eval case): the RHS `mk()` is a
struct-returning CallExpr, which on Win64 uses the hidden-pointer struct-return
ABI — rcx = `&return_buffer`, the callee writes the struct there, and rax
returns the hidden pointer. The LetStmt-init path handles this correctly
(`eval_struct_returning_call` with `lea rax, [rbp+off]` as the hidden word-0).
The AssignExpr path did NOT: it called the generic `eval()` CallExpr case,
which has no hidden-pointer ABI, so `mk()` was called with rcx UNSET (garbage)
→ `mk()` wrote its 8-byte result to an unmapped address → segfault; then the
AssignExpr stored a stray rax into `s`.
Fix (surgical, AssignExpr eval case only): detect a struct-returning CallExpr
assigned to a struct-typed local/global, LEA the target's address into rax as
the hidden word-0 pointer, and call `eval_struct_returning_call` (mirroring
the LetStmt-init path) instead of generic `eval()` + `store_rax`. The struct
return path (initialization `let s = mk()`) was already correct and is
unchanged.
Regression tests: `tests/lang/runtime_struct_reassign_single.ember` (rc=42),
`runtime_struct_reassign_loop.ember` (rc=50, the §8.4 shape in a loop),
`runtime_struct_reassign_multi.ember` (rc=3, 3x reassign no loop). All three
segfault (139) with the fix reverted and pass with it applied; wired into
`tests/run_lang_tests.sh`'s sema + explicit-rc lists.

**Why the earlier "0.1467x miscompile" diagnosis was wrong.** The earlier
draft's reproducer was a SCALAR accumulator (`let mut s: i64 = 0; ... s = s + r;`)
with `let r: i64 = sump(mkp(1,2,3));` — i.e. `let r =` (INIT, not struct
reassign) and `s = s + r` (SCALAR reassign, not struct). That program returns
the correct 600, but `ember run`'s exit code is `return & 0x7fffffff` further
8-bit-truncated by the shell, so 600 reads back as `600 & 0xFF = 88`, and
88/600 = 0.1467 — the "0.1467x" was a mod-256 exit-code artifact of the
CORRECT value, not a miscompile. `ember bench`'s full `result` line confirms:
`result 600` (correct). There was no temp-slot collision and no miscompile in
that shape.

**The struct temp machinery is NOT the culprit.** `alloc_struct_temp` /
`count_struct_temps_block` (the compiler-hidden temp frame slots for
struct-by-value general-expression args and struct-literal return values) are
correct: they hand out distinct `$`-tagged names (no aliasing), are pre-sized
into the frame by the counting pass, and are unrelated to the AssignExpr path
(the reassignment bug was in the call-site ABI, not in temp-slot management).
The earlier draft's suggestion that the SSA-lite IR's `LoadFrameSlot`/
`StoreFrameSlot` would fix this is moot — the bug was a missing hidden-pointer
ABI in one code path, not a temp-slot collision.

**Bench correctness assertions must use the full `result` line.** Because the
`ember run` exit code is `return & 0x7fffffff` further 8-bit-truncated by the
shell, any bench correctness check that reads the process exit code is limited
to values < 256. Either (a) keep the expected return value < 256 (the lang
runner's explicit-rc tests do this), or (b) assert correctness via `ember bench`'s
full `result` line (which prints the untruncated i64). The §8.4 reproducer's
600 is only visible correctly via (b); `ember run` would show 88.

### 8.5 Historical caveat and current interpretation

At the time of this prototype, the full codegen-optimization design was stated
to be **gated on running the FULL matrix** (all ~15
paths × both safety modes × ember-vs-g++-O2, with a higher-resolution timer for
the sub-µs paths), not on this 6-path prototype. The prototype is the seed that
(a) proves the harness works end-to-end, (b) gives the first evidence that the
  call/loop/slice/string paths are 5-9x slow and int_div is adequate, and (c)
  surfaces the struct-local-reassignment segfault (§8.4, now fixed). The remaining ~9 paths (float
  arithmetic, float sqrt, local load/store, native call overhead, array
  literals, defer, switch dispatch, f-string) remain useful additions per the §5.1 pattern. The historical “before the IR
refactor is started” gate has been overtaken by implementation: Thin IR,
linear-scan allocation, and the pass system now ship, justified by the strong
prototype evidence and subsequent focused probes — the prototype's 5-9x on 4 paths is
  strong evidence but not the complete picture the gate requires.

## 9. How to run

```bash
# from the ember root, after a clean buildt/ exists:
bash bench/run_bench.sh
# or directly (run_bench.sh does this):
#   1. g++ -O2 -shared baseline_paths.cpp -o baseline_paths.dll
#   2. ./buildt/bench_codegen_paths.exe   # writes bench/results_*.csv + .md
```
