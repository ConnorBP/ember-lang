# Thin IR Optimization Passes — Read-Only Correctness Audit

**Date:** 2026-07-12  
**Scope:** `extensions/opt/ext_opt.cpp`, declarations in `extensions/opt/ext_opt.hpp`, the contract in `src/thin_ir.hpp`, emission/lowering semantics needed to interpret `meta.frame_off`, and coverage in `examples/ir_passes_test.cpp`.  
**Passes requested:** `constprop`, `dce`, `cse`, `licm`, `forward`, `copyprop`, `instcombine`, `dse`.  
**Additional registered pass noted:** `simplifycfg`.  
**Method:** source audit plus existing test/CLI execution. No implementation code was edited.

## 1. Repository and validation state

The audit began with status/diff inspection.

- Workspace root already had unrelated modifications and untracked directories.
- The nested `ember` repository was on `master` at `a1a1163` and already had one modified file: `extensions/opt/ext_opt.hpp`.
- The pre-existing header diff changes the advertised count from nine to ten and declares/documents `BoundsCheckElimPass`. It was not overwritten. There is no corresponding implementation or registration in the reviewed `ext_opt.cpp`; the implementation currently registers the requested eight passes plus `simplifycfg` (nine total).

Validation reproduced on the existing `buildt` artifacts:

```text
./buildt/ir_passes_test.exe
=> PASS

./buildt/ember_cli.exe run tests/lang/optimization_validation.ember
=> exit 177

./buildt/ember_cli.exe run tests/lang/optimization_validation.ember \
  --passes constprop,forward,copyprop,instcombine,dce,cse,licm,dse
=> exit 177
```

These are useful regression signals, but they do **not** establish value preservation for arbitrary validator-clean Thin IR. The IR contract explicitly says VRegs are not SSA, while most tests are lowerer-produced and therefore usually use monotonic VRegs. Several critical cases below are absent from the suite.

## 2. Executive findings

### Correctness defects

| ID | Severity | Pass(es) | Finding |
|---|---:|---|---|
| C1 | Critical | CSE | A redundant definition is rewritten past a redefinition of its own `old_dst`; the scan stops only on redefinition of `new_dst`. |
| C2 | Critical | CSE | CSE rewrites only later instructions in the current block. It neither rewrites the current block terminator nor successor-block uses, yet deletes the definition. |
| C3 | Critical | DCE, ConstProp sweep, CSE | A producer with nonzero `meta.frame_off` performs an observable implicit frame write. Treating it as a removable/coalescible pure instruction can leave a later frame load uninitialized or stale. |
| C4 | High | ConstProp | Frame-slot constants survive implicit producer writes and aliasing writes, so later expressions can fold from stale memory facts. |
| C5 | High | CSE | Expression keys omit semantic fields (`is_unsigned`, `is_f32`, type, float immediate, lengths, and others) and memory entries survive calls/aliasing stores. Distinct values can be coalesced. |
| C6 | High | LICM | Loop discovery is based on block order rather than dominance, and hoisting does not require speculation safety or execution on every loop entry. Trapping operations and mutable loads can move before a zero-trip/conditional loop. |
| C7 | High | Forward | Forwarding assumes the stored source VReg is SSA. If that VReg is redefined, replacing a load with `Move` reads the new VReg value, not the value captured by the store. |
| C8 | High | Forward, DSE, ConstProp/DCE slot logic | The memory model is offset-only and incomplete: computed `StoreFrame`, `CopyBytes` ranges/globals, aggregate initializers, `StoreAddr`, calls, implicit homes, widths, and escaped frame addresses are not consistently treated as alias barriers/readers/writers. |
| C9 | Medium/High | ConstProp | Signed `int64_t` add/sub/mul in the folder invokes C++ signed-overflow UB. Narrow unsigned normalization is represented as signed, which can poison chained folds. |
| C10 | Medium | LICM | Multiple hoists are erased in reverse order and appended in that reverse order; non-SSA repeated definitions can change order and meaning. Nested/multiple-latch loops have no hierarchy or dominance checks. |

