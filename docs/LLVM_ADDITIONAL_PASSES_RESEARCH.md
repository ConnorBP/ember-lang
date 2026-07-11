# LLVM Additional Passes Research — For Ember's Thin IR

> **Purpose:** Research which *additional* LLVM optimization passes could be adapted for ember's thin IR (`ThinFunction`), assess each for applicability to naive three-address TAC (NOT SSA), implementability as an IR→IR transform over `ThinFunction`, and expected impact on a compile-time-sensitive JIT. This is the companion to `docs/LLVM_PASS_SYSTEM_RESEARCH.md` (which studied the pass *system* architecture) — this doc studies the *passes themselves*.
>
> **Primary source:** LLVM 18.1.8 source tree at `E:\DEVELOPER\LLVM_18_1_8`.
> **Ember source:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`.
> **Scope:** Research only. No ember source code is written here. Each pass is assessed for: complexity (1–10), expected bench-path impact, analysis requirements, and IR-level-vs-byte-level placement.
>
> **The performance basis** is `docs/audit/PERFORMANCE_AUDIT_2026-07-11.md` (the read-only deep audit). Every impact claim in this doc routes through a specific finding in that audit.

---

## Table of Contents

1. [The Performance Problem These Passes Must Address](#1-the-performance-problem-these-passes-must-address)
2. [Ember's IR Shape — What the Passes Have to Work With](#2-embers-ir-shape--what-the-passes-have-to-work-with)
3. [The Eight Passes](#3-the-eight-passes)
   - [3.1 Store-to-Load Forwarding (EarlyCSE shape) — the headline](#31-store-to-load-forwarding-earlycse-shape--the-headline)
   - [3.2 Instruction Combining (InstCombine)](#32-instruction-combining-instcombine)
   - [3.3 Constant Propagation Through Memory (MemCpyOpt / load-store forwarding)](#33-constant-propagation-through-memory-memcpyopt--load-store-forwarding)
   - [3.4 Dead Store Elimination (DSE)](#34-dead-store-elimination-dse)
   - [3.5 Branch Folding / Unreachable Block Elimination](#35-branch-folding--unreachable-block-elimination)
   - [3.6 Copy Propagation](#36-copy-propagation)
   - [3.7 Strength Reduction](#37-strength-reduction)
   - [3.8 Peephole on the IR (not bytes)](#38-peephole-on-the-ir-not-bytes)
4. [The Analysis Problem — What Ember Doesn't Have Yet](#4-the-analysis-problem--what-ember-doesnt-have-yet)
5. [Recommended Implementation Order](#5-recommended-implementation-order)
6. [What NOT to Port (YAGNI for a JIT)](#6-what-not-to-port-yagni-for-a-jit)
7. [Cross-References](#7-cross-references)

---

## 1. The Performance Problem These Passes Must Address

The deep audit (`docs/audit/PERFORMANCE_AUDIT_2026-07-11.md`) established three facts that drive every recommendation in this document:

**Fact A — the IR backend is uniformly slower than the tree-walker.** Finding 2.1 measured the IR backend at 1.2–1.9× *slower* than the tree-walker on every bench path:

| path | tree-walker median ns | IR backend median ns | IR / tree-walker |
|---|---|---|---|
| loop_overhead | 9,729,700 | 17,347,300 | 1.78× slower |
| call_overhead | 1,064,000 | 1,584,000 | 1.49× slower |
| cse_redundant | 1,563,800 | 2,520,100 | 1.61× slower |
| dce_dead_store | 1,354,700 | 2,128,500 | 1.58× slower |
| constprop_fold | 1,538,800 | 2,504,600 | 1.63× slower |
| licm_invariant | 1,352,100 | 2,505,800 | 1.86× slower |

**Root cause (audit Finding 2.1, `src/thin_emit.cpp:364-391`):** `record_dst` does an additional `store_rax_to_rbp(e, meta.frame_off)` on every instruction that has a `frame_off` — and the lowering assigns frame slots to most VRegs. So the IR backend's per-instruction pattern is:

```
load lhs from frame   → load_rbp_to_rax      (thin_emit.cpp:81)
push rax                                       (thin_emit.cpp:1421, emit_int_binop VReg form)
load rhs from frame   → load_rbp_to_rax
mov rcx, rax
pop rax
op rax, rcx
store result to frame → store_rax_to_rbp      (thin_emit.cpp:80, called from record_dst:382)
```

The next instruction that uses the result loads it right back. **Every instruction result is round-tripped through a frame slot.** The tree-walker keeps the result in `rax` and uses it directly in the next eval. This is the single most impactful overhead in the IR backend, and it is exactly what store-to-load forwarding addresses.

**Fact B — the existing passes have O(n²) compile-time patterns.** Finding 3.1: ConstProp recomputes `compute_used_vregs(f)` and `compute_read_slots(f)` (each a full O(n) scan) on *every instruction removal* (`ext_opt.cpp:262-263`), making the dead-constant sweep O(n²) per fixpoint iteration. Finding 3.2: CSE scans the entire hash table on every VReg redefinition (`ext_opt.cpp:374-382`) and scans all following instructions on every rematch (`ext_opt.cpp:418-427`) — O(n²) per block. These are bounded by "the functions are small" today but will bite on large functions.

**Fact C — the tree-walker is 5–9× slower than g++ -O2.** Finding 1.1: the per-binary-op `push rax; eval(rhs); pop rax` spill (`codegen.cpp:1893-1900`) is the most-executed instruction on the loop and arithmetic paths. Finding 1.2: per-call indirect dispatch + arg double-move (`codegen.cpp:2434-2490`). These are the headline numbers that the audit's Recommendation 1 names as "the single highest-leverage runtime fix."

**The implication for this research:** the passes that matter most are the ones that attack Fact A (the store-to-frame round-trip) and Fact B (the O(n²) compile-time). Fact C is a tree-walker/regalloc problem, not an IR-pass problem — but the IR passes can *partially* compensate by eliminating instructions so there are fewer store-to-frame round-trips to pay for (the audit's Finding 3.4 shows constprop_fold is the one case where IR+passes catches the tree-walker, precisely because it eliminated enough instructions).

---

## 2. Ember's IR Shape — What the Passes Have to Work With

Before assessing any pass, the constraints from `src/thin_ir.hpp`:

**Naive three-address TAC, NOT SSA.** Each `ThinInstr` is `dst = op src1 src2` (+ imm + meta + args). A `VReg` may be reassigned (`thin_ir.hpp:59`: "NOT single-assignment: a VReg may be reused/reassigned"). There are no phi nodes. This means:

- **No dominance frontier, no phi insertion, no rename.** Cheaper to build than SSA-lite (the comment at `thin_ir.hpp:53` says so explicitly).
- **A VReg's value is NOT stable across its lifetime.** `v3 = Add v1 v2` at instruction 5 and `v3 = Mul v4 v5` at instruction 12 are the same VReg holding different values. Any pass that tracks "what value does v3 hold?" must invalidate on redefinition. This is the same constraint LLVM's non-SSA analyses handle with generations (see §3.1).
- **CSE's kill rule already respects this** (`ext_opt.cpp:374-382`): when an instruction redefines a VReg, table entries using it as a source are erased. The O(n²) cost is in *how* it erases (full table scan), not in *whether* it respects redefinition.

**Frame offsets are absolute rbp-negative.** `ThinMeta::frame_off` (`thin_ir.hpp:118-120`) is the absolute offset. `LoadFrame`/`StoreFrame`/`CopyBytes`/`FieldAddr` use it. A frame slot is identified by its `frame_off` integer — there is no alloca, no pointer SSA, no MemorySSA. **A frame slot is just an int32_t key.** This is enormously simpler than LLVM's memory model and makes the store-to-load forwarding pass much easier than LLVM's EarlyCSE (see §3.1).

**Blocks + terminators, but no dominator tree.** `ThinFunction` has `blocks[0]` as entry, each `ThinBlock` has a `ThinTerm` (`Jmp`/`Branch`/`Return`/`Trap`). There is no dominator tree, no post-dominator tree, no loop info object. `LICMPass` (`ext_opt.cpp:456-611`) builds its own predecessor map + back-edge detection on the fly because no analysis provides it. **Any pass that needs dominance or liveness must either build it locally or wait for the `EmberAnalysisManager` to grow** (the PASS_SYSTEM_DESIGN.md §6 defers the analysis manager to "FUTURE").

**Side-effect classification already exists.** `ext_opt.cpp:25-41` (`is_side_effecting` / `is_pure`) classifies every `ThinOp`. This is the foundation every memory pass needs — it tells you which instructions can be removed by DCE and which can be CSE'd. The eight passes below build on this.

**The bench paths the impact assessment references** (from `bench/bench_codegen_paths.cpp:make_paths`):

| bench path | what it exercises | ir_safe |
|---|---|---|
| `int_div` | `s = s + (i*7)/(i+1) + (i*13)%(i+2)` — div/mod + add chain in a loop | yes |
| `call_overhead` | `a(i) → b(x) → c(x)` — 4 script calls per iteration | yes |
| `loop_overhead` | `s = s + i; i = i + 1` — tight integer loop | yes |
| `slice_bounds` | `a[i%64]` — bounds-checked indexing | no (Stage A gap) |
| `string_decrypt` | `string_length("hello world!")` — inline-XOR decrypt per use | no (Stage A gap) |
| `struct_by_value` | `sump(mkp(1,2,3))` — hidden-pointer struct ABI | no (Stage A gap) |
| `cse_redundant` | `let a = i*7; s = s + a + a` — redundant computation | yes |
| `dce_dead_store` | `let dead = i*13;` (unused) — dead store | yes |
| `constprop_fold` | `let b=3; let c=b+4;` — constant folding | yes |
| `licm_invariant` | `let k = 100*200;` in a loop — loop-invariant | yes |

The 6 original paths are the first 6; the 4 IR-pass workloads are the last 4. The `ir_safe` column tells you which paths the IR backend can even run on (the known Stage A gaps — slices/strings/structs — would crash the IR path). **Every impact claim below is scoped to the `ir_safe` paths**, because that's where IR passes can help.

---

## 3. The Eight Passes

For each pass: what LLVM does (with file:line), how it maps to ember's IR, complexity (1=easy, 10=very hard), expected impact per bench path, analysis requirements, and IR-level-vs-byte-level placement.

### 3.1 Store-to-Load Forwarding (EarlyCSE shape) — the headline

**What LLVM does.** `EarlyCSE` (`llvm/lib/Transforms/Scalar/EarlyCSE.cpp`) is a single-pass dominator-tree walk that maintains an `AvailableLoads` table mapping a **pointer** → `(defining instruction, generation, matching id, is_atomic, is_load)`. The mechanism (traced from the source):

1. **On a store** (`EarlyCSE.cpp:1771-1772`): insert the store into `AvailableLoads` with `is_load=false`. This says "the value at this pointer is now known to be the value this store wrote." The store's *stored value* is the forwarded value.
2. **On a load** (`EarlyCSE.cpp:1557-1571`): look up the pointer in `AvailableLoads`. If the defining instruction is at the same memory generation (no intervening write), **replace the load with the stored value** (`Inst.replaceAllUsesWith(Op)` at `:1569`). This is store-to-load forwarding.
3. **Generation counter** (`:1370`, `:1764`): `CurrentGeneration` is bumped whenever memory is written (`++CurrentGeneration` at `:1764`). Loads from a previous generation are invalid. For single-predecessor blocks, the generation carries forward; for multi-predecessor blocks, it's bumped at block entry (`:1370`). This is the conservative invalidation that replaces MemorySSA in the simple case.
4. **Writeback DSE** (`:1687-1711`): if a store writes back the same value a load just read from the same location, the store is dead (removing it keeps the `AvailableLoads` table valid so forwarding can continue past it).

EarlyCSE also does trivial CSE (the `AvailableValues` table at `:1521-1535`) and trivial dead-store elimination (`LastStore` at `:1779-1793`), but the **store-to-load forwarding** is the part ember needs.

**Why this is THE pass for ember.** Ember's IR backend round-trips every instruction result through a frame slot (audit Finding 2.1, `thin_emit.cpp:364-391`). The IR pattern after lowering is:

```
v3 = Add v1 v2        ; meta.frame_off = -24 (the lowering assigns a slot)
                       ; emit: load v1 from -8, push, load v2 from -16, pop, add,
                       ;       store rax to -24   ← the round-trip store
