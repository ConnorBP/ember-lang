# ember — Code-Correctness Deep Audit

**Repo:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Audited commit:** working tree at `7c73361` (build `buildt/` was built from this; the
task named `bf76217` but `7c73361` is one commit ahead — the diff between the two is
cosmetic only: 15 unused-variable/warning removals in `codegen.cpp`/`thin_lower.cpp`/
`peephole.cpp`/`parser.cpp`, no semantic changes. The running binary in `buildt/` is from
`7c73361`, so I audited the tree that matches the binary and verified every finding by
running `.ember` probes through `./buildt/ember_cli.exe`.)
**Build:** MinGW g++ 15.2.0, C++17, Release. `ctest`: 32/32 pass. Lang suite: 274/274 pass.
**Scope:** read-only audit of frontend (lexer/parser/sema), codegen (tree-walker + thin
IR), runtime (engine/dispatch/hot-reload). No source edited.

Findings are filed as **CONFIRMED DEFECT** (reproduced with a probe), **POTENTIAL ISSUE**
(reasoned but not crisply reproducible / contingent), or **VERIFIED CORRECT** (checked and
sound). Exact `file:line` cited for each.

---

## CONFIRMED DEFECTS

### C1. `arr[i].field` on an array/slice of structs miscompiles silently (both backends)

**Files:**
- tree-walker: `src/codegen.cpp:2777` (FieldExpr eval — `if (auto* bid = dynamic_cast<const Ident*>(fl->base.get()))` is the only handled base shape; an `IndexExpr` base falls through with **no code emitted**)
- thin IR: `src/thin_lower.cpp:1893` (same Ident-only base; an `IndexExpr` base returns `{LoweredValue::Scalar, 0, 0, ex.ty}` — a zero/invalid VReg)

**Cause:** `arr[i].field` parses as `FieldExpr { base: IndexExpr { arr, i }, field }`. Both
codegen paths only handle a bare `Ident` base for a struct field read; when the base is an
`IndexExpr` they emit nothing (tree-walker) / return VReg 0 (IR). Sema **accepts** the
expression (`sema.cpp` FieldExpr case types it from the resolved element type + field
layout), so the program reaches codegen and miscompiles with no diagnostic.

The related IndexExpr eval (`src/codegen.cpp:2748` `load_elem_to_rax(..., width, ...)`)
also only loads a single 8-byte rax for a struct element (`load_elem_to_rax`'s `default`
case is an 8-byte `load_reg_mem`), so even `let p: Struct = arr[i];` copies only the first
8 bytes of a >8-byte struct. The `arr[i].field` field-read is the most direct surface.

**Probe** (`/tmp/ember_audit/idx_struct_clean.ember`):
```
struct P { a: i64; b: i64; }
fn main() -> i64 {
    let arr: P[3] = [ P { a: 10, b: 100 }, P { a: 20, b: 200 }, P { a: 30, b: 300 } ];
    return arr[0].a;
}
```
- tree-walker: exit=44 (expected 10)
- IR (`--passes dce`): exit=80 (expected 10)

Element size 8 (`struct P1 { v: i64; }`) happens to return the right value by accident
(the single field is the whole 8-byte load, and the dead FieldExpr leaves rax untouched),
which is why this escaped the suite — no in-tree test does `arr[i].field` on a struct
array. The defect is fully general for any struct element whose `value_bytes` ∉ {1,2,4,8}
and for any `slice[i].field`.

**Impact:** silent wrong-code on a construct sema admits. Any script indexing a struct
array/slice and reading a field gets garbage.

---

### C2. for-each over a slice with element size ∉ {1,2,4,8} reads the wrong element + truncates it

**File:** `src/codegen.cpp:3222-3227` (ForEachStmt codegen, tree-walker-only — the IR
backend falls back to the tree-walker for for-each, `src/thin_lower.cpp:966`).

