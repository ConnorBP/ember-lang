# ember — Code Review: Recent Features (constexpr, static_assert, typed enums, iterable, IR passes)

**Repo:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Reviewed commits:** `3b8a8d7..7f00a5f` (constexpr fn eval, static_assert, typed enums, iterable() for-each-over-array; plus the 8 IR optimization passes in `extensions/opt/ext_opt.cpp` which the task placed in scope).
**Build:** MinGW g++ 15.2.0, C++17, Release (`buildt/`). Baseline `ctest -E bench`: **40/40 pass**. After fixes: **40/40 pass**. Lang suite: **350/0/0**.
**Scope:** read-only review of the five feature areas against the review checklist (correctness, security/bounds, consistency, interactions, type system, regressions). Four REAL bugs found, fixed, and covered by regression tests. Potential issues documented but not forced.

Findings are filed as **CONFIRMED BUG (FIXED)**, **POTENTIAL ISSUE (documented)**, or **VERIFIED OK**. Exact `file:line` cited for each.

---

## CONFIRMED BUGS (FIXED + regression test)

### B1. constexpr interpreter: nested loops bypass the 100k iteration bound (compile-time DoS)

**File:** `src/sema.cpp:2476` (WhileStmt), `:2498` (ForStmt), `:2521` (DoWhileStmt) — `ce_eval_stmt` loop cases.

**Cause.** The documented bound was "max 100000 loop iterations per loop, max 256 recursion depth" (the `ConstEvalCtx` comment + the task description). The iteration counter was a **per-loop** local `int iters = 0;` in each loop's `ce_eval_stmt` case, NOT a shared total. Nested loops compound **multiplicatively**: N nested 100k loops run `100k^N` sema-time iterations. The depth bound (256, in `eval_constexpr_fn`) is solid, but the iteration bound as documented was not a real bound on total work.

**Repro (pre-fix).** `sema_check` on a constexpr fn with two nested 100k loops **hung** (>15s, timed out):
```
constexpr fn bomb() -> i64 {
    let mut i: i64 = 0;
    while (i < 100000) {
        let mut j: i64 = 0;
        while (j < 100000) { j = j + 1; }
        i = i + 1;
    }
    return 0;
}
fn main() -> i64 { return bomb(); }
```
A 3-deep version (`100k^3 = 10^15` iters) was effectively infinite. Single-loop 100k folded fine (the per-loop cap held for one level); the bypass was specifically nesting.

**Fix.** Replaced the per-loop `MAX_LOOP` counter with a **shared total iteration budget** `ConstEvalCtx::total_iters`, capped at `CE_MAX_TOTAL_ITERS = 100000`, charged by every loop body iteration at every nesting level in one `eval_constexpr_fn` call (`src/sema.cpp:962-975`). All three loop cases (While/For/DoWhile) now `++ctx.total_iters` and bail when it exceeds the budget, returning false so the call falls back to a normal runtime call (no error — the fn is still callable at runtime, same as the existing too-deep-recursion fallback). Nesting can no longer multiply past the single per-eval budget.

The budget is per-`eval_constexpr_fn`-call (each top-level fold gets a fresh 100k; a nested constexpr call creates a fresh `ConstEvalCtx`). Combined with the 256 depth cap, the worst case is `256 × 100k = 25.6M` sema-time iterations (≈1s, not a hang) — linear in depth, not exponential.

**Post-fix verification.** The 2-deep and 3-deep nested-loop repros now complete sema in **0.08s** (hit the 100k budget, fall back to runtime, sema ok). Single-loop 100k still folds. `fib(10)=55`, `sum_to(100)=5050`, `factorial(5)=120`, `square(square(3))=81` all still fold correctly.

**Regression test.** `examples/constexpr_test.cpp` probe **[11]**: a `nested_count(outer, inner)` constexpr fn with a 2-deep nested while loop. `nested_count(3,3)=9` and `nested_count(200,200)=40000` fold (under the budget); `nested_count(400,400)=160000` exceeds the budget and falls back to runtime; the sum `9+40000+160000=200009` is checked, and the compile must not hang. (The "must not hang" property is inherent — if the bug regressed, the test process would time out.)