### Main missed optimizations

- Local CSE misses commutative expressions and expressions with different destination spill homes.
- There is no safe inter-block CSE/GVN, constant propagation, forwarding, copy propagation, or DSE.
- The normal eight-pass ordering runs DCE before CSE/LICM/DSE and has no final iterative cleanup.
- Constant propagation handles only a narrow integer-binop subset, not comparisons, unary operations, casts, branch facts, or join facts.
- LICM cannot hoist dependency chains and skips the header categorically.
- CopyProp misses the read-use encoded in `CopyBytes::dst`.
- Forward/DSE miss safe opportunities around proven non-overlapping ranges, but broadening them first requires a unified alias/effect model.

## 3. Required minimal repros for correctness findings

The examples use `v0` as the invalid/sentinel value and assume ordinary scalar widths unless stated otherwise.

### C1 — CSE rewrites past `old_dst` redefinition

```text
block 0:
  v1 = ConstInt 1
  v2 = ConstInt 2
  v3 = Add v1, v2
  v4 = Add v1, v2       ; redundant with v3
  v4 = ConstInt 9       ; redefines old_dst
  v5 = Add v4, v1
  Return v5
```

**Baseline:** `10`.  
**Current CSE:** rewrites the final `v4` use to `v3` because its scan stops only at `dst == new_dst` (`v3`), not `dst == old_dst` (`v4`). It then removes the redundant definition and produces `3 + 1 = 4`.  
**Required behavior:** stop rewriting at either definition, or represent the redundant computation as a `Move v4, v3` and let non-SSA-aware def/use cleanup handle it.

**Coverage gap:** no same-block non-SSA CSE test with `old_dst` redefinition.

### C2a — CSE deletes a value used by the same block terminator

```text
block 0:
  v1 = ConstInt 1
  v2 = ConstInt 2
  v3 = Add v1, v2
  v4 = Add v1, v2
  Return v4
```

**Baseline:** `3`.  
**Current CSE:** scans only `blk.instrs` after the redundant instruction, removes `v4`, and never rewrites `term.ret`. The return references an undefined VReg.  
**Required after IR:** `Return v3`, or keep/replace the second definition with `Move v4, v3`.

### C2b — CSE deletes a value used in a successor block

```text
block 0:
  v1 = ConstInt 1
  v2 = ConstInt 2
  v3 = Add v1, v2
  v4 = Add v1, v2
  Jmp block 1
block 1:
  v5 = Add v4, v1
  Return v5
```

**Baseline:** `4`.  
**Current CSE:** removes the `v4` definition but does not rewrite block 1, leaving `v4` undefined.  
**Required behavior:** local CSE must not delete a definition with out-of-block uses. Global replacement requires dominance plus non-SSA reaching-definition checks; the simpler safe form is `Move v4, v3`.

**Coverage gap:** tests exercise neither terminator use nor successor use of the redundant destination.

### C3a — DCE and ConstProp dead sweeps erase an implicit spill-home write

`meta.frame_off` on a producer is not merely annotation. `thin_emit.cpp::pin_int_dst` stores the result to that frame offset.

```text
block 0:
  v1 = ConstInt 5, frame_off=-8
  v2 = LoadFrame -8
  Return v2
```

**Baseline:** `5`.  
**Current DCE/ConstProp sweep:** `v1` has no explicit VReg use, so the `ConstInt` is removed. The `LoadFrame` remains because its result is returned, and now reads uninitialized memory.  
**Required behavior:** regard a nonzero producer home as a frame write. It can be removed only after proving the home write dead with memory liveness/alias analysis, or after rewriting every memory consumer.

This applies to all frame-backed producers, not only constants: arithmetic, comparisons, casts, moves, float/slice producers, and scalar call results.

**Coverage gap:** D6 tests a `ConstInt` used directly by a return and an explicit `StoreFrame`; it does not test an otherwise-dead producer whose only observable effect is `meta.frame_off`.

