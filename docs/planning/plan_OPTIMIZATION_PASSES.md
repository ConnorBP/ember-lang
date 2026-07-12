# ember JIT - Optimization Pass Implementation Plan

**Status:** research/design only  
**Scope:** future IR and code-generation work; no pass is implemented here  
**Research date:** 2026-07-12

## 1. Purpose and conclusions

This plan maps the advisor's next eight priorities onto ember's current `ThinFunction` IR and pass framework. The passes should **not** be implemented as eight unrelated scans. SimplifyCFG, SCCP, GVN, loop transforms, bounds-check elimination, and cross-block spill elimination need the same CFG, dominance, loop, def-use, memory-effect, and range analyses. Building those analyses first is the shortest path and the main correctness safeguard.

The highest expected near-term return is:

1. **cross-block forwarding plus dead spill elimination**, because lowering gives every scalar intermediate a frame home;
2. **range-based bounds-check elimination and induction/strength reduction** on indexed loops;
3. **SCCP + SimplifyCFG** as enabling cleanups;
4. **GVN with memory generations**, replacing block-local CSE's incomplete memory model;
5. **leaf/tail call work** for `call_overhead`, restricted by Win64 ABI, depth accounting, and hot reload.

Small-loop unrolling should be late and tightly costed. It can reduce branches and expose forwarding/CSE, but is the easiest item to turn into a JIT compile-time and instruction-cache regression.

## 2. Current implementation constraints

### 2.1 Thin IR and emission

`src/thin_ir.hpp` defines three-address IR that is **not formally SSA**:

- scalars are `VReg`s; slices use consecutive `{ptr,len}` VRegs;
- locals, parameters, aggregates, and scalar fallback homes use absolute negative `rbp` offsets;
- every `ThinBlock` has an explicit `Jmp`, `Branch`, `Return`, or `Trap` terminator;
- `ThinOp` IDs are serialized verbatim, so new opcodes must be appended;
- calls carry `args`, aggregate `arg_frame_offs`, `arg_types`, and `ret_type`.

Lowering creates dense block IDs, but transforms must not assume vector order permanently equals ID. Emission indexes `block_labels` by `blk.id` and terminator targets. A block-removing pass must compact IDs, rewrite all targets, and preserve entry ID 0.

### 2.2 Dominant spill cost

The post-lowering pass in `src/thin_lower.cpp` gives every plain scalar/float producer an eight-byte `meta.frame_off`. This is a correctness fallback: after `rax`/`xmm0` is clobbered, the VReg remains reloadable. Without regalloc, nearly every producer stores and many uses reload.

The new allocator improves this by keeping scalar integer/bool VRegs in callee-saved registers and promoting hot loop-carried scalar frame slots through `RegAllocResult::frame_reg_map`. It does not eliminate every fallback home, float/slice spill, redundant cross-block local access, call argument stash, or home store that is never reloaded. Dead-spill work must preserve correctness when regalloc is off or a range is spilled.

### 2.3 Existing passes

`extensions/opt/ext_opt.cpp` has `constprop`, `dce`, `cse`, `licm`, `forward`, `copyprop`, `instcombine`, and `dse`. The standard pipeline runs seven of these before regalloc. `EmberAnalysisManager` is a stub and `EmberPreserved` only reports all/none.

Important limitations not to copy:

- constprop and forwarding are block-local;
- CSE puts `frame_off` in every key even when it is merely the destination spill home;
- CSE does not treat calls as memory-version barriers for cached loads;
- loop discovery approximates back edges by block order, not dominance;
- aliases include byte ranges, `CopyBytes`, `StoreAddr`, aggregate call arguments, and exposed `FieldAddr` bases.

### 2.4 Benchmark targets

`bench/bench_codegen_paths.cpp` provides:

- `int_div`: integer arithmetic/division and loop-carried values;
- `call_overhead`: dispatch calls, prologues/epilogues, and argument stash;
- `loop_overhead`: the smallest hot scalar loop;
- `slice_bounds`: indexed array access plus bounds checks;
- `string_decrypt` and `struct_by_value`: aggregate/string paths;
- `cse_redundant`, `dce_dead_store`, `constprop_fold`, `licm_invariant`: pass probes.

