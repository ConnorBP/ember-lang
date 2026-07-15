# Self-Hosted Module Image Format (EMBM v1)

The self-hosted codegen emits a single `array<u8>` *module image*. A host
loader native (`load_executable_module`) parses it, allocates stable code /
rodata / data memory regions, applies relocations, protects permissions, and
returns an opaque owning handle. This replaces the bare code-blob +
`make_executable` model so the self-hosted compiler can emit real string
literals (rodata), mutable globals (data section), and native/extension calls
(relocated call sites).

Design authority: advisor Phase 1 guidance (in-process module loader, ABS64
relocations, allowlisted native symbol resolution with signature validation,
no `resolve_native`, no runtime-built pseudo-rodata).

---

## 1. Top-level layout

The image is one contiguous byte buffer:

```
| header (80 bytes) | code | rodata | data | symbols | relocations |
```

All offsets are **byte offsets from the start of the image**. All integers are
**little-endian**. Every section follows the previous with no padding
requirement (the loader copies each section to its own aligned region).

---

## 2. Header (80 bytes, fixed)

| offset | size | field         | meaning                                                |
|--------|------|---------------|--------------------------------------------------------|
| 0      | 4    | magic         | `0x45 0x4D 0x42 0x4D` ("EMBM")                         |
| 4      | 4    | version       | `1` (u32)                                              |
| 8      | 8    | code_off      | u64 — byte offset of code section in image             |
| 16     | 8    | code_len      | u64 — length of code section in bytes                  |
| 24     | 8    | rodata_off    | u64 — byte offset of rodata section (0 if none)        |
| 32     | 8    | rodata_len    | u64 — length of rodata section                         |
| 40     | 8    | data_off      | u64 — byte offset of initialized mutable data (0 if none) |
| 48     | 8    | data_len      | u64 — length of mutable data section                   |
| 56     | 8    | syms_off      | u64 — byte offset of symbol-name table (0 if none)     |
| 64     | 8    | syms_len      | u64 — length of symbol-name table                      |
| 72     | 8    | reloc_count   | u64 — number of relocation records (follow symbols)    |

**Validation caps (reject image if exceeded):**
- `code_len` <= 16 MiB
- `rodata_len` <= 16 MiB
- `data_len` <= 16 MiB
- `syms_len` <= 1 MiB
- `reloc_count` <= 65536
- total image size <= 64 MiB
- every `(off + len)` must be `<= total image size` and `off` values must be
  non-overlapping and in section order (header < code < rodata < data < symbols
  < relocations). A zero-length section may have `off = 0`.
- `version` must be exactly `1`; unknown magic or version → reject.

---

## 3. Code section

Raw x64-64 machine bytes. The loader copies these to an **RX** page. Internal
script-to-script `call rel32` fixups are already resolved by the self-hosted
codegen's `cg_resolve_fixups` (relative within the code section), so the loader
does **not** touch them. Only ABS64 relocations (§6) are patched by the loader.

Entry point: `module_entry_ptr(handle, offset)` returns
`code_base + offset` for invoking a specific function (e.g. `main`). The
offset is validated against `code_len`.

---

## 4. Rodata section

Read-only initialized bytes: string-literal contents, f-string template
fragments, and any other compile-time constant data the code references by
pointer. Copied to an **R** (read-only) region. A string literal `{ptr, len}`
is encoded as a rodata addend (ptr = `rodata_base + addend`) + a length baked
as an immediate; the codegen emits a `mov reg, <ABS64_RODATA imm64>` +
`mov reg2, len` to materialize the slice pair.

---

## 5. Data section (mutable globals)

Initialized writable bytes: the initial values of `global` variables with
constant initializers. Copied to a **RW** region. A global variable is
referenced by a `mov reg, <ABS64_DATA imm64>` (ptr = `data_base + addend`),
then loaded/stored through that pointer. Non-constant global initialization
is deferred (Phase 1 supports only constant-initialized globals).

---

## 6. Symbol-name table

A sequence of **null-terminated** UTF-8 name strings, concatenated. Symbols
are referenced by **index** (0-based, in order of appearance) from native
relocation records. The loader resolves each symbol name against the current
ember `context_t`'s registered `NativeSig` table (allowlist). Unknown name →
reject. Example: `string_length\0string_concat\0print\0` defines symbols 0,1,2.

