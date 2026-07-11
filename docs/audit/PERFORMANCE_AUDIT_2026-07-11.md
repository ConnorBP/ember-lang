# Ember Language Performance Audit — 2026-07-11

**Read-only deep audit. No source edited.**
**Repo:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Stated HEAD:** `bf76217`. **Actual HEAD at audit time:** `7c73361` (one commit past — "Hourly audit: fix 15 compiler warnings in codegen/thin_lower/peephole/parser + doc refresh". The diff is warning fixes + doc line-cite refresh only — zero performance-relevant code changes. The bench numbers below are from the current tree and are representative of bf76217.)
**Build:** `buildt/` (MinGW g++ 15.2.0, C++17, Release, Ninja). **32/32 ctest pass** (verified this run, 85.65s total).
**Method:** Source read + live bench runs (tree-walker default, tree-walker + Stage1 opts, IR backend no-passes, IR backend + all passes). Every finding cites `file:line`.

---

## Executive Summary

Ember's tree-walking codegen is **5–9× slower than g++ -O2** on every hot path except integer div (1.04×, where YAGNI wins). The dominant overheads are architectural, not micro:

1. **Per-binary-op stack spill/reload** (`push rax; eval(rhs); pop rax`) — every BinExpr and every compound AssignExpr spills the LHS to the stack across the RHS eval. This is the single most-executed instruction on the loop and arithmetic paths.
2. **Per-call indirect dispatch + arg double-move** — every script call does `mov r11, dispatch_base; call [r11+slot*8]` (memory-indirect, defeats BTB) plus an arg stash to `[rsp]` then reload into rcx/rdx/r8/r9 (eval→rax→store→load→argreg instead of eval→argreg).
3. **Per-call frame allocation** — every call does `sub rsp, total` / `add rsp, total` even for 0-arg calls (32-byte shadow space minimum), plus an unconditional rbx save/restore in every function's prologue/epilogue regardless of whether the pin is used.
4. **Safety-on guards are expensive** — budget check at every loop back-edge (`sub [r14+off], imm32; jg; trap`) + depth check at every call (`inc [r14+off]; cmp; jge; trap` + `dec`). Safety-on adds **+35.6%** to call_overhead and **+17.6%** to loop_overhead.

The **thin IR backend is uniformly SLOWER than the tree-walker** (1.2–1.9× slower) because it emits the same push/pop spill pattern PLUS an extra store-to-frame on every instruction result (`record_dst` → `store_rax_to_rbp`). The IR optimization passes recover some of this loss on constprop/dce workloads but never beat the tree-walker on the measured set. The IR path's value today is **serialization readiness**, not runtime speed.

The IR optimization passes have real **O(n²) compile-time patterns**: ConstProp recomputes the full use/slot sets from scratch on every instruction removal; CSE scans the entire hash table on every VReg redefinition and scans all following instructions on every rematch. These are bounded by "the functions are small" today but will bite on large functions.

---

## 1. Tree-Walker Codegen (`src/codegen.cpp`) — Top 5 Hot-Path Inefficiencies

### Finding 1.1 — Per-binary-op stack spill/reload — **MEANINGFUL OVERHEAD**

**Location:** `src/codegen.cpp:1893-1900` (BinExpr integer path, regalloc OFF — the default/bench baseline), `src/codegen.cpp:2119-2121` (compound AssignExpr integer path).

The integer BinExpr path (the `s = s + i` and `i = i + 1` in every loop body) does:

```
eval(lhs)          → rax
push rax           ← spill LHS to stack (1 store + rsp bump)
eval(rhs)          → rax
mov rcx, rax       ← rhs to rcx
pop rax            ← reload LHS (1 load + rsp bump)
op rax, rcx
```

`src/codegen.cpp:1898` (regalloc ON path): the Stage 1 regalloc replaces `push/pop` with `mov r10, rax` / `mov rax, r10` — but only when `enable_local_regalloc` is true (default OFF), only when the RHS doesn't clobber r10 (`expr_clobbers_r10` check at `:1895`), and only for the outermost BinExpr (single holding register can't nest, `r10_holding_lhs` at `:1896`).

**Compound AssignExpr** (`s += i`, `i += 1`) at `src/codegen.cpp:2119-2121` uses the **same push/pop pattern and is NOT covered by the r10 regalloc** — the regalloc only applies to BinExpr, not AssignExpr compound. So the loop body's `s = s + i` (which sema may or may not lower to compound) and `i = i + 1` always spill.

**Measured impact:** loop_overhead (safety-off) = 9.73M ns vs g++-O2 1.84M ns = **5.29×**. With Stage1 regalloc ON: 9.95M ns — **the regalloc does not help loop_overhead** because the loop body (`s = s + i; i = i + 1`) is dominated by the AssignExpr store-back + the stack spill that the regalloc doesn't cover. The regalloc helps cse_redundant (~6%) and licm_invariant (~6%) where the hot expression is a pure BinExpr.

### Finding 1.2 — Per-call indirect dispatch + arg double-move — **MEANINGFUL OVERHEAD**

**Location:** `src/codegen.cpp:2434-2438` (arg stash), `src/codegen.cpp:2483-2490` (arg reg reload), `src/codegen.cpp:2520-2525` (dispatch call).

