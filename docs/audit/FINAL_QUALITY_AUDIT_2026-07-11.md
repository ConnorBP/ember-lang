# Final Quality Audit — ember compiler

**Date:** 2026-07-11
**Scope:** ember code quality — `src/` (52 files, ~19,914 LOC) + `examples/` (56 files)
**Mode:** READ-ONLY audit; fixed only critical issues (warnings), documented the rest.
**Build under test:** GCC/MinGW `buildt/` with `-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers` (CMakeLists.txt:36). MSVC `/W4` path noted separately.
**Test baseline:** 54/54 ctest passing (captured before any change).

---

## 0. Executive summary

| Category | Before | After | Status |
|---|---|---|---|
| Compiler warnings (GCC `-Wall -Wextra`) | **11** | **0** | Fixed |
| Compiler warnings (MSVC `/W4`) | 0 (last good config) | not re-run* | env |
| Compiler errors | 0 | 0 | OK |
| ctest | 54/54 | **54/54** | OK |
| Resource leaks (VirtualAlloc / fopen / raw new) | 0 found | 0 | Clean |
| Critical production bugs found | **1** | fixed | OK |
| Code-quality items documented (not fixed) | 7 | — | Below |

\* MSVC `build_msvc/` could not be rebuilt in this shell — `LINK : fatal error LNK1104: cannot open file 'kernel32.lib'` because the MSVC dev environment (`vcvarsall.bat`) is not sourced. This is an environment issue, not a code issue. The stale `build_msvc/` was last configured cleanly against `/W4`, so no `/W4` regressions are known; re-validate `/W4` from a Developer Command Prompt when convenient.

**One real production bug was found and fixed:** the constexpr compound-assignment evaluator read a possibly-uninitialized variable (`src/sema.cpp`). Details in §1.1.

---

## 1. Compiler warnings — fixed (11 to 0)

A clean rebuild (`cmake --build . -j 8 --clean-first`) produced 11 warnings. All were fixed. The full build is now warning-free under `-Wall -Wextra`.

### 1.1 CRITICAL — `src/sema.cpp:3854` `cur` may be used uninitialized  [-Wmaybe-uninitialized]

**Severity: HIGH (production code, latent correctness bug).**
**Status: FIXED.**

The constexpr fn interpreter's `AssignExpr` handler, in the compound-assignment branch (`x += rhs`), searched the scope chain for `x` but did not track whether it was found:

```cpp
// BEFORE — src/sema.cpp ~3829
if (a->compound) {
    int64_t cur;                                   // <-- uninitialized
    for (int i = int(ctx.scopes.size()) - 1; i >= 0; --i) {
        auto it = ctx.scopes[size_t(i)].find(id->name);
        if (it != ctx.scopes[size_t(i)].end()) { cur = it->second; break; }
    }
    // if the name was NOT found, cur is still uninitialized here:
    switch (*a->compound) {
    case BinExpr::Op::Add: tmp = bit_cast_i64(uint64_t(cur) + uint64_t(rhs)); break;
    ...
```

If `x` was not declared in any enclosing constexpr scope, `cur` was read uninitialized and fed into the arithmetic switch — undefined behavior producing a garbage fold result. For a compound assignment the target **must** already exist (you cannot `+=` to an undeclared variable), so the correct behavior is to fail the constexpr eval (the call falls back to a runtime call, same as every other `return false` path in the interpreter).

**Fix applied** (`src/sema.cpp`): track a `found` flag; if the variable is not in any scope, set an error and `return false` instead of reading uninitialized memory. `cur` is now zero-initialized and only read when `found == true`.

This is the only warning in production `src/` code, and the only one that represented an actual correctness defect rather than hygiene.

### 1.2 `examples/ir_passes_test.cpp:723` unused variable `orig`  [-Wunused-variable]

**Severity: LOW (test code, dead local).**  **Status: FIXED.**

