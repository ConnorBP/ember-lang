# Custom ThinIR code-generation passes

Ember's supported customization point is the typed `ThinFunction` pass system,
not post-hoc byte patching. A pass runs after AST lowering and before register
allocation/x64 emission:

```text
source → tokens → AST → sema → ThinIR → checked pass pipeline
       → register allocation → x64 emitter → executable page
```

This chapter develops a deterministic instruction-substitution pass, registers
it as an extension, and differential-tests the generated result. The complete,
build-tested implementation is
[`examples/custom_pass/mov_substitute_pass.cpp`](../../../examples/custom_pass/mov_substitute_pass.cpp)
with [`mov_substitute_test.cpp`](../../../examples/custom_pass/mov_substitute_test.cpp).

> **Important abstraction boundary.** ThinIR contains virtual registers and
> semantic operations. Physical x64 registers, `push`, `pop`, `xchg`, and
> address-encoding details do not exist yet. Express substitutions as ThinIR
> equivalences when possible. If an exact x64 sequence is required, extend
> `ThinOp` append-only and implement/verifiy its lowering in `thin_emit.cpp`;
> do not inject opaque bytes that bypass relocations, safety checks, or the IR
> validator.

## 1. Pass-system overview

Two broad pass families use the same interface:

- **Optimization:** value-preserving and usually reduces cost/size
  (`constprop`, `dce`, `gvn`, `licm`, `peephole`, and others).
- **Obfuscation/diversification:** semantic-preserving while intentionally
  changing representation or increasing complexity (`subst`, `mba_expand`,
  `const_encode`, `opaque_pred`, `deadcode`, `str_encrypt`, `block_split`).

A pass is a value type with metadata and `run`:

```cpp
struct MyPass : ember::EmberPassInfoMixin<MyPass> {
    static constexpr const char* pass_name = "my-pass";
    ember::EmberPreserved run(ember::ThinFunction& function,
                              ember::EmberAnalysisManager& analyses);
};
```

Return `EmberPreserved::all()` only when nothing changed; return `none()` after
any mutation. `is_required = true` bypasses instrumentation skip callbacks but
not validation or growth limits. Most custom passes should remain optional.

Pipelines execute in listed order. This matters:

```text
constprop → my-substitution → dce
```

creates substitution candidates after folding, then lets DCE remove dead junk.
The reverse order has different output. Do not run a complexity-increasing pass
to fixpoint unless it is idempotent. The CLI's `light`, `balanced`, and `heavy`
profiles are named recipes, not hidden modes; a custom host can define its own
recipe string.

Use `run_checked` or `compile_func_checked`. Checked execution validates input
and each reported mutation, enforces absolute and growth ceilings, catches
`PassError`/`std::exception`, rolls back on failure, and prevents invalid IR
from reaching regalloc or emission.

## 2. ThinIR essentials

`ThinFunction` contains:

- `blocks`: vector of `ThinBlock`; block zero is entry;
- `frame`: fixed frame plan and GC slot metadata;
- `ret_type`, `rodata`, relocations, slot, and serializability metadata;
- `declared_max_vreg`: serialized VReg bound;
- `ra`: transient register-allocation result, which a mutating pass invalidates.

Each block has a stable `id`, an instruction vector, and one non-`None`
terminator (`Jmp`, `Branch`, `Return`, or `Trap`). `ThinInstr` is a
three-address record:

```cpp
struct ThinInstr {
    ThinOp op;
    VReg dst, src1, src2;
    ThinImm imm;
    ThinMeta meta;                  // width, type, frame slot, native name…
    std::vector<VReg> args;         // calls
    std::vector<int32_t> arg_frame_offs;
    std::vector<const Type*> arg_types;
    const Type* ret_type;
    void* native_fn;
    Loc loc;
};
```

Common `ThinOp`s include constants (`ConstInt`, `ConstFloat`), movement and
memory (`Move`, `LoadFrame`, `StoreFrame`, `LoadGlobal`, `StoreGlobal`,
`CopyBytes`), integer/float arithmetic, `Cmp`, `Cast`, calls, address/aggregate
operations, bounds/depth/budget guards, and exception operations. The enum's
numeric values are serialized: append new operations only; never reorder them.

Conventions that substitution code must preserve:

- VReg zero is invalid/absent and also an aggregate-call sentinel.
- ThinIR is three-address but **not guaranteed SSA**; a VReg may be redefined.
- An arithmetic instruction with `src2 == 0` uses `imm.i` as its second operand.
- Slice/lambda results use two consecutive VRegs.
- Aggregate values use frame offsets rather than ordinary scalar VRegs.
- Producing instructions generally pin a result to `meta.frame_off`.
- Calls, guards, global/indirect memory, and exception operations are effectful.
- Every CFG edge and block ID must remain valid.

