# ember — Pending Actions (as of 2026-07-11)

**Read-only audit synthesis.** This document collates every actionable item across the
nine audit/planning/research documents the task named, classifies each as **COMPLETED**,
**PENDING** (with priority), or **DEFERRED**, and produces a single prioritized action list.

**Audited revision:** `5df97f2` (HEAD)
**Baseline:** ctest 34/34 PASS, lang suite 274/0/0 (green tree)

**Source documents reviewed:**
1. `docs/audit/AUDIT_2026-07-11_SYNTHESIS.md` — deep audit synthesis (27 confirmed findings)
2. `docs/audit/PENDING_FEATURES_2026-07-11.md` — pending language features
3. `docs/audit/AUDIT_2026-07-11_DOCS_TESTS.md` — doc/test gaps
4. `docs/audit/PERFORMANCE_AUDIT_2026-07-11.md` — performance findings
5. `docs/audit/SECURITY_AUDIT_2026-07-11.md` — security findings
6. `docs/audit/AUDIT_CODE_CORRECTNESS_2026-07-11.md` — correctness findings
7. `docs/planning/plan_OS_IO_EXTENSIONS.md` — OS I/O extension plan
8. `docs/planning/plan_STANDALONE_BUNDLER.md` — standalone exe bundler plan
9. `docs/LLVM_ADDITIONAL_PASSES_RESEARCH.md` — additional LLVM pass research

---

## 0. What's Already Done (confirmed against git log + source)

### 0.1 Correctness defects C1–C6 — ALL FIXED

The six confirmed codegen/sema correctness defects from `AUDIT_CODE_CORRECTNESS_2026-07-11.md`
are all fixed. The `PENDING_FEATURES_2026-07-11.md` doc was written at commit `d25cc8c` when
they were still open; they were fixed in subsequent commits. Verified against the git log
and the SYNTHESIS post-audit note.

| ID | Defect | Fix commit | Pin |
|----|--------|-----------|-----|
| C1 | `arr[i].field` on struct arrays miscompiles | `381a616` | ctest `field_of_index` (`examples/field_of_index_test.cpp`) |
| C2 | for-each over non-{1,2,4,8}-byte elements (wrong scale + truncation) | `ab49898` | `src/codegen.cpp` for-each now uses `imul`-based element address |
| C3 | `g[..]` view of global fixed array (garbage/segfault) | `3b7c8df` | `src/codegen.cpp` global array/slice/struct base resolution |
| C4 | thin-IR int compare with >32-bit literal (truncated immediate) | `4d4036e` | `src/thin_emit.cpp` `emit_cmp` falls back to `mov imm64` + `cmp_reg_reg` |
| C5 | u64 literals ≥ 2^63 rejected by sema | (post-audit fix) | `src/sema.cpp` `adapt_int_lit` U64 arm accepts any 64-bit bit pattern |
| C6 | f64 global initializers lose precision (folded through f32) | `3b7c8df` | `src/sema.cpp` `try_eval_const_f64` added; ctest `float_global_regression` |

### 0.2 Security findings S1/S2 — ALL FIXED

| ID | Finding | Fix commit | Verification |
|----|---------|-----------|--------------|
| S1 (CRITICAL) | v5 IR `frame_off` validation gap — return-address overwrite | `d25cc8c` | `src/thin_ir_ser.cpp:648-650` now checks `frame_off != 0` for ALL instructions (extended beyond the original 7 ops) |
| S2 (HIGH) | ext_array element bounds check overflow via `size_t` mul wrap | `d25cc8c` | `extensions/array/ext_array.cpp` now uses element-count check `size_t(i) < s->bytes.size()/s->elem_size` (no overflow) |

### 0.3 Correctness potential issues — partially fixed

| ID | Issue | Status |
|----|-------|--------|
| P2 | `finalize()` rodata-relocation pass 2 lacks bounds check (asymmetric with pass 1) | **FIXED** — commit `2223b3a` ("fix P2 OOB-write in engine.cpp finalize pass 2") |
| P6 | Native returning script-struct by value = Win64 ABI mismatch (latent) | **FIXED** — commit `a6ba756` ("Fix: Win64 hidden-pointer ABI for >8-byte struct args to native calls"); pinned by ctest `win64_abi` |

### 0.4 Doc gaps partially fixed (commit `2223b3a`)

| ID | Gap | Status |
|----|-----|--------|
| A3 | PASS_SYSTEM_DESIGN header "Status: pre-implementation" | **FIXED** — now "shipped (Steps 1-5, 2026-07-11)" |
| A4 | ext_opt.hpp header "Three IR→IR optimization passes" | **FIXED** — now "Four IR→IR optimization passes" |
| A5 | GAP_ANALYSIS marks for-each and match "✗ v1" | **FIXED** — now "✓ v1 (shipped 2026-07-11)" |
| B1 | README self-contradicts on `--load-em` CLI action | **FIXED** — line 100's "There is no CLI --load-em action" removed; line 145 now documents the action |
| A1 | PASS_SYSTEM_DESIGN §8 marks LICM/SubstitutionPass FUTURE | **FIXED** — §8 now marks Step 4 (LICM) and Step 5 (SubstitutionPass) as "SHIPPED (2026-07-11, partial)"; FlatteningPass/MBAPass remain FUTURE (Step 6) |
| A2 | CODEGEN_OPTIMIZATION_DESIGN §8 "first three passes" / "remain FUTURE" | **FIXED** — now "Stage C SHIPPED (2026-07-11, Steps 1-5; Steps 4-5 partial)" |

