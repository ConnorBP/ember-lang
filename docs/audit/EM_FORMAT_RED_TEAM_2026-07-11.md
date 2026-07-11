# Red Team Audit — ember `.em` Module Format Security

**Date:** 2026-07-11
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** b07841a
**Auditor scope:** READ-ONLY. No source files edited. One probe added under `tmp_edit/` (gitignored).
**Build/test:** `cd buildt && ctest -E bench --timeout 30` → 49/50 pass. The one failure (`ember_test_cli`) is a **pre-existing, unrelated** lambda-WIP regression (`valid_lambda_as_arg.ember`, `valid_lambda_nested.ember` — 247/249 lang cases pass; the recent commit log shows active lambda work: `27486da Cleanup: remove lambda debug prints`, `b456dc3 Fix: cross-module handle type compatibility`). It is not caused by this audit and not related to the `.em` format. `git status` is clean (no tracked source modified).

---

## 0. Why this audit exists — the two questions

The user raised two concerns that the prior `SECURITY_AUDIT_2026-07-11.md` did **not** answer:

1. **Compile-side vs load-side.** ember's safety story (bounds checks, `PERM_FFI` gating, call-target guard, budget/depth limits, trap unwind) is described in `docs/spec/SAFETY_AND_SANDBOX.md` as a set of guarantees. But are those guarantees enforced when a `.em` file is **loaded**, or only when source is **compiled**? If someone hand-crafts a `.em` file (writes the bytes directly, skipping the compiler/sema entirely), which guarantees survive?

2. **Raw x86.** Why does the `.em` format support storing raw x86 machine code at all? Is that a security hole?

This audit answers both. The short version:

- **Almost every ember safety property is compile-side-only.** A hand-crafted `.em` bypasses sema, the lowerer, and the codegen's guard-emission entirely. The loader trusts the bytes. The only load-side gate that meaningfully restrains *content* is the v4 Ed25519 signature (provenance, not safety) and the v5 IR structural validator (which itself has a residual bypass — see Finding A).
- **Raw x86 (v1–v4) is an arbitrary-code-execution surface by construction.** It exists because it was the original format, before the IR backend shipped. It is still accepted in dev mode. The recommendation is to drop it.

Two new confirmed vulnerabilities are documented below (Finding A: a residual frame_off bypass; Finding B: `PERM_FFI` not load-enforced), plus the full compile-side/load-side classification and the raw-x86/IR attack-surface analysis.

---

## 1. The `.em` format — versions, what each stores, how it's loaded

Source: `src/em_file.hpp`, `src/em_writer.cpp`, `src/em_loader.cpp` (impl), `src/em_loader.hpp` (API).

### 1.1 Format versions

| Ver | Name | What the per-function record stores | Signature block? | Loader accepts? |
|-----|------|--------------------------------------|------------------|-----------------|
| v1  | historical | Raw x86 + relocs (5-byte reloc record, no addend) | No | Yes (dev mode) |
| v2  | historical | Raw x86 + relocs (9-byte, with addend) + canonical export signature + symbolic native bindings | No | Yes (dev mode) |
| v3  | F1 visibility | Byte-identical to v2 per-fn; name directory repurposed as EXPORT TABLE (only `pub fn`/bare `fn`) | No | Yes (dev mode) |
| v4  | F2 content auth | Byte-identical to v3 + additive Ed25519 signature block after the name directory | Yes (`EMSG`, 104 bytes) | Yes **only if** signature verifies against a trusted key (signed-only mode); **rejected** in dev mode |
| v5  | Stage B IL-`.em` | **Per-function `is_ir` byte**: `1` → IR blob (`serialize_thin_function` output, opaque to the container); `0` → raw-x86 fallback (byte-identical v4 body). Mixed mode allowed per-function. | No (unsigned for Stage B; v5-signed is FUTURE work) | Yes (dev mode, unsigned branch) |

Constants: `EM_MAGIC = 0x454D424C` ("EMBL"), `EM_VERSION = 4` (default writer), `EM_VERSION_V5 = 5`. Header is 40 bytes: magic, version, flags, function_count, global_size, rodata_total, entry_slot, build_id(lo/hi), target_abi_hash (`em_file.hpp:104-130`).

### 1.2 v1–v4: raw x86 — how it's loaded and executed

`em_loader.cpp` `parse_file` + `load_em_bytes_impl`:

1. `parse_file` reads the header, per-function records (name, slot, code_size, `code` bytes, rodata, relocs, native bindings), globals, name directory, and (v4) the signature block. It validates **structure** only: counts/size against `MAX_*` limits, reloc `offset+8 <= code.size()`, reloc `kind` range, native-binding offset range, native name present in the host allowlist + canonical signature match (`em_loader.cpp:424-426`). For v4 it cross-checks `sig_payload_len` against the parser position (`em_loader.cpp:222-228`).
2. `load_em_bytes_impl` verifies the v4 Ed25519 signature **before** `alloc_executable_rw` (`em_loader.cpp:582-603`).
3. The exec-page loop (`em_loader.cpp:785-821`): for each function, `alloc_executable_rw(code||rodata)` (RW), `memcpy` the bytes, patch each reloc `imm64` (dispatch base / globals base / registry base / function-rodata base), patch each native binding `imm64` with `it->second.fn_ptr`, then `seal_executable` (RX). W^X is honored (RW → patch → RX, never RWX).

**The loader never disassembles, validates, or inspects the semantics of the raw x86 bytes.** It treats them as an opaque blob to map executable. The only content-level gate is the v4 signature (provenance) and the native-binding allowlist (name + signature).

### 1.3 v5: IR blob — how it's re-emitted at load time

`em_loader.cpp:632-763` (the v5 branch of `load_em_bytes_impl`):

