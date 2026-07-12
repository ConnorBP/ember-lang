# Plan — IR obfuscation passes and user-authored custom-pass examples

> **Status: research / planning only (2026-07-12).** No pass is implemented by
> this document. The implementation targets below are deliberately separated
> from the current shipped behavior.
>
> **Scope:** `ThinFunction` IR transforms, pass registration, deterministic
> seed handling, post-pass validation, and `examples/custom_pass/`.
>
> **Primary sources read:** `src/thin_ir.hpp`, `extensions/obf/ext_obf.{hpp,cpp}`,
> `extensions/opt/ext_opt.cpp`, `src/ember_pass.hpp`,
> `src/ember_pass_pipeline.hpp`, `src/ember_pass_registry.hpp`, and
> `docs/spec/PASS_SYSTEM_DESIGN.md`. Serialization/emission and existing tests
> were also inspected where they constrain a safe plan.

---

## 1. Executive recommendation

Implement the work in four layers rather than adding six independent loops over
`ThinFunction`:

1. **Pass-authoring foundations:** a shared IR mutation helper, a public and
   conservative instruction-effect classifier, deterministic seed plumbing,
   post-pass verification, and explicit growth limits.
2. **Local transforms:** constant encoding, expanded MBA substitution, and
   pure junk instruction injection. These do not initially need CFG surgery.
3. **CFG transforms:** block splitting, edge trampolines/indirect jumps, opaque
   predicates, and bogus blocks. These share predecessor/successor utilities
   and block-ID canonicalization.
4. **String integration:** convert `ConstStringRef` to `StringDecrypt`, rebuild
   rodata without plaintext, and make the complete decrypt metadata
   serializable. This must not ship until the serializer issue in §2.5 is
   resolved.

The user examples should be built after layer 1 and before the production
passes. They will serve as executable documentation for the same mutation,
validation, side-effect, serialization, and seed contracts the production
passes use.

Recommended production pass names:

| Pass | Pipeline name | Initial scope |
|---|---|---|
| Opaque predicates / bogus control flow | `opaque` | Always-true/false integer predicates plus semantically inert rejoining blocks |
| Dead code and bogus block injection | `deadinject` | Pure integer junk only; no calls, traps, or externally visible stores |
| Expanded arithmetic/MBA substitution | `mba` | `Add`, `Sub`, `And`, `Or`, `Xor`, and bounded `Mul` variants |
| Block splitting and branch redirection | `split-redirect` | Splits, edge trampolines, then symbolic-target indirect machine jumps |
| Constant encoding | `constenc` | Integer/bool constants and explicitly supported arithmetic immediates |
| IR string-literal encryption | `strenc` | `ConstStringRef` to stack-decrypting `StringDecrypt`; no plaintext left in rodata |

Keep the shipped `subst` name as a compatibility alias for the present
Add-only substitution pass during migration. Once `mba` has equivalent test
coverage, either make `subst` an alias for `mba` or deprecate it explicitly;
do not silently leave two subtly different implementations indefinitely.

---

## 2. What the current implementation establishes

### 2.1 Pass lifecycle today

A pass is a concrete type with:

```cpp
struct ExamplePass : EmberPassInfoMixin<ExamplePass> {
    static constexpr const char* pass_name = "example";
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&);
};
```

`PassModel<T>` type-erases it, `EmberPassRegistry` maps a string to a
fresh default-constructed instance, and `EmberPassManager` runs passes in
pipeline order. `EmberPreserved::all()` means no mutation; `none()` means all
analyses are invalid. The analysis manager is currently an empty stub.

Obfuscation passes may set `static constexpr bool is_required = true`. This
bypasses instrumentation skip gates. It does **not** provide randomness,
configuration, validation, or a compile-time growth budget.

### 2.2 Existing obfuscation pattern

`SubstitutionPass` in `extensions/obf/ext_obf.cpp` is the reference pattern:

- scan the function to find the next free VReg;
- allocate new frame slots for new intermediate VRegs;
- preserve the original destination, type, width, frame slot, and location;
- insert three instructions before the original instruction;
- rewrite `Add(a,b)` to `(a ^ b) + ((a & b) << 1)`;
- update the frame plan;
- return `none()` only when a mutation occurred.