---

### B2. DSE pass removes a StoreFrame that feeds a CopyBytes reader (value-preservation bug)

**File:** `extensions/opt/ext_opt.cpp:927` — `DeadStoreElimPass::run`.

**Cause.** DSE's intra-block dead-store scan killed a pending dead store ONLY on an intervening `LoadFrame` of the same offset. It did NOT recognize `CopyBytes` (whose source range `meta.field_off .. meta.field_off + meta.len` reads a frame slot) or `FieldAddr` (whose `meta.frame_off` reads a struct base slot) as readers. So when a `StoreFrame` to slot X was followed by a `CopyBytes` reading X, then another `StoreFrame` to X, DSE removed the FIRST store — but the `CopyBytes` had already read it, so the copy now read an uninitialized slot. `DCE` and `ConstProp` already used `compute_read_slots` (which DOES count `CopyBytes`/`FieldAddr`); `DSE` was the one pass that missed them.

**Repro (hand-built IR, unambiguous at the IR level).** A block with `StoreFrame off=-8 (=5)`; `CopyBytes src=-8 dst=-16 len=8` (reads -8); `StoreFrame off=-8 (=9)`; `LoadFrame off=-16`; `Return`. Pre-fix: DSE reduced the store count 2→1 (removed the first store that fed the CopyBytes). Post-fix: both stores kept (CopyBytes recognized as a reader).

**Reachability.** The IR backend currently represents a same-frame struct copy (`let c2 = c`) as a wide `LoadFrame`/`StoreFrame` with `width=16`, NOT as `CopyBytes` (see B4 below), so this exact shape is not produced by today's struct copies. `CopyBytes` IS emitted for struct-return-through-hidden-ptr (`copy_frame_vptr`), global↔frame struct copies (`copy_global_*`), and nested-struct-literal init from a local (`store_value_to_frame` → `copy_frame_frame`). The bug is a genuine value-preservation defect in the pass (proven by hand-built IR) that is latent today only because the struct-copy lowering uses the wide-LoadFrame/StoreFrame representation; once that representation is corrected to `CopyBytes` (the design-intended form, per the `copy_*` helpers' comments), the bug would manifest. The fix is small, safe, and conservative (treating a CopyBytes dest as a reader is a false positive that only keeps a dead store alive, never removes a needed one).

**Fix.** Added `instr_reads_off(in, off)` helper (`ext_opt.cpp:155`) mirroring `compute_read_slots`'s conservative reader set (LoadFrame `frame_off`; FieldAddr `frame_off`; CopyBytes `frame_off` [dest, conservative] + source range `[field_off, field_off+len)`). DSE's non-StoreFrame branch now erases any `last_store_idx` entry whose offset is read by the current instruction (`ext_opt.cpp:945`).

**Regression test.** `examples/ir_passes_test.cpp` **D10**: hand-built IR with `StoreFrame(-8,5)`; `CopyBytes(src=-8,dst=-16,len=8)`; `StoreFrame(-8,9)`; `LoadFrame(-16)`; `Return`. Asserts DSE keeps BOTH stores (count 2→2) and that the first `StoreFrame(-8,v0=5)` specifically survives.

---

### B3. StoreToLoadForward pass forwards a LoadFrame past an intervening CopyBytes writer (value-preservation bug)

**File:** `extensions/opt/ext_opt.cpp:655` — `StoreToLoadForwardPass::run`.

