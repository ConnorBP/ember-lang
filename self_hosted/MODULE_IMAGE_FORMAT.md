# Self-hosted module image format: EMBM v2

The self-hosted code generator emits one contiguous `array<u8>` module image. `load_executable_module` validates the complete image, allocates code/rodata/data/dispatch storage, applies relocations, seals permissions, and returns an opaque owning handle.

- **Current emitter:** EMBM v2
- **Compatibility:** the loader accepts v1 and v2
- **Byte order:** little-endian
- **v2 header size:** 96 bytes
- **Dispatch relocation:** `RELOC_DISPATCH = 4`

## 1. Layout

```text
v1: | 80-byte header | code | rodata | data | symbols | relocations |
v2: | 96-byte header | code | rodata | data | symbols | fn-table | relocations |
```

Offsets are absolute byte offsets from the beginning of the image. Sections need not have internal alignment; the loader copies them into separately allocated regions.

Relocation placement is implicit: records begin immediately after the final declared section (symbols for v1, function table for v2). The image must end exactly after `reloc_count * 32` relocation bytes; trailing bytes are rejected.

## 2. Headers

### v1 header (80 bytes)

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | magic | bytes `45 4D 42 4D` (`EMBM`) |
| 4 | 4 | version | `1` as u32 |
| 8 | 8 | `code_off` | code section offset |
| 16 | 8 | `code_len` | code bytes |
| 24 | 8 | `rodata_off` | read-only data offset; may be 0 when empty |
| 32 | 8 | `rodata_len` | read-only data bytes |
| 40 | 8 | `data_off` | mutable data offset; may be 0 when empty |
| 48 | 8 | `data_len` | mutable data bytes |
| 56 | 8 | `syms_off` | symbol table offset; may be 0 when empty |
| 64 | 8 | `syms_len` | symbol table bytes |
| 72 | 8 | `reloc_count` | number of 32-byte relocation records |

### v2 header (96 bytes)

Bytes 0–79 retain the v1 layout, with `version = 2`. v2 appends:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 80 | 8 | `fntable_off` | function-table offset; may be 0 when empty |
| 88 | 8 | `fntable_len` | function-table bytes; multiple of 16 |

The self-hosted `codegen_emit_image()` always writes version 2 and computes section positions as:

```text
code_off    = 96
rodata_off  = code_off + code_len
data_off    = rodata_off + rodata_len
syms_off    = data_off + data_len
fntable_off = syms_off + syms_len
reloc_off   = fntable_off + fntable_len
```

For an empty section the emitter stores offset 0, while still computing the following nonempty section's physical position from the concatenated lengths.

## 3. Validation limits

The loader rejects an image exceeding any limit:

| Item | Limit |
|---|---:|
| code | 16 MiB |
| rodata | 16 MiB |
| data | 16 MiB |
| function table | 16 MiB |
| symbol table | 1 MiB |
| relocation records | 65,536 |
| total image | 64 MiB |
| dispatch slots | `16 MiB / sizeof(void*)` |

Every nonempty section must start at/after the current section cursor, must not overflow `off + len`, and must end within the image. Sections therefore cannot overlap or appear out of order. Unknown magic/version, a short header, a function-table length not divisible by 16, ambiguous relocation placement, or trailing bytes cause rejection before publication.

## 4. Code section

Code contains x86-64 machine bytes. The loader allocates it RW, copies and relocates it, then seals it RX.

Direct script-to-script `call rel32` fixups are resolved by the self-hosted code generator because all generated functions share the code section. Indirect function/lambda/coroutine calls use the v2 dispatch vector.

`module_entry_ptr(handle, code_offset)` returns `code_base + code_offset` only when the handle is live and `0 <= code_offset < code_len`.

## 5. Rodata section

Rodata stores string bytes and other immutable data. ABS64 rodata relocations materialize pointers into this section. After relocation, the loader seals the region read-only using the platform's RX protection primitive (the bytes are not executed, but the primitive provides non-writable protection in the current platform abstraction).

An addend equal to `rodata_len` is valid, allowing a zero-length object at the end. An addend greater than the length is rejected.

## 6. Data section

Data stores initialized mutable globals and remains RW. Each load receives a fresh copy. `RELOC_DATA` patches pointers to `data_base + addend`; an addend equal to `data_len` is valid for an end/zero-length address, while a greater value is rejected.

String-handle globals that require runtime construction are initialized by the synthetic `__globals_init` function. The pipeline invokes it before `main`.

## 7. Symbol table

The symbol table is concatenated, null-terminated UTF-8 names:

```text
string_length\0string_from_i64\0print\0
```

Relocation `sym_idx` is the zero-based string index, not a byte offset. Every symbol must end with a null byte inside `syms_len`. Native resolution is allowlisted against the active registered `NativeSig` table; there is no script-visible arbitrary symbol resolver.

## 8. Function table and dispatch vector (v2)

Each function-table record is 16 bytes:

| Record offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | `slot` | logical dispatch slot |
| 8 | 8 | `code_offset` | byte offset within code |

The loader validates every record before allocation:

- `slot < MAX_DISPATCH_SLOTS`
- `code_offset < code_len`
- no duplicate slots

It allocates `max_slot + 1` pointers, zeroes sparse entries, then writes `code_base + code_offset` at each declared slot. Slot 0 is ordinary. Empty tables produce dispatch base/count 0.