This is a useful first pass, but every new pass should not duplicate its VReg,
frame, and vector-insertion logic. CFG transforms and string buffers make that
approach too error-prone.

### 2.3 Thin IR invariants relevant to pass authors

The implementation must preserve all of these:

- `VReg 0` is invalid/none and is also used as a documented sentinel.
- The IR is three-address but **not guaranteed SSA**; a VReg may be assigned
  more than once. Do not assume LLVM-style SSA dominance.
- Slice values occupy two consecutive VRegs: pointer at `v`, length at `v+1`.
- Aggregate values are frame-backed rather than ordinary scalar VRegs.
- Frame offsets are absolute negative `rbp` offsets. They are not abstract
  stack-slot numbers.
- `CopyBytes::dst` is a **read** of a destination-pointer VReg, not a normal
  produced value. Generic “dst is always a definition” logic is wrong.
- Call `args`, `arg_frame_offs`, and `arg_types` are parallel structures with
  special `v0 + frame offset` aggregate-argument encoding.
- Every block needs a non-`None` terminator.
- Block IDs must be unique and in `[0, blocks.size())`; `blocks[0].id` must be
  zero. The emitter indexes its label vector by `blk.id`.
- `ThinOp` is a stable serialized `uint16_t`; append new operations only.
- Symbolic native identity is `meta.native_name`; `native_fn` is not
  serializable.
- `rodata` is function-local and serialized.
- Passes run before register allocation in the normal compile path. A transform
  applied to a function with `ra.enabled == true` would make the allocation
  stale and must reject or clear/recompute it.

### 2.4 Current framework limitations that affect this project

1. **No pass options or seed path.** `reg.add<T>()` requires `T{}`. There is no
   CLI `--pass-seed`, no configured factory overload, and no run context.
2. **The pipeline parser is only comma splitting.** Despite forward-looking
   comments, parenthesized pass arguments/sub-pipelines are not parsed.
3. **The analysis manager is a stub.** There is no cached CFG, predecessor,
   use-def, dominator, or liveness result and no real invalidation call.
4. **Side-effect classification is private to `ext_opt.cpp`.** Custom and obf
   passes cannot reuse one authoritative table.
5. **No shared mutation builder.** Every pass must currently rediscover max
   VRegs, frame allocation, and block-ID repair.
6. **No automatic verifier invocation.** Instrumentation can host one, but the
   CLI does not currently install post-pass validation.
7. **True indirect basic-block branches have no IR representation.** Current
   terminators are direct `Jmp`, direct two-way `Branch`, `Return`, and `Trap`.
   `CallIndirect` is a function call and must not be misused as a branch.
8. **Growth passes do not converge.** `run_to_fixpoint()` is appropriate for
   simplifying optimizations, but an injection pass can add more material on
   every round until `max_rounds` is reached.

### 2.5 Serialization blocker discovered for string encryption

`ThinMeta` declares `data_temp_off` specifically for `StringDecrypt`, and
`thin_lower.cpp` populates it. The emitter uses it as the decrypted byte-buffer
slot, falling back to `frame_off` if it is zero.

However, IR blob version 1 does **not** write or read `meta.data_temp_off` in
`thin_ir_ser.cpp`, and `validate_thin_function()` does not validate that offset.
After a serialize/deserialize round trip, a `StringDecrypt` can therefore lose
its distinct data-buffer location and fall back to the slice-result slot.

This is a hard gate for `strenc`:

- add `data_temp_off` to a new IR blob version;
- accept old version-1 blobs with `data_temp_off = 0` only under the existing
  compatibility behavior;
- validate a nonzero `data_temp_off` as a negative in-frame range capable of
  holding `meta.len` bytes;
- add a serialize/deserialize/emit execution test for an encrypted literal;
- document the format addition.

Do not implement an IR string-encryption pass that creates new
`StringDecrypt` instructions while this metadata can be dropped.

---

## 3. Shared foundations to implement first

### 3.1 `ThinIRMutation` helper

Add one helper used by both `ext_obf` and the custom examples. The exact file
can be `src/thin_ir_mutation.hpp` or an extension-local helper if the public API
is not ready, but there should be one implementation of:

- `scan_next_vreg()`, including args and terminator operands;
- `new_scalar_vreg(type, width)`;
- `allocate_frame_bytes(size, alignment)` and `allocate_scalar_spill()`;
- `new_block_id()`, `split_block()`, and `redirect_edge()`;
- `predecessors()` / `successors()`;
- `canonicalize_block_ids()` with a complete old-ID/new-ID target rewrite;
- `finish()` to update `frame.next_local_off`, `frame.frame_size`,
  `declared_max_vreg`, and invalidate stale `ra` state.

The frame helper must codify the lowerer/emitter convention rather than copy
`SubstitutionPass`'s local `+8` calculation into every pass. It must handle
8-byte scalar spills and byte-sized string buffers, retain stack alignment,
and prove every generated offset lies within the final frame.

For a deserialized function, a pass that allocates VRegs must raise
`declared_max_vreg`. Leaving the old bound causes in-memory post-pass validation
to reject the pass's output, even though serialization later recomputes a new
header bound.

### 3.2 Public instruction-effect classification

Move effect knowledge out of the anonymous namespace in `ext_opt.cpp` into a
shared, exhaustive API, for example:

```cpp
enum class ThinEffect : uint32_t {
    None, ReadsFrame, WritesFrame, ReadsGlobal, WritesGlobal,
    ReadsIndirect, WritesIndirect, CallsUnknown, MayTrap, WritesTemp
};
ThinEffects thin_effects(ThinOp op);
bool is_removable_if_result_dead(const ThinInstr& in);
```

At minimum distinguish pure value computation, frame reads/writes,
global/indirect reads/writes, unknown call effects, guards that may trap, and
stack-temp writes such as `StringDecrypt` and aggregate initialization.

The current boolean `is_side_effecting` is not expressive enough for CFG and
junk transforms. `StoreFrame` is not externally observable like `StoreGlobal`,
but it is still a mutation and cannot be moved blindly. A mutable global load
also cannot be treated as pure arithmetic across calls or stores.

**Author rule:** effects are defined centrally by `ThinOp`, not marked ad hoc on
individual instructions. Adding a new op requires adding its classification in
the same change. Calls remain conservatively effectful until native purity is a
real ABI property.

### 3.3 Deterministic seed model

Use reproducibility by construction:

- Add `--pass-seed <u64>` (or `--obf-seed`; choose one spelling everywhere).
- Use a documented fixed default. A random-each-build mode must be explicit and
  print the chosen seed.
- Derive a local function seed from base seed, stable pass name, function name,
  and function slot. Do not retain an advancing RNG across functions.
- Specify the stable hash and PRNG (for example FNV-1a/SplitMix64 plus a pinned
  xoshiro/PCG step). Do not use `std::hash` as a persistence contract.
- Avoid `std::uniform_int_distribution` for byte-identical cross-library
  output; use documented rejection sampling.
- Sort candidate sites by block/instruction order. Never select based on
  `unordered_map` iteration.
- Derive independent substreams for site selection, variant, truth value,
  keys, and junk count so one added draw does not reshuffle later decisions.

Extend the registry with configured factories while retaining `reg.add<T>()`:

```cpp
reg.add_factory("opaque", [opts] {
    return make_pass_concept(OpaquePredicatePass{opts});
});
```

`ObfPassOptions` should include seed, site probability, maximum added
instructions/blocks, and maximum growth ratio. Capturing immutable options is
simpler than pretending the current parser supports `opaque(seed=42)`;
parameterized pipeline syntax can come later.

### 3.4 Validation, limits, and one-shot semantics

Install debug/test instrumentation that verifies after every mutating pass.
`validate_thin_function()` is the first gate, but a pass-author verifier should
also check op-specific operand shape, definitions, slice pairs, call parallel
vectors, type/width agreement, byte ranges for frame storage, `data_temp_off`,
stale regalloc state, and serializer maxima.

Every production pass needs hard caps on sites, VRegs, instructions, blocks,
frame growth, growth ratio, and `IR_MAX_*`. Skip remaining sites
deterministically on a cap; never leave a half-created site.

Complexity-increasing passes are **one-shot**. Do not put them in
`run_to_fixpoint()` without an explicit idempotence marker.

---
## 4. New production obfuscation passes

### 4.1 Opaque predicates — `opaque`