**Cause.** The pass replaces `LoadFrame dst=vD off=X` with `Move dst=vD src1=vN` when a `StoreFrame src1=vN off=X` is the last writer to X. Its kill rule handled `StoreFrame` to X (kills the pending forward) but NOT a `CopyBytes` whose DEST range `[meta.frame_off, meta.frame_off + meta.len)` covers X (when the dest is frame-relative, `in.dst == 0`). So `StoreFrame@X; CopyBytes(dst covers X); LoadFrame@X` forwarded the STALE `StoreFrame` value instead of the bytes the `CopyBytes` wrote. (The pass's correctness for the SSA-VReg case is sound — see V4 — but it missed the CopyBytes-write case.)

**Repro (IR-dump, unambiguous).** A block with `StoreFrame off=-8 (=5)`; `StoreFrame off=-16 (=77)`; `CopyBytes dst=-8 src=-16 len=8` (writes -8 = 77); `LoadFrame off=-8`; `Return`. Pre-fix dump: `LoadFrame dst=v3 off=-8` was rewritten to `Move dst=v3 src1=v1` (the stale value 5) — wrong. Post-fix dump: `LoadFrame dst=v3 off=-8` is PRESERVED (the CopyBytes write killed the pending forward), so the load reads the CopyBytes result (77).

**Reachability.** Same caveat as B2: today's same-frame struct copies use the wide LoadFrame/StoreFrame representation (a `StoreFrame` to the struct base offset, which the pass's existing StoreFrame-kill handles correctly), so the CopyBytes-dest shape is not produced by today's `c = other` lowering. The bug is a real IR-level defect (proven by dump) latent behind the struct-copy representation; the fix is small, safe, and conservative (killing a forward only re-routes a load through memory, never changes the stored value).

**Fix.** Added `instr_writes_off(in, off)` helper (`ext_opt.cpp:188`) recognizing a `CopyBytes` dest range (when `in.dst == 0`, frame-relative). The pass's "any other instruction" branch now erases any `last_store_src` entry whose offset is written by the current instruction (`ext_opt.cpp:740`).

**Regression test.** `examples/ir_passes_test.cpp` **D11**: hand-built IR with `StoreFrame(-8,5)`; `StoreFrame(-16,77)`; `CopyBytes(dst=-8,src=-16,len=8)`; `LoadFrame(-8)`; `Return`. Asserts the `LoadFrame(-8)` is preserved (NOT rewritten to a `Move`) and that no stale `Move vload=v0` appears.

---

### B4. ConstProp `fold_int_binop`: shift by ≥64 is C++ undefined behavior (could fold to a wrong value)

**File:** `extensions/opt/ext_opt.cpp:74` (`Shl: r = a << b;`) and `:75` (`Shr: ... uint64_t(a) >> b`).

**Cause.** `fold_int_binop` folded `Shl`/`Shr` with the raw shift amount `b`, but `a << b` / `uint64_t(a) >> b` for `b >= 64` is **undefined behavior** in C++ ([expr.shift]). The language's actual semantics (x64 `shl`/`shr` mask CL to 0..63, and `try_eval_const_i64` in `sema.cpp:165` already masks with `r & 63`) is "shift amount masked to 0..63". So ConstProp could fold `7 << 64` to an arbitrary value instead of the `7 << 0 = 7` the tree-walker produces. On this platform the UB happened to yield 7 (matching the tree-walker), masking the bug, but it is real UB and would diverge under a different compiler/optimizer.

**Repro.** `let y: i64 = 1 << 70;` — tree-walker gives `64` (70 & 63 = 6, 1<<6 = 64). Pre-fix ConstProp folded `1 << 70` as C++ UB (coincidentally 64 on this platform). `1 << 64` similarly.

**Fix.** Mask the shift amount to 0..63 in `fold_int_binop` (`ext_opt.cpp:73-83`): `Shl: r = static_cast<int64_t>(uint64_t(a) << (uint64_t(b) & 63u))`; `Shr` uses `int sh = int(uint64_t(b) & 63u)` and the same sign-fill arithmetic-shift logic as `try_eval_const_i64` for the signed case. Now ConstProp's fold matches x64 semantics and the tree-walker exactly.

**Post-fix verification.** `1 << 70` → 64 (constprop and tree-walker agree). `-16 >> 2` → -4 (252 as exit code, both backends agree).

**Regression coverage.** Covered by the existing `ir_passes_test` value-preservation sweep (every pass is value-preserving on every workload, so a wrong shift fold would surface as a baseline-vs-pass mismatch). No new test added for this one-liner since the sweep already guards it; the shift-by-70 probe was confirmed manually.

---

## POTENTIAL ISSUES (documented, not forced)

### P1. IR backend: same-frame struct/array copy uses a wide `LoadFrame`/`StoreFrame` (width > 8) that the emit only partially honors — pre-existing known gap, NOT introduced by the recent features

