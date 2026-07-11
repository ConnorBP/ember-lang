# AUDIT_2026-07-09 — Read-Only Revalidation against current source

**Revalidation date:** 2026-07-11  
**Original audit revision:** `381ed76` (2026-07-09) — see `docs/audit/AUDIT_2026-07-09.md`  
**Current HEAD:** `440df7c` (task stated `75b2d0e`; HEAD has one additional bench-CSV commit past it — no source-semantic impact on any finding)  
**Method:** read-only. No source files were modified. Each finding was tested either by running a minimal `.ember` probe through `./buildt/ember_cli.exe run <file>`, by running the relevant `buildt/*.exe` test binary, or by reading the current source at the cited location. The original `tmp_edit/*.ember` and `tmp_edit/*.em` reproduction artifacts from the audit were re-run where applicable.

**Build baseline:** a clean `buildt/` (MinGW g++ 15.2.0, C++17, Release) passes **32/32 CTest targets** (69.62 s). The `buildt/*.exe` binaries are newer than the source files they were compiled from, so the build is current.

---

## Summary table

| ID | Original severity | Current status | Evidence (file:line or test result) |
|----|-------------------|----------------|--------------------------------------|
| C1 | Critical | **FIXED** | `src/em_loader.cpp:389-392` (subtraction-in-64-bit bounds check); probe returns `err=...relocation offset out of range` rc=1 (was segfault rc=139) |
| C2 | Critical | **FIXED** | `src/em_loader.cpp:240-374` (`MAX_*` limits before every alloc) + `:882-892` (no-throw catch-all); all 4 probes return `limit:` errors rc=1 (was `bad_alloc`/hang) |
| C3 | Critical | **FIXED** | `src/sema.cpp:1276-1291` (explicit v1 cast matrix rejects scalar↔slice/aggregate/handle); probe: `invalid cast from 'i64' to 'u8[]'` |
| C4 | Critical | **FIXED** | Sema: `let bad: vec3 = 999` → `let type mismatch (vec3 = i64)`; runtime null-checks `extensions/vec/ext_vec.cpp:29`, `quat/ext_quat.cpp:18-42`, `mat/ext_mat.cpp:24-55` |
| C5 | Critical | **FIXED** | `extensions/array/ext_array.cpp:22-33` (`checked_bytes` rejects negative + overflow, `noexcept` + catch); probe exits 0 (was uncaught `std::length_error`) |
| H1 | High | **FIXED** | `src/codegen.cpp:3153` (`DoWhileStmt` codegen branch); `do { return 7; } while (false)` → 7 (was 3); mutating probe → 2 (was 0) |
| H2 | High | **FIXED** | `src/codegen.cpp:938-956` (`emit_integer_divmod` guards zero + INT64_MIN/-1 → trap); `/`,`%`,`/=`,`%=` all trap rc=70 (was abnormal termination) |
| H3 | High | **FIXED** | `src/codegen.cpp:3224` (switch subject moved to volatile `r10`); `win64_abi_test`: `[PASS] H3: switch preserves caller r13` |
| H4 | High | **FIXED** | `src/engine.cpp:218-254` (out-of-line asm thunks `pushq %r14`…`popq %r14`); `win64_abi_test`: `[PASS] H4: ember_call_void/i64 preserves incoming r14` |
| H5 | High | **FIXED** | `src/codegen.cpp:349-362` (parity-aware `win64_call_frame_size(32)` + `require_win64_call_alignment()`); `win64_abi_test`: 4× `[PASS] H5: …` |
| H6 | High | **FIXED** | `src/codegen.cpp:2822-2823` (constant-folded emit); sema `:1390-1407`; `sizeof(i64)`→8, `offsetof(P,y)`→8, `sizeof(P)`→24 (was 176 / not parsed) |
| H7 | High | **FIXED** | `src/codegen.cpp` type-directed f64 paths (`movsd`/`ucomisd`/`addsd`/`divsd`/`cvtsi2sd` at :1014-1845); `16777217.0 > 16777216.0` → true |
| H8 | High | **FIXED** | `src/codegen.cpp:938` (unsigned `div`), `:1930` (`shr`/`sar`), `:1932-1943` (unsigned `jb/jbe/ja/jae`), `:926-936` (`normalize_rax` width mask); all 4 probes pass |
| H9 | High | **FIXED** | `src/sema.cpp:337-352` (`check_value` + `can_implicitly_convert` widening-only); `i8 = i64` and `u64 = i64` rejected; legal widening + `as` still work |
| H10 | High | **FIXED** | `src/sema.cpp:826-853` (recorded-sig arg validation); `f(1.0f,2.0f)` on `&add` → `function handle argument type mismatch (expected i64, got f32)` |
| H11 | High | **FIXED** | `src/sema.cpp` escape rejection; `return a[..]` from local array → `cannot return a slice/view derived from a stack local` |
| H12 | High | **FIXED** | `src/em_writer.cpp:142-157,244` (symbolic native bindings, reject non-portable, serialize real globals); `src/em_loader.cpp:443-444` (preserve init bytes); build/ABI IDs `:237-238`; F2 Ed25519 signing |
| H13 | High | **FIXED** | `src/hot_reload.hpp:278-297` (arity/param/return mismatch check); `v0_6_hot_reload_test` PASS |
| H14 | High | **FIXED** | `src/module_linker.hpp:75-80` (v2+ reads `signatures_by_slot`, `unknown_sig=false`); `em_roundtrip_test`: recorded-sig checks PASS |
| H15 | High | **FIXED** | `examples/ext_sync_test.cpp` (loop on `out_pushed`, atomic worker-error propagation); `ext_sync_test`: T11/T12/T15 PASS (was FIFO loss/hang) |
| M1 | Medium | **FIXED** | `extensions/sync/ext_sync.cpp:62` (`std::recursive_mutex g_store_mutex` over alloc/free/reset/lookup); stable `shared_ptr` slots |
| M2 | Medium | **FIXED** | `extensions/lifecycle/ext_lifecycle.cpp:30` (`std::mutex g_mutex` over register/unregister/reset/snapshot) |
| M3 | Medium | **FIXED (already resolved in original audit)** | `src/hot_reload.hpp` `HotReloadDomain` guards/epochs/reclaim; `v0_6_hot_reload_test` 18/18 PASS |
| M4 | Medium | **FIXED** | `src/codegen.cpp:372-374` (budget at fn entry + back-edges), `:418-420` (depth for script **and native** calls) |
| M5 | Medium | **FIXED** | `src/codegen.cpp:3370,3510` (defer cleanup edges); block-exit defer → 100, break defer → 77, loop defer → 3 per-iteration |
| M6 | Medium | **FIXED** | postfix `x++` returns old value; `y*10+x` → 12 (was 22) |
| M7 | Medium | **FIXED** | `src/sema.cpp` rejects implicit fallthrough; `nonempty switch case must end with break, continue, or return` |
| M8 | Medium | **FIXED** | `src/sema.cpp` const global enforcement; `C = 99` → `cannot assign to const global 'C'` |
| M9 | Medium | **FIXED** | `src/sema.cpp`: redeclaration, non-lvalue assign, break/continue outside loop, recursive by-value struct, void local — all rejected |
| M10 | Medium | **FIXED (by design decision + docs reconciled)** | `src/sema.cpp:57-82` packed layout + cycle rejection; `docs/spec/TYPE_SYSTEM.md:35-36`, `BINDING_API.md:271`; `binding_abi_test` [1b][1c][1d] PASS |
| M11 | Medium | **FIXED** | `src/codegen.cpp` typed `globals_offsets` (:1318,:1667-1671,:2217); `aggregate_global_test` 8/8 PASS incl. .em round-trip |
| M12 | Medium | **FIXED** | `src/em_loader.hpp:133-136` (`copy = delete`, explicit `noexcept` move); no double-free |
| M13 | Medium | **FIXED** | `src/lexer.cpp:200-201,209-214,243` (clean lex errors, `stoull` wrapped in try/catch); `0x` → `hex literal requires at least one digit` |