**Cause — two compounding bugs in the element load:**
1. **Wrong index scale** (`src/codegen.cpp:3222`):
   ```
   if (esz == 1) scale = 0; else if (esz == 2) scale = 1;
   else if (esz == 4) scale = 2; else scale = 3;
   ```
   `lea_reg_mem_sib` interprets `scale` as ×(1<<scale): 0→×1, 1→×2, 2→×4, 3→×8. For an
   element size of 16 (slice of two-i64 structs, or slice of slices) or 12/14/etc., the
   `else scale = 3` branch picks ×8, so element `i` is read from `ptr + i*8` instead of
   `ptr + i*esz`. (The IndexExpr load path next door correctly uses `imul rax, width` and
   does not have this bug — only for-each does.)
2. **Truncated element copy** (`src/codegen.cpp:3225-3227`):
   `load_elem_to_rax(..., esz, false)` and `store_rax_elem(..., esz)` both fall to their
   `default:` 8-byte path for any width ∉ {1,2,4,8}, so only the first 8 bytes of a
   >8-byte element are copied to the loop variable slot.

**Probe** (`/tmp/ember_audit/fe_struct.ember`, esz=16):
```
struct Point { x: i64; y: i64; }
fn main() -> i64 {
    let a: Point[3] = [ Point { x: 10, y: 100 }, Point { x: 20, y: 200 }, Point { x: 30, y: 300 } ];
    let s: Point[] = a[..];
    let mut sum: i64 = 0;
    for (p in s) { sum = sum + p.x; }
    return sum;
}
```
- exit=130 (expected 60). The 130 is exactly `10 + 100 + 20` = elem0.x + elem0.y + elem1.x,
  the fingerprint of the ×8 scale (indices 0,1,2 read offsets 0,8,16 → fields x0,y0,x1).
- esz=12 probe (`fe_struct12.ember`): exit=11 (expected 60).
- esz=8 probe (`fe_struct8.ember`, `struct P1 { v: i64 }`): exit=60 (correct, by luck —
  the single 8-byte field is the whole element).

`tests/lang/valid_for_each.ember` only exercises `i64[]` (esz=8), so the suite passes.

**Impact:** any `for (x in slice_of_struct_or_slice)` iterates the wrong memory and copies
a truncated element. Silent wrong-code.

---

### C3. `g[..]` view of a global fixed array is broken in both backends (tree-walker: garbage; IR: segfault)

**Files:**
- tree-walker: `src/codegen.cpp:2758-2768` (ViewExpr eval — only the local-Ident branch
  exists; a global-Ident base misses `locals.find` and emits **nothing**, leaving rax/rdx
  as stale register state)
- thin IR emit: `src/thin_emit.cpp:1275-1290` (`MakeSlice` always emits
  `lea rax, [rbp+frame_off]` — a frame-relative address — even when the lowering set
  `meta.base_kind = GlobalsBase` for a global array (`src/thin_lower.cpp:1872-1880`); the
  base_kind is ignored, so the slice ptr is a stack address, not the globals block)

Sema accepts the expression (`sema.cpp` ViewExpr only checks `bt->array_len > 0`, not
local-vs-global), so it reaches codegen.

**Probe** (`/tmp/ember_audit/view_global.ember`):
```
global g: i64[3] = [1, 2, 3];
fn main() -> i64 {
    let s: i64[] = g[..];
    return s[1];
}
```
- tree-walker: exit=15 (expected 2) — garbage slice ptr/len.
- IR (`--passes dce`): **SIGSEGV** (exit=139) — the relative globals offset is used as an
  absolute pointer and dereferenced.

Direct indexing `g[1]` works in both (`src/codegen.cpp` IndexExpr has a real global branch;
`src/thin_lower.cpp:1812` too) — only the `g[..]` view is broken. No in-tree test views a
global fixed array (`examples/aggregate_global_test.cpp` tests global arrays/slices but
never `g[..]`).

**Impact:** silent wrong-code (tree-walker) or process crash (IR) on a construct sema
admits. `aggregate_global_test` does not cover it.

---

### C4. thin-IR integer compare with an IntLit operand > 32 bits truncates the immediate

