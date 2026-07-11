# Architecture & Design Audit — ember

**Date:** 2026-07-11
**Scope:** Read-only deep audit of architecture + design quality.
**Repo:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD stated:** `bf76217` — **actual HEAD:** `7c73361` ("Hourly audit: fix 15 compiler warnings in codegen/thin_lower/peephole/parser; refresh stale doc line-cites and CTest counts"). The audit was run against the actual HEAD (`7c73361`), one commit ahead of the stated base. Build: `buildt/` (MinGW g++ 15.2.0, C++17, Release). **32/32 ctest pass** (re-verified: `100% tests passed, 0 tests failed out of 32`, 80.91s).
**Mode:** Read-only. No source edited. Findings written to this file only.

---

## Methodology

Read every header and implementation TU in the audit scope:
- `CMakeLists.txt` (library structure, link graph)
- Pass system: `src/ember_pass.hpp`, `src/ember_pass_registry.hpp`, `src/ember_pass_pipeline.hpp`, `src/ember_pass.cpp`
- IR: `src/thin_ir.hpp`, `src/thin_ir.cpp`, `src/thin_ir_ser.hpp`, `src/thin_ir_ser.cpp`
- `.em` format: `src/em_file.hpp`, `src/em_loader.hpp`, `src/em_loader.cpp`, `src/em_writer.hpp`, `src/em_writer.cpp`, `src/em_type_codec.hpp`, `src/em_type_codec.cpp`
- Codegen dual-path: `src/codegen.hpp`, `src/codegen.cpp`, `src/thin_lower.hpp`, `src/thin_lower.cpp`, `src/thin_emit.hpp`, `src/thin_emit.cpp`, `src/peephole.hpp`, `src/peephole.cpp`
- Extensions: all 11 `extensions/*/ext_*.{hpp,cpp}` + `src/binding_builder.hpp`
- Error handling: `src/sema.hpp`, `src/sema.cpp`, `src/engine.hpp`, `src/engine.cpp`
- Link-direction verification: include-graph scan of every core-lib TU against every frontend header.

Severity scale used below: **DESIGN ISSUE** (architectural flaw worth fixing), **MINOR** (style/ hygiene/ redundancy, no behavioral impact), **WELL DESIGNED** (calls out a design choice that is notably sound).

---

## 1. Library Structure (CMakeLists.txt)

### WELL DESIGNED — three-library split with a clean one-way link direction

The `ember` (core) / `ember_frontend` / `ember_import` split is clean and the link direction is one-way: `ember_frontend PUBLIC ember`, `ember_import` standalone, extensions `PUBLIC ember_frontend`. Verified by scanning every core-lib TU (`jit_memory.cpp`, `engine.cpp`, `em_writer.cpp`, `module_registry.cpp`, `thin_ir.cpp`, `em_type_codec.cpp`, `thin_ir_ser.cpp`) for frontend-header includes — **zero hits**. The core lib never references `codegen.hpp`/`sema.hpp`/`parser.hpp`/`lexer.hpp`/`thin_lower.hpp`/`thin_emit.hpp`/`peephole.hpp`/`types.hpp`/`ember_pass.hpp`.

The self-containment discipline is documented inline in `CMakeLists.txt:42-62` (the `thin_ir.cpp` placement comment) and `CMakeLists.txt:64-72` (the `em_type_codec.cpp` comment): both files "touch Type ONLY through public data members + the Prim enum — it never calls a Type method (those live in `types.cpp` / `ember_frontend`)." Verified: `thin_ir.cpp` uses a local `prim_name()` helper (`thin_ir.cpp:101-103`) instead of `Type::to_string()`; `em_type_codec.cpp` reads `t.prim`/`t.is_slice`/`t.array_len`/`t.struct_name`/`t.elem` directly (`em_type_codec.cpp:102-116`) with no Type method calls.

### WELL DESIGNED — `em_loader.cpp` correctly placed in `ember_frontend`

`em_loader.cpp` is in `ember_frontend` (`CMakeLists.txt:85-93`, the "Stage B c2" comment). The rationale is sound: the v5 IR re-emit path (`deserialize_thin_function` → `validate_thin_function` → `emit_x64`) needs frontend symbols (`emit_x64` from `thin_emit.cpp`, `CodeGenCtx` from `codegen.hpp`). The header `em_loader.hpp` stays in `src/` (shared include dir) so both libs can see the API; only the `.cpp` moved. `em_writer.cpp` stays in core (it has no frontend dependencies — verified: `em_writer.cpp` includes only `em_writer.hpp`, `em_type_codec.hpp`, std headers, and ed25519). This is the correct placement.

### WELL DESIGNED — pass extensions (opt, obf) correctly structured