Inspect IR with `dump(function)`. For removal/reordering decisions use
`classify_thin_effects` and `removable_if_result_dead`; never infer purity from
`dst` alone.

## 3. What “MOV substitution” means in ThinIR

At the source/ThinIR level, the useful analogue of `mov reg, imm` is a
`ConstInt` definition. The x64 emitter may later choose a physical destination
or a spill slot. Therefore this tutorial substitutes:

```text
v = ConstInt C
```

with one of these modulo-2^64 identities:

```text
k = ConstInt K              base = ConstInt (C - D)
v = Xor k, (C xor K)        v    = Add base, D
```

They model the intent behind forms such as:

```asm
xor reg, reg
or  reg, imm
```

without clobbering a physical register that does not yet exist. `xor reg, reg;
or reg, imm` is only equivalent when the immediate encoding/width can represent
the full target value and flag behavior is irrelevant. ThinIR's typed identity
is safer and lets the emitter select legal encodings.

Likewise, `push imm; pop reg` is an x64-level move substitute, but introducing
it directly before register allocation would violate stack-model assumptions,
unwind/trap boundaries, and Win64 alignment. If you genuinely need it:

1. append a dedicated `ThinOp` rather than emitting bytes from a pass;
2. define exact width/sign-extension and flags semantics;
3. reject stack-sensitive/guard/call adjacency sites;
4. implement it in `thin_emit.cpp` with balanced stack accounting;
5. extend validators, serializer/deserializer, effect classification, dumps,
   and differential ABI tests.

For ordinary diversification, the constant identities above are preferable.

## 4. Semantic junk and equivalence classes

The example adds work only on a **fresh, dead VReg**:

```text
junk = Move real_result
junk = Move junk
```

and randomly changes the second operation among:

```text
Move junk          # mov r, r analogue
Add  junk, 0       # add r, 0
Sub  junk, 0       # sub r, 0
Xor  junk, 0       # xor r, 0 (not xor r, r)
Or   junk, 0       # or r, 0
```

An x64 `xchg reg, reg` or `lea reg, [reg+0]` is also value-preserving, but
ThinIR has no `Xchg` and its `FieldAddr`/address ops are not generic integer
LEA. Add a dedicated operation and emitter support if exact encodings are the
goal. `Move v,v` is the portable ThinIR self-move analogue.

Useful equivalence-table design:

```cpp
enum class Identity { MoveSelf, AddZero, SubZero, XorZero, OrZero };
struct Rule {
    Identity identity;
    ember::ThinOp op;
    int64_t immediate;
    bool needs_fresh_dead_destination;
};
constexpr Rule rules[] = {
    {Identity::MoveSelf, ember::ThinOp::Move, 0, true},
    {Identity::AddZero,  ember::ThinOp::Add,  0, true},
    {Identity::SubZero,  ember::ThinOp::Sub,  0, true},
    {Identity::XorZero,  ember::ThinOp::Xor,  0, true},
    {Identity::OrZero,   ember::ThinOp::Or,   0, true},
};
```

Other identities require stronger preconditions:

| Proposed sequence | Correctness conditions |
|---|---|
| `or r,-1; and r,0; or r,val` | final immediate represents `val`; flags dead; no observer between operations |
| `push src; pop dst` | same width; balanced stack; legal encoding; no guard/call/unwind boundary; flags unaffected |
| `xor r,r; or r,val` | old `r` dead; immediate width/legal encoding; flags dead |
| `lea r,[r+0]` | integer/pointer width and address semantics match; flags intentionally unchanged |

IR-level arithmetic does not expose x64 flags as values, but trap ordering,
memory effects, spill writes, and later operations are still observable. The
safest junk destination is freshly allocated and unused. A later DCE pass may
remove it; omit DCE after injection if retaining junk is the profile's goal.

## 5. Register-aware substitution and liveness

The current `EmberAnalysisManager` is intentionally a stub: there is no public
cached liveness result to request. Register allocation runs **after** passes, so
there are no physical registers to mark safe. Claims that a custom pass can
query built-in ThinIR liveness today are incorrect.

Use one of these conservative strategies:

1. **Fresh VRegs (recommended):** `ThinIRMutation::allocate_scalar` creates a
   noncolliding VReg and frame home. It is dead unless the pass explicitly adds
   a later use.
2. **Local use scan:** only reuse a VReg after proving no subsequent instruction,
   call argument, or terminator uses it and accounting for redefinitions.
3. **Implement dataflow liveness:** compute `use`/`def`, CFG successors, then
   solve `live_out[B] = union(live_in[S])` and
   `live_in[B] = use[B] union (live_out[B] - def[B])` to a fixpoint. Walk each
   block backward to obtain per-instruction live sets.