1. `parse_file` reads the `is_ir` byte + hoisted signature; for `is_ir=1` it reads `ir_blob_len` + the opaque blob and stores it in `ParsedFn::ir_blob` without interpreting it (`em_loader.cpp:293-322`).
2. After signature-policy handling, the v5 branch builds a `CodeGenCtx ictx` (`em_loader.cpp:638-644`) with `dispatch_base`, `globals_base`, `natives`, `script_slots`, `structs`, and `enable_ir_backend = true`.
3. For each IR function: `deserialize_thin_function` (`thin_ir_ser.cpp`) → native rebind (look up `meta.native_name` in the host table, set `native_fn`, reject on unknown — `em_loader.cpp:665-680`) → `validate_thin_function` (semantic checks — `em_loader.cpp:684-693`) → `emit_x64(thf, ictx)` (re-lower to x64 — `em_loader.cpp:695`). If any step fails, the load is rejected with **no executable page allocated**.
4. The re-emitted `code`/`rodata`/`relocs`/`native_bindings` replace the `ParsedFn`'s fields, and the normal exec-page loop maps them.

So v5 IR is **re-emitted**, not mapped directly. The code goes through `emit_x64`, which is the same emitter the compiler uses. This is the key safety improvement over v1–v4. **But the IR itself is trusted** — `emit_x64` emits whatever the IR says, and `validate_thin_function` is the only semantic gate. Gaps in the validator are exploitable.

### 1.4 What the loader DOES check (exhaustive list)

From `em_loader.cpp` `parse_file` + `load_em_bytes_impl`:

**Structural / size:**
- `magic == EM_MAGIC` (`:158`)
- `version ∈ {1,2,3,4,5}` (`:160-164`)
- `flags == 0` (`:166`)
- v1: reserved fields all zero; v2+: `build_id == EM_BUILD_ID && abi_hash == EM_TARGET_ABI_HASH` (`:168-175`) — the **compatibility** check, not content auth
- `function_count <= MAX_FUNCTIONS`, `global_size <= MAX_GLOBALS`, `rodata_total <= MAX_FILE_SIZE` (`:177-187`)
- per-function min-bytes fit check (`:189-194`)
- `slot_index < MAX_SLOTS`, no duplicate slots (`:243-253`)
- v5: `is_ir ∈ {0,1}`, `ir_blob_len` bounds, `is_ir=1 ⇒ ir_blob_len > 0` (`:266-321`)
- raw-x86 path: `code_size > 0 && <= MAX_CODE_PER_FN`, `rodata_size <= MAX_RODATA_PER_FN`, code+rodata fits (`:327-345`)
- `reloc_count <= MAX_RELOCS_PER_FN`, reloc record size fit (`:359-365`)
- reloc `offset+8 <= code.size()` (via 64-bit subtract), `kind <= max_kind` (version-dependent), addend rules, kind-2 requires registry (`:368-388`)
- v2+ native binding: `offset+8 <= code.size()`, name present in host allowlist, `fn_ptr != null`, **canonical signature match** (`:421-426`)
- `actual_rodata == rodata_total` (`:397`)
- globals block fits (`:402`)
- name directory: `name_count <= MAX_NAMES`, each `slot < MAX_SLOTS && slot_seen[slot]` (`:410-432`)
- v4: trailing bytes == `EM_SIG_BLOCK_SIZE`, `sig_magic == EM_SIG_MAGIC`, `sig_payload_len == payload_end` (`:445-470`)
- v1/v2/v3/v5: trailing bytes == 0 (`:472-475`)
- `entry_slot` names a real slot (`:477-481`)

**Cryptographic (v4 only):**
- Ed25519 `verify(signature, content[0..payload_len], trusted_key)` before `alloc_executable_rw` (`:582-603`)
- dev mode rejects v4 ("a v4 module IS signed, so running it unverified is worse than honest unsigned dev code"); signed-only mode rejects v1/v2/v3/v5 (`:565-580`)

**v5 IR semantic (`validate_thin_function`, `thin_ir_ser.cpp:545-690`):**
- ≥1 block; `blocks[0].id == 0`; every block has a terminator
- `frame_size ∈ [0, 1MB]`; `rbx_save_offset ∈ [-(1MB), -1]`
- every `blk.id < num_blocks`; **no duplicate block ids** (Sec-6 fix)
- VReg bounds (dst/src1/src2/args/term.cond/term.ret `< declared_max_vreg` or 0) — using the **declared** header bound (P1 fix)
- `CallNative ⇒ native_name` non-empty (C2 fix)
- `ConstStringRef`/`StringDecrypt`: `len >= 0` (Item 11) and `addend+len <= rodata.size()` (P2)
- `BoundsCheck`: `len >= 0` (Item 12h)
- **`frame_off` bounds for ANY instr with `frame_off != 0` AND `frame_size > 0`**: must be `∈ [-frame_size, -1]` (Item 12a extended fix) — **see Finding A for the residual gap**
- `CallScript slot < dispatch_size`; `CallCrossModule mod_id < registry_size` (P3)
- `Cmp` predicate `∈ [0,5]` (P4)
- block-target bounds for Jmp/Branch (P3)

### 1.5 What the loader does NOT check (the attack surface)

This is the heart of the audit. The loader does **not** check:

1. **`PERM_FFI` / native permission bits.** The loader's native binding resolution (`em_loader.cpp:424-426` for v2-v4 raw; `:665-680` for v5 IR) checks name presence, `fn_ptr != null`, and canonical signature match. It **never reads `NativeSig::permission`**. → **Finding B**.
2. **Type safety / sema-level rules.** The loader validates the canonical type *shape* (via `em_type_codec` `parse_type`/`validate_canonical_type` — struct flag iff name, array iff array_len, etc.) but does **not** re-run sema. It does not check: no void-to-i64, no int-to-enum without cast, no implicit narrowing, no fn-handle forging, arg/param arity at call sites, return-type matching, struct layout validity, etc. A hand-crafted IR can declare a `CallNative` whose `arg_types` lie about what the raw x86/IR actually passes.
3. **Presence of bounds checks.** `BoundsCheck` is a separate `ThinOp` the lowerer inserts before indexing (`thin_lower.cpp:749-752, 1860-2159`). `emit_x64` emits a bounds check **only if the IR contains a `BoundsCheck` instr** — it never inserts one automatically for `IndexAddr`/element loads (`thin_emit.cpp:1344-1386, 1076-1152`). A hand-crafted IR that omits `BoundsCheck` produces unchecked OOB indexing. Same for `DivOverflowCheck`.
4. **Budget / depth / call-target guard / trap machinery.** These are gated on `CodeGenCtx` host flags. The loader's `ictx` sets **none of them** (`em_loader.cpp:638-644`). See §3.
5. **Raw x86 content (v1–v4, and v5 `is_ir=0` fallback).** No disassembly, no validation. Arbitrary machine code. See §4.
6. **Code/signature consistency.** For v2-v4, the canonical export signature is stored but never cross-checked against what the raw x86 actually does (it can't be — the code is opaque). For v5, the IR's `ret_type`/`arg_types` are trusted, not re-derived.
7. **Export-table honesty.** The name directory is the export table (v3+). The loader reads whatever names/slots the file lists. A hand-crafted `.em` can list a `priv fn`'s slot in the directory, exporting it cross-module (the writer would have omitted it). Minor visibility concern, not a memory-safety bypass.
8. **v5 `frame_size == 0` interaction with the frame_off fix.** → **Finding A**.

---

## 2. Compile-side vs load-side restriction classification

For each ember safety restriction, where is it enforced?

| Restriction | Compile-side (sema/lowerer/codegen) | Load-side (em_loader / IR validator) | Bypassed by hand-crafted `.em`? |
|---|---|---|---|
| **`PERM_FFI` gating** | ✅ sema `:1954` (compile error) | ❌ never checked | **YES** (Finding B) |
| **Type safety** (void-to-i64, int-to-enum, casts, arity, return match, no fn-handle forge) | ✅ sema | ❌ only canonical type *shape* checked | **YES** |
| **Bounds checking** (array/string index) | ✅ lowerer emits `BoundsCheck` ThinOp; codegen tree-walker emits inline | ⚠️ v5: re-emitted **only if the IR contains the instr** (no auto-insert) | **YES** (v5: omit `BoundsCheck`; v1-v4: raw x86 has none) |
| **Call-target guard** (fn handle allowlist) | ✅ codegen `emit_call_target_guard` `:516` (gated on `fn_allowlist_base`) | ❌ loader `ictx.fn_slot_count=0` ⇒ guard emits nothing even if IR has `CallTargetGuard` | **YES** (see §3) |
| **Budget limit** (per-frame + loop back-edge) | ✅ codegen `emit_budget_check` `:397` (gated on `emit_budget_checks`) | ❌ loader `ictx.emit_budget_checks=false` ⇒ `BudgetCheck` ThinOp emits nothing | **YES** |
| **Stack-depth guard** | ✅ codegen `emit_depth_check` `:442` (gated on `emit_depth_checks`) | ❌ loader `ictx.emit_depth_checks=false` ⇒ `DepthCheck` ThinOp emits nothing | **YES** |
| **Trap / checkpoint unwind** | ✅ codegen `emit_trap` `:359` (gated on `trap_stub`; else `ud2`) | ❌ loader `ictx.trap_stub=null` ⇒ traps fall back to `ud2` (process death, no recoverable unwind) | **YES** (no recoverable traps in loaded code) |
| **Div-by-zero / div-overflow** | ✅ codegen `emit_integer_divmod` + `DivOverflowCheck` ThinOp | ⚠️ v5: re-emitted **only if IR contains `DivOverflowCheck`**; the `Div` op itself still emits a div-by-zero check unconditionally | **PARTIAL** (div-by-zero survives v5; overflow check can be omitted) |
| **W^X (no RWX pages)** | ✅ JIT `alloc_executable_rw` → `seal_executable` | ✅ loader uses the same `alloc_executable_rw`/`seal_executable` | NO (this is load-side-enforced) |
| **Size/count DoS limits** | ✅ compile-time limits | ✅ `MAX_*` checks in `parse_file` | NO (load-side-enforced) |
| **v4 content authentication** | ✅ writer signs | ✅ loader verifies Ed25519 before alloc | NO **if** host runs signed-only with a trusted keyring; **YES** in dev mode (the default) |
| **Native allowlist** (can only bind a registered native) | ✅ sema resolves by name | ✅ loader rejects unknown native name | NO (load-side-enforced) — but see `PERM_FFI` (the allowlist doesn't distinguish permission tiers) |
| **Native signature match** | ✅ sema arg-checking | ✅ loader `canonical_type_same` | NO (load-side-enforced) |
| **Frame-off stack-smash** (v5) | n/a (lowerer produces in-range offsets) | ⚠️ validator checks **only if `frame_size > 0`** | **YES** (Finding A) |

**The KEY ANSWER to question 1:** if someone hand-crafts a `.em` file, the restrictions bypassed are: `PERM_FFI`, all type safety, bounds checks, call-target guard, budget, depth, trap unwind, and (for v5) the frame-off guard via `frame_size=0`. The restrictions that **survive** a hand-crafted `.em` are: W^X, size/count DoS limits, native name-allowlist + signature match, and (v4-only, signed-only mode) cryptographic provenance.

The decisive architectural fact: **the loader API has no parameter for safety flags.** `load_em_file`/`load_em_bytes`/`link_em_file` take `registry`, `natives`, and `verify` — nothing for budget/depth/trap/allowlist. The loader builds `ictx` internally with all safety off. So even a host that compiles its own scripts *with* safety on cannot get safety on *loaded* modules.

---

## 3. The safety-flag asymmetry (budget / depth / call-target / trap)

`emit_x64` (the v5 re-emit path) and the tree-walker share the same `CodeGenCtx`-gated guard helpers. Each early-returns when its host flag is off:

- `emit_budget_check`: `if (!ctx.emit_budget_checks || body_cost <= 0) return;` (`thin_emit.cpp:253`)
- `emit_depth_check`: `if (!ctx.emit_depth_checks) return;` (`thin_emit.cpp:271`)
- `emit_call_target_guard`: `if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;` (`thin_emit.cpp:300`)
- `emit_trap`: `if (ctx.trap_stub) { ... } else { ud2 }` (`thin_emit.cpp:235-249`)

At **compile time**, the host sets these flags on its `CodeGenCtx` to turn safety on. At **load time**, the loader's `ictx` (`em_loader.cpp:638-644`) sets:

```cpp
CodeGenCtx ictx;
ictx.dispatch_base = ...;
ictx.globals_base  = ...;
ictx.natives = natives;
ictx.script_slots = &slot_map;
ictx.structs = &empty_structs;
ictx.enable_ir_backend = true;
// emit_budget_checks = false (default)
// emit_depth_checks  = false (default)
// budget_ptr         = nullptr (default)
// depth_ptr          = nullptr (default)
// trap_stub          = nullptr (default)
// trap_ctx           = nullptr (default)
// fn_allowlist_base  = 0 (default)
// fn_slot_count      = 0 (default)
```

**Consequences for a loaded v5 module:**

1. A `BudgetCheck` ThinInstr in the IR emits **nothing** — `emit_budget_checks` is false. A hand-crafted IR that omits `BudgetCheck` is indistinguishable.
2. A `DepthCheck` ThinInstr emits **nothing**. Combined with the fact that the lowerer emits `DepthCheck` as a *separate* instr before each call (`thin_emit.cpp:753-787` comments), a hand-crafted IR that omits it gets unbounded recursion.
3. A `CallTargetGuard` ThinInstr (or the `emit_call_target_guard()` call inside `emit_call` for `CallIndirect`, `thin_emit.cpp:1845`) emits **nothing** because `fn_slot_count == 0`. A `CallIndirect` becomes a raw `call rax` through an unvalidated script-supplied i64 handle — exactly the V2 "i64-as-call-target" surface the guard exists to close (`SAFETY_AND_SANDBOX.md §7a`).
4. Every trap (bounds, div, budget, depth, bad-call-target) falls back to `ud2` (`thin_emit.cpp:248`) because `trap_stub` is null. `ud2` raises `EXCEPTION_ILLEGAL_INSTRUCTION`; without a host VEH this **kills the process** — no recoverable checkpoint unwind. The entire non-local-trap safety model (`SAFETY_AND_SANDBOX.md §2-§7`) is inert for loaded modules.

This is not a bug in `emit_x64` — it's a contract gap: the loader never threads the host's safety configuration into the re-emit context, and the loader API doesn't even accept it.

---

## 4. Raw x86 attack surface (v1–v4, and v5 `is_ir=0`)

### 4.1 What hand-crafted raw x86 can do

A v1–v4 `.em` per-function record is raw x86-64 machine code. The loader maps it to an executable page, patches the reloc `imm64`s and native-binding `imm64`s, and seals it RX. **There is no validation of what the code does.** The bytes are arbitrary.

Therefore hand-crafted raw x86 can:

- Call **any OS API** directly (`syscall`/`int 0x2e` on Windows, or load an address and `call`). It is machine code running with the host process's privileges.
- Read/write **any memory** in the process (the dispatch table, globals block, other modules' pages, the host's heap, the stack).
- **Bypass every ember safety property**: `PERM_FFI` (call the FFI native's address directly, or call `LoadLibrary`/`VirtualAlloc` itself), type safety (reinterpret-cast anything), bounds checks (none exist in raw x86), call-target guard (none), budget/depth (none), trap unwind (none — and `ud2` is just another instruction the attacker won't emit).
- Jump to / call into the **native binding slots** the loader patched — but it can also ignore them and synthesize its own calls.
- Perform **arbitrary code execution** equivalent to loading a native DLL. A `.em` is, in raw-x86 mode, a native code-injection vector.

This is not a subtle bug — it is the definition of "map attacker bytes executable." `SAFETY_AND_SANDBOX.md §1` acknowledges this ("a `.em` module is raw x86-64 native code... it is NOT text the parser/sema/codegen pipeline validates") and pins v4 Ed25519 as the mitigation.

### 4.2 Is the v4 signature a mitigation?

**Partially.** The v4 signature verifies the `.em` was signed by a holder of the trusted private key — it authenticates **provenance**, not **safety**. A signed `.em` from a trusted author is safe *because the author is trusted*, not because the signature says anything about what the code does. The signature does not and cannot validate that the raw x86 obeys `PERM_FFI`, bounds, budget, etc.

The signature is only a real mitigation under **signed-only mode** (`EmVerifyPolicy` with a non-empty keyring). In **dev mode** (empty keyring — the default; every existing test/demo caller uses this), v1/v2/v3 are accepted unsigned, and v5 is accepted unsigned (it falls into the unsigned branch — `em_file.hpp` SECURITY MODEL: "v5 is NOT routed through the v4 Ed25519 verification path"). v4 is *rejected* in dev mode (running signed code unverified is worse than honest unsigned dev code).

So:
- **Dev mode (default):** v1/v2/v3/v5 raw-x86 fallback = unsigned arbitrary code, accepted.
- **Signed-only mode:** v1/v2/v3/v5 rejected; v4 accepted only if signed by a trusted key. The raw x86 is still arbitrary, but it's *trusted-author* arbitrary. This is the standard "signed native artifact" model (signed DLLs / signed firmware) — it defers to the keyholder, it does not sandbox the code.

### 4.3 Can a hand-crafted v1–v4 `.em` bypass all ember safety?

**Yes, all of it.** `PERM_FFI`, type safety, bounds, call-target guard, budget, depth, traps — none exist in raw x86. The raw x86 doesn't go through sema or the ember codegen; it's arbitrary machine code. The only restraints are the OS (DEP, ASLR, the process's own privilege level) and, in signed-only mode, the signature.

---

## 5. v5 IR attack surface

### 5.1 What the IR validator checks (recap)

See §1.4. The validator (`validate_thin_function`) checks structural well-formedness + VReg/block/rodata/slot/cmp-predicate bounds + the frame_off range (gated on `frame_size > 0`) + `CallNative` non-empty name. The loader separately rebinds `CallNative` names against the host table and rejects unknown names (`em_loader.cpp:665-680`).

### 5.2 What the IR validator does NOT check — can a hand-crafted IR...

| Attack | Validator catches it? | Notes |
|---|---|---|
| Reference a native that isn't registered | **Yes** (load-side) | `em_loader.cpp:665-680` rejects unknown `native_name` before validate; validator's C2 check ensures the name is non-empty. **Load-side-enforced.** |
| Reference a native the host marked `PERM_FFI` | **No** | The rebind never checks `permission`. → Finding B |
| Invalid `frame_off` that overwrites the return address | **Partial** | Only if `frame_size > 0`. With `frame_size == 0` the check is skipped. → Finding A |
| Bypass the call-target guard | **Yes (structurally)** for the *guard instr* — but the guard emits nothing because `ictx.fn_slot_count == 0`. A hand-crafted IR can also simply omit `CallTargetGuard` and use `CallIndirect` to `call rax` with an unvalidated handle. | §3 |
| Bypass budget/depth | **Yes (structurally)** — but `BudgetCheck`/`DepthCheck` emit nothing because `ictx` flags are off. A hand-crafted IR can omit them. | §3 |
| Omit `BoundsCheck` before an `IndexAddr`/element load | **No** | `emit_x64` does not auto-insert bounds checks. Omitting `BoundsCheck` = unchecked OOB read/write into the slice/array backing memory (frame slot or global). |
| Omit `DivOverflowCheck` before a signed `Div` | **No** | The `Div` op still emits a div-by-zero check (`thin_emit.cpp:1401` emits `cmp rcx,0; jne` before `idiv`), but the `INT64_MIN / -1` overflow check is only emitted if `DivOverflowCheck` is present. Omitting it → `idiv` raises `#DE` (CPU fault) → process death (no trap stub). |
| Contain arbitrary `ThinOp` sequences that produce wrong/unsafe code | **No** | The validator checks *bounds*, not *semantics*. It does not type-check the IR, does not verify that a `Cast`'s target type is legal, does not verify that a `CallScript`'s arg count matches the target's signature, does not verify `Move`/`Add` operand types agree. A hand-crafted IR can produce type-confused code that reads/writes the wrong width at the wrong slot. |
| Set `frame.rbx_save_offset` to an out-of-frame value | **Partial** | Validator checks `rbx_save_offset ∈ [-(1MB), -1]` but **not** `>= -frame_size`. With `frame_size=0`, `rbx_save_offset=-8` writes `rbx` to `[rbp-8]` (below `rsp`, unallocated stack on Windows) in the prologue (`thin_emit.cpp:495`). Stack write outside the frame. |
| Set `arg_temps_base`/`next_local_off`/param `off` to arbitrary values | **No** | These are not range-checked against `frame_size`. The param-spill / arg-stash emit writes to `[rbp + off]` for these. A hand-crafted IR can write outside the frame via param-spill offsets. (Same class as Finding A; the frame_off fix only covers `instr.meta.frame_off`, not the frame-plan offsets.) |
| Forge a `fn` handle (i64 used as call target) | **No** (sema-only) | Sema forbids i64↔fn assignment. The IR has no such check. A `CallIndirect` with a hand-crafted i64 handle + no `CallTargetGuard` (or with the guard inert because `fn_slot_count=0`) jumps to `dispatch[handle]` — an arbitrary dispatch-table slot, or a wild pointer if `handle` is huge. |

### 5.3 Is v5 safer than v1–v4?

**Yes, materially.** v5 IR is re-emitted through `emit_x64`, so:

- The code is constructed from a bounded `ThinOp` vocabulary, not arbitrary bytes. It can't `syscall` or call raw OS APIs (there is no `ThinOp` for that).
- Native calls go through `CallNative` → name rebind → host allowlist. An unknown native is rejected at load.
- W^X, size limits, and the structural validator all apply before any exec page.
- The validator's bounds checks (VReg, block-target, rodata, slot) close the obvious heap-OOB paths in `emit_x64`.

**But v5 is not safe against a hostile author.** The residual gaps are: Finding A (frame_off via `frame_size=0`), Finding B (`PERM_FFI`), the §3 safety-flag asymmetry (no budget/depth/call-target/trap in loaded code), the ability to omit `BoundsCheck`/`DivOverflowCheck`, the lack of IR-level type checking, and the unchecked frame-plan offsets (`rbx_save_offset`, param `off`, `arg_temps_base`). A hand-crafted v5 IR can still achieve stack corruption (Finding A) and bypass every runtime guard (§3).

---

## 6. Confirmed vulnerabilities (new, with PoC)

### Finding A — `frame_size = 0` bypasses the frame_off bounds check (CRITICAL)

**Files:**
- `src/thin_ir_ser.cpp:649-662` — the "Item 12a fix (extended)" frame_off check, gated on `thf.frame.frame_size > 0`
- `src/thin_ir_ser.cpp:560-563` — the P7 `frame_size` range check, which accepts `0` (`frame_size < 0` rejected; `0` passes)
- `src/thin_emit.cpp:491-494` — the prologue, which **always** does `push rbp; mov rbp, rsp; sub rsp, frame_size` regardless of `frame_size`
- `src/thin_emit.cpp:817-824` (and ~20 other producing ops) — `store_rax_to_rbp(e, in.meta.frame_off)` writes `[rbp + frame_off]`

**Root cause:**

The prior `SECURITY_AUDIT_2026-07-11.md` Finding 1 (ConstInt with `frame_off=+8` overwrites the return address) was fixed by the "Item 12a fix (extended)": check `frame_off` for **any** instr with `frame_off != 0`. But the fix is gated:

```cpp
// thin_ir_ser.cpp:658
if (in.meta.frame_off != 0 && thf.frame.frame_size > 0) {
    if (in.meta.frame_off >= 0 || in.meta.frame_off < -thf.frame.frame_size) {
        return false;  // rejected
    }
}
```

The P7 check (`thin_ir_ser.cpp:561`) accepts `frame_size == 0`:

```cpp
if (thf.frame.frame_size < 0 || thf.frame.frame_size > int32_t(1u << 20)) {
    return false;  // only rejects < 0; 0 passes
}
```

So an attacker sets `frame_size = 0`: the `frame_size > 0` guard is false, the **entire frame_off check is skipped**, and any `frame_off` (including `+8`) is accepted. The prologue (`thin_emit.cpp:492-493`) **always** executes `push rbp; mov rbp, rsp`, so `[rbp+0]` = saved rbp and `[rbp+8]` = the return address regardless of `frame_size`. A producing op (ConstInt, Add, Move, Call*, etc.) with `frame_off = +8` and a controlled `imm.i` writes the attacker's value to the return address. `ret` jumps there.

**PoC:** `tmp_edit/audit_redteam_probe.cpp`, probe_finding_a. Output:

```
=== FINDING A: frame_size=0 bypasses the frame_off bounds check ===

Case A1: ConstInt, frame_size=0, frame_off=+8 (return addr):
  validate_thin_function = TRUE (ACCEPTED)
  >>> CONFIRMED: frame_size=0 skips the frame_off check.
Case A2: round-trip validate = ACCEPTED
  emit_x64 produced 47 bytes
  mov rax, 0x4141414142424242 : FOUND
  mov [rbp+8], rax           : FOUND
  >>> CONFIRMED end-to-end: emitted code overwrites the return address.
Case A3 (contrast): frame_size=256, frame_off=+8 -> rejected (correct)
```

This is the **same primitive** as the prior audit's Finding 1 (arbitrary return-address overwrite → native code execution), reached via a different path (`frame_size=0` instead of an unchecked op). W^X does not stop it (the write targets the stack, RW). The trap model does not stop it (a normal store, not a guard).

**Exploitability:** v5 is LIVE (`em_loader.cpp:161` accepts `version==5`; `em_v5_ir` / `em_v5_mixed` ctests pass). In dev mode (default) a v5 module is accepted unsigned. The IR validator is the only gate before `alloc_executable_rw`. The attacker controls both the write address (`frame_off`) and the write value (`imm.i`). This is a critical arbitrary-code-execution primitive via a hand-crafted v5 `.em`.

**Fix (for the implementer, not applied here):**
1. In the validator, either (a) require `frame_size > 0` for any function with a non-zero `frame_off` on any instr, or (b) drop the `frame_size > 0` gate and check `frame_off ∈ [-max(frame_size, 8), -1]` always (the `8` covers `rbx_save_offset`), or (c) reject `frame_size == 0` outright unless the function has zero instrs with non-zero `frame_off` AND `rbx_save_offset == 0`.
2. Additionally range-check the **frame-plan** offsets (`rbx_save_offset`, `arg_temps_base`, `next_local_off`, each param `off`) against `[-frame_size, -1]`. These are currently unchecked and are written in the prologue/param-spill.

### Finding B — `PERM_FFI` is not enforced at load time (HIGH)

**Files:**
- `src/sema.cpp:1954` — the only `PERM_FFI` check (compile-time, sema)
- `src/em_loader.cpp:424-426` — v2-v4 raw-x86 native binding resolution (no permission check)
- `src/em_loader.cpp:665-680` — v5 IR native rebind (no permission check)
- `src/sema.hpp:55` — `NativeSig::permission` field (never read by the loader)

**Root cause:**

`SAFETY_AND_SANDBOX.md §6` states: "Enforcement point: compile time, at the call-site sema check... a module that doesn't have the permission literally cannot produce code that calls the function, there's no 'check bypassed' path to worry about since the call site doesn't exist in the compiled output."

This is **only true for the compile pipeline.** A hand-crafted `.em` skips sema entirely — the call site *does* exist (in the raw x86's native-binding slot, or in the IR's `CallNative` instr), and the loader binds it because the loader's native resolution checks only: name present in the host allowlist, `fn_ptr != null`, and canonical signature match. It **never reads `NativeSig::permission`.** A grep for `permission`/`PERM_FFI` in `em_loader.cpp`/`em_writer.cpp`/`em_loader.hpp`/`em_file.hpp` returns zero matches.

**PoC:** `tmp_edit/audit_redteam_probe.cpp`, probe_finding_b. It registers a native `ffi_secret` with `permission = PERM_FFI`, builds a v3 `.em` with a native binding to it, and loads it in dev mode:

```
=== FINDING B: PERM_FFI not enforced at load time ===

load_em_file (dev mode, native ffi_secret has PERM_FFI set): SUCCESS (BOUND)
  >>> CONFIRMED: the loader bound a PERM_FFI-flagged native with NO
  >>> permission check. A hand-crafted .em bypasses sema's PERM_FFI gate.
```

A script compiled without `PERM_FFI` could not have produced this binding (sema would reject the call site). A hand-crafted `.em` produces it freely, and the loader binds `&ffi_secret` into the code. A call site in the raw x86 (or a `CallNative` in a hand-crafted IR) reaches the FFI native the host meant to gate.

**Impact:** A hostile `.em` author can call any host-registered native — including `PERM_FFI`-gated filesystem/network/process natives — regardless of the permission tier the host intended for the loading module. This breaks the `PERM_FFI` sandbox boundary for any host that loads `.em` from untrusted sources in dev mode.

**Fix (for the implementer):**
1. Thread a `uint32_t module_permissions` (and/or the `EmVerifyPolicy` / a new `EmLoadPolicy`) into `load_em_file`/`load_em_bytes`/`link_em_file`.
2. In the native binding resolution (both the v2-v4 path `em_loader.cpp:424-426` and the v5 IR rebind `:665-680`), reject when `(it->second.permission & PERM_FFI) && !(module_permissions & PERM_FFI)`.
3. Document that `PERM_FFI` is now enforced at both compile and load time (update `SAFETY_AND_SANDBOX.md §6`).

---

## 7. The raw-x86 question — should it be dropped?

### 7.1 Purpose of raw x86 in v1–v4

Raw x86 is the **original** `.em` format. Before the thin-IR backend shipped (Stage A/B), the only way to serialize a compiled function was to write the tree-walker's emitted x86 bytes plus the reloc table. v1→v4 are progressive refinements of that model (v2: canonical sigs + symbolic natives; v3: export table; v4: Ed25519). The loader maps the bytes executable because that's what a pre-IR artifact *is*: native code.

### 7.2 Is it still needed now that v5 (IR) exists?

**No — not for newly-produced modules.** v5 IR is strictly better:
- Portable (the IR has no process-local pointers; raw x86 bakes native fn ptrs / trap stubs / allowlist bases that are process-local — `SAFETY_AND_SANDBOX.md §1` and `em_file.hpp` note that functions using trap stubs / fn-ref allowlists are `non_serializable` and can't ship raw x86 soundly).
- Re-emittable: the load-time re-emit produces code matched to the host's current dispatch/globals/registry bases.
- Validatable: structural + semantic checks before any exec page.
- Auditable: the IR is a bounded op vocabulary, not arbitrary bytes.

The only legitimate use of raw x86 today is **back-compat** for existing v1–v4 `.em` artifacts and the v5 `is_ir=0` fallback for functions the IR backend can't yet serialize (aggregate/string/struct/defer gaps — `ThinFunction::non_serializable`). The fallback is a compatibility shim, not a design goal.

### 7.3 Impact of dropping v1–v4 (raw x86) + going v5-IR-only

- **Breaking change for existing `.em` files.** Any v1–v4 artifact would need re-emission from source, or a one-time v4→v5 migration tool. The bundler (`docs/BUNDLING_AND_EM_MODULES.md`) and the standalone-exe stub (which appends a `.em` to the stub exe) would need to emit v5 only.
- **The v5 `is_ir=0` raw-x86 fallback must also be dropped** (or restricted) to fully close the surface — otherwise a hand-crafted v5 module just sets `is_ir=0` on every function and gets raw x86 back. Dropping the fallback means every function must be IR-serializable; the current non-serializable gaps (aggregates/strings/structs/defer) would need IR backend support, or those features would be unloadable from `.em` until the gaps close.
- **Major security improvement.** It removes the arbitrary-code-execution-by-construction surface (§4) and forces all loaded code through the IR validator + `emit_x64`.
- **The v5-signed variant becomes the content-authentication layer** (FUTURE work per `em_file.hpp`) — once it lands, signed-only mode gives both provenance and the IR safety gate.

### 7.4 Recommendation

**Drop raw x86.** Concretely:

1. **Short term (immediate hardening, no format break):** In dev mode, the loader should **refuse v1, v2, v3** outright and accept only v4 (signed) and v5. Add a host opt-in flag `EmLoadPolicy::allow_legacy_raw_x86` (default false) for hosts that genuinely need to load old artifacts. This makes dev mode safe-by-default against the §4 surface without breaking hosts that explicitly opt in.
2. **Short term:** Restrict the v5 `is_ir=0` fallback. Either reject `is_ir=0` entirely (v5-IR-only), or require a v5-signed block (once it lands) for any `is_ir=0` function. A hand-crafted v5 module must not be able to smuggle raw x86 via `is_ir=0`.
3. **Medium term:** Land the v5-signed variant and make signed-only mode the documented default for production hosts. Dev mode becomes "v5 IR, unsigned, validated" — safe-by-construction (modulo the validator gaps §5.2).
4. **Long term:** Close the v5 non-serializable gaps so `is_ir=0` is never needed, then remove the raw-x86 code path from the loader entirely.

---

## 8. Recommendations (consolidated)

### 8.1 Move restrictions from compile-side to load-side

The following are currently compile-side-only and **must** gain load-side enforcement to be real guarantees for `.em`:

1. **`PERM_FFI`** — Finding B. Thread `module_permissions` into the loader; check at native bind/rebind. **Highest priority** (it's a documented sandbox boundary that's currently bypassable).
2. **Budget / depth / call-target guard / trap stub** — §3. Thread the host's `CodeGenCtx` safety flags (or a dedicated `EmLoadPolicy` carrying `emit_budget_checks`/`budget_ptr`/`emit_depth_checks`/`depth_ptr`/`trap_stub`/`trap_ctx`/`fn_allowlist_base`/`fn_slot_count`/`use_context_reg`) into `load_em_file`/`load_em_bytes`/`link_em_file` and into the v5 re-emit `ictx`. Without this, **no loaded module has any runtime guard**, ever. This is the largest single gap.
3. **Bounds-check presence (v5)** — either (a) make `emit_x64` auto-insert a bounds check before any `IndexAddr`/element load that lacks one, or (b) have the validator reject an `IndexAddr`/element-load not preceded by a `BoundsCheck` in the same block. (a) is simpler and matches the tree-walker's "always checked, never flag-disabled" rule (`SAFETY_AND_SANDBOX.md §5`).

### 8.2 Strengthen the v5 IR validator

1. **Finding A:** close the `frame_size == 0` frame_off gap. Either require `frame_size > 0` when any `frame_off != 0`, or drop the `frame_size > 0` gate. Also range-check the frame-plan offsets (`rbx_save_offset`, `arg_temps_base`, `next_local_off`, param `off`) against `[-frame_size, -1]` — these are written in the prologue/param-spill and are currently unchecked.
2. **Type-check the IR** (or re-run a sema-like pass on the deserialized IR). The validator currently checks *bounds*, not *semantics*. At minimum: verify each `Cast`'s target type is in the legal cast matrix; verify `CallScript`/`CallNative` arg count + arg types match the target signature (the target sigs are available: native sigs from the host table, script sigs from `signatures_by_slot`); verify `Move`/binop operand types agree. This is the difference between "the IR is well-formed" and "the IR is type-safe." See §8.5.
3. **Require `DivOverflowCheck` before every signed `Div`/`Mod`** (or fold the overflow check into the `Div`/`Mod` emit so it's unconditional, like div-by-zero already is).
4. **Reject `CallIndirect` without a preceding `CallTargetGuard`** when the host has an allowlist configured — otherwise the guard is optional and a hand-crafted IR skips it.

### 8.3 Drop raw x86

Per §7.4: refuse v1–v3 in dev mode by default; restrict/eliminate the v5 `is_ir=0` fallback; land v5-signed; close the non-serializable gaps; eventually remove the raw-x86 loader path.

### 8.4 Should the loader re-run sema on the deserialized IR?

**Not sema itself** — sema operates on the AST, which doesn't exist at load time (the IR is AST-free by design, `thin_ir.hpp` serialization boundary). But a **dedicated IR type-checker** that performs the sema-equivalent checks on the `ThinFunction` (arg/param arity + type match at calls, cast legality, operand type agreement, no fn-handle forge) is the right load-side analogue. It would run between `deserialize_thin_function` and `emit_x64`, reject on failure, and close most of §5.2's "validator checks bounds not semantics" gaps. The validator is necessary but not sufficient; an IR type-checker is the missing half.

### 8.5 Minimal set of load-side checks for a hand-crafted `.em` to be safe

If only a minimal set can be added, in priority order:

1. **Finding A fix** (frame_off check regardless of `frame_size`) — closes the critical stack-smash primitive. ~3 lines.
2. **Finding B fix** (`PERM_FFI` check at native bind) — closes the sandbox boundary bypass. Requires threading `module_permissions` through the API.
3. **Safety flags into `ictx`** (§3) — budget/depth/call-target/trap. Requires an `EmLoadPolicy` API addition. Without this, loaded code has no runtime guards.
4. **Bounds-check presence requirement** (v5) — either auto-insert in `emit_x64` or validator-reject on missing `BoundsCheck`.
5. **Drop v1–v3 + v5 `is_ir=0` raw-x86 acceptance** in dev mode (§7.4).

After (1)-(5), a hand-crafted v5 `.em` would face: a structural validator with no frame_off gap, an IR type-checker, load-side `PERM_FFI`, runtime budget/depth/call-target/trap, mandatory bounds checks, and no raw-x86 escape. That is the minimal set that makes "load a `.em` from an untrusted source in dev mode" a safe operation. Anything less leaves at least one of: stack smash (1), FFI bypass (2), no runtime guards (3), OOB indexing (4), arbitrary code (5).

---

## 9. What is already verified safe (load-side)

To be precise about what the loader *does* get right (so fixes target the real gaps, not the working parts):

- **W^X** — `alloc_executable_rw` → patch → `seal_executable`, never RWX (`em_loader.cpp:785-821`). Same path as JIT. Verified safe.
- **Size/count DoS limits** — every disk-controlled count/size bounded before allocation (`em_loader.cpp:177-187, 243-253, 327-345, 359-365, 421-426`); checked add/mul helpers prevent overflow. Verified safe.
- **Cursor/reader overflow** — `Reader::take` uses 64-bit `checked_add` + `end > bytes.size()`. Verified safe.
- **Reloc offset/kind/addend bounds** — `em_loader.cpp:368-388`. Verified safe.
- **Native name allowlist + signature match** — `em_loader.cpp:424-426` (v2-v4), `:665-680` (v5). Verified safe (but does not check `permission` — Finding B).
- **v4 Ed25519 verification before alloc** — `em_loader.cpp:582-603`, with `sig_payload_len` cross-check. Verified safe (provenance only, in signed-only mode).
- **v5 IR re-emit before alloc** — deserialize → rebind → validate → emit, all before `alloc_executable_rw`; reject = no exec page. Verified safe (modulo validator gaps).
- **v5 structural validator** — block/VReg/rodata/slot/cmp bounds, duplicate-block-ids (Sec-6 fix), CallNative non-empty name (C2 fix), ConstStringRef/StringDecrypt rodata bounds (P2), BoundsCheck len≥0 (Item 12h), Cmp predicate range (P4). Verified safe (modulo Finding A's `frame_size=0` gap).

The prior audit's Finding 1 (frame_off for unchecked ops) and Finding 6 (duplicate block IDs) have both been **fixed** in the current source (the "Item 12a fix (extended)" at `thin_ir_ser.cpp:649` and the `seen_ids` check at `:589-606`). This audit's Finding A is a **residual bypass of that fix** via the `frame_size=0` gate, not a duplicate of the original finding.

---

## 10. Probe

| Probe | Purpose | Result |
|-------|---------|--------|
| `tmp_edit/audit_redteam_probe.cpp` | (A) `frame_size=0` frame_off bypass, end-to-end serialize→deserialize→validate→emit; (B) `PERM_FFI` not load-enforced, v3 `.em` binds a `PERM_FFI` native in dev mode | **Both CONFIRMED** |

Built with:
```
g++ -std=c++17 -O3 -DNDEBUG -DED25519_NO_SEED -Isrc -Ithirdparty/ed25519 \
    -o tmp_edit/audit_redteam_probe.exe tmp_edit/audit_redteam_probe.cpp \
    -Lbuildt -lember_frontend -lember -lember_ed25519 -lkernel32 -luser32
```

No source files edited. The probe is under `tmp_edit/` (gitignored).

---

## 11. Severity summary

| # | Severity | Finding | Load-side? |
|---|----------|---------|------------|
| A | **CRITICAL** | `frame_size=0` bypasses the frame_off bounds check → return-address overwrite → native code execution from a hand-crafted v5 `.em` | validator gap (load-side) |
| B | **HIGH** | `PERM_FFI` enforced only in sema; loader binds FFI-gated natives with no permission check | compile-side-only (missing load-side) |
| §3 | **HIGH** | Budget / depth / call-target guard / trap stub never threaded into the loader's re-emit `ictx`; loaded modules have no runtime guards | compile-side-only (missing load-side) |
| §4 | **CRITICAL (by construction)** | v1–v4 + v5 `is_ir=0` raw x86 = arbitrary machine code, accepted in dev mode; bypasses all ember safety | design (raw x86 surface) |
| §5.2 | **MEDIUM-HIGH** | v5 IR validator checks bounds not semantics; can omit `BoundsCheck`/`DivOverflowCheck`; frame-plan offsets unchecked; no IR type-check | validator gap (load-side) |
| §8 | — | Recommendations: drop raw x86, thread safety flags into the loader, add an IR type-checker, close Finding A/B | — |

**Bottom line:** ember's safety model is a **compile-time contract**. It is real and well-engineered for source-compiled scripts. It is **not** a load-time contract: a hand-crafted `.em` bypasses sema, the lowerer, and the guard-emission, and the loader trusts the bytes (raw x86) or the IR (with a residual validator gap). The v4 signature authenticates provenance in signed-only mode but does not sandbox code. To make `.em` loading safe against a hostile author, the safety flags must move to the load side, the raw-x86 surface should be dropped, and the IR validator needs a type-checking pass plus the Finding A fix. This audit documents the gaps precisely enough for those fixes to be implemented from it.