v4 = Add v3 v5        ; emit: load v3 from -24   ← the round-trip load
                       ;       push, load v5 from -32, pop, add, store rax to -40
```

Store-to-load forwarding at the IR level rewrites this to:

```
v3 = Add v1 v2        ; (frame_off still -24, but the next use is a Move, not a LoadFrame)
v4 = Add v3 v5        ; v3 is used directly as a VReg, not loaded from -24
```

The `LoadFrame dst, off=-24` becomes `Move dst, v3` (or the use of v3 is substituted directly into the next instruction's `src1`/`src2`). When the emit runs, `load_int_vreg(v3)` finds `v3 == rax_vreg` (it was just produced and is still in rax) and emits **nothing** — no `load_rbp_to_rax`, no frame round-trip. This is exactly the compensation the audit's Recommendation 3 names: "It becomes a runtime win only when paired with a real regalloc that eliminates the per-instruction store-to-frame." Store-to-load forwarding is the *IR-level half* of that regalloc — it eliminates the load half of the round-trip even without a real register allocator, because the emit's `rax_vreg` cache (`thin_emit.cpp:123`) already keeps the most-recently-produced value in rax.

**The ember version — simpler than LLVM's.** Ember's frame slots are identified by `int32_t frame_off` (an integer key), not pointers with aliasing. There is no alias analysis, no MemorySSA, no pointer comparison. The `AvailableLoads` table is just `unordered_map<int32_t, LoadValue>` where `LoadValue = { VReg src; uint32_t gen; }`. The "pointer" is the frame offset. The "generation" is bumped on any `StoreFrame` to a *different* slot (conservatively) or on any call (calls may write to frame slots via struct-by-value). **No dominance is needed for the intra-block case** — the pass walks each block's instruction list linearly, maintaining the table per block (exactly like ConstProp does at `ext_opt.cpp:147`).

**The algorithm (intra-block, no dominance):**

```
for each block:
    map<int32_t, {VReg src, uint32_t gen}> available;  // frame_off → last writer
    uint32_t gen = 0;
    for each instr in block:
        if instr is StoreFrame(src=vN, off=X):
            // forward: any later LoadFrame from X can use vN
            available[X] = {vN, gen};
            // (the StoreFrame itself stays — it's needed if the slot is
            //  read after the block ends, or by a CopyBytes/FieldAddr)
        else if instr is LoadFrame(dst=vD, off=X) and X in available at current gen:
            // FORWARD: replace the load with a Move from the stored VReg
            instr.op = Move; instr.src1 = available[X].src; instr.meta.frame_off = 0;
            // (or: substitute available[X].src directly into every use of vD —
            //  but Move is safer because it doesn't change VReg identity)
            // Remove X from available (the Move doesn't preserve the slot value)
            available.erase(X);
        else if instr is StoreFrame(off=X) or call or CopyBytes:
            // invalidate: any write to a slot kills all pending forwards
            // (conservative: bump gen, or just clear available entirely for calls)
            ++gen;
        // any other instr: leave available alone (it doesn't touch frame memory)