Checked-in results establish the priority signal--loop, call, and bounds paths have historically been much farther from g++ `-O2` than `int_div`--but every win must be remeasured after hot-slot promotion. The benchmark is the acceptance gate.

## 3. Shared analysis foundation

Build these before loop/range/global passes. They can begin as `extensions/opt` utilities, then move behind a real cached `EmberAnalysisManager`.

### 3.1 CFGAnalysis

Provide ID-to-index mapping, validated successors/predecessors, entry reachability, reverse postorder, exits, edge replacement, and dense-ID compaction. CFG-changing passes invalidate it; instruction-only passes preserve it.

### 3.2 DominatorAnalysis

Use a simple iterative algorithm over reverse postorder; ember functions are small. Provide `dominates(a,b)` and dominator-tree children. This supports GVN, natural loops, and edge-derived ranges.

### 3.3 DefUseAnalysis

Track uses in `src1`, `src2`, `args`, `CopyBytes::dst` when nonzero, and terminator `cond`/`ret`. Record multiple definitions: join VRegs and hand-built/deserialized IR may redefine a VReg. Use SSA shortcuts only for a unique definition that dominates every use.

### 3.4 MemoryEffects and basic aliases

Classify exact/ranged frame and global reads/writes, unknown indirect memory, calls, traps/guards, and pure operations. Model:

- `LoadFrame`/`StoreFrame` widths and computed-address forms;
- `CopyBytes` source/destination byte ranges;
- `FieldAddr` exposing a frame base;
- `StoreAddr` as unknown unless provenance is exact;
- struct-by-value call arguments as frame reads;
- calls as global/unknown barriers and frame barriers for escaped addresses;
- decrypt, aggregate initializers, and guards as side-effecting.

The fallback is “may alias everything,” never “does not alias.”

### 3.5 LoopAnalysis

A back edge is an edge whose target dominates its source. Build natural loop bodies from predecessors and record header, latches, exits, nesting, and unique preheader. Add safe preheader creation; compact IDs afterward.

### 3.6 InductionAndRangeAnalysis

Represent signed/unsigned lower/upper bounds, width, and wrap status. Recognize lowering's slot-backed loop form:

1. preheader initializes frame slot `S`;
2. header loads `S` and compares it with a bound;
3. a controlling edge enters the body (including the extra compare-to-zero wrapper);
4. latch loads `S`, adds/subtracts constant stride, and stores `S`;
5. no unrecognized write to `S` occurs in the loop.

Record `{slot, initial, stride, bound, predicate, controlling edge}` and cheap affine derived ranges. Possible overflow invalidates a range unless no-wrap is proven.

### 3.7 CodeSizeEstimate

Unrolling needs a target-aware estimate, not just IR count. Estimate bytes per `ThinOp`, charging calls, guards, division, and aggregate copies heavily. Also enforce a hard IR-growth cap.

## 4. Pass plans

## 4.1 SimplifyCFG (`simplifycfg`)

**What it does.** Folds known branches, removes unreachable blocks, threads empty jump blocks, and merges a block with its sole-successor/single-predecessor block. This cuts branches and reduces work for later analyses.

**Mapping to ThinOp IR.**

- Resolve `ThinTerm::Branch.cond` from local `ConstBool`/`ConstInt`, `Move`, or folded `Cmp`; replace with `Jmp` to true or false target.
- Fold branches with identical targets regardless of condition.
- Mark blocks reachable from `blocks[0]`, delete the rest, assign dense IDs, and rewrite all targets.
- Thread an empty `Jmp` block unless it is intentionally retained as a loop preheader/instrumentation boundary.
- Merge `A -> B` when A has an unconditional jump, B has only A as predecessor, and B is not entry; append instructions and copy B's terminator.
- Keep reachable `BudgetCheck`, `DepthCheck`, calls, traps, and memory effects. Unreachable effects may be removed.

