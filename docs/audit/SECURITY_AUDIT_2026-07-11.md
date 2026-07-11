# ember Language Security Audit

**Date:** 2026-07-11
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** bf76217
**Build:** `buildt/` (MinGW g++ 15.2.0, C++17, Release)
**Tests:** 32/32 ctest pass
**Auditor scope:** read-only deep audit; no source files edited.

---

## Executive Summary

The audit examined the .em loader/writer, JIT sandbox (W^X), trap model,
extension safety, pass system, type safety, and thread safety. One critical
vulnerability was confirmed and verified end-to-end: the v5 IR validator's
`frame_off` bounds check is incomplete, allowing a crafted v5 `.em` file to
overwrite the return address on the stack and achieve native code execution.
Several additional potential issues were found in extension bounds checking
and exception safety.

| # | Severity | Finding | Area |
|---|----------|---------|------|
| 1 | **CRITICAL** | v5 IR `frame_off` validation gap — arbitrary stack write / return-address overwrite | em_loader / thin_ir_ser / thin_emit |
| 2 | **HIGH** | ext_array element bounds check bypass via `size_t` multiplication overflow | extensions/array |
| 3 | MEDIUM | ext_vec/mat/quat `*_new` can `std::terminate` on OOM (uncaught `bad_alloc` through native boundary) | extensions/vec,mat,quat |
| 4 | MEDIUM | `n_string_substr` can `std::terminate` on OOM (uncaught `bad_alloc` from `substr`) | extensions/string |
| 5 | MEDIUM | Extension stores (array/string/map/vec/mat/quat) are not thread-safe; concurrent contexts race | extensions |
| 6 | LOW | v5 IR validator does not reject duplicate block IDs (correctness, not memory safety) | thin_ir_ser |

---

## Finding 1 — CONFIRMED VULNERABILITY (CRITICAL)

### v5 IR `frame_off` validation gap: arbitrary stack write via return-address overwrite

**Files:**
- `src/thin_ir_ser.cpp:576-585` — the validator's `frame_off` check (incomplete)
- `src/thin_emit.cpp:817-824` — `ConstInt` emit (writes `[rbp + frame_off]`)
- `src/thin_emit.cpp:80-83` — `store_rax_to_rbp` (emits `mov [rbp+disp32], rax`)
- `src/thin_emit.cpp:412-416` — prologue (`push rbp; mov rbp,rsp; sub rsp,frame_size`)

**Root cause:**

`validate_thin_function` (thin_ir_ser.cpp:576-585) checks `meta.frame_off`
bounds ONLY for seven specific ops:

```cpp
if ((in.op == ThinOp::LoadFrame || in.op == ThinOp::StoreFrame ||
     in.op == ThinOp::CopyBytes || in.op == ThinOp::FieldAddr ||
     in.op == ThinOp::IndexAddr || in.op == ThinOp::StructLitInit ||
     in.op == ThinOp::ArrayLitInit) &&
    thf.frame.frame_size > 0) {
    if (in.meta.frame_off >= 0 || in.meta.frame_off < -thf.frame.frame_size) {
        return false;  // rejected
    }
}
```

But `emit_instr` (thin_emit.cpp) writes to `[rbp + meta.frame_off]` via
`store_rax_to_rbp(e, in.meta.frame_off)` for **~20 other producing ops**
that are NOT in the checked list:

`ConstInt` (line 822), `ConstBool` (line 848), `ConstFloat` (line 839),
`Move` (line 934), `Add/Sub/Mul/And/Or/Xor/Shl/Shr/Div/Mod/Neg` (line 1121),
`Not` (line 1134), `BitNot` (line 1146), `Cmp` (line 1162), `LAnd/LOr`
(line 1173), `Cast` (line 1177), `CallNative/CallScript/CallIndirect/
CallCrossModule` (line 1178-1181), `FieldAddr` (line 1192), `IndexAddr`
(line 1234), `LoadGlobal` (line 1048).

**Attack path:**

1. After the prologue (`push rbp; mov rbp, rsp`), `[rbp+0]` = saved old-rbp
   and `[rbp+8]` = the **return address** of the caller.
2. A `ConstInt` with `meta.frame_off = 8` and `imm.i = <attacker address>`
   passes validation (ConstInt is not in the checked list).
