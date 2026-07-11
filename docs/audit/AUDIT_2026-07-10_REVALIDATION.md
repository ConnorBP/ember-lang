# AUDIT_2026-07-10 — Read-Only Revalidation against current source

**Revalidation date:** 2026-07-11
**Original audit revision:** `baa90c1` (2026-07-10) — see `docs/audit/AUDIT_2026-07-10.md`
**Current HEAD:** `bb0d07a` ("Bench: paired/interleaved comparison + bootstrap 95% CI")
**Method:** read-only. No source files were modified (only untracked probes under `tmp_reval/` were created; `git status` confirms no tracked file changed). Each finding was tested by running a minimal `.ember` probe through `./buildt/ember_cli.exe run <file>`, by running the relevant `buildt/*.exe` test binary, by compiling and running a freshly-linked C++ probe against `buildt/libember.a`, or by reading the current source at the cited location.

**Remediation history between the audit and HEAD.** The audit's own recommended remediation shipped as commit `6cb34ed` ("Audit remediation pass 3: fix all confirmed findings from 07-10 audit + scrutiny"), which is the immediate successor of the audit revision. Between `6cb34ed` and HEAD the tree received substantial additional work — **Stage A** (a thin three-address IR backend behind `enable_ir_backend`, default OFF), **Stage B** (v5 IR `.em` serialization + a shared `em_type_codec` canonical-type codec + more audit fixes), and **Stage C** (a composable IR pass system). None of that work is a *re-audit* of the 07-10 findings; this revalidation is the first independent check of whether the `6cb34ed` fixes survived the Stage A/B/C codegen growth (codegen.cpp grew from ~2.4k to 3744 lines, shifting many line offsets) and whether the M-docs-1 doc refreshes stayed current.