Every script-to-script call:
1. `sub rsp, total` (`:2437`) — frame alloc, minimum 32 bytes (shadow space) even for 0-4 args.
2. For each arg: `eval(arg) → rax; mov [rsp+off], rax` (`:2457-2458`) — **store to stack**.
3. Then: `mov rcx, [rsp+0]; mov rdx, [rsp+8]; ...` (`:2483-2490`) — **reload from stack into arg regs**. This is a double-move: eval→rax→store→load→argreg. A direct eval→argreg (eval lhs into rcx directly) would save the store+load pair per arg.
4. Dispatch: `mov r11, dispatch_base; call [r11 + slot*8]` (`:2522-2523`) — memory-indirect call through a dispatch table. The target is unknown to the CPU's branch predictor at the call site (the table entry can change via hot-reload), so every script call is an **indirect call with BTB pressure**.
5. `add rsp, total` (`:2528`) — frame dealloc.
6. `normalize_rax` at every call boundary (`:2529`).

**Measured impact:** call_overhead (safety-off) = 1.29M ns vs g++-O2 250K ns = **5.16×**. The bench's call_overhead does `a(i) → b(x) → c(x)` = 4 calls per iteration, so the per-call overhead is ~320K ns / 4 calls / 100K iters... actually 1.29M ns / 100K iters = 12.9 ns/iter for 4 calls = ~3.2 ns/call above g++-O2's 2.5 ns/call. The ratio is dominated by the indirect dispatch + prologue/epilogue (next finding).

### Finding 1.3 — Per-function unconditional rbx save/restore + frame setup — **MEANINGFUL OVERHEAD**

**Location:** `src/codegen.cpp:3643-3645` (prologue rbx save), `src/codegen.cpp:1069` (epilogue rbx restore), `src/codegen.cpp:3539-3642` (frame layout prescan).

Every function — even one that never uses the hot-local pin — does:
- Prologue: `push rbp; mov rbp, rsp; sub rsp, frame_size; mov [rbp-8], rbx` (`:3643-3645`)
- Epilogue: `mov rbx, [rbp-8]; mov rsp, rbp; pop rbp; ret` (`:1069-1072`)

The rbx save/restore is unconditional ("every function saves rbx to a reserved frame slot in its prologue regardless of whether it personally uses the hot-local pin" — comment at `:1057-1065`). The frame_size computation (`:3555-3642`) runs 4 separate recursive AST prescans per function: `sum_bytes`, `count_struct_temps_block`, `count_arr_temps_block`, `count_str_temps_block`, `collect_defers` — all walking the full body. This is **compile-time** O(n) per function × 5 walks, but it's pure overhead at compile time.

The runtime cost is the `mov [rbp-8], rbx` + `mov rbx, [rbp-8]` pair per call — 2 memory ops per function entry/exit that are wasted when the pin isn't used (the common case for leaf arithmetic functions like `c(x) = x + 1`).

### Finding 1.4 — Per-loop budget check (safety-on) — **MEANINGFUL OVERHEAD (safety-on only)**

**Location:** `src/codegen.cpp:385-409` (`emit_budget_check`), called at `:3769` (function entry), `:3164` (while back-edge), `:3179` (do-while back-edge), `:3235` (for-each back-edge), `:3279` (for back-edge).

When `emit_budget_checks` is on, every loop back-edge emits:
```
sub qword [r14 + off_budget], imm32    ; 7 bytes (REX.WB + 81 + modrm + disp32 + imm32)
jg .continue                            ; 2 bytes
<trap>                                  ; ~30 bytes (stub call) or 2 bytes (ud2)
.continue:
```

The `body_cost` value baked into the `imm32` is computed by `block_cost` (`src/codegen.cpp:3394-3460`) — a **recursive AST walk with dynamic_cast chains** that recomputes the cost every time it's called. `block_cost` is called once per loop back-edge + once at function entry, with no memoization. For a function with L loops and N statements, this is O(L × N) compile-time work. The cost is baked as a constant, so it's **compile-time only** — but it's redundant recomputation.

**Measured impact:** loop_overhead safety-on = 11.45M ns vs safety-off 9.73M ns = **+17.6% guard overhead**. call_overhead safety-on = 1.44M ns vs safety-off 1.06M ns = **+35.6% guard overhead** (the depth check fires 4× per iteration in the call path).

### Finding 1.5 — Per-expression dynamic_cast dispatch cascade — **MEANINGFUL OVERHEAD (compile-time)**

**Location:** `src/codegen.cpp:1631-1635` (eval entry), repeated in `exec_stmt` (`:3025+`), `prescan_stmt` (`:571+`), `expr_clobbers_r10` (`:1116+`), `block_cost`/`stmt_cost`/`expr_cost` (`:3394+`).

`CG::eval` starts with a cascade of `dynamic_cast<const IntLit*>(&ex)`, then `FloatLit`, `BoolLit`, `Ident`, `BinExpr`, ... — **every expression evaluation walks this chain at compile time**. For an expression with E nodes, this is O(E × types) dynamic_casts. The same pattern repeats in `prescan_stmt`, `expr_clobbers_r10`, `stmt_cost`, `expr_cost` — so the same AST is walked with dynamic_cast cascades 4-5 times per function at compile time.