`ember_add_extension(opt ...)` and `ember_add_extension(obf ...)` (`CMakeLists.txt:173-174`) use the same `ember_add_extension` function as the nine native extensions (`CMakeLists.txt:163-171`), linking `PUBLIC ember_frontend`. The opt/obf extensions depend on `ember_pass.hpp` + `ember_pass_registry.hpp` + `thin_ir.hpp` (all in `src/`, reached via `ember_frontend`'s PUBLIC include). No special-casing, no separate function. Consistent.

### MINOR — `ast.hpp` gratuitously includes `lexer.hpp`, transitively coupling core headers to a frontend TU

`src/ast.hpp:3` includes `src/lexer.hpp`, but `ast.hpp` uses **nothing** from `lexer.hpp` — no `Tk`, no `Token`, no `LexResult`, no `tokenize` (verified by grep). `Loc` is defined in `ast.hpp:65` itself. This include appears vestigial. The downstream effect: `thin_ir.hpp` (a core-lib header) includes `ast.hpp` which pulls in `lexer.hpp` (whose `tokenize()` lives in `lexer.cpp`, a frontend TU). This is **header-only** coupling — the core lib never calls `tokenize()`, so there is no link-level circular dependency — but it means a core header transitively depends on a frontend header for definitions it does not use. Removing the include (or factoring `Loc` + the shared token-type enums into a smaller `src/loc.hpp` that both `ast.hpp` and `lexer.hpp` include) would tighten the boundary. Low priority.

**File:line:** `src/ast.hpp:3`

---

## 2. Pass System (ember_pass.hpp / registry / pipeline)

### WELL DESIGNED — type-erased concept-based polymorphism is sound

`ember_pass.hpp:48-62` implements Sean Parent's concept-based polymorphism: `PassConcept` (virtual interface) + `PassModel<T>` (templated wrapper) + `vector<unique_ptr<PassConcept>>` storage. No vtable-per-pass-class boilerplate, value semantics for `add_pass`, passes are plain structs. The `EmberPassInfoMixin<DerivedT>` CRTP mix-in (`ember_pass.hpp:32-39`) provides `name()` + `is_required()` with a clean default (`is_required = false`) and an opt-in override (`static constexpr bool is_required = true;` in `ext_obf.hpp:27`). This is the right shape.

### WELL DESIGNED — `add_pass` / `add_pass_concept` split is clean and motivated

`ember_pass.hpp:73-82` has two insertion paths:
- `add_pass<T>(T p)` — the typed path (host constructs a pass and type-erases it).
- `add_pass_concept(unique_ptr<PassConcept> p)` — the already-erased path (the pipeline string parser uses this).

This split is necessary because the registry (`ember_pass_registry.hpp:24-29`) creates passes by name via a `PassFactory` lambda that returns `unique_ptr<PassConcept>`, and the pipeline parser (`ember_pass_pipeline.hpp:34-36`) moves that erased pointer directly into the manager. Forcing it back through a typed `add_pass<T>` would require a type switch the registry does not have. The two methods share the same `passes_.push_back(std::move(...))` storage, so there is no divergence in the data structure. Clean.

### WELL DESIGNED — registry pattern is consistent with the extension pattern

`ember_pass_registry.hpp:24-29` (`reg.add<ConstPropPass>("constprop")`) is the direct mirror of the native-binding `register_natives(NativeTable&)` pattern. Each pass extension provides `register_passes(EmberPassRegistry&)` (`ext_opt.hpp:54`, `ext_obf.hpp:31`) the same way each native extension provides `register_natives(...)`. The factory is a `std::function<unique_ptr<PassConcept>()>` — a fresh instance per `create()` call, so pipeline parsing does not share mutable pass state. `has()` and `names()` (`ember_pass_registry.hpp:32-44`) support `--list-passes`. Consistent and complete.

### WELL DESIGNED — `run_to_fixpoint` convergence is correct

`ember_pass.cpp:17-29`: `run()` starts with `Preserved::all()` and intersects each pass's return; `run_to_fixpoint` breaks when `round_result.all_preserved()` is true. Since a pass that **changes** the IR returns `Preserved::none()` (all_ = false) and a pass that does nothing returns `Preserved::all()` (all_ = true), the intersection is `all()` **iff** every pass returned `all()` (did nothing). This is the correct convergence signal. `max_rounds = 8` is a sane safety cap. The `is_required` bypass (`ember_pass.cpp:11`) correctly runs obfuscation passes even when a `before_pass` gate returns false — the check is `p->is_required() || instrumentation.run_before_pass(...)`, short-circuiting the gate for required passes.

### MINOR — `ConstPropPass::run` has a redundant return branch

`ext_opt.cpp:275-279`:
```cpp
if (changed && count_instrs(f) != before)
    return EmberPreserved::none();
if (changed)
    return EmberPreserved::none();
return EmberPreserved::all();
```
Both `changed` branches return `none()`. The `count_instrs(f) != before` sub-condition is dead — if `changed` is true, `none()` is returned regardless of whether the instr count changed. This is harmless (the behavior is correct: any change → `none()`) but the first `if` could be removed or the intent (distinguishing "changed but count same" from "changed and count differs") documented. The `count_instrs(f) != before` check was likely intended to feed a different return contract (e.g. "return all if structurally identical") but the contract is uniform `none()` on any change.

**File:line:** `extensions/opt/ext_opt.cpp:275-279`

### WELL DESIGNED — `EmberAnalysisManager` stub is the right forward-compatible shape

`ember_pass.hpp:117-127` ships an empty `EmberAnalysisManager` with a documented future API (`getResult<T>`, `getCachedResult<T>`, `invalidate`). Passes take `EmberAnalysisManager&` from day one, so the interface is stable when analyses arrive. This avoids a flag-day refactor. The comment explicitly references `PASS_SYSTEM_DESIGN §6` for the growth path. Good.

---

## 3. IR Design (thin_ir.hpp)

### WELL DESIGNED — ThinOp enum is stable, complete for the lowered subset, and append-only

`thin_ir.hpp:129-156`: the `ThinOp : uint16_t` enum is explicitly documented as a serialization-stable spine ("Do NOT renumber, reorder, or insert ... append new ops at the END only" — `thin_ir.hpp:116-118`). The serializer validates the range with `THIN_OP_LAST = static_cast<uint16_t>(ThinOp::CallTargetGuard)` (`thin_ir_ser.cpp:167`, checked at `thin_ir_ser.cpp:459`), so a blob with an out-of-range op ordinal is rejected. The enum covers every operation the tree-walker performs for the lowered subset: literals (4), memory (6), int arith (13), float arith (5), compare (1), short-circuit (2), cast (1), calls (4), aggregates (8), guards (3). The coverage is complete **for the subset that is lowered** — for-each and match fall back to the tree-walker (`thin_lower.cpp:949-972`), so they need no ThinOp yet. This is the documented Stage A scope.

### WELL DESIGNED — representation conventions are pinned and sound

`thin_ir.hpp:67-110` pins the representation conventions with explicit c2-produces/c3-consumes language:
- Scalar/float = 1 VReg; Slice = 2 consecutive VRegs (ptr at v, len at v+1) — matches the tree-walker's `{rax=ptr, rdx=len}` ABI.
- Struct/fixed-array = NOT a VReg; represented by `meta.frame_off` (absolute rbp-negative offset). Struct/array temps are never register candidates.
- Frame refs = absolute rbp-negative offsets (emit does not recompute layout — `ThinFramePlan` carries the pre-computed plan).
- Struct-by-value arg = `args[i] = 0` (sentinel) + `arg_frame_offs[i] = slot offset` — a clean sentinel scheme that avoids a parallel tag array.

These conventions are duplicated (intentionally, for contract visibility) in `thin_lower.hpp:38-100` with the lowering-side specifics. The two headers cross-reference each other. Sound.

### WELL DESIGNED — serialization boundary is clean and documented

`thin_ir.hpp:111-124` and `thin_ir_ser.hpp:13-44` pin the serialization boundary explicitly:
- `ThinOp` serialized verbatim (stable IDs).
- `native_fn` (raw ptr) DROPPED — rebind by `meta.native_name` at load time (`thin_ir_ser.hpp:20-22`). The loader's native-rebind loop (`em_loader.cpp:700-725`) looks up `native_name` in the host table and rejects on unknown name (the v5 security gate).
- `const Type*` fields encoded via `em_type_codec`'s `emit_type`/`parse_type` with a `has_type:u8` prefix; reconstructed `Type` objects live in `ThinFunction::owned_types` so the pointers stay valid (`thin_ir.hpp:191-198`, `thin_ir_ser.hpp:26-29`).
- `abs_fixups` NOT serialized (populated by `emit_x64` at load time); `rodata` IS serialized.

The `non_serializable` + `declared_max_vreg` + `owned_types` fields on `ThinFunction` (`thin_ir.hpp:176-201`) are all documented as **additive** (default-constructed empty/0, untouched by `lower_function`/`emit_x64`/`dump`), so the `thin_ir_struct` ctest that hand-builds a `ThinFunction` stays green. Good additive discipline.

### WELL DESIGNED — `declared_max_vreg` independent bound is a sound validation ceiling

`thin_ir.hpp:199-201` + `thin_ir_ser.cpp:568-578`: the validator uses `declared_max_vreg` (from the blob header) as the VReg ceiling for deserialized functions, falling back to `compute_max_vreg` only for JIT-lowered functions (`declared_max_vreg == 0`). The comment at `thin_ir_ser.cpp:569-573` explains why: recomputing from the function's own VReg references is tautological (every reference is < recomputed_max by construction), so the declared bound is an independent ceiling an attacker cannot inflate. This is the correct defensive choice.

---

## 4. .em Format (em_file.hpp)

### WELL DESIGNED — v5 is additive and backward-compatible

`em_file.hpp:73-101` documents the v5 format precisely: the 40-byte header, globals block, and name directory are **unchanged** from v3/v4; only the per-function record is redesigned with an `is_ir` byte selecting between an IR blob (`is_ir=1`) and a raw-x86 fallback (`is_ir=0`, byte-identical to the v4 per-fn body after the hoisted signature). The `EmFunctionRecord::ir_blob` field (`em_file.hpp:251-258`) is additive (default-constructed empty, untouched by v1-v4 paths). `EM_VERSION` stays 4 (the default writer version); `EM_VERSION_V5 = 5` is a distinct constant the loader accepts (`em_loader.cpp:170-173`). The loader's version dispatch (`em_loader.cpp:198-213`) handles v1/v2/v3/v4/v5 in one chain. Backward-compatible.

### WELL DESIGNED — mixed-mode (IR + raw-x86) is sound

A v5 module may MIX IR and raw-x86 functions per-function: IR-serializable fns ship `ir_blob`; non-serializable fns (obf/for-each/match, flagged by `ThinFunction::non_serializable`) ship raw x86. On disk, `is_ir` is derived as `!ir_blob.empty()` (`em_file.hpp:255-256`). The loader's v5 re-emit loop (`em_loader.cpp:660-760`) skips `pf.ir_blob.empty()` functions (raw-x86 fallback) and re-emits the rest, replacing `pf.code`/`pf.rodata`/`pf.relocs`/`pf.native_bindings` with the re-emitted values so the existing exec-page loop (`em_loader.cpp:775-815`) handles both uniformly. Clean integration.

### WELL DESIGNED — security model is correctly layered (validate-before-exec-page)

The v5 security model (`em_file.hpp:89-101`) is implemented correctly in `em_loader.cpp`:
1. **v4 signature verification** runs BEFORE any `alloc_executable_rw` (`em_loader.cpp:543-596` — the `ed25519::verify` call at ~575, with `alloc_executable_rw` at 786). A tampered v4 module is rejected with "Ed25519 verification FAILED" and no exec page.
2. **v5 IR deserialization + native rebind + validation** runs BEFORE `alloc_executable_rw` (`em_loader.cpp:682` deserialize → `700-725` native rebind (rejects unknown native names) → `733` `validate_thin_function` → `740` `emit_x64` → **then** `786` `alloc_executable_rw`). A malformed v5 IR blob is rejected at validation with no exec page.
3. **W^X discipline**: `alloc_executable_rw` (RW, never RWX) + `seal_executable` (RX) (`em_loader.cpp:786`, `811`). Pages are never writable + executable simultaneously.
4. **Staged commit**: `StagedModule` (`em_loader.cpp:113-131`) owns pages until every function + reloc validates; `commit()` swaps into `out` only after success. On failure, the destructor frees staged pages. No partial module is published.

The key-management model (`EmVerifyPolicy`, `em_loader.hpp:67-75`) is secure-boot-style: empty keyring → dev mode (unsigned v1/v2/v3 accepted, v4 rejected as "worse than honest unsigned dev code"); non-empty keyring → signed-only (unsigned rejected, v4 accepted only if signature verifies). The v4-vs-dev-mode asymmetry (dev mode rejects v4, `em_loader.cpp:552-556`) is a thoughtful choice — a signed module run unverified is worse than honest unsigned code.

### WELL DESIGNED — v4 signature block has a structural cross-check

`em_loader.cpp:489-510`: the v4 signature block's `sig_payload_len` is cross-checked against `payload_end` (the parser's actual position at the end of the name directory). A truncated or lying block is rejected structurally before the cryptographic verify. This is a defense-in-depth check — a file that lies about its signed range is rejected without needing crypto.