`size_t orig = total_instrs(tf);` was captured but never used (only `orig_binops` is read later in the assertion). Removed the dead binding.

### 1.3 `examples/gc_test.cpp:79` unused variable `obj3`  [-Wunused-variable]

**Severity: LOW (test code).**  **Status: FIXED.**

`obj3` is allocated purely for its side effect (an unrooted object the next `collect()` sweep must free). The binding was unused. Kept the allocation, added `(void)obj3;` with a comment documenting the intent, so the test's meaning stays readable.

### 1.4 `examples/import_roundtrip_test.cpp:65` unused function `call_i64_i64`  [-Wunused-function]

**Severity: LOW (test code, dead helper).**  **Status: FIXED (removed).**

`call_i64_i64(void* entry, int64_t)` was defined but never referenced anywhere in the file. A leftover from the i64-arg call path that the test doesn't exercise. Removed the dead function (and its comment).

### 1.5 `examples/bench_ember_vs_as.cpp:99,139` unused functions `ember_call_i64`, `as_exec_i64`  [-Wunused-function]

**Severity: LOW (bench code, dead helpers).**  **Status: FIXED (removed).**

The benchmark only exercises the void-returning `main()` path (`ember_call_void` / `as_exec`); the i64-arg call helpers were never called. Removed both dead functions. `ember_call_void` and `as_exec` (the used ones) are untouched.

### 1.6 Misleading-indentation (x5)  [-Wmisleading-indentation]

**Severity: LOW-MEDIUM (test code, readability hazard — logic was correct).**  **Status: FIXED (x5).**

Five test files shared an identical golfed one-liner:

```cpp
if(!sr.ok){ std::printf("FAIL: sema (%zu errors):\n",sr.errors.size());
    for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str()); return false; }
```

Here `return false;` is part of the **`if` block** (it runs once, after the `for` completes), but it sits on the same line as the `for`'s single-statement body, so the indentation *implies* the return is inside the loop. GCC correctly flagged this. The runtime behavior was already correct; the hazard is that a future reader mis-edits it. Fixed in all five by putting `return` and the closing `}` on their own lines so indentation matches control flow.

Files: `ext_lifecycle_test.cpp:73`, `try_catch_test.cpp:93`, `fn_types_test.cpp:121`, `function_refs_test.cpp:112`, `cross_module_handles_test.cpp:122`.

---

## 2. Resource management — CLEAN (no leaks found)

Audited every allocation site in `src/`. RAII discipline is strong.

| Resource | Site | Ownership | Verdict |
|---|---|---|---|
| `VirtualAlloc` exec pages | `jit_memory.cpp` | `alloc_executable` to `free_executable`; freed on seal failure (line 12) and by caller | no leak |
| `VirtualAlloc` per-fn pages | `em_loader.cpp` `LoadedModule` | `~LoadedModule` frees every `void*` in `pages` via `free_executable` (line 891-893); move-assign frees moved-from (line 906); copy deleted | no leak |
| `std::malloc` GC heap | `gc.cpp` `GcHeap` | `~GcHeap` to `clear()` frees every live object via `std::free(hdr)` (line 177); `collect()` sweep frees unmarked | no leak |
| File I/O | `em_loader.cpp`, `em_writer.cpp`, `import.cpp` | all `std::ifstream`/`std::ofstream` (RAII, scope-bound) | no leak |
| Raw `new` | `src/*.cpp` | **zero** raw owning `new` — everything is `make_unique`/`shared_ptr` | excellent |

The `LoadedModule` RAII contract is explicitly documented in `em_loader.hpp:146-150` and the implementation matches the comment. The GC destructor path (`clear()`) is exercised by the gc_test suite.

---

## 3. Recent-feature review — consistency against established patterns

Compared the eight recent features (constexpr, static_assert, typed enums, lambdas, coroutines, exceptions, GC, regalloc) against the established code patterns. **Overall: well-integrated and consistent.** Each feature is numbered (`#20` lambdas, `#21` coroutines, Tier 1/2/4 for the rest), cross-referenced to its planning doc, and follows the existing error-handling and dispatch conventions.