**Expected performance win.** Helps constant/branch-heavy code and `constprop_fold`; indirectly helps every benchmark through smaller CFGs, better SCCP/GVN/loop analysis, and shorter regalloc intervals. Primarily an enabling pass, with modest standalone runtime/code-size wins.

**Implementation complexity.** **Easy-medium.** Transformations are simple; ID compaction and predecessor maintenance are ember-specific risks.

**Dependencies.** `CFGAnalysis`; local constant lookup initially, SCCP later. Run before expensive analyses, after SCCP, and after unrolling.

**Value-preservation concerns.**

- Remap every ID/target because `thin_emit` indexes labels by block ID.
- Terminators, not vector order, define control flow.
- Never merge across `Return`/`Trap` or duplicate a multi-predecessor successor.
- Preserve block 0 and its entry `BudgetCheck`; emitter fallback charging examines block 0.
- Validate serialized IR before simplifying it; simplification must not hide malformed targets.

## 4.2 SCCP (`sccp`)

**What it does.** Solves constants and executable CFG edges together. A constant branch marks one successor executable, allowing constants to cross blocks and dead blocks to disappear.

**Mapping to ThinOp IR.**

Use a VReg lattice `{unknown, constant, overdefined}` plus executable edges/blocks. Evaluate only executable blocks:

- `ConstInt`, `ConstBool`, optionally exact-bit `ConstFloat`;
- `Move`;
- `Add`, `Sub`, `Mul`, bit ops, and shifts with width normalization and signedness;
- `Cmp` with predicate/width/signedness/float mode;
- only exactly implemented `Cast` cases;
- optionally `LoadFrame` from a slot proven immutable and identically initialized on all executable paths.

Treat calls, globals, indirect loads, div/mod, decrypt/aggregate ops, and uncertain casts as overdefined initially. At convergence, rewrite proven definitions to constants (retaining type, width, location, and spill home), fold branches, run SimplifyCFG, then DCE/copyprop.

For a VReg with multiple executable definitions, constant means all definitions meet to the same value. A unique-def/dominance path can be sparse; otherwise be conservative.

**Expected performance win.** Helps `constprop_fold`, constant conditionals/switches, loop setup, and opportunities exposed by unrolling. It can reduce code and later compile work substantially, but mutable slot-backed loop values limit its direct loop effect initially.

**Implementation complexity.** **Medium-hard.** The algorithm is compact; non-SSA VRegs, frame slots, widths, and ember's branch shape add complexity.

**Dependencies.** CFG, def-use, shared constant evaluator; then SimplifyCFG and DCE. It should supersede inter-block portions of current constprop while retaining a cheap local pre-pass.

**Value-preservation concerns.**

- Match x64 shift masking and width normalization; avoid C++ signed-overflow UB.
- Do not fold div/mod until zero, `INT_MIN/-1`, trap order, signedness, and width are exact.
- Float folding must preserve NaNs, signed zero, f32 rounding, and conversions; omit it in v1 if uncertain.
- A last syntactic constant store does not prove a frame load constant across predecessors, aliases, calls, or iterations.
- Safety checks are not dead merely because their result is unused; only unreachable flow removes them.

## 4.3 Improved CSE / global value numbering (`gvn`)

**What it does.** Assigns value numbers to equivalent computations and reuses a dominating value, extending CSE across blocks while invalidating cached loads at basic alias barriers. It also separates semantic operands from destination spill metadata.

**Mapping to ThinOp IR.**

Build expression keys from operand value numbers and semantic metadata:

- integer ops/`Cmp`: op, operands, width, signedness, predicate;
- float ops: op, operands, f32/f64, without algebraic reassociation;
- `Cast`: source plus full source/target type identity;
- `FieldAddr`/`IndexAddr`: base/index, offsets/width, relocation base/addend;
- `LoadFrame` and `LoadGlobal`: memory region plus its memory version.