### MINOR — v5 is explicitly unsigned for Stage B (raw-x86 fallback has no content authentication)

`em_file.hpp:93-97` and `em_loader.cpp:541-560`: v5 is UNSIGNED for Stage B. The v5 path falls into the unsigned branch (`version != EM_VERSION` at `em_loader.cpp:561`), so a v5 module's raw-x86 fallback (`is_ir=0`) functions are accepted without signature verification in dev mode. The IR functions get the validation gate (structural + native-rebind), but the raw-x86 fallback functions get no content authentication beyond the v3 structural checks. This is documented as FUTURE work ("a v5-signed variant ... is FUTURE work" — `em_file.hpp:97`). This is a **known, documented limitation**, not a defect — the design explicitly defers v5-signed to a future stage. Flagging as MINOR only because a host that opts into signed-only mode and wants v5 must wait for the future variant; today signed-only mode rejects v5 entirely (since v5 is not v4).

---

## 5. Codegen Dual-Path

### WELL DESIGNED — tree-walker / IR-backend split is clean with a correct fallback

`codegen.cpp:3514-3536`: the IR-backend path is gated by `enable_ir_backend` (default false). When on, it checks for obf (annotations `obf`/`obf_keyed` or `ctx.obf.mba/opaque/keyed`) and falls back to the tree-walker if obf is present. Otherwise it calls `lower_function` → (optional `pass_manager->run`) → `emit_x64`, returning directly. If `lower_function` returns an empty blocks list (the `non_serializable` fallback from `thin_lower.cpp:941`/`969`), control falls through to the tree-walker. The fallback is correct: obf transforms (MBA/opaque/CPUID-gate) are emitter-level with no ThinOp representation, so they cannot be lowered; for-each and match are tree-walker-only for now. The `non_serializable_reason` is propagated through `emit_x64` (`thin_emit.cpp:743-744`) so the `.em` serializer can reject non-serializable functions at `em_writer.cpp:143`.

