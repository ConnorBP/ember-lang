# Ember Stage B audit note — 2026-07-10 (IL-.em Stage B shipped: .em IR serialization)

**Audited revision:** `2cd93d5` (`Stage B c3: v5 IR .em integration test + malformed rejection`) — HEAD after chunks c1a + c1b + c2 + c3 landed.
**Primary platform:** Windows x86-64, MinGW g++ 15.2.0, Ninja, C++17, Release.
**Scope:** a focused audit note on the `.em` IR serialization that landed in chunks c1a–c3 (the IL-.em Stage B), recording the v5 format, the serializer/deserializer contract, the security property, the test gate, and the Stage C foundation framing. This is a shipment audit, not a fresh adversarial re-audit; it mirrors the Stage A note's shape so the Stage B landing has a tracked home in `docs/audit/`.

## What shipped

The `.em` IR serialization — Stage B of `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §4.3/§8 (the security property: a `.em` module carries IR, not raw x86, closing the raw-x86 code-injection surface):

- **c1a — `src/em_type_codec.{hpp,cpp}`**: the shared canonical-type codec (emit_type/emit_signature/validate_canonical_type/validate_signature + parse_type/canonical_type_same/parse_signature), extracted from the duplicated writer/loader implementations. The single source of truth for the Type/EmSignature on-disk encoding; the IR serializer reuses it for `const Type*` → stable type ID. Also lands the v5 on-disk contract (`src/em_file.hpp`: `EM_VERSION_V5`, the v5 per-function record spec, `EmFunctionRecord::ir_blob`) and `ThinFunction::owned_types` (reconstruction storage for deserialized Type objects).

- **c1b — `src/thin_ir_ser.{hpp,cpp}`**: the IR serializer/deserializer. `serialize_thin_function` (ThinFunction → opaque byte blob) and `deserialize_thin_function` (blob → ThinFunction). `ThinOp` (uint16) serializes verbatim (stable enum). `const Type*` fields encode via `em_type_codec`. `native_fn` ptr dropped (rebound by `meta.native_name` at load). `abs_fixups` not serialized (populated by `emit_x64` at load). Safety-by-construction: `ir_magic` (0x4952464E), bounded counts (checked before any resize/reserve), `uint64_t` cursor arithmetic (no uint32 wrap), `max_vreg` in the header (O(1) VReg checking), enum-range validation. `validate_thin_function`: semantic validation (VReg bounds, block-target bounds, entry block, terminators, ConstStringRef rodata bounds).

- **c2 — v5 writer + loader paths** (`src/em_writer.cpp` + `src/em_loader.cpp`):
  - Writer: `emit_em_content` for `EM_VERSION_V5` writes the v5 per-function record (`is_ir` byte + hoisted signature, then `ir_blob` or v4 body). `write_em_file_v5` wrapper. `preflight_em_module` takes a version param (IR functions skip the `non_serializable_reason` check).
  - Loader: accepts `EM_VERSION_V5`; `parse_file` reads `is_ir` + ir_blob (opaque — not interpreted at parse time); `load_em_file_impl` deserializes + validates + native-rebinds + re-emits via `emit_x64` BEFORE `alloc_executable_rw`. The native rebind is the core v5 security gate: an IR module referencing a native the host didn't register is rejected with no exec page.
  - Architecture: `em_loader.cpp` moved from the core `ember` lib to `ember_frontend` (the v5 IR re-emit needs `emit_x64` from `thin_emit.cpp` + `CodeGenCtx` from `codegen.hpp`, both frontend). The core lib stays one-way (no `ember_frontend` dependency). `em_loader.hpp` stays in `src/`; `em_writer.cpp` stays in core (no frontend deps).

- **c3 — integration test + malformed rejection** (`examples/em_v5_ir_test.cpp`, ctest `em_v5_ir`): end-to-end IR roundtrip (lower → serialize → write v5 `.em` → load → re-emit → execute → fib(10)==55) + malformed rejection (bad magic, truncated, empty, unknown native name → `load_em_file` returns false with NO exec page).

## The v5 security property (the gate pins it)

A v5 `.em` carries IR, NOT machine code. The loader:
1. `parse_file` reads the ir_blob as opaque bytes (no interpretation).
2. `load_em_file_impl` deserializes the IR (`deserialize_thin_function` — structural validation: magic, version, count bounds, cursor bounds, ThinOp/TermKind/base_kind ranges, canonical-type shape).
3. Native rebind: every `CallNative` instr's `meta.native_name` is looked up in the host natives table. An unknown native REJECTS the module.
4. Semantic validation (`validate_thin_function` — VReg bounds, block-target bounds, entry block, terminators, rodata bounds).
5. Re-emit via `emit_x64` → x86 code + relocs + native_bindings.
6. ONLY THEN `alloc_executable_rw` + reloc patch + native bind + seal + publish.

A tampered or malformed v5 `.em` is rejected at step 2, 3, or 4 — **NO executable page is allocated**. The `em_v5_ir` test pins this: all four malformed cases (bad magic, truncated, empty, unknown native) return `false` with `LoadedModule.pages.empty()`.

## The gate (verified at HEAD `2cd93d5`)

- **ctest 30/30** in the default config (incl. `thin_ir_ser` + `em_v5_ir`, the two new Stage B targets; the 28 Stage A + prior tests still pass).
- **`thin_ir_ser` 3-part gate**: (1) hand-built ThinFunction round-trip (serialize/deserialize/validate/structural-eq), (2) real lowered fib round-trip (lower→serialize→deserialize→validate→re-emit→execute, fib(10)==55), (3) malformed-blob rejection (bad magic, truncated, invalid ThinOp, OOB target, no terminator, empty).
- **`em_v5_ir` 2-part gate**: (1) end-to-end v5 IR .em roundtrip (value-equivalent), (2) malformed v5 .em rejection (no exec page).

## Mixed mode

A v5 module may MIX IR and raw-x86 functions per-function: IR-serializable fns ship IR (`is_ir=1`); non-serializable fns (the Stage A known gaps — slices, structs, strings, defer, fixed-arrays — flagged by `ThinFunction::non_serializable`) ship raw x86 (`is_ir=0`, byte-identical v4 body after the `is_ir` byte + hoisted signature). The `is_ir` byte is derived from `!ir_blob.empty()` at write time. The loader branches on `is_ir` per-function.

## Stage C foundation framing

Stage B is the IR serialization layer — it makes `.em` carry IR, and the loader re-emits from validated IR. It is the foundation for:

- **Stage C — IR optimization passes.** CSE/DCE/const-prop/peephole/LICM become `ThinPass`es over the `ThinFunction` (matching on `ThinOp`, not bytes — far less brittle than Stage 1's byte-peephole). The Stage-1 peephole table carries over as IR-pattern rewrites. The composable pass architecture is researched in `../LLVM_PASS_SYSTEM_RESEARCH.md` (LLVM 18.1.8 pass-manager patterns distilled for ember: concept-based polymorphism, CRTP mix-in, PreservedAnalyses, AnalysisManager, PassInstrumentation, pipeline parsing). The Stage-3 upgrade to full SSA-lite (rename + slot-back + liveness + linear-scan, `../spec/COMPILER_PIPELINE.md` §5/§8) is additive on this instruction set, not a rewrite.

## What v5 does NOT yet cover (documented deferrals)

- **v5-signed variant**: v5 is unsigned for Stage B (the v3 "trailing bytes == 0" rule holds). A v5 + Ed25519 signature block variant is FUTURE work (the v4 signing machinery + the v5 IR format compose; the integration is deferred).
- **Struct/array/slice/string/defer IR functions**: these are the Stage A known gaps (the IR lowering doesn't handle them). They ship raw-x86 fallback (`is_ir=0`) in a v5 module. Closing these gaps is Stage B/C prerequisite work (the IR lowering + emit paths, not the serialization layer).
- **Safety-guard IR at load time**: the IR may carry `DepthCheck`/`BudgetCheck`/`CallTargetGuard` instrs (lowered when the host enabled those flags). At load time, the re-emit ctx defaults safety flags off, so guard instrs become no-ops (the function executes correctly, just without enforcement). A load-time safety-runtime binding (providing trap_stub/budget_ptr/depth_ptr/allowlist to the re-emit ctx) is a future host-contract item.
- **Cross-process v5 portability for raw-x86 fallback functions**: the `is_ir=0` fallback has the same process-local-address constraints as v3/v4 raw-x86 (the `non_serializable_reason` check still applies). The IR path (`is_ir=1`) is portable by construction (no process-local ptrs).

## Cross-references

- `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §8 (Stage B status entry) and §4.3/§4.6 (the hybrid thin-IR option).
- `../LLVM_PASS_SYSTEM_RESEARCH.md` (the composable pass architecture research for Stage C).
- `../audit/AUDIT_2026-07-10_STAGE_A.md` (the Stage A foundation this builds on).
- `src/thin_ir_ser.hpp` (the ir_blob format + serialization boundary contract).
- `src/em_file.hpp` (the v5 format spec + security model).
- `examples/thin_ir_ser_test.cpp` + `examples/em_v5_ir_test.cpp` (the gates).
- `../spec/SPEC_AUDIT_2026-07-10.md` F2 (the signed-.em security layer the v5-signed variant will compose with).