**Build baseline.** A clean rebuild of `buildt/` (MinGW g++ 15.2.0, C++17, Release, `-O2 -DNDEBUG`) completed with one harmless `-Wunused-variable` warning and passes **32/32 CTest** (81.8 s). The language suite returns **268 passed, 0 failed, 0 skipped**. `v0_4_hardening_test`: PASS (22/22 checks). `ext_sync_test`: PASS. `v0_6_hot_reload_test`: PASS. `em_roundtrip_test`: PASS. The `buildt/*.exe` binaries were rebuilt from current source immediately before running (the audit's "re-link discipline" — pre-built probes are never trusted).

**Default code path note.** The Stage A IR backend is OFF by default (`ctx.enable_ir_backend` is set only when `--passes` is given; `compile_func` at `src/codegen.cpp:3406` dispatches to `emit_x64(thf,…)` only then, otherwise the tree-walker runs). The budget/depth/fold/hot-reload code inspected below is the tree-walker path — the one `ember run` and every hardening/ext_sync/hot_reload test exercises. The IR path is byte-identical when off (Stage A c4/c5 gates) and was not the subject of any 07-10 finding.

---

## Summary table

| ID | Original severity | Current status | Evidence (file:line or test result) |
|----|-------------------|----------------|--------------------------------------|
| H-M4-1 | High | **FIXED** | `src/codegen.cpp:3112-3148` (dedicated charged `latch` label is the `continue` target; charge runs before re-evaluating the condition). `v0_4_hardening_test`: `[PASS] H-M4-1: while-true+continue traps under budget=20`; `[PASS] H-M4-1 negative control: finite while+continue completes`. CLI probe `hm41_neg.ember` (100-iter while+continue, sum of odds) → exit 196 = 2500 mod 256 (latch intact) |
| H-M4-2 | High | **FIXED** | `src/codegen.cpp:3294` (`block_cost`), `:3302` (`expr_cost`: AST nodes + native-call setup + per-arg marshalling + aggregate byte copies), `:3356` (`stmt_cost`), `:3287` (`aggregate_copy_cost`). `v0_4_hardening_test`: `[PASS] H-M4-2: 2000-term expression traps under budget=5`, `…300 native calls trap under budget=3`, `…8x8KiB aggregate copies trap under budget=30` |
| H-§10-1 | High | **FIXED** | `src/codegen.cpp:1798-1803` (`if (folded) { e.mov_reg_imm64(Reg::rax, result); if (!is_cmp) normalize_rax(lt); return; }` — folded result normalized to target width before the early return). `v0_4_hardening_test`: `[PASS] H-§10-1: folded i32 enum shift returns -2147483648 (full rax, sign-normalized)` (direct host-call full-rax probe; the in-language exit code is circular by design — both -2147483648 and 2147483648 are 0 mod 256) |
| M-H14-1 | Medium | **FIXED** | `src/em_type_codec.cpp:171-206` (`parse_type` validates before any field assignment: struct-flag iff name, array-flag iff len, fn⇒I64, recorded-sig⇒fn, slice/array⇒Void, recursive) + `:131` (`validate_canonical_type`) + `:155` (`validate_signature`); applied in writer preflight (`em_writer.cpp:152,157`) and loader. Freshly-linked probe `mh14_1_ghost_probe.exe` (hand-patched v2 .em: Prim::Void, flags=0, struct_name="Ghost") → `ok=0 pages=0`, err `inconsistent canonical type: struct flag does not match name` (was `accepted=1 pages=1`). `em_roundtrip_test`: 7× `[4e-*] PASS` canonical-type mutation tests |
| M-§10-2 | Medium | **FIXED** | `extensions/sync/ext_sync.cpp:183-184` (native `n_atomic_cas` masks `expected` via `atom_mask(expected, s->width)`) + `:222-223` (host `atomic_cas_host` masks `expected`). `ext_sync_test`: `[PASS] T2d M-§10-2 narrow CAS masks expected to declared width (native + host)` (8/16/32-bit, should-match + should-fail + host path). CLI probe `m102_cas.ember` (8-bit atomic init -1, CAS expected=-1 desired=7) → exit 11 (swapped; was 77 false-negative pre-fix) |
| M-M4-3 | Medium | **FIXED** | `src/codegen.cpp:435-448` (B1 `emit_depth_check` loads `max_call_depth` from `[r14+context_offsets::max_depth()]`, register-vs-memory; compile-time `ctx.max_call_depth` retained only for the legacy baked branch at `:452`). `v0_4_hardening_test`: `[PASS] M-M4-3: B1 per-context max_call_depth observed (1 traps, 100 permits)` (one compiled body, compile-time max=64, two contexts runtime max 1 vs 100) |
| M-M4-4 | Medium | **FIXED** | `src/dispatch_table.hpp:28-29` (`DispatchTable::set` throws `std::invalid_argument` on null — recoverable host exception, not 0xC0000005) + `src/hot_reload.hpp:91` (`publish` rejects null before reaching `set`). `v0_6_hot_reload_test`: `[PASS] (5) null DispatchTable::set throws std::invalid_argument (recoverable, not 0xC0000005)` |
| M-M3-1 | Medium | **FIXED** | `src/hot_reload.hpp:215-239` (`reload_function` takes `HotReloadDomain&`; `ReloadResult` carries no owning `old_entry`). `docs/HOT_RELOAD.md:7-61` (Section 0: 6-step migration recipe + rationale for no deprecated compat abstraction). `docs/BUNDLING_AND_EM_MODULES.md:143-148` (corrects "unchanged" → "changed in v1.0 (breaking source-API bump)" + migration pointer). `v0_6_hot_reload_test`: `[PASS] (7) …migration-shape…` (8 checks; reverting the signature change would not compile) |
| M-M3-2 | Medium | **FIXED** | `src/hot_reload.hpp:105-140` (`publish` scans the other slots of the same table before retiring the old page; if any still holds `old_entry` it is NOT retired — `old_still_aliased`). `v0_6_hot_reload_test`: `[PASS] (6) same-table alias: replacing slot 0 does not free X while slot 1 holds it`, `[PASS] (6) replacing last alias slot retires and frees X`. `docs/HOT_RELOAD.md:111-118` (contract: reclamation sound while each page is current in at most one slot of its table) |
| M-docs-1 | Medium | **PARTIALLY FIXED** | Stable docs reconciled: `docs/planning/GAP_ANALYSIS.md:105-106` ("portability and signatures are now shipped v2 guarantees … not H12/H14 redesigns"), `docs/MODULES.md:257-260,329-336` (v1 ABI-trusted `unknown_sig` vs v2 canonical sigs), `docs/LIFECYCLE.md:36,96` (real APIs `get_annotated_functions`/`ember_call_void`/`ember_call_i64`; explicit "There is no `ember_module_destroy` facade"), `docs/BUNDLING_AND_EM_MODULES.md:143` ("changed in v1.0"), `docs/spec/SAFETY_AND_SANDBOX.md:88,110` (reach-aware estimator + dedicated charged latch). **Residual:** `docs/planning/plan_CONTEXT_THREADSAFETY.md:72` still says "reach-aware per-statement charging is **in flight**" (it is shipped) and its Appendix evidence index (lines 763-773) carries stale `src/codegen.cpp` line cites (e.g. `:319`–`340` for `emit_budget_check`, actual `:384`; `:364`–`407` for `emit_depth_check`/`leave`, actual `:429`/`:459`; `:293`/`:332`/`:295`/`:307` for the baked imm64s, actual `:358`/`:397`/`:450`) — regressed by the Stage A/B/C codegen growth after `6cb34ed` refreshed them |
| L-§10-3 | Low | **FIXED** | `src/codegen.cpp:22-29` (`bit_cast_i64` via `memcpy`, defined behavior) used for Add/Sub/Mul/Shl (`:1773-1788`) and Shr (`:1789-1794`: unsigned shift + explicit sign fill). `src/sema.cpp:11-19` (same `bit_cast_i64`) + `:168-171` (Shr as unsigned shift + sign fill). CLI probe `l103_shr.ember` (`(-8) >> 2`) → exit 254 = -2 mod 256 (arithmetic right shift). `tests/lang/runtime_integer_boundaries.ember:29` locks the sign-fill behavior |

**Tally:** 11 findings. **10 FIXED** (3/3 High, 6/7 Medium, 1/1 Low). **1 PARTIALLY FIXED** (M-docs-1: stable docs + spec reconciled; one planning doc retains a wrong "in flight" wording and stale line cites regressed by later codegen growth). **0 STILL OPEN, 0 NOT APPLICABLE.**

No 07-10 finding regressed in *behavior*. The single partial is documentation staleness in a *planning* doc (not the live spec), introduced by the Stage A/B/C codegen expansion shifting line numbers after the `6cb34ed` doc refresh — the substantive contradictions the audit flagged (stable docs claiming loop-only budget / script-only depth / nonexistent lifecycle facades / "unchanged" reload / "portability+signatures remain redesigns") are all resolved.

---

## Detailed evidence

### H-M4-1 — `while`-`continue` bypasses the loop budget charge → FIXED

The audit: `WhileStmt` lowered the back-edge charge *after* the body and made `continue` target the condition/top label *before* the charge, so `while (true) { …; continue; }` completed under any budget.

Current source (`src/codegen.cpp:3112-3148`): the `WhileStmt` lowering now allocates a dedicated `latch` label, binds it as the loop's `continue` target (`loops.push_back({latch, end, …})` at `:3143`), and after the body binds `latch`, emits the budget charge, then jumps to `top`:

```
Label top = …, latch = …, end = …;
e.bind(top); eval(cond); cmp rax,0; jcc e,end;
loops.push_back({latch, end, false, …});
exec_block(body);
loops.pop_back();
e.bind(latch);
emit_budget_check(block_cost(ws->body), "budget exceeded at loop back-edge");
e.jmp(top);
e.bind(end);
```

`ContinueStmt` (`:3257-3266`) jumps to `loops[i].cont` (= `latch`), so the charge always runs before the condition is re-evaluated — mirroring `for` (charge at `:3207`) and `do-while` (charge at `:3163`).

Test evidence: `v0_4_hardening_test` `[PASS] H-M4-1: while-true+continue traps under budget=20 (was: completed)` and `[PASS] H-M4-1 negative control: finite while+continue completes (latch intact)`. CLI negative control `tmp_reval/hm41_neg.ember` (100-iteration `while (i<100){ i=i+1; if((i&1)==0){continue;} sum=sum+i; }`, sum of odds 1..99 = 2500) → exit 196 = 2500 mod 256.

### H-M4-2 — Coarse per-statement charging cannot bound reachable straight-line work → FIXED

The audit: `block_cost`/`stmt_cost` defaulted to a per-AST-statement constant and ignored expression size, native-call setup, arg marshalling, and aggregate byte copies.

Current source: `expr_cost` (`src/codegen.cpp:3302-3354`) counts every AST expression node (+1 each), adds +2 per `CallExpr` site (setup + call) plus +1 per marshalled arg plus `aggregate_copy_cost` for by-value struct/array args, +2 for overload dispatch; `aggregate_copy_cost` (`:3287-3298`) is `(bytes+7)/8` floored at 1 (slices = 2). `stmt_cost` (`:3356-3389`) composes `block_cost`/`expr_cost` across all statement forms including switch compare chains (`sw->cases.size()`). `cost_add` (`:3283-3286`) saturates at `int32_t::max` so arbitrarily large legal ASTs stay encodable as the positive imm32 the charge consumes. Trusted native *body* time remains deliberately outside the unit (documented at `:3274-3281`).

Test evidence: `v0_4_hardening_test` `[PASS] H-M4-2: 2000-term expression traps under budget=5`, `…300 native calls trap under budget=3`, `…8x8KiB aggregate copies trap under budget=30` (R3/R4/R5, all "was: completed"). The estimator is also described in the live spec `docs/spec/SAFETY_AND_SANDBOX.md:82-97` ("reach-aware: it counts not only AST statements but the [expression nodes, native-call setup, arg marshalling, aggregate copies]").

### H-§10-1 — Narrow typed literal fold bypasses target-width normalization → FIXED

The audit: the folded integer early return at `codegen.cpp:1279-1305` returned *before* `normalize_rax(lt)` at `:1420`, so `1073741824_i32 << 1` folded to zero-extended `0x0000000080000000` (2147483648) instead of sign-normalized `0xffffffff80000000` (-2147483648).

Current source (`src/codegen.cpp:1742-1803`): `is_cmp` is computed before the fold so the fold early return can use it; the fold computes Add/Sub/Mul/Shl/Shr/bitwise in `uint64_t` then `bit_cast_i64` (L-§10-3), and the early return now normalizes:

```
if (folded) { e.mov_reg_imm64(Reg::rax, result); if (!is_cmp) normalize_rax(lt); return; }
```

(`:1803`.) This applies to **every** folded integer op (Add/Sub/Mul/And/Or/Xor/Shl/Shr share the single early return), exactly as the audit's fix required.

Test evidence: `v0_4_hardening_test` `[PASS] H-§10-1: folded i32 enum shift returns -2147483648 (full rax, sign-normalized)` — the probe (`fold_i32_sign_normalize_direct_probe`, `:356-404`) compiles `enum E{Top=1073741824,One=1} fn folded()->i32{ return E::Top<<E::One; }` and calls it through a raw cast returning the **full 64-bit rax** (not a normalizing call), asserting `folded_ret == -2147483648LL` and `folded_ret == runtime_ret`. This is the audit's `shift_enum_direct_probe` shape; it fails with the fix reverted. The CLI exit code is deliberately circular (both values are 0 mod 256 — the audit's own observation), so the direct-host-call hardening check is the authoritative evidence.