---

## 7. Relocation records

`reloc_count` records immediately follow the symbol table. Each record is
**32 bytes**:

| offset | size | field      | meaning                                                       |
|--------|------|------------|---------------------------------------------------------------|
| 0      | 8    | type       | u64: `1`=ABS64_RODATA, `2`=ABS64_DATA, `3`=ABS64_NATIVE        |
| 8      | 8    | patch_off  | u64 — byte offset **within the code section** of the imm64 to patch |
| 16     | 8    | sym_idx    | u64 — for NATIVE: index into symbol table; for RODATA/DATA: unused (0) |
| 24     | 8    | addend     | u64 — for RODATA/DATA: byte addend into that section; for NATIVE: ABI fingerprint (§8) |

**Semantics:**
- `ABS64_RODATA`: write `rodata_base + addend` (u64 LE) at code offset `patch_off`.
- `ABS64_DATA`: write `data_base + addend` (u64 LE) at code offset `patch_off`.
- `ABS64_NATIVE`: look up symbol `sym_idx`; write the native's function pointer
  (u64 LE) at code offset `patch_off`, AFTER validating the ABI fingerprint.

**Validation:**
- `patch_off + 8 <= code_len` (the patched 8 bytes must lie inside code).
- No two relocations may overlap (sort by patch_off; reject if
  `patch_off[i] + 8 > patch_off[i+1]`).
- `addend < rodata_len` (RODATA) / `addend < data_len` (DATA); reject overflow
  of `base + addend` (must not wrap / must be within the allocated region).
- NATIVE: `sym_idx` must index a valid symbol; the named native must exist in
  `ctx.natives`; the ABI fingerprint must match; the required permission must
  be granted. Any failure → reject the whole image (no partial patching).

Native calls are emitted by the codegen as:
```
mov rax, 0x0000000000000000   ; 48 B8 .. .. .. .. .. .. .. ..  (ABS64_NATIVE reloc at the imm64)
call rax                       ; FF D0
```
This uses an indirect call through a relocated absolute address, avoiding the
±2 GiB reach limit of a direct `call rel32` to a host native.

---

## 8. Native ABI fingerprint (u64)

Packed into the `addend` field of an ABS64_NATIVE record so the loader can
verify the call site matches the registered native's actual signature. Layout
(little-endian u64, bit fields from LSB):

| bits   | field        | encoding                                                    |
|--------|--------------|-------------------------------------------------------------|
| 0..3   | param_count  | 0..15                                                       |
| 4..27  | param types  | 6 bits each, up to 4 params (param i occupies bits 4+6i..9+6i) |
| 28..33 | return type  | 6 bits                                                      |
| 34..37 | permission   | 0=none, 1=FFI (PERM_FFI)                                    |
| 38..63 | reserved     | 0 (must be zero; reject if nonzero)                         |

Type encoding (6 bits): `0=void, 1=i64, 2=f64, 3=f32, 4=bool, 5=string(slice),
6=ptr/opaque, 7=i32, ...` (the C++ loader and .ember codegen must share one
table — define it in `src/em_type_codec.hpp` or a new shared header and mirror
the exact values in the .ember codegen). A `param_count > 4` is allowed for
the fingerprint (stack-passed params) but Phase 1 codegen only emits native
calls with `<= 4` register params; the fingerprint still encodes the full
declared arity for validation.

The loader computes the fingerprint from the located `NativeSig` (param types
+ return + permission) using the **same** encoding and compares. Mismatch →
reject. This catches a codegen that emits a call with the wrong arity/types.

---

## 9. Loader lifecycle (host natives, PERM_FFI-gated)

Added to `extensions/call_raw/ext_call_raw.cpp`:

```
load_executable_module(image: array<u8>) -> i64   // opaque handle, 0 on failure
module_entry_ptr(handle: i64, code_offset: i64) -> i64  // code_base+offset, 0 on failure
free_executable_module(handle: i64) -> void
```

`load_executable_module`:
1. Validate the entire image header + section bounds + caps BEFORE allocating.
2. Allocate stable code (RX), rodata (R), data (RW) regions via existing
   JIT/platform helpers (reuse, do not create a second allocator).
3. Copy sections while writable; copy `data` section as the initial mutable
   state (a per-load copy, so each module load gets fresh globals).