**File:** `src/thin_emit.cpp:1554` (`emit_cmp`, integer immediate path):
```
e.cmp_reg_imm32(Reg::rax, int32_t(in.imm.i));
```
`cmp_reg_imm32` emits `81 /7 id` which sign-extends an **imm32**. The lowering
(`src/thin_lower.cpp:1536`) sets `rhs_is_imm = rhs_lit && !is_float` for **any** IntLit
rhs, regardless of magnitude, so a 64-bit literal compare truncates the literal to its low
32 bits. The tree-walker has no immediate-form compare (it always evals both operands into
rax/rcx and `cmp_reg_reg`), so this is an IR-backend-only wrong-code path. The integer
binop immediate path (`src/thin_emit.cpp:1360-1390`) is *not* affected — it range-checks
and falls back to `mov_reg_imm64` for large imms; only `emit_cmp` lacks that guard.

**Probe** (`/tmp/ember_audit/cmp_imm64.ember`):
```
fn main() -> i64 {
    let x: i64 = 0x1234567890;
    if (x == 0x1234567890) { return 1; }
    return 0;
}
```
- tree-walker: exit=1 (correct).
- IR (`--passes constprop,dce`): exit=0 (**wrong** — `0x1234567890` truncated to
  `0x34567890` for the compare).

Second probe (`cmp_imm64b.ember`, `y == 0x2468acf120`): tree-walker exit=2 (correct), IR
exit=9 (wrong).

**Impact:** any `==`/`!=`/`<`/`<=`/`>`/`>=` comparison against an integer literal > INT32_MAX
miscompiles under the IR backend (`--passes`). Silent wrong-code.

---

### C5. u64 literals in `[2^63, 2^64-1]` rejected by sema despite the lexer accepting them

**Files:**
- lexer: `src/lexer.cpp:~310` (`t.ivalue = int64_t(std::stoull(num, nullptr, 10));` —
  deliberately parses the full u64 range and bit-casts into `int64_t`, with a comment that
  explicitly claims `let x: u64 = 18446744073709551615;` should work)
- sema: `src/sema.cpp:676` (`adapt_int_lit` U64 case: `if (v >= 0) { lit.ty = target; return; }`)

**Cause:** a literal ≥ 2^63 is stored by the lexer as a **negative** `int64_t`
(`int64_t(2^64-1) == -1`). `adapt_int_lit`'s U64 arm gates on `v >= 0`, so a negative `v`
is not adapted and the literal's type stays `i64` (default). `check_value` then runs
`can_implicitly_convert(u64, i64)` which requires `want->is_uint() == got->is_uint()`
(`sema.cpp:300`) — `true != false` → no conversion → "let type mismatch (u64 = i64)".

The lexer was patched to allow the full u64 range specifically to make this literal legal,
but `adapt_int_lit` was not updated to recognize the bit-cast negative as a valid u64
range. The two halves disagree.

**Probe** (`/tmp/ember_audit/u64_max.ember`):
```
fn main() -> i64 { let x: u64 = 18446744073709551615; return x as i64; }
```
- sema error: `line 2: let type mismatch (u64 = i64)`, exit=2.

Boundary probe (`u64_boundary.ember`): `9223372036854775807` (2^63-1, positive as i64)
accepts and works; `9223372036854775808` (2^63, negative as i64) rejects. So the break is
exactly at 2^63.

**Impact:** any u64 literal in the upper half of the range is a compile error. The
workaround (`let x: u64 = (max_i64 as u64) + 1; ...`) works but the literal form is
documented to work and does not.

---

### C6. f64 global initializers lose precision (folded through f32)

