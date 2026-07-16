# ember - codegen optimization design (research + composable architecture)

**Status: DESIGN IMPLEMENTED THROUGH THE THIN-IR STAGES; re-audited
2026-07-15.** This file preserves the original benchmark reasoning and staged
proposal, but its old future-tense status is superseded. The benchmark harness
ships under `bench/`; Thin IR, serialization/re-emission, linear-scan register
allocation, checked pass execution, profiles, and extension registration all
ship. `extensions/opt` currently registers **18** value-preserving passes:
`constprop`, `dce`, `simplifycfg`, `cse`, `gvn`, `licm`, `lsr`, `forward`,
`copyprop`, `instcombine`, `dse`, `bounds-elim`, `sccp`, `unroll`, `spill_elim`,
`peephole`, `branch_folding`, and `tailcall`. `extensions/obf` registers seven
seeded transforms. The remaining future boundary is a richer full-SSA IR and a
cached analysis manager; current global/loop passes compute the analyses they
need directly.

Detail doc for the design and evidence behind the performance work. Historical
line numbers and early test totals are provenance for the original experiment,
not statements about current HEAD.

**This began as a design document; the implementation status is recorded above
and in §8.**
It is explicitly framed as **gated, not speculative**: `docs/planning/DESIGN.md` §9
("no speculative optimization before the bench proves it matters") and
`docs/spec/COMPILER_PIPELINE.md` §5 ("the formal IR + regalloc refactor is the
optimization, gated on a benchmark-proven need") are the governing constraints.
The roadmap entries this doc produces (see `../ROADMAP.md`'s new
"Codegen optimization (gated on benchmark evidence)" section) each carry the
**gate** — the specific benchmark result (from t109) that triggers building them —
and the **effort estimate**. The "build first" ordering is the one the research
predicts; **t109's evidence confirms or refutes that ordering.**

## 0. What this doc is and is not

| It is | It is not |
|---|---|
| A research survey of standard LLVM passes × JIT-scripting relevance | A port of LLVM |
| A map of each technique to ember's actual tree-walker paths (with line numbers) | A spec for a shipped codegen change |
| A composable-architecture design with a recommended staged path | A mandate to build any of it now |
| A set of per-technique win *predictions* to be confirmed by the benchmark | Claimed wins (the benchmark is the truth) |
| A roadmap with explicit gates cross-referencing t109 | A scheduled milestone |

The governing principle, quoted from the existing specs so the frame is
unambiguous:

- `docs/planning/DESIGN.md` §9: *"no speculative optimization before the bench
  proves it matters."*
- `docs/planning/GAP_ANALYSIS.md` §4: *"Matching an optimizing native-JIT
  language's speed is a v2+ goal, gated on a benchmark-proven need."*
- `docs/spec/COMPILER_PIPELINE.md` §5: *"the formal IR + regalloc refactor is the
  optimization, gated on a benchmark-proven need."*

So the design must NOT be speculative. It must (a) research what's possible,
(b) map each technique to which ember codegen path it would help (so the
benchmark evidence can pick), and (c) design the COMPOSABLE ARCHITECTURE that lets
optimizations be added as passes without a monolithic rewrite. That is what the
sections below do, in order.

## 1. ember's codegen today (the baseline the design extends)

### 1.1 The shape

At the audited historical baseline, `src/codegen.cpp` (~3546 lines) was a single `CG` class with one `eval(const Expr&)`
entry point (line 1542) that is a giant `if (auto* X = dynamic_cast<const X*>(&ex))`
cascade over AST node types, emitting bytes directly into an `X64Emitter` (`src/x64_emitter.hpp`).
There is no IR: the AST *is* the IR, and lowering and emission are fused into one
walk. The value convention is **stack-spilling**: every expression evaluates its
result into `rax` (integer/bool) or `xmm0` (float) — a single accumulator — and any
intermediate that must survive evaluation of a sibling expression is pushed to the
machine stack (`push rax` / `mov [rsp], xmm0`) and reloaded after.

`docs/spec/CODEGEN_SPEC.md` §5 documents the *target* design (SSA-lite IR + linear-scan
regalloc) and is explicit that it is **not implemented** ("Not implemented in v1.0.
The shipped backend walks the AST directly and stack-spills expression
intermediates. Everything in this section is a future, benchmark-gated design").
`docs/spec/COMPILER_PIPELINE.md` §5 carries the same deferral with the IR shape
spelled out. `docs/planning/DESIGN.md` v0.2 milestone notes the "honest divergence
from the spec": codegen is a correctness-first tree-walking stack-spilling emitter,
the SSA-lite refactor is deferred until the benchmark harness exists.

### 1.2 The one optimization already shipped: hot local pinning

Important context for the architecture design: ember already hosts a *limited*
local-register optimization. `find_pin_candidate` (`src/codegen.cpp:886`) picks the
single most-referenced outer-scope scalar local in a loop body (N≥2 references,
GP-register-compatible only), and the `WhileStmt`/`ForStmt` lowering
(`src/codegen.cpp:2982`, `:3042`) pins it into `rbx` for the loop body's lifetime —
one load per iteration replaces N `[rbp+off]` reads, with a write-through to keep the
stack slot synced (`src/codegen.cpp:2062`). `eval`'s `Ident` case
(`src/codegen.cpp:1573`) and `AssignExpr`'s `Ident`-target case (`:2062`) have a fast
path reading the pin register directly. v1 pins exactly one register per loop.

This is the existing proof that the architecture can host a per-basic-block local
optimization *without* a flag-day rewrite: it was added as a localized fast path in
the tree-walker, gated on a per-loop candidate search, with the stack slot kept as
the source of truth. **The composable architecture below (§5) generalizes this
existing pattern** rather than throwing it away — that is the migration-friendly
path the staged recommendation (§5.4) exploits.

### 1.3 The prior benchmark evidence already in tree

`examples/bench_ember_vs_as.cpp` (248 lines) ships four workloads (fib(32),
tight_loop 1e8, nested_calls 1e7, mandelbrot 200×200/50). `build/v0.6_BENCHMARK_RESULTS.md`
(untracked, regenerated by ctest `bench_ember_vs_as`) shows ember is already **5–7×
faster than AngelScript** (the bytecode interpreter) on every workload:

| workload | ember ms/iter | AS ms/iter | ember/AS |
|---|---|---|---|
| fib(32) | 15.56 | 107.59 | 0.14 |
| tight_loop (1e8) | 125.04 | 724.23 | 0.17 |
| nested_calls (1e7) | 136.30 | 777.72 | 0.18 |
| mandelbrot (200×200, iters=50) | 3.69 | 6.53 | 0.56 |

This is the categorical native-vs-bytecode win (`../planning/GAP_ANALYSIS.md` §4: baseline native
beats a bytecode interpreter by 5–50×). It does **not** tell us where ember is slow
*relative to an optimizing native-JIT language* — that is the gap t109 closes. The
existing bench harness is the seed t109 extends; the design below assumes t109
produces per-codegen-path microbenchmarks (the prediction-to-validation mapping in
§6 names which path each workload stresses).

---

## 2. Research: what LLVM does for its passes, × JIT-scripting relevance

LLVM's optimization pipeline is an AOT whole-program pipeline. ember is a JIT
scripting language: it compiles once per session per function and runs hot; it is
**not** doing whole-program LTO or link-time alias analysis. The categorization
below separates "relevant to a JIT scripting language" from "LLVM-only, too heavy
or too whole-program for ember." For each relevant pass, §3 names the ember codegen
path it touches; §6 predicts the win. This section is the *research*; the *mapping*
and *win prediction* follow.

### 2.1 The standard LLVM pass categories (survey)

LLVM's `opt -O2` pipeline runs roughly 60+ passes. They cluster into:

- **Instruction selection / lowering** (`SelectionDAGISel`, `GlobalISel`): maps
  target-independent IR to target instructions, picking the cheapest legal form
  (e.g. `mul x, 1` → `x`; `shl x, 2` → `lea`; `xor x, x` → `0`; `add x, 0` → `x`).
  In LLVM this is target-specific and runs after IR opt.
- **Register allocation** (`RegAllocFast` / `RegAllocGreedy` / `RegAllocBasic`):
  maps the infinite virtual-register SSA to the finite physical file with spilling,
  live-range splitting, and coalescing. Greedy is graph-coloring-derived; linear
  scan is the JIT-relevant variant (Poletto & Sarkar; LLVM's `RegAllocFast` is
  close).
- **Constant propagation / Folding** (`InstSimplify`, `ConstantPropagate`,
  `SCCP` — Sparse Conditional Constant Propagation): replaces computations on
  constants with the constant result; SCCP additionally proves blocks unreachable
  when a branch condition folds to a constant.
- **Dead-code / dead-store elimination** (`DCE`, `DSE`): removes instructions
  whose result is never used, and stores whose value is overwritten before any
  load. `MemCpyOpt` / `DeadStoreElimination` need alias analysis.
- **Peephole optimization** (`PeepholeOptimizer`, `InstCombine`, `MachinePeephole`):
  local pattern rewrites on a small window — strength reduction (`mul x, pow2` →
  `shl`), `cmp; sete; movzx` collapsing, redundant `mov` elimination, branch-range
  shrinking (rel32 → rel8), `xor r,r` zeroing instead of `mov r,0`.
- **Common-subexpression elimination** (`EarlyCSE` with memory SSA, `GVN` — global
  value numbering): detects `x op y` recomputed and reuses the first result.
  EarlyCSE is basic-block-local (cheap, JIT-relevant); GVN is global (needs
  dom-tree, alias analysis, heavier).
- **Branch folding / simplification** (`SimplifyCFG`, `BranchFolding`,
  `ConstantHoisting`): merges empty blocks, removes unconditional branches to the
  next block, folds `if (const)`, eliminates dead blocks, hoists constants.
- **Tail-call optimization** (`TailCallElimination` + the target's tail-call
  lowering): turns a `return f(args)` into a `jmp f` (reuses the caller's frame,
  no stack growth). LLVM's `musttail`/`tail` markers drive target lowering.
- **Inlining** (`AlwaysInliner`, `InlineCost`/`Inliner`): replaces a call site
  with the callee body, eliminating call overhead and exposing cross-function
  optimization. Cost-model driven (code-size vs. call-frequency).
- **Loop optimizations** (`LoopSimplifyCFG`, `LICM` — loop-invariant code motion,
  `LoopUnroll`, `LoopRotate`, `IndVarSimplify`, `LoopDeletion`): rotate/unroll,
  hoist invariants, canonicalize induction variables, delete dead loops.
  `LICM` and `IndVarSimplify` are the JIT-relevant ones (cheap, big on tight
  loops); full unroll needs a trip-count.
- **Global / whole-program** (`GlobalOpt`, `GlobalDCE`, `IPSCCP` — interprocedural
  SCCP, `FunctionAttrs`, `Devirt`): cross-function constant prop, dead fn
  elimination, function attribute inference (readnone/readonly), devirtualization.
  These are the **AOT/whole-program passes**; they are the least JIT-relevant.

### 2.2 Categorization: JIT-scripting relevance

ember compiles one function at a time (the dispatch-table-per-slot model,
`CODEGEN_SPEC.md` §7), runs hot, has no LTO, and has whole-program-size budget far
below a C++ translation unit. The relevance cut:

**Relevant (a JIT scripting language measurably benefits):**

1. **Instruction selection** (cheapest legal form) — ember's tree-walker emits
   fixed x86 sequences (e.g. `mov rax, imm64` for every `IntLit` even when `imm32`
   sign-extended fits; the `BinExpr` integer path always does `push rax; eval rhs;
   mov rcx,rax; pop rax`). A selector picks cheaper forms. **Relevant.**
2. **Register allocation** (linear scan, the JIT variant) — the single biggest
   documented gap; `COMPILER_PIPELINE.md` §5 / `CODEGEN_SPEC.md` §5 name it as the
   deferred target. ember's stack-spill convention (every intermediate through
   `rax`/memory) is the baseline this replaces. **Relevant (named in spec).**
3. **Constant propagation / folding** — sema already folds `IntLit op IntLit`
   for `Add/Sub/Mul/And/Or/Xor/Shl/Shr` (`src/codegen.cpp:1700`-ish, the
   `li`/`ri` fold block; see CODEGEN_SPEC.md §10.1). Div/Mod and comparisons are
   excluded. Float literals are not folded. **Partially done; rest relevant.**
4. **Dead-code / dead-store elimination** — the tree-walker emits every statement
   in source order; an unreferenced `let`'s initializer store is dead, and a
   `let` overwritten before any read is a dead store. **Relevant (cheap, local).**
5. **Peephole optimization** — the div-by-zero guard (`emit_integer_divmod`,
   `src/codegen.cpp:932`), the bounds check (`emit_bounds_check_*`, `:304`),
   the call-target-provenance guard (`emit_call_target_guard`, `:497`) are
   per-site byte sequences. Redundant-guard peepholes (a div-by-constant-nonzero
   already skips the zero-check, per `CODEGEN_SPEC.md` §10; a bounds check after
   an already-checked index) and branch-range shrinking (rel32 → rel8,
   `CODEGEN_SPEC.md` §4 explicitly deferred) are peephole wins. **Relevant.**
6. **Common-subexpression elimination** (basic-block-local, the `EarlyCSE` shape)
   — `a[i]` referenced twice in one expression recomputes the element address
   (load base, eval index, mul width, add) both times; the existing pinning is
   the loop-scope version of this. Block-local CSE is the expression-scope version.
   **Relevant (and the pinning is the existing partial impl).**
7. **Branch folding / simplification** — the `BoolLit`/constant-folded `if` cond,
   empty `else` blocks, and `switch` with a single case all emit real branches
   today. Folding `if (true)`/`if (false)` at the AST level is cheap and removes
   dead blocks. **Relevant (cheap).**
8. **Tail-call optimization** — ember recursion hits the budget/depth guard
   (`SAFETY_AND_SANDBOX.md` §3-§4); `fib(n-1)+fib(n-2)` style is the canonical
   TCO win. ember's `return f(args)` shape is detectable at the AST level.
   **Relevant (and the budget guard interacts favorably — TCO removes the
   stack-depth charge on the tail call).**
9. **Inlining** (script-to-script, small leaf fns) — the `nested_calls` benchmark
   workload (`a→b→c` chain) is the inlining stress test. ember's calls go through
   the dispatch table (`CODEGEN_SPEC.md` §7: `mov r11,[table+slot*8]; call r11`),
   so a non-inlined call is ~one load + one indirect call per level. Inlining a
   small leaf removes that and exposes the callee's body to the caller's
   regalloc/CSE. **Relevant (but interacts with hot reload — see §4.9).**
10. **LICM** (loop-invariant code motion) — the `mandelbrot` inner loop recomputes
    `cx`/`cy` per pixel; an outer hoist moves loop-invariant exprs out of the loop.
    **Relevant (the pinning is the load-form of this for one variable).**

**LLVM-only / too heavy / too whole-program (not relevant to a JIT scripting
language at v1–v2 scale):**

- **GVN** (needs dom-tree + alias analysis; the `EarlyCSE` block-local form is the
  JIT-relevant subset — GVN itself is the whole-function version).
- **Full loop unroll** (needs a trip-count; partial unroll pays only on hot loops a
  profile would identify — ember has no profile).
- **IPSCCP / GlobalOpt / FunctionAttrs / Devirt** (interprocedural, whole-program;
  ember compiles one fn at a time with no LTO).
- **Machine-level passes** that need a post-RA view (`MachineLICM`,
  `MachineSink`, `VirtRegRewriter` coalescing) — these are inside LLVM's
  `RegAllocGreedy`; the linear-scan JIT variant folds them.
- `MemCpyOpt` / full `DSE` (need alias analysis across memory ops; ember has no
  aliasing raw pointers in script, so the simple block-local DSE is enough — the
  full LLVM passes are overkill).

### 2.3 The shortlist this doc carries forward

The ten relevant passes above become the ten roadmap entries in §6 (each with an
ember-path mapping, a win prediction, a gate, and an effort estimate). The
LLVM-only set is recorded here so a future reader doesn't ask "why not GVN / full
unroll / IPSCCP" — the answer is the JIT-scripting relevance cut, not oversight.

---

## 3. Mapping techniques to ember's actual codegen paths

This is the "where is ember slow" prediction that the benchmark (t109) validates.
For the current stack-spilling convention, the specific waste is named with line
numbers (all in `src/codegen.cpp` at HEAD `8062195`).

### 3.1 BinExpr integer path — the canonical spill

`src/codegen.cpp:1747`-ish (the integer `BinExpr` case, after the fold early-return
and the logical/float dispatch):

```
eval(*b->lhs);
e.push(Reg::rax);          // <-- spill lhs to stack
eval(*b->rhs);
e.mov_reg_reg(Reg::rcx, Reg::rax);  // rcx = rhs
e.pop(Reg::rax);                    // <-- reload lhs
<the op: add/sub/imul/cmp/...>
```

**The waste:** every integer binary expression with a non-trivial RHS pushes lhs to
memory and pops it back, even when lhs could stay in a register (e.g. `rdx` or
`r8`) across the RHS eval. For `a + b * c` the outer `+` spills `a`, evaluates
`b*c` (which itself spills `b`), then reloads `a`. This is the single most-executed
spill in the language — every arithmetic expression pays it.

The float path (`:1759`, the `is_float` branch) does the same with `sub rsp,8;
movsd [rsp],xmm0; ...; movsd xmm1,[rsp]; add rsp,8` — one stack slot instead of
`push`, but the same memory round-trip.

**Technique that fixes it:** register allocation (§3.x → §6 entry R1) — keep lhs in
a register across the RHS eval. The simplest win is a per-expression local
allocator that picks a second scratch register for lhs; the full win is the
linear-scan over the SSA-lite IR.

### 3.2 CallExpr arg stash — the second-biggest spill

`src/codegen.cpp:2224`-on (the `CallExpr` case):

```
e.sub_reg_imm32(Reg::rsp, total);   // reserve stash + shadow + outgoing
for each operand:
    eval(*op.e);                     // -> rax (or xmm0, or rax/rdx for slice)
    mov [rsp+off], rax  /  movsd [rsp+off], xmm0   // <-- stash every arg to memory
// then reload words 0..3 into rcx/rdx/r8/r9 or xmm0..3:
for w in 0..3: load_reg_mem(int_regs[w], rsp, off)  // <-- reload from stash
```

**The waste:** every call evaluates each arg into `rax`, stores it to a stack slot,
then *reloads* it from that slot into the argument register (`rcx`/`rdx`/`r8`/`r9` /
`xmm0..3`). For a 4-arg call that's 4 stores + 4 reloads that a register allocator
would skip — eval the arg directly into the target register when the arg expression
permits. The indirection exists for correctness: args must survive nested calls
(`ackermann(m-1, ackermann(m,n-1))` needs a fresh stash region per call, the comment
at `:2224` explains), and `rax` is the single accumulator the tree-walker can't move
off of. This is the workload `nested_calls` and `fib` stress.

**Technique:** register allocation (R1) — assign args to their ABI registers as
their eval target, spilling only when an arg's eval clobbers a later arg's register
(the linear-scan's "crosses-a-call" bias, `CODEGEN_SPEC.md` §5 step 6, is exactly
this). For the leaf common case (no nested call in an arg), direct-to-register
eliminates the stash entirely.

### 3.3 The div-by-zero / overflow / bounds / call-target guards — per-site byte cost

- `emit_integer_divmod` (`src/codegen.cpp:932`): every integer `/`/`%` with a
  runtime divisor emits `test divisor; je .divzero_trap` + the `INT64_MIN/-1`
  overflow check + `cqo; idiv`. `CODEGEN_SPEC.md` §10 says a literal nonzero
  divisor already skips the zero-check; a *runtime* divisor that sema could prove
  nonzero (e.g. `x / 2` where `2` is a constant) — already handled. The residual
  cost is the `INT64_MIN/-1` two-`cmp` sequence on every signed div with a runtime
  divisor.
- `emit_bounds_check_reg`/`imm` (`:304`): every slice index and every
  non-constant-index fixed-array access emits `cmp; jae .oob_trap`. A
  *loop-invariant* index (`for i in 0..N: arr[i]`) re-checks `i < len` every
  iteration when `i`'s range already proved it. (Sema elides the check for a
  *compile-time-constant* index in range; it does not elide it for a
  loop-induction variable whose range is provably `< len`.)