### C3b — CSE removes a required spill-home restoration

```text
block 0:
  v1 = ConstInt 1
  v2 = ConstInt 2
  v3 = Add v1, v2, frame_off=-8       ; writes 3 to -8
  CopyBytes src=-16, dst=-8, len=8     ; overwrites -8, e.g. with 77
  v4 = Add v1, v2, frame_off=-8       ; must restore 3 to -8
  v5 = LoadFrame -8
  Return v5
```

**Baseline:** `3`.  
**Current CSE:** `CopyBytes` is side-effecting but does not clear the CSE table. The second `Add` has the same key and can be removed, including its implicit write. The load observes `77`, not `3`.  
**Required behavior:** either preserve the redundant destination/home with a `Move` or explicit store, and invalidate memory-dependent/home-sensitive availability at aliasing barriers.

**Coverage gap:** CSE side-effect tests only prove that `CallNative`/`StoreFrame` themselves are not coalesced; they do not test cached expressions or implicit home writes across a side effect.

### C4 — ConstProp retains a stale frame-slot fact

```text
block 0:
  v1 = ConstInt 5, frame_off=-8        ; slot_const[-8] = 5
  v2 = Add vParam, 0, frame_off=-8     ; nonconstant implicit overwrite
  v3 = LoadFrame -8                    ; actual value is vParam
  v4 = Add v3, 1
  Return v4
```

**Baseline:** `vParam + 1`.  
**Current ConstProp:** the nonconstant producer erases only `vreg_const[v2]`; it does not erase `slot_const[-8]`. The load is treated as 5 and the final add can fold to 6.  
**Required behavior:** every instruction that writes an overlapping frame range must invalidate/update slot facts. Calls, `CopyBytes`, aggregate initializers, `StoreAddr`, computed stores, and escaped pointers require conservative barriers.

A second minimal form replaces the nonconstant `Add` with a `CopyBytes` whose destination covers `-8`; current slot facts likewise survive.

### C5a — CSE key omits float immediate

```text
block 0:
  v1 = ConstFloat 1.0, frame_off=0
  v2 = ConstFloat 2.0, frame_off=0
  ... use v2 ...
```

`CSEKey` stores `imm.i`, not `imm.f`; both float constants normally have `imm.i == 0`. With matching metadata they compare equal.  
**Expected:** preserve 1.0 and 2.0 as distinct definitions.  
**Current risk:** replace uses of 2.0 with 1.0.

### C5b — CSE key omits signedness/type

```text
  v3 = Shr vNeg, vOne, is_unsigned=0, width=8  ; arithmetic: -2 for -4 >> 1
  v4 = Shr vNeg, vOne, is_unsigned=1, width=8  ; logical: 0x7fff...ffe
```

With otherwise matching fields/homes, current keys are equal because `is_unsigned` is absent. `Cmp` has the same signed/unsigned problem; `Cast` keys omit target type; f32/f64 distinctions and aggregate lengths are also incomplete.

**Required behavior:** key every semantic field and exclude operations whose complete semantics are not modeled.

### C5c — CSE reuses stale global/frame loads across side effects

```text
  v1 = LoadGlobal G
  StoreGlobal G, 9
  v2 = LoadGlobal G
  Return v2
```

or:

```text
  v1 = LoadFrame -8
  StoreAddr frame_pointer_to_minus8, 9
  v2 = LoadFrame -8
```

**Expected:** the second load observes the write.  
**Current CSE:** `StoreGlobal`, `StoreAddr`, calls, aggregate writers, and most `CopyBytes` cases do not clear cached loads. Exact `StoreFrame` kills only exact-offset `LoadFrame` entries.

### C6 — LICM speculates a trap before a zero-trip loop

```text
block 0 (preheader):
  vDen = ConstInt 0
  vCond = ConstBool false
  Jmp block 1
block 1 (header):
  Branch vCond, block 2, block 3
block 2 (body/latch):
  vQ = Div vNumerator, vDen             ; operands defined outside loop
  Jmp block 1
block 3:
  vResult = ConstInt 7
  Return vResult
```