### M-H14-1 — v2 loader accepts internally inconsistent canonical type metadata → FIXED

The audit: a hand-built v2 type (Prim::Void, flags=0, struct_name="Ghost") was accepted and published as an exec page; the loader assigned the name without requiring flag/name equivalence.

Current source: canonical-type validation is centralized in the shared `src/em_type_codec.cpp` codec (introduced by Stage B), enforced on **both** sides:
- Writer preflight: `em_writer.cpp:152,157` call `validate_signature` → `validate_canonical_type` (`em_type_codec.cpp:131-153`) before serialization.
- Loader: `parse_type` (`em_type_codec.cpp:171-206`) validates **before assigning any field or allocating an element**: (a) struct flag iff name nonempty (`:186`), (e) array flag iff nonzero length + slice/array mutually exclusive (`:190-191`), (b) fn handle requires Prim::I64 (`:193`), (c)+(d) recorded signature requires fn handle (`:195`), (g) slice/array require Prim::Void (`:199`), with recursive validation of `elem`/recorded params/return (`:202-206`, depth-capped at 16).

Probe evidence: freshly-linked `tmp_reval/mh14_1_ghost_probe.exe` writes a valid v2 module whose native binding recorded-ret is struct "Ghost", then binary-patches the type's flags byte from 4 (struct) to 0 (clearing the struct bit while leaving name_len=5/"Ghost"), then loads. Result: `ok=0 pages=0`, `err: em_type_codec: format: inconsistent canonical type: struct flag does not match name`, `RESULT: PASS` (was `accepted=1 pages=1`). `em_roundtrip_test` `[4e-slice_vs_array/struct_name/fn_handle/recorded_sig/recorded_params_arity/recorded_ret/elem_recursion] PASS` — 7 per-dimension canonical_type_same mutation tests at the loader boundary.