### 0.5 Test gaps partially fixed

| ID | Gap | Status |
|----|-----|--------|
| D2 | No nested for-each test | **FIXED** — `tests/lang/valid_for_each_nested_3level.ember` added (+ `valid_for_each_break_continue.ember`, `valid_for_each_empty_body.ember`, `valid_for_each_i32_slice.ember`, `valid_for_each_single.ember`) |
| D3 | No match-without-wildcard test | **FIXED** — `tests/lang/valid_match_no_wildcard.ember` added (+ `valid_match_bool_subject.ember`, `valid_match_duplicate_patterns.ember`, `valid_match_assignment_in_arm.ember`, `valid_match_nested_in_for_each.ember`, `valid_enum_used_in_match.ember`) |

---

## 1. PENDING Items (need action, ordered by priority)

### P0 — HIGH: Security findings 3, 4, 5, 6 (from SECURITY_AUDIT)

These are the highest-priority open items. S1/S2 (critical/high) are fixed; the four
remaining security findings are all still present in the source (verified).

| Priority | ID | Finding | File | Fix |
|----------|----|---------|------|----|
| **HIGH** | Sec-3 (MEDIUM) | ext_vec/mat/quat `*_new` can `std::terminate` on OOM — `push_back` not wrapped in try/catch; `bad_alloc` propagates through `extern "C"` native boundary into JIT'd code → process death | `extensions/vec/ext_vec.cpp:19,38,55`; `extensions/mat/ext_mat.cpp:17,21`; `extensions/quat/ext_quat.cpp:17` | Wrap each `push_back` in try/catch for `bad_alloc` + `length_error`, return 0 (null handle) on failure. Mirror the pattern already used in `ext_array::arr_new`, `ext_string::str_new`, `ext_map::n_map_new`. |
| **HIGH** | Sec-4 (MEDIUM) | `n_string_substr` can `std::terminate` on OOM — `x->substr(s, actual_len)` allocates before `str_new`'s try/catch; `bad_alloc` propagates through native boundary | `extensions/string/ext_string.cpp:113-119` | Wrap the `substr` call in try/catch, or construct the substring inside `str_new`'s try block. Mirror `n_string_concat` (line 86-98) which correctly wraps its `reserve` + `+=`. |
| **HIGH** | Sec-5 (MEDIUM) | Extension stores (array/string/map/vec/mat/quat) are not thread-safe — concurrent `context_t`s calling `*_new` race on the shared `std::vector` (UB → heap corruption) | `extensions/array/ext_array.cpp:15`; `extensions/string/ext_string.cpp:22`; `extensions/map/ext_map.cpp:21`; `extensions/vec/ext_vec.cpp:17,36,53`; `extensions/mat/ext_mat.cpp:15`; `extensions/quat/ext_quat.cpp:15` | Either (a) add a mutex to each store (matching `ext_sync`/`ext_lifecycle` which already use `g_store_mutex`/`g_mutex`), (b) document that extension stores are single-threaded only (breaking the multi-context model for extension-using scripts), or (c) use thread-local stores. **Recommend (a)** — matches the pattern ext_sync/ext_lifecycle already establish. |
| **MEDIUM** | Sec-6 (LOW) | v5 IR validator does not reject duplicate block IDs — two blocks with the same `id` cause `block_labels[id]` to be bound twice (second wins); `jmp` to that ID goes to the wrong block. Correctness issue (not memory safety — all jumps stay within the function) | `src/thin_ir_ser.cpp:588-595` (checks `blk.id >= num_blocks` and `blocks[0].id == 0`, but no duplicate check) | Add a `std::unordered_set<uint32_t> seen_ids` check in `validate_thin_function`: reject if `blk.id` is seen twice. ~5 lines. |

### P1 — HIGH: The `io` extension (CLI Family B) — biggest day-to-day gap

**From:** `PENDING_FEATURES_2026-07-11.md` §7 P1; `plan_OS_IO_EXTENSIONS.md` (full design exists)

A script can only signal via its integer exit code. There is no `print`/`println`/`read_line`/
`read_file`/`write_file`. The `io` extension is the single feature whose absence makes ember
unusable as a standalone scripting language. The ROADMAP's re-entry trigger ("a demo or real
use genuinely blocked on output beyond the exit code") has effectively already fired.

**Status:** Full design exists in `docs/planning/plan_OS_IO_EXTENSIONS.md` — one `extensions/io/`
library with six granular registration functions (`register_console`/`file`/`path`/`dir`/`exec`/
`spawn` + `register_all`). Console output (print/println/print_i64) and pure path-string ops
ungated; everything touching the filesystem/process tree/blocking input is `PERM_FFI`-gated
(compile-time sema rejection). Two layers of defense: registration layer (host chooses which
natives exist) + permission layer (`PERM_FFI` gating). CLI gets a `--ffi` flag.

**Not implemented:** no `extensions/io/` directory exists. Pure host C++ against the v1
`NativeFn` API; zero JIT/type-system changes. The design is ready; the work is implementation.

**Priority:** HIGH. Unblocks debugging, logging, config loading, code-gen/templating CLI tools.
The single highest-impact feature gap for real use.