**Baseline:** returns `7`; body never executes.  
**Current LICM:** `Div` is considered pure and invariant by the generic source test, so it can be moved to block 0 and trap on divide by zero.  
**Required behavior:** hoist only speculatable/non-trapping operations, or prove the instruction executes on every path where the loop is entered and preserve trap order.

A mutable-load variant uses `LoadGlobal` in the body plus a loop call/store that can change the global. `LoadGlobal` falls through the generic no-source invariant test and can be hoisted despite mutation.

### C7 — Forwarding through a non-SSA source redefinition

```text
block 0:
  v1 = ConstInt 5
  StoreFrame v1, -8
  v1 = ConstInt 9
  v2 = LoadFrame -8
  Return v2
```

**Baseline:** `5`; the store captured the value before redefinition.  
**Current Forward:** rewrites the load to `Move v2, v1`, which reads the current value `9`. The source comment explicitly assumes SSA, contradicting `thin_ir.hpp`.  
**Required behavior:** forward a VReg only while that definition remains reaching and unredefined, or materialize/rematerialize the stored value. Inter-block forwarding additionally needs a predecessor meet and dominance.

**Coverage gap:** D11 covers one `CopyBytes` destination barrier but no VReg redefinition.

### C8a — Computed `StoreFrame` is mistaken for a frame-slot store

`thin_emit.cpp` supports `StoreFrame` with `src2 != 0` as `[src2 + frame_off] = src1`. The passes do not consistently distinguish it.

```text
  StoreFrame v5, [vPtrA + 0]
  StoreFrame v9, [vPtrB + 0]
```

If both have `meta.frame_off == 0`, DSE treats them as two writes to one frame slot and may delete the first even though the pointers differ. Forward/ConstProp can similarly establish a false fact for frame offset 0. `StoreAddr` is now the preferred opcode, but validator-clean/hand-built/deserialized IR and the emitter still define computed `StoreFrame` semantics.

### C8b — Partial-range reads are missed

```text
  StoreFrame vWide, off=-16, width=8
  CopyBytes src=-12, dst=-32, len=4
  ... no other explicit read of -16 ...
```

The copy reads bytes within the store's span, but `compute_read_slots` and `instr_reads_off` test only whether the store's **starting offset** lies inside the copy range. DCE/ConstProp can classify the write as unread. Correct reasoning needs byte intervals and operation widths.

### C8c — Escaped frame pointer call is not a continuing read barrier for DSE

```text
  vPtr = FieldAddr frame_base=-16
  StoreFrame v1, -16
  CallNative consume(vPtr)       ; observes first store
  StoreFrame v2, -16
```

`FieldAddr` kills a pending store only at the point it appears. A later store starts new tracking, and an ordinary pointer call is not recognized by `call_reads_frame_off`; DSE can remove the first store despite the call observing it. Alias/escape state must persist after address exposure.

### C9 — Constant folding overflow and narrow unsigned chaining

Signed overflow example:

```text
  v1 = ConstInt INT64_MAX
  v2 = ConstInt 1
  v3 = Add v1, v2
  Return v3
```

The target semantics wrap to `INT64_MIN`; `fold_int_binop` computes signed `a + b`, which is C++ UB. Add/sub/mul should use unsigned arithmetic plus a defined bit cast, as other compiler paths already do.

Narrow unsigned chain:

```text
  v1 = ConstInt 255             ; u8
  v2 = ConstInt 0
  v3 = Add v1, v2, width=1, unsigned/type=u8
  v4 = Shr v3, 1, width=1, is_unsigned=1
  Return v4
```

**Expected:** `127`.  
**Current fold state:** width-1 normalization casts through `int8_t`, recording `v3` as `-1`; a chained unsigned shift can fold from the wrong 64-bit value and normalize back to `255`. The constant lattice must preserve width/type-consistent bits and signed interpretation.

### C10 — LICM reverses multiple hoists/non-SSA definitions