**File:** `src/globals.hpp:88` (`fold_scalar_init`) and `src/globals.hpp:197`
(`eval_global_initializers` scalar path). Both use `try_eval_const_f32` for **all** float
globals, then widen via `double dv = double(v);` for f64:
```
float v;
if (try_eval_const_f32(init, v)) {
    ...
    if (ty->prim == Prim::F64) { double dv = double(v); std::memcpy(dst, &dv, 8); }
```
There is no `try_eval_const_f64` in the codebase (`sema.hpp` exports only
`try_eval_const_f32`). `try_eval_const_f32` always narrows through `float`
(`sema.cpp:188`), so an f64 global initialized to a non-f32-representable literal is
rounded to f32 and widened back, losing precision. A local f64 literal does **not** go
through this path (it's a `FloatLit` eval with the full f64 bits), so `global g: f64 = 0.1;`
≠ `let local: f64 = 0.1;`.

**Probe** (`/tmp/ember_audit/f64_global.ember`):
```
global g: f64 = 0.1;
fn main() -> i64 {
    let local: f64 = 0.1;
    if (g == local) { return 1; }
    return 0;
}
```
- exit=0 (`g == local` is **false**). Confirming probe (`f64_global2.ember`) shows
  `g == (let f: f32 = 0.1; f as f64)` is **true** (exit=7) — i.e. the global holds the
  f32-rounded value, not the f64 literal.

**Impact:** f64 globals with decimal initializers silently hold a less-precise value than
the identical local expression. Any numeric code that compares a global constant to a
local literal of the same nominal value can disagree.

---

## POTENTIAL ISSUES

### P1. `Type::byte_size()` returns 8 for any struct-named type; the post-switch `if (!struct_name.empty()) return 0;` is dead code

**File:** `src/types.cpp:69-79`. Handle types (`prim = I64, struct_name = "vec3"`) and
script structs (same prim/struct_name shape) hit `case Prim::I64: return 8;` inside the
switch, so the trailing `if (!struct_name.empty()) return 0;` is unreachable. Every caller
that needs a struct's real size routes through `StructLayoutTable` lookups
(`value_bytes`/`local_width_bytes` in `codegen.cpp:147-167`, `frame_byte_width` in
`sema.cpp:73-83`), so this is latent rather than active. But any future caller trusting
`byte_size()` on a struct gets 8, not the layout size. A `sizeof(<struct>)` is handled
correctly only because `sema.cpp:1395` routes sizeof through `frame_byte_width`, not
`byte_size`. Low risk; flag for the dead branch + the footgun.

### P2. `finalize()` rodata-relocation second pass has no bounds check (asymmetric with the first pass)

**File:** `src/engine.cpp:142-149`. The pre-alloc first pass guards
`if (af.kind == AbsFixup::FunctionRodataBase && uint64_t(af.code_offset)+8 <= image.size())`
before writing; the post-alloc second pass (`for(const auto& af:fn.abs_fixups) if(af.kind==AbsFixup::FunctionRodataBase){ ... bytes[af.code_offset+i] ... }`) has **no** bounds check.
A malformed fixup with `code_offset+8 > image.size()` would be skipped by pass 1 (leaving
the stale compile_func-patched host pointer) and then write out of bounds in pass 2.
Codegen always emits fixups inside the code region, so this is latent — but the asymmetry
is a real defect waiting on a future off-by-one in fixup emission.

### P3. Sema does not flag unreachable code

The task asked whether sema catches unreachable code. It does **not**: `return 1; return 2;`
compiles and runs (returns 1, the second return is dead but emitted). `check_block`
(`sema.cpp:1944`) threads a `returns` flag but only to prove "all paths return" for
non-void functions; it never reports statements after a returning statement as
unreachable. This is a documented v1 scope choice (no CFG/dominance analysis), not a
regression, but it does mean sema's "catches all invalid programs" has a known gap.

### P4. Cascading/duplicate sema errors on undefined names

**File:** `src/sema.cpp:712` (Ident check returns `type_void()` after recording the
"undefined name" error). The void-typed expression then flows into a return/let/arg
`check_value`, whose `types_compatible(ret_ty, void)` fails and emits a second
"type mismatch" error for the same root cause. Probe (`sema_undef.ember`):
`fn main() -> i64 { return undefined_var; }` reports **2** errors (undefined name + return
type mismatch). Cosmetic/diagnostic only — no codegen impact (sema halts compilation on
`r.ok == false`).

### P5. `get_entry_function` returns a pointer to a `thread_local` static that is overwritten by the next call

**File:** `src/lifecycle.hpp:96` (`static thread_local AnnotatedFn cached; ... return &cached;`).
A caller holding the returned pointer across a second `get_entry_function` call sees the
new call's data. Not a crash (thread_local is safe per-thread), but a stale-pointer
footgun. Single-shot use is fine.

### P6. Native returning a script-declared `struct` by value would hit an ABI mismatch

**Files:** `src/codegen.cpp` CallExpr native path (the generic call, not
`eval_struct_returning_call`) does not set up a Win64 hidden return pointer in rcx; sema
does not reject a `NativeSig` whose `ret` is a registered struct. If a host registered a
native returning a script `struct` and a script used it as a `let x: S = native();`
initializer, the callee (real Win64 ABI) would expect the hidden ptr in rcx and get
garbage. **Not reachable with the shipped extensions** — all of `extensions/vec|mat|quat`
return `bind_handle(...)` (i64 opaque handles, single-register), not by-value script
structs. Contingent on host registration; flagging as latent.

### P7. Lexer column tracking over-counts on a number-with-exponent

**File:** `src/lexer.cpp:236` (`isfloat = true; i = j; col += uint32_t(j - s);`). At this
point `col` already reflects the mantissa digits consumed (`col == sc + (old_i - s)`), but
the code adds `(j - s)` (the full offset from the number start) instead of the delta
`(j - old_i)`. The resulting column for the token *after* a `1e10`-style literal is too
high by the mantissa digit count. Affects only error-column reporting for a token
following a float with an exponent; token values and parsing are correct. Minor.

---

## VERIFIED CORRECT (spot-checked, no defect)

- **Operator precedence** (`src/parser.cpp:299-309`): or < and < bxor < bor < band < eq <
  rel < shift < add < mul < cast < unary < postfix — matches C (`&` > `^` > `|`; logical
  looser than bitwise). Probe `prec.ember` (`2 + 3 * 4 - 1`, `(2+3)*4`, `1 << 2 + 1`)
  gives 13028 → exit 228, correct.
- **Integer wrap / signed shift / div overflow** (tree-walker **and** IR, value-equivalent):
  `i64::MAX + 1` wraps to INT64_MIN (exit 0); `-1 >> 1` arithmetic-shifts to -1 (exit
  255); `1 << 63` = INT64_MIN (exit 0); `INT64_MIN / -1` traps with "signed division
  overflow" rc=70 in **both** backends (`emit_integer_divmod` is byte-identical across
  `src/codegen.cpp:954` and `src/thin_emit.cpp:294`); `-7 % 3 == -1` (exit 255) in both.