Do **not** include an arithmetic producer's destination spill `meta.frame_off`. Canonicalize operands only for truly commutative integer operations and equality/inequality; never reorder subtraction, shifts, ordered compares, or float arithmetic.

Walk the dominator tree with scoped tables. Replace redundancy with `Move dst = dominating_value`, retaining the redundant destination's type, width, and frame home; copyprop/DCE can remove the move. This is safer than global use rewriting with redefined VRegs.

Memory generations/barriers:

- `StoreFrame` kills overlapping frame regions;
- `StoreGlobal` kills the target global region;
- `CopyBytes` kills overlapping destinations, or all uncertain memory;
- `StoreAddr` kills all cached loads unless provenance is exact;
- calls kill global/unknown entries and escaped frame entries;
- aggregate/decrypt/unknown effects kill relevant tables.

Never CSE guards, calls, stores, copies, initializers, or observable/trapping operations.

**Expected performance win.** Directly targets `cse_redundant`, repeated address calculations in `slice_bounds`, duplicated loop expressions, and code exposed by SCCP/unrolling. It should reduce arithmetic, frame reloads, and register pressure. `loop_overhead` benefits less than from spill elimination.

**Implementation complexity.** **Hard.** Pure GVN is medium; correct memory numbering and non-SSA handling make the complete pass hard.

**Dependencies.** CFG, dominators, def-use, MemoryEffects; SimplifyCFG before and copyprop/DCE after. A cheap second GVN cleanup is useful after LICM/unrolling.

**Value-preservation concerns.**

- Native/script calls can mutate globals and reachable memory; treat them as barriers absent explicit attributes.
- Compare byte intervals, not only frame offsets.
- Reused definitions must dominate replacements and not be redefined on the path.
- Include every semantic field: unsignedness, float mode, predicate, type, width, relocation kind/addend.
- Preserve trap order; conservatively exclude operations that may trap unless the dominating execution necessarily occurred first.

## 4.4 Loop strength reduction and IV simplification (`loopstrength`)

**What it does.** Recognizes canonical induction variables, removes redundant affine induction work, and replaces repeated multiplication/address scaling with cheaper increments or shifts. It also produces facts needed by bounds elimination and unrolling.

**Mapping to ThinOp IR.**

Low-risk phase:

- recognize slot-backed IVs through `LoadFrame`, `StoreFrame`, `Add`/`Sub`, `Cmp`, and loop terminators;
- canonicalize compare orientation in analysis;
- change `Mul x, 2^k` to `Shl x, k` when width/wrap behavior is identical;
- reuse one canonical IV when another is proven affine and has no observable independent store/address escape.

Recurrence phase:

- for `Mul iv,C` or `base + iv*width`, create a derived loop-carried value initialized in the preheader and advanced by `stride*C` in the latch;
- represent it with a hidden frame slot, fresh VRegs, `LoadFrame`/`StoreFrame`, and `Add` (compatible with no-phi IR); hot-slot promotion can keep it in a register;
- replace dominated original computations;
- allocate frame space below current storage, update `next_local_off`/aligned `frame_size`, and update VReg bounds for transformed deserialized IR.

Only create a recurrence when saved per-iteration work exceeds initialization/update and pressure. Power-of-two scales may already be cheaper as `IndexAddr` than as a new recurrence.

**Expected performance win.** Helps repeated indexing in `slice_bounds`, redundant IV work in `loop_overhead`, and arithmetic loops in `int_div`. The biggest expected win is indexed loops with nontrivial scaling, not `idiv` itself.

**Implementation complexity.** **Hard.** Recognition is medium; safely creating slot-backed recurrences is hard.

**Dependencies.** CFG, dominators, LoopAnalysis, def-use, induction/range analysis, preheaders; GVN/copyprop/DCE after. Run before bounds elimination.

**Value-preservation concerns.**

- Prove no-wrap whenever monotonic range reasoning depends on it.
- Narrow operations normalize after each instruction; reassociation across truncation can change results.
- Reject negative/variable/zero strides, multiple latches, early exits, and body IV writes initially.
- Hidden slots must not overlap existing storage and must serialize correctly.
- Never move/combine `BudgetCheck`; frequency and charge are observable safety behavior.