### WELL DESIGNED — flags are composable; peephole runs on BOTH paths

The peephole composes with the IR path. The IR path returns at `codegen.cpp:3530` (`return emit_x64(thf, ctx);`), which is **before** the tree-walker's peephole block at `codegen.cpp:3845`. One might conclude the peephole is skipped on the IR path — but `emit_x64` runs the peephole **internally** at `thin_emit.cpp:733-740` (the "peephole hook (identical to compile_func's peephole block)" comment). So:
- `enable_ir_backend=false, enable_peephole=true` → tree-walker + peephole (`codegen.cpp:3845`).
- `enable_ir_backend=true, enable_peephole=true` → IR path + peephole inside `emit_x64` (`thin_emit.cpp:734`).
- `enable_ir_backend=true, enable_peephole=false` → IR path, no peephole.
- `enable_local_regalloc=true` → only affects the tree-walker (`codegen.cpp:1907-1908`); the IR path ignores it (the IR path has its own materialization model). This is correct — `enable_local_regalloc` is a tree-walker-specific spill elimination, not an IR concept.

The `codegen.hpp:146-147` comment ("Composes with enable_peephole (the IR path runs the same post-emit peephole)") is accurate. The `pass_manager` composes only with `enable_ir_backend` (`codegen.cpp:3525-3529`), which is correct — passes operate on `ThinFunction`, which only exists on the IR path. Composability is sound.