- `emit_call_target_guard` (`:497`): the Tier 2 indirect-call provenance guard
  (range + `bt` bit test) runs on every `handle(args)`. A handle that sema proved
  to be a constant `&fn` literal (the `FnHandleExpr` baked slot) is still re-checked
  at runtime. (Sema bakes the slot as a literal; the guard runs on the runtime
  value regardless.)

**The waste:** these are correctness guards and must stay; the *redundant* ones are
the win. A peephole that proves a guard's predicate already holds at a site (a div
by a constant nonzero; a bounds check after an induction-variable-range proof; a
call-target guard on a constant handle) trims the per-site bytes. These are
peephole + a small amount of sema-level range propagation.

**Technique:** peephole + limited range propagation (P1, R2-provenance).

### 3.4 The struct/array/string temp copies — aggregate materialization

- Struct-by-value arg temp: `stash_struct_arg` (`src/codegen.cpp:1269`) materializes
  a `StructLit` or struct-returning `CallExpr` arg into a fresh `__tmp$N` frame
  slot, then `copy_bytes` into the arg-stash slot. `CODEGEN_SPEC.md` §16.2.
- Array literal: `__arrtmp$N` backing temp + per-element `store_value_to_memory`
  (`:2675`, §16.3).
- String literal (encrypted): the inline-XOR loop materializes the plaintext into a
  `__strtmp` slot at *every use site* (`:2367`, `alloc_str_temp`). A literal used in
  a loop re-XORs every iteration.