**Tally:** 33/33 findings no longer reproduce as described. **5/5 Critical FIXED, 15/15 High FIXED, 13/13 Medium FIXED** (M3 was already marked resolved in the original audit and remains so). No finding is PARTIALLY FIXED, STILL OPEN, or NOT APPLICABLE.

---

## Detailed evidence

### Critical findings

#### C1. `.em` relocation-offset wrap → out-of-bounds write — FIXED

The original additive check `r.offset + 8 > f.code.size()` (which wrapped in 32 bits for `0xfffffffc`) is gone. The loader now uses subtraction in 64-bit space, exactly as the audit's fix recommended:

`src/em_loader.cpp:389-392`:
```cpp
// Use subtraction in 64-bit space: r.offset + 8 must never wrap.
if (!(f.code.size() >= 8 &&
      uint64_t(r.offset) <= uint64_t(f.code.size()) - 8)) {
    set_error(err, "em_loader: format: relocation offset out of range in \"" + ...);
```

Probe (`tmp_edit/em_reloc_wrap.em`, the original audit reproduction):
```
ok=0 err=em_loader: format: relocation offset out of range in "f" pages=0 dispatch=0 globals=0
exit: 1
```
Was: signal/return code 139 (segfault). Now a clean rejection with no page published (`pages=0`).

#### C2. `.em` counts/sizes → unbounded allocations + uncaught exceptions — FIXED