```text
block 2 (loop body):
  v1 = ConstInt 1
  ... use v1 ...
  v1 = ConstInt 2
  ... use v1 ...
```

Both constants are unconditionally considered invariant. LICM collects indices, sorts descending for safe erasure, then appends in that descending order, placing the `2` definition before the `1` definition in the preheader. In non-SSA IR, definition order is semantic. Even independent hoists should preserve original execution order; repeated definitions require reaching-def/use analysis and generally should not be hoisted as one invariant value.

## 4. Pass-by-pass audit

### 4.1 `constprop`

**Sound parts**

- Facts are reset per block, avoiding unsound implicit loop propagation.
- Direct VReg redefinitions erase/update local facts.
- Terminator uses are counted by the later dead sweep.
- Division/modulo are deliberately not folded.
- Shift counts are masked to x64's 0–63 behavior.
- Explicit struct-by-value sentinels are conservatively counted as frame reads in the dead-store helper.

**Defects/limits**

- C3/C4/C8/C9 apply.
- The slot lattice is exact-offset only and is not invalidated by most writes/calls.
- `LoadFrame` with a computed base (`src1 != 0`) is not distinguished from an ordinary frame load.
- The dead sweep is whole-function syntactic use, not reaching-def liveness; this is conservative for multiple definitions but does not solve implicit writes.
- Unreachable uses/stores prevent cleanup until SimplifyCFG runs.

**Safe local opportunities**

Before:

```text
v1 = ConstInt 4
v2 = ConstInt 3
v3 = Cmp.lt v1, v2
Branch v3, B1, B2
```

After:

```text
v3 = ConstBool false
Jmp B2                       ; with simplifycfg
```

Also safely add exact unary folds (`Neg`, `Not`, `BitNot`), exact integer casts with type-aware normalization, immediate substitution in either modeled operand position, and local `Move` chains—provided the shared evaluator uses defined wrap/width semantics.

**Requires analyses**

- Cross-block constants and branch executability: SCCP/reaching definitions and CFG.
- Frame constants across blocks/calls: memory dataflow plus alias/effect analysis.
- Join/loop facts: executable-edge lattice; block order is insufficient.

### 4.2 `dce`

**Sound parts**

- Explicit side-effecting calls, globals/indirect stores, copies, aggregate initialization, decrypt, and guards are retained.
- VReg uses include instruction operands, call args, `CopyBytes::dst`, and terminators.
- The sweep iterates to a fixpoint.

**Defects/limits**

- C3 and C8 apply.
- `is_pure` conflates “no explicit side effect opcode” with “safe to erase”; producer homes disprove that equivalence.
- Any use anywhere keeps every definition of a reused VReg. This is a missed optimization, but avoiding it requires reaching definitions/liveness.
- Reads in unreachable blocks keep stores/defs alive.

**Safe opportunity**

Run DCE again after rewriting passes. In the common sequence:

```text
v2 = Move v1
v3 = Add v2, 0
Return v1
```

CopyProp/InstCombine may turn all uses into `v1`; a final DCE can remove `v2`/`v3` once home-write semantics are handled. A bounded pipeline cleanup (`copyprop,instcombine,dce` to no-change) is safe and does not need global analysis for explicit VReg-only instructions.

**Requires analyses**

- Definition-level DCE for non-SSA VRegs: reaching definitions + liveness.
- Dead implicit homes/global frame stores: backward memory liveness and alias/escape analysis.

### 4.3 `cse`

**Defects**

C1, C2, C3, and C5 make the current implementation not generally value-preserving.

Additional observations:

- The key includes destination `frame_off`, so semantically identical arithmetic with different spill homes is missed.
- Commutative operations are not canonicalized.
- The remap scan does not rewrite `CopyBytes::dst`, even though it is a read-use.
- Rewriting all later uses is quadratic in block size and is especially fragile under redefinitions.

**Safe local opportunity**

Before:

```text
v3 = Add v1, v2, frame_off=-24
v4 = Add v2, v1, frame_off=-32
Return v4
```

