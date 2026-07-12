# Authoring custom Ember IR passes

This guide is the user-facing contract for passes over `ThinFunction`. Complete,
copyable examples live in [`examples/custom_pass/`](../examples/custom_pass/).
The framework headers are `src/ember_pass.hpp`, `ember_pass_registry.hpp`, and
`ember_pass_pipeline.hpp`.

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
2. **Pipeline construction:** `build_pipeline_from_string` creates passes in the
   user's comma-separated order and rejects an unknown name.
3. **Analysis:** inspect the function or request cached analyses. The analysis
   manager is currently a stub, so shipped passes scan the IR directly.
4. **Mutation:** change instructions, CFG, frame layout, or rodata while keeping
   every IR invariant and observable behavior intact.
5. **Preservation:** return `EmberPreserved::all()` only when nothing changed;
   return `none()` after any current transform.
6. **Invalidation:** selective cached invalidation is future work. Until then,
   `none()` is the conservative contract for all mutations.
7. **Validation:** tests should validate after each mutating pass.
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
  `v+1`; scalar allocation must not collide with either word.
- A producing instruction normally needs a valid negative frame spill offset.
  Grow `frame.next_local_off` and aligned `frame.frame_size`, update
  `declared_max_vreg`, and clear `f.ra` when mutation makes regalloc stale.
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

There is not yet a public exhaustive effects API, so custom authors must be
conservative. The optimizer's current `is_side_effecting` table treats these as
side-effecting:

- all calls (`CallNative`, `CallScript`, `CallIndirect`, `CallCrossModule`);
- global and indirect stores (`StoreGlobal`, `StoreAddr`);
- `CopyBytes`, aggregate initializers, and `StringDecrypt`;
- bounds, overflow, depth, budget, and call-target checks.

`StoreFrame` is local memory, but it is still a write and cannot be freely moved
or coalesced. Mutable loads cannot be moved across unknown calls/stores merely
because they produce a value. Pure scalar arithmetic may be removed when its
result is dead, provided it cannot trap; avoid injecting `Div`, `Mod`, calls,
checks, loads, or writes as “junk.” Until native purity is explicit ABI
metadata, classify every call as effectful.

When a new `ThinOp` is added, update every effect classifier and exhaustive op
switch in the same change. Do not annotate an individual instruction as pure to
bypass the central operation semantics.

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

Seeded transforms should derive a local stream from a base seed, stable pass
name, function name, and function slot. This makes output independent of
function compilation order and thread scheduling. Keep candidates in
block/instruction vector order; do not use `unordered_map` iteration as random
input.

Use pinned algorithms such as byte-defined FNV-1a plus SplitMix64, not
`std::hash` or implementation-dependent standard distributions, when binary
reproducibility matters. A seed controls layout variation, never semantics, and
is not a cryptographic secret. Random-each-build mode should be explicit and
print/store the generated seed. Record source/tool version, options, and seed
with artifacts.

The current registry default-constructs pass instances and the stock CLI has no
`--pass-seed` option. The NOP example therefore contains a fixed default seed.
An embedding host may register a differently configured concrete pass directly
in a manager; configurable registry factories/CLI seed plumbing remain future
API work and must not be implied by documentation.

## Registration and invocation

Expose one extension-shaped function:

```cpp
namespace my_extension {
void register_passes(ember::EmberPassRegistry& reg) {
    reg.add<MyPass>("my-pass");
}
}
```

Call it while constructing the host registry:

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

## Composition and tests

Simplifying optimization normally precedes obfuscation/injection. A later
`constprop`, `cse`, `dce`, or CFG simplifier may deliberately erase a semantic
NOP or obfuscating identity. Test each pass alone and in its intended pipeline.
At minimum cover unchanged/changed preservation results, value and trap
 equivalence, validation, serialization round-trip, same-seed equality,
pinned-seed variation, compilation-order independence, instrumentation skip
behavior for optional passes, and growth limits.