### WELL DESIGNED — default-off flags preserve byte-identity (the gate)

All optimization flags (`enable_peephole`, `enable_local_regalloc`, `enable_ir_backend`) default false (`codegen.hpp:127,136,148`). With all false, `compile_func` runs the original tree-walker byte-for-byte. This is the "no flag-day rewrite" discipline: the 32/32 ctest gate and the lang suite (268/0/0) hold unchanged when flags are off. The IR path is value-equivalent (not byte-identical), pinned by `thin_ir_test`. Good gating discipline.

### MINOR — `resolve_fixups()` is the only `throw` in the compile path and is uncaught by the CLI

`engine.cpp:14` throws `std::runtime_error("ember: unbound label ...")` from `X64Emitter::resolve_fixups()`. The tree-walker calls this at `codegen.cpp:3793`; `emit_x64` calls it at `thin_emit.cpp:712`. This is the **only** `throw` in the core/frontend codegen TUs (verified by grep). In practice it is unreachable: sema validates all label bindings before codegen, so a sema-checked program never has an unbound label. However, `examples/ember_cli.cpp:565` calls `compile_func` **without** a try/catch (the only try/catch in the CLI is around import resolution at `ember_cli.cpp:335-339`). If `resolve_fixups` ever threw (a sema gap, or a hand-built AST), the CLI would terminate with an uncaught exception rather than printing a categorized error. The `em_loader` public API correctly wraps everything in a try/catch (`em_loader.cpp:825-836`), so the load path is safe; only the JIT compile path in the CLI is unguarded. Low risk (unreachable for sema-checked input), but inconsistent with the no-throw boundary discipline elsewhere.

**File:line:** `src/engine.cpp:14` (throw), `examples/ember_cli.cpp:565` (unguarded call)