### P2 — MEDIUM: Remaining doc/test gaps (from AUDIT_2026-07-11_DOCS_TESTS)

The `2223b3a` commit fixed 6 of the 11 STALE findings (A1-A5, B1) and 2 of the 3 INACCURATE
findings (B1; B2/B3 status below). The following remain open:

| Priority | ID | Gap | Fix |
|----------|----|-----|-----|
| **MEDIUM** | D1 | **map extension has ZERO ctest coverage** — no `ext_map_test.cpp` ctest target; only lang-suite coverage (4 `valid_map_*.ember` files). The SYNTHESIS flagged this as the highest-impact test gap. | Add `examples/ext_map_test.cpp` mirroring `ext_sync_test`/`ext_lifecycle_test` — registration smoke + happy-path (set/get/contains/length/remove/clear) + bounds (invalid handle, missing key). Add `add_test` in CMakeLists. |
| **MEDIUM** | A8/C5 | **v5 IR `.em` format undocumented** in `BUNDLING_AND_EM_MODULES.md` and `MODULES.md` — the bundling doc (canonical `.em` reference) is two format versions behind; no v5/`ir_blob`/`is_ir`/re-emit-at-load security model documentation. Grep confirms 0 occurrences. | Add a "Version 5 — IR `.em`" section to `BUNDLING_AND_EM_MODULES.md`: the `is_ir` per-function byte, the `ir_blob` record, the re-emit-at-load security model (deserialize → validate → re-emit via `emit_x64` BEFORE `alloc_executable_rw`), malformed-rejection guarantees. |
| **MEDIUM** | A6 | **SPEC_AUDIT_2026-07-10 F2 says "no serializable IR today"** — superseded by the thin-IR + v5 shipment. The "No." answer at line 93 and "IL-`.em` re-lowering remains deferred" at line 78 are stale. | Add a correction note to `SPEC_AUDIT_2026-07-10.md` F2: the thin three-address IR shipped 2026-07-10 (Stage A) and the v5 IR `.em` serialization shipped (Stage B). The "Accurate" verdict at line 227 is no longer accurate. |
| **MEDIUM** | A7 | **CODEGEN_SPEC §5 "the shipped backend walks the AST directly"** omits the thin-IR backend. The sibling COMPILER_PIPELINE §5 was updated with a Stage-A note; CODEGEN_SPEC §5 was not. | Update `docs/spec/CODEGEN_SPEC.md` §5 to note the second shipped backend (thin three-address IR, `enable_ir_backend`, Stage A 2026-07-10). The full SSA-lite + linear-scan remains deferred (ROADMAP Stage 3). |
| **LOW** | A9 | **extensions/README.md extension table omits map/opt/obf** and under-lists array/string/math APIs. | Add `map`, `opt`, `obf` rows; update `string` (add find/substr), `array` (add push_f32/i64, pop_*, clear, remove), `math` (add f64 variants + abs_i64) rows. |
| **LOW** | A10 | **README "eight extensions"** — there are nine addon extensions + two pass extensions. | Update `README.md:162` count and list (add `map`; note `opt`/`obf` are pass extensions, a separate category). |
| **LOW** | A11 | **README syntax Control list omits for-each and match** | Add `for (x in slice)` and `match (expr) { pat => body, _ => default }` to `README.md:138-139`. |
| **LOW** | B2 | **README Docs spec list omits PASS_SYSTEM_DESIGN.md and CODEGEN_OPTIMIZATION_DESIGN.md** | Add both to the README "Docs → Spec" list. |
| **LOW** | B3 | **ROADMAP match entry omits the IR-backend fallback** that for-each's entry states | Add the same `non_serializable` IR-fallback note to the match entry that the for-each entry carries. |
| **LOW** | C1 | **for-each and match absent from canonical grammar + AST docs** — `COMPILER_PIPELINE.md` §2 BNF and §3 AST node types have no `ForEachStmt`/`MatchStmt`/`MatchArm` entries | Add grammar productions and AST node entries for for-each and match to `COMPILER_PIPELINE.md` §2/§3. |
| **LOW** | C2 | **for-each and match codegen/lowering absent from CODEGEN_SPEC** | Add for-each lowering (slice {ptr,len} → while-loop-with-indexing) and match lowering (per-arm branches) sections to `CODEGEN_SPEC.md`. |
| **LOW** | C3 | **match absent from TYPE_SYSTEM** — the subject type rule ("must be integer or bool") is enforced in sema but not in the spec | Add match subject-type rule and pattern-type documentation to `TYPE_SYSTEM.md`. |
| **LOW** | C4 | **map<K,V> extension has no spec documentation** — API, i64/i64 key/value convention, and bounds behavior (invalid handle → 0, missing key → 0) undocumented in the spec layer | Add map extension API documentation to `TYPE_SYSTEM.md` or `BINDING_API.md`. |

### P3 — MEDIUM: Remaining test gaps (from AUDIT_2026-07-11_DOCS_TESTS)