4. Apply relocations (§7) with full validation; on ANY failure, free all
   allocated regions and return 0 (no partial state).
5. Protect code RX, rodata R, data RW.
6. Return an opaque owning handle (store code/rodata/data bases + lengths).
7. Reject: unknown magic/version, oversize sections, OOB/overlap relocations,
   unknown native, ABI mismatch, missing permission, double-load of same
   image is allowed (fresh copy each time).

`module_entry_ptr`: validate `handle` known and `0 <= code_offset < code_len`;
return `code_base + code_offset` (0/invalid otherwise). Entry pointers are
invalidated when the handle is freed.

`free_executable_module`: validate handle known; free all regions; remove
handle; reject unknown handle (double-free).

`call_raw` (existing) is still used to *invoke* an entry pointer returned by
`module_entry_ptr`. The old `make_executable`/`free_executable_ptr` remain for
back-compat but the self-hosted pipeline moves to the module API.

---

## 10. Self-hosted codegen changes (Phase 1b)

`codegen.ember` gains parallel buffers:
- `cg_code` (existing byte array — the code section)
- `cg_rodata` (new array<u8>) — string-literal bytes; `cg_rodata_alloc(bytes)`
  appends + returns the addend (offset within rodata).
- `cg_data` (new array<u8>) — global initial values; `cg_data_alloc(bytes)`
  appends + returns the addend.
- `cg_syms` (new) — accumulated null-terminated symbol names; `cg_sym_index(name)`
  returns existing index or appends + returns new index.
- `cg_relocs` (new array<u8>, 32 bytes each) — relocation records; `cg_reloc(type, patch_off, sym_idx, addend)`.

`codegen_emit_image() -> array<u8>`: builds the 80-byte header + concatenates
code + rodata + data + symbols + relocations into one `array<u8>` and returns
it (replacing the old "return code bytes" contract).

`full_pipeline.ember` `compile_and_run`:
- `let img = codegen_emit_image();`
- `let h = load_executable_module(img); if (h == 0) { return -501; }`
- `let entry = module_entry_ptr(h, main_off); if (entry == 0) { free_executable_module(h); return -502; }`
- `let result = call_raw(entry, 0);`
- `free_executable_module(h); return result;`

Real string literals: `cg_eval(StringLit)` emits `mov rax, <ABS64_RODATA>` +
`mov rdx, len` (the {ptr,len} slice in rax:rdx), replacing the sham
length-as-representation. `string_length`, `string_concat`, `string_substr`
become real NATIVE relocations (no special-case in `cg_eval_callexpr`).

Mutable globals: `global g: i64 = <const>` → data section slot + ABS64_DATA
reloc; reads/writes go through the relocated pointer.

`>4` args: `cg_eval_callexpr` emits stack args (slots `[rsp+32+shadow]`,
aligned to 16, 32-byte shadow space) for args 5+, keeping Win64 ABI.

---

## 11. Deferred (later in Phase 1 or beyond)

- Cross-module linking / `link` decls / cross-module handles.
- Non-constant global initialization (runtime init expressions).
- Aggregate (struct) by-value args/returns ABI.
- Float args/returns in XMM regs (Win64 float ABI).
- Import/module/namespace resolution in the self-hosted frontend (pre-inlining
  acceptable for the first bootstrap harness; final parity requires the
  frontend to reproduce native import behavior or consume a documented
  canonical preprocessed format in both bootstrap generations).

The header is versioned so these can be added as v2 records / capabilities
without changing v1 records.

---

## 12. TDD coverage required

- real string contents (read bytes back, not just length)
- two distinct same-length strings (distinct rodata addends)
- string concatenation + content equality through native calls
- mutable global read + write
- more than four call arguments (5, 6, 7) with correct values + alignment
- code→rodata and code→data relocation correctness
- unknown native symbol → image rejected
- native ABI fingerprint mismatch → image rejected
- relocation patch_off out of bounds / overflow / overlap → rejected
- truncated image (shorter than header) and oversized image → rejected
- invalid section relationships (overlapping offsets) → rejected
- W^X: code region not writable after load; rodata not writable
- handle lifetime: free then entry_ptr returns invalid; double-free rejected;
  entry_ptr out-of-code-bounds rejected