After:

```text
v3 = Add v1, v2, frame_off=-24
v4 = Move v3, frame_off=-32       ; preserves v4 and its home
Return v4
```

Canonicalize operands only for integer `Add`, `Mul`, `And`, `Or`, `Xor`, and equality/inequality comparisons. Do not reorder subtraction, shifts, ordered comparisons, or floating operations. Replacing with a `Move`, instead of deleting the destination definition and globally rewriting uses, is the safest local non-SSA shape.

**Requires analyses**

- Inter-block CSE/GVN: dominators, value numbering, reaching definitions.
- Load CSE: byte-range memory versions and alias/effect barriers.
- Eliding the destination home: memory liveness or regalloc-aware dead-spill information.

### 4.4 `licm`

**Defects**

C6 and C10 apply. Further risks:

- A back edge is guessed from target index `<` source index, not “target dominates source.” Irreducible, reordered, and unreachable CFGs can produce false loops.
- “Defined outside loop” does not prove a definition dominates the preheader/use in non-SSA IR.
- Loop writes include only explicit `StoreFrame`, missing implicit homes, copies, initializers, globals, indirect stores, calls, widths, and escapes.
- Nested loops/multiple latches are processed as an unstructured list; there is no loop hierarchy or canonical preheader construction.
- Header instructions are skipped categorically. That avoids some regressions but misses ordinary invariants executed by the header.
- Dependency chains cannot be hoisted because every loop-defined operand is considered variant, even after its defining invariant is selected.

**Safe opportunity after basic loop/dominance support**

Before:

```text
body:
  v3 = Mul v1, v2
  v4 = Add v3, 7
  use v4
```

where `v1/v2` dominate the preheader and neither operation traps.

After:

```text
preheader:
  v3 = Mul v1, v2
  v4 = Add v3, 7
body:
  use v4
```

Discover invariants iteratively/topologically so dependency chains move in original order. Header invariants are legal when the instruction is speculatable or loop entry/execution is proven and the definition dominates all uses.

**Requires analyses**

CFG, dominators, natural-loop construction, loop nesting, unique/preheader creation, reaching definitions, memory effects/aliasing, and speculation/trap classification.

### 4.5 `forward`

**Sound parts**

- Deliberately local scope avoids unsound predecessor assumptions.
- The existing D11 regression correctly blocks forwarding across a direct frame-destination `CopyBytes` range.

**Defects/limits**

C7 and C8 apply. In particular, barriers are not unified with DSE/ConstProp and do not include all implicit/ranged/escaped writes.

**Safe local opportunity**

Continue forwarding only when:

1. the store is an ordinary rbp-relative exact/ranged store;
2. the source definition has not changed;
3. no overlapping writer/unknown barrier intervenes; and
4. widths/types are compatible.

Before:

```text
StoreFrame v1, -8
LoadFrame v2, -8
```

After:

```text
StoreFrame v1, -8
v2 = Move v1
```

This existing case remains safe with a source-definition generation check.

**Requires analyses**

Inter-block forwarding requires forward dataflow with predecessor meet, dominators/reaching definitions, and memory alias/effect generations. Agreeing predecessors may forward; disagreement must yield unknown.

### 4.6 `copyprop`

**Sound parts**

- Local maps are killed when a destination or a copy source is redefined.
- Ordinary `src1`, `src2`, call args, and block terminator `cond`/`ret` are rewritten.
- Locality avoids predecessor/dominance assumptions.

**Missed safe case — `CopyBytes::dst`**

`CopyBytes::dst` is a read-use of the runtime destination pointer, not a definition. CopyProp currently does not rewrite it and then treats the field as an ordinary destination kill.

Before:

```text
v2 = Move v1
CopyBytes dst=v2, src_frame=-32, len=16
```

After:

```text
v2 = Move v1
CopyBytes dst=v1, src_frame=-32, len=16
```

This is a safe local opportunity under the same redefinition rules already used for other operands. DCE can later remove `v2` if its spill-home write is proven unnecessary/preserved.