| Priority | ID | Gap | Fix |
|----------|----|-----|-----|
| **MEDIUM** | D4 | No bounds-checking tests for array get/set (negative index, OOB index) — the `i>=0 && i<size` bounds contract is unverified | Add negative-index and OOB-index cases to `ext_registration_test.cpp` or a new `ext_array_bounds_test.cpp`. |
| **MEDIUM** | D5 | No bounds-checking tests for string `char_at`/`substr` OOB — the clamping/null-return bounds contract is unverified | Add OOB char_at and substr (negative start, start≥length, negative len, len past end) cases. |
| **MEDIUM** | D7 | LICM tested only for value-preservation, not for actually hoisting anything — a regression where LICM silently hoists nothing would pass | Add an assertion that the invariant `100*200` is hoisted out of the loop (instr count in the loop body decreases; or a direct check that the hoist moved the instr to the pre-header). |
| **LOW** | D6 | No IR optimization pass tests for no-op / empty-function paths — the `Preserved::all()` returns are unverified | Add a pass-on-`fn main()->i64 { return 7; }` test (should return `Preserved::all()`, change nothing) + an empty-body function test + a side-effecting-only function test (DCE/CSE must not remove). |
| **LOW** | D8 | No .em v5 mixed-mode test (some IR, some raw-x86 functions in one module) — the loader explicitly supports this but it's untested | Add a v5 module with multiple functions where some have `is_ir=1` and some have `is_ir=0`; assert both paths load + execute correctly. |
| **LOW** | D9 | No test that for-each/match functions correctly fall back to the tree-walker under `enable_ir_backend` — if the fallback regressed, no test would catch it | Add a test that compiles a for-each/match function with `enable_ir_backend=true` and asserts the fallback produces the correct result (no crash, no silent miscompile). |

### P4 — MEDIUM: Spec-vs-implementation drift (from PENDING_FEATURES §4, D1–D8)

| Priority | ID | Drift | Fix |
|----------|----|-------|-----|
| **MEDIUM** | PF-D1 | **`constexpr` keyword lexes but has no parser/sema support** — using it is a parse error. The spec claims it works (TYPE_SYSTEM §11.5: "`constexpr` ... usable as array sizes, switch case labels"; COMPILER_PIPELINE §1/§6). `const N: u32 = 256; let buf: [u8; N];` is a parse error. | **Either** implement the restricted `constexpr` form (resolve a named `const` Ident to its literal value at array-size/case-label positions — small extension to the existing `try_eval_const_i64` folder), **or** remove the `constexpr`-works claims from the specs. The spec must stop claiming behavior that doesn't exist. **Recommend implementing** — it's the narrow form the spec already claims, not full const-eval. |
| **LOW** | PF-D2 | Spec says "arr[i] requires unsigned integer type" — implementation accepts any signed or unsigned int | Decide: enforce unsigned-only in sema (matching the spec) or relax the spec to match the implementation (any int). The implementation (any-int) is more ergonomic; recommend relaxing the spec. |
| **LOW** | PF-D3 | Spec claims integer literal width suffixes (`42u`, `42i64`) — lexer rejects them | Remove the int-suffix claim from `COMPILER_PIPELINE.md` §1 (keep the float `f`-suffix note). |
| **LOW** | PF-D4 | Spec says "no raw strings" — `r"""..."""` triple-quoted raw strings ship | Add raw strings to `COMPILER_PIPELINE.md` §1 + `TYPE_SYSTEM.md` §6. |
| **LOW** | PF-D5 | `MEMORY_AND_GC.md` §6 says "no distinct string type" — the owned `string` handle ships | Update §6 to reflect the owned `string` handle (or note the two representations: `slice<u8>` literal vs owned `string` handle). |
| **LOW** | PF-D6 | `GAP_ANALYSIS.md` §2 says "Default args ✗ v1" — default args ship | (May have been fixed in the `2223b3a` GAP_ANALYSIS refresh — verify.) |
| **LOW** | PF-D7 | `GAP_ANALYSIS.md` §3 says "no map extension; array pop/clear/remove deferred" — all shipped | (May have been fixed in the `2223b3a` GAP_ANALYSIS refresh — verify.) |
| **LOW** | PF-D8 | `COMPILER_PIPELINE.md` §2 BNF is stale — omits `match`/`for-each`/`enum`/`fn`-type/`&fn`/`link`/`priv`/array literals/`=>`/`::` | Refresh the BNF to include all shipped v1.0 forms. (Note: PF-D8 overlaps with docs-tests C1 — both are about the stale grammar doc.) |

### P5 — MEDIUM: Performance — IR optimization passes (from LLVM_ADDITIONAL_PASSES_RESEARCH)

