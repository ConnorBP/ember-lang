# Ember Stage A audit note — 2026-07-10 (IL-.em Stage A shipped)

**Audited revision:** `36814d7` (`Stage A c5: IR-backend correctness gate (flags-off byte-identical, flags-on value-equivalent)`) — HEAD after chunks c1–c5 landed; this c6 is the doc-only status pass.
**Primary platform:** Windows x86-64, MinGW g++ 15.2.0, Ninja, C++17, Release.
**Scope:** a focused audit note on the thin three-address IR backend that landed in chunks c1–c5 (the IL-.em Stage A), recording the dual-path contract, the gate that pins it, the honest coverage, and the Stage B/C foundation framing. This is a shipment audit, not a fresh adversarial re-audit (those are `AUDIT_2026-07-09.md` / `AUDIT_2026-07-10.md`); it mirrors their shape so the Stage A landing has a tracked home in `docs/audit/`.

## What shipped

The thin three-address IR compile-time backend — the landed Stage-2 stepping stone of `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §4.3/§4.6 (option (c)):

- `src/thin_ir.{hpp,cpp}` — the IR data structures (`ThinFunction`/`ThinBlock`/`ThinInstr` + the stable `ThinOp` uint16_t enum, the serialization-ready contract Stage B consumes) + the debug pretty-printer.
- `src/thin_lower.{hpp,cpp}` — the AST→`ThinFunction` lowering (`lower_function`); a mechanical, value-equivalent mirror of what `CG::eval`/`exec_block`/`compile_func` (`src/codegen.cpp`) would emit.
- `src/thin_emit.{hpp,cpp}` — the `ThinFunction`→x64 emit (`emit_x64`); reproduces the tree-walker's byte sequences keyed off `ThinOp`.
- `CodeGenCtx::enable_ir_backend` (`src/codegen.hpp`) — the flag, default off. `compile_func` dispatches: off → the existing `CG` tree-walk; on → `lower_function` + `emit_x64`.
- `examples/thin_ir_test.cpp` (ctest `thin_ir`) — the correctness gate, modeled on Stage 1's `codegen_opt_test`; `thin_ir_struct` (ctest) pins the IR struct invariants.

No `src/` changes in this c6 chunk — doc-only.

## Dual-path contract (the gate pins both)

The codegen is now **dual-path**: the tree-walker (default) and the thin-IR backend (opt-in). The contract, pinned by `thin_ir_test`'s three assertion classes:

1. **default off = byte-identical tree-walker.** `enable_ir_backend` defaults false everywhere (the CLI `ember_cli.cpp` and the engine helpers never set it), so the default codegen path is the unchanged pre-Stage-A tree-walker. `thin_ir_test` Part 1 compiles the same source twice with the flag off and asserts byte-identical `main` bytes (no hidden nondeterminism), AND asserts those bytes equal a fresh tree-walker baseline (the default path is unchanged — the flag is inert when off).
2. **on = value-equivalent (NOT byte-identical).** `thin_ir_test` Part 2 compiles a corpus both ways, finalizes both, calls both, and asserts identical i64 returns. The IR path may emit push/pop where the tree-walker used r10, etc. — only the JIT'd *execution* is value-equivalent.
3. **flag actually runs.** `thin_ir_test` Part 3 asserts the IR path produces bytes that DIFFER from the tree-walker for a non-trivial source (proving the dispatch went through `lower_function` + `emit_x64`, not a silent no-op fallback), and calls `lower_function` directly to assert a non-empty `ThinFunction` (the lower+emit path is wired).

**Composition:** the IR path runs the same post-emit peephole as the tree-walker (Stage 1's `src/peephole.{hpp,cpp}` operate on the emitted `vector<uint8_t>`, IR-path or tree-walker alike), so `enable_ir_backend` composes with `enable_peephole`. **Obf fallback:** the lowering marks `@obf`/`@obf_keyed`/mba/opaque functions `non_serializable` (the obfuscation transforms are emitter-level with no `ThinOp` representation); the dispatch skips the thin path for them and falls back to the tree-walker (see `src/thin_lower.hpp`'s fallback note). All other functions — including those using safety guards, indirect calls, cross-module calls, trap stubs, and budget/depth pointers — ARE lowered (those have `ThinOp` representations).

## The gate (verified at HEAD `36814d7`)

- **ctest 27/27** in the default config (incl. `thin_ir` + `thin_ir_struct`, the two new Stage A targets). When the optional AngelScript SDK is present, `bench_ember_vs_as` is configured too for 28/28; that bench is not a Stage A target.
- **lang suite 268/0/0** (268 passed, 0 failed, 0 skipped). The CLI never sets `enable_ir_backend`, so the lang suite exercises the default-off tree-walker path and is unaffected by Stage A — the flag's inertness when off is exactly what keeps the lang gate unchanged.
- **`thin_ir_test` PASS/SKIP breakdown** (read from the test, the honest coverage): the PASSING cases are PINNED (regression protection); the SKIP cases are documented as known gaps for Stage B/C, NOT silently passing and NOT failing the whole gate.

## Honest coverage

**PASSING the value-equivalence gate** (pinned in `thin_ir_test` Part 2): scalar integer arithmetic + overflow wrap (u8/i8 boundaries), comparisons (all six predicates), short-circuit `&&`/`||`, control flow (if/while/for/do-while/switch, break/continue), recursion (fib), script-to-script calls, native calls (i64(i64,i64) + math `sqrt`), cast (int width + int↔float), ternary; corpus `runtime_audit_semantics` + `runtime_division_forms` (the division forms — signed/unsigned Div/Mod + the `INT64_MIN/-1` overflow guard).

**KNOWN GAPS (documented as SKIP in `thin_ir_test`, Stage B/C work):**

| gap | node class | root cause (per the test's known-gaps block) |
|---|---|---|
| slices | index + bounds | the slice element load (LoadFrame from a computed IndexAddr) is not frame-backed, so summing `s[0]+s[1]+s[2]` reuses a stale `rax` for the earlier element — a frame-slot field split is needed (Stage B/C) |
| structs | by-value arg/return/field/reassign | the hidden word-0 ptr / `CopyBytes` / `FieldAddr` ABI has emit-path bugs beyond the scalar spill |
| strings | native + inline-XOR decrypt | the `string_from_slice` implicit conversion + the encrypted-literal inline-XOR decrypt have emit bugs |
| defer/global | defer-cleanup emission | the defer-cleanup block emission segfaults the IR path (a separate emit bug) |
| fixed-array | indexed store (`a[expr] = v`) | the fixed-array indexed-store emit path crashes the IR path (corpus `runtime_cast_regressions`, `runtime_integer_boundaries`) |

These node classes are covered by the lang suite on the default-off (tree-walker) path; the IR path's handling of them is the Stage B/C work. The post-lowering per-vreg spill pass (`src/thin_lower.cpp`) already fixed the SCALAR/float intermediate-result case (a producing instr leaving its dst in `rax` with no frame slot), which is why arithmetic, control flow, recursion, short-circuit, ternary, cast, and script/native calls now pass; the remaining gaps are slice/struct/string/fixed-array/defer-specific emit paths with their own bugs.

## Stage B/C foundation framing

Stage A is the thin-IR *substrate* — correct lowering + emit, **no optimization** (LAZY MODE per `src/thin_lower.hpp`: no regalloc, no peephole, no CSE/DCE/const-prop; those are Stage C IR passes over the `ThinFunction` AFTER the lowering produces it). It is the foundation for:

- **Stage B — `.em` IR serialization (the security property).** The `ThinOp` enum is a stable uint16_t serialization boundary (the SERIALIZATION BOUNDARY note in `src/thin_ir.hpp` — do not renumber; append new ops at the END only). Stage B serializes the IR verbatim so a `.em` module carries IR (not raw x86), closing the raw-x86 code-injection surface that the signed-`.em` work (F2, `../spec/SPEC_AUDIT_2026-07-10.md`) addresses at the signature layer. The only non-serialized IR fields are the JIT-time `native_fn` ptr (recovered by `meta.native_name` at load time) and the `const Type*` fields (replaced with stable type IDs — the canonical encoding `em_writer`/`em_loader` already use).
- **Stage C — IR optimization passes.** CSE/DCE/const-prop/peephole/LICM become `ThinPass`es over the `ThinFunction` (matching on `ThinOp`, not bytes — far less brittle than Stage 1's byte-peephole). The Stage-1 peephole table carries over as IR-pattern rewrites. The Stage-3 upgrade to full SSA-lite (rename + slot-back + liveness + linear-scan, `../spec/COMPILER_PIPELINE.md` §5/§8) is additive on this instruction set, not a rewrite.

## Cross-references

- `../spec/CODEGEN_OPTIMIZATION_DESIGN.md` §8 (Stage A status entry, mirroring the Stage 1 §8 shape) and §4.3/§4.6 (the hybrid thin-IR option this implements).
- `../spec/COMPILER_PIPELINE.md` §5 (the SSA-lite target; the Stage A implementation note recording the thin IR as a deliberate subset).
- `../ROADMAP.md` "Codegen optimization" → Stage 2 entry (Stage A SHIPPED as the landed stepping stone; full Stage 2 + Stage 3 remain the still-future upgrade path).
- `src/thin_ir.hpp` / `src/thin_lower.hpp` / `src/thin_emit.hpp` (the landed signatures + the representation/correctness contracts).
- `src/codegen.hpp` (`enable_ir_backend` flag + its comment).
- `examples/thin_ir_test.cpp` (the gate — header comment + PASS/SKIP breakdown).
- `../spec/SPEC_AUDIT_2026-07-10.md` F2 (the signed-`.em` security layer Stage B builds on).