**Other limits**

- No inter-block propagation.
- Copy cycles/self-copies deserve explicit tests even though the current dependent-entry cleanup often removes a self-map.
- Aggregate sentinel `args[i] == 0` must remain zero; only real nonzero VRegs should resolve.

**Requires analyses**

Cross-block copy propagation requires reaching definitions and dominance, especially because the IR is non-SSA.

### 4.7 `instcombine`

**Sound parts**

- Replacements retain metadata, including destination home and type.
- Implemented integer identities preserve wrap semantics because they remove only neutral/idempotent operations after operands have already been evaluated.
- Constant facts are killed on local redefinition.
- Float algebraic identities are wisely not applied.

**Misses/limitations**

- The `Div x,1` case in the switch is unreachable because the pass first rejects every op not in `is_foldable_int_binop`, and that helper excludes `Div`.
- `x << 64`/`x >> 64` are identities under the documented x64-masked shift semantics, but only literal zero is recognized. More generally, counts congruent to zero modulo 64 are safe if this target semantic is the language contract.
- Narrow constants are not normalized in the lattice, causing missed identities such as an all-ones value represented as `255` for `u8`.
- It does not run cleanup itself; a later DCE is needed.

**Expected safe before/after**

```text
v2 = Div v1, 1       -> v2 = Move v1
v3 = Shl v1, 64      -> v3 = Move v1   ; only under the pinned x64 mask contract
```

Keep division trap behavior in mind: only divisor exactly one is removable; never fold divisor zero.

### 4.8 `dse`

**Sound parts**

- Scope is local, so it does not falsely assume overwrite on all successor paths.
- D10 correctly preserves an explicit store read by a simple `CopyBytes` source range.
- Explicit `LoadFrame`, `FieldAddr`, and struct-by-value sentinels are treated conservatively as readers in covered exact-offset cases.

**Defects/limits**

C8 applies: computed stores, overlapping widths/ranges, globals, indirect aliases, calls through escaped pointers, and implicit producer homes are not modeled. Calls are readers only for the aggregate sentinel case. The pass also cannot use `CopyBytes`/aggregate writes as safe overwrite opportunities because it lacks exact destination aliasing and conservatively labels some destinations as reads.

**Safe opportunity**

With exact non-overlapping interval classification:

```text
StoreFrame v1, [-16,-8)
CopyBytes src=[-40,-32), dst=[-16,-8)
LoadFrame [-16,-8)
```

The explicit store is dead because the exact copy destination fully overwrites it before any read. The current pass misses this. Partial overwrite must not remove the store.

**Requires analyses**

- Cross-block/global DSE: backward memory liveness over CFG.
- Calls/`StoreAddr`/escaped `FieldAddr`: escape and alias analysis.
- Aggregates and narrow accesses: byte-interval effects.

## 5. `simplifycfg` and header/registration mismatch

`simplifycfg` is registered in `ext_opt.cpp` even though it is outside the requested eight-pass set.

Current positive properties:

- It folds only narrowly local boolean facts, handles terminator uses directly, removes unreachable blocks by graph traversal, compacts IDs/targets, and avoids merging detected loop members.
- The current validation script returns 177 with the requested pipeline; `simplifycfg` was not included in that command.

Limitations/coverage:

- Loop membership still uses block-order backedge heuristics rather than dominance, although this is currently used conservatively to skip merges.
- It does not thread empty blocks.
- Registry coverage in `ir_passes_test.cpp` does not assert `reg.has("simplifycfg")`, and there are no focused structural tests in the reviewed test file for constant branch folding, unreachable dead cycles, ID remapping, equal-target branches, or merge constraints.

The pre-existing modified `ext_opt.hpp` additionally advertises and declares `bounds-elim`, but `ext_opt.cpp` neither implements nor registers it. Thus the header's “ten passes” claim and declaration do not match the implementation (nine registered passes: eight requested plus `simplifycfg`). This audit did not modify the header.