The vector remains stable until `free_executable_module`. Function handles remain logical slot values rather than raw code addresses, and generated indirect calls use:

```text
call [dispatch_base + slot * 8]
```

## 9. Relocation records

Each relocation is 32 bytes:

| Record offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | `type` | relocation kind |
| 8 | 8 | `patch_off` | offset of an 8-byte immediate within code |
| 16 | 8 | `sym_idx` | native symbol index; otherwise 0 |
| 24 | 8 | `addend` | section addend, ABI fingerprint, or 0 |

Kinds:

| Value | Name | Patched value |
|---:|---|---|
| 1 | `RELOC_RODATA` | `rodata_base + addend` |
| 2 | `RELOC_DATA` | `data_base + addend` |
| 3 | `RELOC_NATIVE` | validated native function pointer |
| 4 | `RELOC_DISPATCH` | stable v2 dispatch-vector base |

`RELOC_DISPATCH` is valid only in v2 and requires `sym_idx == 0` and `addend == 0`. It writes zero if the table is empty.

For every record:

- `patch_off + 8` must not overflow and must be `<= code_len`
- 8-byte patch ranges must not overlap
- section references require a present corresponding section
- base-plus-addend arithmetic must not wrap
- unknown relocation types reject the entire image

All records are parsed, sorted, and fully resolved before any code byte is patched. Validation failure tears down every allocated region and returns handle 0.

A native call is commonly emitted as:

```text
48 B8 00 00 00 00 00 00 00 00   mov rax, <ABS64_NATIVE>
FF D0                           call rax
```

The relocation points at the eight immediate bytes following `48 B8`.

## 10. Native ABI fingerprint

`RELOC_NATIVE.addend` is a u64 fingerprint:

| Bits | Field |
|---|---|
| 0–3 | parameter count, saturated to 15 |
| 4–27 | first four parameter type codes, 6 bits each |
| 28–33 | return type code |
| 34–37 | low four permission bits |
| 38–63 | reserved, must be zero |

Type codes:

| Code | Type |
|---:|---|
| 0 | `void` |
| 1 | `i64` |
| 2 | `f64` |
| 3 | `f32` |
| 4 | `bool` |
| 5 | string/slice pair |
| 6 | pointer/opaque handle/function handle/lambda |
| 7 | `i32` |
| 8 | `u8` |
| 9 | `u16` |
| 10 | `u32` |
| 11 | `u64` |
| 12 | `i8` |
| 13 | `i16` |
| 14 | aggregate/fixed array/fallback |
| 15–63 | reserved |

The loader computes the same fingerprint from the registered `NativeSig`. It rejects a mismatch, a missing native, null function pointer, nonzero reserved bits, or missing required permission. Parameters beyond the fourth are represented by count but not individual type codes; the self-hosted native-call ABI currently uses the shared encoding above.

The C++ authority is `src/module_abi_fingerprint.hpp`; `codegen.ember` mirrors its constants.

## 11. Loader API

The `call_raw` extension registers these `PERM_FFI` module operations:

```text
load_executable_module(image: i64) -> i64
module_entry_ptr(handle: i64, code_offset: i64) -> i64
module_dispatch_base(handle: i64) -> i64
module_dispatch_count(handle: i64) -> i64
free_executable_module(handle: i64) -> void
```

Lifecycle:

1. validate header, sections, records, limits, and exact image consumption
2. allocate/copy code, rodata, and data
3. validate/build the v2 dispatch vector
4. parse symbols and resolve every relocation
5. patch code
6. seal code RX and rodata non-writable; leave data RW
7. publish an opaque owning handle
8. query entries/dispatch while live
9. free all regions and invalidate the handle

Double-loading an image creates independent copies. Invalid/freed handles return zero from queries. Freeing an unknown/already-freed handle is rejected/no-op by the native lifecycle implementation.

The older `make_executable`/`free_executable_ptr` pair remains for compatibility but cannot represent rodata, data, native relocation, or dispatch metadata.

## 12. Try/catch and coroutine integration

The extension exposes permission-free context layout queries used by self-hosted try/catch emission:

```text
context_off_catch_depth() -> i64
context_off_thrown_value() -> i64
context_off_catch_bufs() -> i64
context_off_catch_saved_depths() -> i64
context_catch_buf_stride() -> i64
```

The values come directly from `context_offsets`, avoiding hard-coded C++ object layout in Ember source.

Before running a module that may use coroutines, the pipeline calls:

```text
set_coroutine_dispatch(module_dispatch_base(h), module_dispatch_count(h))
```

This allows `coroutine_start` to resolve logical function slots in the loaded self-hosted module.

## 13. Emitter contract

`codegen.ember` maintains:

- `cg_code`
- `cg_rodata`
- `cg_data`
- `cg_syms`
- `cg_relocs` (32-byte records)
- `cg_fn_labels` (source for 16-byte function-table records)

`codegen_emit_image()` writes a 96-byte v2 header, concatenates the sections in order, emits one `(slot, code_offset)` record per generated function, then appends relocation bytes.

The two-generation bootstrap loads that image, installs its dispatch vector, calls the generated globals initializer when present, resolves compiler B's `main` entry, and invokes it with `call_raw`.