**The waste:** the per-use-site materialization is the design (transient plaintext,
no heap), and it's correct. The *redundant* cost is: (a) a string literal used
textually twice already gets two `StringLit` nodes and two slots (the comment at
`:2388` notes this — "mirrors how struct temps are allocated one-per-site"); (b) a
string literal used in a loop re-XORs every iteration when the plaintext could be
materialized once before the loop and reused (LICM for the str temp); (c) struct
temps are over-reserved as the *sum* of all temp bytes across the fn
(`CODEGEN_SPEC.md` §16.1 "a future optimization could compute
max-simultaneously-live instead").

**Technique:** LICM for the loop-invariant string/struct temp (R-loop); a
max-simultaneously-live temp-sizing prescan (cheap, local, no IR needed).

### 3.5 The `IntLit` → `mov rax, imm64` — instruction selection

`src/codegen.cpp:1547` (`IntLit` case): `e.mov_reg_imm64(Reg::rax, lit->v)` always
emits the 10-byte `mov rax, imm64` form. The encoder's "shortest correct encoding"
rule (`CODEGEN_SPEC.md` §3) exists at the emitter level for *some* instructions but
the `IntLit` path does not route through the smart-imm selector for `rax` — it takes
the full imm64 form. A literal that fits signed-32 (`mov rax, imm32` via `C7`,
6 bytes, sign-extended) or unsigned-32 (`mov eax, imm32`, 5 bytes, zero-extends)
would be shorter and faster (icache).

**Technique:** instruction selection (I1) — a smart-imm pass on the emitted buffer,
or (better) the `IntLit` case picking the shortest correct form at emit time.

### 3.6 The `setcc; movzx` pair — peephole

Comparison results (`:1790`-ish, the `Eq..Ge` case in `BinExpr`): `cmp; setcc al;
movzx rax, al`. The `movzx rax, al` is necessary because `setcc` writes only the low
byte. But `xor rax,rax; setcc al` (zero the reg first, then set the byte) is one
byte shorter and avoids the false-dependency on `rax`'s upper bytes that `movzx`
also breaks — a peephole-level rewrite. (This is a micro-win; recorded for
completeness, not a headline.)

### 3.7 Summary table — ember path → waste → technique

| # | ember path (src/codegen.cpp line) | The waste (stack-spill convention) | Technique (§6 entry) |
|---|---|---|---|
| W1 | `BinExpr` int `:1747` (`push rax; eval rhs; pop rax`) | lhs spills to stack across every RHS eval | R1 regalloc (linear scan) |
| W2 | `BinExpr` float `:1759` (`sub rsp,8; movsd [rsp]; ...; movsd xmm1,[rsp]`) | same, float form | R1 |
| W3 | `CallExpr` arg stash `:2224` (eval→rax; store [rsp]; reload to ABI reg) | every arg does a store+reload | R1 (direct-to-ABI-reg) |
| W4 | `IntLit` `:1547` (`mov rax, imm64`, always 10 bytes) | never uses the 5/6-byte imm32 form | I1 instruction selection |
| W5 | `emit_integer_divmod` `:932` (`INT64_MIN/-1` check on every signed div) | the overflow check runs even when divisor is a constant != -1 (sema handles constant-nonzero-zero-check; not the -1 case) | P1 peephole / sema fold |
| W6 | `emit_bounds_check_*` `:304` (loop-invariant index re-checked per iter) | a loop-induction index with provable range re-checks `< len` every iter | R2 range propagation + peephole |
| W7 | `emit_call_target_guard` `:497` (constant `&fn` handle re-checked) | a handle baked as a constant slot is re-validated at runtime | P1 peephole / sema fold |
| W8 | `StringLit` inline-XOR `:2367` (re-XORs in a loop) | loop-invariant plaintext re-materialized per iter | LICM (R-loop) |
| W9 | Struct/array temp over-reserve (`alloc_struct_temp`, count-as-sum) | frame sized as sum of all temps, not max-live | local temp-sizing prescan |
| W10 | `setcc; movzx` `:1790` (cmp-result) | 1-byte-longer than `xor; setcc`, false-dep on rax | P1 peephole (micro) |
| W11 | empty `else` / constant-folded `if` (`IfStmt` lowering) | dead branch emitted | B1 branch folding |
| W12 | `return f(args)` (tail position, fib-style) | grows the stack frame + charges depth on the tail call | T1 TCO |
| W13 | `nested_calls` `a→b→c` chain | one indirect dispatch-table call per level | N1 inlining (leaf fns) |
| W14 | `mandelbrot` inner loop recomputes `cx`/`cy` per pixel | loop-invariant exprs re-evaluated per iter | LICM (R-loop) |

---

## 4. The composable / extensible codegen architecture design

The current codegen is a monolithic tree-walker: one `eval()` with a giant
if-else over AST node types, emitting directly to the `X64Emitter`. A composable
design lets optimization passes be added independently. This section evaluates the
realistic options for a JIT scripting language (NOT a full LLVM pipeline — that's
too heavy) and recommends a **staged path**.

### 4.1 Option (a): the SSA-lite IR + linear-scan regalloc (the documented target)

`COMPILER_PIPELINE.md` §5 and `CODEGEN_SPEC.md` §5 already specify this:
`AST → SSA-lite IR (basic blocks, single-assignment IrValue, no phi — slot-backed
merge vars) → (optimization passes over IR) → run_linear_scan → emit_x64`.
`COMPILER_PIPELINE.md` §8 sketches the three-stage split:
`lower → run_linear_scan → emit_x64`, "each stage has a narrow, checkable contract."

**Composable-pass fit: excellent.** This is the architecture LLVM uses; passes plug
in as IR→IR transforms over a stable `IrFunction`. The `IrInstr::Op` enum
(`COMPILER_PIPELINE.md` §5) is the pass interface's vocabulary; a pass is a function
`IrFunction transform(IrFunction)`. Constant folding, DCE, peephole, CSE,
branch folding, LICM, and inlining all become IR passes; regalloc is the
IR→phys-assignment pass; emit is the final IR→bytes pass.

**Migration cost: substantial.** The tree-walker is ~3000 lines of fused
lower+emit. Building the IR layer (lower), the pass manager, the linear-scan
allocator, and the new emitter is a near-total codegen rewrite — the spec calls it
"the formal IR + regalloc refactor is the optimization." It is the *target*, but it
is a flag-day-class change unless staged. The spec's own "testable stages rather
than one monolithic pass" note (`COMPILER_PIPELINE.md` §8) is the staging principle.

**When it pays:** when (b) (below) is insufficient — i.e. when the benchmark shows
the per-expression/per-block local win is not enough and cross-block liveness /
global CSE / real register pressure across a whole function is the bottleneck. t109
decides this.

### 4.2 Option (b): a lighter peephole + per-basic-block local regalloc layer over the tree-walker

Keep the tree-walker. Add two post-emit passes over the emitted byte buffer:

1. **A peephole pass** over the `vector<uint8_t>` the `X64Emitter` produces: a
   pattern table of (byte-sequence → cheaper-byte-sequence) rewrites. Covers W4
   (`mov rax,imm64` → `mov rax,imm32`/`mov eax,imm32`), W10 (`setcc;movzx` →
   `xor;setcc`), W5/W7 (redundant-guard removal where a peephole can prove the
   predicate), and branch-range shrinking (rel32→rel8, `CODEGEN_SPEC.md` §4's
   explicit deferral). The peephole operates on bytes + the fixup table (it needs
   the label-offset map to shrink branches).
2. **A per-basic-block local register allocator**: identify basic blocks in the
   emitted bytes (labels that are branch targets start a block; the block ends at
   the next branch/label), and within a block, keep values in registers instead of
   the stack-spill. This is the *generalization* of the existing hot-local pinning
   (§1.2): pinning is "one register, one loop-body block, candidate-search-gated";
   the local allocator is "N registers, every basic block, liveness-driven." The
   pinning code is the prototype that already proved the tree-walker can host this.

**Composable-pass fit: good but coarser.** Passes plug in as byte-buffer→byte-buffer
transforms, not IR→IR. The pass interface is simpler (a `vector<uint8_t>` + a
fixup table in, same out), but the vocabulary is "x86 bytes," not "IR ops" — a
peephole that wants to do CSE has to recognize the load/add/cmp byte patterns,
which is more brittle than matching `IrInstr::Op::Add`. Local regalloc over bytes
is hard (you'd have to recover liveness from the bytes); the realistic form is
*local regalloc over the AST within a basic block*, emitted with a small local
register pool, spilling at block boundaries — i.e. the tree-walker gets a
per-block local-allocator mode, not a post-emit pass.

**Migration cost: low.** The peephole is a new pass over the existing output, no
tree-walker rewrite. The local regalloc is a generalization of the existing pinning
(a per-block candidate search + a small register pool instead of one register),
additive in the tree-walker. Both ship without touching the existing emit logic.

**When it pays:** when the wins are dominated by per-expression/per-block spills
(W1, W2, W3) and per-site byte cost (W4–W10) — i.e. when the benchmark shows the
hot spots are local, not cross-block. This is the *expected* shape for small
game-script functions (the `CODEGEN_SPEC.md` §5 assumption: "register pressure in
small game-script functions is expected to be low enough that spilling is rare").

### 4.3 Option (c): a hybrid — a thin three-address IR + peephole + local regalloc, deferring full SSA/linear-scan

`AST → thin IR (three-address form, basic blocks, but no SSA renaming, no phi) →
(peephole + local regalloc passes over the thin IR) → emit x86`. The thin IR is
"closer to three-address than full SSA": each instruction is `dst = op src1 src2`,
`dst` is a virtual register, but virtual registers are *not* single-assignment
(they can be reassigned, like a naive three-address TAC). This is cheaper to build
than full SSA-lite (no dominance frontier, no phi insertion, no rename) but gives
passes a stable op vocabulary (not x86 bytes) — so CSE, DCE, constant prop, and
peephole are IR passes (matching on op, not bytes), which is far less brittle than
the byte-peephole of (b).

**Composable-pass fit: very good.** The thin IR is the pass interface; passes are
IR→IR transforms. Local regalloc is IR→phys-assignment within a block. The full
SSA-lite + linear-scan (option a) is a *later* upgrade of the thin IR: add SSA
renaming + phi/slot-backing + a real linear-scan liveness pass — the thin IR's
instruction set is a subset of the SSA-lite IR's, so the upgrade is additive, not a
rewrite. This is the staged path the spec's "testable stages" note points at.

**Migration cost: moderate.** Lowering from AST to the thin IR is a real change
(the tree-walker's `eval` becomes `lower_expr → thin IR`), but the thin IR is much
smaller than the SSA-lite IR (no phi, no dominance, no full liveness). The emit
pass is a new `thin IR → x86` walker, simpler than the current fused tree-walker
because it operates on three-address form (no recursive eval). The existing
pinning and the stack-spill logic become the local regalloc's spill path.

**When it pays:** when (b)'s byte-peephole proves too brittle (a real risk — x86
byte-pattern matching is fragile) but the full SSA-lite (a) is not yet justified.
This is the *middle* of the staged path.

### 4.4 Recommendation: a staged path, gated by the benchmark

The recommendation is **(b) now, (c) if (b)'s peephole proves brittle and the
benchmark shows cross-block wins, (a) if (c) is insufficient** — explicitly staged,
each stage gated on t109's evidence, matching the spec's "testable stages rather
than one monolithic pass" principle.

**Stage 1 — (b): peephole + per-basic-block local regalloc over the tree-walker.**
- **SHIPPED (2026-07-10).** Ships as additive passes: a `peephole(vector<uint8_t>&, Fixups&)`
  over the emitted buffer (`src/peephole.{hpp,cpp}` — `SmartImmPass` W4 + the
  inert `SetccMovzxPass` W10 placeholder), and a per-expression local-allocator
  mode in the tree-walker's BinExpr integer path (a volatile r10 holding
  register gated on an `expr_clobbers_r10` check, generalizing the existing
  pinning from "one register, one loop" to "a second volatile accumulator,
  every r10-safe integer BinExpr"). Behind flags (`CodeGenCtx::enable_peephole` /
  `enable_local_regalloc`, default off → byte-identical to today; the 26/26 ctest
  gate + 268/0/0 lang gate hold with flags off AND on). See §8 for the measured
  before/after bench numbers (call_overhead -14%, loop_overhead -15%
  regalloc-only, slice_bounds +8% regression — the mixed result that motivates
  Stage 2's cost-model regalloc).
- **No tree-walker rewrite.** The existing emit logic stays; the peephole runs after;
  the local allocator is a mode of the existing `eval` that allocates into a
  volatile holding register per BinExpr instead of always `rax`+push.
- Gates: t109 shows W1/W2/W3 (per-expression spills) and W4–W10 (per-site bytes)
  are the dominant cost on the hot workloads (CONFIRMED — the bench proved the
  5-9x call/loop/slice/string slowdowns; int_div 1.00x YAGNI vindicated).
- Effort: ~2–4 weeks (peephole table + local allocator generalization of pinning).

**Stage 2 — (c): thin three-address IR, if Stage 1's peephole is brittle or the
benchmark shows cross-block wins (CSE across blocks, loop-invariant hoist that
needs a real CFG, register pressure that spills across blocks).**
- AST → thin IR lowering replaces the tree-walker's recursive `eval`; the peephole
  and local regalloc become IR passes (matching on `ThinOp`, not bytes — far less
  brittle).
- The Stage-1 peephole table carries over as IR-pattern rewrites; the Stage-1 local
  allocator carries over as a per-block IR regalloc.
- Gates: t109 shows cross-block liveness/CSE/LICM is the bottleneck (the
  `mandelbrot` loop-invariant recomputation, W8/W14), OR Stage 1's byte-peephole
  maintenance cost is high (the brittleness risk).
- Effort: ~4–8 weeks (lowering + thin IR + pass manager + emit walker).

**Stage 3 — (a): full SSA-lite IR + linear-scan, if Stage 2 is insufficient.**
- Upgrade the thin IR to SSA-lite: add SSA renaming + slot-backed merge vars (no
  phi, per `COMPILER_PIPELINE.md` §5) + the real linear-scan liveness pass
  (`CODEGEN_SPEC.md` §5). This is the documented target.
- The thin IR's instruction set is a subset of the SSA-lite IR's, so this is
  additive (a rename pass + a phi-to-slot pass + a liveness pass + the linear-scan
  allocator), not a rewrite.
- Gates: t109 shows register pressure across a whole function is the bottleneck
  (large fns with many simultaneously-live values), OR cross-block CSE/LICM that
  needs real SSA liveness (not the thin IR's per-block view).
- Effort: ~6–12 weeks (SSA rename + liveness + linear-scan + the pass upgrades).

**Why this staging and not "just build (a)":** (a) is the spec's documented target
but it is the most invasive (a near-total codegen rewrite). The spec's own §8 note
("testable stages rather than one monolithic pass") and §9 ("no speculative
optimization before the bench proves it matters") mandate staging. (b) is the
least invasive and gets the per-expression/per-block wins the research predicts are
dominant (W1–W3) at a fraction of the cost — and it *generalizes an optimization
ember already ships* (pinning). If (b) is enough, (a) was never needed. If (b) is
not enough, (c) gives the IR vocabulary that makes passes non-brittle, and (a) is
then an additive upgrade of (c). The benchmark gates each transition.

### 4.5 The pass interface (Stage 1 shape, the recommended starting point)

```cpp
// A pass operates on the emitted byte buffer + the fixup table.
// Stage 1: two concrete passes; the interface is extensible.
struct PeepholeCtx {
    std::vector<uint8_t>& bytes;
    const std::unordered_map<uint32_t /*label_id*/, uint32_t /*offset*/>& resolved_labels;
    std::vector<PendingFixup>& fixups;   // may be rewritten (rel32→rel8 shrink)
    // ... the AbsFixup table (RIP-relative data, dispatch base, globals base)
};

struct PeepholePass {
    virtual ~PeepholePass() = default;
    virtual const char* name() const = 0;
    // Returns true iff any bytes were rewritten. Idempotent: a second run is a no-op.
    virtual bool run(PeepholeCtx& ctx) = 0;
};

// The manager, additive:
struct PeepholePipeline {
    std::vector<std::unique_ptr<PeepholePass>> passes;
    void run_all(PeepholeCtx& ctx) {
        for (auto& p : passes) {
            bool changed = true;
            while (changed) { changed = p->run(ctx); }  // fixed point per pass
        }
    }
    void add(std::unique_ptr<PeepholePass> p) { passes.push_back(std::move(p)); }
};
```

The historical Stage-1 design proposed two byte passes. The shipped Stage-1
pipeline contains `SmartImmPass` and an inert `SetccMovzxPass`; it does **not**
claim a shipped `BranchShrinkPass`. The later Thin-IR pass set supplies local
peephole and branch-folding transforms without rel8 branch shrinking. Historically proposed: `SmartImmPass` (W4) and `BranchShrinkPass` (rel32→rel8,
the `CODEGEN_SPEC.md` §4 deferral). The redundancy-removal peepholes (W5, W6, W7) are
a later pass added when the benchmark shows the guard cost; they need a small
amount of predicate-proof info from sema (range propagation for W6, constant-handle
for W7), threaded as a side-table the peephole reads. The local regalloc
(W1–W3) is *not* a post-emit pass — it's a mode of the tree-walker (§4.2), and its
interface is the per-block register-pool API the existing pinning generalizes.

Stage 2's interface upgrades this to IR→IR passes (`ThinPass` over `ThinFunction`),
and Stage 3 to SSA-lite passes (`IrPass` over `IrFunction`, the `COMPILER_PIPELINE.md`
§8 shape). The pass-manager shape (`run_all` to fixed point, additive `add`) is the
same across all three stages — only the payload type changes. This is the
composable architecture: the *manager* is stable, the *payload* upgrades with the
IR maturity.

### 4.6 The representation each stage operates on

- **Stage 1 (b):** the emitted `vector<uint8_t>` + the fixup tables (the
  `X64Emitter`'s `PendingFixup` / `RipFixup` / `AbsFixup`, `CODEGEN_SPEC.md` §4).
  The tree-walker's AST is the lowering; the byte buffer is the opt target.
- **Stage 2 (c):** the thin three-address IR (`ThinFunction` = list of
  `ThinBlock` = list of `ThinInstr { ThinOp op; VReg dst; VReg src1; VReg src2; }`,
  with a `VReg` that is *not* single-assignment). The AST → `ThinFunction` lower
  replaces `eval`; the `ThinFunction → bytes` emit replaces the fused emit.
- **Stage 3 (a):** the SSA-lite IR (`IrFunction` per `COMPILER_PIPELINE.md` §5:
  single-assignment `IrValue`, `BasicBlock` with terminators, slot-backed merge
  vars, no phi). The thin IR's `VReg` becomes `IrValue` (single-assignment); the
  rename + slot-backing passes upgrade `ThinFunction → IrFunction`. `run_linear_scan`
  and `emit_x64` per `COMPILER_PIPELINE.md` §8.

### 4.7 Migration without a flag-day rewrite (testable stages)

The staging above is the migration plan. Each stage is independently testable:

- **Stage 1 ships behind a flag** (`CodeGenCtx::enable_peephole`,
  `enable_local_regalloc`, default off → byte-identical to today, the 26/26 ctest
  gate + 268/0/0 lang gate hold). **SHIPPED 2026-07-10.** Enabled per-function
  for benchmark comparison (the bench harness toggles via the `EMBER_STAGE1_OPTS`
  env var: `peephole`/`regalloc`/`both`). The existing `ember_check`/`sema_check`/
  `binding_abi_test`/`win64_abi_test` suite pins correctness (all pass with flags
  on — the optimizations are correctness-preserving); a new `codegen_opt_test`
  (ctest target `codegen_opt`) pins each peephole rewrite's + the regalloc's value
  equivalence (the rewritten sequence computes the same value — design §4.7's
  correctness pin).
- **Stage 2 is gated on Stage 1's brittleness or cross-block evidence.** It
  replaces the tree-walker's `eval` with `lower_expr` + a `ThinFunction → bytes`
  emit. The Stage 1 peephole table carries over as `ThinPass`es. The 22/22 gate
  must hold after the lowering rewrite; a `thin_ir_test` pins the lowering per
  AST node.
- **Stage 3 is gated on Stage 2's insufficiency.** It adds the SSA rename + the
  linear-scan. The `CODEGEN_SPEC.md` §5 acceptance criteria (spill-heavy test →
  `RegAllocResult` correctness; encoding test → `emit_x64` byte-exact) become the
  new test surface, exactly as §8 describes.

Each stage's gate is a t109 benchmark result, not a schedule. The roadmap entries
(§6) carry the gate per entry.

---

## 5. Per-technique win prediction mapped to ember paths

This is the prediction the benchmark (t109) confirms or refutes. The top-3
predictions are the headline; the rest are ordered by predicted win (the "build
first" ordering the research suggests; t109 reorders if the evidence says so).

### Top-3 predicted wins (the headline)

1. **Register allocation (R1)** fixes **W1/W2/W3** — the `BinExpr` per-expression
   spill (`push rax; eval rhs; pop rax`) and the `CallExpr` arg stash
   (eval→store→reload). These are the two most-executed spills in the language
   (every arithmetic expression, every call). **Predicted biggest win.** The
   existing pinning is the proof-of-concept that this is buildable additively.
   Gate: t109 shows `tight_loop`, `nested_calls`, `fib` are spill-bound (the
   per-expression store/reload dominates the cycle count).

2. **LICM / loop-invariant code motion (R-loop)** fixes **W8/W14** — the
   `mandelbrot` inner loop recomputing `cx`/`cy` per pixel, and the encrypted
   `StringLit` re-XORing per iteration. **Predicted second-biggest win**, because
   the hot game-logic shape is a tight loop with loop-invariant setup, and the
   tree-walker re-evaluates the invariant every iteration. The existing pinning is
   the load-form of this for one variable; LICM is the expression-form.
   Gate: t109 shows `mandelbrot` and any loop-with-invariant-expr workload is
   dominated by the re-evaluation, not the loop body's arithmetic.

3. **Peephole + instruction selection (P1/I1)** fixes **W4/W5/W6/W7/W10** — the
   `mov rax,imm64`-always-10-bytes, the redundant div-overflow check, the
   re-checked loop-invariant bounds index, the re-checked constant handle, the
   `setcc;movzx` pair. **Predicted third-biggest win** — these are per-site byte
   savings, individually small but cumulative across a hot loop, and the cheapest
   to build (a peephole table, no IR).
   Gate: t109 shows per-site byte cost (icache/code-size) is a measurable fraction
   of the hot-loop cycle count (the `tight_loop` workload is the one that would
   show this — a tight straight-line loop where every byte counts).

### The full ordered prediction (research ordering; t109 reorders)

| Rank | Technique | ember paths fixed | Gate (t109 result) | Effort (stage) |
|---|---|---|---|---|
| 1 | **R1** Register allocation (per-block local first, linear-scan later) | W1, W2, W3 | `tight_loop`/`nested_calls`/`fib` spill-bound | Stage 1 (local) → 3 (linear-scan) |
| 2 | **R-loop** LICM (loop-invariant code motion) | W8, W14 | `mandelbrot` invariant-dominated | Stage 1 (expr hoist in tree-walker) → 2 (IR pass) |
| 3 | **P1** Peephole (redundant-guard + `setcc;movzx`) | W5, W6, W7, W10 | per-site byte cost measurable on `tight_loop` | Stage 1 (byte peephole) → 2 (IR pass) |
| 4 | **I1** Instruction selection (smart-imm) | W4 | `IntLit`-heavy hot loop shows imm-size cost | Stage 1 (byte peephole) |
| 5 | **N1** Inlining (small leaf script fns) | W13 | `nested_calls` dispatch-bound | Stage 2 (IR pass; hot-reload interaction — §4.9) |
| 6 | **T1** Tail-call optimization | W12 | `fib`-style recursion depth/stack-bound | Stage 2 (AST-level detect → IR pass) |
| 7 | **C1** Constant prop/folding (the missing folds) | sema-done: int Add/..Shr; missing: Div/Mod, float, comparisons | constant-heavy workload shows unfolded cost | Stage 2 (IR pass); partly sema |
| 8 | **D1** Dead-code/dead-store elimination | unreferenced `let` init, overwritten-before-read | dead-code-heavy synthetic workload | Stage 2 (IR pass) |
| 9 | **B1** Branch folding/simplification | W11 (empty `else`, constant `if`), single-case `switch` | branch-heavy workload | Stage 2 (IR pass; partly AST-level) |
| 10 | **CSE1** Block-local CSE | W: `a[i]` recomputed in one expression | CSE-heavy synthetic workload | Stage 2 (IR pass); pinning is the partial impl |
| 11 | **R2** Range propagation (bounds-check elision for induction vars) | W6 (loop-invariant index) | loop-index-bound-checked workload | Stage 2 (sema + IR pass) |
| 12 | **W9** Max-simultaneously-live temp sizing | W9 (struct/array/str temp over-reserve) | aggregate-heavy workload frame-size-bound | Stage 1 (prescan; no IR) |

### Notes on the prediction

- **R1 first** because it is the single most-executed spill and the architecture
  already ships a limited form (pinning) — the migration is additive and the win is
  predicted largest.
- **R-loop second** because the hot game-logic shape is a tight loop and the
  tree-walker re-evaluates invariants; the existing pinning is the
  load-form precedent.
- **P1/I1 third** because they're the cheapest to build (a peephole table, no IR)
  and cumulative across a hot loop.
- **N1 (inlining) is ranked 5th, not higher, despite `nested_calls`** — because
  ember's calls go through the dispatch table (`CODEGEN_SPEC.md` §7), and inlining
  interacts with hot reload: an inlined call site does not re-route on callee hot
  reload. The design must either restrict inlining to non-hot-reloadable fns
  (a `@noinline`/`@inline` annotation, or "inline only `const` fns"), or accept
  that an inlined site is stale until the *caller* is recompiled. This is a
  real design constraint (§4.9), not a pure win — hence the gate.
- **T1 (TCO) interacts favorably with the budget/depth guard**
  (`SAFETY_AND_SANDBOX.md` §3-§4): a tail call removes the stack-frame growth *and*
  the depth charge on the tail call. The depth guard's `inc/cmp/jcc` is currently
  emitted at every script-to-script call (`emit_depth_check`, `src/codegen.cpp:423`);
  a tail call that the guard recognizes as tail would skip the charge. This makes
  TCO a safety-budget win as well as a performance win.
- **C1/D1/B1/CSE1** are the "rest" — real wins, but smaller individually, and they
  benefit most from the IR vocabulary (Stage 2), so they cluster there.

### 4.9. Inlining × hot reload (the design constraint on N1)

ember's hot reload (`../HOT_RELOAD.md`) replaces a function's dispatch-table slot
atomically; every caller's `call [table+slot*8]` re-routes without recompilation.
**Inlining breaks this**: an inlined call site has the callee's body baked into the
caller's bytes, so a callee hot-reload does not update the inlined site until the
*caller* is recompiled. The design options:

- **Restrict inlining to fns the host marks non-hot-reloadable** (an
  `@inline`/`@noinline` annotation, or "inline only `const` fns" — `const` fns are
  not hot-reload candidates by definition). This keeps hot reload correct for the
  fns that matter (game-logic handlers) while inlining the small math leaves
  (`c(x) = x+1` in `nested_calls`).
- **Accept stale inlining, recompile the caller on callee reload** (the
  transitive-recompile path). This is more invasive and changes the hot-reload
  contract; defer unless a benchmark shows inlining is a top-3 win *and* the
  restriction above is too tight.

The Stage-2 IR pass for inlining carries this constraint as a gate: the annotation
or `const`-only restriction must ship with the pass. This is why N1 is ranked 5th,
not 2nd — the design cost is real.

---

## 6. Roadmap entries (the deliverable, placed in `docs/ROADMAP.md`)

The roadmap entries below are written into `docs/ROADMAP.md` under a new
**"Codegen optimization (gated on benchmark evidence)"** section. Each entry: the
technique, the ember paths it helps (§3/W-table), the composable-pass shape (§4/§5),
the gate (the t109 benchmark result that triggers building it), and the effort.
They are **explicitly gated**, not scheduled, and cross-reference the parallel
benchmark system (t109, `BENCHMARK_SYSTEM_DESIGN.md`) as the trigger.

The ordering is the research prediction (§5); **t109's evidence reorders.**

See `docs/ROADMAP.md` for the committed entries.

---

## 7. Cross-references

- `docs/spec/COMPILER_PIPELINE.md` §5 — the SSA-lite IR target (Stage 3).
- `docs/spec/COMPILER_PIPELINE.md` §8 — the three-stage split (lower → regalloc →
  emit), "testable stages rather than one monolithic pass."
- `docs/spec/CODEGEN_SPEC.md` §5 — the deferred linear-scan regalloc design (Stage 3).
- `docs/spec/CODEGEN_SPEC.md` §4 — the rel32→rel8 branch-shrink deferral (Stage 1
  `BranchShrinkPass`).
- `docs/spec/CODEGEN_SPEC.md` §10 — the constant-folding already done + the
  div/overflow guards (W5).
- `docs/spec/CODEGEN_SPEC.md` §16 — the struct/array/string temp materialization
  (W8, W9).
- `docs/planning/DESIGN.md` §9 — "no speculative optimization before the bench
  proves it matters" (the governing principle).
- `docs/planning/GAP_ANALYSIS.md` §4 — "Matching an optimizing native-JIT
  language's speed is a v2+ goal, gated on a benchmark-proven need."
- `docs/ROADMAP.md` v0.6 milestone — "benchmark harness vs AngelScript; tune
  regalloc/peephole only if the bench shows a need" (the existing gate).
- `examples/bench_ember_vs_as.cpp` + `build/v0.6_BENCHMARK_RESULTS.md` — the
  existing prior evidence (ember 5–7× faster than AngelScript; does not locate
  ember's own slow paths; t109 extends this).
- `src/codegen.cpp` — the tree-walker this design extends (line numbers in §3).
- t109 / `BENCHMARK_SYSTEM_DESIGN.md` (in flight) — the parallel benchmark
  investigation that gates the "build first" ordering of the roadmap entries below.

## 8. Status

**Stage 1 SHIPPED (2026-07-10).** The peephole + per-basic-block local
register allocator layered over the tree-walker, behind flags
(`CodeGenCtx::enable_peephole` / `enable_local_regalloc`, default off →
byte-identical to the pre-Stage-1 tree-walker; the 26/26 ctest gate + 268/0/0
lang gate hold unchanged with flags off AND with flags on — the optimizations
are correctness-preserving). `src/peephole.{hpp,cpp}` ship the post-emit peephole
framework + two passes; the tree-walker's BinExpr integer path gained an r10
volatile-holding-register mode; `examples/codegen_opt_test.cpp` pins each
rewrite's value-equivalence (design §4.7).

**Measured (bench/bench_codegen_paths, safety-off median, ember ns):**

| path | flags off | flags on | on/off | verdict |
|---|---|---|---|---|
| int_div | 2700 | 2700 | 1.00 | flat (YAGNI) |
| call_overhead | 1225700 | 1058700 | 0.86 | **-14%** ✓ |
| loop_overhead (regalloc-only) | 9546300 | 8100000 | 0.85 | **-15%** ✓ |
| slice_bounds | 1540300 | 1669600 | 1.08 | **+8%** ✗ (regression) |
| string_decrypt | 1481200 | 1509800 | 1.02 | flat |
| struct_by_value | 200 | 200 | 1.00 | flat |

**What shipped:**
- **SmartImmPass (W4):** `mov r, imm64` (10 bytes) → `mov eax, imm32` (5 B,
  u32-fit, zero-extends) or `mov r, imm32` (7 B, s32-fit, sign-extends) for
  small literals, skipping relocatable AbsFixup/NativeFixup loads. Strictly
  local in-place rewrite padded with trailing NOPs (no label shift). A pure
  code-size win (icache); never worse than baseline.
- **SetccMovzxPass (W10):** shipped INERT. The in-place `xor rax,rax; setcc al`
  rewrite clobbers the `cmp`'s flags (which `setcc` reads); a correct W10 needs
  the zeroing moved before the `cmp` (a cross-instruction peephole = Stage 2).
  Retained in the pipeline as a placeholder so the Stage-2 upgrade is additive.
- **Local regalloc (W1):** the BinExpr integer path keeps lhs in the VOLATILE
  scratch r10 across the RHS eval instead of `push rax; ...; pop rax`, gated on
  an `expr_clobbers_r10` check (falls back to push/pop when the RHS contains a
  call, a signed Div/Mod (emit_integer_divmod's INT64_MIN check uses r10), or a
  global/slice aggregate access (the IndexExpr/FieldExpr base path uses r10)).
  r10 is volatile → NO prologue save/restore tax (a callee-saved holding
  register would need per-function save/restore and net WORSE on call-heavy
  code; the volatile design avoids that). A nested BinExpr whose r10 is
  occupied falls back to push/pop (a single holding register can't nest).

**The mixed perf result is honest and motivates Stage 2.** The regalloc is a
strong win on tight arithmetic loops (loop_overhead -15% regalloc-only, where
both `s+i` and `i+1` fire) and on call chains combined with the peephole
(call_overhead -14%), but a regression on slice_bounds (+8%, where only the
`i+1` BinExpr fires and the `mov r10/rax` reg-reg dependency chain + 6 bytes is
net slower than the hot-L1 `push/pop` store-to-load forwarding for that simple
pattern). This is the microarchitectural finding Stage 2's cost-model regalloc
addresses: a holding register helps when it eliminates a reload that would stall,
but for a simple `a + <cheap rhs>` the stack slot is hot in L1 and push/pop is
cheaper than a reg-reg dependency chain. Stage 1 ships the working subset
(flags off = inert = gate holds; flags on = correctness-preserving = gate holds +
the measured wins, with the documented slice_bounds regression).

The design (§4 architecture, §5 pass interface, §6 representation, §7 migration)
is unchanged; the sections below are the design as written pre-ship, with §4.2/§4.5/§4.7
updated to mark Stage 1 SHIPPED and record the measured numbers.

**Stage A SHIPPED (2026-07-10).** The thin three-address IR compile-time backend
— the IL-.em Stage A, the landed Stage-2 stepping stone of §4.3/§4.6 (option (c))
— shipped behind a flag (`CodeGenCtx::enable_ir_backend`, default off →
byte-identical to the pre-Stage-A tree-walker; the 27/27 ctest gate — incl. the
new `thin_ir` + `thin_ir_struct` targets — + 268/0/0 lang gate hold unchanged
with the flag off; the CLI never sets it, so the default codegen path is the
unchanged tree-walker). What shipped:
- `src/thin_ir.{hpp,cpp}` — the IR data structures (`ThinFunction`/
  `ThinBlock`/`ThinInstr` + the stable `ThinOp` uint16_t enum, the
  serialization-ready contract Stage B consumes; see the SERIALIZATION BOUNDARY
  note in `thin_ir.hpp`) + the debug pretty-printer.
- `src/thin_lower.{hpp,cpp}` — the AST→`ThinFunction` lowering
  (`lower_function`); a mechanical, value-equivalent mirror of what
  `CG::eval`/`exec_block`/`compile_func` would emit.
- `src/thin_emit.{hpp,cpp}` — the `ThinFunction`→x64 emit (`emit_x64`);
  reproduces the tree-walker's byte sequences keyed off `ThinOp`.
- `examples/thin_ir_test.cpp` — the correctness gate (modeled on Stage 1's
  `codegen_opt_test`); `thin_ir_struct` (ctest) pins the IR struct invariants.

**Contract (dual-path; the gate pins both):**
- **default off = byte-identical tree-walker.** `compile_func` falls through to
  the existing `CG` tree-walk unchanged; the ctest + lang gates hold (the flag
  is inert when off — `thin_ir_test` Part 1 pins byte-identity across two
  off-compiles and against a fresh tree-walker baseline).
- **on = value-equivalent (NOT byte-identical).** `compile_func` calls
  `lower_function` then `emit_x64`; the IR path may emit push/pop where the
  tree-walker used r10, etc. — only the JIT'd *execution* is value-equivalent
  (`thin_ir_test` Part 2 compiles a corpus both ways and asserts identical i64
  returns).
- **composes with `enable_peephole`.** The IR path runs the same post-emit
  peephole as the tree-walker (Stage 1's `peephole.{hpp,cpp}` operate on the
  emitted `vector<uint8_t>`, IR-path or tree-walker alike).
- **obf functions fall back to the tree-walker.** The lowering marks
  `@obf`/`@obf_keyed`/mba/opaque functions `non_serializable` (the obf
  transforms are emitter-level with no `ThinOp` representation); the dispatch
  skips the thin path for them (see `thin_lower.hpp`'s fallback note).

**Honest coverage (the gate records what WORKS and flags what doesn't):**

PASSING the value-equivalence gate (pinned in `thin_ir_test` Part 2, regression
protection): scalar integer arithmetic + overflow wrap, comparisons (all six
predicates), short-circuit `&&`/`||`, control flow (if/while/for/do-while/
switch, break/continue), recursion (fib), script-to-script calls, native calls
(i64(i64,i64) + math `sqrt`), cast (int width + int↔float), ternary; corpus
`runtime_audit_semantics` + `runtime_division_forms` (the division forms —
signed/unsigned Div/Mod + the `INT64_MIN/-1` overflow guard).

KNOWN GAPS (documented as SKIP in `thin_ir_test`, Stage B/C work — NOT silently
passing, NOT failing the whole gate): slices (index + bounds — the
element-load is not frame-backed, stale `rax` across a sum), structs
(by-value arg/return/field/reassign — the hidden word-0 ptr / `CopyBytes` /
`FieldAddr` ABI has emit-path bugs), strings (native `string_from_slice` +
inline-XOR decrypt emit bugs), defer-cleanup block emission (segfaults the IR
path), fixed-array indexed store (`a[expr] = v`). These node classes are
covered by the lang suite on the default-off (tree-walker) path; the IR path's
handling of them is the Stage B/C work.

**Foundation framing.** Stage A is the thin-IR *substrate* — correct lowering +
emit, no optimization (LAZY MODE per `thin_lower.hpp`: no regalloc, no peephole,
no CSE/DCE/const-prop; those are Stage C IR passes over the `ThinFunction`
AFTER the lowering produces it). It is the foundation for:
- **Stage B — `.em` IR serialization (the security property).** The `ThinOp`
  enum is a stable uint16_t serialization boundary (`thin_ir.hpp`'s
  SERIALIZATION BOUNDARY note); Stage B serializes the IR verbatim so a `.em`
  module carries IR (not raw x86), closing the raw-x86 code-injection surface
  the signed-`.em` work (F2) addresses at the signature layer.
- **Stage C — IR optimization passes.** CSE/DCE/const-prop/peephole/LICM become
  `ThinPass`es over the `ThinFunction` (matching on `ThinOp`, not bytes — far
  less brittle than Stage 1's byte-peephole, per §4.3); the Stage-1 peephole
  table carries over as IR-pattern rewrites. The Stage-3 upgrade to full
  SSA-lite (rename + slot-back + liveness + linear-scan) is additive on this
  instruction set, not a rewrite (§4.3/§4.6).

**Stage B SHIPPED (2026-07-10).** The `.em` IR serialization — the security
property. A v5 `.em` module carries IR (not raw x86); the loader deserializes +
validates + re-emits via `emit_x64` BEFORE `alloc_executable_rw`, so a
tampered/malformed v5 `.em` is rejected with NO executable page allocated.
What shipped:
- `src/em_type_codec.{hpp,cpp}` (c1a) — the shared canonical-type codec
  (the Type/EmSignature on-disk encoding, extracted from the duplicated
  writer/loader implementations; the IR serializer reuses it for `const Type*`
  → stable type ID).
- `src/thin_ir_ser.{hpp,cpp}` (c1b) — the IR serializer/deserializer
  (`serialize_thin_function` / `deserialize_thin_function` / `validate_thin_function`).
  `ThinOp` (uint16) serializes verbatim; `const Type*` via `em_type_codec`;
  `native_fn` dropped (rebound by `meta.native_name`); safety-by-construction
  (ir_magic, bounded counts, uint64 cursor arithmetic, max_vreg up front).
- v5 writer + loader paths (c2) — `em_writer.cpp` `write_em_file_v5` + the v5
  `emit_em_content` per-function record (`is_ir` byte + hoisted signature +
  ir_blob or v4 body); `em_loader.cpp` v5 `parse_file` (opaque ir_blob read) +
  `load_em_file_impl` (deserialize + native-rebind + validate + `emit_x64` re-
  emit BEFORE `alloc_executable_rw`). `em_loader.cpp` moved to `ember_frontend`
  (the re-emit needs `emit_x64` / `CodeGenCtx`).
- `examples/thin_ir_ser_test.cpp` + `examples/em_v5_ir_test.cpp` (c3) — the
  gates: IR round-trip (serialize/deserialize/validate/re-emit/execute,
  fib(10)==55) + malformed rejection (bad magic, truncated, empty, unknown
  native → no exec page).

Mixed mode: a v5 module may MIX IR (`is_ir=1`) and raw-x86 (`is_ir=0`)
functions per-function. v5 is UNSIGNED for Stage B (a v5-signed variant is
FUTURE work). See `docs/audit/AUDIT_2026-07-10_STAGE_B.md` for the full
shipment audit + the v5 security model.

**Stage C foundation.** The composable pass architecture for Stage C (IR
optimization passes over `ThinFunction`) is researched in
`docs/LLVM_PASS_SYSTEM_RESEARCH.md` (LLVM 18.1.8 pass-manager patterns
distilled for ember: concept-based polymorphism, CRTP mix-in,
PreservedAnalyses, AnalysisManager, PassInstrumentation, pipeline parsing) and
designed in `docs/spec/PASS_SYSTEM_DESIGN.md` (the mixture architecture:
extension-style discovery + LLVM-style pass interface).

**Stage C SHIPPED (2026-07-11, Steps 1-5 + four additional passes).** The composable
pass system + **eight** IR optimization passes (+ the `SubstitutionPass`
obfuscation pass):
- `src/ember_pass.{hpp,cpp}` + `src/ember_pass_registry.hpp` +
  `src/ember_pass_pipeline.hpp` — the infrastructure (type-erased pass manager,
  CRTP mix-in, PreservedAnalyses, registry, pipeline string parser,
  instrumentation callbacks). `examples/ember_pass_test.cpp` (ctest `ember_pass`).
- `extensions/opt/ext_opt.{hpp,cpp}` (ember_ext_opt) — the eight IR optimization
  passes, registered by name via `register_passes`: `ConstPropPass` (`"constprop"`),
  `DeadCodeElimPass` (`"dce"`), `CSEPass` (`"cse"`), `LICMPass` (`"licm"`),
  `StoreToLoadForwardPass` (`"forward"`), `CopyPropPass` (`"copyprop"`),
  `InstCombinePass` (`"instcombine"`), `DeadStoreElimPass` (`"dse"`).
  `examples/ir_passes_test.cpp` (ctest `ir_passes`) verifies value-preservation +
  instr-count-reduction (LICM/forward/copyprop/instcombine are value-preserving;
  LICM hoists and forward/copyprop rewrite rather than remove instrs, so they
  are checked for value-preservation rather than instr-count-reduction).
- Pass manager wired into `CodeGenCtx` (compile_func runs passes between
  lower_function and emit_x64). CLI `--passes constprop,cse,dce`. Bench
  `EMBER_IR_PASS` env var. Code-size reductions verified: constprop_fold
  406→318B, dce 382→326B, cse 418→404B.
Steps 1-5 fully shipped (2026-07-11): the infrastructure (Step 1), the first
three opt passes constprop/dce/cse (Step 2), the `CodeGenCtx` wiring + CLI +
bench (Step 3), `LICMPass` (Step 4), and `SubstitutionPass` (Step 5,
`extensions/obf/ext_obf.cpp`, `is_required = true`). Four additional IR
optimization passes shipped beyond the original Step plan: `forward`,
`copyprop`, `instcombine`, `dse` (the store-to-load forwarding pipeline that
closes the IR backend's 1.2-1.9× tree-walker gap + intra-block copy
propagation + identity-fold inst combining + dead-store elimination). See
`extensions/opt/ext_opt.hpp` for the per-pass one-line descriptions.
`EmberAnalysisManager` remains a stub/cached-analysis future. The old claim
that MBA and bogus-control-flow families remain future is superseded:
`mba_expand`, `opaque_pred`, `deadcode`, `const_encode`, `str_encrypt`, and
`block_split` ship alongside compatibility `subst`. A dedicated classic
control-flow-flattening pass still does not ship.

The design sections (§4 architecture, §5 pass interface, §6 representation, §7
migration) are unchanged; §4.3/§4.6/§4.7 are the design as written pre-ship (the
hybrid thin-IR option this Stage-A shipment implements), now with a shipped
status entry above.