---

## 6. Extension Pattern (BindingBuilder / register_natives)

### WELL DESIGNED — BindingBuilder pattern is consistent across all native extensions

All nine native extensions (`vec`, `quat`, `mat`, `string`, `array`, `math`, `map`, `sync`, `lifecycle`) follow the identical shape:
1. `.cpp` includes `binding_builder.hpp` (verified: all 9 do, `grep -rn "binding_builder.hpp" extensions/`).
2. `register_natives(std::unordered_map<std::string, NativeSig>& m)` constructs a `BindingBuilder b`, calls `b.add(name, ret, params, fn)` per native, then:
   ```cpp
   NativeTable t = b.build();
   for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
   ```
   This exact 2-line merge appears in all 9 extensions (verified by grep). Extensions with operator overloads (`vec`, `quat`, `mat`, `string`) add a second `b.build()` + merge into `overloads.entries`. The `ext_sync` and `ext_lifecycle` headers also provide `*_host` accessors (host-side reach-in, not ember natives) with the `_host` suffix discipline (`ext_sync.hpp:78-115`, `ext_lifecycle.hpp:42-52`) — a load-bearing naming convention that distinguishes host entries from ember-callable natives.

### WELL DESIGNED — pass extensions mirror the native extension pattern

`ext_opt` and `ext_obf` provide `register_passes(EmberPassRegistry&)` (`ext_opt.hpp:54`, `ext_obf.hpp:31`) — the direct analog of `register_natives`. The `reg.add<PassT>("name")` call (`ember_pass_registry.hpp:24-29`) is the pass-side mirror of `b.add(name, ...)`. The opt/obf extensions include `ember_pass.hpp` + `ember_pass_registry.hpp` + `thin_ir.hpp` instead of `sema.hpp` + `binding_builder.hpp`, which is the correct dependency set for IR passes (they operate on `ThinFunction`, not the native table). Consistent.

### MINOR — `ext_map.hpp` leaks `binding_builder.hpp` into the public header; all others keep it in the .cpp

`extensions/map/ext_map.hpp:11` includes `../src/binding_builder.hpp` in the **header**, while all other extensions include `binding_builder.hpp` only in the **.cpp** (verified: `ext_array.hpp`, `ext_string.hpp`, `ext_lifecycle.hpp`, `ext_vec.hpp`, `ext_math.hpp`, etc. include only `sema.hpp` in their headers). The `ext_map.hpp` public API (`register_natives`, `reset`) uses no `BindingBuilder` type — the include is needed only by `ext_map.cpp:69` (`BindingBuilder b;`). Since `ext_map.cpp` includes `ext_map.hpp` first, it gets `binding_builder.hpp` transitively, so the .cpp does not separately include it. This is a header-hygiene deviation: `binding_builder.hpp` (a host-side implementation detail) leaks into the public include surface of `ext_map`, and any TU that includes `ext_map.hpp` now transitively depends on `binding_builder.hpp` + `ast.hpp` + `sema.hpp`. Moving the include to `ext_map.cpp` (matching the other 8 extensions) would tighten the boundary. No behavioral impact.

**File:line:** `extensions/map/ext_map.hpp:11`

### MINOR — the 2-line `b.build()` + merge boilerplate is repeated in 10 places

The pattern:
```cpp
NativeTable t = b.build();
for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
```
appears verbatim in all 9 native extensions + the overload merge variant in 4 extensions. A helper `BindingBuilder::merge_into(m)` (or `build_into(m, overloads)`) would eliminate the repetition. This is a style/hygiene observation, not a defect — the boilerplate is uniform, so a reader recognizes it instantly. Low priority.

---

## 7. Error Handling

### WELL DESIGNED — `em_loader` has a rigorous no-throw boundary with categorized errors

`em_loader.cpp` is the gold standard:
- `set_error(std::string* err, ...)` (two overloads, `em_loader.cpp:42-51`) is `noexcept` and guards against allocation failure in the catch handler (`try { *err = message; } catch (...) {}`).
- Every failure path calls `set_error` with a categorized message prefix (`em_loader: format:`, `em_loader: limit:`, `em_loader: binding:`, `em_loader: signature:`, `em_loader: v5 IR:`, `em_loader: allocation:`, `em_loader: io:`, `em_loader: exception:`).
- The public `load_em_file` (`em_loader.cpp:819-836`) wraps `load_em_file_impl` in a try/catch for `bad_alloc`, `length_error`, `std::exception`, and `...` — all surfaced as `false` + categorized error.
- Every disk-controlled count/size is bounded before allocation (`MAX_FILE_SIZE`, `MAX_FUNCTIONS`, `MAX_GLOBALS`, `MAX_CODE_PER_FN`, `MAX_RODATA_PER_FN`, `MAX_RELOCS_PER_FN`, `MAX_SLOTS`, `MAX_NAMES`, `MAX_NAME_SIZE` — `em_file.hpp:179-188`), with `checked_add`/`checked_mul` (`em_loader.cpp:33-40`) preventing overflow.
- Relocation offsets are range-checked (`em_loader.cpp:300-303`), reloc kinds validated against a version-appropriate max (`em_loader.cpp:304-307`), native binding offsets checked (`em_loader.cpp:375`), and native signatures canonical-compared (`em_loader.cpp:377-379`).