Every disk-controlled count/size is now checked against a format-level maximum *before* any `reserve`/`resize`/`assign`. The limits live in `src/em_file.hpp:265-273`:

```cpp
constexpr uint64_t MAX_FILE_SIZE       = 256ull * 1024ull * 1024ull;
constexpr uint32_t MAX_FUNCTIONS       = 16u * 1024u;
constexpr uint32_t MAX_GLOBALS         = 64u * 1024u * 1024u;   // bytes
constexpr uint32_t MAX_CODE_PER_FN     = 16u * 1024u * 1024u;
constexpr uint32_t MAX_RODATA_PER_FN   = 16u * 1024u * 1024u;
constexpr uint32_t MAX_RELOCS_PER_FN   = 256u * 1024u;
constexpr uint32_t MAX_SLOTS           = 64u * 1024u;
constexpr uint32_t MAX_NAMES           = 64u * 1024u;
constexpr uint32_t MAX_NAME_SIZE       = 4u * 1024u;
```

Checked at `src/em_loader.cpp:240-374` (function_count, global_size, slot_index, code_size, rodata_size, reloc_count, name_count, file_size). Overflow-safe helpers `checked_add`/`checked_mul` at `:44-52`. The public `load_em_file` is a complete no-throw boundary (`:882-892`: catches `bad_alloc`, `length_error`, `std::exception`, `...`).

All four original probes now return categorized `limit:` errors, rc=1, no exception, no hang:

| probe | result |
|-------|--------|
| `em_huge_fc.em` | `limit: function_count exceeds MAX_FUNCTIONS` |
| `em_huge_globals.em` | `limit: global_size exceeds MAX_GLOBALS` |
| `em_huge_slot.em` | `limit: slot_index exceeds MAX_SLOTS` |
| `em_huge_reloc.em` | `limit: reloc_count exceeds MAX_RELOCS_PER_FN` |

Was: `std::bad_alloc` rc=100 / 4 GiB allocation / timeout. This directly satisfies the loader's `em_loader.hpp` contract that malformed files return `false` + error and never throw.

#### C3. Arbitrary casts forge slices/host pointers — FIXED

`src/sema.cpp:1276-1291` now implements an explicit v1 cast matrix. Only four cast families are permitted: same-type no-op, int↔int, float↔float, and signed-int↔float. Everything else — scalar↔slice, scalar↔aggregate, unrelated-handle, bool reinterpretation — is rejected:

```cpp
const bool same = from && from->same(*to);
const bool int_to_int = is_plain_integer(from) && is_plain_integer(to);
const bool float_to_float = from && to && from->is_float() && to->is_float();
const bool int_float = (is_plain_integer(from) && !from->is_uint() && to && to->is_float()) ||
                       (from && from->is_float() && is_plain_integer(to) && !to->is_uint());
if (!same && !int_to_int && !float_to_float && !int_float) {
    err("invalid cast from '" + ... + "' to '" + to->to_string() + "'", ...);
```

Probe:
```
ember: sema errors (2):
  line 3: invalid cast from 'i64' to 'u8[]'
```
Was: accepted by sema, segfault rc=139 when indexed. The scalar→slice forge is blocked at sema, so no invalid raw pointer is ever constructed.

#### C4. Opaque extension handles forgeable + null deref — FIXED (defense in depth, both layers)

**Sema layer** — opaque handle types are now semantically distinct from integers. The "any integer is compatible with any integer-backed type" rule is gone:
```
ember: sema errors (1):
  line 2: let type mismatch (vec3 = i64)
```
A forged handle arriving through a native-arg boundary is also rejected: `arg 1 of 'vec3_x': expected vec3, got i64`.

**Runtime layer** — every extension operator/accessor now null-checks the slot lookup and returns a safe default (0) instead of dereferencing:
- `extensions/vec/ext_vec.cpp:29`: `n_vec3_add` → `auto* x=v3_slot(a); auto* y=v3_slot(b); return (x&&y)?v3_new(...):0;`
- `extensions/quat/ext_quat.cpp:18-42`: all `q_slot` callers guard `if (!p || !q) return 0;`
- `extensions/mat/ext_mat.cpp:24-55`: all `m4_slot` callers guard `if (!m ...) return 0;` / `if (!x || !y) return 0;`

Both layers the audit required are present. Was: `let bad: vec3 = 999; bad + good` → segfault rc=139.

#### C5. Negative/overflowing extension sizes → uncaught C++ exceptions — FIXED

`extensions/array/ext_array.cpp` now rejects negative sizes, checks multiplication overflow, caps each container at 1 GiB, and is `noexcept` with allocation exceptions caught at the extension boundary:

```cpp
static constexpr size_t MAX_CONTAINER_BYTES = size_t(1) << 30;
static bool checked_bytes(int64_t elem_size, int64_t count, size_t* out) {
    if (elem_size < 1 || count < 0) return false;           // reject negative
    const uint64_t es = uint64_t(elem_size), n = uint64_t(count);
    if (n != 0 && es > uint64_t(MAX_CONTAINER_BYTES) / n) return false;  // mul overflow
    *out = size_t(es * n);
    return *out <= MAX_CONTAINER_BYTES;
}
static int64_t arr_new(int64_t elem_size, int64_t count) noexcept {
    size_t bytes = 0;
    if (!checked_bytes(elem_size, count, &bytes)) return 0;  // safe error handle
    try { ... } catch (const std::bad_alloc&) { return 0; }
             catch (const std::length_error&) { return 0; }
}
```

Probe (`array_new(1, -1)`): exits 0, returns 0 (error handle). The overflow probe `array_new(8, 536870912)` (4 GiB) also exits 0 — rejected by the cap. Was: uncaught `std::length_error` + abnormal exit. The same `checked_bytes` guard protects `array_resize` and `array_push_u8`.

---

### High findings

#### H1. `do-while` emits no code — FIXED

`src/codegen.cpp` now has a `DoWhileStmt` branch in `exec_stmt` (`:3153`) plus the supporting prescan/temp/pin/defer walks (`:582,:647,:718,:784,:840,:3370,:3510`). Probes:

| probe | result | was |
|-------|--------|-----|
| `do { return 7; } while (false); return 3;` | **7** | 3 |
| `do { count++; i++; } while (i < 2); return count;` | **2** | 0 |

#### H2. Integer division/modulo bypass trap model — FIXED

`src/codegen.cpp:938-956` `emit_integer_divmod(bool want_mod, bool is_unsigned)`:
```cpp
e.cmp_reg_imm32(Reg::rcx, 0); e.jcc(Cond::ne, nonzero);
emit_trap(int(TrapReason::DivByZero), "integer division by zero"); e.bind(nonzero);
if (!is_unsigned) {
    e.cmp_reg_imm32(Reg::rcx, -1); e.jcc(Cond::ne, safe);
    e.mov_reg_imm64(Reg::r10, INT64_MIN); e.cmp_reg_reg(Reg::rax, Reg::r10); e.jcc(Cond::e, overflow);
    ...
    emit_trap(int(TrapReason::DivByZero), "signed division overflow"); ...
}
```
All four forms route through the unified trap stub:

| probe | result |
|-------|--------|
| `10 / 0` | `RUNTIME TRAP: integer division by zero` rc=70 |
| `10 % 0` | `RUNTIME TRAP: integer division by zero` rc=70 |
| `a /= 0` | `RUNTIME TRAP: integer division by zero` rc=70 |
| `a %= 0` | `RUNTIME TRAP: integer division by zero` rc=70 |
| `INT64_MIN / -1` | `RUNTIME TRAP: signed division overflow` rc=70 |

Was: abnormal termination (no `TrapReason`).

#### H3. Generated `switch` clobbers nonvolatile `r13` — FIXED

The switch subject is now held in `r10` (volatile), not `r13` (nonvolatile). `src/codegen.cpp:3222-3224`:
```cpp
eval(*sw->subject);
// Win64 r10 is volatile, unlike the old r13 scratch.  Sema permits
// only IntLit/BoolLit case values; their eval paths are immediate
// loads into rax and cannot clobber r10, so the subject remains live
// across the complete compare chain without a nonvolatile save.
e.mov_reg_reg(Reg::r10, Reg::rax);
```
`buildt/win64_abi_test.exe`: `[PASS] H3: switch preserves caller r13` (seeds r13 with `0x1122334455667788`, runs a switch-bearing function, confirms r13 unchanged). Note: the stale `tmp_edit/test_r13.exe` still shows the old failure because it was compiled against the audit-period library and its API signatures no longer link against current source — the current `win64_abi_test.exe` is authoritative.

#### H4. B1 entry wrappers clobber nonvolatile `r14` — FIXED

`src/engine.cpp:218-254` replaces the old inline-assembly-in-a-C++-function with proper out-of-line assembly thunks that save/restore r14:
```asm
ember_call_void_thunk:
  pushq %r14          # save caller's incoming r14
  subq $32, %rsp      # mandatory Win64 shadow space (keeps alignment)
  movq %rcx, %r11     # entry
  movq %rdx, %r14     # install B1 context
  callq *%r11
  addq $32, %rsp
  popq %r14           # restore r14
  retq
```
`buildt/win64_abi_test.exe`: `[PASS] H4: ember_call_void preserves incoming r14` and `[PASS] H4: ember_call_i64 preserves incoming r14`. The `#else` branch is now an explicit `#error` ("B1 context thunks require MinGW GNU assembly on x64; MSVC x64 not yet supported"), making the MSVC limitation honest rather than silently broken.