3. `emit_instr` for ConstInt (thin_emit.cpp:817-824) emits:
   ```
   mov rax, <imm.i>          ; 48 B8 <8 bytes>  — attacker's value
   mov [rbp+8], rax          ; 48 89 85 08 00 00 00 — write to return address
   ```
4. When the function returns (`mov rsp,rbp; pop rbp; ret`), `ret` pops the
   overwritten return address and jumps to the attacker's value.

**Exploitability:**

The v5 format is LIVE (the loader accepts `version == 5` at
em_loader.cpp:161). In **dev mode** (empty keyring — the default), v5
modules are accepted without Ed25519 signature verification
(em_loader.cpp:594-604 — v5 falls into the unsigned branch, same as v3).
The IR validation is the **only** security gate before `alloc_executable_rw`
(em_loader.cpp:783), and it is incomplete.

The attacker controls both the write **address** (`meta.frame_off`, any
`int32_t`) and the write **value** (`imm.i`, any `int64_t`). A positive
`frame_off` writes above `rbp` (into the caller's frame / return address).
A large negative `frame_off` writes to an arbitrary stack address below
`rbp`. This is a classic stack buffer overflow with an arbitrary 8-byte
write primitive.

W^X does not prevent this: the write targets the **stack** (RW), not the
JIT page (RX). The trap model does not prevent this: the store is a normal
x64 instruction, not a bounds-check/budget/depth trap.

**Probe verification:**

`tmp_edit/audit_frame_off_probe.cpp` — proves `validate_thin_function`
accepts `ConstInt` with `frame_off=+8` while correctly rejecting
`StoreFrame` with the same offset (the contrast proves the gap):

```
Case 1: ConstInt with frame_off=+8 (return address slot):
  validate_thin_function returned: TRUE (ACCEPTED)
  >>> CONFIRMED: validator accepts ConstInt with frame_off=+8.
Case 2: StoreFrame with frame_off=+8 (contrast — checked op):
  validate_thin_function returned: FALSE (rejected)
  error: thin_ir_ser: validate: frame_off out of frame bounds
Case 3: ConstInt with frame_off=-999999 (far below frame):
  validate_thin_function returned: TRUE (ACCEPTED)
Case 4: Add with frame_off=+8 (another unchecked op):
  validate_thin_function returned: TRUE (ACCEPTED)
```

`tmp_edit/audit_frame_off_probe2.cpp` — proves the FULL end-to-end path
(serialize → deserialize → validate ACCEPTS → `emit_x64` produces the
return-address overwrite in the emitted bytes):

```
Step 3: validate_thin_function = ACCEPTED (BUG)
Step 4: emit_x64 produced 47 bytes
Step 5: scanning emitted bytes for the attack pattern:
  mov rax, 0x4141414142424242 (48 B8 ...): FOUND at offset 18
  mov [rbp+8], rax (48 89 85 08 00 00 00): FOUND at offset 28
>>> CONFIRMED: emit_x64 produced the return-address overwrite.

  First 40 emitted bytes:
  55 48 89 E5 48 81 EC 00 01 00 00 48 89 9D F8 FF FF FF 48 B8 42 42 42 42 41 41 41 41 48 89 85 08 00 00 00 48 8B 9D F8 FF
```

The emitted sequence is: `push rbp; mov rbp,rsp; sub rsp,256; mov [rbp-8],rbx;
mov rax,0x4141414142424242; mov [rbp+8],rax; mov rbx,[rbp-8]; ...` — the
return address at `[rbp+8]` is overwritten before the epilogue's `ret`.

**Fix:** The validator's `frame_off` check at thin_ir_ser.cpp:576-585
should apply to **every** instruction with a non-zero `meta.frame_off`,
not just the seven listed ops. Alternatively, `emit_instr` should refuse
to emit a store for a `frame_off` outside `[-frame_size, 0)` for any op.
The check should also reject `frame_off == 0` (which would overwrite the
saved rbp) and any positive `frame_off` (which is above rbp — the caller's
frame). The most robust fix: in the validator, for EVERY instruction, if
`meta.frame_off != 0`, require `frame_off < 0 && frame_off >= -frame_size`.

---

## Finding 2 — CONFIRMED VULNERABILITY (HIGH)

### ext_array element bounds check bypass via `size_t` multiplication overflow

**File:** `extensions/array/ext_array.cpp:65-68, 108-112`

**Root cause:**

`n_array_set_i64`, `n_array_get_i64`, `n_array_set_f32`, `n_array_get_f32`,
and `n_array_remove` compute the element byte offset as
`size_t(i) * size_t(elem_size)` and check
`size_t(i)*elem_size + elem_size <= bytes.size()`. When `i` is near
`INT64_MAX`, the unsigned multiplication overflows (wraps modulo 2^64),
producing a small value. The `+ elem_size` then also wraps, potentially to
0, which passes the `<= bytes.size()` check. The subsequent
`&s->bytes[wrapped_value]` is a wild pointer (e.g. `data() - 8`), causing
a heap buffer overflow.

Example (ext_array.cpp:67):
```cpp
static void n_array_set_i64(int64_t h, int64_t i, int64_t v) {
    auto* s = arr_slot(h);
    if (s && i>=0 && size_t(i)*8+8<=s->bytes.size())
        std::memcpy(&s->bytes[size_t(i)*8], &v, 8);
}
```

For `i = 0x7FFFFFFFFFFFFFFF` (INT64_MAX):
- `size_t(i)*8 = 0xFFFFFFFFFFFFFFF8` (overflowed)
- `+8 = 0x0000000000000000` (wrapped to zero)
- `0 <= bytes.size()` → **passes**
- `&s->bytes[0xFFFFFFFFFFFFFFF8]` → wild pointer (`data() - 8`)
- `memcpy` writes 8 bytes below the array → **heap buffer overflow**

**Probe verification:**

`tmp_edit/audit_array_overflow_probe.cpp` replicates the check logic
(without triggering the actual crash, which would segfault):

```
Input: i = 0x7FFFFFFFFFFFFFFF (INT64_MAX), array bytes.size() = 1024
Check 2: size_t(i)*8     = 0xFFFFFFFFFFFFFFF8 (overflowed!)
Check 3: size_t(i)*8 + 8 = 0x0000000000000000 (wrapped to ZERO)
Check 4: sum <= size     = true (bounds check PASSES (BYPASSED!))

--- f32 variant (size_t(i)*4+4) ---
size_t(i)*4+4 = 0x0000000000000000 (wrapped to ZERO)
bounds check  = PASSES (BYPASSED!)
```

**Fix:** Check the index against the element COUNT before multiplication:
```cpp
if (s && i >= 0 && size_t(i) < s->bytes.size() / size_t(s->elem_size))
    std::memcpy(&s->bytes[size_t(i) * size_t(s->elem_size)], &v, size_t(s->elem_size));
```
Or use a checked multiplication. The same fix applies to `n_array_remove`.

**Note:** The script can produce `INT64_MAX` as an index value (sema allows
any `i64` expression). The `i >= 0` check does not prevent this — INT64_MAX
is non-negative.

---

## Finding 3 — POTENTIAL ISSUE (MEDIUM)

### ext_vec/mat/quat `*_new` can `std::terminate` on OOM

**Files:**
- `extensions/vec/ext_vec.cpp:19,38,55` — `v3_new`, `v2_new`, `v4_new`
- `extensions/mat/ext_mat.cpp:17,21` — `m4_new_zero`, `m4_new_identity`
- `extensions/quat/ext_quat.cpp:17` — `q_new`

**Root cause:**

These functions do `g_vec3s.push_back(...)` / `g_mat4s.push_back(...)` /
`g_quats.push_back(...)` without a try/catch. If the vector's growth
allocation fails (`std::bad_alloc`), the exception propagates through the
`extern "C"` native call boundary into JIT'd code. JIT'd code has no C++
unwind metadata (as noted in codegen.cpp's comments about RaiseException
being unreliable from JIT'd code). An uncaught exception reaching the JIT
boundary calls `std::terminate`, killing the process.

Contrast: `ext_array::arr_new` (ext_array.cpp:41-52), `ext_string::str_new`
(ext_string.cpp:22-30), `ext_map::n_map_new` (ext_map.cpp:36-42),
`ext_sync::*_alloc`, and `ext_lifecycle::n_register_routine` all wrap
their allocations in try/catch. The vec/mat/quat extensions are the
exceptions.

**Impact:** A script that allocates many vec/mat/quat values can trigger
OOM, which terminates the process rather than returning a null handle.
This is a DoS vector (process crash), not a code-execution vulnerability.

**Fix:** Wrap each `push_back` in try/catch for `bad_alloc` + `length_error`,
returning 0 (null handle) on failure, matching the pattern in
ext_array/ext_string.

---

## Finding 4 — POTENTIAL ISSUE (MEDIUM)

### `n_string_substr` can `std::terminate` on OOM

**File:** `extensions/string/ext_string.cpp:113-119`

**Root cause:**

```cpp
static int64_t n_string_substr(int64_t a, int64_t start, int64_t len) {
    auto* x = str_slot(a);
    if (!x || start < 0) return str_new("");
    size_t s = size_t(start);
    if (s >= x->size()) return str_new("");
    size_t actual_len = (len < 0) ? (x->size() - s) : std::min(size_t(len), x->size() - s);
    return str_new(x->substr(s, actual_len));  // substr can throw bad_alloc
}
```

`x->substr(s, actual_len)` allocates a new `std::string` BEFORE
`str_new`'s try/catch (the temporary is constructed at the call site).
If the allocation fails (`actual_len` can be up to 1 GiB —
`MAX_STRING_BYTES`), `bad_alloc` propagates through the native boundary
into JIT'd code → `std::terminate`.

Contrast: `n_string_concat` (ext_string.cpp:86-98) correctly wraps its
`reserve` + `+=` in try/catch.

**Fix:** Wrap the `substr` in try/catch, or construct the substring inside
`str_new`'s try block.

---

## Finding 5 — POTENTIAL ISSUE (MEDIUM)

### Extension stores are not thread-safe; concurrent contexts race

**Files:**
- `extensions/array/ext_array.cpp:15` — `static std::vector<ArraySlot> g_arrays;`
- `extensions/string/ext_string.cpp:22` — `static std::vector<std::string> g_strings;`
- `extensions/map/ext_map.cpp:21` — `static std::vector<MapSlot> g_maps;`
- `extensions/vec/ext_vec.cpp:17,36,53` — `g_vec3s`, `g_vec2s`, `g_vec4s`
- `extensions/mat/ext_mat.cpp:15` — `g_mat4s`
- `extensions/quat/ext_quat.cpp:15` — `g_quats`

**Root cause:**

The thread-safety model (context.hpp:34-44) states: "a context_t is NOT
shared across threads. Each concurrent caller thread allocates its own
context_t; they share the dispatch table, JIT'd code, and module registry
(all read-only after compile)."

However, the extension host stores (g_arrays, g_strings, g_maps, g_vec3s,
etc.) are **shared mutable state** that grows at runtime (via `*_new`,
`*_push`, `*_resize`). They are NOT protected by mutexes (except
`ext_sync` and `ext_lifecycle`, which use `g_store_mutex` / `g_mutex`).

If a host runs multiple `context_t`s concurrently and both call
`array_new` (or `string_new`, `vec3_new`, etc.), the concurrent
`std::vector::push_back` calls are a data race (undefined behavior in
C++). This can corrupt the vector's internal state (size/capacity/
pointer), leading to use-after-free, double-free, or wild pointer
dereferences.

`ext_sync` and `ext_lifecycle` correctly use mutexes for their stores.
The other six extensions do not.

**Impact:** A host using the documented multi-context concurrency model
(thread_safety_test.cpp proves two threads can run the same compiled entry
concurrently) will race on extension store mutations if both threads call
extension natives. This is a data race that can corrupt heap metadata and
potentially lead to code execution.

**Fix:** Either (a) document that extension stores are not thread-safe
and must only be used from one thread at a time (breaking the
multi-context model for extension-using scripts), or (b) add a mutex to
each extension's store (matching ext_sync/ext_lifecycle), or (c) use
thread-local stores.

---

## Finding 6 — POTENTIAL ISSUE (LOW)

### v5 IR validator does not reject duplicate block IDs

**File:** `src/thin_ir_ser.cpp:559-566` (validate_thin_function block id check)

**Root cause:**

The validator checks `blk.id < num_blocks` (bounds) and `blocks[0].id == 0`
(entry), but does NOT check for duplicate block IDs. `emit_x64`
(thin_emit.cpp:681-700) sizes `block_labels` by `thf.blocks.size()` and
indexes it by `blk.id`:

```cpp
block_labels.resize(thf.blocks.size());       // sized by position count
for (size_t i = 0; i < thf.blocks.size(); ++i)
    block_labels[i] = e.alloc_label();         // allocated by position
...
e.bind(block_labels[blk.id]);                  // indexed by blk.id
...
e.jmp(block_labels[term.target]);              // indexed by term.target
```

If two blocks share the same `id`, `block_labels[id]` is bound twice
(the second binding wins). A `jmp` to that ID goes to the last block with
that ID, not the first. The first block's instructions are emitted but
unreachable (every block has a terminator, so no fall-through).

**Impact:** This is a **correctness** issue (wrong control flow), not a
memory safety issue. The generated code jumps to the wrong block but
cannot escape the sandbox (all jumps target `block_labels[0..N-1]`, which
are within the function). Infinite loops are caught by the budget check;
stack overflow by the depth check. No host crash or memory corruption.

**Fix:** Add a `std::unordered_set<uint32_t> seen_ids` check in the
validator: reject if `blk.id` is seen twice.

---

## Areas Verified Safe

### .em loader (src/em_loader.cpp)

- **Header validation:** All counts (function_count, global_size,
  rodata_total, entry_slot, name_count, reloc_count, binding_count) are
  checked against MAX_* limits before any allocation (em_loader.cpp:163-195,
  299-310, 388-395, 416-422). Checked add/mul helpers prevent overflow
  (em_loader.cpp:36-45). **VERIFIED SAFE.**

- **Cursor ops:** The `Reader::take` helper (em_loader.cpp:48-54) uses
  `checked_add(pos, n, end)` and `end > bytes.size()` — no integer overflow,
  no OOB read. **VERIFIED SAFE.**

- **Relocation bounds:** `r.offset + 8 <= f.code.size()` checked via
  64-bit subtraction (em_loader.cpp:336-340). Relocation kind range-checked
  (em_loader.cpp:341-344). Addend bounds checked for FunctionRodataBase
  (em_loader.cpp:346). **VERIFIED SAFE.**

- **Native binding resolution:** `natives` null-deref at em_loader.cpp:815
  is unreachable — v2+ modules reject `!natives` at line 423; v5 IR
  functions reject CallNative with null natives at line 713-719; v5
  raw-x86 fallback goes through the v2+ path. When `native_bindings` is
  non-empty, `natives` is guaranteed non-null. **VERIFIED SAFE** (though
  an explicit null check would be more defensive).

- **v4 signature verification:** Ed25519 verification runs BEFORE
  `alloc_executable_rw` (em_loader.cpp:582-603). The `sig_payload_len` is
  cross-checked against the parser position (em_loader.cpp:222-228).
  Untrusted keys are rejected (em_loader.cpp:587-595). **VERIFIED SAFE.**

- **W^X in the exec-page loop:** `alloc_executable_rw` (RW) → patch
  relocations → `seal_executable` (RX). The page is never RWX.
  (em_loader.cpp:785-821). **VERIFIED SAFE.**

- **v5 IR re-emit path:** deserialize → native rebind → validate →
  `emit_x64` → all BEFORE `alloc_executable_rw`. A malformed blob is
  rejected with no exec page (em_loader.cpp:680-763). **VERIFIED SAFE**
  (except for the frame_off gap in Finding 1).

### .em writer (src/em_writer.cpp)

- `preflight_em_module` checks all counts/sizes fit u32/u16 before writing
  (em_writer.cpp:71-103). Relocation offsets bounds-checked
  (em_writer.cpp:128). The writer cannot produce a file the loader
  accepts incorrectly — the writer zeros reloc/native-binding bytes in the
  code (em_writer.cpp:131-134) so the loader's patches are the only writes
  to those positions. **VERIFIED SAFE.**

### JIT sandbox (src/jit_memory.cpp, src/codegen.cpp)

- **W^X enforced:** `alloc_executable_rw` → `VirtualAlloc(PAGE_READWRITE)`
  → copy → caller patches → `seal_executable` →
  `VirtualProtect(PAGE_EXECUTE_READ)`. Never `PAGE_EXECUTE_READWRITE`.
  (jit_memory.cpp:16-36). **VERIFIED SAFE.**

- **JIT'd code cannot escape the sandbox:** All calls go through the
  dispatch table (`call [r11+slot*8]`), native table (symbolic name
  rebind), or registry (cross-module). No raw address computation from
  script data. The indirect-call guard (emit_call_target_guard,
  codegen.cpp:504-533) validates fn handles against an allowlist before
  dispatch. **VERIFIED SAFE.**

### Trap model (src/context.hpp, src/codegen.cpp)

- **Trap bypass:** All traps (bounds check, budget, depth, CPUID gate,
  bad call target) emit either a trap-stub call (longjmp to checkpoint)
  or `ud2` (process death). The bounds check uses unsigned compare
  (codegen.cpp:280-314) — a negative index reinterprets as huge unsigned
  and fails the `jb` (in-bounds) check. **VERIFIED SAFE.**

- **Trap-stub return:** If a faulty host trap stub returns (instead of
  longjmping), the codegen emits a balanced `add rsp, call_frame` after
  the `call_reg(rax)` (codegen.cpp:365-366), so execution continues
  harmlessly rather than crashing. **VERIFIED SAFE.**

- **No trap stub + no VEH → process death:** If `ctx.trap_stub` is null,
  traps fall back to `ud2` (codegen.cpp:368-370), which raises
  `EXCEPTION_ILLEGAL_INSTRUCTION`. Without a VEH, this kills the process.
  This is by design (documented in context.hpp and codegen.cpp comments)
  — the host must install a trap stub or VEH for recoverable traps.
  **VERIFIED SAFE** (documented behavior, not a vulnerability).

### Type safety (src/sema.cpp)

- **Cast matrix:** Explicit `as` casts (sema.cpp:1276-1295) allow only:
  same-type, int-to-int, float-to-float, signed-int-to-float (and
  reverse). Slices, aggregates, opaque handles, bool, and function
  handles can ONLY take a same-type no-op cast. No pointer
  reinterpretation. **VERIFIED SAFE.**

- **Pointer forging:** `is_plain_integer` (sema.cpp:308-310) excludes
  `is_fn_handle` and `struct_name` (opaque handles), so handles cannot be
  cast to/from `i64`. Implicit conversion (sema.cpp:326-329) is
  widening-only for same-signedness integers. A script cannot forge a
  pointer or handle through casts. **VERIFIED SAFE.**

- **Function handle provenance:** `is_fn_handle` types are only produced
  by `&fn` (sema.cpp:794-807). The runtime call-target guard
  (emit_call_target_guard, codegen.cpp:504-533) validates every indirect
  call's handle against a bit allowlist before dispatch. A forged i64
  used as a call target traps (BadCallTarget). **VERIFIED SAFE.**

### Pass system (src/ember_pass.hpp, extensions/opt/, extensions/obf/)

- **Peephole (src/peephole.cpp):** Strictly local in-place rewrite with
  NOP padding — no label offset shifts, no rel32 re-resolution needed.
  SmartImm skips guarded relocatable imm64s. SetccMovzx is a no-op
  (Stage 1). **VERIFIED SAFE.**

- **SubstitutionPass (extensions/obf/ext_obf.cpp):** Allocates new VRegs
  (from `compute_max_vreg`) and frame slots (from `next_local_off`).
  Updates `frame.next_local_off` and `frame.frame_size` correctly
  (`(next_off + 8 + 15) & ~15`). New `frame_off` values are within the
  updated frame. The pass runs at JIT compile time (not at v5 load time),
  so its input is sema-clean (not attacker-controlled). **VERIFIED SAFE.**

- **ConstPropPass / DCE / CSE / LICM (extensions/opt/ext_opt.cpp):** All
  are value-preserving and conservative. DCE never removes side-effecting
  instrs. CSE remaps VReg uses carefully (handles redefinition). LICM
  hoists only pure invariant instrs to a safe pre-header. **VERIFIED SAFE.**

- **Pass manager (src/ember_pass.cpp):** Runs passes in order with
  instrumentation callbacks. `run_to_fixpoint` bounded by `max_rounds=8`.
  No memory safety issues. **VERIFIED SAFE.**

### Thread safety — context model (src/engine.cpp, src/context.hpp)

- **r14 preservation:** The B1 thunks (engine.cpp:131-155) save/restore
  the caller's r14 and set r14 = context. JIT'd code reads r14 as a base
  register (`[r14+off]`) but never writes to r14 itself. Native calls
  preserve r14 (Win64 callee-saved). Script-to-script calls forward r14
  (callee-saved, never modified). **VERIFIED SAFE.**

- **Stack alignment:** The thunk achieves rsp%16=0 at the `callq *%r11`
  (entry rsp%16=8 after the call from `ember_call_void`; `pushq %r14` → 0;
  `subq $32` → 0; 32 is 16-aligned). The JIT'd code enters with rsp%16=8
  (standard Win64 entry), then `push rbp` → rsp%16=0. **VERIFIED SAFE.**

- **Per-context isolation:** Each thread's `context_t` has its own
  `budget_remaining`, `call_depth`, `max_call_depth`, `checkpoint`, and
  `has_checkpoint`. The JIT'd code reads these via `[r14+off]` (the
  per-call context register), so one compiled body serves N per-thread
  contexts with no shared mutable state in the JIT path.
  **VERIFIED SAFE** (for the context model; extension stores are a
  separate issue — Finding 5).