```

**Inter-block forwarding** (a LoadFrame in block B forwarding from a StoreFrame in dominating block A) requires a dominator tree to prove A dominates B and no intervening write exists on every path. This is the `DomTreeNode` walk in EarlyCSE (`:1751-1770`). **For the first cut, skip inter-block — do intra-block only.** The bench's hot loops are single-block loop bodies where the store and the reload are in the same block. Inter-block forwarding is a Stage 2 upgrade that needs the dominator-tree analysis (§4).

**Complexity: 4/10 (intra-block), 7/10 (inter-block with dominance).** The intra-block version is ~80 lines — a linear walk with a hash map, the same shape as ConstProp's per-block constant tracking (`ext_opt.cpp:147-205`). The inter-block version needs a dominator tree (which ember doesn't have) and a scoped hash table that pops on block exit (the `ScopedHashTable` at `EarlyCSE.cpp:695-700`). Do intra-block first.

**Expected impact — the highest of any pass in this doc:**

| bench path | impact | why |
|---|---|---|
| `loop_overhead` | **HIGH** | `s = s + i; i = i + 1` — each result is stored then reloaded next iteration. Forwarding eliminates the reload of `s` and `i`. This is the path where the IR backend is 1.78× slower than the tree-walker (audit Finding 2.1); forwarding is the direct fix. |
| `int_div` | **HIGH** | `(i*7)/(i+1) + (i*13)%(i+2)` — the `i*7` and `i*13` results are stored to frame, then loaded for the div/mod. Forwarding eliminates both reloads. |
| `cse_redundant` | **MEDIUM** | `let a = i*7; s = s + a + a` — `a` is stored to frame, then loaded twice. Forwarding eliminates both loads (the second becomes a Move from the VReg, which the emit keeps in rax). |
| `constprop_fold` | **MEDIUM** | After constprop folds `c = 7`, the `c` is a ConstInt with a frame_off. Forwarding eliminates the reload. But constprop already eliminates the instruction in the best case. |
| `dce_dead_store` | **LOW** | The dead store is removed by DCE, not forwarded. Forwarding doesn't help here. |
| `licm_invariant` | **MEDIUM** | After LICM hoists `100*200`, the result is stored to frame in the pre-header, then loaded in the loop body. Forwarding eliminates the in-loop reload. |
| `call_overhead` | **LOW** | Calls don't store-to-frame in the same way (args go to arg regs). Forwarding doesn't help the call path much. |
| `slice_bounds` / `string_decrypt` / `struct_by_value` | **N/A** | Not ir_safe (Stage A gaps). |

**Analysis requirements: none for intra-block.** The intra-block pass is a linear walk — no dominance, no liveness, no MemorySSA. It uses the same `is_side_effecting` classification that already exists (`ext_opt.cpp:25`). **Inter-block forwarding needs a dominator tree** (§4).

**IR-level or byte-level? IR-level, unambiguously.** The store-to-frame pattern is a structural property of the IR (every instruction with a `frame_off` stores to it; every `LoadFrame` loads from it). At the byte level you'd be pattern-matching `48 89 85 <off>` (store) followed by `48 8B 85 <off>` (load) — fragile, and you can't tell which VReg the load corresponds to. At the IR level, the `StoreFrame.src1` VReg and the `LoadFrame.dst` VReg are explicit. **This is the strongest case for IR-level work in the entire document.**

**The subtlety — when forwarding is unsafe.** A `LoadFrame` from slot X can only forward from the most recent `StoreFrame` to X if:
1. No intervening `StoreFrame` to X (the table handles this — a new store overwrites the entry).
2. No intervening call (`CallNative`/`CallScript`/`CallIndirect`/`CallCrossModule`) — calls may write to frame slots via struct-by-value args or hidden return pointers. **Conservative: bump the generation on any call.** The audit's Finding 6.2 notes calls get depth checks; they don't normally write to *local* frame slots, but struct-by-value args do (`thin_ir.hpp:97-100`), so the conservative rule is correct.
3. No intervening `CopyBytes` or `StructLitInit`/`ArrayLitInit` — these may write to frame slots. The `is_side_effecting` check at `ext_opt.cpp:25` already flags them.
4. The `StoreFrame.src1` VReg is not redefined between the store and the load. **This is the critical check.** If `v3 = StoreFrame v1, off=-24` and then `v1 = Mul v4 v5` and then `v6 = LoadFrame off=-24`, you CANNOT forward `v6 → v1` because `v1` now holds a different value than what was stored. The generation counter handles intervening stores to the *same slot*, but not redefinition of the *source VReg*. **The pass must track VReg redefinition separately** — if `available[X].src` is redefined, erase `available[X]`. This is the same kill rule CSE uses (`ext_opt.cpp:374-382`), applied to the source VReg.

Check 4 is what makes this a 4/10 and not a 2/10. But it's a well-understood pattern — CSE already does it.

---

### 3.2 Instruction Combining (InstCombine)

**What LLVM does.** `InstCombine` (`llvm/lib/Transforms/InstCombine/`) is LLVM's general-purpose peephole-on-IR pass. It is **the** IR-level instruction folder. The structure (traced from `InstructionCombining.cpp` and `InstructionWorklist.h`):

1. **Worklist-driven** (`InstructionWorklist.h:25-130`): seed the worklist with all instructions. Pop one, try to fold it. If the fold changes the instruction, push all its *users* back onto the worklist (`pushUsersToWorkList` at `:106-109`) because they may now fold too. Repeat until the worklist is empty (`InstCombinerImpl::run()` at `InstructionCombining.cpp:4373-4374`).
2. **Per-opcode visitors** (`InstCombineAddSub.cpp`, `InstCombineAndOrXor.cpp`, `InstCombineMulDivRem.cpp`, `InstCombineShifts.cpp`, `InstCombineCasts.cpp`, `InstCombineLoadStoreAlloca.cpp`, etc.): each file implements `visit<Op>` methods that pattern-match a single instruction and try to fold it into a simpler one. Examples: `x + 0 → x`, `x * 1 → x`, `x & 0 → 0`, `(x << C) >> C → x` (with mask), `x - 0 → x`, `x | -1 → -1`.
3. **Multi-instruction folds**: InstCombine looks at the *operands* of the instruction being folded. `x + (x << 1) → x * 3` (a lea-style fold). `((x << C1) >> C2) & Mask` → a mask+shift fold. These are the "combine multiple instructions into one" folds that give the pass its name.
4. **The worklist's deferred set** (`InstructionWorklist.h:73-94`): instructions that become dead (zero uses) are moved to a deferred list and erased in batches — this avoids re-visiting dead instructions.

InstCombine is **huge** — ~15 files, thousands of folds. It is the single largest pass in LLVM's scalar pipeline. Ember does not need all of it.

**How it maps to ember.** Ember's `ConstPropPass` already does the *constant* half of InstCombine (folding `Add(ConstInt 3, ConstInt 4) → ConstInt 7` at `ext_opt.cpp:186-200`). What ember lacks is the *algebraic identity* half — the folds that apply even when operands aren't constants:

- `x + 0 → x` (replace `Add v1 ConstInt(0)` with `Move v1`)
- `x * 1 → x`
- `x * 0 → 0` (replace `Mul v1 ConstInt(0)` with `ConstInt 0`)
- `x & 0 → 0`, `x | 0 → x`, `x ^ 0 → x`
- `x << 0 → x`, `x >> 0 → x`
- `x - x → 0` (replace `Sub v1 v1` with `ConstInt 0`)
- `x | x → x`, `x & x → x`, `x ^ x → 0`
- `Move(Move(x)) → Move(x)` (coalesce chained moves)
- `Cast(Cast(x, T1), T2) → Cast(x, T2)` (when the intermediate is a no-op width)

These are all **one-instruction-in, one-instruction-out** folds (or one-in, zero-out when the result is a Move that store-to-load forwarding then eliminates). They don't need a worklist — a single linear pass per block catches them (and the existing pipeline runs to fixpoint via `run_to_fixpoint` if the host requests it, `PASS_SYSTEM_DESIGN.md §3`).

**The multi-instruction folds** (the `x + (x << 1) → x * 3` shape) are where InstCombine's worklist earns its complexity. These are harder — they require looking at the operand's definition. In ember's non-SSA IR, "the operand's definition" is "the last instruction that wrote to this VReg," which requires a backward scan or a def-map. **Skip these for the first cut.** They're a 6/10 and the bench paths don't exercise them (no `x*3` patterns in the bench).

**Complexity: 3/10 for the identity folds, 6/10 for multi-instruction folds.** The identity folds are a switch statement on `ThinOp` with immediate-form checks (`in.src2 == 0 && in.imm.i == 0` for the zero case). ~60 lines. The multi-instruction folds need a def-map (VReg → last defining instruction index per block) and operand-lookup — ~150 lines plus the worklist.

**Expected impact:**

| bench path | impact | why |
|---|---|---|
| `loop_overhead` | **LOW** | `s = s + i; i = i + 1` — no identity folds apply (no `+0`, `*1`). |
| `int_div` | **LOW** | `(i*7)/(i+1)` — no identity folds. |
| `cse_redundant` | **LOW** | `i*7` — no identity folds. |
| `constprop_fold` | **MEDIUM** | After constprop folds `c = 7`, the `s = s + c` becomes `s = s + 7` — but that's not an identity fold. The identity folds help when the *script* has `x + 0` patterns, which the bench doesn't. |
| all others | **LOW** | The bench paths don't exercise identity-foldable patterns. |

**Honest assessment: low impact on the current bench.** The identity folds are *correctness-preserving infrastructure* — they matter on real scripts that have `x + 0` (e.g. a default parameter added unconditionally, or a `+ 0` from a no-op cast). The bench paths are too clean to show the win. **Implement this because real scripts need it, not because the bench shows it.** It's cheap (3/10) and it makes the pipeline more robust.

**Analysis requirements: none.** The identity folds are local (one instruction). The multi-instruction folds need a def-map (VReg → defining instruction) but not dominance or liveness.

**IR-level or byte-level? IR-level.** The folds are on `ThinOp` semantics (`Add`, `Mul`, etc.), not on x86 byte patterns. A byte-level `add rax, 0` peephole would catch the emitted form, but the IR-level fold catches it *before* emit and may eliminate the instruction entirely (reducing the store-to-frame round-trip too). **IR-level strictly dominates byte-level for these folds.**

---

### 3.3 Constant Propagation Through Memory (MemCpyOpt / load-store forwarding)

**What LLVM does.** `MemCpyOpt` (`llvm/lib/Transforms/Scalar/MemCpyOptimizer.cpp`) is LLVM's memory-copy optimization pass. It uses **MemorySSA** (`:28-29`) to reason about memory dependences. The relevant sub-transforms:

1. **`processStoreOfLoad`** (`:630-756`): when a store stores the value just loaded from another location, and the two locations don't alias in between, convert the load-store pair into a `memcpy`. This is load-store forwarding at the memory-copy granularity.
2. **`processStore`** (`:757-834`): the general store-processing entry point, which calls `processStoreOfLoad` when the stored value is a load (`:778-780`).
3. **`processMemCpyMemCpyDependence`** (`:1121-1207`): coalesce adjacent memcpys.
4. **`writtenBetween`** (`:314-348`): the MemorySSA walker query that proves no write happened between the load and the store.

This pass is **heavier than ember needs.** It operates on `memcpy`/`memmove`/`memset` intrinsics and alloca-to-alloca copies — ember doesn't have these. Ember has `CopyBytes` (which is a fixed-size byte copy between frame slots), but `CopyBytes` is rare (struct/array copies) and not on the hot bench paths.

**What ember actually needs from this pass family is the *constant* variant of store-to-load forwarding** — and that's already partially done. `ConstPropPass` tracks `slot_const` (`ext_opt.cpp:153`): when a `StoreFrame` stores a constant VReg to a slot, the slot is marked constant, and a later `LoadFrame` from that slot marks its dst constant (`ext_opt.cpp:165-176`). This IS constant propagation through memory — it's just intra-block and doesn't use a generation counter.

**The gap:** ember's `slot_const` is intra-block only (`ext_opt.cpp:147`: "Per-block constant tracking (local — no inter-block propagation, safe for loops since the loop body block starts fresh)"). LLVM's MemCpyOpt is inter-block (via MemorySSA). **The inter-block constant propagation through memory is the real gap**, but it's the same dominance requirement as inter-block store-to-load forwarding (§3.1) — defer it until ember has a dominator tree.

**Complexity: 2/10 for the constant-through-memory case (already done as `slot_const` in ConstProp), 8/10 for a MemorySSA-style inter-block version.** The intra-block case is already shipped. The inter-block case needs MemorySSA or a dominator-tree + generation scheme — ember has neither. **Do not build a MemorySSA equivalent for a JIT** (§6).

**Expected impact:** **Already captured by ConstProp's `slot_const`.** The audit's Finding 3.4 shows constprop_fold is the one case where IR+passes catches the tree-walker (39% improvement, `constprop_fold: IR no-passes 2.50M → IR+passes 1.54M`). That's the constant-through-memory pass working. **No additional pass needed for the intra-block case.**

**Analysis requirements: none (intra-block, already shipped); dominator tree or MemorySSA (inter-block, deferred).**

**IR-level or byte-level? IR-level.** The frame slot is an integer key; the constant is a `ThinImm`. There's nothing to do at the byte level — the IR-level pass eliminates the load entirely.

**Verdict: this pass is effectively already shipped as the `slot_const` half of ConstProp.** The research value is in naming the gap (inter-block constant propagation through memory) and confirming it's deferred until the dominator-tree analysis exists. **Do not implement a separate MemCpyOpt pass — extend ConstProp's `slot_const` to inter-block when the dominator tree lands.**

---

### 3.4 Dead Store Elimination (DSE)

**What LLVM does.** LLVM has two DSE passes:

1. **Trivial DSE in EarlyCSE** (`EarlyCSE.cpp:1779-1793`): the `LastStore` mechanism — if two stores to the same location occur with no intervening load, the first store is dead. This is intra-block, no analysis beyond the store tracking.
2. **MemorySSA-backed DSE** (`DeadStoreElimination.cpp`): the heavyweight pass. Uses MemorySSA (`:46-47`), a dominator tree (`:867`), and alias analysis. Finds stores that are overwritten before any read, even across blocks. The `isOverwrite` function (`:932`) classifies whether one store completely overwrites another. The `eliminateDeadStores` walk (`:1458`) follows MemorySSA def-use chains to find killing stores. This is one of the most analysis-heavy passes in LLVM.

**What ember already has.** `DeadCodeElimPass` (`ext_opt.cpp:225-275`) already removes dead `StoreFrame` — a store to a slot that is *never read anywhere in the function* (`compute_read_slots` at `ext_opt.cpp:119-132`, checked at `:244-246`). This is the function-wide "store to a slot nobody loads" case. **This is already more than EarlyCSE's trivial DSE** (which is intra-block only) because ember's `compute_read_slots` scans the whole function.

**What ember lacks** is the *overwritten-before-read* case: `StoreFrame v1, off=-24; StoreFrame v2, off=-24` with no intervening `LoadFrame` from -24. The first store is dead (overwritten before read). Ember's DCE doesn't catch this because `compute_read_slots` finds -24 in the *second* store's... wait — `compute_read_slots` only looks at `LoadFrame`/`CopyBytes`/`FieldAddr` reads (`ext_opt.cpp:121-127`), not at `StoreFrame`. So if -24 is never loaded, both stores are removed. But if -24 *is* loaded later (after the second store), the first store is dead (overwritten) and the second is live — and ember's DCE removes *both* incorrectly? No — ember's DCE only removes a `StoreFrame` if the slot is *never read anywhere* (`:244-246`). So if -24 is read later, neither store is removed, even though the first is dead. **This is the gap: overwritten-before-read dead stores are not eliminated.**

**The ember version (intra-block, no analysis):** a linear walk per block tracking `last_store[slot] → instruction index`. When a second `StoreFrame` to the same slot is seen with no intervening `LoadFrame`/`CopyBytes`/`FieldAddr`/call, the first store is dead. This is EarlyCSE's `LastStore` mechanism (`:1779-1793`) applied to ember's frame slots. ~50 lines.

**Complexity: 3/10 (intra-block overwritten-before-read), 9/10 (MemorySSA-style inter-block).** The intra-block version is a linear walk with a slot→store-index map. The inter-block version needs MemorySSA or a dominator tree + post-dominator tree to prove a store is dead on all paths. **Do intra-block only.**

**Expected impact:**

| bench path | impact | why |
|---|---|---|
| `dce_dead_store` | **MEDIUM** | The bench's dead store (`let dead = i*13`) is already caught by ember's function-wide DCE (the slot is never read). The overwritten-before-read case would help if the bench had `x = 1; x = 2;` — it doesn't. |
| `loop_overhead` | **LOW** | `s = s + i` — the store to `s` is read next iteration (the load of `s`). Not dead. |
| `int_div` | **LOW** | No overwritten-before-read pattern. |
| `struct_by_value` | **N/A** | Not ir_safe. |

**Honest assessment: low impact on the current bench.** The bench's dead-store path is already handled by the function-wide DCE. The overwritten-before-read case matters on real scripts with reassignment (`x = 1; if (cond) { x = 2; } use(x)` — the `x = 1` is dead on the `cond` path but live on the `!cond` path, so this actually needs the inter-block version). **Implement the intra-block version as infrastructure; the inter-block version is YAGNI until the bench shows a need.**

**Analysis requirements: none (intra-block); dominator + post-dominator (inter-block, deferred).**

**IR-level or byte-level? IR-level.** The dead store is a `StoreFrame` instr; eliminating it at the IR level removes the `store_rax_to_rbp` byte emission entirely. At the byte level you'd be matching a store followed by a store to the same offset — fragile and you can't tell if a load happens in between without a full byte disassembly.

---

### 3.5 Branch Folding / Unreachable Block Elimination

**What LLVM does.** Three related passes:

1. **`removeUnreachableBlocks`** (`llvm/lib/Transforms/Utils/Local.cpp:3162-3192`): a utility called by `SimplifyCFGPass` (`SimplifyCFGPass.cpp:270, 283, 288`). It does a BFS from the entry block (`markAliveBlocks` at `:2926-2960`), marks all reachable blocks, then deletes any block not in the reachable set (`DeleteDeadBlocks` at `:3186`). This is the "blocks that can never execute" pass.
2. **`JumpThreading`** (`llvm/lib/Transforms/Scalar/JumpThreading.cpp`): when a branch condition is a constant or can be proven constant on a given path (`:538-914`), thread the edge — replace the conditional branch with an unconditional jump to the known target. This is "branch folding" in the sense of folding a known-condition branch.
3. **`ADCE`** (`llvm/lib/Transforms/Scalar/ADCE.cpp`): Aggressive Dead Code Elimination. Uses post-dominator + control-dependence analysis (`:182`: `markLiveBranchesFromControlDependences`) to remove branches that are *not* control-dependent on any live instruction. This removes unreachable *code* (not just blocks) — a branch where neither target does anything live.

**How it maps to ember.** Ember's `ThinTerm` has `Branch` with `cond` (a VReg) + `target` + `false_target` (`thin_ir.hpp:166-172`). The branch-folding case: if `cond` is a known constant (from ConstProp's `vreg_const` table), the `Branch` can be replaced with a `Jmp` to the known target, and the now-unreachable other target block can be deleted.

**ConstProp already folds constant conditions** — but only within a block (it doesn't rewrite terminators). The lowering (`thin_lower.cpp`) emits `Branch` with a condition VReg; if that VReg is a `ConstBool`, the branch is statically determined. **The pass ember needs is a "constant branch folding" pass**: walk all blocks, if a `Branch`'s `cond` is a `ConstBool` instr (or a VReg known-constant from ConstProp), replace the `Branch` with a `Jmp` and mark the dead target unreachable. Then a dead-block sweep (the `removeUnreachableBlocks` shape) deletes the unreachable blocks.

**The algorithm:**

```
// Phase 1: fold constant branches
for each block:
    if term is Branch(cond=vC):
        find the def of vC (backward scan or def-map)
        if def is ConstBool(true):  term = Jmp(target)
        if def is ConstBool(false): term = Jmp(false_target)
        changed = true