**Goal:** add conditions whose outcome is mathematically fixed but whose
dataflow appears runtime-dependent, with a semantically inert bogus path on the
untaken edge.

For an integer VReg `x`, the first reviewed construction should be:

```text
p0 = x + 1
p1 = x * p0
p2 = p1 & 1
cond_true  = (p2 == 0)
cond_false = (p2 != 0)
```

The product of consecutive integers is even, including under fixed-width
modular wraparound. Apply the same width to intermediates and produce canonical
bool with `Cmp` (`cmp=Eq` or `Neq`). A site becomes:

```text
original prefix
cond = opaque(x)
Branch cond -> real continuation, bogus block

bogus block:
    pure junk using fresh VRegs only
    Jmp real continuation

real continuation:
    original suffix and original terminator
```

The bogus block should rejoin without modifying visible state. This gives a
stronger safety property than relying only on the predicate never being wrong.

**Candidate rules:**

- use an integer scalar available at the insertion point;
- exclude pointers, slices, floats, aggregate sentinels, and values not
  available on every incoming path;
- do not split a safety guard from the operation it protects;
- never duplicate or move a side-effecting instruction;
- skip trivial functions with no safe runtime value and enforce growth caps.

Later variants may include `(x^x)==0`, `((x|1)&1)==1`, and reviewed masked
identities. Do not use `Div`/`Mod`: zero divisors and overflow guards can add
observable traps.

**Tests:** baseline/emitted equivalence for branches and loops; forced
always-true and always-false forms; no effects in bogus blocks; same-seed blob
equality; different pinned seeds produce different eligible shapes; growth
caps; validation and serialization round trip. Document that const-prop/DCE
after `opaque` may remove it.

### 4.2 Dead-code and bogus-block injection — `deadinject`

There is no `ThinOp::Nop`, so support two explicit modes:

1. **Live-path semantic NOPs:** pure computations into fresh, unused VRegs and
   dedicated internal spill slots.
2. **Bogus blocks:** rejoining blocks attached to opaque predicates, or
   structurally unreachable appended blocks only for narrowly scoped tests.

The safe junk vocabulary is `ConstInt`, `Move`, `Add`, `Sub`, `Mul`, `And`,
`Or`, `Xor`, `Neg`, and `BitNot`, with bounded shifts only after emitter
semantics are pinned. Exclude calls, division/modulo, all guards/checks,
global/indirect/aggregate writes, `CopyBytes`, `StringDecrypt`, uninitialized
loads, existing destinations, and user-visible frame slots.

Build a chain from a live scalar or encoded seed constant so it looks less like
isolated constants; leave only the final result unused. Prefer rejoining bogus
blocks to self-looping blocks: a bad edge rewrite to a self-loop hangs.

**Tests:** generated junk contains no effectful op; no old VReg/slot is written;
value equivalence; count increase; DCE-after-injection removes live-path junk;
limits and deterministic selection.

### 4.3 Expanded arithmetic/MBA substitution — `mba`

Generalize the shipped Add-only `subst` into deterministically selected forms.
All identities are modulo the instruction width:

```text
Add: a+b = (a^b) + ((a&b)<<1)
Add: a+b = a - (-b)
Sub: a-b = a + (~b) + 1
Sub: a-b = (a+r) - (b+r)
Xor: a^b = (a|b) - (a&b)
Xor: a^b = (a+b) - ((a&b)<<1)
And: a&b = (a|b) - (a^b)
Or:  a|b = (a^b) + (a&b)
Mul: a*b = a*(b+r) - a*r
```

`Mul` still contains multiplication but hides the original relation; cap its
expansion. Constant-multiply shift/add decomposition is a separate bounded
variant.

Snapshot original candidates before editing so inserted arithmetic is not
reprocessed. Initially require width `1/2/4/8` and two VReg operands. Add
immediates later by materializing an encoded VReg. Copy width, type,
signedness where relevant, and `loc` to intermediates; assign each a fresh
spill. Keep the old destination/frame slot on the final operation. Compute
random constants with unsigned-width helpers to avoid C++ signed overflow.

Do not touch float ops, introduce division/traps, assume SSA, or rely on only
final truncation without narrow-width tests.