## 4.5 Redundant bounds-check elimination (`bounds-elim`)

**What it does.** Removes a `BoundsCheck` only when dominance and loop ranges prove its index is nonnegative and strictly below length on every reaching execution. First target canonical fixed-array and invariant-slice-length loops.

**Mapping to ThinOp IR.**

`BoundsCheck` uses `src1=index`, `src2=dynamic length`, or immediate `imm.i` when `src2==0`; `IndexAddr` commonly follows with the same index. Remove a check under one of these proofs:

1. **Fixed array:** controlling true edge proves `0 <= iv < upper`, `upper <= imm_len`, stride positive, and the check precedes update.
2. **Slice:** condition proves `0 <= iv < len`, where `len` is the same dominating VReg/value or invariant length slot, with no write/barrier.
3. **Affine index:** range `[lo,hi]` proves `lo>=0 && hi<len`, initially only constant affine offsets with no wrap.
4. **Dominating check:** an identical/stronger prior check dominates and index/length cannot change.

Delete only the check; leave `IndexAddr` and the memory operation. DCE removes dead setup afterward.

**Expected performance win.** Directly helps `slice_bounds` and fixed-array scans/initialization by removing a compare, branch, and cold trap path per access. It also reduces code size and pressure around `IndexAddr`.

**Implementation complexity.** **Hard.** The edit is trivial; proving both bounds under signed/unsigned wrapping rules is difficult.

**Dependencies.** CFG, dominators, LoopAnalysis, induction/ranges, value identity/GVN, MemoryEffects for dynamic lengths. Run after IV simplification and before DCE.

**Value-preservation concerns.**

- Emission checks unsigned; prove a negative signed index impossible, not merely signed `index < len`.
- Dynamic length must be invariant and identical, not just equal at one point.
- Reject possible overflow, negative/variable stride, body mutation, multi-entry loops, and bypass paths.
- A false positive is a memory-safety bug; conservative misses are acceptable.
- Optimize the IR actually present, including deserialized IR; do not assume source lowering supplied a proof.

## 4.6 Forwarding improvements and dead spill elimination (`memforward` / `deadspill`)

**What it does.** Extends frame-slot forwarding and DSE across CFG edges, then suppresses emitter fallback stores that no later use requires. This attacks the main bottleneck directly.

**Mapping to ThinOp IR.**

### Cross-block frame forwarding

Compute reaching frame values per block. A region is forwardable only when all executable predecessors agree on the same VReg/value number and no aliasing writer intervenes.

- `StoreFrame src1=v,off=S` establishes `S -> v` for its written range;
- `LoadFrame dst=d,off=S` becomes `Move d,v` when exact;
- copies, `StoreAddr`, aggregate writes, calls, and overlaps kill mappings via MemoryEffects;
- joins meet to same-value or unknown;
- retain metadata/type/width; copyprop/DCE follows.

### Global frame DSE

Run backward slot liveness over CFG. A `StoreFrame` is dead only if every path overwrites/exits before a read and the slot has not escaped. Count `LoadFrame`, `CopyBytes` sources, `FieldAddr`, struct call arguments, and unknown readers.

### Dead implicit spills

Producer `meta.frame_off` causes an **implicit emitter store**, not a `StoreFrame`. Handle it late and regalloc-aware:

- extend `RegAllocResult` (or a post-RA map) with per-VReg `needs_home_store/reload`;
- a fully register-resident VReg needs no home store;
- suppress a spilled VReg's store only if all uses occur while the result register remains valid or the value is rematerializable, with no edge/call clobber;
- allow rematerialization of constants/cheap pure values when costed profitable;
- retain frame offsets as eviction fallbacks. Do not clear them before regalloc, which currently requires them.

This needs a post-regalloc optimization hook or integration with allocation/emission. Initially leave `frame_size` unchanged; compacting holes risks aggregate/temp offset errors.