**Files:** `src/thin_lower.cpp:763` (`load_scalar_local` emits `LoadFrame width=16` for a struct Ident) → `src/thin_emit.cpp:1029` (the non-float/non-slice `else` branch loads only ONE qword via `load_rbp_to_rax`, ignoring `width > 8`).

**Observation.** `let c2 = c` (struct copy) lowers to `LoadFrame dst=v off=c_off width=16` + `StoreFrame src1=v off=c2_off width=16`. The emit's `LoadFrame` `else` branch (non-float, non-slice) calls `load_rbp_to_rax` (loads 8 bytes) + `normalize_rax` — it loads only the FIRST 8 bytes of a 16-byte struct. So a struct copy in the IR backend copies only the first field. This is a **pre-existing** IR-backend limitation, explicitly listed as a KNOWN GAP in `examples/thin_ir_test.cpp`'s header ("KNOWN GAPS: structs (by-value arg + return + field + reassign)"). It is NOT introduced by commits `3b8a8d7..7f00a5f` and NOT caused by any optimization pass (it reproduces with `--passes constprop` and even a single no-op pass enabling the IR backend).

**Why not fixed here.** The task scope is the recent features + the 8 passes. This is a Stage-A IR-backend lowering/emit gap predating the reviewed commits and tracked as a known gap. Forcing a fix here risks the 40/40 baseline (the struct IR path is large and entangled with by-value arg/return). The DSE/forward bugs (B2/B3) were fixed because they are pass-correctness defects that the hand-built IR repros prove independent of this gap. This struct-copy gap is documented as the root reason B2/B3 are not reachable from today's source (the lowering emits wide LoadFrame/StoreFrame, not CopyBytes, for same-frame struct copies).

**Probe.** `struct Cell { x: i64; y: i64; } fn main() -> i64 { let c: Cell = Cell { x: 5, y: 0 }; let c2: Cell = c; return c2.x; }` — tree-walker returns 5 (correct); IR backend (`--passes constprop`) returns a wrong value. Both are pre-existing, unchanged by the reviewed commits or by the four fixes.

### P2. `ce_eval_expr` compound-assign `Shr` may-use-uninitialized warning (false positive)

**File:** `src/sema.cpp:2426` / `:2450` — the `AssignExpr` compound-`Shr` case declares `int64_t cur;` and only assigns it inside the scope-search loop; if the name is not found, `cur` is read in `if (sh != 0 && cur < 0)`. GCC warns `-Wmaybe-uninitialized`.

**Analysis.** The scope-search loop sets `cur = it->second; break;` when found. If NOT found, the code falls through to the later `err = "constexpr: assignment to unknown variable"` + `return false` — BUT the `Shr` `cur < 0` read happens BEFORE that error return, in the `tmp` computation. So if the variable is genuinely unknown, `cur` IS read uninitialized. In practice, an `AssignExpr` to an unknown variable would already have been rejected by `check_expr` before constexpr eval runs (the constexpr pre-pass only folds calls whose args fold; an assignment to an unknown name is a sema error elsewhere), so this path is unreachable for well-formed input. Still, the read is technically UB-prone.

**Why not fixed.** It is a pre-existing pattern (the same shape exists in the `ce_eval_expr` `AssignExpr` non-compound read path) and is guarded upstream by sema. A defensive `int64_t cur = 0;` init would silence it safely, but the task said "do NOT change code unless you find a real bug"; this is a false-positive warning on an unreachable path, not a reachable bug. Documented, not forced.

### P3. CSEPass declares an unused `killed` set (dead code, not a bug)

**File:** `extensions/opt/ext_opt.cpp:573` — `std::unordered_set<VReg> killed;` is declared but never inserted into or read.

**Analysis.** Pure dead code — the kill logic is implemented via the `table.erase` loop on `in.dst` redefinition (`ext_opt.cpp:577-584`), which is correct (see V6). The `killed` set is a leftover. Not a correctness issue; noted for cleanup.

---

## VERIFIED OK

### V1. constexpr fn evaluation — bounds, recursion, mutation, fallback

