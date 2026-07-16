# Custom ThinIR pass examples

These examples are copyable C++ extensions for Ember's `ThinFunction` pass
system. The full contract is [`docs/PASS_AUTHORING.md`](../../docs/PASS_AUTHORING.md).

| File | Registered name | Demonstrates |
|---|---|---|
| `minimal_pass.cpp` | `example-minimal` | concrete pass shape and unchanged preservation |
| `nop_injection_pass.cpp` | `example-nop-injection` | deterministic pure-NOP growth, fresh VRegs/frame storage, regalloc invalidation |
| `block_merge_pass.cpp` | `example-block-merge` | predecessor checks, instruction moves, terminator replacement, block deletion, edge remapping |
| `custom_passes.hpp` | — | declarations and explicit `register_passes` |
| `custom_pass_example.ember` | — | sample script and invocation notes |

## Build

The directory defines `ember_custom_pass_examples`. It is not added by the root
build automatically; include it from an embedding CMake project:

```cmake
add_subdirectory(path/to/ember/examples/custom_pass custom_pass_build)
target_link_libraries(my_ember_host PRIVATE ember_custom_pass_examples)
```

Or build the root test that compiles all three examples and checks registration:

```bash
cmake --build build --target pass_registry_coverage_test
ctest --test-dir build -R '^pass_registry_coverage$' --output-on-failure
```

## Register and run

Linking does not register names. The host must call the example registration
beside the 25 built-in passes:

```cpp
ember::EmberPassRegistry registry;
ember::ext_opt::register_passes(registry); // 18 optimization passes
ember::ext_obf::register_passes(registry); // 7 obfuscation passes
ember::examples::custom_pass::register_passes(registry);

ember::EmberPassManager manager;
std::string error;
if (!ember::build_pipeline_from_string(
        "example-minimal,example-nop-injection,example-block-merge",
        registry, manager, &error)) {
    // report error
}
ctx.pass_manager = &manager;
ctx.enable_ir_backend = true;
```

A correspondingly modified host can run:

```bash
./my_ember_host run examples/custom_pass/custom_pass_example.ember \
  --passes example-minimal,example-nop-injection,example-block-merge
```

The unmodified stock `ember_cli` registers only `ext_opt` and `ext_obf`, so it
correctly reports an unknown pass for `example-minimal`.

## Authoring checklist

- Return `EmberPreserved::all()` only when unchanged; return `none()` after a
  mutation.
- VReg 0 is reserved and ThinIR is not guaranteed SSA.
- Prefer `ThinIRMutation` for transactional VReg/frame/CFG growth. If mutating
  manually, update `declared_max_vreg`, `next_local_off`, aligned `frame_size`,
  and clear stale `f.ra`.
- Slice/lambda values use consecutive VRegs. Keep `args`, `arg_frame_offs`, and
  `arg_types` parallel. `CopyBytes::dst` is not an ordinary definition.
- Keep block 0 as entry, every block terminated, IDs unique/in-range, and every
  edge rewritten after insertion/deletion.
- Classify effects through `classify_thin_effects` and
  `removable_if_result_dead`; calls, guards, aggregate/string operations,
  indirect/global memory, and uncertain operations are not semantic NOPs.
- Validate changed output with `verify_thin_function_for_codegen`, then test
  serialize -> deserialize -> validate -> emit.
- Respect serializer and pass growth limits. Do not run the injection example
  to fixpoint.

## Determinism

`NopInjectionPass` derives a function-local stream from a fixed seed and stable
function identity, with candidate order equal to block/instruction vector order.
Seeds affect layout only, never behavior. The stock CLI now has
`--pass-seed U64`, but that seed configures the built-in profile/polymorphic
factories; it does not automatically rewrite this custom pass's constructor.
Register your own configured factory if the example seed must be host-selectable.

A later `dce` may remove the deliberately unused NOP chain. That is expected and
is a useful composition test.