### M-§10-2 — Narrow atomic CAS does not mask the expected operand to declared width → FIXED

The audit: `ext_sync.cpp:173-187` masked `desired` to width but compared `expected` raw, so an out-of-range `expected` false-negatively failed against a canonical stored value.

Current source: `n_atomic_cas` (`extensions/sync/ext_sync.cpp:176-193`) masks `expected` via `atom_mask(expected, s->width)` (`:183`) exactly as `desired` is masked (`:184`), and `atomic_cas_host` (`:219-226`) does the same on the host path (`:222-223`).

Test evidence: `ext_sync_test` `[PASS] T2d M-§10-2 narrow CAS masks expected to declared width (native + host)` — for each of 8/16/32-bit: stores `width(-1)` (all-ones low bits), probes a should-match CAS (`expected=-1`, MUST succeed — pre-fix false-negatively returned 0) and a should-fail CAS (low bits differ, MUST fail), plus the same two on the host `atomic_cas_host` path. CLI probe `tmp_reval/m102_cas.ember` (`atomic_new(8,-1); atomic_cas(h,-1,7)`) → exit 11 (swapped to 7; would be 77 false-negative pre-fix).

### M-M4-3 — B1 ignores the per-context `max_call_depth` → FIXED

The audit: `emit_depth_check` compared `call_depth` to the compile-time `int32_t(ctx.max_call_depth)` in B1, never loading `[r14 + context_offsets::max_depth()]`; the CLI masked this by copying the runtime value into the codegen value pre-compile.