**Tests:** every width and integer edge pattern; fixed-seed randomized operand
pairs; each identity forced; immediate forms once supported; same-seed
blob/dump equality; no recursive growth; `subst` compatibility.

### 4.4 Block splitting and branch redirection — `split-redirect`

**Phase A — neutral CFG transforms.** To split block `A` at instruction `k`:

1. keep `[0,k)` in `A`;
2. move `[k,end)` and the old terminator to continuation `B`;
3. terminate `A` with `Jmp B`;
4. optionally insert pure-junk trampoline blocks;
5. redirect selected incoming edges through trampolines;
6. canonicalize IDs and rewrite every target.

Avoid splitting empty blocks except for explicit edge-trampoline mode, avoid
recognized guard/operation pairs, preserve entry at `blocks[0]/id 0`, and
respect per-block serialization limits.

**Phase B — safe indirect machine jumps.** `CallIndirect` is a function call,
not a branch. Do not branch to an arbitrary address VReg. Add an append-only,
constrained terminator instead:

```cpp
enum class TermKind : uint8_t {
    None, Jmp, Branch, Return, Trap,
    IndirectJmp // target remains a validated block ID
};
```

`IndirectJmp target=N` has the same CFG successor as direct `Jmp N`, but the
emitter materializes the internal label address and emits `jmp reg`. The IR and
blob still contain only a bounds-checked symbolic block ID. Conditional edges
can branch to trampolines that end in `IndirectJmp`.

This requires updates to dump, serializer/version, validator, CFG utilities,
emitter label-address fixups, and every exhaustive terminator switch. Invalid
targets must be rejected exactly like direct jumps.

**Tests:** first/middle/last split; all terminator kinds; loops/backedges and
diamonds; predecessor remapping; emitted indirect jump proof; malformed target
rejection; inability to use arbitrary VRegs as code addresses; deterministic
semantic equivalence.

### 4.5 Constant encoding — `constenc`

Replace integer literals with width-normalized computations using a nontrivial
seed-derived `K`:

```text
C = (C^K)^K
C = (C-K)+K
C = A+B, where B=C-A
C = (C+K)-K
```

Use unsigned modular arithmetic for construction, then preserve bit patterns
safely in `int64_t`. For bool, finish with `Cmp` to canonicalize `0/1`.

Start with `ConstInt`/`ConstBool`. Later materialize immediates only for an
explicit allowlist (`Add/Sub/Mul/And/Or/Xor/Shl/Shr/Cmp`) after emitter tests.
Never “encode” structural integer metadata: block targets, frame offsets,
widths, lengths, slots, module IDs, addends, predicates, trap reasons, decrypt
keys, or sentinels.

Snapshot original constants and transform each once. Place `constenc` after
simplifying optimization; const-prop after it intentionally reverses the work.

**Tests:** every width/signed edge pattern; bool canonicalization; allowlisted
runtime immediates versus structural exclusion; no frame/rodata metadata
change; semantic equivalence; seeded structure and serialization.

### 4.6 String-literal encryption integration — `strenc`

This pass removes casual plaintext strings; it is not a confidentiality
boundary. A key and decryptor in one artifact are recoverable at runtime.

The CLI already sets `Program::string_xor_key=0xA5`, so sema often emits
`StringDecrypt` before passes. `strenc` primarily supports hosts using key zero,
programmatic/deserialized plaintext IR, and deterministic per-literal keys.
Never double-encrypt an existing `StringDecrypt`; leave it unchanged or use an
explicit rekey mode.

After the §2.5 serialization fix:

1. Collect all `ConstStringRef`/`StringDecrypt` ranges in stable order and
   validate nonnegative in-bounds lengths.
2. Build a new rodata image; do not mutate in place because ranges may overlap
   or share bytes under different keys.
3. Derive a nonzero byte key from seed and stable site identity; append encoded
   bytes and record the new addend.
4. Preserve or explicitly rekey existing encrypted ranges.
5. Rewrite `ConstStringRef` to `StringDecrypt`, preserving dst, slice-result
   `frame_off`, type, len, base kind, and `loc`; key goes in `imm.i`.
6. Allocate a distinct `data_temp_off` buffer of at least `len` bytes, not
   overlapping the 16-byte slice slot or another live buffer.
7. Remap all rodata references and atomically replace `f.rodata` only after the
   complete plan validates. Do not leave the old plaintext copy appended.
