# Final Red Team — ember `.em` Module Format Security (Follow-Up Audit)

**Date:** 2026-07-11
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**Submodule HEAD:** `acf01d0`
**Fix commit under review:** `fd5304d` "Security: fix .em Finding A (frame_size=0) + Finding B (PERM_FFI load-side) + drop raw x86 v1-v4"
**Prior audit:** `docs/audit/EM_FORMAT_RED_TEAM_2026-07-11.md`
**Auditor scope:** READ-ONLY. No source files edited. No probe built (the findings are confirmed by reading the code + the existing passing test `em_v5_mixed`).
**Build/test:** `cd buildt && ctest -E bench --timeout 30` -> **52/52 pass**, including `em_redteam_audit` (#45), `em_v5_ir` (#46), `em_v5_mixed` (#52), `thin_ir_ser` (#44). `git status` clean.

---

## 0. Executive summary

The `fd5304d` fix commit addressed the three items the prior audit raised:

| Prior finding | Fix in `fd5304d` | Verdict |
|---|---|---|
| **Finding A** (`frame_size=0` skips the `frame_off` bounds check) | `thin_ir_ser.cpp:664` — the `frame_size > 0` gate was dropped; the check is now `if (in.meta.frame_off != 0)` (unconditional) | **FIXED for the `instr.meta.frame_off` path.** Residual bypass via the frame-plan offsets — see **Finding C** below. |
| **Finding B** (`PERM_FFI` not load-enforced) | `em_loader.cpp:432` (v2-v4 raw) + `em_loader.cpp:710` (v5 IR rebind) — both now check `(it->second.permission & PERM_FFI) && !(module_permissions & PERM_FFI)` | **FIXED.** Both paths covered. `EmLoadPolicy::module_permissions` threaded through `parse_file` + `load_em_bytes_impl` + the public API. Verified solid. |
| **Raw x86 v1-v4** (arbitrary-code-execution surface) | `em_loader.cpp:581` — `if (parsed.version != EM_VERSION_V5 && !allow_raw_x86)` rejects v1-v4 by default; `EmLoadPolicy::allow_raw_x86` defaults false | **INCOMPLETE.** Rejects v1-v4 by *version*, but a v5 module with `is_ir=0` functions carries raw x86 through the v5 door and is accepted by the same "secure default." See **Finding D** below (CRITICAL). |

**Two new confirmed vulnerabilities** + two carried-forward gaps:

- **Finding C (HIGH->CRITICAL):** the Finding A fix closed `instr.meta.frame_off` but left the **frame-plan offsets** (`rbx_save_offset`, `struct_ret_ptr_offset`, `params[].off`) unvalidated against `[-frame_size, -1]`. These are written to `[rbp + offset]` in the prologue/param-spill. A `param.off = +8` spills the first argument register (attacker-controlled) to `[rbp+8]` = the return address. **Same primitive as Finding A, via a different unvalidated offset.**
- **Finding D (CRITICAL):** the raw-x86 drop rejects v1-v4 by version only. A v5 module with `is_ir=0` functions is accepted by the secure default (`allow_raw_x86=false`) and those functions are raw x86 — arbitrary machine code. **The existing passing test `em_v5_mixed` is a live, end-to-end demonstration of the bypass.**
- **Finding E (carried forward, HIGH):** the prior audit's section 3 safety-flag asymmetry (budget / depth / call-target guard / trap stub never threaded into the loader's v5 re-emit `CodeGenCtx`) is **unchanged** — `fd5304d` did not touch the `ictx` construction. Loaded v5 modules still have no runtime guards.
- **Finding F (MEDIUM, new observation):** every production caller (CLI, bundle, standalone-exe stub, all back-compat tests) passes `EmLoadPolicy{..., allow_raw_x86=true}`, so the "secure default" is opted out of in every real deployment. The raw-x86 surface is still wide open in practice.

The fixes that landed are real and correct for what they target. Finding A and Finding B are closed on their original paths. But the raw-x86 drop has a hole (Finding D), and the frame-off fix has a sibling hole (Finding C) — both reachable from a hand-crafted v5 `.em` in dev mode (the default).

---

## 1. Verification — Finding A fix (`frame_size=0` frame_off bypass)

### 1.1 The fix

`src/thin_ir_ser.cpp:664` (current source):

```cpp
// FINDING A: the prior version gated this check on
// `frame_size > 0`, so a hand-crafted IR with frame_size == 0
// skipped the check entirely ... The fix: ALWAYS
// check frame_off regardless of frame_size. When frame_size == 0,
// -frame_size == 0, so any non-zero frame_off (positive or
// negative) fails the range check and is rejected.
if (in.meta.frame_off != 0) {
    if (in.meta.frame_off >= 0 || in.meta.frame_off < -thf.frame.frame_size) {
        if (err) *err = "thin_ir_ser: validate: frame_off out of frame bounds";
        return false;
    }
}
```

### 1.2 Verification