#### H5. Trap-stub calls misaligned — FIXED

`src/codegen.cpp:349-362` `emit_trap` now computes the call frame from a parity-aware emitter invariant instead of a hardcoded `0x28`:
```cpp
const int32_t call_frame = e.win64_call_frame_size(32);  // 32-byte shadow + tracked parity padding
e.sub_reg_imm32(Reg::rsp, call_frame);
...
e.require_win64_call_alignment();   // explicit invariant: rsp 16-byte aligned before the call
e.call_reg(Reg::rax);
```
`buildt/win64_abi_test.exe` covers both parities and the temporary-push case:
```
[PASS] H5: aligned trap stub returned safely at normal parity
[PASS] H5: aligned trap stub returned safely with pushed temporary
[PASS] H5: keyed-gate mismatch restores rbx before aligned trap call
[PASS] H5: OOB f32 indexed store tracks its eight-byte temporary
```

#### H6. `sizeof` undefined runtime value; `offsetof` not parsed — FIXED

Sema constant-folds both (`src/sema.cpp:1390-1407` resolves `SizeofExpr`/`OffsetofExpr` against the finalized layout table); codegen emits the folded immediate (`src/codegen.cpp:2822-2823`: `e.mov_reg_imm64(Reg::rax, int64_t(s->resolved))`). Probes:

| probe | result | was |
|-------|--------|-----|
| `sizeof(i64)` | **8** | 176 (undefined) |
| `offsetof(P, y)` (struct `{x:i64;y:i64}`) | **8** | not parsed |
| `sizeof(P)` (struct 3×i64) | **24** | — |

#### H7. `f64` executes as `f32` — FIXED

Codegen is now type-directed for float width. `src/codegen.cpp` selects `movsd`/`ucomisd`/`addsd`/`subsd`/`mulsd`/`divsd`/`cvtsi2sd` for `Prim::F64` and `movss`/`ucomiss`/`addss`/… for `Prim::F32` (e.g. `:1014-1015,:1831-1845`). Probe: `16777217.0 > 16777216.0` (f64) → **true** (was false under single-precision). This matches `docs/spec/TYPE_SYSTEM.md`'s IEEE-754 binary64 spec.

#### H8. Unsigned/narrow integer semantics not implemented — FIXED

Codegen is now type-directed across all four sub-issues:
- **Unsigned comparison**: `src/codegen.cpp:1932-1943` selects `jb/jbe/ja/jae` (0x2/0x6/0x7/0x3) for unsigned, `jl/jle/jg/jge` (0xC/0xE/0xF/0xD) for signed.
- **Unsigned division**: `emit_integer_divmod(..., is_unsigned)` uses `div rcx` (unsigned) vs `idiv rcx` (`:949-952`); `Div`/`Mod` pass `lt && lt->is_uint()` (`:1927-1928`).
- **Right shift**: `Shr` emits `0xE8` (`shr`, logical) for unsigned, `0xF8` (`sar`, arithmetic) for signed (`:1930`).
- **Narrow truncation**: `normalize_rax` (`:926-936`) masks to the declared width (unsigned: `shl`/`shr` zero-extend; signed: `shl`/`sar` sign-extend).

Probes:

| probe | result | was |
|-------|--------|-----|
| `u64(-1) > 0` | true | false |
| `u64(INT64_MIN) >> 62 == 2` | true | -1 (arithmetic) |
| `u8(255) + 1 == 0` | true | no wrap |
| `u64(-1) / 3 == 6148914691236517205` | true | (would be 0 signed) |
| `i64(-9) / 2 == -4` | true (still correct) | — |

#### H9. Sema permits narrowing/signedness-changing implicit conversions — FIXED

`src/sema.cpp:337-352` `check_value` is the single implicit-conversion gate; it calls `can_implicitly_convert(want, got)` (widen-only) and only inserts an explicit `CastExpr` for legal same-signedness widening. Narrowing and signedness changes are not converted and surface as a type mismatch:

| probe | result |
|-------|--------|
| `let y: i8 = x_i64` | `let type mismatch (i8 = i64)` |
| `let y: u64 = x_i64` | `let type mismatch (u64 = i64)` |
| `let y: i64 = x_i8` (legal widen) | accepted, returns 5 |
| `let y: i8 = x as i8` (explicit) | accepted, returns 100 |

#### H10. Recorded-signature fn-ref calls skip arg-type validation — FIXED