8. Update frame layout through the shared mutation helper.

Define empty-string behavior explicitly. A stronger keystream would require a
separate versioned emitter/metadata design; do not describe byte XOR as strong
encryption.

**Tests:** plaintext-to-decrypt rewrite; plaintext absent from final rodata and
blob; lengths 0, 1, 256, and >256; repeated/overlapping references; no double
encryption; nonoverlapping frame buffers; serialize/deserialize/validate/emit
execution; same-seed identical output and different-seed encoded variation.

---

## 5. Recommended ordering and interactions

Run cleanup before obfuscation:

```text
constprop,forward,copyprop,instcombine,dce,cse,licm,dse,
strenc,constenc,split-redirect,opaque,deadinject,mba
```

String/constant encoding establishes data; CFG passes add structure; MBA last
expands source and injected arithmetic. If redirection should affect newly
opaque edges, expose split and redirect as internal phases or a final
redirect-only pass. Do not run const-prop, CSE, DCE, or block merging afterward
unless intentionally measuring how readily the obfuscation is removed.

Use `run()`, not `run_to_fixpoint()`.

---

## 6. `examples/custom_pass/` design

Create a package small enough to copy but complete enough to teach the real
contracts:

```text
examples/custom_pass/
├── README.md
├── custom_passes.hpp
├── custom_passes.cpp
├── register_custom_passes.hpp
├── register_custom_passes.cpp
├── custom_pass_test.cpp
├── sample.ember
└── CMakeLists.txt                 # optional standalone-consumer example
```

The root project should add a `custom_pass_test` CTest target. Examples must use
the production mutation/effect/seed helpers, not private copies.

### 6.1 Minimal pass: deterministic semantic NOP injection

Name: `example-nop`.

- Select eligible blocks from a stable list using the configured seed.
- Insert a fresh `ConstInt` plus a short pure `Xor/Add/Move` chain into fresh
  VRegs and spill slots.
- Leave the result unused; never touch old destinations, locals, calls, or
  terminators.
- Return `none()` if inserted, otherwise `all()`.

Call it a **semantic NOP**, not a native NOP. Show that `dce` after it removes
it. Keep it optional (`is_required=false`) so instrumentation gating can be
demonstrated.

### 6.2 Instruction rewrite: `x + 0 -> x`

Name: `example-add-zero`. Initially handle only:

```text
Add dst, src1, immediate(0) -> Move dst, src1
```

`src2==0` denotes immediate form and `imm.i` carries zero. Keep dst,
`meta.frame_off`, type, width, and `loc`; clear irrelevant fields for canonical
output. A later section may track a zero VReg locally while respecting
redefinitions. Do not teach a global search for a zero definition in non-SSA
IR.

### 6.3 CFG pass: single-predecessor block merging

Name: `example-block-merge`. Merge `A -> B` only when:

- `A` ends in unconditional direct `Jmp B`;
- `B` is not entry and has exactly one predecessor, `A`;
- `A != B` and concatenation stays under `IR_MAX_INSTRS`;
- neither block has a future EH/cleanup reservation.

Append `B` instructions to `A`, replace `A`'s terminator with `B`'s, remove
`B`, canonicalize IDs, and rewrite every target. Recompute predecessors after
each merge or collect a noninterfering merge set first. This demonstrates why
CFG mutation is more than vector erasure.

### 6.4 Registration and host/CLI usage

Expose the standard extension shape:

```cpp
namespace ember::examples::custom_pass {
void register_passes(EmberPassRegistry& reg,
                     CustomPassOptions options = {});
}
```

Use `reg.add<T>()` for unconfigured passes and the proposed factory overload
for `NopInjectionPass{options}`. The README host snippet should be exact:

```cpp
EmberPassRegistry reg;
ext_opt::register_passes(reg);
ext_obf::register_passes(reg, obf_options);
ember::examples::custom_pass::register_passes(reg, custom_options);

EmberPassManager pm;
build_pipeline_from_string("example-add-zero,example-nop", reg, pm, &err);
ctx.pass_manager = &pm;
ctx.enable_ir_backend = true;
```