### 3.1 What's consistent (good)

- **Error handling.** Every recent feature uses the established `return false` + out-param-`err` pattern (constexpr evaluator, GC, regalloc, sema checks). No silent failures introduced.
- **Numbered feature tags.** `#20`/`#21` markers on lambda/coroutine code mirror the older `Tier 1/2/4` tags and route every comment to a planning/spec doc. Annotating *why* is the norm here, and the new features follow it.
- **Coroutines — IR-backend fallback is correct.** `compile_func` (codegen.cpp:4600-4664) routes an `is_coroutine` fn to the tree-walker instead of the IR backend (`thin_lower`), because `thin_lower` has **zero** `YieldStmt`/coroutine handling. Crucially this is gated on `f.is_coroutine` (the sema-set flag, single source of truth), **not** on the `has_try_catch` walker — so the gap in `has_try_catch` (it doesn't list `YieldStmt`) is *not* a bug. Verified: the `is_coroutine` condition in the fallback guard covers it. This is well-designed.
- **regalloc.** Linear-scan with explicit callee-saved save slots and frame-size update. Follows the "regalloc only changes WHERE a VReg lives" contract documented in `regalloc.hpp`. No leaks (no allocation — it reuses lowering spill slots).
- **GC.** Precise mark-sweep with a hidden header, RefMap-driven tracing, and a `header_bytes` trailer at `user-4` for O(1) header recovery. Memory-safe (destructor frees all). The header-recovery design is correct.

### 3.2 Consistency observations (documented, not fixed)

**3.2.1 — Two i64 interpreters with deliberately different Div/Mod policy.**
`try_eval_const_i64` (the fold helper for case labels / global inits, sema.cpp:146) **excludes** `Div`/`Mod` on purpose, so a literal-zero divisor hits the real runtime trap rather than a silent compile-time fold. `ce_eval_expr` (the constexpr-fn interpreter, sema.cpp:3720) **handles** `Div`/`Mod` but guards `rhs == 0` then `return false` with "constexpr division by zero" (falls back to runtime). Both policies are documented at their sites. This is a *defensible* design choice (two contexts, two safety profiles), but it means the same integer operation is evaluated by two code paths with different semantics. Worth a spec footnote so a future maintainer doesn't "unify" them and accidentally let `1/0` fold at compile time. **Not a bug.**

**3.2.2 — Coroutine/yield not in `has_try_catch` walker (intentional, safe).**
The `has_try_catch` lambda at codegen.cpp:4604 checks `TryCatchStmt` and `ThrowStmt` but not `YieldStmt`. At first glance this looks like a parity gap (a coroutine might be sent to the IR backend that can't lower yield). It is **not** a bug: the coroutine fallback is handled one line later via `f.is_coroutine` (sema's flag), which is the correct single-source-of-truth gate. Recommend a one-line comment in `has_try_catch` noting "yield is gated separately by `is_coroutine`" so the asymmetry doesn't read as an omission. **Not a bug; documentation suggestion only.**

---

## 4. Code quality — documented (not fixed)

Per the task scope ("fix only critical issues; document the rest"), the following are recorded for future work. None are correctness bugs; all are maintainability/clarity items.

### 4.1 Structural: duplicated AST-walk dispatch (HIGH maintainability debt)

**The single largest quality issue in the codebase.** `codegen.cpp` contains **five near-identical recursive AST walker passes**, each a `_block`/`_stmt`/`_expr` trio with a full `dynamic_cast` dispatch chain over every Stmt/Expr kind:

1. `prescan_*` — computes `makes_calls` / `max_args` (lines 674-756)
2. `count_struct_temps_*` — struct-by-value temp reservation (759-846)
3. `count_arr_temps_*` — array-literal backing temps (865-937)
4. `count_str_temps_*` — string-literal inline temps (947-1010)
5. `count_pin_refs_*` — pinned-local ref counts (1019-1100)

Plus the real codegen tree-walker `exec_*` (1789+). Each must independently handle every Stmt type. **Total `dynamic_cast` dispatch sites: 268 in codegen.cpp, 245 in thin_lower.cpp, 258 in sema.cpp.**

**The concrete cost:** when a new Stmt kind is added (e.g. `YieldStmt`, `ThrowStmt`, `TryCatchStmt`), it must be wired into *every* pass. The `YieldStmt` comments in codegen explicitly reference this — it appears in 7 places, and `ThrowStmt` in 8. A missed handler is a silent correctness bug (the pass just falls through to `return` and under-counts). This is exactly the class of bug that §1.1 almost was, in a different evaluator.

**Recommendation (not done — large refactor):** introduce a `ConstStmtVisitor` / `StmtVisitor` base with one virtual per Stmt kind, and have each pass override only the kinds it cares about (defaulting the rest to a no-op recurse). This collapses the N-by-M dispatch matrix to N+M. Alternatively a `walk_stmt(callback, recurse)` helper that centralizes the child-recursion would remove the boilerplate without a full visitor rewrite. This is a multi-day refactor and out of scope for a warning-fix audit; flagged for a dedicated cleanup pass.

### 4.2 `gc.cpp:84-107` — rambling "stream of consciousness" comment (clarity)

The mark phase has a **22-line comment** walking through five rejected header-recovery designs before reaching the one the code actually uses (`header_bytes` stored at `user - 4`). Excerpt:

```
// SIMPLER: store header_bytes as the FIRST uint32 ...
// Actually, let me just put a back-pointer ...
// SIMPLEST FIX: read header_bytes from a fixed position ...
// OK: let me just store the header pointer ...
// Actually, the cleanest: store header_bytes as a uint32 at (user - 4) ...
// No — that's redundant. Let me just read it from the known position.
```

The final design is correct and the code does exactly `[Header (16B)] [ref_offsets] [header_bytes(4B) trailer] [user bytes]` with recovery via `memcpy(&hb, user-4, 4)`. The comment should be replaced with 3-4 lines stating the final layout and the recovery invariant, dropping the dead-end reasoning. **Not a bug; readability only.**

### 4.3 `parser.cpp:197` — no-op ternary + dead assignment (clarity)

```cpp
f->loc = loc(toks[i-1 > 0 ? 0 : 0]); // placeholder; real loc set below
```

Two issues in one line:
1. `i-1 > 0 ? 0 : 0` — both branches are `0`, so the index is always `toks[0]`. This is a vestigial botched bounds-check (from the initial commit, `f7afc35`) that compiles to a no-op.
2. The value is **immediately overwritten** at line 219 (`f->loc = loc(peek());`) before any read, so the line is dead.

Harmless (no behavioral effect — confirmed the overwrite precedes all reads), but confusing. The line could simply be removed, or replaced with `f->loc = loc(peek());` like the sibling declarations at 173/219/228. **Not a bug; dead/confusing code.**

### 4.4 Near-duplicate `try_eval_const_*` family (low duplication)

`sema.cpp` has four constant-fold helpers — `try_eval_const_i64` (146), `_f32` (194), `_f64` (226), `_bool` (249) — sharing the same `IntLit`/`UnaryExpr`/`BinExpr` dispatch skeleton. The per-type arithmetic is genuinely different (i64 has `bit_cast` wraparound + signed-shift sign-fill; f32 narrows; bool short-circuits), so a naive template won't unify them — it would need per-type policy structs. **Low-priority; the differences are real.** A `ConstFold<T, Policy>` template would remove ~60 lines but add abstraction cost. Not recommended unless the family grows past 4.

### 4.5 Empty `catch (...) {}` in `em_loader.cpp:36,43` — INTENTIONAL, correct

`set_error(std::string* err, ...)` is `noexcept` and swallows exceptions from `*err = message` (string assignment can throw under OOM). The empty catch is **exactly right**: an error-reporting helper must never itself throw. The comment at line 41-42 explains this. **Recorded here only to confirm it was reviewed and is correct — no action.**

### 4.6 MSVC `/W4` not re-validated this session

`build_msvc/` is configured for `/W4` (CMakeLists.txt:34) but could not be rebuilt (`LNK1104: kernel32.lib` — vcvars not sourced in this shell). The last clean `/W4` config passed; the GCC `-Wall -Wextra` build is the authoritative clean result for this audit. **Action: re-run `/W4` from a Developer Command Prompt to confirm parity.** Low risk — the GCC warnings fixed in §1 are all standard diagnostics MSVC also emits (`/W4` covers unused-variable, unused-function, and misleading-indentation-equivalent cases).

---

## 5. Files changed (this audit)

### Fixed (10 files)
| File | Change |
|---|---|
| `src/sema.cpp` | **§1.1 critical fix** — constexpr compound-assign: detect missing variable, fail eval instead of reading uninitialized `cur` |
| `examples/ir_passes_test.cpp` | §1.2 remove unused `orig` |
| `examples/gc_test.cpp` | §1.3 `(void)obj3` + intent comment |
| `examples/import_roundtrip_test.cpp` | §1.4 remove dead `call_i64_i64` |
| `examples/bench_ember_vs_as.cpp` | §1.5 remove dead `ember_call_i64`, `as_exec_i64` |
| `examples/ext_lifecycle_test.cpp` | §1.6 reformat misleading-indentation |
| `examples/try_catch_test.cpp` | §1.6 reformat misleading-indentation |
| `examples/fn_types_test.cpp` | §1.6 reformat misleading-indentation |
| `examples/function_refs_test.cpp` | §1.6 reformat misleading-indentation |
| `examples/cross_module_handles_test.cpp` | §1.6 reformat misleading-indentation |

### Added (1 file)
| File | Content |
|---|---|
| `docs/audit/FINAL_QUALITY_AUDIT_2026-07-11.md` | This report |

---

## 6. Verification

- **Build:** `cmake --build buildt -j 8 --clean-first` to **220/220 steps, 0 warnings, 0 errors** (was 11 warnings before fixes).
- **Tests:** `ctest --output-on-failure` to **54/54 passed** (~118s), unchanged from the pre-fix baseline.
- **Warning grep:** `grep -ci warning` on the full clean-build log to **0**.
- **Resource audit:** every `VirtualAlloc`/`malloc`/file-open in `src/` has a matching release path (§2).

---

## 7. Recommended follow-up (prioritized)

| # | Item | Severity | Effort |
|---|---|---|---|
| 1 | Visitor/`walk_stmt` refactor to collapse the 5 duplicated AST passes (§4.1) | Med (maintainability) | Multi-day |
| 2 | Re-validate MSVC `/W4` from a Developer Command Prompt (§4.6) | Low | Minutes |
| 3 | Replace gc.cpp stream-of-consciousness comment with final-layout note (§4.2) | Low (clarity) | Minutes |
| 4 | Remove/fix the no-op ternary + dead `f->loc` assignment in parser.cpp:197 (§4.3) | Low (clarity) | Minutes |
| 5 | Add a one-line comment in `has_try_catch` noting yield is gated by `is_coroutine` (§3.2.2) | Low (clarity) | Minutes |
| 6 | Spec footnote on the two-interpreter Div/Mod policy split (§3.2.1) | Low (docs) | Minutes |

None of items 2-6 are correctness issues. Item 1 is the only one with real defect-prevention value (it would make the next feature addition unable to silently miss a pass).

---

*Audit complete. Codebase is warning-free, leak-free, and test-green after fixes. One production correctness bug (uninitialized read in constexpr compound-assign) was found and fixed; everything else is documented for follow-up.*