Current source (`src/codegen.cpp:429-457`): in B1 (`ctx.use_context_reg`), `emit_depth_check` now emits `inc dword [r14+off_depth]` (`:442`), `mov eax, dword [r14+off_max_depth]` (`:445`), `cmp dword [r14+off_depth], eax` (`:448`) — register-vs-memory, reading the per-context field. The compile-time `ctx.max_call_depth` is retained **only** for the legacy baked branch (`:450-452`: `mov rax, ctx.depth_ptr; inc [rax]; cmp [rax], imm32`).

Test evidence: `v0_4_hardening_test` `[PASS] M-M4-3: B1 per-context max_call_depth observed (1 traps, 100 permits)` — `b1_per_context_max_call_depth` (`:306-355`) compiles one recursive body with `use_context_reg=true` and compile-time `max_call_depth=64` (deliberately neither 1 nor 100), then runs it against two contexts with runtime max 1 (traps on the first recursive edge) and 100 (completes). The pre-fix imm32 path would have used 64 for both and ignored each runtime value. The CLI is itself a B1 caller (`examples/ember_cli.cpp:526` `use_context_reg=true`, `:528` copies the value only for the legacy field), so the live path is the r14 path.

### M-M4-4 — Null direct/indirect dispatch entries fault outside Ember traps → FIXED

The audit: a null `dispatch[slot]` produced `0xC0000005` instead of an Ember `TrapReason`.

Current source: the fix is at the publication boundary. `DispatchTable::set` (`src/dispatch_table.hpp:28-29`) rejects null with `throw std::invalid_argument("DispatchTable::set: null function")` — a recoverable C++ exception the host can catch, not a process fault. `HotReloadDomain::publish` (`src/hot_reload.hpp:91`) rejects null before reaching `set` (`if (!new_entry || slot >= table.slots.size()) return info;`). The audit's alternative ("guard call targets with a null-check trap") was not taken; the chosen approach prevents the null entry from ever reaching the generated `call [base+slot*8]`.

Test evidence: `v0_6_hot_reload_test` `[PASS] (5) null DispatchTable::set throws std::invalid_argument (recoverable, not 0xC0000005)` (try/catch confirms the exception type and that the slot stays null).