The stock CLI currently registers only `ext_opt` and `ext_obf`; linking a
library does not dynamically discover passes. Provide either a small
`ember_custom_pass_cli` target or a documented registration addition in the
consumer. Then show:

```bash
ember_custom_pass_cli run examples/custom_pass/sample.ember \
  --passes example-nop,example-add-zero --pass-seed 42
ember_custom_pass_cli run examples/custom_pass/sample.ember \
  --passes example-block-merge --dump
```

Also show the expected unknown-pass error from the unmodified CLI so plugin
semantics are not overstated.

### 6.5 Example tests

`custom_pass_test.cpp` should prove:

1. all names register and factories create fresh instances;
2. each returns `all()` on no-op input and `none()` when changed;
3. baseline and transformed emitted results match;
4. same IR/seed gives identical `dump()` and serialized blob;
5. two selected seeds give different structures on a candidate-rich workload;
6. reverse function compilation order leaves each function's output unchanged;
7. pass -> validate -> serialize -> deserialize -> validate succeeds;
8. block merge handles diamonds, loops, and multi-predecessor no-merge cases;
9. frame/VReg growth stays in bounds;
10. instrumentation can skip optional examples.

Prefer serialized-byte equality as the strict reproducibility check; use dump
equality for readable diagnostics.

---
## 7. Pass-author documentation to add

Create `docs/PASS_AUTHORING.md` and link it from
`docs/spec/PASS_SYSTEM_DESIGN.md` and `examples/custom_pass/README.md`.

### 7.1 Lifecycle

Document this full sequence:

1. **Registration:** stable unique name to fresh factory; configured factories
   capture immutable options.
2. **Pipeline construction:** create instances in user order and reject unknown
   names.
3. **Analysis:** request cached results when available; today passes scan
   directly.
4. **Mutation:** use shared utilities and update all structural metadata.
5. **Preservation:** return `all()` only if IR is unchanged; otherwise `none()`
   until selective preservation exists.
6. **Invalidation:** the manager drops cached results based on preservation once
   analyses are implemented.
7. **Validation:** debug/test instrumentation verifies each mutating pass.
8. **Serialization/emission:** output must round-trip without fallback-only
   state.

Required passes bypass skip gates, but still obey safety and growth limits.

### 7.2 Mutation rules

Include a “must/must not” checklist:

- preserve observable behavior, traps, and call ordering;
- allocate fresh VRegs and valid frame storage;
- preserve op-relevant metadata and source locations;
- update every use when replacing a VReg;
- preserve slice pairs and call parallel vectors;
- preserve entry/block-ID/target invariants;
- do not reorder calls, guards, mutable global reads, or writes without proof;
- do not treat `CopyBytes::dst` as a definition;
- do not create raw pointers in serialized fields;
- append, never renumber, `ThinOp`/terminator ordinals;
- invalidate/re-run regalloc after mutation;
- do not use complexity growth in a fixpoint loop.

### 7.3 Side-effect classification

Publish the shared effect table and examples distinguishing pure arithmetic,
local memory, externally visible global/indirect access, unknown calls,
may-trap operations, and temp-buffer writes. Explain how to update it when
adding an op. Until native purity is ABI metadata, all calls are effectful.

### 7.4 Analysis invalidation

Separate current and future behavior:

- **Current:** `EmberAnalysisManager` is empty; any changed pass returns
  `EmberPreserved::none()`.
- **Future:** instruction rewrites invalidate use-def, value numbering, and
  liveness; CFG edits additionally invalidate predecessors, dominators, loop
  info, and block order; frame/rodata edits invalidate layout/serialization
  summaries.

When caches exist, `EmberPassManager::run()` must call
`am.invalidate(f, preserved)` after each pass. A pass cannot claim preservation
merely because its own local scan is current.

### 7.5 Serialization constraints

Document stable enum ordinals, blob-version requirements for layout changes,
runtime-only dropped fields (`native_fn`, `abs_fixups`, regalloc result), native
rebinding by symbolic name, stable type encoding instead of pointers, rodata
bounds/remapping, VReg/count maxima, and the required
validate/serialize/deserialize/validate test sequence.

A production IR pass is incomplete if it only works in immediate JIT mode.
Do not use `non_serializable` as an escape hatch for an advertised IR pass.

### 7.6 Seed handling