**Expected performance win.** Expected to be the **largest pass-level win** on `loop_overhead`, `cse_redundant`, `constprop_fold`, and arithmetic portions of `int_div`. It may help call args/results, though the separate argument stash remains expensive.

**Implementation complexity.** **Hard.** Cross-block forwarding is medium; alias-correct global DSE and implicit-spill suppression are hard.

**Dependencies.** CFG, def-use, MemoryEffects, value numbering, liveness, and regalloc. Forward before GVN/DCE; decide implicit spills after all IR transforms with/after regalloc.

**Value-preservation concerns.**

- Hidden reads include copies, field addresses, struct calls, and escaped addresses.
- Overlapping narrow/wide accesses need byte ranges.
- Calls/native code barrier escaped memory.
- `CopyBytes` destinations invalidate forwarding, as current regression tests show.
- Removing homes before final allocation recreates the stale-`rax` bugs the spill pass fixed.
- Regalloc-disabled behavior must remain independently correct.

## 4.7 Small-loop unrolling with strict limits (`loop-unroll`)

**What it does.** Clones a very small canonical loop body to reduce branch/compare overhead and expose adjacent operations to forwarding, GVN, and instcombine. It refuses transformations whose growth, control flow, safety accounting, or pressure exceeds tight limits.

**Mapping to ThinOp IR.**

Start with:

1. **Full unroll** for known trip counts 0-4, single-entry loops, one latch, no early exits.
2. **Factor-2 partial unroll** only with a proven trip count/remainder or a correctly generated cleanup loop.

Clone body/step instructions in execution order. Give cloned definitions fresh VRegs and remap intra-clone uses; leave loop-carried state in frame slots. Update `declared_max_vreg`/serialization bounds. Preserve `Loc`, types, frame homes, calls, and relocation metadata.

Recommended first defaults:

- full trip count <=4; partial factor exactly 2;
- body <=12 ordinary IR instructions;
- estimated net x64 growth <=64 bytes and <=10% of function;
- total IR growth <=24 instructions;
- no calls, decrypt, aggregate copy/init, `StoreAddr`, nested loops, multiple exits, or unknown memory effects;
- skip if predicted liveness grows by more than two registers;
- skip loops containing `BudgetCheck` in v1 unless cloning/charging is proven behaviorally identical.

Immediately run SimplifyCFG, SCCP/instcombine, GVN, and DCE afterward.

**Expected performance win.** Helps tiny fixed-count loops and may help factor-2 versions of `loop_overhead`/array scans. It also exposes more forwarding/GVN. Strict limits mean large `%N` loops only benefit if partial unrolling is enabled.

**Implementation complexity.** **Hard.** Cloning is mechanical; remainder handling, VReg renaming, safety behavior, and profitability are not.

**Dependencies.** Canonical loops, induction/trip counts, CodeSizeEstimate, fresh-VReg helper, CFG mutation/compaction. Run after strength/bounds work and before final cleanup.

**Value-preservation concerns.**

- Preserve iteration count, side-effect/trap order, and final visible IV value.
- Budget/depth behavior is observable; do not silently amortize checks.
- Reject duplicated calls, stores, decrypts, and traps initially.
- Fresh VRegs need valid homes/frame sizing and serialization bounds.
- Hard growth limits are required to prevent runtime and JIT-time regressions.

## 4.8 Tail-call or leaf-call optimization (`callopt`)

**What it does.** Recognizes a call whose result is immediately returned and lowers eligible script calls as sibling tail transfers; separately classifies leaf functions so emission can omit unnecessary save/restore and frame work. Tail calls target stack growth, while leaf work directly targets `call_overhead`.

**Mapping to ThinOp IR.**

### Tail-call recognition

Match a block with optional paired `DepthCheck`, one `CallScript`, no observable work afterward, and `Return ret=call.dst` (or void/void), with compatible ABI.

Do not overload an unrelated metadata field. Append a serialized representation, such as `TailCallScript`, or add a tail-call terminator. Because `ThinTerm` lacks argument vectors, an appended `ThinOp::TailCallScript` constrained to the last instruction is the smaller schema change, but validator/serializer must enforce it.