The performance audit established that the IR backend is uniformly 1.2–1.9× slower than the
tree-walker (the per-instruction store-to-frame round-trip). The LLVM pass research identified
store-to-load forwarding as the single highest-impact pass to address this. These are
**optimization-gated** (DESIGN §9: "no speculative optimization before the bench proves it
matters") — but the bench already proves the IR backend is slower, so the first pass is
justified.

| Priority | ID | Pass | Complexity | Impact | Notes |
|----------|----|------|-----------|--------|-------|
| **MEDIUM** | LLVM-1 | **Store-to-load forwarding (intra-block)** — EarlyCSE shape. Eliminates the load half of the frame round-trip. `LoadFrame dst=vD off=X` → `Move dst=vD src1=vN` when a `StoreFrame src1=vN off=X` is the last writer. The emit's `rax_vreg` cache then keeps the result in rax (no frame load). | 4/10, ~80 lines | **HIGH** on loop_overhead, int_div, cse_redundant, licm_invariant | The direct fix for audit Finding 2.1. Highest impact-per-effort. No analysis needed (intra-block linear walk). |
| **MEDIUM** | LLVM-2 | **Copy propagation (intra-block)** — the partner pass to LLVM-1. Forwarding creates `Move`s; copy propagation replaces uses of the `Move`'s dst with its src; DCE removes the dead `Move`. | 3/10, ~70 lines | LOW standalone, **HIGH as cleanup** for LLVM-1 | Pipeline: `constprop,forward,copyprop,dce`. Without this, forwarding leaves `Move`s that still store-to-frame. |
| **LOW** | LLVM-3 | **Emit fix: shl-by-imm8 short form** — `thin_emit.cpp` Shl/Shr case emits `mov rcx,imm; shl rax,cl` (13 bytes) instead of `shl rax,imm8` (3 bytes). | 1/10, ~10 lines | MEDIUM on shift-heavy code | Byte-level emit improvement, not an IR pass. Nearly free. |
| **LOW** | LLVM-4 | **InstCombine identity folds** — `x+0→x`, `x*1→x`, `x*0→0`, `x-x→0`, `x|x→x`, etc. | 3/10, ~60 lines | LOW on bench, MEDIUM on real scripts | Infrastructure for real scripts (the bench is too clean to show the win). |
| **LOW** | LLVM-5 | **Dead store elimination (intra-block, overwritten-before-read)** — `StoreFrame off=X; StoreFrame off=X` with no intervening load → first store dead. Ember's function-wide DCE misses this. | 3/10, ~50 lines | LOW on bench, MEDIUM on real scripts | Pairs with LLVM-1 (forwarding may make stores dead). |
| **LOW** | LLVM-6 | **Branch folding + unreachable block elimination** — constant `Branch` → `Jmp`; dead-block BFS sweep. | 4/10, ~100 lines | NONE on bench, MEDIUM on real scripts | Infrastructure for scripts with dead branches. Zero bench impact. |
| **DEFERRED** | LLVM-7 | **Dominator tree analysis** — the foundation for inter-block versions of LLVM-1/2/5. | 5/10 | Enables inter-block passes | Build when intra-block passes are stable and the bench shows inter-block forwarding would help. |
| **DEFERRED** | LLVM-8 | **Inter-block store-to-load forwarding** — needs dominator tree (LLVM-7). | 7/10 | HIGH on inter-block store-load paths | Needs LLVM-7 first. |

### P6 — LOW: Performance — compile-time O(n²) patterns (from PERFORMANCE_AUDIT)

| Priority | ID | Finding | Fix |
|----------|----|---------|-----|
| **LOW** | Perf-3.1 | ConstProp re-scans the entire function on every instruction removal — `compute_used_vregs` + `compute_read_slots` recomputed from scratch per erase → O(n²) per fixpoint iteration | Make incremental: a worklist of "newly dead VRegs/slots" checked against a static use map (computed once, updated on removal). Or cache as a `ThinUseSet` analysis in the `EmberAnalysisManager`. |
| **LOW** | Perf-3.2 | CSE scans the entire hash table on every VReg redefinition + scans all following instructions on every rematch → O(n²) per block | Use a scoped hash table (per-block, pops on block exit) or a VReg-keyed kill map instead of a full table scan. |
| **LOW** | Perf-5.1 | Bench harness missing: (a) IR-engine axis in output [partially done — CSV has `engine` column], (b) load-time measurement, (c) large-function scaling test, (d) allocation/GC pressure, (e) float-heavy path, (f) native-call-overhead path, (g) i32 `normalize_rax` cost | Add the missing bench axes. The IR-engine column exists in the CSV header; verify it's populated. The large-function path is the most important (exposes the O(n²) compile-time patterns). |

### P7 — LOW: Correctness potential issues still open (from AUDIT_CODE_CORRECTNESS)

| Priority | ID | Issue | Notes |
|----------|----|-------|-------|
| **LOW** | P1 | `Type::byte_size()` returns 8 for any struct-named type; the post-switch `if (!struct_name.empty()) return 0;` is dead code. Latent — every caller routes through `StructLayoutTable` lookups instead. | Flag for the dead branch + the footgun. Low risk; fix when a future caller trusts `byte_size()` on a struct. |
| **LOW** | P3 | Sema does not flag unreachable code — `return 1; return 2;` compiles (returns 1, second return is dead but emitted). | Documented v1 scope choice (no CFG/dominance analysis). Not a regression. Defer until sema gains dominance. |
| **LOW** | P4 | Cascading/duplicate sema errors on undefined names — `return undefined_var;` reports 2 errors (undefined name + return type mismatch). | Cosmetic/diagnostic only — no codegen impact (sema halts on `r.ok == false`). Low priority. |
| **LOW** | P5 | `get_entry_function` returns a pointer to a `thread_local` static that is overwritten by the next call. | Stale-pointer footgun. Single-shot use is fine. Low priority. |
| **LOW** | P7 | Lexer column tracking over-counts on a number-with-exponent (`1e10`). | Affects only error-column reporting. Minor. |

### P8 — LOW: Other ROADMAP-tracked items (from PENDING_FEATURES §7)

| Priority | Item | Notes |
|----------|------|-------|
| **LOW** | `ember test` CLI subcommand (CLI Family A) | Replace the bash harness with a fast, parallel, TAP-ish runner. Blocked on extracting the compile-to-entry flow from `main` into a reusable helper. The only MISSING (not deferred) ROADMAP item. |
| **LOW** | `string_format` / richer formatting (P5 in PENDING_FEATURES) | Add width/precision/hex control. The f-string pipeline already threads through `__fstring_to_string` → `string_from_*`. Medium cost. |
| **LOW** | Broaden `math` — f32 floor/ceil/abs/pow, atan2/log/exp, min/max/clamp (P6 in PENDING_FEATURES) | Pure host C++ additions to `extensions/math/`. Low cost. |
| **LOW** | Restricted `constexpr` (P4 in PENDING_FEATURES) | If PF-D1 chooses "implement" rather than "remove the claim": wire `Kw_constexpr` into parser + sema for named const as array size / case label. Small extension to `try_eval_const_i64`. (Overlaps with P4/PF-D1 above.) |

---

## 2. DEFERRED Items (explicitly deferred, do NOT build until trigger fires)

These are ROADMAP-tracked deferrals with named re-entry triggers. They are not pending
action — they are deliberate scope decisions.

| Item | Re-entry trigger | Source |
|------|-----------------|--------|
| Classes + single inheritance + vtables (Tier 3) | A real game has >1 entity types sharing a base interface | ROADMAP Tier 3 |
| Interfaces / mixins / templates (Tier 3) | Classes land | ROADMAP Tier 3 |
| Tracing GC / `new` / `delete` / lambdas with capture (Tier 4) | Tier 3 forces it | ROADMAP Tier 4 |
| Coroutines / `yield` (Tier 5) | Real demand; dep GC | ROADMAP Tier 5 |
| Exceptions `try`/`catch`/`throw` (Tier 5) | Real demand | ROADMAP Tier 5 |
| In-context threads / `thread` addon (Tier 5) | Non-goal; defer as long as possible | ROADMAP Tier 5 |
| Namespaces (Tier 6) | Module size makes flat scope crowded | ROADMAP Tier 6 |
| `static_assert` (Tier 1) | `constexpr` evaluation broadened | ROADMAP Tier 1 |
| Full `constexpr` function evaluation (Tier 1) | Post-v1 | ROADMAP Tier 1 |
| Codegen Stage 2 full + Stage 3 (SSA-lite + linear-scan) | Stage A insufficiency / cross-block evidence | ROADMAP codegen candidate |
| Slice-escape Stage 2 (C3 native-arg + C5 script-fn-arg) | Needs borrow/escape analysis + `NativeSig::borrows`/`retains` | ROADMAP investigation candidate |
| CLI Family C (`ember pipe`, `ember live`) | Demo ergonomics prove the APIs are pleasant enough | ROADMAP CLI |
| `auto` removal | After a grace period | ROADMAP slated-for-removal |
| Bare-`fn` parameterized function types (`fn(i64)->i64`) + cross-module fn handles | v2+ | ROADMAP Tier 2 |
| Preprocessor / `goto` / multiple inheritance / self-hosting | **NEVER** (hard non-goals) | ROADMAP |
| Standalone exe bundler (`ember bundle`) | Planning complete (`plan_STANDALONE_BUNDLER.md`); implement when a concrete distribution need fires | `plan_STANDALONE_BUNDLER.md` |
| `spawn`/`proc_status`/`proc_wait` (async subprocess) | Ship `exec` first; `spawn` if a concrete use needs non-blocking subprocess | `plan_OS_IO_EXTENSIONS.md` §6.5 |
| LLVM inter-block passes (LLVM-7, LLVM-8) | Dominator tree analysis built + bench shows inter-block forwarding would help | `LLVM_ADDITIONAL_PASSES_RESEARCH.md` §5 |
| `FlatteningPass` / `MBAPass` / bogus control flow (obfuscation) | Step 6 FUTURE | `PASS_SYSTEM_DESIGN.md` §8 |
| `EmberAnalysisManager` concrete analyses | When a pass needs it (first pass that needs CFG/loop analysis) | `PASS_SYSTEM_DESIGN.md` §6/§8 |

---

## 3. Consolidated Prioritized Action List

The single ordered list of what to do next. Everything above the line is actionable now;
everything below is deferred.

### Immediate (do first)

| # | Priority | Item | Effort |
|---|----------|------|--------|
| 1 | **HIGH** | **Sec-3:** Wrap vec/mat/quat `*_new` `push_back` in try/catch (OOM → null handle, not `std::terminate`) | Small (~15 lines across 3 files) |
| 2 | **HIGH** | **Sec-4:** Wrap `n_string_substr`'s `substr` call in try/catch | Small (~5 lines) |
| 3 | **HIGH** | **Sec-5:** Add mutexes to array/string/map/vec/mat/quat extension stores (match ext_sync/ext_lifecycle) | Medium (~6 files, one mutex + lock per store) |
| 4 | **HIGH** | **P1 — `io` extension:** Implement `extensions/io/` per `plan_OS_IO_EXTENSIONS.md`. Ship `register_console` (print/println/print_i64 ungated) first; file/path/dir/exec behind `PERM_FFI` + CLI `--ffi` flag. | Medium (one TU, ~500 lines; design is complete) |
| 5 | **MEDIUM** | **Sec-6:** Reject duplicate block IDs in `validate_thin_function` | Small (~5 lines) |
| 6 | **MEDIUM** | **D1:** Add `ext_map_test.cpp` ctest (map extension has zero ctest coverage) | Small (~80 lines + CMake add_test) |

### Near-term (do next)

| # | Priority | Item | Effort |
|---|----------|------|--------|
| 7 | **MEDIUM** | **A8/C5:** Document v5 IR `.em` format in `BUNDLING_AND_EM_MODULES.md` | Medium (doc section) |
| 8 | **MEDIUM** | **PF-D1:** Resolve `constexpr` spec drift — implement the restricted form OR remove the spec claims | Small-Medium (implement: ~40 lines parser/sema; remove: doc edits) |
| 9 | **MEDIUM** | **LLVM-1:** Store-to-load forwarding pass (intra-block) — the direct fix for the IR backend being 1.2–1.9× slower than the tree-walker | Medium (~80 lines) |
| 10 | **MEDIUM** | **LLVM-2:** Copy propagation pass (intra-block) — the cleanup partner to LLVM-1 | Small (~70 lines) |
| 11 | **MEDIUM** | **D4/D5:** Array + string bounds-checking tests (negative/OOB index, OOB char_at/substr) | Small (~60 lines) |
| 12 | **MEDIUM** | **D7:** LICM hoist-verification test (assert the invariant actually moves, not just value-preservation) | Small (~30 lines) |
| 13 | **MEDIUM** | **A6/A7:** Fix stale SPEC_AUDIT F2 + CODEGEN_SPEC §5 (thin-IR backend shipped) | Small (doc edits) |

### When bandwidth allows (low risk, low urgency)

| # | Priority | Item | Effort |
|---|----------|------|--------|
| 14 | **LOW** | **A9/A10/A11/B2/B3:** README + extensions/README refresh (map/opt/obf rows, extension count, control list, spec doc list, match IR-fallback note) | Small (doc edits) |
| 15 | **LOW** | **C1/C2/C3/C4:** for-each + match + map in canonical grammar/AST/codegen/type-system specs | Small-Medium (doc additions) |
| 16 | **LOW** | **PF-D2–D8:** Remaining spec-vs-impl drifts (unsigned-index, int-suffixes, raw-strings, string-type, default-args, map/addons, BNF) | Small (doc edits) |
| 17 | **LOW** | **D6/D8/D9:** IR pass no-op tests, v5 mixed-mode test, for-each/match IR-fallback test | Small (~100 lines total) |
| 18 | **LOW** | **LLVM-3:** Emit fix: shl-by-imm8 short form in `thin_emit.cpp` | Small (~10 lines) |
| 19 | **LOW** | **LLVM-4/5/6:** InstCombine identity folds, intra-block DSE, branch folding | Small-Medium (~210 lines total) |
| 20 | **LOW** | **Perf-3.1/3.2:** Make ConstProp + CSE incremental (fix O(n²) compile-time) | Medium |
| 21 | **LOW** | **Perf-5.1:** Add bench axes (load-time, large-function, float-heavy, native-call) | Medium |
| 22 | **LOW** | **P1/P3/P4/P5/P7:** Remaining correctness potential issues (dead branch in `byte_size`, unreachable-code detection, cascading sema errors, `get_entry_function` thread_local static, lexer exponent column) | Small each |
| 23 | **LOW** | **P8:** `ember test` subcommand, `string_format`, broader `math`, restricted `constexpr` (if PF-D1 chooses implement) | Small-Medium each |

### Deferred (do NOT build until the ROADMAP trigger fires)

Classes/interfaces/mixins/templates, GC/new/lambdas, coroutines/yield, try/catch/throw,
in-context threads, namespaces, static_assert, full constexpr fn eval, codegen Stage 2 full +
Stage 3, slice-escape Stage 2, CLI Family C, `auto` removal, bare-fn parameterized types +
cross-module fn handles, standalone exe bundler, `spawn`/`proc_*` async subprocess, LLVM
inter-block passes (needs dominator tree), `FlatteningPass`/`MBAPass`, `EmberAnalysisManager`
concrete analyses. Preprocessor / goto / multiple inheritance / self-hosting = **never**.

---

## 4. Summary

**What's done:** All 6 correctness defects (C1–C6), both critical/high security findings (S1/S2),
2 correctness potential issues (P2, P6), 6 stale doc findings (A1–A5, B1), 2 inaccurate doc
findings (B1; A1/A2 which were the pass-system spec layer), and 2 test gaps (D2 nested for-each,
D3 match-no-wildcard). The tree is green: ctest 34/34, lang suite 274/0/0.

**What remains (HIGH priority):**
1. **4 security findings** (Sec-3/4/5/6) — OOM-terminate in vec/mat/quat/string_substr, extension
   store thread-safety, duplicate block ID validation. All verified still present in source.
2. **The `io` extension** (P1) — the single biggest day-to-day gap; design is complete, implementation
   is not.

**What remains (MEDIUM priority):**
3. **Doc/test gaps** — map ctest (D1, highest-impact test gap), v5 IR format docs (A8/C5), stale
   SPEC_AUDIT + CODEGEN_SPEC (A6/A7), `constexpr` spec drift (PF-D1).
4. **IR optimization passes** — store-to-load forwarding (LLVM-1) + copy propagation (LLVM-2) are
   the headline pair to make the IR backend competitive with the tree-walker.
5. **Bounds-checking tests** (D4/D5) + **LICM hoist verification** (D7).

**What's deferred:** All ROADMAP Tier 3–6 features, codegen Stage 2/3, slice-escape Stage 2,
standalone bundler, async subprocess, LLVM inter-block passes, and the remaining obfuscation
passes — each with a named re-entry trigger that hasn't fired.

---

*Audit method: read-only. Every claim verified against the git log (`git log --oneline -30`),
the source files (`src/`, `extensions/`), and the cited audit docs. No source files were edited.
This document is the consolidated action list; the individual audit docs remain the detailed
record.*

---

## 5. Post-dating update — 2026-07-13 (Finding C closed; appended, historical context preserved)

**This section is a post-dating append.** Everything above is the 2026-07-11 read-only
synthesis at revision `5df97f2` and is left intact as the dated historical record. This
addendum records a later closure so the canonical todo record stays current; it does not
rewrite any 2026-07-11 content.

### Selected item implemented: Finding C — deserialized Thin IR rbp-relative full-span validation

The 2026-07-11 synthesis above lists **S1 (CRITICAL)** — the v5 IR `frame_off` validation gap
(return-address overwrite) — as **FIXED** at `d25cc8c` (§0.2), noting the fix "now checks
`frame_off != 0` for ALL instructions." That fix closed the **base-offset** path of Finding A.
`docs/audit/FINAL_EM_REDTEAM_2026-07-11.md` §4 then identified the **sibling hole, Finding C
(HIGH→CRITICAL)**: the frame-plan offsets (`rbx_save_offset`, `struct_ret_ptr_offset`,
`params[].off`) and the actual read/write **span** (not just the base offset) at `[rbp + off]`
were still unvalidated — the same return-address-overwrite primitive via sibling offsets and
short-negative bases (e.g. `frame_off = -1` writes 8 bytes to `[rbp-1, rbp+7)`, overwriting
saved rbp / the return address even though `-1` is in `[-frame_size, 0)`). Finding C was the
selected deferred item; it is now **CLOSED** across two focused cycles:

| Half | Commit | Scope |
|------|--------|-------|
| Frame-plan full-span | `3a2a804` (2026-07-13 14:00 EDT) | `rbx_save_offset` / `struct_ret_ptr_offset` / `params[].off` full-span validation against `[-frame_size, 0)` (`frame_plan_span_ok` helper); low-end bound always applied so `frame_size == 0` rejects every non-zero offset (Finding A cases A1/A2 preserved). |
| Per-instruction full-span + args + data_temp | `235b8a6` (2026-07-13 15:30 EDT) | Per-op full-span validation of every rbp-relative `frame_off` / `field_off` / `data_temp_off` access (`instr_frame_span_ok` + `dst_spill_span`, per-op 1/2/4/8/16-byte widths derived from exact `thin_emit.cpp` behavior); `arg_frame_offs[]` validation; computed-address ops (`StoreAddr`, `StoreFrame src2!=0`, `IndexAddr`, `MakeSlice`) EXCLUDED (displacement from a runtime pointer, not an rbp-relative frame access). |

**Shipped validation behavior:** `validate_thin_function` now rejects any deserialized v5 IR
blob whose rbp-relative frame-plan OR per-instruction offset has a read/write span crossing
either frame boundary, BEFORE re-emit and BEFORE executable page allocation. Computed-address
displacements are distinguished from rbp-relative frame accesses and are not wrongly rejected.

**Test coverage shipped:**
- `examples/thin_ir_ser_test.cpp` — Part 4 (7 Finding C frame-plan cases, c1) + Part 5
  `frame_span_arg_validation` (19 cases: 7 full-span malformed + 5 legitimate negative
  controls + 3 computed-address distinction + 4 `arg_frame_offs`, c2). Parts 1-5 all green.
- `examples/em_v5_ir_test.cpp` — case (f): loader-level proof a span-crossing `frame_off`
  ir_blob is rejected by `validate_thin_function` BEFORE any executable page is allocated
  (`!ok && pages.empty()`).

**Full verification status (post-c2, HEAD `235b8a6`):** `cmake --build buildt -j 8` → exit 0;
`ctest --test-dir buildt --timeout 120` → **70/70 PASS** (incl. bench + soak), no regressions
vs. the pre-fix baseline; `ember_cli.exe run tests/lang/optimization_validation.ember --fn main
--passes constprop,forward,copyprop,instcombine,dce,licm,dse` → **exit 177** (the required
sentinel). Commit: see git log (`3a2a804`, `235b8a6`).

**Residual limitation (not claimed broader than implemented):** the `StructLitInit` /
`ArrayLitInit` combined-offset (`frame_off + field_off`) full-span check is included for
defense-in-depth, but those ops only appear in `non_serializable` (`is_ir = 0`) functions
that never reach `validate_thin_function` at load time under the secure default, so that
combined-offset path is not directly test-exercised beyond the existing suite. No known gap
remains in the per-instruction rbp-relative frame-access validation surface itself. The
protected serialization boundaries (`src/thin_ir.hpp` `ThinOp` enum, `src/em_file.hpp` format
comments) and `thirdparty/` were NOT modified.