Specify the PRNG/hash algorithms, default seed, function-seed derivation,
substreams, candidate ordering, CLI spelling, and artifact logging. State:

- source + options + tool version + seed reproduces the same IR blob;
- seed controls layout variation, not semantics;
- seed is not a cryptographic secret;
- random mode records the generated seed;
- output is independent of scheduling and function compilation order.

### 7.7 Composition and tests

Include the optimization-before-obfuscation order, interactions that erase
obfuscation, one-shot warning, and a test template covering value equivalence,
verifier success, serialization, reproducibility, variation, and growth caps.

---

## 8. Analysis invalidation matrix

| Pass | Instructions/use-def | CFG/preds/dom/loops | Frame | Rodata | Regalloc |
|---|---:|---:|---:|---:|---:|
| `mba` | invalidate | preserve | invalidate | preserve | invalidate |
| `constenc` | invalidate | preserve | invalidate | preserve | invalidate |
| live `deadinject` | invalidate | preserve | invalidate | preserve | invalidate |
| bogus-block `deadinject` | invalidate | invalidate | invalidate | preserve | invalidate |
| `opaque` | invalidate | invalidate | invalidate | preserve | invalidate |
| `split-redirect` | invalidate local summaries | invalidate | preserve unless junk spills | preserve | invalidate |
| `strenc` | invalidate | preserve | invalidate | invalidate | invalidate |
| `example-add-zero` | invalidate | preserve | preserve | preserve | invalidate |
| `example-block-merge` | invalidate | invalidate | preserve | preserve | invalidate |

Until `EmberPreserved` grows beyond a bool, every changed row returns `none()`.

---

## 9. Implementation phases and gates

### Phase 0 — foundations

- shared effects and mutation/CFG helpers;
- configured registry factory;
- seed CLI and stable PRNG;
- post-pass verifier instrumentation;
- growth budgets and author-doc skeleton.

**Gate:** infrastructure/factory/seed tests; no production behavior changed.

### Phase 1 — executable custom examples

- all three example passes;
- registration and host/CLI docs;
- deterministic, validation, and serialization tests.

**Gate:** `custom_pass_test` passes and examples need no private opt helpers.

### Phase 2 — local production transforms

- `constenc`, generalized `mba` with `subst` compatibility, and live-path
  `deadinject`.

**Gate:** width edge tests, end-to-end equivalence, fixed-seed blob equality,
and bounded one-shot growth.

### Phase 3 — CFG transforms

- split/trampolines, opaque predicates/rejoining bogus blocks, canonical IDs,
  constrained `IndirectJmp`, and small-CFG property/fuzz tests.

**Gate:** round-trip/emission equivalence, malformed target rejection, and no
arbitrary code-address target.

### Phase 4 — format update and strings

- versioned `data_temp_off` plus new terminator support;
- version-1 backward loading;
- validator byte-range checks;
- `strenc` rodata rebuild/remap and plaintext-absence tests.

**Gate:** encrypted strings survive `.em` round trip and plaintext is absent
from final rodata/blob.

### Phase 5 — integration and measurement

- register production names in `ext_obf`;
- update CLI help/listing and design docs;
- benchmark compile time, emitted bytes, frame growth, and runtime;
- publish explicit light/balanced/heavy presets rather than hidden behavior.

---

## 10. Acceptance criteria

The work is complete only when:

- all six requested pass families are registered/documented or explicitly
  version-gated;
- mutations preserve values across target workloads and integer edge cases;
- preservation/invalidation is correct;
- one shared exhaustive classifier defines effects;
- same seed gives byte-identical serialized IR independent of compile order;
- different pinned seeds vary eligible transforms;
- transformed functions validate before emit and after serialization;
- VReg/block/instruction/frame/rodata limits are enforced;
- passes use no runtime-only pointers and do not silently become
  non-serializable;
- string encryption waits for `data_temp_off` round-trip and range validation;
- indirect branches target only validated symbolic blocks;
- `examples/custom_pass/` covers insertion, rewrite, CFG mutation,
  registration, CLI integration, and deterministic tests;
- author docs cover lifecycle, mutation, effects, invalidation, serialization,
  seeds, composition, and tests;
- obfuscation is described as increased analysis cost, not a security boundary
  or cryptographic confidentiality guarantee.
