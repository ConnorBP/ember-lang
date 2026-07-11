# ember per-path codegen bench (prototype) — results

Machine: gcc 15.2.0, 64-bit. Date: Jul 11 2026 09:46:12.
Baseline: g++ -O2 -std=c++17 (compiled from source this run).
Stage1 opts: none (flags off — byte-identical baseline).
Headline = **median ns** (resistant to scheduler outliers). ratio = ember/g++-O2 (median). >1 = ember slower than an optimizing native compiler.

| path | safety | ember med ns | g++-O2 med ns | ember/g++-O2 | verdict |
|---|---|---|---|---|---|
| int_div | off | 4100.0 | 2600.0 | 1.58 | ember slower (peephole candidate) |
| int_div | on | 4000.0 | 2700.0 | 1.48 | ember slower (peephole candidate) |
| call_overhead | off | 1556300.0 | 246600.0 | 6.31 | ember MUCH slower (SSA-IR warranted) |

## safety-on overhead (guard cost = safety_on - safety_off)

| path | safety-off med ns | safety-on med ns | guard overhead (abs / %) |
|---|---|---|---|
| int_div | 4100.0 | 4000.0 | -100.0 ns / -2.4% |
| call_overhead | 1556300.0 | 0.0 | -1556300.0 ns / -100.0% |
| loop_overhead | 17205000.0 | 19267300.0 | +2062300.0 ns / +12.0% |
| slice_bounds | 1535000.0 | 1733900.0 | +198900.0 ns / +13.0% |
| string_decrypt | 1513600.0 | 1558600.0 | +45000.0 ns / +3.0% |
| struct_by_value | 200.0 | 300.0 | +100.0 ns / +50.0% |
| cse_redundant | 2483000.0 | 2492000.0 | +9000.0 ns / +0.4% |
| dce_dead_store | 1718000.0 | 1731500.0 | +13500.0 ns / +0.8% |
| constprop_fold | 1533200.0 | 1541300.0 | +8100.0 ns / +0.5% |
| licm_invariant | 1936600.0 | 1927600.0 | -9000.0 ns / -0.5% |