### M-M3-1 — Old supported single-threaded reload source API removed without migration → FIXED

The audit: `reload_function` changed from a 7-arg helper returning a caller-owned `old_entry` to a `HotReloadDomain&`-taking helper with no old pointer; existing hosts no longer compiled and the ownership migration was non-mechanical, with no migration doc.

Current source: `reload_function` (`src/hot_reload.hpp:215-239`) takes `(new_fn_source, prog, table, domain, ctx, natives, overloads, structs)` and returns `ReloadResult` carrying `publication_epoch`/`retirement_epoch`/`old_page_retired`/`new_fn` — no owning old pointer (the domain owns the replaced page from `publish` success).

Doc evidence: `docs/HOT_RELOAD.md:7-61` (Section 0 "Migration from the pre-v1.0 single-threaded reload API (breaking bump)") — a 6-step recipe (persistent domain beside the table; guard before every outer call; stop freeing/reading `old_entry`; disown the old `CompiledFn`; periodically `reclaim()`/`quiesce()`; drain guards then quiesce before freeing current pages) plus the rationale that a hidden temporary domain is unsafe. `docs/BUNDLING_AND_EM_MODULES.md:143-148` corrects the old "unchanged" claim to "changed in v1.0 (breaking source-API bump)" with a pointer to the recipe. No deprecated compat abstraction ships (documented as intentional).

Test evidence: `v0_6_hot_reload_test` `[PASS] (7) compile migration-shape module` + 7 further `(7) …` checks (persistent domain, guarded calls, domain-based reload succeeds, replaced page transferred to domain with no caller-owned old_entry, post-reload call guarded, reclaim frees retired page, shutdown quiesce) — reverting the signature change would not compile.

### M-M3-2 — Duplicate current-page alias can be reclaimed while still published → FIXED

The audit: two slots publishing the same allocation, then replacing one, recorded and reclaimed it while the other slot still pointed to it; a guarded call through the surviving slot would jump to decommitted memory.

Current source (`src/hot_reload.hpp:101-140`): `publish` scans the other slots of the same table before retiring the old page — `old_still_aliased` (`:113-119`); if any other slot still holds `old_entry`, the page is **not** pushed to `retired_` (`:122`), so `reclaim` cannot free it while it is still current via another slot. The page is retired only when its last publishing slot is replaced. `ReloadResult.old_page_retired`/`retirement_epoch` reflect this (`:139-140`). This is the audit's "reference-counting" alternative (implemented per-table via scan rather than a separate count structure). The contract is documented at `docs/HOT_RELOAD.md:111-118`: "the domain's reclamation is sound only while each executable page is current in at most one slot of the table it was published through" — i.e. cross-table aliasing of one allocation under a shared domain is out of contract (the scan is per-table; a domain spanning multiple tables does not cross-scan).

