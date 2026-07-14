# Authoring custom Ember IR passes

This guide is the user-facing contract for passes over `ThinFunction`. Complete,
copyable examples live in [`examples/custom_pass/`](../examples/custom_pass/).
The framework headers are `src/ember_pass.hpp`, `ember_pass_registry.hpp`,
`ember_pass_pipeline.hpp`, `src/extension_registry.hpp` (the shared
`ExtensionError` / `ExtensionStatus` / `ExtensionResult<T>` contracts used by
the pass registry and, later, by other extension registries), and
`src/seed_derivation.hpp` (the shared, versioned seed-derivation service — see
[Deterministic seeds](#deterministic-seeds)).

## Pass shape and lifecycle

A pass is a concrete C++ type, not a subclass of a virtual pass base. It inherits
the CRTP metadata mix-in and provides `run`:

```cpp
struct MyPass : ember::EmberPassInfoMixin<MyPass> {
    static constexpr const char* pass_name = "my-pass";
    ember::EmberPreserved run(ember::ThinFunction& f,
                              ember::EmberAnalysisManager& am);
};
```

The lifecycle is:

1. **Registration:** map a stable, unique pipeline name to a fresh pass factory
   with `reg.add<MyPass>("my-pass")`.
2. **Pipeline construction:** `build_pipeline_from_string` resolves every
   name into temporary ownership and APPENDS the resolved passes to the manager
   only after the complete specification succeeds. It rejects empty elements,
   parentheses, unknown names, null factory results, and trailing junk, and
   leaves the manager unchanged on any failure (see
   [Pipeline grammar and transactional guarantees](#pipeline-grammar-and-transactional-guarantees)).
3. **Analysis:** inspect the function or request cached analyses. The analysis
   manager is currently a stub, so shipped passes scan the IR directly.
4. **Mutation:** change instructions, CFG, frame layout, or rodata while keeping
   every IR invariant and observable behavior intact.
5. **Preservation:** return `EmberPreserved::all()` only when nothing changed;
   return `none()` after any current transform.
6. **Invalidation:** selective cached invalidation is future work. Until then,
   `none()` is the conservative contract for all mutations.
7. **Validation:** tests should validate after each mutating pass with
   `verify_thin_function_for_codegen` (the in-memory, codegen-facing verifier;
   `validate_thin_function` is the disk-facing one). The checked execution path
   does this for you — see [Checked execution](#checked-execution).
8. **Serialization/emission:** transformed IR must survive a serialize,
   deserialize, validate, and emit cycle. Immediate-JIT-only success is not
   enough for a user-facing IR pass.

`static constexpr bool is_required = true` makes a pass bypass instrumentation
skip gates. Use it only for a genuinely mandatory transform; it does not relax
correctness, limits, or validation requirements.

## Mutation rules

### Instructions and values

- `VReg 0` means absent/invalid and is also a documented aggregate-argument
  sentinel. Never allocate it.
- Thin IR is **not guaranteed SSA**. Do not globally substitute a definition
  without accounting for redefinitions and control flow.
- Allocate fresh VRegs for new values. Slice and lambda values occupy `v` and
  `v+1`; scalar allocation must not collide with either word. Prefer
  `ThinIRMutation::allocate_scalar` / `allocate_pair` over hand-rolled VReg +
  frame growth — see [ThinIRMutation: transactional mutation
  helper](#thinirmutation-transactional-mutation-helper).
- A producing instruction normally needs a valid negative frame spill offset.
  Grow `frame.next_local_off` and aligned `frame.frame_size`, update
  `declared_max_vreg`, and clear `f.ra` when mutation makes regalloc stale.
  `ThinIRMutation::commit()` performs all of this atomically; without it, a
  pass must do each step explicitly and leave the function unchanged on
  failure.
- Preserve destination, type, width, signedness, frame slot, and `Loc` when
  replacing an operation unless the new operation deliberately changes one.
- Keep call `args`, `arg_frame_offs`, and `arg_types` parallel. Aggregate calls
  use special `v0 + frame offset` encoding.
- `CopyBytes::dst` is read as a destination pointer; it is not an ordinary
  definition. Generic “every dst defines a value” logic is incorrect.
- Do not reorder calls, traps/guards, mutable memory reads, or writes without a
  proof that behavior, trap order, and call order are unchanged.

### CFG

Every block must have a non-`None` terminator. Entry remains `blocks[0]` with ID
zero. IDs must be unique and in range; every `Jmp`/`Branch` target must name a
surviving block. After insertion or erasure, canonicalize IDs **and rewrite all
edges**. Recompute predecessors after an interfering merge rather than relying
on stale counts. Keep each block under `IR_MAX_INSTRS` and the function under
all serializer limits.

Do not put complexity-increasing/injection passes in `run_to_fixpoint()` unless
they are explicitly idempotent. Use one-shot `run()` for such passes.

## Side effects

The authoritative operation-effect model is the public
`classify_thin_effects()` / `removable_if_result_dead()` API in
`src/thin_effects.hpp`. It is exhaustive over the current `ThinOp` set and
classifies explicit and implicit frame-home reads/writes, globals,
indirect/unknown memory, calls, traps, `CopyBytes` intervals, temporary
writes, and escaped addresses. The optimizer's DCE/CSE passes consult this
model (via an op-level face that classifies a canonical minimal instruction);
the per-instruction `ThinEffectDescriptor` carries exact `ByteInterval` reads
and writes plus an `aliases_unknown_memory` flag for computed/escaped
addresses.

```cpp
#include "thin_effects.hpp"

ThinEffectDescriptor d = classify_thin_effects(in);
// d.flags is a bitmask of ThinEffectFlag values (ReadsFrame, WritesFrame,
// ReadsGlobal, WritesGlobal, ReadsIndirect, WritesIndirect, CallsUnknown,
// MayTrap, WritesTemp, ImplicitSpillWrite, EscapesAddress).
// d.reads / d.writes are exact/overlapping ByteInterval ranges.
// d.aliases_unknown_memory is set for computed/escaped-address accesses.

// The single dead-result predicate a DCE/coalesce rule consults:
if (removable_if_result_dead(in, d)) { /* safe to drop when dst is unused */ }
```

Calls are conservatively effectful (`CallsUnknown` + `aliases_unknown_memory`)
until purity is explicit ABI metadata. A call with `dst != 0 && frame_off != 0`
(not struct-by-ptr) also carries an `ImplicitSpillWrite` — the emitter pins the
result to `frame_off` (`pin_int_dst` / `store_xmm0_to_rbp` / slice two-word
store). `StoreFrame` is a local memory write classified `WritesFrame`; a
dead-store analysis (not the dead-result rule) decides its removability.
Mutable loads (`LoadFrame`/`LoadGlobal`) carry read intervals and are not
removable by a dead-result rule. A **computed** `LoadFrame` (`src1 != 0`)
reads `[src1 + field_off]` — a computed address through a vreg pointer — so it
is classified `ReadsIndirect` + `aliases_unknown_memory`, NOT `ReadsFrame` from
`frame_off` (the spill to `frame_off` is still an implicit write). Trapping
guards (`BoundsCheck`, `DivOverflowCheck`, `DepthCheck`, `BudgetCheck`,
`CallTargetGuard`) set `MayTrap` and are never removable. **Trapping
arithmetic** — integer `Div`/`Mod` (div-by-zero / signed-overflow trap paths)
and `FMod` (directly emits a trap) — also sets `MayTrap`; a dead-result rule
must not remove them even if `dst` is unused. `MakeSlice` does NOT write
`frame_off` — `frame_off` is the backing array address (the `lea` base), not a
result spill slot; it carries only `EscapesAddress`. A producer that pins its
result to `meta.frame_off` carries an `ImplicitSpillWrite` — a seemingly dead
definition whose slot is read later is live, and the dead-result rule refuses
to remove it.

Static flags alone NEVER authorize removal. Always pair `classify_thin_effects`
with `removable_if_result_dead` for a dead-result decision, and keep the
context-sensitive frame-home / read-slot protections the optimizer uses.

When a new `ThinOp` is added, update `classify_thin_effects` in the same
change. Do not annotate an individual instruction as pure to bypass the
central operation semantics.

## ThinIRMutation: transactional mutation helper

`src/thin_ir_mutation.hpp` provides the checked, transactional helper for
production and example passes. It centralizes the VReg / frame / CFG / regalloc
bookkeeping that obfuscation passes previously hand-rolled.

```cpp
#include "thin_ir_mutation.hpp"

EmberPreserved MyPass::run(ThinFunction& f, EmberAnalysisManager&) {
    ThinIRMutation mut(f, PassGrowthLimits{});
    bool changed = false;
    for (auto& blk : f.blocks) {
        for (auto it = blk.instrs.begin(); it != blk.instrs.end(); ++it) {
            ThinInstr& in = *it;
            if (!eligible(in)) continue;
            // Allocate a fresh scalar VReg + an 8-byte frame spill slot.
            auto r = mut.allocate_scalar(in.meta.type, in.meta.width);
            if (!r.ok()) break;          // soft ceiling: stop before the next site
            ThinInstr fresh;
            fresh.op = ThinOp::Xor;
            fresh.dst = r.get().vreg;
            fresh.src1 = in.src1; fresh.src2 = in.src2;
            fresh.meta.width = in.meta.width; fresh.meta.type = in.meta.type;
            fresh.meta.frame_off = r.get().frame_off;   // the staged spill slot
            fresh.loc = in.loc;
            it = blk.instrs.insert(it, std::move(fresh));
            ++it;
            changed = true;
        }
    }
    if (!changed) return EmberPreserved::all();
    auto rc = mut.commit();
    if (!rc.ok()) return EmberPreserved::all();   // hard ceiling: rolled back
    return EmberPreserved::none();
}
```

Transaction semantics:

- The constructor snapshots the function. Allocation methods
  (`allocate_scalar`, `allocate_pair`, `allocate_frame_bytes`) do NOT mutate
  the function's frame plan immediately — they return the staged VReg / offset
  and record the growth internally. CFG methods (`split_block`,
  `redirect_edge`, `canonicalize_block_ids`) mutate the blocks directly so the
  pass can insert into split blocks; the snapshot restore on abandon undoes
  them.
- `commit()` publishes `frame.next_local_off`, aligns `frame.frame_size` to 16
  (growing only), sets `declared_max_vreg`, and clears stale `f.ra`. After
  commit the destructor performs NO rollback.
- If the object is destroyed without `commit()` (failure or abandon), the
  snapshot is restored: the function is byte-for-byte unchanged, including any
  direct block/instr mutations the pass made.
- A failed allocation returns a `MutationResult` carrying a `MutationStatus`
  (`LimitExceeded`, `InvalidArgument`, or `NotCommitted`) and stages nothing.
  The pass stops before the next site (soft-ceiling semantics) and commits what
  it has, or abandons if nothing was staged.
- Every allocation preflights the configured `PassGrowthLimits`
  (`max_added_vregs`, `max_added_blocks`, `max_added_frame_bytes`) and the hard
  ceilings (`IR_MAX_VREGS`, 1 MB frame) BEFORE reserving the resource.

The central VReg-bound enumeration (`compute_central_max_vreg`) includes
implicit `dst+1` slice/lambda pair results, call args, and terminator
cond/ret. It is the single source of truth shared with the serializer's
`ir_blob` header `max_vreg`. Do not re-derive a private VReg bound.

`allocate_pair` returns two consecutive VRegs (`hi == lo + 1`) and a 16-byte
frame slot for slice/lambda results. `allocate_frame_bytes(size, alignment)`
returns an aligned negative offset for temporary buffers (e.g. `StringDecrypt`
data storage, dead-code injection scratch). The returned offset is aligned to
`alignment` (the region's lowest address is aligned), and all arithmetic is
checked — a request that would overflow `int32_t` or `uint32_t` returns
`LimitExceeded` rather than wrapping.

### Atomic per-site preflight (`reserve_site`)

When a transform consumes multiple resources per site (VRegs + frame slots +
instructions), allocating them one at a time can leave an **orphan partial
allocation**: if the 2nd of 3 `allocate_scalar` calls fails a limit, the 1st
VReg/frame slot is already staged and committed. `reserve_site` prevents this
by atomically preflighting ALL of a site's resources BEFORE any is consumed:

```cpp
// Preflight 3 VRegs + 24 frame bytes + 3 instructions for one MBA site.
// If any limit would be exceeded, returns LimitExceeded and stages nothing.
auto rs = mut.reserve_site(3, 24, 3, 0);
if (!rs.ok()) break;               // stop before this site
// Now the 3 allocate_scalar calls are guaranteed to succeed.
auto r1 = mut.allocate_scalar(ty, width);
auto r2 = mut.allocate_scalar(ty, width);
auto r3 = mut.allocate_scalar(ty, width);
// ... insert instructions ...
mut.record_added_instructions(3);  // account for the 3 inserted instrs
```

`reserve_site` checks the site count (`max_sites`), the soft VReg / frame /
instruction / block limits, the hard VReg / frame ceilings, and the growth
ratio (`initial * growth_numerator / growth_denominator`) — all with checked
arithmetic. On success it increments only the site counter; the individual
`allocate_*` methods increment the resource counters as they consume. On
failure it increments nothing. `record_added_instructions(n)` accounts for
instructions the pass inserts directly (not via an allocate method).

### Frame allocation and arg-temps

When the frame plan has a reserved arg-temp area (`arg_temps_base != 0`), new
allocations start below the ENTIRE existing frame (`frame_size`) so they
cannot overlap the arg temps that live below the locals. When
`arg_temps_base == 0`, allocations grow from `next_local_off` as usual. The
non-overlap check also barriers the below-locals region when arg temps are
present.

## Analysis invalidation

Today `EmberAnalysisManager` has no cached analyses and `EmberPreserved` is an
all-or-nothing boolean. Return `all()` for an exact no-op and `none()` for every
mutation.

When selective analyses are implemented:

- instruction rewrites invalidate use-def, value numbering, and liveness;
- CFG edits additionally invalidate predecessors, dominators, loops, and block
  order;
- frame or rodata edits invalidate layout and serialization summaries;
- every VReg/CFG mutation invalidates an existing register allocation.

A pass cannot claim preservation merely because its own local scan remains
usable. The manager will eventually use the returned set to invalidate shared
cached results.

## Serialization constraints

`ThinOp` is serialized as a stable `uint16_t`; append operations, never reorder
or renumber them. The same applies to persisted enum ordinals and requires a
blob-version update when representation changes.

Raw pointers are not portable IR state:

- `native_fn`, `abs_fixups`, and regalloc results are runtime-only/dropped;
- native calls must retain `meta.native_name` for symbolic rebinding;
- type pointers are serialized through Ember's stable type codec;
- rodata references must remain in bounds and every remapped addend must point
  into the final `f.rodata`;
- VReg, block, argument, parameter, fixup, and instruction counts must respect
  the `IR_MAX_*` limits in `thin_ir_ser.hpp`.

The minimum production test sequence is:

```text
pass -> validate_thin_function -> serialize_thin_function
     -> deserialize_thin_function -> validate_thin_function -> emit/run
```

Do not set `non_serializable` as an escape hatch for an advertised IR pass.

## Deterministic seeds

Seeded transforms derive per-site randomness from the **shared seed-derivation
service** in `src/seed_derivation.hpp`, never from a process-global mutable
stream and never from a hand-rolled FNV-1a/SplitMix64 copy. This makes output
independent of function compilation order and thread scheduling. Keep
candidate enumeration in stable block/instruction vector order; do not use
`unordered_map` iteration as random input.

A seed controls layout/diversity variation only, never semantics, and is **not
a cryptographic secret**. Random-each-build mode should be explicit and
print/store the generated seed. Record source/tool version, options, and seed
with artifacts.

### SeedRequest and the SeedDeriver interface

A pass builds a `SeedRequest` carrying the canonical identities of the site it
is diversifying and hands it to an immutable `SeedDeriver`:

```cpp
#include "seed_derivation.hpp"

ember::SeedRequest req;
req.engine_version         = "ember-1";
req.module_id              = module_stable_id;
req.build_profile_id       = "light";
req.pass_name              = "mba_expand";
req.pass_algorithm_version = 1;
req.function_name          = f.name;
req.logical_slot           = f.slot;
req.block_id               = blk.id;
req.instruction_ordinal    = ordinal;
req.purpose                = "variant";   // select|variant|constant|truth|...

auto r = deriver.derive(req);             // const SeedDeriver& deriver
if (!r) { /* r.error->registry == "ember-seed-deriver" */ }
std::array<uint8_t,32> seed = *r.value;
```

`SeedDeriver::derive` is `const` and every provided implementation mutates
nothing, so one `const SeedDeriver&` may be shared across worker threads.
There is **no mutable/global stream**: a pass consumes the 256-bit result (or
one of its four 64-bit lanes) through a *local* `StableRng` it constructs per
site, so a draw for one site never reshuffles a later site and compile order
never advances a shared generator.

### StableRng: local per-site randomness

`StableRng` is a pinned SplitMix64 advancing generator with a
rejection-sampled bounded-index helper. Construct it **per site** from a
derived lane (or the whole 32-byte result); it is never held in process-global
state and is not shared across sites:

```cpp
ember::StableRng rng(seed_lane64);        // a 64-bit lane from derive()
uint64_t choice = rng.bounded(candidate_count);  // unbiased index in [0,n)
uint64_t draw   = rng.next();                     // raw 64-bit draw
```

`bounded(n)` uses rejection sampling to remove the modular bias a naive
`next() % n` introduces when `n` does not divide 2^64. `n == 0` returns 0.

### Fixed-root and u64-to-root adapter

For ordinary reproducible builds, use the immutable
`FixedRootSeedDeriver`, backed by a 256-bit root. A fixed 64-bit CLI seed is
adapted **once** into a 256-bit root with the pinned `u64_to_root` adapter,
then wrapped in a `FixedRootSeedDeriver` shared by every configured factory:

```cpp
auto root = ember::u64_to_root(cli_seed_u64);
auto deriver = std::make_shared<const ember::FixedRootSeedDeriver>(root);
// capture `deriver` by value into each factory's PolymorphicPassOptions
```

A host may instead supply its own `SeedDeriver` (e.g. the companion plan's
external-material adapter); the pass API only ever sees opaque derived
material, never raw machine identifiers or a provider's root secret.

### Canonical byte encoding (pinned, implementation-independent)

The `derive` result is a pure function of `(root, request)` computed with
**only** implementation-independent integer operations. There is no `std::hash`,
no `std::random_device`, no standard distribution, no native struct bytes, and
no unordered-container iteration anywhere in the encoding. The canonical
algorithm is:

```text
DOMAIN  = the 20 ASCII bytes of "Ember.SeedDeriver.v1"   (fixed, no length prefix)

B := DOMAIN
     || u32_le(len(engine_version))   || engine_version bytes
     || u32_le(len(module_id))        || module_id bytes
     || u32_le(len(build_profile_id)) || build_profile_id bytes
     || u32_le(len(pass_name))        || pass_name bytes
     || u32_le(pass_algorithm_version)
     || u32_le(len(function_name))    || function_name bytes
     || u32_le(logical_slot)
     || u32_le(block_id)
     || u32_le(instruction_ordinal)
     || u32_le(len(purpose))          || purpose bytes

String lengths are uint32_t byte counts (no NUL terminator). Integers are
fixed-width uint32_t. Every multi-byte integer is LITTLE-ENDIAN.

base = FNV1a-64( root_32_bytes || B )          # fold the root first, then B
       FNV1a-64: h0 = 0xcbf29ce484222325
                  h  = (h ^ byte) * 0x100000001b3   (per byte, mod 2^64)

Four INDEPENDENT, domain-separated 64-bit lanes (no shared advancing state):
  splitmix_mix(z) = (z=(z^(z>>30))*0xBF58476D1CE4E5B9; z=(z^(z>>27))*0x94D049BB133111EB; z^(z>>31))
  lane[i] = splitmix_mix( base ^ LANE_DOMAIN[i] )      for i in 0..3
  LANE_DOMAIN = { 0x454D53440C000000, 0x454D53440C000001,
                  0x454D53440C000002, 0x454D53440C000003 }
  out[i*8 .. i*8+8) = u64_le(lane[i])   ->  the returned array<uint8_t,32>

u64_to_root(seed):                       # the 64-bit -> 256-bit adapter
  rng = StableRng(seed ^ 0x454D424552524F54)   # ROOT_DOMAIN
  root[i*8 .. i*8+8) = u64_le(rng.next())  for i in 0..3
```

`StableRng` is the pinned SplitMix64 advancing generator
(`state += 0x9E3779B97F4A7C15; return splitmix_mix(state)`) plus the
rejection-sampled `bounded(n)` described above. Pass algorithm versions are
independent fields, so changing one transform does not renumber every other
stream.

### Golden vectors and reproduction

The encoding above is the single source of truth. The `ember_pass` test target
carries **hard-coded golden bytes** for root-level, function-level, site,
distinct-purpose, fixed-seed-0, fixed-seed-`UINT64_MAX`, and initial
`StableRng` derivations, plus forward-vs-reverse order-independence and a
shared-`const`-deriver parallel check. Expected bytes are literals computed by
a separate reference script; the production algorithm is never duplicated inside
the test to recompute them, so the test pins the encoding rather than asserting
it equals itself. Changing any constant above (domain string, lane tags,
constants, field order, or endianness) breaks those literals and is a deliberate
algorithm-version bump.

### Per-purpose streams

A pass never derives a single stream for all of a site's choices. Each choice
has its own **purpose** string, domain-separated inside the `SeedRequest`, so a
draw for one purpose never reshuffles another and there is no single advancing
function-wide RNG. The purposes the shipped obfuscation passes use are:

| Purpose       | Used by                  | Picks                                       |
|---------------|--------------------------|---------------------------------------------|
| `select`      | every transform          | whether the site is selected (density)      |
| `variant`     | subst, mba_expand, const_encode | which identity / encoding form        |
| `constant`    | const_encode, deadcode   | the encoding key / junk-chain keys          |
| `truth`       | opaque_pred              | always-true vs always-false predicate       |
| `junk-count`  | opaque_pred              | the bogus-path junk immediate               |
| `string-key`  | str_encrypt (Red 7)      | the per-string encryption key               |

Site identity is the **stable original block ID + instruction ordinal** the
candidate occupied in the snapshot before any mutation. Snapshot the
candidate list before inserting any new instruction (so inserted instructions
are not recursively transformed this run) and re-locate each original by a
running inserted-count as the block grows. Two runs of the same function then
produce identical per-site streams regardless of how earlier sites shifted
indices.

```cpp
// Per-site selection: derive the "select" stream and accept the site iff a
// rejection-sampled draw in [0, 1_000_000) is below the configured density.
bool selected = (select_rng.bounded(1'000'000) < options.site_probability_ppm);
// Per-purpose variant stream (independent of the select stream):
ember::StableRng vrng = per_site_rng(options, pass_name, f, blk.id,
                                    original_ordinal, "variant");
uint64_t identity = vrng.next() & 1ULL;
```

### PolymorphicPassOptions and configured polymorphic factories

The immutable option record every configured obfuscation pass factory captures
lives in `src/polymorphic_options.hpp`:

```cpp
#include "polymorphic_options.hpp"

class ember::PolymorphicPassOptions {
    std::shared_ptr<const ember::SeedDeriver> seed_deriver;
    uint32_t algorithm_version = 1;            // independent per pass
    std::string engine_version;                // stable module/profile identities
    std::string module_id;                       //   needed by SeedRequest
    std::string build_profile_id;
    uint32_t site_probability_ppm = 0;         // 0 = no-op; 1'000'000 = all sites
    ember::PassGrowthLimits limits{};          // soft per-pass growth ceilings
};
```

`validate_polymorphic_options(opts)` / `make_polymorphic_options(...)` check the
record and return a structured `ExtensionStatus` / `ExtensionResult` (registry
`ember-polymorphic-options`) on:
- `site_probability_ppm > 1_000_000` (above 100%);
- `limits.growth_denominator == 0`;
- a growth ratio that overflows the instruction ceiling.
- a null `seed_deriver` (a configured factory must derive; the only null-deriver
  record is the no-op sentinel, which is never a functioning pass configuration).

A configured pass is a no-op when it cannot derive -- `seed_deriver() == nullptr`
**or** `site_probability_ppm() == 0`. This is the ZERO-DENSITY CONFIGURED path
(a profile that wants no diversification), NOT the bare `reg.add<T>()` legacy
path: the obf passes' DEFAULT constructors capture `legacy_defaults(pass_name)`
(a non-null deriver + the pass's prior per-pass eligibility density), so a bare
`reg.add<SubstitutionPass>("subst")` is a FUNCTIONING pass that transforms
every eligible Add -- preserving the prior `reg.add<T>()` behavior, not a
zero-density no-op.

Register a whole obfuscation extension with deterministic defaults through the
configured factory entry point, then expose a no-argument compatibility wrapper
that retains the existing pipeline names and eligibility behavior:

```cpp
// extensions/obf/ext_obf.hpp
namespace ember::ext_obf {
ember::ExtensionStatus register_passes(ember::EmberPassRegistry& reg,
                     const ember::PolymorphicPassOptions& options);
void register_passes(ember::EmberPassRegistry& reg);  // compat wrapper
}

void ext_obf::register_passes(ember::EmberPassRegistry& reg,
                              const ember::PolymorphicPassOptions& options) {
    // VALIDATING (Red 6): reject unvalidated options; register nothing on failure.
    if (auto st = ember::validate_polymorphic_options(options); !st) return st;
    reg.add_factory("mba_expand", [options]() {
        return ember::make_pass_concept(MBAExpansionPass{options});
    });
    // ...one add_factory per pass...
    return ember::make_extension_ok();
}
// The compat wrapper: every pass through its DEFAULT constructor, which
// captures legacy_defaults(pass_name) -- a functioning pass with the prior
// per-pass eligibility (NOT a no-op).
void ext_obf::register_passes(ember::EmberPassRegistry& reg) {
    reg.add<SubstitutionPass>("subst");
    reg.add<MBAExpansionPass>("mba_expand");
    // ...one reg.add<T>() per pass...
}
```
The factory captures `options` **by value**; every `create("mba_expand")`
returns a fresh `PassConcept` carrying its own constructed pass. The validating
configured overload rejects unvalidated options (registers nothing on failure).
The compat wrapper registers every pass through its DEFAULT constructor
(`reg.add<T>()` -> `legacy_defaults(pass_name)`), so the resulting passes are
FUNCTIONING with the prior per-pass eligibility, and existing
`register_passes(reg)` callers keep working unchanged.

### Per-purpose streams

Each configured pass derives INDEPENDENT per-site, per-purpose streams through
`src/seed_derivation.hpp` (a stable `SeedRequest` per site: the ORIGINAL block
ID + instruction ordinal + a domain-separated `purpose`). The purposes are
`select` (site-selection density), `variant` (which MBA identity / encode
form), `constant` (the encode key), `truth` (always-true vs always-false
opaque predicate), and `junk-count` (the dead-code junk immediates). There is
NO single advancing function-wide RNG: a draw for one site never reshuffles a
later site, so the transformed IR is a pure function of (source, options,
seed) and is reproducible. The site identity uses the ORIGINAL block ID +
ordinal (snapshotted before any mutation) so two runs of the same function
produce identical streams regardless of how earlier sites shifted indices.

## Registration and invocation

Expose one extension-shaped function:

```cpp
namespace my_extension {
void register_passes(ember::EmberPassRegistry& reg) {
    reg.add<MyPass>("my-pass");
}
}
```

`reg.add<PassT>(name)` default-constructs the pass and returns an
`ember::ExtensionStatus` that callers may ignore (existing
`reg.add<PassT>("name");` call sites stay source-compatible). Registration is
strict: an empty name or a duplicate name is rejected without storing or
replacing anything, and the first registration is retained. Inspect the
returned status when you need collision diagnostics:

```cpp
if (auto st = reg.add<MyPass>("my-pass"); !st) {
    // st.error->registry == "ember-pass"
    // st.error->name     == "my-pass"
    // st.error->message  == "pass name already registered"
}
```

### Configured factories

When a pass needs immutable constructor options (a seed, density, growth
caps, or any other configuration), register a configured factory with
`add_factory` and `make_pass_concept` instead of `add<T>`. The shipped
obfuscation passes share one such option record — `ember::PolymorphicPassOptions`
(see [PolymorphicPassOptions and configured polymorphic
factories](#polymorphicpassoptions-and-configured-polymorphic-factories));
custom passes may use their own option type with the same pattern:

```cpp
struct MyPass : ember::EmberPassInfoMixin<MyPass> {
    static constexpr const char* pass_name = "my-pass";
    MyOptions options;
    explicit MyPass(MyOptions o) : options(std::move(o)) {}
    ember::EmberPreserved run(ember::ThinFunction& f,
                              ember::EmberAnalysisManager& am);
};

void register_passes(ember::EmberPassRegistry& reg, const MyOptions& options) {
    reg.add_factory("my-pass", [options]() {
        return ember::make_pass_concept(MyPass{options});
    });
}
```

The factory captures `options` **by value** (an immutable copy owned by the
factory for the registry's lifetime). Every `create("my-pass")` — including
the calls `build_pipeline_from_string` makes while constructing a pipeline —
invokes the factory and returns a **fresh** `PassConcept` instance, so two
lookups yield distinct pass objects and each pipeline pass gets its own
constructed instance. A configured pass stays compatible with a
default-constructed one: both are type-erased into the same
`unique_ptr<PassConcept>` and flow through `create()`, `has()`, and the
pipeline parser identically.

Provide a default-argument overload so existing
`register_passes(reg)` callers keep working:

```cpp
void register_passes(ember::EmberPassRegistry& reg) {
    register_passes(reg, MyOptions{});
}
```

`add_factory` is strict in the same way as `add<T>`: it rejects empty names,
rejects a null/empty `std::function` factory, and rejects a duplicate name
without replacing the first registration. On rejection it returns an
`ExtensionStatus` carrying an `ExtensionError` (`registry == "ember-pass"`, the
offending `name`, and a `message`) and stores nothing, so the registry's
contents are unchanged.

### Deterministic listing

`reg.names()` returns the registered pass names sorted lexicographically, so
`--list-passes` output and any host-driven enumeration are deterministic and
independent of `unordered_map` iteration order.

### Pipeline grammar and transactional guarantees

The accepted initial grammar is deliberately small:

```text
pipeline := <empty> | name (',' name)*
```

`name` is alphanumeric plus `_` and `-` (the characters every shipped pass
name uses). Surrounding spaces and tabs around each name are permitted and
trimmed. An **entirely empty** specification is a successful no-op (no passes
are appended). The parser is **strict** and **transactional**:

- every token is resolved into temporary ownership *first*;
- the caller's `EmberPassManager` is mutated **only after the complete
  specification succeeds**;
- on any failure the manager's pass vector **and** its `PassInstrumentation`
  are left completely unchanged — nothing is moved, cleared, or silently
  replaced.

Rejected with a useful error written to `*err`:

- **empty elements** — middle (`a,,b`), leading (`,b`), trailing (`a,`), and
  whitespace-only (`a,   ,b`);
- **unsupported parentheses** (`a,(b)`, `a,b)`, `(a,b`,
  `flatten(subst,mba)`) — parameterized sub-pipelines are not supported in this
  first cut;
- **invalid characters / trailing junk** inside a token (`a,b!`, `a;b`,
  `a b`) — any character outside `[A-Za-z0-9_-]` makes the token invalid;
- **unknown names** (`a,nonexistent`) — reported as `unknown pass: "name"`;
- **a registered factory whose `operator()` returns `nullptr`** — reported
  distinctly as `pass factory returned null for "name"` (the name *is*
  registered, so this is not collapsed to the unknown-name diagnostic); a
  pipeline never silently omits a requested pass.

Two operations are provided:

- `build_pipeline_from_string(spec, reg, out, &err)` **appends** the resolved
  passes to any already in `out`. This is the existing public call and its
  append behavior is preserved.
- `replace_pipeline_from_string(spec, reg, out, &err)` **replaces** `out`'s
  passes with the resolved sequence. An empty spec replaces with the empty
  pipeline (success, `out` ends up with zero passes).

Both resolve into temporary storage and commit atomically. **Neither moves,
clears, or silently replaces the caller's `PassInstrumentation`** on success or
failure; only the pass vector is appended or exchanged. This means a host that
has installed tracing/timing/gating callbacks keeps them across a pipeline
build or rebuild.

### Host wiring

Call the registration function(s) while constructing the host registry:

```cpp
ember::EmberPassRegistry reg;
ember::ext_opt::register_passes(reg);
ember::ext_obf::register_passes(reg);
my_extension::register_passes(reg); // explicit: linking is not discovery

ember::EmberPassManager pm;
std::string error;
if (!ember::build_pipeline_from_string("constprop,my-pass,dce",
                                       reg, pm, &error)) {
    // report error
}
ctx.pass_manager = &pm;
ctx.enable_ir_backend = true;
```

A CLI host wired this way accepts:

```bash
ember run program.ember --passes constprop,my-pass,dce
```

The stock `ember_cli` registers only `ext_opt` and `ext_obf`; it reports
`unknown pass` for custom names until its registry setup calls the custom
registration function.

## Checked execution

The plain `run` / `run_to_fixpoint` return `EmberPreserved` and stay
source-compatible, but they now ALSO enforce the hard growth ceilings: a pass
that blows the IR past the absolute instruction cap or the 10× growth factor
simply stops the pipeline (later passes do not run). They do not print to
`stderr` — a host that needs the reason uses the checked path below.

The checked path returns a structured `PassRunReport` and never prints:

```cpp
ember::PassRunReport rep = pm.run_checked(f, am, opts);
// rep.stop_reason: Completed | Converged | RoundLimit | GrowthLimit |
//                  ValidationFailure | PassError | ExceptionError
// rep.pass_name:   the pass that was running when the run stopped
// rep.error:       a diagnostic on every non-clean stop (empty on success)
// rep.rounds:       1 for one-shot, N for run_checked_fixpoint
// rep.initial_count / rep.final_count: total instrs before / at stop
```

`run_checked` runs the pipeline once; `run_checked_fixpoint` runs rounds until
convergence, the round limit, or a failure. Both:

1. **validate the input** (`verify_thin_function_for_codegen`) before any pass
   — an already-malformed function is reported as `ValidationFailure` and no
   pass runs;
2. **snapshot the function** and run each pass under that snapshot;
3. **validate after each reported mutation** — a pass returning `none()` claims
   a change, so the verifier runs on its output; a pass returning `all()` claims
   no change and is not re-verified;
4. **enforce the instruction/growth ceilings** after each pass — a pass that
   exceeds `opts.max_instructions` or the growth ratio (clamped to the hard 10×)
   is reported as `GrowthLimit`;
5. **catch `PassError`** as a recoverable, expected failure (`stop_reason ==
   PassError`, the message carried) and **any other `std::exception`** as an
   unexpected failure (`stop_reason == ExceptionError`);
6. **roll back to the last verified state** on any failure — the function is
   byte-for-byte unchanged after a `ValidationFailure` / `GrowthLimit` /
   `PassError` / `ExceptionError`;
7. **stop** — later passes do not run after a failure.

`CheckedRunOptions` lets a caller request SMALLER ceilings than the hard caps
but never larger: `max_instructions = 0` means "use the hard cap",
`growth_numerator = 0` means "use the hard 10×". A tighter ratio (smaller
numerator or larger denominator) is honored; a looser one is clamped to 10×.

A pass signals a structured, recoverable failure by throwing `PassError`:

```cpp
if (precondition_unmet)
    throw ember::PassError("my-pass", "site requires a dominator tree");
```

`PassError` is the ONLY exception type the adapter treats as expected. Anything
else (a `std::runtime_error`, an `assert`, …) is an unexpected exception and
becomes `ExceptionError`. Neither crosses the compile boundary — see below.

### The codegen verifier

`verify_thin_function_for_codegen` is the **in-memory** verifier for pass
output (and the pre-regalloc/emit gate). It covers the same CFG / frame /
rodata / VReg / op-shape invariants as the disk-facing
`validate_thin_function`, plus a frame-plan consistency check
(`next_local_off` must stay within `frame_size`). It does NOT validate the
regalloc result — `thf.ra` is a JIT-time annotation produced by
`run_regalloc`, not an IR invariant a pass produces; a stale or bogus `ra` is
cleared by `compile_func_checked` before the single regalloc/emit stage. Tests
must say which verifier they use; "validator-clean" alone is not a semantic
claim.

### The compile boundary

`compile_func_checked` runs the configured pass manager in checked mode between
`lower_function` and regalloc/emit and returns a structured `CompileResult`
(backend used, fallback/failure reason, transformed IR when requested, the pass
reports, and the emitted `CompiledFn`):

- it honors `CodeGenCtx::analysis_manager` (passes receive the host's manager,
  not a freshly-constructed local);
- a validation / growth / pass / exception failure **cannot reach
  `run_regalloc` or `emit_x64`** — no executable is produced on failure;
- stale / pre-existing regalloc on the lowered function is cleared before the
  single allowed allocation stage;
- **exceptions do not cross the boundary** — a thrown pass or backend error
  becomes a structured `CompileResult` failure, not a propagated exception;
- when the IR backend is unavailable for a function (obf / try/catch /
  coroutine / empty lowering) it falls back to the tree-walker and reports the
  reason.

The reported `backend` is the **actual** backend used: `CompileBackend::
IRBackend` whenever the IR path ran — including when `ctx.pass_manager` is
`nullptr` (lower → no passes → regalloc → emit, still the IR backend, with the
checked pre-regalloc/emit verification gate still applied) — and
`CompileBackend::TreeWalker` only on a real fallback (IR backend disabled,
unavailable for the function, or `lower_function` produced no blocks). A null
`pass_manager` is NOT reported as TreeWalker.

To request the post-pass IR back, set `ctx.request_transformed_ir = true`; the
checked path then populates `CompileResult::transformed` with a deep copy of
the `ThinFunction` as it stood after the checked pass pipeline and AFTER the
pre-regalloc/emit verification gate (so a requested `transformed` is always
clean for codegen). With `request_transformed_ir = false` (the default)
`transformed` is left empty (the opt-out path; no extra copy).

`compile_func(...)` stays source-compatible as the legacy wrapper returning just
`CompiledFn` — and is now **exception-safe**: it delegates to the same internal
checked implementation as `compile_func_checked` (in legacy, non-checked mode)
and catches any pass or backend exception, returning an empty `CompiledFn`
(`exec == nullptr`) on failure rather than propagating the exception across
the public compile boundary. A pass that throws `PassError` or any other
`std::exception` through the legacy `compile_func` therefore produces an empty
`CompiledFn`, never a propagated exception.

## Composition and tests

Simplifying optimization normally precedes obfuscation/injection. A later
`constprop`, `cse`, `dce`, or CFG simplifier may deliberately erase a semantic
NOP or obfuscating identity. Test each pass alone and in its intended pipeline.
At minimum cover unchanged/changed preservation results, value and trap
 equivalence, validation, serialization round-trip, same-seed equality,
pinned-seed variation, compilation-order independence, instrumentation skip
behavior for optional passes, and growth limits.

The `polymorphic_pass_test` executable (`examples/polymorphic_pass_test.cpp`)
is the Red 6 / Red 7 coverage driver for the seven obfuscation transforms
(subst, mba_expand, const_encode, opaque_pred, deadcode, block_split,
str_encrypt). It is built by CMakeLists.txt **without**
`add_test` so the filtered CTest total is unchanged; run it explicitly, e.g.
`./buildt/polymorphic_pass_test`. It covers options validation, configured
factory registration, no-op/changed preservation, same-seed serialized Thin IR
equality, two-pinned-seed structural variation, baseline differential
execution, serialize/deserialize round-trip, stale-regalloc invalidation, exact
growth boundaries + stop-before-site atomicity, widths 1/2/4/8, empty and
no-candidate functions, straight-line / diamond / loop / long-block CFGs, and
the full str_encrypt matrix (per-site `site_selected` density gating, seeded
per-site nonzero keys, plaintext absence from rodata AND serialized v2 blobs,
REAL overlapping/repeated/empty literals via hand-built explicit (addend,len)
specs, distinct nonoverlapping data/slice frame regions, an atomic
private-region-per-site rodata rebuild with original-plaintext scrubbing for
every selected site, exact/repeated/partial-overlap execution that inspects
every StringDecrypt key + verifies each encrypted region == plaintext XOR key,
and no double encryption).