This is **compile-time only** (the emitted x86 has no dispatch overhead — it's direct machine code). But it's the dominant compile-time cost: `compile_ns` in the bench is 100K-250K ns per small function. For the bench's tiny functions this is irrelevant, but for large scripts it will dominate compile time. A tagged-union AST (or a virtual `eval_to` dispatch) would cut this to O(E) with a single indirect call per node.

**Runtime impact: NOT A BOTTLENECK** (it's compile-time). Listed here because the task asked for per-expression overhead and this is the per-expression compile cost.

---

## 2. Thin IR Backend (`src/thin_lower.cpp`, `src/thin_emit.cpp`)

### Finding 2.1 — IR backend is uniformly SLOWER than the tree-walker — **MEANINGFUL OVERHEAD**

**Measured (IR backend ON, no passes, safety-off):**

| path | tree-walker median ns | IR backend median ns | IR / tree-walker |
|---|---|---|---|
| loop_overhead | 9,729,700 | 17,347,300 | **1.78× slower** |
| call_overhead | 1,064,000 | 1,584,000 | **1.49× slower** |
| int_div | 2,700 | 4,100 | **1.52× slower** |
| cse_redundant | 1,563,800 | 2,520,100 | **1.61× slower** |
| dce_dead_store | 1,354,700 | 2,128,500 | **1.58× slower** |
| constprop_fold | 1,538,800 | 2,504,600 | **1.63× slower** |
| licm_invariant | 1,352,100 | 2,505,800 | **1.86× slower** |

**Root cause:** `src/thin_emit.cpp:1402-1414` (`emit_int_binop` VReg form) emits the **same `push rax; load rhs; mov rcx; pop rax; op` pattern** as the tree-walker — and then `record_dst` (`src/thin_emit.cpp:364-391`) does an **additional `store_rax_to_rbp(e, meta.frame_off)`** on every instruction that has a frame_off (which is most of them, since the lowering assigns frame slots). So the IR backend does: load lhs → push → load rhs → pop → op → **store result to frame** → (next use) load result from frame. The tree-walker keeps the result in rax and uses it directly in the next eval. The IR backend's `rax_vreg` tracking (`src/thin_emit.cpp:123`) is a minimal "is it already in rax" cache, but `load_int_vreg` (`:315-329`) still does a `vregs.find()` (hash lookup) on every use and falls through to a frame load if the VReg isn't the current `rax_vreg`.

The IR backend has **no equivalent of the tree-walker's r10 regalloc** — it always push/pop. And it has **no equivalent of the tree-walker's constant folding** (`src/codegen.cpp:1786-1819`) — the lowering emits ConstInt instrs that only get folded if the constprop pass runs.

### Finding 2.2 — `unordered_map<VReg, VRegInfo>` is a compile-time cost, not a runtime bottleneck — **MINOR**

**Location:** `src/thin_emit.cpp:118-122` (VRegInfo + map), `src/thin_emit.cpp:315-329` (load_int_vreg), `src/thin_emit.cpp:364-391` (record_dst).

The `std::unordered_map<VReg, VRegInfo> vregs` is populated during emit: every `record_dst` does `vregs[dst] = info` (hash insert) and every `load_int_vreg` does `vregs.find(v)` (hash lookup). For a function with I instructions, this is O(I) hash operations during **compile time** (emit). The emitted x86 has no map overhead — it's direct frame loads/stores.

The map is **not a runtime bottleneck**. It is a **compile-time overhead** vs the tree-walker (which uses `std::unordered_map<std::string, int32_t> locals` at `src/codegen.cpp:63` — also compile-time). The VReg map is keyed on `uint32_t` (cheaper to hash than `std::string`), so it's likely faster per-lookup than the tree-walker's string-keyed `locals` map. But the IR backend does more lookups (one per src + one per dst per instruction) than the tree-walker (one per Ident eval).

**Verdict: MINOR compile-time cost. Not a runtime bottleneck.** A `std::vector<VRegInfo>` indexed by VReg (dense, since VRegs are 1..N sequential) would eliminate the hashing entirely — but this is a compile-time micro-opt, not a runtime win.

### Finding 2.3 — IR backend unnecessary copy: `out.rodata = thf.rodata` — **MINOR**

**Location:** `src/thin_emit.cpp:746` (`out.rodata = thf.rodata;  // copy`), `src/thin_emit.cpp:744` (`out.abs_fixups = e.abs_fixups()` — copy, not move).

`emit_x64` takes `const ThinFunction& thf`, so `out.rodata = thf.rodata` is a **copy** (the comment admits it). `out.abs_fixups = e.abs_fixups()` copies the fixup vector (returns const ref). These are compile-time copies of typically-small vectors (rodata is the function's string-literal bytes; abs_fixups is a handful of reloc slots). **MINOR compile-time cost.** Could be avoided if `emit_x64` took `ThinFunction` by value or rvalue-ref and moved rodata out — but the v5 loader re-uses `thf` after emit (it clears `pf.ir_blob` but `thf` is local to the loop body at `src/em_loader.cpp:678`), so a move would require restructuring. Not worth it for the sizes involved.

### Finding 2.4 — IR lowering uses the same dynamic_cast cascade as the tree-walker — **MINOR (compile-time)**

**Location:** `src/thin_lower.cpp:1238-1261` (`lower_expr` entry), `src/thin_lower.cpp:273-312` (`prescan_stmt`).

`ThinLowerer::lower_expr` has the same `dynamic_cast<const IntLit*>(&ex)` cascade as `CG::eval`. Plus the lowering runs its own `prescan_block` (`:273-312`) before lowering. So the IR path does: prescan (dynamic_cast walk) + lower (dynamic_cast walk) + emit (hash-map walk) = **3 compile-time walks** vs the tree-walker's **2** (prescan + eval). This is why `compile_ns` is higher for the IR path (e.g. int_div: tree-walker ~230K ns vs IR ~415K ns).

**Verdict: MINOR compile-time cost. Not a runtime bottleneck.**

---

## 3. IR Optimization Passes (`extensions/opt/ext_opt.cpp`)

### Finding 3.1 — ConstProp re-scans the entire function on every instruction removal — **MEANINGFUL OVERHEAD (compile-time, O(n²))**

**Location:** `extensions/opt/ext_opt.cpp:244-263` (dead-constant sweep).

The dead-constant sweep is a `while (sweep_changed)` fixpoint loop. Inside the inner `for (auto& blk : f.blocks)` → `for (auto it = instrs.begin(); ...)` loop, **every time an instruction is erased** (`:261`), the pass recomputes `used = compute_used_vregs(f)` and `read_slots = compute_read_slots(f)` from scratch (`:262-263`). The comment admits: *"Recompute used + read_slots (conservative — could be incremental, but the functions are small)."*

`compute_used_vregs` (`:103-118`) and `compute_read_slots` (`:119-132`) are each **O(total_instrs)** full scans. So removing K instructions from a function with N total instrs is **O(K × N)** per fixpoint iteration. If the fixpoint runs I iterations (each removing at least one instr), the total is **O(I × K × N)** — effectively **O(N²)** for a function where a large fraction of instructions are dead (worst case O(N³) if the fixpoint re-enters).

**Measured compile-time impact:** constprop_fold compile_ns = 241K ns (IR+passes) vs 218K ns (IR no-passes) = +23K ns pass overhead for a 21-instr function. This is negligible at current sizes. **The O(n²) will bite on large functions** (a 1000-instr function with 500 dead consts = 500K × 1000 = 500M operations — seconds).

### Finding 3.2 — CSE kill rule scans the entire hash table per VReg redefinition — **MEANINGFUL OVERHEAD (compile-time, O(n²))**

**Location:** `extensions/opt/ext_opt.cpp:374-382` (kill rule), `extensions/opt/ext_opt.cpp:418-427` (remap scan).

Two O(n²) patterns in CSE:

**(a) Kill rule** (`:374-382`): every instruction that redefines a VReg (`in.dst != 0`) scans the **entire** `table` (unordered_map) to erase entries that use `in.dst` as src1/src2 or have `in.dst` as their result VReg. For a block with B instructions and T table entries, this is O(B × T) = **O(B²)** per block.

**(b) Remap scan** (`:418-427`): when a CSE match is found, the pass scans **all instructions after the current one** in the block to remap `old_dst → new_dst`. For a match at position i, this is O(B - i). Summed over all matches, this is **O(B²)** per block.

**Measured compile-time impact:** cse_redundant compile_ns = 183K ns (IR+passes) vs 253K ns (IR no-passes) — the pass *reduced* compile time here because CSE removed instructions and the emit was cheaper. The O(n²) is not visible at 21 instrs. **Will bite on large basic blocks** (a 500-instr block = 250K table-scan operations per redefinition).

### Finding 3.3 — CSE is barely effective on the measured workload — **MEANINGFUL OVERHEAD (pass doesn't pay for itself)**

**Measured:** cse_redundant: IR no-passes 2.52M ns → IR+passes 2.50M ns (**1.6% improvement**, within noise). Code: 418 B → 404 B (14 bytes saved — one `imul` removed). The CSE pass found `i*7` computed twice and eliminated one, but the per-iteration runtime didn't move because the remaining `i*7` + the `s + a + a` still dominates.

The pass **doesn't pay for its compile-time cost** on this workload (the runtime win is within noise). CSE would pay off on larger basic blocks with more redundancy, but the bench's small functions don't exercise that.

### Finding 3.4 — DCE and ConstProp are effective where they apply — **MINOR (passes work, but IR backend overhead negates the win)**

**Measured:**
- dce_dead_store: IR no-passes 2.13M → IR+passes 1.73M (**19% improvement** — DCE removed the dead `i*13` store). Code 382 → 326 B. But tree-walker is 1.35M — **the IR+passes is still 1.27× slower than the tree-walker** because the IR backend's per-instruction store-to-frame overhead negates the DCE win.
- constprop_fold: IR no-passes 2.50M → IR+passes 1.54M (**39% improvement** — constprop folded `c=7` and the loop body shrank). Code 406 → 318 B. This matches tree-walker's 1.54M — **the only case where IR+passes catches the tree-walker**, because constprop eliminated enough instructions to overcome the IR backend's overhead.
- licm_invariant: IR no-passes 2.51M → IR+passes 1.92M (**24% improvement** — LICM hoisted `100*200`). Code 400 → 347 B. But tree-walker is 1.35M — **IR+passes is still 1.42× slower**.

**Verdict:** The passes are correct and effective at reducing instruction count, but the IR backend's fundamental overhead (Finding 2.1) means **IR+passes is slower than the tree-walker on 5 of 6 measured workloads**. The passes are infrastructure for a future SSA-lite + linear-scan regalloc that would actually make the IR path faster — today they're ahead of their backend.

### Finding 3.5 — LICM predecessor/loop analysis is O(blocks²) — **MINOR (compile-time)**

**Location:** `extensions/opt/ext_opt.cpp:456-611`.

LICM builds a predecessor map (`:462-470`, O(blocks)), finds back-edges + does reverse BFS per loop (`:477-505`, O(blocks) per loop, O(loops × blocks) total), then hoists. The `loop_def_vregs` and `loop_written_slots` are `std::set` (`:552-558`) — O(log n) inserts. The overall complexity is O(loops × blocks × instrs) — **not quadratic in the total IR size** unless there are many loops. For the bench's single-loop functions this is trivial. **MINOR compile-time cost.**

---

## 4. .em Loader (`src/em_loader.cpp`)

### Finding 4.1 — v5 IR load-time re-emit is significantly more expensive than v4 raw-x86 — **MEANINGFUL OVERHEAD (load-time)**

**Location:** `src/em_loader.cpp:651-749` (v5 IR re-emit path).

The v5 path for an IR function does:
1. `deserialize_thin_function` (`:682-688`) — parse the ir_blob into a ThinFunction.
2. Native rebind scan (`:693-712`) — **full scan of all blocks × instrs** to find CallNative instrs and look up their `native_name` in the host table. O(total_instrs) with a hash lookup per CallNative.
3. `validate_thin_function` (`:715-724`) — **another full scan** for VReg bounds, block-target bounds, slot range checks. O(total_instrs).
4. `emit_x64(thf, ictx)` (`:727-730`) — **the full IR emit** (Finding 2.1): builds the `unordered_map<VReg,VRegInfo>`, walks all blocks/instrs, emits x86. O(total_instrs) with hash ops.
5. Convert `cf.abs_fixups` → `pf.relocs` (`:737-745`) and `cf.native_fixups` → `pf.native_bindings` (`:747-755`) — small vector copies.

vs the **v4 raw-x86 path**: `memcpy` the pre-compiled code bytes into the exec page + apply relocs. O(code_size).

The v5 re-emit is **O(total_instrs) with multiple full scans + hash operations + x86 emission**, vs v4's **O(code_size) memcpy**. For a function with I instrs producing C code bytes, v5 does ~4× I work (deserialize + rebind scan + validate scan + emit) plus the emit's hash-map overhead, vs v4's single memcpy of C bytes.

**Is the re-emit cost acceptable?** For the **security model** — yes. The re-emit is the v5 security gate: a tampered IR is rejected at validation with no exec page allocated (`:653-654`). You cannot get this property with raw-x86 (raw bytes can't be validated for safety). The re-emit is the price of IR-level verification.

For **load latency** — it's a measurable cost. The bench doesn't measure load time (it measures compile + run), but the v5 load is ~4× the per-instruction work of a JIT compile (deserialize + validate + emit) plus the v4-style reloc patch. For a large module this will be the dominant load cost. **Acceptable given the security model; worth measuring separately.**

### Finding 4.2 — v5 native rebind scan is redundant with validate — **MINOR**

**Location:** `src/em_loader.cpp:693-712` (native rebind), `src/em_loader.cpp:715-724` (validate).

Both the native rebind scan and the validate scan do a full `for blk → for in` walk. `validate_thin_function` already checks "CallNative non-empty name" (per the comment at `:716-718`). The native rebind could be folded into the validate scan (or vice versa) to save one full walk. **MINOR — one extra O(n) walk at load time.**

### Finding 4.3 — v5 ir_blob clear + shrink_to_fit — **NOT A BOTTLENECK (good practice)**

**Location:** `src/em_loader.cpp:689-691`.

`pf.ir_blob.clear(); pf.ir_blob.shrink_to_fit();` after deserialize — frees the blob memory before the re-emit. This is correct and good (the blob is never read again). **NOT A BOTTLENECK.**

---

## 5. Benchmark Harness (`bench/bench_codegen_paths.cpp`)

### Finding 5.1 — The harness is sufficient for the current optimization gate but missing critical measurements — **MEANINGFUL GAP**

The harness (`bench/bench_codegen_paths.cpp`) measures 10 paths (6 original + 4 IR-pass workloads) × 2 safety modes × ember-vs-g++-O2, with paired interleaved comparison + bootstrap CI (the paired CI was added per the task note). It records compile_ns, code_bytes, ir_instrs per cell. This is **sufficient for the current gate** ("no speculative optimization before the bench proves it matters" — DESIGN §9).

**What's missing:**

**(a) No IR-backend-vs-tree-walker direct comparison in the output.** The harness supports `EMBER_IR_BACKEND=1` but the results MD doesn't have an "engine" column for IR-vs-tree-walker — it only shows ember-vs-g++-O2. The IR backend's uniform slowdown (Finding 2.1) was only visible by running the bench twice and diffing. **Add an `engine ∈ {tree_walker, ir_backend, ir_backend+passes}` axis** so the IR path's cost is a first-class result, not a manual diff.

**(b) No load-time measurement.** The harness measures compile_ns (tokenize→parse→sema→codegen→finalize) but NOT .em load time. The v5 re-emit cost (Finding 4.1) is invisible. **Add a load-time path** that serializes a function to .em, loads it, and times the load (deserialize + validate + re-emit + reloc patch) vs a v4 raw-x86 load.

**(c) No large-function scaling test.** All bench paths are tiny (20–100 inner_n, ≤30 IR instrs). The O(n²) compile-time patterns in ConstProp (Finding 3.1) and CSE (Finding 3.2) are invisible at these sizes. **Add a "large_fn" path** with a 500+ statement function (e.g. a generated switch with 100 cases, or a loop body with 50 statements) to expose compile-time scaling.

**(d) No allocation/GC pressure measurement.** The harness doesn't measure heap allocations during compile/run. The `unordered_map` constructions (locals, vreg_types, vregs, vreg_const, slot_const, CSE table) and `std::vector` growth (instrs, args, arg_frame_offs, arg_types) are invisible. **Add an allocation counter** (or at least a compile_ns breakdown: lex/parse/sema/codegen/emit sub-timings).

**(e) No float-heavy path.** The bench has int_div but no float arithmetic path (the BENCHMARK_SYSTEM_DESIGN.md §2 table lists "float arithmetic add/mul" and "float sqrt" as unshipped). The float path has a different spill pattern (`sub rsp,8; movsd [rsp],xmm0; ...; movsd xmm1,[rsp]; add rsp,8` at `src/codegen.cpp:1848-1855`) that should be measured separately.

**(f) No native-call-overhead path.** The bench measures script-to-script call_overhead but not script-to-native call overhead (the `mov rax,imm64 + call rax` pattern, Finding 7). The BENCHMARK_SYSTEM_DESIGN.md §2 table lists "native call overhead" as unshipped.

**(g) No measurement of the `normalize_rax` cost for i32 types.** All bench paths use i64 (where normalize_rax is a no-op). i32 code gets `movsxd rax,eax` or `mov eax,eax` after every operation — this should be measured for i32-heavy workloads.

### Finding 5.2 — Paired CI is well-implemented — **NOT A BOTTLENECK (good)**

**Location:** `bench/bench_codegen_paths.cpp:198-249` (`time_paired`), `:405-420` (call site).

The paired/interleaved comparison alternates ember/baseline per iteration, collects per-pair ratios, and computes a bootstrap 95% CI (B=1000 resamples, fixed seed). This is the right methodology for killing shared-mode noise. The only minor issue: the bootstrap does 1000 × `ratios.size()` work with a `std::vector<double>` allocation per resample (`:240-243`) — for 200 pairs this is 200K operations + 1000 vector allocations, ~0.5ms. **NOT A BOTTLENECK.**

### Finding 5.3 — constprop_fold baseline measures 0 ns (g++-O2 const-folds the loop) — **MINOR (harness limitation)**

**Measured:** constprop_fold g++-O2 median = 0.0 ns (the baseline `bench_constprop_fold` is `volatile`-guarded but g++-O2 still const-folds `b=3; c=b+4; s+=c` to `s+=7` and the loop becomes `s += 7 * N` which... actually the baseline takes N and loops, so 0 ns means g++-O2 eliminated the loop entirely or the call is <1 ns). The `ratio = inf` is meaningless. **The harness should use a non-foldable baseline** (e.g. feed `b` and `c` through a `volatile` so g++-O2 can't const-fold them) — the string_decrypt path already does this (`check_correctness = false` because the baseline is a non-decrypt reference). The constprop path should follow the same pattern.

---

## 6. Extension / Native Call Overhead

### Finding 6.1 — `mov rax, imm64 + call rax` is optimal for the relocation model — **NOT A BOTTLENECK**

**Location:** `src/codegen.cpp:478-484` (`emit_counted_native_call`), `src/codegen.cpp:141-156` (`emit_native`), `src/x64_emitter.hpp:148-154` (`mov_reg_native`).

Every script-to-native call emits:
```
mov rax, imm64(native_ptr)    ; 10 bytes (REX.W + B8 + 8-byte ptr) — or a native_fixup placeholder for .em
call rax                       ; 2 bytes (FF D0)
```
Total: 12 bytes + the depth check (safety-on).

A direct `call rel32` would be 5 bytes, but it requires the target address at emit time and isn't relocatable. The `mov rax,imm64 + call rax` pattern is **required** for the `.em` serialization model (the imm64 is a placeholder patched at load time via `native_fixups`). The SmartImm peephole (`src/peephole.cpp:67-90`) can shrink the `mov rax, imm64` to `mov eax, imm32` (5 bytes) when the ptr fits in u32 — but native function pointers on x64 are 8 bytes and rarely fit in u32, so this rarely fires for native calls.

The real cost is the **indirect call through rax** — the CPU's call predictor has to predict the target via the BTB. For a fixed native (same ptr every call) this is well-predicted. For the dispatch-table script call (`call [r11+slot*8]`) it's also well-predicted if the slot doesn't change. **NOT A BOTTLENECK** — the pattern is inherent to the relocatable-JIT design.

### Finding 6.2 — Depth check on every native call (safety-on) — **MEANINGFUL OVERHEAD (safety-on)**

**Location:** `src/codegen.cpp:478-484` (`emit_counted_native_call` calls `emit_depth_check` before + `emit_depth_leave` after).

Every native call (including operator overloads like `vec3_add` and string natives like `string_length`) gets the depth check/leave pair when safety is on. The depth check (`src/codegen.cpp:430-456`) emits:
```
inc dword [r14+off_depth]      ; 4 bytes
mov eax, [r14+off_max_depth]   ; 4 bytes
cmp [r14+off_depth], eax       ; 4 bytes
jge .trap                       ; 2 bytes
.ok:
```
And `emit_depth_leave` (`:460-475`) emits `dec dword [r14+off_depth]` (4 bytes). That's ~18 bytes of guard per native call.

**Measured impact:** the string_decrypt path (safety-on) is +4% over safety-off — the string_length native call's depth check is a small fraction of the inline-XOR decrypt cost. For a call-heavy path with cheap natives (e.g. `vec3_add` called in a tight loop), the depth check would be a larger fraction. **MEANINGFUL for call-heavy safety-on code; NOT measurable on the current bench's native paths.**

---

## 7. Memory / Allocations on the Hot Path

### Finding 7.1 — Runtime hot path has zero heap allocations — **NOT A BOTTLENECK (good)**

The emitted x86 code does **no heap allocation at runtime**. All locals are frame slots (`[rbp+off]`), all temps are pre-counted into the frame size at compile time (`src/codegen.cpp:3555-3642`), string-literal decryption is inline into a stack slot (`src/codegen.cpp:2567-2630`), and slices are {ptr,len} register pairs. The `rodata` is a single append at compile time. **This is the correct design for a JIT — no GC, no allocator on the hot path.**

### Finding 7.2 — Compile-time hot path has many string-keyed unordered_map ops — **MINOR (compile-time)**

**Location:** `src/codegen.cpp:63` (`std::unordered_map<std::string, int32_t> locals`), `src/codegen.cpp:1669` (`local_types[id->name]` — hash lookup per Ident eval), `src/codegen.cpp:560-566` (`alloc_local` — hash insert per local).

Every `Ident` eval does `locals.find(name)` (string hash + compare) and `local_types[name]` (another hash lookup). Every `alloc_local` does two hash inserts. These are **compile-time only** (the emitted code uses the baked `[rbp+off]` offset), but they're the dominant compile-time cost for name resolution. A `std::unordered_map<std::string_view, ...>` or a pre-built dense index (intern the names once, index by int) would cut this. **MINOR compile-time cost.**

### Finding 7.3 — ThinInstr has 3 vectors + 1 string — push_back moves a heavy struct — **MINOR (compile-time)**

**Location:** `src/thin_ir.hpp:175-186` (ThinInstr), `src/thin_lower.cpp:246-249` (`emit` → `cur_block().instrs.push_back(std::move(in))`).

Each `ThinInstr` has `std::vector<VReg> args`, `std::vector<int32_t> arg_frame_offs`, `std::vector<const Type*> arg_types`, and `std::string native_name`. The lowering's `emit()` does `push_back(std::move(in))` — the move transfers the 3 vector headers + the string (cheap, ~4 pointer assignments each), but the `ThinInstr` struct itself is ~96 bytes moved. For a function with I instructions, this is I moves. **MINOR compile-time cost.** Most instrs have empty args/arg_frame_offs/arg_types (only calls populate them), so the moves are mostly empty-vector-header transfers.

### Finding 7.4 — `rodata.insert(end, ...)` is amortized O(1) but not reserved — **MINOR (compile-time)**

**Location:** `src/codegen.cpp:131-135` (`append_rodata`), `src/thin_lower.cpp:205-208` (`append_rodata`).

`rodata.insert(rodata.end(), data, data + size)` grows the vector without `reserve`. For a function with S string literals totaling B bytes, this is O(B) with possible reallocations. **MINOR compile-time cost** — could reserve based on the string-literal count prescan, but rodata is typically small (KB).

---

## Consolidated Verdict Table

| Finding | Category | Severity | File:Line |
|---|---|---|---|
| 1.1 Per-binary-op push/pop spill | tree-walker runtime | **MEANINGFUL** | `codegen.cpp:1893-1900, 2119-2121` |
| 1.2 Per-call indirect dispatch + arg double-move | tree-walker runtime | **MEANINGFUL** | `codegen.cpp:2434-2490, 2520-2525` |
| 1.3 Unconditional rbx save/restore + 5 prescans | tree-walker runtime+compile | **MEANINGFUL** | `codegen.cpp:3643-3645, 1069, 3555-3642` |
| 1.4 Per-loop budget check (safety-on) | tree-walker runtime | **MEANINGFUL** (safety-on) | `codegen.cpp:385-409, 3164, 3179, 3235, 3279` |
| 1.5 dynamic_cast cascade per expr | tree-walker compile-time | **MEANINGFUL** (compile) | `codegen.cpp:1631-1635, 3025, 571, 1116, 3394` |
| 2.1 IR backend uniformly slower than tree-walker | IR runtime | **MEANINGFUL** | `thin_emit.cpp:1402-1414, 364-391` |
| 2.2 `unordered_map<VReg,VRegInfo>` | IR compile-time | **MINOR** | `thin_emit.cpp:118-122, 315-329` |
| 2.3 `out.rodata = thf.rodata` copy | IR compile-time | **MINOR** | `thin_emit.cpp:744-746` |
| 2.4 IR lowering dynamic_cast cascade | IR compile-time | **MINOR** | `thin_lower.cpp:1238-1261, 273-312` |
| 3.1 ConstProp full re-scan on every erase | opt compile-time | **MEANINGFUL** (O(n²)) | `ext_opt.cpp:244-263` |
| 3.2 CSE full-table kill scan + remap scan | opt compile-time | **MEANINGFUL** (O(n²)) | `ext_opt.cpp:374-382, 418-427` |
| 3.3 CSE barely effective on bench | opt effectiveness | **MEANINGFUL** (doesn't pay) | measured |
| 3.4 DCE/ConstProp effective but IR overhead negates | opt effectiveness | **MINOR** | measured |
| 3.5 LICM O(loops×blocks×instrs) | opt compile-time | **MINOR** | `ext_opt.cpp:456-611` |
| 4.1 v5 IR re-emit vs v4 raw-x86 | load-time | **MEANINGFUL** (load) | `em_loader.cpp:651-749` |
| 4.2 v5 native rebind scan redundant with validate | load-time | **MINOR** | `em_loader.cpp:693-724` |
| 4.3 v5 ir_blob clear+shrink | load-time | **NOT A BOTTLENECK** | `em_loader.cpp:689-691` |
| 5.1 Bench missing IR axis, load-time, large-fn, float, native | bench coverage | **MEANINGFUL GAP** | `bench_codegen_paths.cpp` |
| 5.2 Paired CI implementation | bench quality | **NOT A BOTTLENECK** | `bench_codegen_paths.cpp:198-249` |
| 5.3 constprop_fold baseline = 0 ns | bench limitation | **MINOR** | measured |
| 6.1 `mov rax,imm64 + call rax` native pattern | native runtime | **NOT A BOTTLENECK** | `codegen.cpp:478-484, x64_emitter.hpp:148-154` |
| 6.2 Depth check per native call (safety-on) | native runtime | **MEANINGFUL** (safety-on) | `codegen.cpp:478-484, 430-475` |
| 7.1 Runtime hot path zero heap alloc | memory runtime | **NOT A BOTTLENECK** (good) | design |
| 7.2 String-keyed unordered_map per Ident | memory compile-time | **MINOR** | `codegen.cpp:63, 1669, 560-566` |
| 7.3 ThinInstr 3-vector push_back | memory compile-time | **MINOR** | `thin_ir.hpp:175-186, thin_lower.cpp:246-249` |
| 7.4 rodata.insert without reserve | memory compile-time | **MINOR** | `codegen.cpp:131-135, thin_lower.cpp:205-208` |

---

## Recommendations (prioritized, for the optimization gate — NOT applied)

These are **design observations from the data**, not changes I made (read-only audit):

1. **The single highest-leverage runtime fix is eliminating the per-binary-op push/pop spill** (Finding 1.1). The Stage 1 r10 regalloc helps BinExpr but misses compound AssignExpr. Extending the holding-register scheme to compound AssignExpr + allowing 2 holding registers (r10 + another volatile) would cut the loop_overhead ratio meaningfully. The bench's loop_overhead (5.29×) is the headline number that triggers the "SSA-IR warranted" verdict.

2. **The call path's arg double-move** (Finding 1.2) is the second target. Eval'ing the first arg directly into rcx (instead of eval→rax→store→load→rcx) saves 2 memory ops per arg. This requires a multi-register eval convention (eval into a specific reg, not always rax) — which is exactly what a linear-scan regalloc would provide.

3. **The IR backend should not be enabled for performance today** (Finding 2.1). It's uniformly slower than the tree-walker. Its value is serialization (v5 .em) + the optimization pass infrastructure. It becomes a runtime win only when paired with a real regalloc that eliminates the per-instruction store-to-frame — which the current `record_dst` always does.

4. **The ConstProp O(n²) re-scan** (Finding 3.1) should be made incremental before the IR path sees large functions. A worklist of "newly dead VRegs/slots" checked against a static use map (computed once, updated on removal) would cut it to O(n).

5. **The bench should add the IR-engine axis + a large-function path + a load-time path** (Finding 5.1) before any IR-backend optimization investment. The current bench can't see the IR backend's overhead or the O(n²) scaling — both are invisible at ≤30 instrs.

---

*Audit complete. No source files edited. Bench artifacts regenerated: `buildt/results_codegen_paths.{csv,md}`.*