No silent-failure path exists in the loader. Every `return false` is accompanied by a `set_error`.

### WELL DESIGNED — `em_writer` preflight validates before writing

`em_writer.cpp:130-168` (`preflight_em_module`) validates every count fits u32, every name fits u16, every native binding has a symbolic name, every offset is in range, and every signature is canonical-type-valid before any byte is written. A non-portable function (`non_serializable_reason` non-empty) is rejected with `em_writer.cpp:143`. The writer returns false + `*err` on any failure. No silent corruption.

### WELL DESIGNED — `thin_ir_ser` is safety-by-construction

`thin_ir_ser.hpp:59-62` documents "safety-by-construction": every count checked against a hard max (`IR_MAX_BLOCKS`, `IR_MAX_INSTRS`, `IR_MAX_ARGS`, `IR_MAX_PARAMS`, `IR_MAX_NFIXUPS`, `IR_MAX_VREGS` — `thin_ir_ser.hpp:35-41`) before resize; all cursor arithmetic uses `uint64_t` for `avail = end - cur` with `n > avail` (no uint32_t wrap); `ir_magic` rejects garbage before allocation; `max_vreg` in the header enables O(1) VReg bounds without a pre-scan. No exceptions escape the deserializer (`thin_ir_ser.hpp:54`). Every failure is `return false` + `*err` with a categorized `thin_ir_ser:` message. `validate_thin_function` (`thin_ir_ser.cpp:546-694`) adds the semantic layer (block id bounds, entry block id == 0, terminator presence, VReg bounds, rodata bounds, slot range, Cmp predicate range, CallNative non-empty name, frame plan sanity).

### WELL DESIGNED — sema error reporting is consistent

`sema.hpp:96-103`: `SemaResult { bool ok; vector<SemaError> errors; vector<SemaError> warnings; }`. `sema.cpp:269-274`: `Checker::err(msg, line, col)` pushes to `errs`; `warn(msg, line, col)` pushes to `warns`. Every sema failure path calls `err(...)` with a line/col + message. `SemaResult::ok = errors.empty()` (`sema.cpp:2064`). Warnings are non-fatal (the CLI prints them but still runs). Consistent.

### MINOR — `X64Emitter::resolve_fixups` throw is the one non-no-throw path in codegen

As noted in §5: `engine.cpp:14` is the sole `throw` in the codegen TUs. Every other failure path in `codegen.cpp`/`thin_lower.cpp`/`thin_emit.cpp` uses `non_serializable_reason` (a string propagated to the `.em` serializer) or structural fallback rather than exceptions. The `resolve_fixups` throw is inconsistent with this discipline, though unreachable for sema-checked input. Wrapping it in a `bool resolve_fixups(std::string* err)` (or having `compile_func` catch it and set `non_serializable_reason`) would make the codegen path uniformly no-throw.

**File:line:** `src/engine.cpp:14`

---

## Summary Table

