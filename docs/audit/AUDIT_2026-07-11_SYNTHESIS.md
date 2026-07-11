# Ember Deep Audit Synthesis — 2026-07-11

**Audited revision:** `bf76217` (+1 commit at audit time)
**Scope:** 5 parallel read-only audits: code correctness, security, performance, docs+tests, architecture
**Baseline:** 32/32 ctest pass, 274/0/0 lang suite

## Finding summary

| Dimension | Confirmed | Potential | Verified OK |
|---|---:|---:|---:|
| Code correctness | 6 | 7 | 8 |
| Security | 2 | 4 | 10+ |
| Performance | 5 meaningful | 3 minor | 5 |
| Docs + tests | 14 stale/missing | 3 inaccurate | 16 current |
| Architecture | 0 design issues | 6 minor | 8 well designed |
| **Total** | **27** | **23** | **47+** |

## Critical findings (fixed in this commit)

### S1 (CRITICAL) — v5 IR frame_off validation gap
The validator checked `meta.frame_off` bounds only for 7 specific ops, but `emit_x64` writes to `[rbp + frame_off]` for ~20 producing ops (ConstInt, Add, etc.). A crafted v5 `.em` with `ConstInt(frame_off=+8)` could overwrite the return address.
**Fix:** Extended the check to ALL instructions with `frame_off != 0`.

### S2 (HIGH) — ext_array element bounds check overflow
`size_t(i)*8+8` wraps to 0 for `i=INT64_MAX`, bypassing the bounds check → wild-pointer heap write.
**Fix:** Changed to element-count check (`size_t(i) < bytes.size()/elem_size`) which cannot overflow.

## High-priority findings (not yet fixed — tracked for follow-up)

### C1 (CORRECTNESS) — `arr[i].field` on struct arrays miscompiles silently
Both backends only handle `Ident` base for FieldExpr; `IndexExpr` base emits nothing (tree) / returns VReg 0 (IR). Sema accepts it → silent garbage. Needs codegen fix for struct element indexing.

### C2 (CORRECTNESS) — for-each over struct/slice elements with esz ∉ {1,2,4,8}
Wrong SIB scale + truncated element copy for non-power-of-2 element sizes. Needs imul-based addressing for arbitrary esz.

### C3 (CORRECTNESS) — `g[..]` view of global fixed array broken
Tree-walker emits nothing; IR's MakeSlice ignores base_kind → segfault. Needs global-array view codegen fix.

### C4 (CORRECTNESS) — thin-IR int compare with >32-bit literal
`emit_cmp` uses `cmp_reg_imm32` which truncates the immediate. Needs a mov+cmp fallback for large literals.

### C5 (CORRECTNESS) — u64 literals ≥ 2^63 rejected by sema
Lexer produces the full u64 range but `adapt_int_lit`'s U64 arm gates on `v >= 0`, rejecting values ≥ 2^63.

### C6 (CORRECTNESS) — f64 global initializers lose precision
Folded through `try_eval_const_f32` (no `try_eval_const_f64` exists).

## Performance findings

- Tree-walker is 5-9× slower than g++ -O2 (expected for a tree-walking JIT)
- Thin IR backend is 1.2-1.9× slower than tree-walker (extra store-to-frame per instruction)
- ConstProp has O(n²) re-scan on every removal
- CSE has O(n²) full hash-table scan per VReg redefinition
- IR+passes still slower than tree-walker on 5/6 workloads (IR overhead negates optimization wins)

## Doc/test gaps

- PASS_SYSTEM_DESIGN marked LICM/SubstitutionPass as FUTURE (now fixed)
- GAP_ANALYSIS marks for-each and match as ✗ v1 (both shipped)
- map extension has zero test coverage
- for-each and match absent from canonical grammar/codegen specs
- Several new features lack edge-case tests

## Architecture verdict

**Well designed.** No design issues found. Clean three-lib split, sound pass system, stable IR, correct .em v5 security model, composable codegen flags. 6 minor findings (gratuitous include, redundant branch, comment inaccuracies).

## Reports

- `docs/audit/AUDIT_CODE_CORRECTNESS_2026-07-11.md` (402 lines)
- `docs/audit/SECURITY_AUDIT_2026-07-11.md` (569 lines)
- `docs/audit/PERFORMANCE_AUDIT_2026-07-11.md` (375 lines)
- `docs/audit/AUDIT_2026-07-11_DOCS_TESTS.md` (237 lines)
- `docs/audit/AUDIT_2026-07-11_ARCH_DESIGN.md` (303 lines)

---

**Update 2026-07-11 (post-audit fix pass):** the six correctness findings
C1-C6 above are no longer open — all were fixed after this synthesis was
written, and the tree is green (ctest 34/34, lang suite clean) at the fix
head. This note is appended, not retrofitted; the finding text above is the
historical record as of the audit revision.

- **C1** (`arr[i].field` on struct arrays) — fixed; pinned by
  `examples/field_of_index_test.cpp` (ctest `field_of_index`, commit `381a616`).
- **C2** (for-each over non-power-of-2 element sizes) — fixed; `src/codegen.cpp`
  for-each now uses an `imul`-based element address for esz ∉ {1,2,4,8}
  (commit `ab49898`).
- **C3** (`g[..]` view of a global fixed array) — fixed; `src/codegen.cpp`
  global array/slice/struct base resolution (commit `3b7c8df`).
- **C4** (thin-IR int compare with a >32-bit literal) — fixed; `src/thin_emit.cpp`
  `emit_cmp` falls back to `mov imm64` + `cmp_reg_reg` when the immediate
  doesn't fit `int32` (commit `4d4036e`).
- **C5** (u64 literals ≥ 2^63 rejected by sema) — fixed; `src/sema.cpp`
  `adapt_int_lit`'s `U64` arm now accepts any 64-bit bit pattern
  unconditionally (no `>= 0` guard).
- **C6** (f64 global initializers lose precision) — fixed; `src/sema.cpp`
  `try_eval_const_f64` bakes the exact double bytes into the globals block
  (commit `3b7c8df`).

The two larger doc/test items below (map extension test coverage; for-each
and match absent from the canonical grammar/codegen specs) remain open and
are tracked for a separate follow-up — they are beyond a conservative
doc-fix pass.