## 6. Unified improvement plan

### Safe, local work that does not require new global analyses

1. Stop CSE replacement at both old/new destination redefinitions; include terminators, `CopyBytes::dst`, or preferably emit a metadata-preserving `Move` rather than deleting the destination definition.
2. Key every semantic field and use defined integer folding helpers.
3. Model producer `meta.frame_off` as a write in every deletion/coalescing pass.
4. Add local source-definition generations to Forward.
5. Propagate copies into `CopyBytes::dst`.
6. Preserve original order for any LICM hoists; restrict v1 hoists to explicitly non-trapping scalar operations.
7. Run a final bounded `copyprop,instcombine,dce,dse` cleanup after CSE/LICM rewrites.
8. Add exact local commutative CSE using a destination-preserving `Move`.

Even these changes should share one instruction-effect classifier; independent ad hoc reader/writer lists have already diverged.

### Work that requires dataflow/dominance/alias infrastructure

1. **CFG/reachability + dominators:** global CSE/GVN, safe LICM, header handling, loop discovery.
2. **Reaching definitions/def-use for non-SSA VRegs:** definition-specific CSE/DCE/copy/forward correctness.
3. **Byte-range memory effects:** frame/global exact and ranged reads/writes, widths, `CopyBytes`, aggregate initialization.
4. **Escape/alias analysis:** `FieldAddr`, `IndexAddr`, `StoreAddr`, pointers passed to calls, unknown native/script effects.
5. **Loop analysis:** natural loops from dominance, preheaders, latches, exits, nesting, and irreducible rejection.
6. **SCCP:** broad inter-block constant propagation and unreachable-edge discovery.
7. **Memory liveness/versioning:** inter-block forwarding, load CSE, and global DSE.

The conservative fallback for any unknown call/pointer operation must be “may read/write all escaped memory,” not “no alias.”

## 7. Test coverage additions required

The current `ir_passes_test` passes, including D10/D11, but it lacks the cases that expose the most serious defects. Add hand-built IR tests for:

- CSE old-destination redefinition (expected 10, not 4).
- CSE same-block terminator use.
- CSE successor-block use.
- DCE and ConstProp sweep preserving a producer-only `meta.frame_off` write.
- CSE preserving/restoring a redundant producer's spill-home write across `CopyBytes`.
- ConstProp invalidating slot facts on implicit homes, `CopyBytes`, aggregate writers, calls, and computed stores.
- CSE signed/unsigned shift/compare distinction, cast target distinction, and float constants 1.0 vs 2.0.
- CSE load barriers for `StoreGlobal`, `StoreAddr`, `CopyBytes`, calls, and aggregate initializers.
- Forward source VReg redefinition.
- DSE computed `StoreFrame` pointers and partial byte-range overlap.
- DSE escaped frame pointer consumed by a call.
- CopyProp propagation into `CopyBytes::dst`.
- ConstProp `INT64_MAX + 1`, narrow unsigned chained shifts, and shifts at 0/63/64/127.
- LICM zero-trip trapping body, mutable global/frame loads, non-dominating operands, repeated VReg definitions, nested loops, multiple latches, irreducible and unreachable cycles, and invariant dependency chains.
- Final DCE after CSE/InstCombine/CopyProp rewrites.
- `simplifycfg` registry presence and focused CFG/ID tests.

Every source-level equivalence test should run with regalloc both enabled and disabled where possible: implicit frame homes are correctness fallbacks when values are spilled, so a register-resident run can mask a removed home write.

## 8. Bottom line

The reproduced `PASS` and validation value `177` confirm no regression on the current curated workloads. They do not support the broader claim that all eight implementations are value-preserving under the Thin IR contract. CSE has immediate non-SSA and block-boundary miscompilations; DCE/ConstProp/CSE misunderstand implicit spill-home writes; ConstProp, CSE, Forward, LICM, and DSE each have incomplete memory/alias models. These should be treated as correctness blockers before enabling the pipeline by default or broadening any inter-block optimization.