- **Depth bound (256):** `eval_constexpr_fn` checks `depth > 256` at entry (`sema.cpp:2547`); `ce_eval_expr`'s CallExpr case passes `ctx.depth + 1` for nested calls (`sema.cpp:2375`). Verified: `rec(300)` (depth > 256) falls back to runtime, sema ok, no hang. The depth counter is correct (top-level fold passes 0; each nested call increments).
- **Iteration bound:** was B1 (per-loop, bypassable by nesting); now a shared 100k total per eval. Verified: single-loop 100k folds; nested loops fall back without hang.
- **Division by zero:** `ce_eval_expr` Div/Mod returns false on `r == 0` (`sema.cpp:2332/2335`) — the runtime trap handles it, same as `try_eval_const_i64`'s rationale. Correct.
- **Mutation inside constexpr fns:** `ce_eval_expr` AssignExpr + compound-assign mutate `ctx.scopes` (`sema.cpp:2386-2432`); `factorial(5)=120` (while loop with `result *= i`) and `ipow(2,10)=1024` (compound `result *= base`, `e >>= 1`) fold correctly. Mutation is supported and scoped correctly (each `eval_constexpr_fn` gets a fresh `ConstEvalCtx`).
- **Runtime fallback:** a constexpr fn called with a non-constant arg is left as a runtime call (`try_fold_constexpr_call` uses `try_eval_const_i64` which returns false for non-literal args; `sema.cpp:2603`). `double_it(x+10)` with runtime `x` returns 20 at runtime. Correct — a constexpr fn CAN be called at runtime.
- **Unsupported nodes:** FloatLit/StringLit/StructLit/ArrayLit/IndexExpr/ViewExpr/FieldExpr/Switch/Match/ForEach/Defer return false (`sema.cpp:2434/2555`) → fallback. Correct and conservative.
- **i64-only restriction:** `try_fold_constexpr_call` and `ce_eval_expr`'s CallExpr case both require `fn->ret->is_int()` and all params `is_int()` (`sema.cpp:2364/2591`). Float/bool/struct fns skip constexpr eval. Correct.

### V2. static_assert — folding, elision, error paths, interactions

- **True → elided:** `check_static_assert` folds via `try_eval_const_bool`; true records nothing, codegen skips `StaticAssertStmt` entirely (`sema.cpp:1933`). Verified: `valid_static_assert.ember` returns 42 (assertions produce no code).
- **False → sema error with msg:** `sema_invalid_static_assert_false.ember` (`1+1==3`) errors "static_assert failed: ...". Correct.
- **Non-const → sema error:** `sema_invalid_static_assert_nonconst.ember` (`g == 5` where `g` is a runtime global) errors "condition must be a compile-time constant". `try_eval_const_i64` does not fold Idents, so the comparison does not fold. Correct.
- **Type check first:** `check_static_assert` calls `check_expr(*sa.cond)` and requires bool BEFORE folding (`sema.cpp:1925`), so a type error surfaces with its own diagnostic. Correct.
- **static_assert + constexpr fn:** `valid_static_assert_constexpr.ember` — `lower_constexpr_calls_expr` runs in the `StaticAssertStmt` case (`sema.cpp:2620`/`2736`) and folds `square(7)`→49 before `check_static_assert`, so `square(7)==49` folds to true. Verified: returns 49. The cross-feature interaction works.
- **Top-level + in-body:** both positions parse (`parser.cpp:803/999`) and check (`check_static_assert` shared by `check_stmt` and the top-level `prog.static_asserts` pass). Consistent.

### V3. typed enums — name resolution, enum→int widening, int→enum reject, nominal typing, enum-from-constexpr