`src/sema.cpp:819-853` now enforces each recorded parameter type after expression checking:
```cpp
if (tt->has_recorded_sig) {
    ... arity check ...
    for (auto& a : c->args) {
        const Type* want = tt->recorded_params[i++].get();
        const Type* got = check_value(a, want);
        if (!types_compatible(want, got))
            err("function handle argument type mismatch (expected " + want->to_string() +
                ", got " + got->to_string() + ")", ...);
    }
    e.ty = intern(*tt->recorded_ret);
}
```
Probe (`let f = &add; f(1.0f, 2.0f)` where `add: (i64,i64)->i64`):
```
function handle argument type mismatch (expected i64, got f32)
```
The bare-`fn` (no recorded sig) path remains a documented Tier 2 deferral — the runtime provenance guard still validates the slot. Was: two `f32` args accepted, callee read GP registers while codegen placed XMM → segfault.

#### H11. Local-array slice escape analysis absent — FIXED

Sema now rejects returning/storing a view derived from a stack local:
```
fn bad() -> u8[] { let a: u8[1]; return a[..]; }
→ ember: sema errors (1):
  line 3: cannot return a slice/view derived from a stack local
```
The provenance bit is tracked through assignment/global paths (`src/sema.cpp` local-array-view tracking around `:1336+`). Was: accepted, returned a pointer into a dead JIT stack frame.

#### H12. `.em` embeds process-local pointers + loses initialized globals — FIXED

This finding had several facets; all are addressed:

- **Process-local native pointers** → symbolic native bindings. `src/em_writer.cpp:150-157,235-236` emits per-function `native_bindings` (name + canonical signature); `src/em_loader.cpp:417-422` reads them back and resolves by name against the host table. The v5 IR path uses `CallNative` with a name that is rebind-checked against the host's native table (Stage B "C2" fix). This is the "symbolic native/import identities resolved through a host-provided allowlisted binding table" the audit recommended.
- **Trap stub / detail / allowlist pointers** → `src/em_writer.cpp:142-143` rejects non-portable functions at emit time (`f.non_serializable_reason`), so raw-x86 with embedded pointers is loudly rejected rather than silently shipped. The v5 IR path avoids these pointers entirely.
- **Globals reverting to zero** → `src/em_writer.cpp:244` `emit_bytes(ofs, mod.globals)` serializes the actual initialized bytes; `src/em_loader.cpp:443-444` preserves them exactly (`// Preserve serialized initialized bytes exactly; do not substitute a new zero-filled block`).
- **ABI/build IDs** → `src/em_loader.cpp:237-238` cross-checks `EM_BUILD_ID` and `EM_TARGET_ABI_HASH` (`src/em_file.hpp:201-249`) and rejects mismatches.
- **Content authentication** → F2 Ed25519 signed `.em` (commit `48edb8a`); v4 modules are verified against the host's trusted keyring before any exec page is allocated (`src/em_loader.cpp:596-624`).

`em_roundtrip_test` and `em_v5_ir_test` both PASS; `float_global_regression` confirms initialized globals survive load (tests 6-8).

#### H13. Hot reload accepts incompatible signatures — FIXED

`src/hot_reload.hpp:278-297`:
```cpp
auto mismatch = [&](const std::string& what) {
    r.error = "reload: incompatible signature for '" + new_fn.name + "': " + what;
    ...
};
if (new_fn.params.size() != it->params.size()) mismatch("arity changed ...");
for (size_t i = 0; i < it->params.size(); ++i) { ... mismatch("parameter N changed ..."); }
... mismatch("return type changed ...");
```
`buildt/v0_6_hot_reload_test.exe`: 18/18 PASS, including the domain-based reload and reclaim paths. Was: `fn f() -> i64` accepted as a reload of `fn f(x:i64) -> i64`.

#### H14. `.em` linking has no export-signature verification — FIXED

`src/module_linker.hpp:75-80` now reads the per-slot signature from the v2+ format:
```cpp
if (mod.format_version >= EM_VERSION_V2 && slot < mod.signatures_by_slot.size()) {
    exp.ret = mod.signatures_by_slot[slot].ret;
    exp.params = mod.signatures_by_slot[slot].params;
    exp.unknown_sig = false;
} else {
    exp.unknown_sig = true;  // v1 ABI-trusted unknown signature
}
```
`em_roundtrip_test`: `[4e-recorded_sig] PASS`, `[4e-recorded_params_arity] PASS`, `[4e-recorded_ret] PASS`. Only the historical v1 format remains `unknown_sig` (documented as ABI-trusted).

#### H15. SPSC stress test incorrect (FIFO loss / hang) — FIXED