Test evidence: `v0_6_hot_reload_test` `[PASS] (6) same-table alias: replacing slot 0 does not free X while slot 1 holds it` (after `reclaim`, `freed==0`, slot 1 still callable and returns X's value) and `[PASS] (6) replacing last alias slot retires and frees X` (`old_page_retired`, `freed==1`). This is the audit's reproduced same-table scenario, now closed. (The audit did not reproduce a cross-table scenario; that case is contractually excluded per the docs.)

### M-docs-1 — Stable docs contradict the remediated H12/H14/M4 and name nonexistent lifecycle APIs → PARTIALLY FIXED

The audit cited six doc locations. Status of each:

| Cited location (audit) | Current file | Status |
|---|---|---|
| `planning/GAP_ANALYSIS.md:98-103` "portability/signatures remain redesigns" | `docs/planning/GAP_ANALYSIS.md:105-106` | **FIXED** — "portability and signatures are now shipped v2 guarantees (`../MODULES.md` Section 5), not H12/H14 redesigns." |
| `MODULES.md:309-322,374-378` "only v1/no-metadata as live-link" | `docs/MODULES.md:257-260,329-336` | **FIXED** — distinguishes v1 ABI-trusted `unknown_sig` from v2 canonical `Type` signatures + build/ABI identity; "the metadata gap is now closed for v2." |
| `planning/DESIGN.md:136-144` "loop-only budget/script-only depth" | `docs/planning/DESIGN.md:133-144` | **OK (historical)** — this is the v0.4 *milestone* entry in a versioned timeline; "sub+jg charged at function entry AND loop back-edges" accurately describes what v0.4 shipped. The v1.0 reach-aware refinement lives in the spec (`SAFETY_AND_SANDBOX.md:88`). Not a contradiction of the remediated work. |
| `planning/plan_CONTEXT_THREADSAFETY.md:70-83,755-769` "loop-only budget/script-only depth, stale line cites" | `docs/planning/plan_CONTEXT_THREADSAFETY.md:70-83,762-773` | **PARTIALLY FIXED** — see below. |
| `LIFECYCLE.md:24-39,56-68,89-110` "presents `ember_get_annotated_functions`/`ember_module_destroy`/`ember_call`/`ember_reload_*` as real APIs" | `docs/LIFECYCLE.md:25-39,56-68,89-110` | **FIXED** — uses the real `get_annotated_functions` (declared `src/lifecycle.hpp:35`), `ember_call_void`/`ember_call_i64` (declared `src/engine.hpp:97-98`), `reload_function`; explicitly states "There is no `ember_module_destroy` facade — unload is host-owned teardown" (`:36`, repeated `:96`). No nonexistent facade is presented as real. |
| `BUNDLING_AND_EM_MODULES.md:136` "single-function reload unchanged" | `docs/BUNDLING_AND_EM_MODULES.md:143-148` | **FIXED** — "changed in v1.0 (breaking source-API bump)" + migration recipe pointer (cross-ref to M-M3-1). |

The live *spec* is correct: `docs/spec/SAFETY_AND_SANDBOX.md:82-97` describes the reach-aware estimator (expression nodes, native-call setup, arg marshalling, aggregate copies) and `:110-112` the dedicated charged latch for `while` back-edges; `:109` B1 r14 addressing. This is the authoritative document and it matches the shipped code.

**Residual (the partial):** `docs/planning/plan_CONTEXT_THREADSAFETY.md` is a *planning* doc (a design investigation, not the live spec). Its substantive budget/depth description was updated by `6cb34ed` (it now mentions reach-aware charging and B1 r14), but two stalenesses remain:
1. Line 72: "reach-aware per-statement charging is **in flight**" — it is **shipped** (H-M4-2 fix is live and tested). This wording was added by `6cb34ed` even as it shipped the feature in the same commit.
2. Stale `src/codegen.cpp` line cites throughout §1.2 and the Appendix evidence index (lines 72, 82, 99-102, 763-773), regressed by the Stage A/B/C codegen expansion *after* `6cb34ed` refreshed them. Verified actuals: `emit_budget_check` is at `:384` (doc says `:319`–`340`); `emit_depth_check` at `:429` / `emit_depth_leave` at `:459` (doc says `:364`–`407`); the baked `trap_ctx`/`budget_ptr`/`depth_ptr` imm64s at `:358`/`:397`/`:450` (doc says `:293`/`:332`/`:295`/`:307`). `context.hpp` cites are roughly intact; `codegen.hpp` cites are off by ~15-20 lines.

**Severity assessment:** these are line-cite drift and one wrong "in flight" qualifier in a planning/archaeology doc. They do not contradict the remediated behavior (the live spec is correct), and a reader following the *function names* (`emit_budget_check`, `emit_depth_check`) lands on the right code. The audit's core M-docs-1 concern — stable docs presenting a stale budget model, nonexistent facades, and a wrong "unchanged" reload claim — is resolved. The partial is real but low-impact; it would be closed by re-running the line-cite refresh against the current file and changing "in flight" to "shipped."

### L-§10-3 — Implementation-defined fold/conversion portability → FIXED

The audit: the signed-negative right-shift fold (`sema.cpp:150`, `codegen.cpp:1302`) and out-of-range `uint64_t`→`int64_t` conversions were implementation-defined (matched x64 on MinGW but not portable).

Current source: `bit_cast_i64` (`src/codegen.cpp:22-29`, duplicated at `src/sema.cpp:11-19`) does the uint64→int64 conversion via `std::memcpy` (defined behavior, identical on every two's-complement target). All integer folds route through it: Add/Sub/Mul/Shl (`codegen.cpp:1773-1788`, `sema.cpp:154-160`), Neg (`sema.cpp:144`), and Shr is implemented as an **unsigned shift plus explicit sign fill** (`codegen.cpp:1789-1794`, `sema.cpp:168-171`: `uint64_t ur = uint64_t(l) >> sh; if (sh!=0 && l<0) ur |= ~((1ULL<<(64-sh))-1); out = bit_cast_i64(ur);`) — defined behavior that matches x64 `sar`, replacing the implementation-defined signed `>>`.

Test evidence: CLI probe `tmp_reval/l103_shr.ember` (`let v: i64 = (-8) >> 2; return v;`) → exit 254 = -2 mod 256 (arithmetic right shift, sign fill). `tests/lang/runtime_integer_boundaries.ember:29` locks the sign-fill behavior in the language suite (268/0/0). The fold and runtime paths now agree via the same `bit_cast_i64` + sign-fill logic.

---

## Reproduction artifacts (this revalidation)

Untracked probes under `tmp_reval/` (read-only w.r.t. tracked source; `git status` shows only `?? tmp_reval/`):

| Artifact | Purpose | Result |
|---|---|---|
| `tmp_reval/mh14_1_ghost_probe.cpp/.exe` | M-H14-1: hand-patch a v2 .em to the audit's "Ghost" shape (Prim::Void, flags=0, struct_name="Ghost") and load | `ok=0 pages=0`, err "struct flag does not match name" → PASS (was `accepted=1 pages=1`) |
| `tmp_reval/m102_cas.ember` | M-§10-2: 8-bit atomic CAS with out-of-range `expected=-1` against stored all-ones | exit 11 (swapped) → PASS (would be 77 pre-fix) |
| `tmp_reval/hm41_neg.ember` | H-M4-1 negative control: finite while+continue, sum of odds | exit 196 = 2500 mod 256 → PASS (latch intact) |
| `tmp_reval/l103_shr.ember` | L-§10-3: negative-literal arithmetic right shift fold | exit 254 = -2 mod 256 → PASS (sign fill) |
| `tmp_reval/h101_fold.ember` | H-§10-1: folded i32 enum shift (CLI exit code is circular by design) | exit 0 (both ±2147483648 are 0 mod 256; authoritative evidence is the hardening test's full-rax direct host call) |

Build/link discipline: `mh14_1_ghost_probe.exe` was compiled against the current `buildt/libember_frontend.a` + `buildt/libember.a` + `buildt/libember_ed25519.a` immediately before running. The `.ember` probes ran against a freshly-rebuilt `buildt/ember_cli.exe`. The committed `buildt/*.exe` test binaries were also rebuilt from current source (`cmake --build buildt --target ember_cli` + full `ctest`) before this run.

---

## Conclusion

Of the 11 findings the 07-10 audit confirmed, **10 are fully fixed at HEAD `bb0d07a`** and hold under independent probing — the three Highs (M4 while-continue latch, M4 reach-aware charging, §10 fold normalization), six Mediums (H14 canonical-type validation, §10 narrow-CAS expected masking, M4-3 B1 per-context depth, M4-4 null-dispatch rejection, M3-1 reload API migration, M3-2 same-table alias retirement), and the Low (§10 bit-preserving folds). The `6cb34ed` remediation shipped real fixes, not circular self-tests, and the subsequent Stage A/B/C work (IR backend, v5 `.em`, pass system) did not regress any of them in behavior — verified because the default code path is still the tree-walker those fixes live in, and the hardening/ext_sync/hot_reload tests all pass against current source.

The single **PARTIALLY FIXED** finding is M-docs-1: the stable docs and the live spec are reconciled, but the planning doc `plan_CONTEXT_THREADSAFETY.md` retains one wrong "in flight" qualifier (the reach-aware charging is shipped) and a batch of stale `src/codegen.cpp` line cites that drifted when Stage A/B/C expanded codegen.cpp after `6cb34ed` had refreshed them. This is low-impact documentation staleness in a planning/archaeology document, not a behavioral regression or a contradiction of the remediated work — the authoritative spec (`SAFETY_AND_SANDBOX.md`) is correct. It would be closed by re-running the line-cite refresh against the current file and changing "in flight" to "shipped."

No finding is STILL OPEN or NOT APPLICABLE. No 07-10 fix was regressed by the Stage A/B/C work.
