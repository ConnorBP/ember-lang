# ember — Pass Impact Benchmark (2026-07-11)

**Audited revision:** `7485ea9` (after GC wiring)
**Scope:** Measure the runtime impact of the 8 IR optimization passes + Stage 3 regalloc on the per-path codegen benchmark.
**Method:** `bench_codegen_paths --passes` — runs each benchmark path in BOTH no-passes (tree-walker) and passes-on (IR backend + constprop,forward,copyprop,instcombine,dce,licm,dse + regalloc) configurations, reporting the speedup.

---

## 0. Headline

The optimization passes + regalloc provide **modest speedups on compute-heavy paths** (constprop_fold 1.34x, int_div 1.31x) but are **not yet closing the gap to g++-O2** on most paths. Some paths are slower with passes (loop_overhead, call_overhead) because the IR backend + regalloc add overhead without benefit on simple paths. Three paths (slice_bounds, string_decrypt, struct_by_value) hit an "IR-backend gap" — the IR backend doesn't handle those features yet, so passes-on is N/A.

---

## 1. Passes impact (nopass → passes + regalloc, safety-off)

| path | nopass ns | passes ns | speedup | passes/g++-O2 | notes |
|---|---|---|---|---|---|
| constprop_fold | 1542000 | 1149000 | **1.34x** | inf | constprop folds some constants; g++-O2 still folds to 0 |
| int_div | 4600 | 3500 | **1.31x** | 1.30 | passes close the gap to g++-O2 (1.30x) |
| dce_dead_store | 1352700 | 1336100 | 1.01x | 7.09 | DCE removes the dead store but the path is still compute-bound |
| licm_invariant | 1344000 | 1351200 | 0.99x | 7.23 | LICM doesn't help (the invariant is already cheap) |
| cse_redundant | 1514600 | 1634800 | 0.93x | 8.91 | CSE + regalloc add overhead > benefit |
| call_overhead | 1276900 | 1494500 | 0.85x | 6.01 | regalloc overhead > benefit on simple call path |
| loop_overhead | 9698200 | 13619600 | 0.71x | 7.14 | regalloc overhead > benefit on simple loop |
| slice_bounds | 1525100 | N/A | — | — | IR-backend gap (bounds check not in IR) |
| string_decrypt | 1921500 | N/A | — | — | IR-backend gap (string xor not in IR) |
| struct_by_value | 200 | N/A | — | — | IR-backend gap (struct ABI not in IR) |

---

## 2. Analysis

### Where passes help
- **constprop_fold (1.34x)**: The constprop pass folds constant expressions at compile time, eliminating runtime computation. The 1.34x speedup is real but the path is still `inf` vs g++-O2 because g++-O2 folds the ENTIRE expression to 0ns while ember's constprop only folds part of it (the IR backend doesn't fully eliminate the dead code after folding).
- **int_div (1.31x)**: The passes + regalloc improve the int_div path from 4600ns to 3500ns, bringing it within 1.30x of g++-O2 (was 1.70x). This is the best result — the regalloc keeps the operands in registers instead of spilling to the stack.

### Where passes don't help (or hurt)
- **loop_overhead (0.71x)**, **call_overhead (0.85x)**, **cse_redundant (0.93x)**: The IR backend + regalloc add overhead (IR build + pass execution + re-emit) that exceeds the benefit on these simple paths. The tree-walker is already generating reasonable code for simple loops/calls; the regalloc doesn't improve it enough to offset the IR overhead. This suggests the regalloc needs to be more aggressive (better register utilization) to win on these paths.
- **dce_dead_store (1.01x)**, **licm_invariant (0.99x)**: The DCE and LICM passes technically work (the dead store is removed, the invariant is hoisted) but the effect is negligible because the paths are dominated by the loop iteration overhead, not the dead/invariant computation.

### IR-backend gaps
- **slice_bounds, string_decrypt, struct_by_value**: These paths use features the IR backend doesn't support yet (bounds-checked indexing, string XOR decryption, struct-by-value ABI). When `--passes` is used, the IR backend can't lower these, so the benchmark reports N/A. This is a known limitation — the IR backend covers the common subset of ember but not all features.

---

## 3. Conclusion

The optimization passes + regalloc are **correct** (value-preserving: validation returns 177 with and without passes) and provide **targeted speedups** (constprop 1.34x, int_div 1.31x). However, they are not yet the "SSA-IR warranted" win the benchmark verdicts call for. The gap to g++-O2 remains 5-8x on most paths because:

1. The regalloc is not aggressive enough (doesn't keep enough values in registers vs spilling to stack)
2. The IR backend doesn't cover all features (3 paths N/A)
3. The IR build + pass execution + re-emit overhead offsets the benefit on simple paths

**Recommendations:**
1. Improve the regalloc to keep more values in registers (the 5x call/loop overhead gap is the regalloc's job to close)
2. Extend the IR backend to cover the missing features (slice bounds, string XOR, struct ABI)
3. Consider running passes selectively (only on functions where the benefit exceeds the overhead) rather than always-on
4. The constprop pass should fully eliminate dead code after folding (to match g++-O2's 0ns on constprop_fold)