A conservative scalar use enumerator must include `src1`, `src2`, every
`args[]` VReg, terminator `cond`/`ret`, exceptional/control-flow edges, and
operation-specific conventions (`CopyBytes::dst` is a use, not a normal def).
ThinIR is not SSA, so `dst` kills only according to the operation's actual
semantics. Use `thin_effects.hpp` to avoid overlooking implicit frame reads and
writes.

Even with VReg liveness, do not hand-select a physical scratch register. The
linear-scan allocator owns that decision and may spill. For substitutions that
must use an x64 scratch register, add an emitter-level pseudo-op whose lowering
uses the emitter's documented scratch discipline, or run a separate,
well-specified post-regalloc machine-IR stage (Ember does not currently expose
one).

## 6. Transactional mutation

Production passes should mutate through `ThinIRMutation`:

```cpp
ember::ThinIRMutation mutation(function, ember::PassGrowthLimits{});
if (!mutation.reserve_site(/*vregs*/2, /*frame*/16,
                           /*instructions*/3, /*blocks*/0).ok())
    return ember::EmberPreserved::all();
auto tmp = mutation.allocate_scalar(in.meta.type, in.meta.width);
if (!tmp.ok()) return ember::EmberPreserved::all();
// Insert/rewrite instructions with tmp.get().vreg and frame_off.
mutation.record_added_instructions(3);
if (!mutation.commit().ok()) return ember::EmberPreserved::all();
return ember::EmberPreserved::none();
```

The helper snapshots the function, centrally allocates VRegs (including paired
values), checks frame overlap/alignment and hard/soft growth limits, supports
CFG splitting/remapping, updates `declared_max_vreg`, grows the aligned frame,
and clears stale `f.ra` on commit. Destruction without commit restores the
snapshot. Call `reserve_site` with the whole site's worst case **before** any
partial mutation.

Snapshot candidate locations before inserting instructions so a pass does not
recursively transform its own output. Process same-block candidates in reverse
ordinal order or carefully track insertion offsets. Derive random decisions
from stable function/block/original-ordinal identity rather than one global
advancing RNG; output then remains reproducible under parallel compilation.

## 7. Complete pass walkthrough

The checked-in pass is also compiled/executed by the existing
`pass_registry_coverage` CMake test, so the in-tree example remains on a CTest
gate without modifying the root build script. It does the following:

1. Snapshot every 64-bit integer `ConstInt` site.
2. Traverse sites in reverse vector order.
3. Preflight two fresh VRegs, 16 frame bytes, and three net added instructions.
4. Derive a deterministic per-site stream from the configured seed, function
   name, original block ID, and original ordinal.
5. Choose XOR encoding or add/delta encoding.
6. Preserve the original destination, type, width, frame home, and source
   location on the replacement instruction.
7. Allocate a fresh junk VReg and append a self/zero identity on that dead value.
8. Commit frame/VReg bookkeeping, which invalidates stale regalloc.

Core shape (abridged only for prose; the linked `.cpp` is complete):

```cpp
struct MovSubstitutePass : EmberPassInfoMixin<MovSubstitutePass> {
    static constexpr const char* pass_name = "example-mov-substitute";
    uint64_t seed = 0x4d4f565f53554253ULL;

    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        // Snapshot eligible {block, ordinal} sites, then process in reverse.
        ThinIRMutation mutation(f, PassGrowthLimits{});
        // reserve_site(2, 16, 3, 0)
        // allocate_scalar() for encoded constant and dead junk
        // rewrite ConstInt C as either:
        //   ConstInt K; Xor K, (C xor K)
        // or:
        //   ConstInt (C-D); Add base, D
        // append Move-to-junk plus Move/Add/Sub/Xor/Or identity by zero
        // record_added_instructions(3)
        if (!mutation.commit().ok()) return EmberPreserved::all();
        return EmberPreserved::none();
    }
};
```

Read the complete source before copying it; its bit-preserving `uint64_t` ↔
`int64_t` conversion avoids implementation-defined out-of-range casts, and its
reverse traversal is part of correctness.

## 8. Register and add to a pipeline

Configured factory:

```cpp
void register_mov_substitute_pass(ember::EmberPassRegistry& registry,
                                  uint64_t seed) {
    registry.add_factory("example-mov-substitute", [seed]() {
        MovSubstitutePass pass;
        pass.seed = seed;
        return ember::make_pass_concept(std::move(pass));
    });
}
```

Host wiring:

```cpp
ember::EmberPassRegistry registry;
ember::ext_opt::register_passes(registry);
ember::ext_obf::register_passes(registry);
ember::examples::custom_pass::register_mov_substitute_pass(registry, 42);

ember::EmberPassManager manager;
std::string error;
if (!ember::build_pipeline_from_string(
        "constprop,example-mov-substitute", registry, manager, &error))
    throw std::runtime_error(error);

ember::EmberAnalysisManager analyses;
codegen.enable_ir_backend = true;
codegen.pass_manager = &manager;
codegen.analysis_manager = &analyses;
codegen.request_transformed_ir = true; // optional inspection
ember::CompileResult result = ember::compile_func_checked(function, codegen);
if (!result.ok) /* report structured failure */;
```

`build_pipeline_from_string` appends transactionally;
`replace_pipeline_from_string` replaces transactionally. Both preserve
instrumentation callbacks and reject malformed/unknown names without partially
mutating the manager.

To integrate with an application profile, define a stable recipe such as:

```text
light-custom = constprop,example-mov-substitute
```

and expand that name in the host before pipeline construction. The stock CLI
cannot discover a linked custom pass automatically; modify its pass-registry
setup or provide your own host executable.

## 9. CMake integration

The example directory can expose the pass as a library:

```cmake
add_library(ember_mov_substitute_pass STATIC mov_substitute_pass.cpp)
target_include_directories(ember_mov_substitute_pass PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${EMBER_ROOT}/src)
target_link_libraries(ember_mov_substitute_pass PUBLIC ember_frontend)
target_compile_features(ember_mov_substitute_pass PUBLIC cxx_std_17)

add_executable(mov_substitute_test mov_substitute_test.cpp)
target_link_libraries(mov_substitute_test PRIVATE
    ember ember_frontend ember_mov_substitute_pass)
add_test(NAME mov_substitute COMMAND mov_substitute_test)
```

The repository root intentionally keeps documentation examples independent of
its production targets. Out-of-tree hosts can use
`add_subdirectory(examples/custom_pass)` and add this source to their custom
pass target.

## 10. Testing semantic preservation

The checked test constructs ThinIR for `100 + 77`, runs the pass through
`run_checked`, emits and executes both original and transformed functions, and
requires both to return **177**:

```bash
cmake --build build --target mov_substitute_test
ctest --test-dir build -R '^mov_substitute$' --output-on-failure
```

Without adding the optional CMake target, the files can be compiled directly:

```bash
g++ -std=c++17 -Isrc -Iexamples/custom_pass \
  examples/custom_pass/mov_substitute_pass.cpp \
  examples/custom_pass/mov_substitute_test.cpp \
  -Lbuild -Wl,--start-group -lember_frontend -lember -Wl,--end-group \
  -o build/mov_substitute_test
./build/mov_substitute_test
```

A serious pass suite should add:

1. **Differential source corpus:** compile each function with no pass and with
   the pass over boundary constants (`0`, `-1`, signed min/max, random 64-bit
   patterns), branches, loops, calls, and spills; compare return values and
   host-visible side effects.
2. **Many deterministic seeds:** output may differ, behavior may not. Record a
   failing seed for reproduction.
3. **Checked validation:** require `PassStopReason::Completed`, then run
   `verify_thin_function_for_codegen` explicitly in focused unit tests.
4. **Round trip:** serialize transformed ThinIR, deserialize, validate, emit,
   and compare again. A JIT-only success is insufficient for `.em` support.
5. **Composition/order:** test before and after `constprop`, `dce`, regalloc,
   and built-in obfuscation passes. Never assume another pass preserves your
   inserted junk.
6. **Limit/rollback:** force tiny `PassGrowthLimits`; assert no partial site and
   byte-for-byte rollback.
7. **Determinism:** same seed and stable identity must produce the same
   transformed dump regardless of function compilation order.
8. **No TreeWalker fallback:** when the test intends to exercise ThinIR, require
   `CompileBackend::IRBackend` in `CompileResult`.

## 11. When exact x64 substitution is unavoidable

`x64_emitter.hpp` exposes low-level encoders such as `mov_reg_reg`,
`mov_reg_imm64`, `push`, `pop`, and `lea_reg_mem_disp`, but a `ThinFunction`
pass does not receive the emitter and should not bypass it. An exact machine
transform is an engine change with a larger maintenance contract:

- define a typed pseudo-op or a post-regalloc machine representation;
- preserve Win64 nonvolatile registers, stack alignment, shadow space, hidden
  aggregate-return pointers, context register `r14`, and keyed route `r15`;
- preserve trap/check ordering and GC shadow-stack records;
- teach effect classification, dumps, structural validation, serialization,
  deserialization, and every emitter backend about the operation;
- use only legal immediate encodings (`push imm32` sign-extends on x64);
- never place unmatched push/pop around calls, throws, traps, or non-local
  recovery paths;
- differential-test flags where a later machine operation can observe them.

That boundary is why the tutorial's pass performs semantic substitution in
ThinIR and leaves instruction selection to `thin_emit.cpp`: it is modular,
serializable, checked, and register-allocation-safe.