// Phase 2: remove unreachable blocks (the markAliveBlocks BFS)
reachable = BFS from blocks[0] following Jmp/Branch targets
for each block not in reachable:
    erase it (and fix up any Jmp/Branch that targeted it — but those
    should already be folded or dead)
```

**Complexity: 4/10.** The constant-branch fold is a backward scan for the cond's def (or a pre-built def-map). The unreachable-block sweep is a BFS + erase pass. ~100 lines total. No dominance needed — just reachability from entry.

**Expected impact:**

| bench path | impact | why |
|---|---|---|
| all 6 original + 4 IR-pass paths | **NONE** | None of the bench paths have constant branches or unreachable blocks. They're straight-line loops with a single `while (i < N)` back-edge. |
| real scripts with `if (false)` / dead code | **MEDIUM** | Scripts with compile-time-known conditions (`if (DEBUG)`, `if (const_flag)`) benefit. The bench doesn't exercise this. |

**Honest assessment: zero impact on the current bench, medium impact on real scripts.** This is infrastructure for scripts with dead branches. The bench paths are all single-loop, no-branch (except the loop back-edge, which is not constant-foldable because `i < N` is not constant). **Implement this when the bench adds a path with dead branches, or when real scripts show dead-block overhead.** It's cheap (4/10) and correct, but it's not a performance win on the measured set.

**Analysis requirements: none.** Reachability from entry is a BFS, not dominance. Constant-branch folding needs the cond's def, which is a backward scan or a def-map (same as InstCombine's multi-instruction folds).

**IR-level or byte-level? IR-level.** The branch is a `ThinTerm`; the constant is a `ConstBool` instr. At the byte level you'd be matching a `cmp; jcc` sequence where the cmp's operand is an immediate — fragile. IR-level is clean.

---

### 3.6 Copy Propagation

**What LLVM does.** **There is no standalone "CopyProp" pass in modern LLVM.** Copy propagation is folded into EarlyCSE and GVN:

- **EarlyCSE** handles copies via the `AvailableValues` table (`EarlyCSE.cpp:1521-1535`): when a `Copy`/`Move`/`BitCast` of a value is seen, the copy is registered as available, and later uses of the copy are replaced with the original (`replaceAllUsesWith` at `:1530`). This IS copy propagation — "replace uses of a copied value with the original."
- **GVN** does the same via value numbering (`GVN.cpp`): two instructions with the same value number are CSE'd, which subsumes copy propagation (a copy has the same value number as its source).

The legacy `CopyPropPass` was removed from LLVM years ago because EarlyCSE/GVN subsume it. **Standalone copy propagation is obsolete in LLVM's pipeline.**

**How it maps to ember.** Ember's `Move` op (`thin_ir.hpp:74`) is the copy instruction: `dst = Move src1`. Copy propagation replaces later uses of `dst` with `src1` (until `src1` is redefined). This is distinct from CSE (which finds *redundant computations*) — copy propagation finds *redundant moves*.

**ConstProp already does partial copy propagation** (`ext_opt.cpp:158-164`): `Move dst=vN src1=vM` propagates the constant through (if `vM` is constant, `vN` becomes constant). But it doesn't do the *non-constant* copy propagation: replacing uses of `vN` with `vM` when neither is constant.

**CSE does NOT subsume copy propagation in ember** the way it does in LLVM, because ember's CSE keys on `(op, src1, src2, imm, ...)` (`ext_opt.cpp:343-357`) — a `Move vM` and a `Move vN` with the same source have the same key, so CSE *would* coalesce them. But CSE is intra-block and has the O(n²) remap scan (Finding 3.2). A dedicated copy-propagation pass is simpler and cheaper.

**The ember version (intra-block, no analysis):** a linear walk per block tracking `copy_map[VReg dst] → VReg src`. When a `Move dst=vN src1=vM` is seen, record `copy_map[vN] = vM`. When a later instruction uses `vN` as `src1`/`src2`/`args`, replace it with `vM` (unless `vM` has been redefined in between — the same kill rule as CSE). When `vN` or `vM` is redefined, erase the entry. ~70 lines.

**Complexity: 3/10.** A linear walk with a VReg→VReg map and the standard redefinition kill rule. Simpler than CSE (no hash-key construction, no remap scan) because the map is keyed on a single VReg, not a compound key.

**Expected impact:**

| bench path | impact | why |
|---|---|---|
| `loop_overhead` | **LOW** | `s = s + i; i = i + 1` — no Moves in the hot loop (the lowering emits Add directly, not Move-then-Add). |
| `cse_redundant` | **LOW** | `let a = i*7; s = s + a + a` — `a` is a Mul result, not a Move. |
| all paths | **LOW** | The bench paths don't have Move chains. |

**Honest assessment: low impact on the bench, but high value as a *cleanup* pass.** Copy propagation's real value is cleaning up after *other* passes. Store-to-load forwarding (§3.1) rewrites `LoadFrame dst=vD off=X` to `Move dst=vD src1=vN` — and then copy propagation replaces uses of `vD` with `vN`, eliminating the Move entirely. **Copy propagation is the partner pass to store-to-load forwarding.** Run forwarding first, then copy propagation, then DCE to remove the dead Moves. This is the pipeline:

```
store-to-load forwarding  →  creates Moves
copy propagation          →  eliminates the Moves (replaces uses with the source VReg)
DCE                       →  removes the now-dead Move instrs
```

**Analysis requirements: none (intra-block).** The redefinition kill rule is local. Inter-block copy propagation needs dominance (a copy in a dominating block is available in dominated blocks) — defer.

**IR-level or byte-level? IR-level.** The copy is a `Move` instr with explicit `src1` and `dst` VRegs. Byte-level copy propagation would mean matching `mov rax, r10` and then replacing later `rax` uses with `r10` — but the emit doesn't keep values in fixed registers, so there's nothing to propagate at the byte level. **IR-level.**

---

### 3.7 Strength Reduction

**What LLVM does.** Two passes:

1. **`StraightLineStrengthReduce`** (`llvm/lib/Transforms/Scalar/StraightLineStrengthReduce.cpp`): rewrites straight-line multiply-add chains to use shifts and LEA-style adds. E.g. `x * 3 → (x << 1) + x`, `x * 5 → (x << 2) + x`. The `Candidate` class (`:127`) tracks multiply candidates and their "basis" (a cheaper form with the same result). The `rewriteCandidateWithBasis` (`:613`) emits the reduced form. This is the **straight-line** strength reducer — no loop needed.
2. **`LoopStrengthReduce`** (`llvm/lib/Transforms/Scalar/LoopStrengthReduce.cpp`): the heavyweight loop strength reducer. Rewrites induction variables and their derived expressions to use cheaper forms (e.g. replacing a multiply inside a loop with an add outside). Uses `IVUsers` (`:71`), `ScalarEvolution`, `Loop` info, `DominatorTree`, `TargetTransformInfo` — it is one of the most analysis-dependent passes in LLVM. **This is NOT appropriate for a JIT** (§6).

**How it maps to ember.** The relevant case is **straight-line strength reduction** — replacing `Mul v1 ConstInt(2)` with `Shl v1 ConstInt(1)`, `Mul v1 ConstInt(4)` with `Shl v1 ConstInt(2)`, etc. (powers of two). The general `x * 3 → (x << 1) + x` is more complex and rarely wins on modern CPUs (a single `imul` is 3-cycle latency, 1/cycle throughput on Skylake — the `(x<<1)+x` form is 2 instructions but the same or worse latency due to the dependent add).

**The power-of-two multiply → shift fold is the one that matters.** `Mul v1, imm=2` → `Shl v1, imm=1`. `Mul v1, imm=4` → `Shl v1, imm=2`. This is a one-instruction fold (same shape as the InstCombine identity folds in §3.2). The emit already has `imul` for Mul and `shl` for Shl — the fold replaces one with the other.

**But — does this actually help ember?** The emit for `Mul` with an immediate (`thin_emit.cpp:1393-1395`):
```
e.mov_reg_imm64(Reg::rcx, imm);    // 10 bytes
e.imul_reg_reg(Reg::rax, Reg::rcx); // 3 bytes
```
vs `Shl` with an immediate (`thin_emit.cpp:1399-1400`):
```
e.mov_reg_imm64(Reg::rcx, imm);    // 10 bytes
e.byte(0x48); e.byte(0xD3); e.byte(0xE0); // shl rax,cl — 3 bytes
```
**Same byte count!** The `imul` and `shl` are both 3 bytes, and both need the immediate in `cl` (the emit doesn't use the `shl rax, imm8` short form for Shl — it goes through `rcx`). So the power-of-two fold saves **zero bytes** with the current emit. The latency is also the same (both go through the `mov rcx, imm` → `op rax, rcx` dependency chain).

**Strength reduction would only help if the emit is fixed to use the `shl rax, imm8` short form** (3 bytes, no `rcx` setup) for constant shifts. That's a byte-level emit improvement, not an IR pass. **The IR pass produces the same bytes as the Mul because the emit doesn't specialize Shl-by-constant.**

**Complexity: 2/10 for the power-of-two fold (IR pass), 1/10 for the shl-by-imm8 short form (emit fix).** The IR pass is a 20-line switch. The emit fix is changing the Shl/Shr case in `emit_int_binop` to check `in.src2 == 0 && in.imm.i >= 0 && in.imm.i <= 63` and emit `48 C1 E0 imm8` (shl rax, imm8) instead of the `mov rcx; shl rax, cl` form.

**Expected impact:**

| bench path | impact | why |
|---|---|---|
| `int_div` | **LOW (IR pass), MEDIUM (emit fix)** | `i*7` and `i*13` are not powers of two. The IR fold doesn't help. But the emit fix (shl-by-imm8) would help any `<<` in the code — the bench doesn't have shifts. |
| `cse_redundant` | **NONE** | `i*7` — not a power of two. |
| all paths | **NONE to LOW** | No bench path has a power-of-two multiply or a shift. |

**Honest assessment: the IR pass is nearly useless with the current emit; the emit fix is the real win.** The emit fix (shl-by-imm8 short form) is a 1/10 change in `thin_emit.cpp` that saves 10 bytes per constant shift and breaks a dependency chain. **Do the emit fix, not the IR pass.** If the emit fix is done, the IR pass becomes a 2-byte save (the `imul` vs `shl` opcode) — marginal.

**The general strength reduction (`x*3 → (x<<1)+x`)** is **not worth it** on modern x86. `imul` is fast enough that the 2-instruction shift-add form is not a win (it's often a loss due to the dependent add). LLVM's StraightLineStrengthReduce targets architectures where multiply is expensive; x86 is not one of them. **Skip it.**

**Analysis requirements: none (IR pass, it's a local fold); none (emit fix, it's a byte-level specialization).**

**IR-level or byte-level? The emit fix is byte-level.** The IR pass is IR-level but produces no benefit without the emit fix. **Do the byte-level emit fix in `thin_emit.cpp`'s Shl/Shr case** — emit `48 C1 E0 imm8` for `shl rax, imm8` when the shift amount is a known constant 0–63.

---

### 3.8 Peephole on the IR (not bytes)

**What LLVM does.** **InstCombine IS the IR peephole pass.** There is no separate "IR peephole" pass in LLVM — InstCombine serves that role (§3.2). The term "peephole" in LLVM usually refers to *machine-level* peephole passes (`llvm/lib/Target/X86/X86Peephole.cpp`) that operate on `MachineInstr`s after register allocation. Those are byte-level (post-emit, in ember's terms).

**What ember already has at the byte level:** `peephole.cpp` ships `SmartImmPass` (shrinks `mov r64, imm64` to `mov eax, imm32` or `mov r64, imm32` when the value fits, `peephole.cpp:67-130`) and `SetccMovzxPass` (inert no-op in Stage 1 because the `xor; setcc` rewrite clobbers flags, `peephole.cpp:155-170`). These are byte-level, post-emit, in-place rewrites with NOP padding (`peephole.cpp:14-20`).

**What "peephole on the IR" means for ember** is exactly §3.2 (InstCombine's identity folds) plus §3.6 (copy propagation) plus §3.7 (strength reduction's power-of-two fold). These are all IR-level pattern-matches on `ThinOp` sequences. **There is no separate "IR peephole" pass to write — it's the collection of the folds in §3.2, §3.6, and §3.7.**

**The question is whether to combine them into one pass or keep them separate.** LLVM keeps them in one pass (InstCombine) because the worklist lets one fold expose another (folding `x + 0 → x` exposes `x * 1 → x` on the same instruction). Ember's pass system supports a fixpoint loop (`run_to_fixpoint` in `PASS_SYSTEM_DESIGN.md §3`), so separate passes run in sequence until stable achieve the same effect — at the cost of more full-function traversals.

**Recommendation: keep them separate for the first cut.** A single `PeepholePass` that does identity folds + copy propagation + power-of-two strength reduction is tempting, but it conflates three concerns. Ember's pass system is designed for composable small passes (`PASS_SYSTEM_DESIGN.md §1`: "passes that are 5-line structs"). **Ship `InstCombinePass` (identity folds), `CopyPropPass`, and the emit fix for shifts separately.** If the fixpoint loop is too expensive (too many traversals), combine them later.

**Complexity: N/A — this is §3.2 + §3.6 + §3.7, not a separate pass.** If you do build a combined `PeepholePass`, it's a 5/10 (the worklist adds complexity over separate linear passes).

**Expected impact: the sum of §3.2 + §3.6 + §3.7.** Low on the current bench, medium on real scripts.

**Analysis requirements: none (the folds are local).**

**IR-level or byte-level? IR-level for the folds; the byte-level peephole (`peephole.cpp`) is already shipped and is the right place for SmartImm-style byte shrinks.** The two are complementary: the IR-level peephole eliminates instructions (reducing emit work); the byte-level peephole shrinks the emitted bytes (reducing code size + icache pressure). **Run both.**

---

## 4. The Analysis Problem — What Ember Doesn't Have Yet

Several passes above have an **intra-block version** (no analysis needed, implementable today) and an **inter-block version** (needs dominance/liveness, deferred). The split:

| pass | intra-block (today) | inter-block (needs analysis) |
|---|---|---|
| Store-to-load forwarding (§3.1) | ✅ 4/10, ~80 lines | needs dominator tree |
| Constant prop through memory (§3.3) | ✅ already shipped (`slot_const`) | needs dominator tree |
| Dead store elimination (§3.4) | ✅ 3/10, ~50 lines | needs dominator + post-dominator |
| Copy propagation (§3.6) | ✅ 3/10, ~70 lines | needs dominator tree |
| Branch folding (§3.5) | ✅ 4/10, ~100 lines (reachability, not dominance) | — |
| InstCombine (§3.2) | ✅ 3/10 identity folds | 6/10 multi-instruction folds (need def-map, not dominance) |

**The pattern:** the intra-block versions are all cheap (2–4/10) and need no analysis. The inter-block versions all need a dominator tree. **The dominator tree is the single analysis that unlocks the most value.**

**What ember has today:**
- `LICMPass` builds its own predecessor map + back-edge detection on the fly (`ext_opt.cpp:456-505`) — this is a local CFG analysis, not cached.
- `ConstPropPass` and `DeadCodeElimPass` use `compute_used_vregs` and `compute_read_slots` — these are function-wide *scans*, not analyses (they recompute every time, which is the O(n²) problem in Finding 3.1).
- `EmberAnalysisManager` is a stub (`PASS_SYSTEM_DESIGN.md §6`: "empty class — passes take `EmberAnalysisManager&` but don't request analyses yet").

**What ember should build first (the analysis foundation):**

1. **A dominator tree.** This is the highest-value analysis. It unlocks inter-block store-to-load forwarding, inter-block copy propagation, inter-block constant propagation through memory, and it makes LICM's loop detection cheaper (cache the back-edge analysis). The algorithm: Cooper-Harvey-Kennedy iterative dominators (the standard simple algorithm, ~50 lines for a function with <100 blocks). Store it as a `ThinDominatorTree` analysis result in `EmberAnalysisManager`.
2. **A def-map.** A `VReg → (block index, instr index)` map of the last definition of each VReg. This is what InstCombine's multi-instruction folds and branch folding need to find "the instruction that produced this VReg." It's a single forward walk per function — O(n) to build, O(1) to query. Store it as a `ThinDefMap` analysis result.
3. **A use-set.** The `compute_used_vregs` function (`ext_opt.cpp:103-118`) already computes this, but it recomputes every time. Cache it as a `ThinUseSet` analysis. This fixes ConstProp's O(n²) re-scan (Finding 3.1) — the pass requests the analysis once, and on removal, updates it incrementally (or marks it invalidated and lets the next pass recompute).

**The `EmberAnalysisManager` stub** (`PASS_SYSTEM_DESIGN.md §6`) is designed for exactly this: `getResult<AnalysisT>(f)` is lazy + cached, `getCachedResult<AnalysisT>(f)` peeks, `invalidate(f, preserved)` drops results the preservation set doesn't cover. The infrastructure is specified; it just needs the first three concrete analyses.

**Recommendation: build the dominator tree first.** It's the single analysis that unlocks the most inter-block pass value. The def-map and use-set are cheaper but less impactful (they fix O(n²) compile-time, not runtime). The dominator tree is a 5/10 to implement (Cooper-Harvey-Kennedy is well-documented) and it's the foundation for the inter-block versions of three of the eight passes above.

---

## 5. Recommended Implementation Order

Ordered by **impact-per-effort**, gated on the bench (DESIGN §9: "no speculative optimization before the bench proves it matters"):

| priority | pass | complexity | bench impact | analysis needed | why this order |
|---|---|---|---|---|---|
| **1** | Store-to-load forwarding (§3.1, intra-block) | 4/10 | **HIGH** on loop_overhead, int_div, cse_redundant, licm_invariant | none | The direct fix for audit Finding 2.1 (IR backend 1.2–1.9× slower than tree-walker). Eliminates the load half of the frame round-trip. The `rax_vreg` cache in the emit already keeps the result in rax — forwarding makes the IR use it. **Highest impact-per-effort in the entire doc.** |
| **2** | Copy propagation (§3.6, intra-block) | 3/10 | LOW standalone, **HIGH as cleanup** for #1 | none | The partner pass to store-to-load forwarding. Forwarding creates Moves; copy propagation eliminates them; DCE removes the dead instrs. Without this, forwarding leaves Moves that still store-to-frame. |
| **3** | Emit fix: shl-by-imm8 short form (§3.7) | 1/10 | MEDIUM on shift-heavy code | none (byte-level) | A 10-line change in `thin_emit.cpp` that saves 10 bytes per constant shift and breaks a dependency chain. Not an IR pass — an emit improvement. Do it because it's nearly free. |
| **4** | InstCombine identity folds (§3.2) | 3/10 | LOW on bench, MEDIUM on real scripts | none | `x+0→x`, `x*1→x`, `x*0→0`, `x-x→0`, etc. Cheap infrastructure that makes the pipeline robust on real scripts (the bench is too clean to show the win). |
| **5** | Dead store elimination (§3.4, intra-block overwritten-before-read) | 3/10 | LOW on bench, MEDIUM on real scripts | none | The overwritten-before-read case that ember's function-wide DCE misses. Cheap; pairs with #1 (forwarding may make stores dead). |
| **6** | Branch folding + unreachable block elimination (§3.5) | 4/10 | NONE on bench, MEDIUM on real scripts | none (reachability) | Constant-branch folding + dead-block BFS. Zero bench impact; infrastructure for scripts with dead branches. |
| **7** | Dominator tree analysis (§4) | 5/10 | enables inter-block versions of #1, #2, #5 | — | The analysis foundation. Build this when the intra-block passes are stable and the bench shows inter-block forwarding would help (add a path with a store in a dominating block and a load in a dominated block). |
| **8** | Inter-block store-to-load forwarding (§3.1, with dominance) | 7/10 | HIGH on inter-block store-load paths | dominator tree | The inter-block version of #1. Needs the dominator tree from #7. |

**The pipeline string for #1–#2 (the headline pair):**
```
constprop,forward,copyprop,dce
```
Where `forward` is the new store-to-load forwarding pass and `copyprop` is the new copy propagation pass. Constprop first (folds constants, which makes some loads trivially forwardable), then forwarding (eliminates frame reloads), then copy propagation (eliminates the Moves forwarding created), then DCE (removes the dead instrs).

**What NOT to do first:** strength reduction (§3.7, the IR pass produces no benefit without the emit fix), MemCpyOpt (§3.3, already shipped as `slot_const`), LoopStrengthReduce (§6, YAGNI for a JIT), and the inter-block versions of any pass before the dominator tree (#7) exists.

---

## 6. What NOT to Port (YAGNI for a JIT)

LLVM has many passes that are **not appropriate for ember's JIT** — they're either too analysis-heavy for compile-time-sensitive compilation, they target problems ember doesn't have, or they're subsumed by simpler passes. Documenting these so nobody wastes time porting them:

| LLVM pass | why NOT for ember |
|---|---|
| **`LoopStrengthReduce`** (`LoopStrengthReduce.cpp`) | Needs `ScalarEvolution`, `IVUsers`, `Loop`, `DominatorTree`, `TargetTransformInfo`. ~10,000 lines. The most analysis-dependent pass in LLVM. Targets loop induction-variable rewriting (replacing multiplies with adds) — ember's loops are simple `while (i < N)` with no derived IVs. **YAGNI for a JIT.** |
| **`NewGVN`** (`NewGVN.cpp`) | Full value numbering with MemorySSA. ~4,000 lines. EarlyCSE's intra-block forwarding + CSE covers 80% of the win at 10% of the complexity. **Defer until the bench shows CSE misses are a problem.** |
| **`GVN`** (`GVN.cpp`) | Legacy GVN with load PRE (partial redundancy elimination). Needs MemorySSA + dominator tree + alias analysis. The PRE part (hoisting loads to dominating blocks) is inter-block store-to-load forwarding — §3.1's inter-block version covers it. **Defer.** |
| **`MemCpyOpt`** (`MemCpyOptimizer.cpp`) | Needs MemorySSA. Targets `memcpy`/`memmove`/`memset` coalescing — ember has no memcpy intrinsics (only `CopyBytes` for struct/array copies, which are rare). **The constant-through-memory part is already shipped as ConstProp's `slot_const`.** |
| **`ADCE`** (`ADCE.cpp`) | Needs post-dominator + control-dependence analysis. Removes dead *control flow* (branches where neither target does anything live). Ember's branch folding (§3.5) + DCE covers the practical cases. **Defer until the bench shows dead-control-flow overhead.** |
| **`JumpThreading`** (`JumpThreading.cpp`) | Needs `LazyValueInfo` + `DominatorTree`. Threads edges across multiple blocks when a condition is known on a path. Ember's constant-branch folding (§3.5) is the single-block version. **Defer the multi-block threading until the dominator tree exists.** |
| **`SROA`** (Scalar Replacement of Aggregates) | Breaks alloca'd structs into scalar SSA values. Ember has no alloca — struct temps are frame slots by design (`thin_ir.hpp:85-88`). **Not applicable.** |
| **`MergedLoadStoreMotion`** (`MergedLoadStoreMotion.cpp`) | Merges loads/stores across diamond CFGs. Needs dominance. **Defer until inter-block forwarding is justified.** |
| **`CorrelatedValuePropagation`** (`CorrelatedValuePropagation.cpp`) | Uses `LazyValueInfo` to propagate constant ranges through branches. Needs LVI. **YAGNI — ember has no range analysis.** |
| **`LoopUnrollPass`** | Unrolls loops. **For a JIT, unrolling is a compile-time/runtime tradeoff that needs a heuristic** (trip count, code size budget). Defer until the bench shows loop overhead is dominated by the back-edge (it's not — audit Finding 1.1 says the body's push/pop spill dominates). |
| **`SLPVectorizer`** / **`LoopVectorizePass`** | Auto-vectorization. Ember's IR is scalar three-address — no vector types. **Not applicable.** |

**The principle:** LLVM's passes are designed for AOT whole-program compilation with no compile-time pressure. Ember is a JIT — **compile time is a user-visible cost** (the audit measures `compile_ns` per path). Every pass added to the pipeline pays a compile-time tax. The intra-block passes in §5 (#1–#6) are all O(n) per block — cheap. The inter-block passes and the analysis-heavy LLVM passes above are O(n²) or worse in some cases, or they need analyses that are themselves O(n log n) to build. **Only add a pass when the bench proves the runtime win exceeds the compile-time tax.**

---

## 7. Cross-References

**Ember source (the IR the passes operate on):**
- `src/thin_ir.hpp` — `ThinFunction`, `ThinBlock`, `ThinInstr`, `ThinOp`, `ThinMeta`, `ThinTerm`. The IR shape (§2).
- `src/thin_emit.cpp:80-82` (`store_rax_to_rbp`), `:364-391` (`record_dst`), `:315-329` (`load_int_vreg`), `:123` (`rax_vreg`), `:1366-1448` (`emit_int_binop`) — the store-to-frame round-trip that store-to-load forwarding (§3.1) addresses (audit Finding 2.1).
- `extensions/opt/ext_opt.cpp` — the 4 existing opt passes: `ConstPropPass` (`:139-275`), `DeadCodeElimPass` (`:225-275`), `CSEPass` (`:281-450`), `LICMPass` (`:456-611`). The `is_side_effecting`/`is_pure` classification (`:25-41`) that all memory passes build on.
- `extensions/obf/ext_obf.cpp` — `SubstitutionPass` (the 5th pass, MBA obfuscation).
- `src/peephole.cpp` — the byte-level peephole (`SmartImmPass`, `SetccMovzxPass`). The byte-level complement to the IR-level folds in §3.2/§3.8.

**Ember docs (the design basis):**
- `docs/LLVM_PASS_SYSTEM_RESEARCH.md` — the companion research (LLVM's pass *system* architecture: PassManager, PreservedAnalyses, AnalysisManager, instrumentation, adaptors, Pluto/Hikari obfuscation patterns). This doc studies the *passes themselves*; that doc studied the *machinery*.
- `docs/spec/PASS_SYSTEM_DESIGN.md` — ember's pass architecture design (the `EmberPassManager`, `EmberPassRegistry`, `EmberPassInfoMixin`, `EmberAnalysisManager` stub at §6, the `run_to_fixpoint` loop at §3).
- `docs/audit/PERFORMANCE_AUDIT_2026-07-11.md` — the performance basis. Finding 2.1 (IR backend uniformly slower, the store-to-frame round-trip), Finding 3.1 (ConstProp O(n²) re-scan), Finding 3.2 (CSE O(n²) table scan), Finding 1.1 (tree-walker push/pop spill), Recommendations 1–5.
- `bench/bench_codegen_paths.cpp` — the 10 bench paths (6 original + 4 IR-pass workloads, `make_paths` at the bottom). The `EMBER_IR_PASS` env var that builds the pipeline from a string.

**LLVM 18.1.8 source (the pass implementations studied):**
- `llvm/lib/Transforms/Scalar/EarlyCSE.cpp` — store-to-load forwarding (§3.1). `AvailableLoads` table (`:695-700`), store insertion (`:1771-1772`), load replacement (`:1557-1571`), generation counter (`:1370`, `:1764`), `LastStore` DSE (`:1779-1793`), writeback DSE (`:1687-1711`), dominator-tree walk (`:1751-1770`).
- `llvm/lib/Transforms/InstCombine/InstructionCombining.cpp` + `InstructionWorklist.h` — InstCombine (§3.2). Worklist (`InstructionWorklist.h:25-130`), `run()` loop (`:4373-4374`), per-opcode visitors (`InstCombineAddSub.cpp`, `InstCombineAndOrXor.cpp`, `InstCombineMulDivRem.cpp`, `InstCombineShifts.cpp`, `InstCombineCasts.cpp`).
- `llvm/lib/Transforms/Scalar/MemCpyOptimizer.cpp` — MemCpyOpt (§3.3). `processStoreOfLoad` (`:630-756`), `processStore` (`:757-834`), `writtenBetween` (`:314-348`), MemorySSA usage (`:28-29`).
- `llvm/lib/Transforms/Scalar/DeadStoreElimination.cpp` — DSE (§3.4). `DSEState` (`:867`), `isOverwrite` (`:932`), MemorySSA-backed walk (`:1458`).
- `llvm/lib/Transforms/Utils/Local.cpp` — `removeUnreachableBlocks` (`:3162-3192`), `markAliveBlocks` (`:2926-2960`). §3.5.
- `llvm/lib/Transforms/Scalar/JumpThreading.cpp` — JumpThreading (§3.5). `run()` (`:238`), constant-condition threading (`:538-914`).
- `llvm/lib/Transforms/Scalar/ADCE.cpp` — ADCE (§3.5). `AggressiveDeadCodeElimination` (`:120`), `markLiveBranchesFromControlDependences` (`:182`).
- `llvm/lib/Transforms/Scalar/StraightLineStrengthReduce.cpp` — SLSR (§3.7). `Candidate` (`:127`), `rewriteCandidateWithBasis` (`:613`).
- `llvm/lib/Transforms/Scalar/LoopStrengthReduce.cpp` — LSR (§3.7, the one NOT to port). `:71` (`IVUsers`), the analysis dependency list.
- `llvm/lib/Transforms/Scalar/GVN.cpp` + `NewGVN.cpp` — GVN (§6, defer). `processNonLocalLoad` (`:1837`), `AvailableValue` (`:190`).
- `llvm/lib/Transforms/Scalar/SimplifyCFGPass.cpp` — SimplifyCFG (§3.5). `removeUnreachableBlocks` call (`:270`, `:283`, `:288`).

---

*Research complete. No ember source files edited. Every pass assessed for applicability to naive three-address TAC, implementability as an IR→IR transform over `ThinFunction`, compile-time cost, bench-path impact, and analysis requirements. The headline recommendation: store-to-load forwarding (§3.1) is the single highest-impact pass for ember's IR backend, directly addressing audit Finding 2.1. Build it first, with copy propagation as its cleanup partner.*