Narrow first eligibility:

- script call only, retaining dispatch lookup/hot reload;
- same scalar ABI, or self-tail call with identical argument word layout;
- no struct-by-pointer/slice return, indirect/cross-module/native call, defer/cleanup, try/catch, or coroutine;
- outgoing words fit safely in reusable incoming/shadow space unless a full Win64 stack shuffler exists;
- parallel argument moves use temporary homes so swaps such as `f(a,b)->f(b,a)` work.

The emitter must evaluate/stash args and resolve dispatch before frame destruction, restore regalloc's callee-saved registers, tear down the frame while preserving the original return address, place Win64 register/stack args, then `jmp` to the current dispatch target.

Depth accounting needs policy. To preserve current behavior, perform the normal check, then undo its increment before the jump because no return-side leave occurs. The callee's budget entry check still runs. A simpler v1 may disable TCO with depth checks enabled.

### Leaf emission

A function is leaf only with no call op and no trap path that calls a host stub. After regalloc, if no callee-saved pool register and no required frame/param/local address exists, use a reduced/frameless prologue. Even without full frameless emission, omit unused saves/restores. Preserve alignment and any host unwind/debug contract.

**Expected performance win.** Leaf emission helps `call_overhead`, whose tiny helpers pay repeated prologue/epilogue cost. Tail calls help a new tail-recursion benchmark and avoid linear stack growth; they do not help non-tail Fibonacci. Dispatch remains indirect for hot reload, so not all call overhead disappears.

**Implementation complexity.** **Leaf: medium; tail: hard.** Win64 stack args, saved registers, depth checks, and parallel moves make general TCO hard.

**Dependencies.** CFG, def-use, ABI/call classification, cleanup-state metadata, final regalloc result, serializer/validator changes, emitter support. Recognize tails after CFG/copy cleanup; decide leaf emission after regalloc.

**Value-preservation concerns.**

- Preserve hot reload by jumping through dispatch, never baking the callee entry.
- Preserve depth-limit traps and avoid leaked increments.
- Callee must see aligned Win64 stack, shadow/stack args, context register, and original return address.
- Restore every caller-owned callee-saved register before jumping.
- Reject active defers/cleanups until explicit safe tail positions exist.
- Exclude struct/slice hidden-return ABIs initially.

## 5. Recommended sequence and pipeline

### 5.1 Engineering sequence

1. Analysis substrate: CFG, dominators, def-use, memory effects, loops, ranges, code-size estimate; upgrade analysis caching/preservation IDs.
2. SimplifyCFG.
3. SCCP plus SimplifyCFG cleanup.
4. Cross-block memforward/global frame DSE; measure it early.
5. GVN, pure first, then versioned frame/global loads.
6. Induction/range analysis and low-risk strength reduction.
7. Fixed-array bounds elimination, then invariant slice lengths.
8. Regalloc-aware dead implicit spill suppression.
9. Full tiny-loop unroll, then factor-2 only after remainder/budget tests.
10. Leaf optimization, then restricted script TCO.

### 5.2 Proposed optimized pipeline

```text
simplifycfg,
sccp,simplifycfg,
memforward,copyprop,instcombine,dce,
gvn,copyprop,dce,
loopstrength,licm,bounds-elim,
loop-unroll,simplifycfg,
sccp,gvn,copyprop,instcombine,dce,dse,
callopt,
regalloc,deadspill
```

`regalloc,deadspill` are stages rather than ordinary registry passes today. Bound any cleanup fixpoint; never put unrolling in an outer fixpoint. Run performance optimization before obfuscation `subst`, because later optimization would undo substitutions. Migrate LICM to shared dominance/loop analyses before depending on it.

## 6. Testing and benchmark plan

### 6.1 Structural pass tests

Extend the hand-built `ThinFunction` style in `examples/ir_passes_test.cpp`:

- **SimplifyCFG:** constant branches, equal targets, unreachable trap, threading, merge, dense remapping.
- **SCCP:** executable diamond; identical/conflicting multi-def constants; loop convergence; width/shift boundaries.
- **GVN:** dominating reuse, sibling non-reuse, commutativity, metadata distinctions, call/StoreAddr/overlapping-copy barriers.
- **Loop strength:** accepted positive IV; rejected negative/variable/wrapping IV; recurrence placement.
- **Bounds:** accepted fixed range; rejected negative start, wrap, wrong dynamic length, mutation, bypass.
- **Memforward/deadspill:** agreeing/disagreeing predecessors, copy/call/escape barriers, retained home on regalloc eviction.
- **Unroll:** trip 0/1/4, remainder, final IV, growth refusal, effect/budget refusal, fresh VRegs.
- **Callopt:** exact/non-tail shape, arg swap, depth behavior, leaf classification, hot-reloaded target.

Each pass also needs empty/no-op tests returning `EmberPreserved::all()`.

### 6.2 End-to-end equivalence

For every source workload run: no pass, one pass, full pipeline; regalloc on/off; safety on/off. Compare values and, for failing inputs, trap reason and side-effect order. Include narrow signed/unsigned values, negative indices, integer boundaries, NaNs/signed zero, calls mutating globals, copies, and nested CFGs.

Round-trip transformed functions through Thin IR serialization and `validate_thin_function`. New VRegs, blocks, opcodes, frame offsets, and enum values must survive. Add a differential fuzzer for validator-clean scalar CFGs: execute baseline/transformed IR for random inputs and compare return/trap outcomes.

### 6.3 Benchmark additions

Add:

- `cfg_constant_diamond`;
- `sccp_cross_block`;
- `gvn_dominated` plus alias-barrier correctness probe;
- `strength_indexed_loop`;
- `bounds_proven_fixed`, then `bounds_proven_slice`;
- `spill_dependency_chain` and `spill_loop_carried`;
- `unroll_trip4` and `unroll_factor2`;
- `tail_recursive_sum` and `leaf_call_chain`.

Report median runtime, paired CI, compile time, bytes, IR/block counts, checks removed, and estimated/actual frame load-store counts. Enable a pass only when its target improves without meaningful unrelated regressions or unacceptable compile/code growth.

## 7. Acceptance gates

A pass enters the default performance pipeline only when:

- value and trap equivalence hold in focused/full tests, safety on/off, regalloc on/off;
- transformed IR validates and serialization round-trips;
- block IDs remain dense and targets valid;
- compile-time and code growth are measured/bounded;
- code growth is nonpositive except cost-gated unroll/TCO support;
- the target benchmark improves repeatably after hot-slot promotion;
- unrelated regressions are noise-level or profitability-gated;
- alias/range uncertainty causes a missed optimization, never speculation.

## 8. Final priority recommendation

If only three efforts are funded next:

1. **Build shared analyses and ship SimplifyCFG + SCCP.** This creates the reliable platform current direct traversals lack.
2. **Implement cross-block forwarding and regalloc-aware dead-spill suppression.** This attacks the documented frame bottleneck and is most likely to move `loop_overhead`.
3. **Implement induction/range analysis and fixed-array bounds elimination.** This targets the bounds-heavy path and enables strength reduction/unrolling.

GVN follows once memory generations exist. Leaf work precedes general TCO. Unrolling is last: useful only after the optimizer can clean what it clones and prove growth pays.

## 9. Files studied

- `src/thin_ir.hpp`
- `src/thin_lower.cpp`
- `src/thin_emit.cpp`
- `src/regalloc.cpp`
- `extensions/opt/ext_opt.cpp`, `ext_opt.hpp`
- `extensions/obf/ext_obf.cpp`
- `src/ember_pass.hpp`, `ember_pass_pipeline.hpp`, `ember_pass_registry.hpp`
- `bench/bench_codegen_paths.cpp`, `bench/results_codegen_paths.md`
- `examples/ir_passes_test.cpp`
- `docs/spec/PASS_SYSTEM_DESIGN.md`
- `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md`