`examples/ext_sync_test.cpp` now (a) loops until `out_pushed == true` rather than on the handle-valid return value, and (b) propagates worker failures through `std::atomic<int>& error` (`worker_error` helper). `buildt/ext_sync_test.exe`:
```
[PASS] T11 SPSC host producer retries out_pushed across 8 FIFO/no-loss rounds
[PASS] T12 SPSC main producer/host consumer propagates worker errors across 8 rounds
[PASS] T15 MPMC workers complete by deadline; pushed count == popped count
[PASS] T15 MPMC sum pushed == sum popped under contention
```
Was: `expected 3888 got 4407` (FIFO loss) / `ext_sync` blocked. The 17→32 test gate is now deterministic (32/32 ctest PASS).

---

### Medium findings

#### M1. `ext_sync` store vectors race across independent contexts — FIXED

`extensions/sync/ext_sync.cpp:62` introduces `static std::recursive_mutex g_store_mutex;` serializing all store-management operations (alloc/free/reset/lookup/publication). Slots are held behind `std::shared_ptr` (stable publication — a reallocation does not invalidate another context's already-acquired slot). Per the file header (`:10-13`): "store-management mutex serializes lookup/publication, alloc/free, and reset … primitive operations on an already acquired stable slot can remain fast." `ext_sync_test` T14-T16 PASS under contention.

#### M2. Lifecycle "approximate snapshot" not C++-race-safe — FIXED

`extensions/lifecycle/ext_lifecycle.cpp:30` introduces `static std::mutex g_mutex;` protecting registration, unregistration, reset, and snapshot (`:37,:50,:73,:79,:89`). Per the header (`:8-10`): "A mutex protects registration, unregistration, reset, snapshots … no lock is held while invoking a routine." The CLI's sequencing avoids the race; the mutex makes it safe for concurrent hosts too.

#### M3. Multi-context hot reload in-flight reclamation — FIXED (already resolved in the original audit)

The original audit marked this "resolved." It remains resolved: `src/hot_reload.hpp` `HotReloadDomain` provides execution guards, monotonic publication epochs, domain-owned retired pages, nonblocking reclamation, and blocking quiescence. `buildt/v0_6_hot_reload_test.exe`: 18/18 PASS, including "(4) nonblocking reclaim refuses page pinned by in-flight guard," "(4) blocking quiesce waits while old guard is active," and "(7) domain-based reload succeeds."

#### M4. Budget/depth accounting weaker than spec — FIXED

- **Budget at function entry**: `src/codegen.cpp:372-374` — "Emitted at function entry and preserved at loop back-edges." `emit_budget_check` is called at entry (`:384`) as well as back-edges, so long straight-line/call-heavy execution is now accounted.
- **Native calls in depth accounting**: `src/codegen.cpp:418-420` — "Emitted for every script-issued script **or native call**. Native re-entry therefore remains nested while the earlier native invocation is active." `emit_depth_check`/`emit_depth_leave` bracket every call.

#### M5. `defer` is function-exit scoped, not lexical — FIXED

Defer now runs on nested-block fallthrough, `break`, and `continue`, and a defer inside a loop runs each iteration. `src/codegen.cpp` collects defers per scope (`:3510`) and emits cleanup edges (`:3370`). Probes:

| probe | result | was |
|-------|--------|-----|
| `{ defer x=100; x=1; } return x;` | **100** (block-exit) | 1 (fn-exit only) |
| `while { defer x=77; ...; break; } return x;` | **77** (break cleanup) | 0 |
| `while(i<3){ defer count++; i++; } return count;` | **3** (per-iteration) | 1 (at most once) |

#### M6. Postfix `++`/`--` return new value — FIXED

Postfix forms now preserve the old value. Probe: `x=1; y=x++; return y*10+x;` → **12** (y=1, x=2). Was: 22 (prefix/postfix equivalent).

#### M7. `switch` fallthrough disagrees across code/tests/docs — FIXED

The language rule is now uniformly "no implicit fallthrough." Sema rejects it:
```
nonempty switch case must end with break, continue, or return; implicit fallthrough is not allowed
```
Codegen emits adjacent case bodies (fallthrough would occur if it reached them), but sema prevents reaching that state. Tests and docs are reconciled to this rule.

#### M8. Top-level `const` discarded — FIXED

`const` is now preserved in the symbol table and enforced. Probe: `const C: i64 = 42; … C = 99;` → `cannot assign to const global 'C'`. Was: assignment legal.

#### M9. Several invalid forms pass sema — FIXED

All five categories the audit named are now rejected:

| probe | result |
|-------|--------|
| same-scope redeclaration `let x; let x;` | `redeclaration of 'x' in the same scope` |
| assign to non-lvalue `5 = 6` | `assignment target is not an lvalue` |
| `break` outside loop | `break is only valid inside a loop or switch` |
| `continue` outside loop | `continue is only valid inside a loop` |
| recursive by-value struct `struct Node { next: Node; }` | `struct 'Node' has a recursive, unknown, or invalid by-value layout` |
| `let v: void;` | `local 'v' cannot have void type` |

#### M10. Struct layout / >8-byte arg ABI vs spec — FIXED (design decision + docs reconciled)

The language chose **packed Ember-layout** (no alignment padding) and **reject >8-byte by-value native arguments**, then made code/tests/docs consistent:
- `src/sema.cpp:57-82` `build_struct_layouts` packs fields tightly (`off += sz`, no alignment rounding) with cycle rejection (`active.insert(name).second`) and overflow checks.
- `docs/spec/TYPE_SYSTEM.md:35-36`: "fields are tightly packed in declaration order, with no alignment gaps or trailing padding."
- `docs/spec/BINDING_API.md:271`: "struct >8 bytes argument | deferred/unsupported | v1 sema rejects native by-value aggregate arguments over 8 bytes."
- `buildt/binding_abi_test.exe`: `[PASS] [1b] trailing nested field at offset 5 uses its exact 3-byte packed extent`, `[PASS] [1c] native by-value aggregate argument >8 bytes rejected`, `[PASS] [1d] recursive aggregate layout rejected explicitly`. Struct returns >8B ([2]/[2c]) and slice args ([5]) work via the Ember calling convention.

#### M11. Aggregate globals receive only one 8-byte slot — FIXED

Codegen now resolves aggregate globals through a typed byte-offset table (`ctx.globals_offsets`), not the legacy flat `index*8`. `src/codegen.cpp:1318,1353,1667-1671,2217-2219` — the comment at `:1668` reads "aggregate globals land at non-8-byte slots." Struct globals occupy their packed Ember size; slice globals occupy 16 bytes (ptr+len). `buildt/aggregate_global_test.exe`: 8/8 PASS, including struct/fixed-array/slice global field reads, by-value arg/return, and `.em` round-trip (relative-ptr relocation for slice globals).

#### M12. `LoadedModule` copyable despite owning exec pages — FIXED

`src/em_loader.hpp:131-136`:
```cpp
LoadedModule() = default;
~LoadedModule();
LoadedModule(const LoadedModule&) = delete;
LoadedModule& operator=(const LoadedModule&) = delete;
LoadedModule(LoadedModule&& other) noexcept;
LoadedModule& operator=(LoadedModule&& other) noexcept;
```
Copy is deleted; move is explicit and `noexcept`. No double-free of executable pages.

#### M13. Malformed hex aborts compiler tooling — FIXED

`src/lexer.cpp:198-214` validates hex digits before parsing and wraps `std::stoull` in try/catch:
```cpp
while (i < src.size() && std::isxdigit(...)) adv(1);
if (i == digits) { r.error = "hex literal requires at least one digit"; ... return r; }
...
try { unsigned long long value = std::stoull(num.substr(2), &used, 16); ... }
catch (const std::exception&) { r.error = "integer literal out of range or malformed: " + num; ... }
```
Integer-width suffixes (decimal and hex) are now consistently rejected with a clear message (`:243`: "integer literal width suffixes are unsupported; use an explicit `as` cast"). Probe: `0x` → `hex literal requires at least one digit` (exit 2). Was: `std::stoull` `invalid_argument` abort.

---

## Notes on methodology and scope

- This revalidation is **read-only**. No source files, the original audit doc, or test sources were modified. Probes were written to `/tmp/` or re-used from the existing ignored `tmp_edit/` reproductions.
- The `tmp_edit/test_r13.exe` and `tmp_edit/audit_em_loader_probe*.exe` binaries that pre-date the revalidation were used as reproduction inputs where their file-format/API still applies (the `.em` probes and `.ember` scripts). The `test_r13.exe` *binary* is stale (it links against the audit-period library and its C++ API signatures no longer match current headers), so for H3/H4 the current `buildt/win64_abi_test.exe` — which compiles against current source — is the authoritative evidence.
- HEAD at revalidation time is `440df7c`, one commit past the `75b2d0e` named in the task. The additional commit (`Bench: add compile_ns/code_bytes/ir_instrs columns to CSV output`) touches only benchmark CSV reporting and has no bearing on any audit finding.
- The full CTest suite (32 targets) passes 100% in 69.62 s, establishing a deterministic release gate — the H15 nondeterminism concern is closed.
- The audit's separate Section 5 (API/build/CLI discrepancies), Section 6 (performance), Section 7 (documentation/test gaps), Section 10 (static-analysis candidates), and the D1-D11 documentation gaps were **not** in scope for this revalidation (the task covers C1-C5, H1-H15, M1-M13 only). Several of those are visibly improved (CLI help now lists `--emit-em`/`--tick`/`--tick-count`/`--tick-interval`/`--load-em`/`bench`; CMake version and AngelScript-gating were not re-checked here).
