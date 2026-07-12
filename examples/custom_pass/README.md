# Custom pass examples

These are copyable user examples for Ember's `ThinFunction` pass system. They
are intentionally small; the complete authoring contract is
[`docs/PASS_AUTHORING.md`](../../docs/PASS_AUTHORING.md).

| File | Registered name | Demonstrates |
|---|---|---|
| `minimal_pass.cpp` | `example-minimal` | bare pass shape, block/instruction traversal, no-op preservation |
| `nop_injection_pass.cpp` | `example-nop-injection` | deterministic semantic-NOP insertion, fresh VRegs/frame slots, regalloc invalidation |
| `block_merge_pass.cpp` | `example-block-merge` | predecessor analysis, instruction moves, terminator replacement, block erasure and edge remapping |
| `custom_pass_example.ember` | — | a script and commented `--passes` commands |

Declarations and the standard `register_passes` entry point are in
`custom_passes.hpp`.

## Lifecycle in one page

1. Register each concrete pass under a stable, unique name. The registry
   creates a fresh default-constructed instance for each pipeline occurrence.
2. The host parses the comma-separated `--passes` string in user order.
3. `run(ThinFunction&, EmberAnalysisManager&)` analyzes and optionally mutates
   one function. The analysis manager is currently empty, so examples scan
   directly.
4. Return `EmberPreserved::all()` only when unchanged, otherwise `none()`.
5. Validate changed output, then require it to serialize, deserialize, validate,
   and emit successfully.

## Mutation checklist

- Preserve observable values, traps, memory effects, and call order.
- VReg zero is reserved; Thin IR is not guaranteed SSA.
- Allocate fresh VRegs and negative in-frame spill slots. Update
  `next_local_off`, aligned `frame_size`, and `declared_max_vreg`; clear stale
  `f.ra` after instruction or CFG mutation.
- Preserve relevant type/width/frame/source metadata on rewrites.
- Slice/lambda values consume consecutive VRegs. Keep call argument vectors
  parallel. Do not treat `CopyBytes::dst` as a normal definition.
- Every block needs a terminator. Keep entry at ID zero, IDs unique/in-range,
  and rewrite every target after block insertion/removal.
- Respect `IR_MAX_*` serializer limits. Do not run a growth pass to fixpoint.

## Side effects and safe “NOPs”

Ember has no `ThinOp::Nop`. The injection example uses pure arithmetic into new,
unused values: a random constant followed by `x + 0`. It never uses calls,
division, guards, loads, existing destinations, or externally visible stores.
A later `dce` may remove it; that is expected.

Calls, global/indirect stores, copies, aggregate initialization, string decrypt,
and all safety checks are effectful. `StoreFrame` is local but remains a memory
write. Until Ember exposes a public exhaustive effect classifier, custom passes
must classify uncertain operations conservatively. See the central discussion
in `docs/PASS_AUTHORING.md` before moving or deleting instructions.

## Analysis and serialization

The current preservation model is all-or-nothing: changed instruction passes
invalidate use-def/value/liveness facts; CFG edits also invalidate predecessor,
dominator, loop, and block-order facts. Frame/rodata edits invalidate layout
summaries. Today those analyses are not cached, but returning `none()` preserves
the future contract.

Persisted operation ordinals are stable: append, never renumber, `ThinOp` values.
Do not store runtime pointers in serializable fields. Native identity is
`meta.native_name`, types use the canonical codec, and rodata addends must stay
in bounds. Test `pass -> validate -> serialize -> deserialize -> validate`.

## Deterministic seed policy

`NopInjectionPass` uses a fixed default base seed, derives a function-local seed
from pass name/function name/function slot with FNV-1a, and uses a locally
specified SplitMix64 stream. Candidate order is block/instruction vector order.
The same source, tool version, and seed therefore produce the same transform
independently of function compilation order. Seeds select layout only; they are
not secrets and must never affect semantics.

The current stock CLI has no `--pass-seed`; change the pass's `seed` in a custom
host or provide your own configured construction path. Do not use `std::hash`
or unordered-container iteration for reproducible artifacts.

## Build and host registration

This directory's `CMakeLists.txt` defines `ember_custom_pass_examples`. Add it
from Ember's root or copy the same target into your embedding project:

```cmake
add_subdirectory(examples/custom_pass)
target_link_libraries(my_ember_host PRIVATE ember_custom_pass_examples)
```

Then include `custom_passes.hpp` and explicitly register it beside the built-in
extensions:

```cpp
EmberPassRegistry pass_reg;
ext_opt::register_passes(pass_reg);
ext_obf::register_passes(pass_reg);
ember::examples::custom_pass::register_passes(pass_reg);

EmberPassManager pass_pm;
std::string error;
if (!build_pipeline_from_string(
        "example-minimal,example-nop-injection,example-block-merge",
        pass_reg, pass_pm, &error)) {
    // report error
}
ctx.pass_manager = &pass_pm;
ctx.enable_ir_backend = true;
```

**Linking is not runtime discovery.** The unmodified stock CLI registers only
`ext_opt` and `ext_obf`, so it correctly reports:

```text
ember: --passes: unknown pass: "example-minimal"
```

After adding the registration call to your host, invoke:

```bash
ember run examples/custom_pass/custom_pass_example.ember \
  --passes example-minimal,example-nop-injection,example-block-merge

# Built-in DCE removes the deliberately unused semantic-NOP chain:
ember run examples/custom_pass/custom_pass_example.ember \
  --passes example-nop-injection,dce,example-block-merge
```

Use `EmberPassManager::run()`, not `run_to_fixpoint()`, for the injection pass.