- **reset_for_call (context.hpp:74-80):** After longjmp recovery,
  `call_depth` is reset to 0 (balanced leave instructions on the
  abandoned stack cannot execute). `last_trap` and `last_error` are
  cleared. `budget_remaining` is NOT reset (host manages batch budgets).
  **VERIFIED SAFE.**

---

## Probes Run

| Probe | Purpose | Result |
|-------|---------|--------|
| `tmp_edit/audit_frame_off_probe.cpp` | Show validator accepts ConstInt with frame_off=+8 (rejected for StoreFrame) | CONFIRMED — validator accepts out-of-bounds frame_off for unchecked ops |
| `tmp_edit/audit_frame_off_probe2.cpp` | End-to-end: serialize→deserialize→validate→emit_x64, inspect emitted bytes for return-address overwrite | CONFIRMED — emitted bytes contain `mov [rbp+8], rax` with attacker value |
| `tmp_edit/audit_array_overflow_probe.cpp` | Show size_t(i)*8+8 wraps to 0 for i=INT64_MAX, bypassing the bounds check | CONFIRMED — bounds check passes via unsigned overflow |

All probes compiled with:
```
g++ -std=c++17 -O3 -DNDEBUG -DED25519_NO_SEED -Isrc -Ithirdparty/ed25519
    -o probe.exe probe.cpp -Lbuildt -lember_frontend -lember -lember_ed25519
    -lkernel32 -luser32
```

No source files were edited. All probes are in `tmp_edit/`.

---

## Prioritized Fix List

1. **CRITICAL — Finding 1:** Extend the `frame_off` bounds check in
   `validate_thin_function` (thin_ir_ser.cpp:576-585) to ALL instructions
   with a non-zero `meta.frame_off`, not just the seven listed ops. Reject
   `frame_off >= 0` (above rbp) and `frame_off < -frame_size` (below frame)
   for every producing op.

2. **HIGH — Finding 2:** Fix the element bounds checks in ext_array
   (ext_array.cpp:65-68, 108-112) to check the index against the element
   count before multiplication, preventing `size_t` overflow.

3. **MEDIUM — Finding 3:** Add try/catch for `bad_alloc`/`length_error`
   around `push_back` in ext_vec/mat/quat `*_new` functions.

4. **MEDIUM — Finding 4:** Wrap the `substr` call in `n_string_substr`
   (ext_string.cpp:118) in try/catch.

5. **MEDIUM — Finding 5:** Add mutex protection to ext_array/string/map/
   vec/mat/quat stores, or document that they're single-threaded only.

6. **LOW — Finding 6:** Reject duplicate block IDs in
   `validate_thin_function`.