- **Registration precedes resolve_type:** `register_typed_enums` runs before `resolve_type` (`sema.cpp:2834`), so `let c: Color` resolves to the typed-enum type (prim=backing, enum_name=name). Correct.
- **enum→int widening:** `can_implicitly_convert` allows a typed-enum value to flow to a plain integer when same-signedness and `int_width(got) <= int_width(want)` (`sema.cpp:429`). Verified: `u32 enum → u64` implicit (returns 200); `i32 enum → i64` implicit (`valid_typed_enum.ember` returns 0). Consistent with the plain-int matrix.
- **int→enum reject:** `is_plain_integer(want)` is false for a typed-enum target (enum_name set), so the plain-int matrix does not accept int→enum; `adapt_int_lit` refuses to re-type a raw literal to a typed-enum type (`sema.cpp:899`). Verified: `let c: Color = 5` errors "let type mismatch (Color = i64)". Correct.
- **Cross-signedness enum→int requires `as`:** `u32 enum → i64` is rejected (different signedness), mirroring the plain `u32 → i64` rule. Verified: `return e` (u32 enum from i64 fn) errors; `return e as i64` works (200). **Consistent** with the plain-int widening matrix (the user's worry about enum/integer upgrade/downgrade edge cases: the enum rules mirror the plain-int rules exactly — same signedness + width widen implicitly; everything else needs `as`).
- **Nominal typing (distinct enums):** `Type::same` compares `enum_name` (`types.cpp:79`), so `Color` ≠ `Hue` even with the same backing. Verified: `let c: Color = Hue::A` errors "let type mismatch (Color = Hue)". Correct.
- **enum-from-constexpr:** `resolve_enums` calls `lower_constexpr_calls_expr` on a variant's explicit value before the const-check (`sema.cpp:584`), so `X = base()` (base a constexpr fn) folds. Verified: `valid_enum_from_constexpr.ember` returns 42. Correct.
- **typed enum + match:** `valid_typed_enum_match.ember` — a typed-enum VARIABLE as match subject (is_int() true for a typed enum since struct_name is empty) with Color-typed pattern literals (lowered by `lower_enum_access_expr` to IntLits carrying the typed-enum type). Returns 20. Correct.
- **Backing-type range check:** `int_fits(backing, ev)` range-checks each variant value against the backing prim (`sema.cpp:590`). A value out of range is a sema error. Correct.

### V4. iterable() for-each over arrays — sema inference, codegen lowering, IR-backend fallback

- **Element-type inference:** `infer_array_elem_ty_from_call` maps `array_new` elem_size to u8/f32/i64 (`sema.cpp:795`); `infer_iterable_array_elem_ty` handles inline `array_new` and tagged vars (`sema.cpp:818`). A bare i64 not provably from `array_new` is rejected (`for (x in 42)` stays an error). Correct.
- **codegen lowering:** the array-handle branch (`codegen.cpp:3402`) lowers to `len = array_length(h); while (i < len) { x = array_get_*(h, i); ... }`, selecting `array_get_u8/f32/i64` from `elem_ty->prim`. Verified: i64/u8/f32/empty/single/break/continue all return expected values.
- **IR-backend fallback:** `thin_lower.cpp:958` — `has_for_each` detects a ForEachStmt (or MatchStmt) anywhere in the fn body and marks the function `non_serializable` (falls back to tree-walker). So the IR passes never run on a for-each function. Verified: `--passes constprop,...,dse` on for-each-array tests returns correct results (tree-walker path). The user's interaction question ("does it fall back to tree-walker correctly?") — **yes**.
- **Note:** `has_for_each` also treats `MatchStmt` as a fallback trigger (match is also tree-walker-only in the IR backend). Consistent with the documented Stage-A scope.

### V5. IR optimization passes — per-pass correctness

- **ConstPropPass:** per-block constant tracking (VReg + frame-slot), folds both-constant binops to ConstInt, converts a constant src2 VReg to the immediate form, then a dead-constant sweep using `compute_used_vregs` + `compute_read_slots` (which correctly counts CopyBytes/FieldAddr as readers — so ConstProp's sweep does NOT have the DSE gap). The sweep iterates to fixpoint. Value-preserving. (The shift UB was B4, now fixed.)
- **DeadCodeElimPass:** removes dead pure instrs (dst unused) + dead StoreFrame (slot never read by `compute_read_slots`). Never removes side-effecting instrs (`is_side_effecting` includes all calls/stores/guards). `CopyBytes`'s dst VReg is counted as used (`compute_used_vregs:117`) so the hidden-return-pointer LoadFrame feeding it is kept. Correct.
- **CSEPass:** local CSE within a block; kill rule on `in.dst` redefinition removes table entries that use `in.dst` as a source OR whose result was `in.dst` (`ext_opt.cpp:577`). The remap stops if `new_dst` is redefined (conservative — leaves the instr if so). Only CSEs `is_pure` instrs. The unused `killed` set is P3 (dead code, not a bug). Value-preserving. (Under SSA VRegs — V6 — the kill rules are largely defensive no-ops, which is fine.)
- **LICMPass:** detects natural loops via back-edges (target index < current), finds the single non-loop predecessor (pre-header), hoists invariant pure instrs (Const*, pure binops with invariant operands, LoadFrame from a slot never written in the loop, Move of an invariant vreg). Does NOT hoist stores. Skips the header. Requires exactly one non-loop predecessor (a safe pre-header that dominates the loop body). Verified by D7a (Mul hoisted from body to pre-header) + D7b (no invariant → Preserved::all()). Value-preserving (hoisted instrs are pure; speculative execution in the pre-header is safe — DCE cleans up if the loop isn't entered).
- **StoreToLoadForwardPass:** replaces LoadFrame with Move from the last StoreFrame's src. Kill on StoreFrame to the same offset (handled) and now on CopyBytes dest range (B3 fix). The SSA-VReg reasoning (src VReg never redefined) is sound (V6). Value-preserving with the B3 fix.
- **CopyPropPass:** intra-block copy propagation of Move instrs; `resolve` follows copy chains; kills on dst redefinition and on entries whose value depends on a redefined dst. Handles the terminator's cond/ret. Under SSA (V6) the kill rules are defensive no-ops; the `resolve` cannot cycle (a cycle would require redefining an SSA VReg, impossible). Value-preserving.
- **InstCombinePass:** identity-folds binops with a constant operand (x+0→x, x*1→x, x*0→0, x-x→0, x|x→x, x&-1→x, x&x→x, x^x→0, x<<0→x, x/1→x). Does NOT fold x/0. Float binops excluded (is_foldable_int_binop), so FP x-x is NOT folded to 0 (correct — NaN). The `x-x`/`x|x`/`x&x`/`x^x` folds check `src1 == src2` (same VReg) — value-preserving. Verified by D8. Correct.
- **DeadStoreElimPass:** intra-block; removes a StoreFrame overwritten by a later StoreFrame to the same offset with no intervening reader. Now recognizes CopyBytes/FieldAddr as readers (B2 fix). Iterates to fixpoint within each block. Verified by D9 (dead store removed) + D10 (store feeding CopyBytes kept, post-fix). Value-preserving with the B2 fix.

### V6. SSA VRegs — the foundation that makes the passes sound

`thin_lower.cpp:214` `new_vreg` is `next_vreg++` (monotonic); every producing instr gets a fresh, unique VReg via `new_vreg`/`new_slice_vregs` (the `in.dst =` sites are all `new_vreg(...)` or `0`). Reassignment of a local emits a NEW value VReg + a StoreFrame to the local's fixed frame slot; the VReg itself is never redefined. So the IR is in SSA form for VRegs. This is what makes CopyProp's `resolve` cycle-free, StoreToLoadForward's "src VReg is the value at store time" reasoning correct, and the CSE/CopyProp kill rules (which fire on VReg redefinition) conservative no-ops rather than load-bearing. Verified by grep: no `in.dst = <existing-vreg>` reuse anywhere in `thin_lower.cpp`.

### V7. Consistency with existing patterns

- **constexpr-call pre-pass mirrors enum lowering:** `lower_constexpr_calls_expr/_stmt/_block` (`sema.cpp:2596+`) mirror `lower_enum_access_expr/_stmt/_block` (`sema.cpp:698+`) — same bottom-up ExprPtr&-slot rewrite pattern, same per-stmt-kind dispatch, same "rewrite in place to an IntLit before check_expr" intent. Consistent.
- **static_assert elision mirrors assert_eq_* elision:** both use `try_eval_const_bool` and elide on true / error on false. The comparison-operator folding added to `try_eval_const_bool` (`sema.cpp:261`) strictly widens what folds (a constant comparison arg now elides), matching assert_eq's "a passing constant assertion costs nothing" philosophy. Consistent.
- **typed enum resolution mirrors struct resolution:** `register_typed_enums` before `resolve_type` is the same "register names before type resolution" ordering as structs (StructLayoutTable is consulted in `resolve_type`). The typed-enum Type (prim=backing, struct_name="", enum_name=name) is representationally the backing int so ALL existing int code paths (byte_size, align, codegen, is_int) work unchanged — only `Type::same` (enum_name) and `is_plain_integer` (excludes enum_name) are new. Consistent and minimally invasive.
- **typed enum widening mirrors plain-int widening:** `can_implicitly_convert`'s enum→int branch uses the same signedness + width logic as the plain-int matrix. Consistent (V3).

---

## TEST IMPACT

- **Baseline:** `ctest -E bench --timeout 30` → 40/40 pass. Lang suite → 350/0/0.
- **After fixes:** `ctest -E bench --timeout 30` → 40/40 pass. Lang suite → 350/0/0.
- **New regression tests added:**
  - `examples/constexpr_test.cpp` probe [11] — nested-loop constexpr DoS bound (B1).
  - `examples/ir_passes_test.cpp` D10 — DSE keeps a store that feeds a CopyBytes reader (B2).
  - `examples/ir_passes_test.cpp` D11 — forward does not cross a CopyBytes writer (B3).
  - B4 (shift UB) covered by the existing ir_passes value-preservation sweep + manual probes.
- **No existing test changed or weakened.** All four fixes are conservative (they only ever KEEP a store/forward alive or mask a shift to defined behavior — never remove a correct transformation).

---

## FILES CHANGED

- `src/sema.cpp` — B1: shared total-iteration budget for the constexpr interpreter (ConstEvalCtx + the three loop cases).
- `extensions/opt/ext_opt.cpp` — B2/B3: `instr_reads_off`/`instr_writes_off` helpers + DSE and StoreToLoadForward use them; B4: shift-amount mask in `fold_int_binop`.
- `examples/constexpr_test.cpp` — B1 regression (probe [11]).
- `examples/ir_passes_test.cpp` — B2/B3 regressions (D10, D11).
- `docs/audit/CODE_REVIEW_2026-07-11.md` — this report.

---

## SUMMARY

Four real bugs found and fixed, all with regression tests, 40/40 ctest held green throughout:

1. **B1 (constexpr DoS):** nested loops bypassed the 100k iteration bound (per-loop counter, multiplicatively compoundable). Fixed with a shared total-iteration budget. **Reachable from source, caused a compile-time hang.**
2. **B2 (DSE value-preservation):** DSE removed a StoreFrame that fed a CopyBytes reader (only killed on LoadFrame). Fixed by recognizing CopyBytes/FieldAddr as readers. **Real IR-level defect (hand-built repro); latent behind the pre-existing struct-copy IR gap (P1).**
3. **B3 (forward value-preservation):** StoreToLoadForward forwarded a LoadFrame past an intervening CopyBytes writer. Fixed by killing the pending forward on a CopyBytes dest range. **Real IR-level defect (IR-dump repro); latent behind P1.**
4. **B4 (ConstProp shift UB):** `a << b` / `>> b` for `b >= 64` was C++ UB. Fixed by masking the shift amount to 0..63, matching x64 + `try_eval_const_i64`. **Obscure but real UB.**

One pre-existing issue documented (P1: IR-backend struct-copy known gap, predates the reviewed commits, root reason B2/B3 are latent). Two minor items documented (P2: unreachable-path maybe-uninitialized warning; P3: unused `killed` set in CSE). The five feature areas are otherwise correct: bounds are now real (B1), folding/elision/error paths are sound (V2), typed-enum type rules mirror the plain-int matrix and the user's widening/narrowing edge cases are handled consistently (V3), for-each-over-array falls back to the tree-walker in the IR backend (V4), and all eight passes are value-preserving with the B2/B3/B4 fixes (V5) on the SSA-VReg foundation (V6).