- **Unconditional?** Yes. The `thf.frame.frame_size > 0` guard is gone. The only remaining gate is `frame_off != 0` (the "not frame-backed, safe to skip" case), which is correct — `frame_off == 0` means the instr does not write to `[rbp + frame_off]`.
- **Correct when `frame_size == 0`?** Yes. With `frame_size == 0`, `-thf.frame.frame_size == 0`. The range test becomes `frame_off >= 0 || frame_off < 0` — i.e., any non-zero `frame_off` (positive **or** negative) fails and is rejected. There is no value of `frame_off != 0` that passes when `frame_size == 0`. The original Finding A PoC (ConstInt, `frame_size=0`, `frame_off=+8`) is now rejected.
- **Correct when `frame_size > 0`?** Yes. The test is `frame_off >= 0` (rejects positive — would hit saved rbp / return addr) **or** `frame_off < -frame_size` (rejects below the frame). The surviving range is `[-frame_size, -1]`, which is exactly the allocated frame.
- **Other bypass paths for `instr.meta.frame_off`?** None found. The check runs for **every** instr in **every** block (it is inside the `for (const auto& in : blk.instrs)` loop, not gated on `in.op`). It covers all ~20 producing ops that `emit_x64` routes through `store_rax_to_rbp(e, in.meta.frame_off)`. There is no op-specific carve-out.

**Verdict: Finding A is FIXED on the `instr.meta.frame_off` path.** The fix is minimal, correct, and unconditional. `em_redteam_audit_test` does not re-test this specific case, but the code path is unambiguous.

### 1.3 Residual bypass — Finding C (see section 4)

The fix covers `instr.meta.frame_off` (the per-instruction destination offset). It does **not** cover the **frame-plan offsets** — `rbx_save_offset`, `struct_ret_ptr_offset`, and `params[].off` — which are also written to `[rbp + offset]` in the prologue and param-spill, and which the validator does not range-check against `[-frame_size, -1]`. These are a sibling attack surface to the one Finding A closed. See section 4.

---

## 2. Verification — Finding B fix (`PERM_FFI` load-side enforcement)

### 2.1 The fix

`EmLoadPolicy::module_permissions` (new, `em_loader.hpp:130`, default `0`) is threaded through `load_em_file` / `load_em_bytes` / `link_em_file` -> `load_em_bytes_impl` -> `parse_file`. Two enforcement points:

**v2-v4 raw-x86 native binding resolution** (`em_loader.cpp:432`, inside `parse_file`'s binding loop):
```cpp
if((it->second.permission&PERM_FFI)&&!(module_permissions&PERM_FFI)){
    set_error(err,"em_loader: binding: native \""+b.name+"\" requires PERM_FFI permission (module lacks it)");
    return false;
}
```

**v5 IR native rebind** (`em_loader.cpp:710`, inside `load_em_bytes_impl`'s v5 branch, per-CallNative-instr):
```cpp
if ((it->second.permission & PERM_FFI) && !(module_permissions & PERM_FFI)) {
    set_error(err, "em_loader: v5 IR: native \"" + in.meta.native_name + "\" in \"" +
                   pf.name + "\" requires PERM_FFI permission (module lacks it)");
    return false;
}
```

### 2.2 Verification

- **Both paths covered?** Yes. The raw path (v2-v4, and v5 `is_ir=0` which falls through to the same v2+ binding parse) checks at `parse_file:432`. The v5 IR path checks at rebind `:710`. A `grep -n "PERM_FFI\|permission" src/em_loader.cpp` returns exactly these two sites plus the `module_permissions` resolution — no other native-binding resolution exists.
- **`module_permissions` correctly resolved?** Yes. `load_em_bytes_impl:567`: `const uint32_t module_permissions = load_policy ? load_policy->module_permissions : 0u;`. Null policy (the API default) -> `0` -> no FFI natives bound. This is the secure default and matches the compile-side default (a module compiled without `PERM_FFI` cannot call FFI natives).
- **`parse_file` receives `module_permissions`?** Yes. The signature was extended (`em_loader.cpp:211`) and `load_em_bytes_impl:571` passes it through. The raw-path check at `:432` uses the same value the v5 IR check at `:710` uses — no drift.
- **Checked before the native ptr is baked into exec memory?** Yes. The `parse_file` check runs during parsing (before any `alloc_executable_rw`). The v5 IR check runs during rebind (before `emit_x64`, before any `alloc_executable_rw`). On rejection, no exec page is allocated. This is the correct ordering.
- **Other native-permission bypass paths?** None found. The only two places a native `fn_ptr` is resolved and (eventually) baked into executable memory are (a) `parse_file`'s binding loop (raw x86 imm64 patch, later written at `em_loader.cpp:827`) and (b) the v5 IR rebind (sets `in.native_fn`, which `emit_x64` bakes into a native-fixup imm64, later written at `:827` from `cf.native_fixups`). Both are gated. There is no third path.
- **What about `natives == nullptr`?** If a v2+ module (or v5 `is_ir=0` function) has `binding_count > 0` and `natives` is null, `parse_file:425` rejects ("v2 module requires a host native allowlist"). If `binding_count == 0`, no bindings exist and the exec-page loop's `for(const auto& b:f.native_bindings)` at `:827` is a no-op (no `natives->find` call). For v5 IR, the rebind's `else` branch (`:728`) rejects if any `CallNative` exists and `natives` is null. So `natives` is never dereferenced when null. Safe. (Minor defensive note: `:827` does `natives->find(b.name)` without an explicit null check — it relies on the invariant that `native_bindings` is non-empty only when `natives` is non-null. This is currently true but fragile; a future code change that could populate `native_bindings` without setting `natives` would turn `:827` into a null deref. Not a vulnerability today.)

**Verdict: Finding B is FIXED.** Both the raw and IR paths check `PERM_FFI` against `module_permissions` before any exec page. The `EmLoadPolicy` API addition is clean, additive, and secure-by-default. `em_redteam_audit_test` confirms both the raw and v5 IR rejection cases.

---

## 3. Verification — raw x86 drop (v1-v4 refused by default)

### 3.1 The fix

`em_loader.cpp:581`:
```cpp
if (parsed.version != EM_VERSION_V5 && !allow_raw_x86) {
    set_error(err, "em_loader: format: raw x86 format v" +
                   std::to_string(parsed.version) +
                   " rejected by default (only v5 IR accepted); " +
                   "pass EmLoadPolicy{allow_raw_x86=true} for back-compat");
    return false;
}
```

`EmLoadPolicy::allow_raw_x86` defaults `false` (`em_loader.hpp:131`). Null policy -> `allow_raw_x86 = false` (`em_loader.cpp:568`).

### 3.2 Verification — v1-v4 rejection

- **v1-v4 refused by default?** Yes. `parsed.version != EM_VERSION_V5` is true for v1/v2/v3/v4 (`EM_VERSION == 4`, `EM_VERSION_V5 == 5`). With `allow_raw_x86 = false`, the module is rejected. The check runs **before** the signature/dev-mode policy (`:581` is before `:588`), so a v1-v4 module is rejected regardless of signature status.
- **`allow_raw_x86` secure (not default-on)?** The **API default** is secure: `EmLoadPolicy::allow_raw_x86 = false`, and null policy -> false. **But every production caller overrides it to true** — see Finding F (section 6). The "secure default" is the API default, not the deployment default.
- **Is the check bypassable for v1-v4?** No. The version is read from the header and validated against `{1,2,3,4,5}` at `parse_file:191-195` before this check. A hand-crafted file cannot lie about its version to bypass this (version 0 or 6+ is rejected at `:193`; version 1-4 hits this check; only version 5 passes).

**Verdict: v1-v4 rejection is FIXED.** The hole is not in the v1-v4 path — it is in the v5 `is_ir=0` path, which the version-only check does not cover. See Finding D (section 5).

### 3.3 The gap the fix missed — Finding D (see section 5)

The check is `parsed.version != EM_VERSION_V5`. A **v5** module passes this check regardless of `allow_raw_x86`. But a v5 module can contain `is_ir=0` functions, which carry raw x86 (the v4 per-fn body). The loader's v5 branch skips `is_ir=0` functions (`em_loader.cpp:679`: `if (pf.ir_blob.empty()) continue;  // raw-x86 fallback — skip`), and the exec-page loop maps their raw x86 executable. So a hand-crafted v5 module with `is_ir=0` on every function is **accepted by the secure default** and carries arbitrary machine code. This is the bypass. See section 5.

---

## 4. NEW — Finding C: frame-plan offsets bypass the Finding A fix (HIGH->CRITICAL)

### 4.1 What the Finding A fix did not cover

The Finding A fix range-checks **one** rbp-relative offset: `instr.meta.frame_off` (the per-instruction destination, written by `store_rax_to_rbp(e, in.meta.frame_off)` for ~20 producing ops). But `emit_x64`'s prologue and param-spill write to `[rbp + offset]` using **three other offsets** that come straight from the deserialized `ThinFunction::frame` and are **not range-checked against `[-frame_size, -1]`**:

| Offset | Where written | Validator check | Value written |
|---|---|---|---|
| `frame.rbx_save_offset` | prologue `thin_emit.cpp:497`: `store_reg_mem(rbp, rbx_save_offset, rbx)` | `thin_ir_ser.cpp:565`: `>= 0 \|\| < -(1<<20)` — **NOT** `>= -frame_size` | `rbx` (callee-saved, not attacker-controlled) |
| `frame.struct_ret_ptr_offset` | param-spill `thin_emit.cpp:564`: `spill_word(0, struct_ret_ptr_offset, nullptr)` -> `store_reg_mem(rbp, off, rcx)` | **NONE** (deserialized at `thin_ir_ser.cpp:404`, never validated) | `rcx` = the hidden return-pointer arg (caller-controlled buffer address) |
| `frame.params[].off` | param-spill `thin_emit.cpp:609` etc.: `spill_word(param_word, p.off, ...)` -> `store_reg_mem(rbp, p.off, int_arg_regs[w])` | **NONE** (deserialized at `thin_ir_ser.cpp:419`, never validated) | `rcx`/`rdx`/`r8`/`r9` = the integer argument registers (caller-controlled) |

(`frame.arg_temps_base` and `frame.next_local_off` are also unvalidated, but they are frame-plan fields consumed by the **lowerer** — at load time the IR is already lowered and these are not used directly as rbp-relative offsets in `emit_x64`. `ra->save_offsets` is also unvalidated, but `RegAllocResult` is not serialized — a deserialized `ThinFunction` has `ra.enabled = false` (`thin_ir.hpp:302-303`), so the prologue's `if (ra && ra->enabled)` save loop is skipped at load time. So the live attack surface at load time is the three offsets above.)

### 4.2 The exploit — `params[].off = +8` overwrites the return address

The prologue (`thin_emit.cpp:491-494`) always does:
```
push rbp          ; [rsp] = saved rbp, [rsp+8] = return addr
mov rbp, rsp      ; rbp points at saved rbp; [rbp+0] = saved rbp, [rbp+8] = return addr
sub rsp, frame_size
```

After the prologue, `[rbp+0]` = saved rbp and `[rbp+8]` = the **return address**, regardless of `frame_size` (the `sub rsp, frame_size` only grows the frame downward; it does not move `[rbp+8]`).

`emit_param_spills` then writes each integer argument register to `[rbp + p.off]`. If a hand-crafted IR sets `params[0].off = +8`, the first argument register (`rcx`) is written to `[rbp+8]` = the return address. The argument value is **controlled by the caller** — if the attacker controls the call (or the function is reachable via the dispatch table with a controlled first argument), the attacker controls the return address. `ret` jumps to it.

This is the **same primitive** as Finding A (arbitrary return-address overwrite -> native code execution), reached via `params[].off` instead of `instr.meta.frame_off`. The Finding A fix does not cover it because the fix only checks `instr.meta.frame_off`, not the frame-plan offsets.

### 4.3 Why the validator does not catch it

`validate_thin_function` (`thin_ir_ser.cpp:545-690`) checks:
- `frame_size in [0, 1MB]` (`:561`) — accepts 0
- `rbx_save_offset in [-(1MB), -1]` (`:565`) — checks sign + 1MB cap, **not** `>= -frame_size`
- `struct_ret_ptr_offset` — **not checked at all**
- `params[].off` — **not checked at all**

So `params[0].off = +8` passes validation (no check exists), survives deserialization, and `emit_x64`'s param-spill writes `rcx` to `[rbp+8]`.

### 4.4 Exploitability

- **v5 is LIVE.** `em_loader.cpp:191` accepts `version == 5`; `em_v5_ir` / `em_v5_mixed` ctests pass. In dev mode (default) a v5 module is accepted unsigned.
- **The attacker controls the write address** (`params[].off` / `struct_ret_ptr_offset` — unvalidated) **and the write value** (the argument register, which the caller supplies). For `params[].off`, the caller must supply a controlled first argument; for `struct_ret_ptr_offset = +8`, the caller must supply a controlled hidden return-pointer (which happens iff `returns_struct_by_ptr` is set in the hand-crafted IR and the function is called as a struct-returning function).
- **W^X does not stop it** (the write targets the stack, RW). **The trap model does not stop it** (a normal spill store, not a guard). **The Finding A fix does not stop it** (it checks `instr.meta.frame_off`, not `params[].off`).

### 4.5 PoC shape (not built — READ-ONLY audit; the code path is unambiguous)

A hand-crafted v5 IR function with:
- `frame.frame_size = 0` (or any value — `params[].off` is unchecked regardless)
- `frame.params = [{ ty: i64, off: +8 }]` (one i64 param, offset +8 = the return address)
- `returns_struct_by_ptr = false` (so `struct_ret_ptr_offset` is not spilled first)
- A body that does nothing and returns

`emit_param_spills` writes `rcx` (the caller's first arg) to `[rbp+8]`. The caller invokes the function via the dispatch table with `rcx = <target address>`. The function returns to `<target address>`. Arbitrary control-flow hijack.

### 4.6 Fix (for the implementer)

In `validate_thin_function`, after the existing frame-plan sanity checks, add range checks for **every** rbp-relative offset the prologue/param-spill/restore write, against `[-frame_size, -1]`:

1. `rbx_save_offset`: require `rbx_save_offset >= -frame_size` (in addition to the existing `>= -(1<<20)` and `< 0` checks). If `frame_size == 0`, `rbx_save_offset` must be `0` (no save) — but the prologue unconditionally writes it, so either reject `frame_size == 0` when `rbx_save_offset != 0`, or require `rbx_save_offset == 0` when `frame_size == 0`.
2. `struct_ret_ptr_offset`: if `returns_struct_by_ptr`, require `struct_ret_ptr_offset in [-frame_size, -1]` (and `!= 0` — the spill is gated on `!= 0` at `:563`, so `0` skips the spill, but a non-zero positive value is the exploit).
3. `params[].off`: for each param with `ty != nullptr`, require `p.off in [-frame_size, -1]` (and the `p.off + words_for_type*8` extent must also be `>= -frame_size`). A positive `p.off` is the exploit.
4. **Generalize:** the Finding A fix established the rule "any rbp-relative write offset must be in `[-frame_size, -1]`." Apply that rule to *all* rbp-relative offsets, not just `instr.meta.frame_off`. The frame-plan offsets are rbp-relative writes too.

This is the same class of fix as Finding A, extended to the frame-plan offsets the first audit's section 8.2 item 1 already named ("Also range-check the frame-plan offsets (`rbx_save_offset`, `arg_temps_base`, `next_local_off`, each param `off`) against `[-frame_size, -1]`"). The `fd5304d` fix implemented the `instr.meta.frame_off` half but not the frame-plan half.

---

## 5. NEW — Finding D: v5 `is_ir=0` fallback bypasses the raw-x86 drop (CRITICAL)

### 5.1 The bypass

The raw-x86 rejection at `em_loader.cpp:581` checks **only the top-level version**:
```cpp
if (parsed.version != EM_VERSION_V5 && !allow_raw_x86) { ... reject ... }
```

A **v5** module passes this check regardless of `allow_raw_x86`. But the v5 per-function record has an `is_ir` byte (`em_file.hpp:50`): `is_ir=1` -> IR blob (re-emitted through `emit_x64` + the validator); `is_ir=0` -> **raw x86** (the v4 per-fn body, byte-identical, mapped executable with no validation). The v5 loader branch explicitly skips `is_ir=0` functions and lets them be handled as raw x86:

```cpp
// em_loader.cpp:679
if (pf.ir_blob.empty()) continue;  // raw-x86 fallback — skip
```

So a hand-crafted v5 module with `is_ir=0` on every function:
1. Passes the version check (`:581`) — it IS v5.
2. Passes `parse_file` — the `is_ir=0` path (`:334`) reads `code_size`/`code`/`rodata`/`relocs`/`native_bindings` exactly like v3/v4.
3. Is skipped by the v5 re-emit branch (`:679`) — `ir_blob` is empty.
4. Is mapped executable by the exec-page loop (`:795-820`) — `alloc_executable_rw(f.code + f.rodata)`, `memcpy`, patch relocs, patch native bindings, `seal_executable`. **The raw x86 is arbitrary machine code, mapped RX with no validation.**

This is the **full section-4 attack surface from the prior audit** (arbitrary OS API calls, arbitrary memory read/write, bypass of every ember safety property) — reachable through the v5 door that the fix left open.

### 5.2 Confirmation — the existing test IS the PoC

`examples/em_v5_mixed_test.cpp` builds a 2-function v5 module:
- `add` (is_ir=1, IR blob)
- `double` (is_ir=0, **raw x86**)

and loads it with **no `EmLoadPolicy`** (the "secure default", `allow_raw_x86 = false`):

```cpp
// em_v5_mixed_test.cpp:172
bool loaded = load_em_file(tmp.string().c_str(), lm, &lerr, nullptr, &natives);
```

The test **passes** (ctest #52 `em_v5_mixed`): the raw-x86 `is_ir=0` function is loaded and called (`double(21) == 42`). This is a live, end-to-end demonstration that the secure default accepts a v5 module carrying raw x86. The bypass is not theoretical — it is a first-class, tested, passing code path.

### 5.3 Why the fix missed it

The fix's intent (per the `EmLoadPolicy` doc comment, `em_loader.hpp:117-120`) was: "if false (the default), the loader REFUSES v1-v4 (raw x86) modules ... Only v5 (IR, re-emitted through emit_x64 + the structural validator) is accepted." The implementation reads "v5" as "IR" — but v5 is **mixed-mode**: a v5 module can be all-IR, all-raw-x86, or a mix. The version check cannot distinguish "v5 all-IR" (safe) from "v5 all-raw-x86" (unsafe). The per-function `is_ir` byte is the only signal that distinguishes them, and the fix does not consult it.

The prior audit flagged this explicitly (section 7.4 item 2: "Restrict the v5 `is_ir=0` fallback. Either reject `is_ir=0` entirely (v5-IR-only), or require a v5-signed block ... for any `is_ir=0` function. A hand-crafted v5 module must not be able to smuggle raw x86 via `is_ir=0`."). The `fd5304d` fix implemented section 7.4 item 1 (refuse v1-v3 in dev mode) but **not** item 2 (restrict v5 `is_ir=0`).

### 5.4 Exploitability

- **Live.** v5 is accepted in dev mode (default). `em_v5_mixed` passes. A hand-crafted v5 module with `is_ir=0` functions is indistinguishable from a legitimate mixed-mode module at the version-check layer.
- **Full raw-x86 power.** The `is_ir=0` function's bytes are mapped executable with no disassembly/validation. Per the prior audit section 4.1: arbitrary OS API calls (`syscall`), arbitrary memory read/write, bypass of `PERM_FFI` (call the native's address directly, or call `LoadLibrary`/`VirtualAlloc` itself), bypass of every runtime guard. Equivalent to loading a native DLL.
- **PERM_FFI check is moot for is_ir=0.** The `parse_file:432` PERM_FFI check runs on the native *bindings* (the symbolic native call sites the format records). But raw x86 is not bound to the binding table — it can call the native's address directly (the loader patches the binding imm64 at `:827`, and the raw x86 can read/call that address), or ignore the bindings entirely and synthesize its own calls. The Finding B fix restrains the *symbolic* binding surface; it does not restrain raw x86. So for a v5 `is_ir=0` function, Finding B is bypassed too — the raw x86 can reach any native, FFI-gated or not, by direct address.

### 5.5 Fix (for the implementer)

The raw-x86 rejection must be **per-function**, not per-module-version. In `load_em_bytes_impl`, after `parse_file`, add:

```cpp
if (parsed.version == EM_VERSION_V5) {
    for (const auto& pf : parsed.functions) {
        if (pf.ir_blob.empty() && !allow_raw_x86) {  // is_ir=0 + secure default
            set_error(err, "em_loader: format: v5 function \"" + pf.name +
                           "\" is raw-x86 (is_ir=0), rejected by default; " +
                           "pass EmLoadPolicy{allow_raw_x86=true} for back-compat");
            return false;
        }
    }
}
```

This makes `allow_raw_x86` gate **all** raw x86 — v1-v4 (by version) AND v5 `is_ir=0` (per-function) — under one flag. The secure default then refuses any raw x86 regardless of how it is packaged. The `em_v5_mixed_test` would need to pass `EmLoadPolicy{0u, true}` (which it already should, as a back-compat test) — a one-line fix to the test.

Alternatively (stricter): split into `allow_v1_v4` and `allow_v5_raw_fallback`, or reject v5 `is_ir=0` unconditionally (v5-IR-only) and make the mixed mode require the explicit opt-in. The prior audit's section 7.4 item 2 recommended exactly this.

---

## 6. NEW — Finding F: every production caller opts out of the secure default (MEDIUM)

### 6.1 Observation

`EmLoadPolicy::allow_raw_x86` defaults `false` (the "secure default"). But every production caller passes `true`:

| Caller | Policy | `allow_raw_x86` |
|---|---|---|
| `examples/ember_cli.cpp:415` (load-em path) | `{opts.ffi_mode ? PERM_FFI : 0u, true}` | **true** |
| `examples/ember_cli.cpp:971` | `{0u, true}` | **true** |
| `examples/ember_cli.cpp:1149` | `{0u, true}` | **true** |
| `examples/ember_cli.cpp:1514` | `{ffi_mode ? PERM_FFI : 0u, true}` | **true** |
| `examples/ember_bundle.cpp:287` | `{PERM_FFI, true}` | **true** |
| `examples/ember_stub_main.cpp:189` | `{PERM_FFI, true}` | **true** |

Every back-compat test (`em_bytes_test`, `em_loader_hardening_test`, `em_roundtrip_test`, `em_signed_test`, `import_roundtrip_test`, `pub_priv_test`, `v0_5_live_modules_test`, `aggregate_global_test`) uses `RAW_X86_POLICY{0u, true}`.

The only callers that rely on the secure default (`allow_raw_x86=false` / null) are `em_redteam_audit_test` (the fix-verification test) and `em_v5_mixed_test` (which, per Finding D, inadvertently demonstrates the bypass).

### 6.2 Impact

The raw-x86 drop is a **structural** improvement (the API default is secure) but **not a deployment improvement**: every real entry point (CLI, bundle, standalone stub) opts into raw x86. A user running `ember --load-em module.em` through the CLI is loading v1-v4 raw x86 with no signature, no validation. The secure default is only exercised by the audit test.

This is not a code bug — it is a back-comat decision (the CLI must load existing v1-v4 artifacts). But it means the raw-x86 surface is still the **practical** default for every real user, even after the fix. The security gain from `fd5304d`'s raw-x86 drop is realized only by a hypothetical caller that passes null/no policy — and no production caller does.

### 6.3 Recommendation

- The CLI's `--load-em` path should distinguish "load a trusted/bundled .em" (back-comat, `allow_raw_x86=true` acceptable) from "load an untrusted .em" (should refuse raw x86). Today there is no such distinction.
- Once Finding D is fixed (v5 `is_ir=0` refused by default), the secure default becomes meaningful for v5 modules — a v5 module from an untrusted source would be forced through the IR validator. The CLI should expose a "strict" mode that passes null policy (no raw x86, no FFI) for untrusted modules.
- The `em_v5_mixed_test` should pass `EmLoadPolicy{0u, true}` explicitly, to document that it is exercising the back-compat path, not the secure default.

---

## 7. Carried forward — Finding E: safety-flag asymmetry unchanged (HIGH)

### 7.1 Status

The prior audit section 3 documented that the loader's v5 re-emit `CodeGenCtx ictx` (`em_loader.cpp:638-649`) sets **none** of the runtime safety flags:

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
// use_context_reg    = false (default)
```

A `grep -n "emit_budget_checks\|emit_depth_checks\|fn_allowlist_base\|fn_slot_count\|trap_stub\|budget_ptr\|depth_ptr\|use_context_reg" src/em_loader.cpp` returns **no matches** — confirming `fd5304d` did not touch this. The asymmetry is **unchanged**.

### 7.2 Consequence (unchanged from prior audit section 3)

For a loaded v5 IR module (the path the fix is trying to make safe):
1. `BudgetCheck` ThinInstr emits **nothing** — `emit_budget_checks` is false. A hand-crafted IR that omits `BudgetCheck` is indistinguishable.
2. `DepthCheck` ThinInstr emits **nothing** — `emit_depth_checks` is false. Unbounded recursion.
3. `CallTargetGuard` emits **nothing** — `fn_slot_count == 0`. A `CallIndirect` becomes a raw `call rax` through an unvalidated script-supplied i64 handle.
4. Every trap falls back to `ud2` — `trap_stub` is null. No recoverable checkpoint unwind; traps kill the process.

The `EmLoadPolicy` struct added by `fd5304d` carries `module_permissions` and `allow_raw_x86` but **no** safety-flag fields. The loader API still has no way to receive the host's budget/depth/call-target/trap configuration. So even a host that compiles its own scripts *with* safety on cannot get safety on *loaded* modules.

### 7.3 Recommendation (carried forward)

Extend `EmLoadPolicy` (or a companion struct) with the safety-flag fields: `emit_budget_checks`, `budget_ptr`, `emit_depth_checks`, `depth_ptr`, `trap_stub`, `trap_ctx`, `fn_allowlist_base`, `fn_slot_count`, `use_context_reg`. Thread them into the v5 re-emit `ictx`. This is the prior audit's section 8.1 item 2 and remains the largest single gap. Without it, no loaded v5 module has any runtime guard, ever.

---

## 8. Other carried-forward gaps (not addressed by `fd5304d`, still open)

These were documented in the prior audit section 5.2 / 8.2 and are **not** addressed by `fd5304d` (the commit touched only `thin_ir_ser.cpp:23 lines` + the loader PERM_FFI/raw-x86 checks). Listed for completeness — they remain exploitable from a hand-crafted v5 IR module:

1. **Bounds-check presence not required.** `emit_x64` emits a bounds check only if the IR contains a `BoundsCheck` instr; it does not auto-insert one for `IndexAddr`/element loads. A hand-crafted IR that omits `BoundsCheck` produces unchecked OOB indexing. (Prior section 5.2 / 8.1 item 3.)
2. **No IR type-checking.** The validator checks *bounds*, not *semantics*. It does not verify cast legality, call-site arg/param arity + type match, operand type agreement, or fn-handle forging. A hand-crafted IR can produce type-confused code. (Prior section 5.2 / 8.2 item 2 / 8.4.)
3. **`DivOverflowCheck` not required** before a signed `Div`/`Mod`. Omitting it -> `idiv` raises `#DE` on `INT64_MIN / -1` -> process death (no trap stub). (Prior section 5.2 / 8.2 item 3.)
4. **`CallIndirect` without `CallTargetGuard`** is not rejected. A hand-crafted IR skips the guard and `call rax` with an unvalidated handle. Compounded by Finding E (`fn_slot_count == 0` makes the guard inert even if present). (Prior section 5.2 / 8.2 item 4.)

These are the "validator checks bounds, not semantics" gaps. The minimal fix for each is in the prior audit section 8.2. They are not regressions from `fd5304d`; they are pre-existing gaps that the fix did not scope in.

---

## 9. What `fd5304d` got right (verified safe)

To be precise about the fix's reach:

- **Finding A (`instr.meta.frame_off`):** FIXED. The check is unconditional and correct for all `frame_size` values. No bypass found on this path. (`thin_ir_ser.cpp:664`.)
- **Finding B (PERM_FFI):** FIXED at both the raw (`parse_file:432`) and v5 IR (`load_em_bytes_impl:710`) native-binding resolution points. `module_permissions` correctly threaded; null policy -> 0 -> no FFI. No other native-binding path exists. (`em_loader.cpp:432, 710`; `em_loader.hpp:130`.)
- **v1-v4 rejection:** FIXED for the version-gated path. v1/v2/v3/v4 are refused when `allow_raw_x86=false`, before signature/dev-mode handling. (`em_loader.cpp:581`.)
- **`EmLoadPolicy` API design:** Clean, additive, secure-by-default (null -> `{0, false}`). The default args preserve source compatibility for existing callers. (`em_loader.hpp:128-131, 238, 260`.)
- **Test coverage:** `em_redteam_audit_test` (new, 405 lines) covers (a) PERM_FFI rejection on raw v3, (b) PERM_FFI rejection on v5 IR, (c) v3 rejected by default / accepted with `allow_raw_x86=true` / rejected with `false`, (d) v5 IR accepted by default. All pass.
- **No regressions:** 52/52 ctests pass. The existing `em_v5_ir`, `em_v5_mixed`, `thin_ir_ser`, and all back-compat tests continue to pass.

The fix is well-engineered for what it targets. The findings below are what it did not target.

---

## 10. Severity summary

| # | Severity | Finding | Status | Load-side? |
|---|----------|---------|--------|------------|
| A | **CRITICAL->FIXED** | `frame_size=0` bypasses the `instr.meta.frame_off` bounds check | **FIXED** (`fd5304d`, `thin_ir_ser.cpp:664`) — unconditional, correct | validator (load-side) |
| B | **HIGH->FIXED** | `PERM_FFI` not load-enforced | **FIXED** (`fd5304d`, both raw + IR paths) | loader (load-side) |
| — | **FIXED** | Raw x86 v1-v4 accepted in dev mode | **FIXED for v1-v4** (`fd5304d`, version-gated) | loader (load-side) |
| **C** | **HIGH->CRITICAL** | Frame-plan offsets (`rbx_save_offset`, `struct_ret_ptr_offset`, `params[].off`) unvalidated -> `params[].off=+8` overwrites return address with caller-controlled arg | **NEW, OPEN** — sibling of Finding A, not covered by the fix | validator gap (load-side) |
| **D** | **CRITICAL** | v5 `is_ir=0` fallback bypasses the raw-x86 drop — v5 module with raw-x86 functions accepted by the secure default; `em_v5_mixed` test is the live PoC | **NEW, OPEN** — the fix checks version, not per-function `is_ir` | loader gap (load-side) |
| **E** | **HIGH** | Safety-flag asymmetry: budget/depth/call-target/trap not threaded into v5 re-emit `ictx`; loaded modules have no runtime guards | **carried forward, OPEN** — `fd5304d` did not touch `ictx` | compile-side-only (missing load-side) |
| **F** | **MEDIUM** | Every production caller passes `allow_raw_x86=true`; the secure default is opted out of in every real deployment | **NEW observation** — back-comat decision, not a code bug | deployment |
| section 8 | **MEDIUM-HIGH** | Bounds-check presence not required; no IR type-checking; `DivOverflowCheck` optional; `CallIndirect` without guard | **carried forward, OPEN** — pre-existing, out of `fd5304d` scope | validator gap (load-side) |

---

## 11. Minimal fix priority (for the implementer)

In priority order, to close the residual surface:

1. **Finding D** (v5 `is_ir=0` raw-x86 bypass) — make the raw-x86 rejection per-function, not per-version. ~6 lines in `load_em_bytes_impl` after `parse_file`. One-line update to `em_v5_mixed_test` (pass `EmLoadPolicy{0u, true}`). This is the highest priority: it is a CRITICAL, live, tested bypass of the fix's headline change (raw-x86 drop).
2. **Finding C** (frame-plan offsets) — range-check `rbx_save_offset`, `struct_ret_ptr_offset`, and every `params[].off` against `[-frame_size, -1]` in `validate_thin_function`. ~10 lines. Closes the sibling of Finding A — same primitive (return-address overwrite), different unvalidated offset.
3. **Finding E** (safety flags) — extend `EmLoadPolicy` with budget/depth/call-target/trap fields; thread into the v5 re-emit `ictx`. Larger API change; the largest single gap but the biggest surface to design.
4. **Finding F** (caller opt-out) — CLI strict mode for untrusted `.em`; document the back-comat vs. untrusted distinction. Process/policy, not code.
5. **section 8 gaps** — bounds-check presence requirement, IR type-checker, `DivOverflowCheck` requirement, `CallIndirect` guard requirement. The prior audit's section 8.2 has the specifics.

After (1) and (2), a hand-crafted v5 `.em` in dev mode faces: no raw-x86 escape (D), no return-address overwrite via any rbp-relative offset (A + C), and PERM_FFI enforcement (B). After (3) it also has runtime guards. That is the path to "load an untrusted v5 `.em` in dev mode safely."

---

## 12. Bottom line

The `fd5304d` fix is **correct and complete for Finding A's `instr.meta.frame_off` path and for Finding B**, and it **correctly refuses v1-v4 raw x86 by version**. The fix is well-tested (52/52 pass, new `em_redteam_audit_test` covers the fix cases) and introduces no regressions.

But the fix is **incomplete in two ways that the prior audit already named**:

- **Finding C:** the prior audit's section 8.2 item 1 said "Also range-check the frame-plan offsets (`rbx_save_offset`, `arg_temps_base`, `next_local_off`, each param `off`) against `[-frame_size, -1]`." The fix implemented the `instr.meta.frame_off` half only. The frame-plan half is still open — `params[].off = +8` is the same return-address-overwrite primitive as Finding A, via a sibling offset. (HIGH->CRITICAL.)

- **Finding D:** the prior audit's section 7.4 item 2 said "Restrict the v5 `is_ir=0` fallback ... A hand-crafted v5 module must not be able to smuggle raw x86 via `is_ir=0`." The fix implemented section 7.4 item 1 (refuse v1-v3) only. The v5 `is_ir=0` fallback is still open — and the existing `em_v5_mixed` test is a live, passing, end-to-end demonstration of a v5 module with raw-x86 functions accepted by the secure default. (CRITICAL.)

Both are reachable from a hand-crafted v5 `.em` in dev mode (the default). Finding D is the more severe because it restores the full raw-x86 arbitrary-code-execution surface that the fix's headline change was supposed to remove. Finding C is the more subtle because it is a sibling of the very bug the fix just closed.

The recommendation is to land the Finding D per-function raw-x86 check and the Finding C frame-plan offset range checks as a follow-on to `fd5304d`. Both are small, localized, and have clear test shapes (`em_v5_mixed` for D; a `params[].off=+8` rejection test for C). With those two, the `.em` format's load-side story against a hand-crafted v5 module in dev mode becomes: no raw-x86 escape, no return-address overwrite, PERM_FFI enforced. Finding E (safety flags) and the section 8 semantic gaps remain the longer-term work.