| Area | Finding | Severity |
|------|---------|----------|
| Library structure | Three-lib split, one-way link direction, verified no core→frontend includes | WELL DESIGNED |
| Library structure | `em_loader.cpp` correctly in `ember_frontend` (v5 re-emit needs frontend symbols) | WELL DESIGNED |
| Library structure | opt/obf pass extensions use the same `ember_add_extension` as native extensions | WELL DESIGNED |
| Library structure | `ast.hpp` includes `lexer.hpp` but uses nothing from it; core headers transitively pull a frontend header (header-only, no link cycle) | MINOR |
| Pass system | Concept-based polymorphism (PassConcept/PassModel), CRTP mix-in, no vtable-per-pass | WELL DESIGNED |
| Pass system | `add_pass`/`add_pass_concept` split is motivated and clean | WELL DESIGNED |
| Pass system | Registry pattern mirrors native `register_natives`; fresh instance per `create()` | WELL DESIGNED |
| Pass system | `run_to_fixpoint` convergence correct (`Preserved::all()` iff no pass changed); `is_required` bypass correct | WELL DESIGNED |
| Pass system | `ConstPropPass::run` redundant return branch (both `changed` paths return `none()`) | MINOR |
| Pass system | `EmberAnalysisManager` stub is forward-compatible (stable interface from day one) | WELL DESIGNED |
| IR design | ThinOp enum stable, append-only, range-validated; complete for lowered subset | WELL DESIGNED |
| IR design | Representation conventions (slice=2 VRegs, struct=frame slot, sentinel struct-by-value arg) pinned and sound | WELL DESIGNED |
| IR design | Serialization boundary clean (native_fn dropped→rebind by name, Type*→codec, abs_fixups not serialized) | WELL DESIGNED |
| IR design | `declared_max_vreg` independent validation ceiling (not tautological recompute) | WELL DESIGNED |
| .em format | v5 additive: header/globals/name-dir unchanged, only per-fn record redesigned | WELL DESIGNED |
| .em format | Mixed-mode (IR + raw-x86 per-fn) sound; uniform exec-page loop after re-emit | WELL DESIGNED |
| .em format | Security model layered: v4 sig verify + v5 IR validate BEFORE `alloc_executable_rw`; W^X; staged commit | WELL DESIGNED |
| .em format | v4 `sig_payload_len` cross-checked against parser position (defense-in-depth) | WELL DESIGNED |
| .em format | v5 unsigned for Stage B; raw-x86 fallback has no content authentication (documented future work) | MINOR |
| Codegen dual-path | Tree-walker/IR split clean; obf + for-each + match fallback to tree-walker with explicit reason | WELL DESIGNED |
| Codegen dual-path | Flags composable; peephole runs on BOTH paths (inside `emit_x64` for IR path) | WELL DESIGNED |
| Codegen dual-path | Default-off flags preserve byte-identity (the gate); IR path value-equivalent, pinned by test | WELL DESIGNED |
| Codegen dual-path | `resolve_fixups` throw is uncaught by CLI's `compile_func` call (unreachable for sema-checked input) | MINOR |
| Extension pattern | BindingBuilder + `b.build()` + merge consistent across all 9 native extensions | WELL DESIGNED |
| Extension pattern | Pass extensions (`register_passes`) mirror native extension pattern | WELL DESIGNED |
| Extension pattern | `ext_map.hpp` leaks `binding_builder.hpp` into public header (all others keep it in .cpp) | MINOR |
| Extension pattern | 2-line `b.build()` + merge boilerplate repeated in 10 places (could be a helper) | MINOR |
| Error handling | `em_loader` no-throw boundary: `set_error` noexcept, categorized prefixes, try/catch in public API | WELL DESIGNED |
| Error handling | `em_writer` preflight validates counts/ranges/canonical types before writing | WELL DESIGNED |
| Error handling | `thin_ir_ser` safety-by-construction: bounded counts, uint64 cursor arithmetic, no exceptions escape | WELL DESIGNED |
| Error handling | sema `err`/`warn` consistent; `SemaResult.ok = errors.empty()`; warnings non-fatal | WELL DESIGNED |
| Error handling | `resolve_fixups` throw is the one non-no-throw path in codegen (inconsistent with discipline) | MINOR |

---

## Overall Assessment

The architecture is **well designed**. The dominant signal is consistency: the same patterns (BindingBuilder, register_natives/register_passes, additive fields with default-constructed empty, default-off flags preserving byte-identity, no-throw boundaries with categorized errors) appear uniformly across the codebase. The link direction is clean and verified. The v5 security model is correctly layered (validate-before-exec-page, W^X, staged commit). The dual-path codegen composes correctly (peephole on both paths, pass manager on IR path only, fallback for non-serializable functions).

The findings are 6 MINOR + 0 DESIGN ISSUE:
1. `ast.hpp` gratuitous `lexer.hpp` include (header-only coupling, no link cycle).
2. `ConstPropPass` redundant return branch (dead sub-condition, no behavioral impact).
3. v5 unsigned for Stage B (documented future work, not a defect).
4. `resolve_fixups` throw uncaught by CLI (unreachable for sema-checked input).
5. `ext_map.hpp` leaks `binding_builder.hpp` into public header (hygiene).
6. `b.build()` + merge boilerplate repeated in 10 places (style).

No DESIGN ISSUE found. The MINOR items are hygiene/style with no behavioral impact. The codebase builds clean (32/32 ctest pass) and the design choices are well-documented inline with cross-references to the design docs.

**Note on HEAD:** The task stated HEAD `bf76217` but the actual HEAD is `7c73361` (one commit ahead — "fix 15 compiler warnings"). The audit was run against the actual HEAD. The two commits are maintenance-only (warning fixes + doc line-cite refresh), so the architecture is identical between them.