- **int↔float casts** (`src/codegen.cpp:2017-2043`, `src/thin_emit.cpp:1620-1660`): `42 as
  f64 as i64` = 42; `-3.7 as i64` = -3 (truncation) — both backends agree.
- **`normalize_rax`** (`src/codegen.cpp:943`, `src/thin_emit.cpp:157`): the
  `t->is_uint() ? 0xE0 : 0xE0` ternary in codegen is **redundant but correct** (both
  branches `0xE0` = `shl`, the intended first shift; the sign/zero distinction is in the
  *second* shift's `0xE8`/`0xF8`). Not a bug, just ugly. thin_emit cleaned it to a bare
  `0xE0`. Same emitted bytes.
- **Bounds checks** (`src/codegen.cpp:300-330`): single unsigned `jb` against the length
  catches both too-large and negative indices (a negative i64 reinterpreted as unsigned is
  huge). `emit_bounds_check_imm` correctly notes fixed-array lengths fit imm32. Verified
  the `idx_struct` family fails on the field read, not the bounds path.
- **Peephole `SetccMovzxPass` shipped as a no-op** (`src/peephole.cpp:189`): the design
  correctly identifies that an in-place `xor rax,rax; setcc al` **clobbers the cmp's
  flags** (xor sets ZF=1) which setcc reads, so the naive W10 rewrite is unsound and is
  deferred to a Stage-2 cross-instruction form. Sound decision. `SmartImmPass` range-
  checks and falls back correctly; guarded relocatable imm64s are skipped.
- **Hot-reload page lifetime** (`src/hot_reload.hpp`): `eligible_locked` (`min active
  epoch >= retirement_epoch` or no active guards) is correct; `publish` scans for aliasing
  of `old_entry` in other slots before retiring (prevents freeing a page still published
  elsewhere); guard enrollment is serialized by the same mutex that publishes the epoch
  before the release-store, so a guard is either at the old epoch (saw the old page, pins
  it) or enters after the store (never saw it). `reload_function` validates signature
  (arity + per-param `same` + `words_for_type` match + return match) before compile and
  restores the old FuncDecl on any failure. Ownership of the replaced page transfers to
  the domain exactly once at the single publication point.
- **Dispatch table** (`src/dispatch_table.hpp`): release/acquire pairing on
  `set`/`get`; null rejected at publication. ModuleRegistry base is stable (sized at
  construction, no grow). Concurrency model (per-thread `context_t`, shared read-only
  dispatch/JIT/registry) is consistent with the docs.
- **Defer / cleanup** (`src/codegen.cpp:1177-1222`): activation-flag (run-once), LIFO
  scope-exit ordering, locals-map save/restore around the deferred expression, pin
  save/restore. Break/continue chase `cleanup_depth` correctly; continue skips
  switch-only frames. Return stashes the value in a 16-byte aligned temp across defer
  eval. Switch requires each nonempty case to terminate (sema enforces).
- **em_loader binary parsing** (`src/em_loader.cpp:50-98`): `Reader::take` uses
  `checked_add(pos, n, end) || end > bytes.size()` — overflow-safe bounds check; every
  field read goes through `u8/u16/u32`/`take`; reloc/binding offsets are range-checked
  before patching (`em_loader.cpp:391`, `422`, `403`). Defensive.
- **Empty blocks / nested exprs / malformed input** (parser): `{}`, `if (true) {}`,
  `while (false) {}` parse and run (exit 5). `break`/`continue` outside loop/switch are
  sema errors. Missing return on a non-void fn is a sema error. Parser sync-and-continue
  on error is bounded (advances past `;`/`}` to guarantee progress).
- **Lexer raw-string / f-string / block-comment termination**: boundary conditions at EOF
  checked (`lexer.cpp:78-82` block comment, `106-110` raw string, `86-92` f-string);
  unterminated forms return `ok=false` with position. f-string nested-string scan
  correctly does not track brace depth inside a nested `"..."`.

---

## Summary table

| ID | Severity | Backend | file:line | One-line |
|----|----------|---------|-----------|----------|
| C1 | high | both | `codegen.cpp:2777`, `thin_lower.cpp:1893` | `arr[i].field` on struct array/slice → silent garbage |
| C2 | high | tree | `codegen.cpp:3222` | for-each wrong scale + truncation for esz∉{1,2,4,8} |
| C3 | high | both | `codegen.cpp:2763`, `thin_emit.cpp:1278` | `g[..]` view of global array: garbage (tree) / segfault (IR) |
| C4 | high | IR | `thin_emit.cpp:1554` | int compare with >32-bit literal immediate truncates |
| C5 | med | sema | `sema.cpp:676` | u64 literals ≥ 2^63 rejected (lexer/sema disagree) |
| C6 | med | runtime | `globals.hpp:88` | f64 global init folded through f32, loses precision |
| P1 | low | types | `types.cpp:79` | `byte_size()` returns 8 for structs; dead branch |
| P2 | low | engine | `engine.cpp:142` | `finalize` rodata pass 2 lacks bounds check |
| P3 | low | sema | `sema.cpp:1944` | unreachable code not flagged |
| P4 | low | sema | `sema.cpp:712` | duplicate cascading errors on undefined names |
| P5 | low | lifecycle | `lifecycle.hpp:96` | `get_entry_function` returns thread_local static ptr |
| P6 | latent | codegen | `codegen.cpp` CallExpr | native returning script-struct by value = ABI mismatch (unreached) |
| P7 | low | lexer | `lexer.cpp:236` | exponent-number column over-count (error col only) |

All six CONFIRMED DEFECTS (C1-C6) reproduce with `.ember` probes run through
`./buildt/ember_cli.exe` (command + exit code captured inline above). The 32/32 ctest and
274/274 lang-suite gates pass because none of C1-C6 is exercised by an in-tree test:
struct-array `arr[i].field`, struct/slice for-each, global-array `g[..]`, >32-bit literal
compares under `--passes`, u64 literals ≥ 2^63, and f64 global decimal initializers are all
outside the current coverage.
