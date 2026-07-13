# Plan — Composable polymorphic code engine for Ember

> **Status: research and implementation plan only (2026-07-13).**
> This document proposes changes; it does not claim they are implemented.
>
> **Reviewed baseline:** Ember `HEAD` was `2ac6a01`. The worktree was dirty and
> the review intentionally included those local changes. In particular, the
> observed per-module dispatch-slot-count publication and v5
> `CallCrossModule` target-size validation in `module_registry.*`,
> `module_linker.hpp`, `em_loader.cpp`, and `thin_ir_ser.*` are uncommitted
> worktree behavior, not guarantees of `2ac6a01`. The implementation phase must
> rebase this plan against the then-current tree and must not overwrite those
> unrelated changes.
>
> **Primary objective:** turn Ember's existing seeded IR obfuscation passes into
> a reproducible, configurable build-time polymorphic engine without replacing
> the current pass architecture or coupling it to one host, machine-fingerprint
> provider, packer, or runtime authorization policy.
>
> **Companion plan:**
> [`plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md`](plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md)
> covers optional environment-influenced module layout and runtime dispatch.
> That feature may supply a domain-separated build seed to this engine, but it
> remains a separate module-layout/runtime concern.

---

## 1. Executive recommendation

Ember already contains the correct core boundary for a polymorphic engine:

```text
source → sema → ThinFunction → composable passes → regalloc → x64 emit
```

Seven shipped passes in `extensions/obf/` already perform meaningful structural
variation:

- arithmetic substitution;
- mixed Boolean-arithmetic expansion;
- constant encoding;
- opaque-predicate insertion;
- dead-code injection;
- string-literal encoding;
- block splitting.

The missing pieces are not a second compiler or a new dependency. They are:

1. **configured extension factories** so a host can register passes with an
   immutable seed, density, and growth policy;
2. **one stable seed-derivation service** shared by all transforms;
3. **checked mutation utilities** for VRegs, frame storage, CFG edits, and
   regalloc invalidation;
4. **a public instruction-effect model** instead of private duplicated tables;
5. **checked pass execution** with validation, structured diagnostics, and
   ordinary-run growth limits;
6. **explicit build profiles** expressed as ordinary pipeline recipes;
7. **TDD gates** for semantic equivalence, reproducibility, variation,
   serialization, resource limits, and composition.

Do not turn environmental enforcement into a `ThinFunction` pass. Do not add a
second hidden pass manager. Do not make profile behavior implicit. Preserve the
existing extension style:

```cpp
ember::ext_obf::register_passes(registry, immutable_options);
```

The design is additive: existing `register_passes(reg)` and `reg.add<T>()`
callers remain valid.

---

## 2. Scope and terminology

### 2.1 What "polymorphic" means here

For this plan, a polymorphic build is one where:

- the same Ember source may produce structurally different valid IR and machine
  code under different build seeds;
- one fixed source/toolchain/configuration/seed produces reproducible output;
- selected variation never changes defined values, traps, visible memory
  effects, native-call ordering, or calling conventions;
- every output remains ordinary Ember IR/x64 and participates in existing
  validation, serialization, loading, hot reload, and W^X policy.

This raises the cost of signature-based/static analysis. It is not a
cryptographic security boundary and does not make client-side code
unrecoverable.

### 2.2 Three concerns that remain separate

| Concern | Correct boundary | This plan |
|---|---|---|
| Semantic code diversity | Function-level `ThinFunction` passes | Primary scope |
| Module/function physical layout diversity | Module manifest/layout planning after all signatures exist and before emission; physical placement after finalization | Companion plan |
| Runtime environmental enforcement | Executor key provider + dispatch strategy | Companion plan |

A seed derived from an environment key may feed the polymorphic engine, but the
pass API must only receive opaque derived seed material. It must not know how a
machine identity, TPM secret, licensing policy, or activation service works.

### 2.3 Non-goals

- Runtime self-modifying code or rewriting RX pages.
- Per-call JIT recompilation.
- Dynamic loading of untrusted pass DLLs.
- A stable cross-compiler C++ plugin ABI.
- Treating seeds as secrets.
- Anti-debugging, anti-VM, or undefined-execution behavior.
- Hiding malformed IR behind `non_serializable`.
- Replacing Ember's existing optimizer or register allocator.

---

## 3. Codebase audit

The following code was reviewed as the basis for this plan:

### 3.1 Pass infrastructure

- `src/ember_pass.hpp`
- `src/ember_pass.cpp`
- `src/ember_pass_registry.hpp`
- `src/ember_pass_pipeline.hpp`
- `src/codegen.hpp`
- `src/codegen.cpp`
- `docs/PASS_AUTHORING.md`
- `docs/spec/PASS_SYSTEM_DESIGN.md`
- `examples/ember_pass_test.cpp`
- `examples/custom_pass/`

### 3.2 IR, serialization, emission, and allocation

- `src/thin_ir.hpp`
- `src/thin_lower.cpp`
- `src/thin_emit.cpp`
- `src/thin_ir_ser.hpp`
- `src/thin_ir_ser.cpp`
- `src/regalloc.cpp`
- `src/peephole.cpp`
- `src/em_file.hpp`
- `src/em_writer.cpp`
- `src/em_loader.cpp`

### 3.3 Existing transform extensions and tests

- `extensions/opt/ext_opt.{hpp,cpp}`
- `extensions/obf/ext_obf.{hpp,cpp}`
- `examples/ir_passes_test.cpp`
- `examples/regalloc_test.cpp`
- `examples/thin_ir_ser_test.cpp`
- `docs/planning/plan_OBFUSCATION_PASSES.md`
- `docs/planning/plan_OPTIMIZATION_PASSES.md`

### 3.4 Current execution order

`compile_func()` currently performs:

```text
lower_function
    ↓
ctx.pass_manager->run(thf, local_analysis_manager)
    ↓
run_regalloc(thf), if enabled
    ↓
emit_x64(thf, ctx)
```

That placement is correct. Four details need correction:

1. `CodeGenCtx::analysis_manager` exists but is ignored; `compile_func()` always
   constructs a local empty manager.
2. Normal compilation calls `run()`, not `run_to_fixpoint()`, so the hard
   growth ceilings implemented only in `run_to_fixpoint()` do not protect the
   ordinary obfuscation path.
3. `compile_func()` returns only `CompiledFn`; it discards the transformed
   `ThinFunction` and exposes neither backend-used/fallback status nor a pass
   report. A structured compile-result boundary is required.
4. The normal CLI `--emit-em` construction uses emitted x64 records and
   `write_em_file()`. It does not automatically preserve transformed IR through
   `serialize_thin_function()`/`write_em_file_v5()`. Production artifact
   integration must be implemented explicitly rather than inferred from unit
   round trips.

### 3.5 Current extension registration

The existing extension shape is sound:

```cpp
EmberPassRegistry registry;
ext_opt::register_passes(registry);
ext_obf::register_passes(registry);
```

The limitation is the registry factory:

```cpp
reg.add<PassT>(name); // factory can only construct PassT{}
```

A configured pass can be inserted directly into a manager, but then it cannot
be selected through the normal registry/pipeline composition path.

### 3.6 Existing obfuscation behavior

| Pass | Pipeline name | Current behavior | Current variability |
|---|---|---|---|
| `SubstitutionPass` | `subst` | Rewrites every eligible two-VReg integer `Add` using one MBA identity | None |
| `MBAExpansionPass` | `mba_expand` | Selects `Add`, `Sub`, and multiply-by-two sites; chooses several identities | Seeded site and form selection |
| `ConstantEncodingPass` | `const_encode` | Encodes selected nontrivial constants using add/sub/XOR/safe shifts | Seeded site, key, and form |
| `OpaquePredicatesPass` | `opaque_pred` | Splits one selected site and inserts a fixed predicate plus rejoining bogus block | Seeded site/truth/junk |
| `DeadCodeInjectionPass` | `deadcode` | Splits one block and injects a pure chain consumed by a same-target branch | Seeded block/split/constants |
| `StringEncryptionPass` | `str_encrypt` | XORs referenced rodata and emits `StringDecrypt` | Fixed key `0xA5`; no variation |
| `BlockSplittingPass` | `block_split` | Splits each original block longer than eight instructions once | Seeded split point |

The local seed is currently derived only from a fixed label, pass name,
function name, and slot. Rebuilding the same source therefore recreates the
same variant.

### 3.7 Existing deterministic strengths

Keep these properties:

- FNV-1a and SplitMix64 are explicitly specified in source.
- No `std::hash`, `random_device`, or implementation-dependent distributions
  determine persisted output.
- Candidate enumeration generally follows stable block/instruction order.
- RNG state is local to a function/pass, so compile order does not advance one
  process-global stream.
- Optimization and obfuscation order is chosen explicitly by the host.

### 3.8 Gaps that block a production-quality engine

#### Pass API gaps

- No configured factory overload.
- Duplicate names silently overwrite prior registrations.
- Registry name enumeration follows `unordered_map` order.
- Pipeline parsing is comma-only although comments claim parenthesis support.
- Pipeline construction is not transactional; a later unknown name leaves
  earlier passes appended.
- Instrumentation supports one borrowed callback bundle and cannot reject a
  malformed result cleanly.
- Required passes bypass the `before_pass` callback entirely, preventing
  complete timing/tracing.
- Pass execution returns preservation only, not convergence/limit/validation
  status.
- No pass metadata distinguishes one-shot growth passes from converging
  simplifiers.

#### IR-authoring gaps

- Effect classification is private to `extensions/opt/ext_opt.cpp`.
- Obfuscation and custom passes hand-roll VReg scans, frame allocation, CFG ID
  repair, and regalloc clearing.
- Normal pass execution does not automatically validate output before regalloc.
- Ordinary `run()` has no total or per-pass growth policy.
- Several current transforms rely on "passes run before regalloc" rather than
  enforcing stale-allocation invalidation in one shared helper.

#### Serialization gaps

- `StringDecrypt::meta.data_temp_off` is validated and consumed by emission but
  is not serialized in Thin IR blob version 1.
- A transformed function is not automatically exercised through
  validate→serialize→deserialize→validate before being called production-ready.
- Emitter-level `@obf`/`@obf_keyed` annotations force tree-backend fallback and
  therefore do not compose with the Thin IR pass pipeline.

#### Existing correctness findings to gate before default profiles

These are not assumptions for new code; they require regression tests and
focused correction before aggressive profiles become defaults:

- implicit frame-home reads can make a seemingly dead VReg definition live;
- block-local CSE must not remove a definition used by successor blocks;
- integer folding must use explicit modular-width arithmetic rather than C++
  signed-overflow behavior;
- non-SSA VRegs require join-aware SCCP semantics;
- loop-strength-reduction scratch frame spans must not overlap later
  allocations;
- calling regalloc repeatedly must not keep extending the frame.

---

## 4. Target architecture

```text
                         Host / CLI / build service
                                  │
                       PolymorphicBuildOptions
                                  │
               ┌──────────────────┴──────────────────┐
               │ configured extension registration  │
               ▼                                     ▼
       EmberPassRegistry                    PipelineProfileRegistry
               │                                     │
               └──────────────┬──────────────────────┘
                              ▼
             source → sema → stable module manifest
                              │
              optional domain/layout planning (companion plan)
                              │
                      lower to ThinFunction
                              │
                    checked pass execution
                              │
                    validate after mutation
                              │
                       regalloc exactly once
                              │
                         x64 / v5 IR emit
                              │
             physical placement, validation, publication
```

The layout plan must exist before emission when call sites consume domain
metadata or embed final storage addresses. Compiled pages are placed into the
already-planned physical topology only after finalization.

### 4.1 Extension categories

Keep one recognizable registration style while acknowledging different
extension units:

```cpp
register_natives(NativeTable&);
register_overloads(OpOverloadTable&);
register_passes(EmberPassRegistry&, const PassOptions&);
```

The companion environmental plan may add:

```cpp
register_keyed_dispatch(KeyedDispatchRegistry&,
                        const KeyedDispatchOptions&);
```

Do not collapse these into one untyped `register_everything(void*)` interface.
The composability comes from consistent registries and explicit host wiring,
not from erasing the distinction between compile-time and runtime services.

### 4.2 Immutable options

```cpp
struct PassGrowthLimits {
    uint32_t max_sites = 64;
    uint32_t max_added_instructions = 4096;
    uint32_t max_added_blocks = 256;
    uint32_t max_added_vregs = 8192;
    uint32_t max_added_frame_bytes = 64 * 1024;
    uint32_t max_added_rodata_bytes = 4 * 1024 * 1024;
    uint32_t growth_numerator = 3;
    uint32_t growth_denominator = 1;
};

struct PolymorphicPassOptions {
    std::shared_ptr<const SeedDeriver> seed_deriver;
    uint32_t algorithm_version = 1;
    uint32_t site_probability_ppm = 500000;
    PassGrowthLimits limits{};
};
```

Options are copied into registry factories. `seed_deriver` has shared immutable
ownership for the complete registry/pipeline lifetime. A fixed 64-bit CLI seed
is adapted once into the same 256-bit domain-separated `SeedDeriver` interface;
external machine providers use the adapter defined by the companion plan. No
pass reads process-global mutable seed state.

### 4.3 Configured factories

Add without removing `add<T>()`:

```cpp
using PassFactory = std::function<std::unique_ptr<PassConcept>()>;

template<class PassT>
std::unique_ptr<PassConcept> make_pass_concept(PassT pass);

struct ExtensionError {
    std::string registry;
    std::string name;
    std::string message;
};

struct ExtensionStatus {
    std::optional<ExtensionError> error;
    explicit operator bool() const { return !error.has_value(); }
};

template<class T>
struct ExtensionResult {
    std::optional<T> value;
    std::optional<ExtensionError> error;
    explicit operator bool() const { return value.has_value() && !error; }
};

ExtensionStatus EmberPassRegistry::add_factory(std::string name,
                                                PassFactory factory);

template<class PassT>
ExtensionStatus EmberPassRegistry::add(std::string name);
```

Recommended registration:

```cpp
void ext_obf::register_passes(EmberPassRegistry& reg,
                              const PolymorphicPassOptions& options) {
    reg.add_factory("mba_expand", [options] {
        return make_pass_concept(MBAExpansionPass{options});
    });
    // ...
}
```

Compatibility wrapper:

```cpp
void ext_obf::register_passes(EmberPassRegistry& reg) {
    register_passes(reg, PolymorphicPassOptions{});
}
```

Collision-aware APIs should reject duplicate names. The `ExtensionResult` /
`ExtensionError` contract is the shared registry foundation for pass, profile,
and keyed-dispatch registries; the keyed-dispatch plan depends
on this Phase 1 work rather than reimplementing registration and diagnostics.
If legacy `add<T>()` must retain replacement behavior temporarily, document it
as deprecated and use the strict result-returning API in all new registration.

### 4.4 Pass metadata

Add only metadata with an immediate enforcement consumer:

```cpp
enum class PassKind { Optimization, Diversification, Validation, Utility };
enum class PassRepeatability { Converging, OneShot, Idempotent };

struct PassDescriptor {
    std::string registry_name;
    PassKind kind;
    PassRepeatability repeatability;
    bool required;
};
```

Uses:

- reject one-shot growth passes inside an automatic fixed-point group;
- list passes deterministically;
- produce accurate diagnostics and manifests.

Do not add declarative `changes_cfg/frame/rodata` flags as a second source of
truth. Checked execution measures before/after state and runs the required
verifiers after every reported mutation. Registry name is the canonical
manifest/diagnostic identity, including aliases.

### 4.5 Checked execution report

Keep current methods for compatibility and add a checked path:

```cpp
enum class PassStopReason {
    Completed,
    Converged,
    RoundLimit,
    GrowthLimit,
    ValidationFailure,
    PassFailure
};

struct PassRunReport {
    EmberPreserved preserved = EmberPreserved::all();
    PassStopReason reason = PassStopReason::Completed;
    unsigned rounds = 1;
    size_t initial_instructions = 0;
    size_t final_instructions = 0;
    std::string pass_name;
    std::string error;
};
```

Existing passes keep the source-compatible
`EmberPreserved run(ThinFunction&, EmberAnalysisManager&)` shape. The type-erased
adapter catches a documented `PassError` (and converts unexpected
`std::exception` to `PassFailure`); new passes may instead implement
`PassExecutionResult run_checked(...)`, detected by the adapter. Both forms
produce one internal structured result before snapshot validation. Exceptions
never cross the compile/module boundary.

Library code returns diagnostics rather than printing only to `stderr`.
Embedding hosts choose logging policy. This does not replace
`safety::check_memory_limit()`, whose current process-wide contract aborts and
therefore cannot be reported as a recoverable pass stop reason.

Checked execution must define mutation ownership. The initial implementation
uses a function snapshot for each third-party/legacy pass in checked mode:

```text
snapshot ThinFunction
run pass
validate + enforce postconditions
on failure: discard mutated value and restore snapshot
```

Production passes using `ThinIRMutation` may build a journal/copy-on-write plan
and commit atomically, but checked mode cannot assume all extensions use that
helper. `compile_func_checked()` returns a structured `CompileResult` carrying
backend used, fallback reason, transformed IR when requested, `PassRunReport`,
and emitted code; module compilation commits no function records if any
required function fails or falls back.

### 4.6 Transactional pipeline construction

`build_pipeline_from_string()` should resolve every token into a temporary
manager and mutate the caller's manager only after complete success.

Initial grammar remains deliberately small:

```text
pipeline := name (',' name)*
```

Reject:

- empty middle elements;
- unsupported parentheses;
- unknown names;
- null factories;
- trailing junk.

Parameterized pipeline syntax is not needed for the first implementation.
Configured factories and named profiles solve the immediate problem with less
parser complexity.

---

## 5. Seed architecture

### 5.1 Contract

```text
same source + canonical options + tool version + build seed
    = identical transformed Thin IR

different selected build seeds
    = different eligible structure, same semantics
```

The seed controls layout choices. It is not a cryptographic secret and must not
be used as a substitute for environmental authorization.

### 5.2 Root seed sources

Support explicit modes:

```cpp
enum class BuildSeedMode {
    Fixed,
    RandomAndRecord,
    ExternalDerived
};
```

- **Fixed:** user supplies a 64/256-bit seed for reproducible builds.
- **RandomAndRecord:** build tool obtains randomness, prints/stores the resolved
  seed in private build metadata, then proceeds deterministically.
- **ExternalDerived:** a host supplies opaque derived material, potentially from
  the companion environmental-key provider. Ember never receives raw machine
  identifiers or the provider's root secret.

### 5.3 Domain separation

Derive independent streams from a canonical request:

```text
engine version
module stable identity
build/profile identity
pass name + pass algorithm version
function name + logical slot
site block ID + original instruction ordinal
purpose: select | variant | constant | truth | junk-count | string-key
```

Do not use one advancing RNG for all choices. Adding a draw for one constant
must not reshuffle every later site.

A small API:

```cpp
struct SeedRequest {
    std::string_view engine_version;
    std::string_view module_id;
    std::string_view build_profile_id;
    std::string_view pass_name;
    uint32_t pass_algorithm_version;
    std::string_view function_name;
    uint32_t logical_slot;
    uint32_t block_id;
    uint32_t instruction_ordinal;
    std::string_view purpose;
};

class SeedDeriver {
public:
    virtual ~SeedDeriver() = default;
    virtual ExtensionResult<std::array<uint8_t, 32>> derive(
        const SeedRequest&) const = 0;
};
```

For ordinary builds, provide an immutable/thread-safe implementation backed by
the explicit build seed. Hosts may provide the shared external-material adapter
from the companion plan. A per-module compilation context supplies canonical
module/profile identity. Pass algorithm versions are independent so changing
one transform does not renumber every other stream. Passes consume per-purpose
output, not the provider's root.

One `EmberPassManager` is not concurrently runnable: concrete `run()` methods
are mutable. Parallel compilation creates a fresh configured pipeline per
worker/function, while sharing only immutable thread-safe options/derivers.

### 5.4 Stable randomness

- Pin hash/KDF and PRNG algorithm versions.
- Keep byte encoding little-endian and specified.
- Never use `std::hash` or unordered-container iteration as input order.
- Use rejection sampling where unbiased selection matters.
- Sort equal regalloc intervals with a final VReg tie-break.
- Add golden derivation vectors independent of the compiler's standard library.

### 5.5 Artifact metadata

A reproducible build manifest may record:

- engine/pass algorithm versions;
- profile name;
- pipeline names and immutable public options;
- source/module digest;
- optional build-seed identifier controlled by the build system.

It need not be embedded in the executable. Environmental mode must not require
embedding a machine key, expected key, key digest, or raw fingerprint.

---

## 6. Shared IR-authoring foundation

### 6.1 `ThinIRMutation`

Add a checked helper used by production and example passes:

```cpp
class ThinIRMutation {
public:
    explicit ThinIRMutation(ThinFunction&, const PassGrowthLimits&);

    Result<VReg> allocate_scalar(const Type*, int32_t width);
    Result<std::pair<VReg,VReg>> allocate_pair(const Type*);
    Result<int32_t> allocate_frame_bytes(uint32_t size, uint32_t alignment);
    Result<uint32_t> split_block(uint32_t block, size_t instruction_index);
    Result<void> redirect_edge(uint32_t from, uint32_t old_to, uint32_t new_to);
    Result<void> canonicalize_block_ids();
    Result<void> commit();
};
```

The helper owns:

- central VReg enumeration, including implicit `dst+1` slice/lambda pairs,
  arguments, and terminators; the serializer and validator must consume the
  same enumeration rather than retaining their current explicit-field-only
  scans;
- checked `declared_max_vreg` updates;
- negative frame-offset allocation and non-overlap checks;
- `frame_size` alignment and maximum-span checks;
- block-ID/target rewriting;
- deterministic growth accounting;
- stale-regalloc clearing;
- all-or-nothing commit for a selected site and deterministic stop-before-site
  behavior when a soft site budget is exhausted.

A pass must not leave a half-created rewrite when a limit is reached. Semantics
are fixed as follows:

- `max_sites` and per-pass added-resource caps are **soft success ceilings**:
  stop before the next site and report completed-with-truncation;
- invalid options (`denominator==0`, probability above 1,000,000, overflowing
  ratios) are registration/build failures;
- the growth ratio bounds final count relative to `max(1, initial_count)` using
  checked integer arithmetic;
- manager hard ceilings are failures with snapshot rollback;
- every pass preflights one site's worst-case allocation before mutating.

### 6.2 Public operation effects

Move the authoritative model into core headers:

```cpp
enum class ThinEffectFlag : uint32_t {
    None, ReadsFrame, WritesFrame, ReadsGlobal, WritesGlobal,
    ReadsIndirect, WritesIndirect, CallsUnknown, MayTrap, WritesTemp,
    ImplicitSpillWrite, EscapesAddress
};

struct ByteInterval {
    MemorySpace space;
    int64_t begin;
    uint64_t size;
    bool unknown = false;
};

struct ThinEffectDescriptor {
    EffectFlags flags;
    std::vector<ByteInterval> reads;
    std::vector<ByteInterval> writes;
    bool aliases_unknown_memory = false;
};

ThinEffectDescriptor classify_thin_effects(const ThinInstr&);
bool removable_if_result_dead(const ThinInstr&,
                              const ThinEffectDescriptor&);
```

The descriptor represents explicit and implicit producer frame-home writes,
exact/overlapping `CopyBytes` ranges, computed/unknown address accesses, frame
address escape, and call barriers. Static flags alone never authorize removal.
Every new `ThinOp` must update this model in the same change. Calls remain
conservatively effectful until purity is explicit ABI metadata.

### 6.3 Validator boundary

Checked mode runs:

```text
validate input
for each mutating pass:
    run pass
    validate output
validate before regalloc
regalloc once
emit
```

A validation failure restores the checked-mode snapshot, stops before regalloc
or executable allocation, and reports which pass created invalid IR.

`validate_thin_function()` remains the disk-facing structural/security
validator; it does not prove use-before-definition, reaching definitions,
complete op/type shapes, or semantic equivalence. Add a stricter
`verify_thin_function_for_codegen()` for pass output, covering op-specific
shapes, central VReg/pair enumeration, definitions at uses, type/width
compatibility, CFG/frame/rodata invariants, and call-vector consistency. Tests
must say which verifier they use; "validator-clean" alone is not a semantic
claim.

### 6.4 Analysis-manager correction

Use the existing host-provided field:

```cpp
EmberAnalysisManager local;
auto& analyses = ctx.analysis_manager ? *ctx.analysis_manager : local;
ctx.pass_manager->run_checked(thf, analyses, options);
```

When cached analyses are later implemented, the pass manager invalidates them
after every pass according to `EmberPreserved`.

---

## 7. Transform roadmap

### 7.1 Preserve and configure the shipped passes

The first milestone should not add new transform families. It should make the
seven existing passes configurable, bounded, serializable, and thoroughly
verified.

Each seeded pass receives:

- independent site-selection stream;
- independent variant/key/truth streams;
- density/maximum-site limits;
- per-pass growth limits;
- explicit algorithm version.

### 7.2 `subst`

Retain as a compatibility name. Either:

- make it the deterministic "rewrite every eligible Add" pass; or
- deprecate it in favor of `mba_expand` after equivalent coverage exists.

Do not silently make the same name mean a radically different profile.

Fix shared invariants:

- update `declared_max_vreg`;
- clear stale `ra`;
- use `ThinIRMutation` for frame slots;
- apply ordinary-run growth limits.

### 7.3 `mba_expand`

Extend only after width-correct property tests exist:

- current `Add`, `Sub`, and multiply-by-two forms;
- optional `Xor`, `And`, `Or`, and bounded multiply identities;
- immediate materialization through checked fresh values;
- explicit modular arithmetic helpers for 1/2/4/8-byte widths.

Snapshot original candidates so inserted instructions are not recursively
expanded in one invocation.

### 7.4 `const_encode`

- Preserve current width-normalized forms.
- Derive site keys independently.
- Never encode structural metadata such as slots, block IDs, frame offsets,
  lengths, relocation kinds, trap reasons, or signature IDs.
- Add bool canonicalization tests before transforming `ConstBool`.

### 7.5 `opaque_pred`

- Retain rejoining bogus paths so either edge is memory-safe.
- Select from reviewed fixed-width identities.
- Never introduce division/modulo, uninitialized loads, calls, or visible
  stores.
- Avoid splitting guard/operation pairs.
- Allow multiple bounded sites only after single-site CFG tests are complete.

### 7.6 `deadcode`

- Keep injected instructions pure.
- Use only fresh destinations and dedicated internal frame storage.
- Make liveness intentional through a same-target branch or another validated
  use; do not lie in the effect classifier.
- Cap injected chains and blocks per function.

### 7.7 `str_encrypt`

Hard gate: first add Thin IR blob v2, serialize `data_temp_off`, and retain an
explicit v1 decoder. V1 loads with `data_temp_off = 0` only where the old safe
fallback is valid; a v1 `StringDecrypt` whose buffer/result layout cannot be
proven nonoverlapping is rejected with a format diagnostic rather than emitted.

Then:

- derive a nonzero key per literal/site;
- rebuild rodata instead of in-place XOR when references overlap under
  different keys;
- preserve separate nonoverlapping data and `{ptr,len}` frame regions;
- avoid double encryption unless explicit rekey mode is requested;
- assert plaintext absence from final rodata/IR blob for selected literals.

This remains obfuscation. The decrypt operation and key-dependent behavior are
present in executable code.

### 7.8 `block_split`

- Use shared CFG utilities.
- Preserve entry ID 0 and every target.
- Maintain a central list of inseparable instruction pairs.
- Add bounded edge trampolines only after split-only behavior is stable.

### 7.9 Future transforms

Only after foundations and shipped passes are green:

- control-flow flattening over symbolic validated blocks;
- per-function block-order permutation;
- instruction-selection alternatives in `thin_emit`;
- register-allocation tie-break diversity;
- relocation-safe x64 peephole variants.

These should be separate registered extensions/passes, not hidden modes inside
unrelated transforms.

---

## 8. Profiles and composition

Profiles are named recipes in a `PipelineProfileRegistry` built on the shared
extension-registry foundation:

```cpp
struct PipelineProfile {
    std::string name;
    std::string pipeline;
    PolymorphicPassOptions options;
};
```

Selecting a profile first registers/configures its immutable factories into a
fresh registry and then builds a fresh manager. An explicit `--passes` pipeline
replaces the profile pipeline; explicit seed/density/limit flags override the
profile's corresponding options before factories are created. Pipeline parsing
exposes separate atomic `replace` and `append` operations; neither moves or
silently replaces manager instrumentation.

Suggested initial profiles:

### `light`

```text
constprop,forward,copyprop,instcombine,dce,dse,
const_encode,mba_expand
```

### `balanced`

```text
simplifycfg,constprop,forward,copyprop,instcombine,cse,dce,dse,
str_encrypt,const_encode,block_split,opaque_pred,deadcode,mba_expand
```

This profile is illustrative and cannot be promoted until every included pass
and the Thin IR backend are green on the known frame-home DCE, successor-use
CSE, modular-overflow, non-SSA SCCP, LSR span, and repeat-regalloc regressions.

### `heavy`

Initially experimental; may increase site counts and later include flattening.
It must not enter default builds until compile-time, code-size, frame, and
runtime budgets are measured.

Rules:

- simplifying optimization precedes diversification;
- complexity-increasing passes run once;
- cleanup after diversification is opt-in because it may erase variation;
- regalloc runs exactly once after all IR mutation;
- profiles never silently override an explicit user pipeline.

CLI surface:

```text
--passes <pipeline>
--pass-profile light|balanced|heavy
--pass-seed <u64-or-hex>
--pass-random-seed
--list-passes
--require-ir-passes
--regalloc / --no-regalloc
```

`--require-ir-passes` fails if a requested function falls back to the tree
backend instead of silently leaving a partially transformed module.

---

## 9. TDD strategy

### 9.1 Test harness

Ember already uses executable tests registered with CTest. Add focused targets:

```text
ember_pass                  existing infrastructure tests
polymorphic_pass            new seeded engine tests
custom_pass                 new continuously-built extension example test
thin_ir_ser                 extended serialization tests
ir_passes                   extended per-transform semantics tests
polymorphic_cli             CLI seed/profile parsing tests
```

Recommended commands:

```bash
cmake -S . -B build-tdd -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DEMBER_BUILD_VST3=OFF
cmake --build build-tdd
ctest --test-dir build-tdd --output-on-failure \
  -R "^(ember_pass|polymorphic_pass|custom_pass|thin_ir_ser|ir_passes|polymorphic_cli)$"
```

Final gate:

```bash
ctest --test-dir build-tdd --output-on-failure
```

### 9.2 Four mandatory buckets

#### Standard / should succeed

- Legacy `reg.add<T>()` still creates and runs passes.
- Configured factories capture exact immutable options.
- Same seed reproduces identical serialized Thin IR.
- Two pinned seeds produce known different eligible structures.
- Every transformed program matches baseline return/trap/effect traces.
- Serialization round-trip and both regalloc modes work.

#### Should fail

- Duplicate strict registration.
- Empty/null factory.
- Unknown/invalid pipeline token.
- Invalid seed syntax/overflow.
- Pass-created malformed CFG/frame/rodata.
- Growth beyond configured limits.
- Requested required-IR mode encountering tree-backend fallback.

#### Edge

- Seeds `0` and `UINT64_MAX`.
- Empty functions and no eligible sites.
- Integer widths 1/2/4/8 and signed/unsigned extrema.
- Empty, singleton, diamond, loop, and irreducible CFG shapes.
- Exact serializer/frame/growth limits.
- Repeated/overlapping/empty string references.
- Regalloc off / one register / full pool and forced spilling (`run_regalloc`
  currently treats numeric zero as the full default pool, not zero registers).

#### Regression

- `data_temp_off` survives round-trip.
- Implicit frame-home reads keep their definitions live.
- CSE does not remove successor-used definitions.
- SCCP respects conflicting non-SSA definitions.
- LSR scratch spans do not overlap later allocation.
- Re-running regalloc is rejected or idempotent and does not grow the frame
  again; orchestration also refuses preallocated `f.ra` before its one allowed
  allocation stage.
- Checked pass failures restore the function snapshot and module compilation
  publishes no partial artifact.

### 9.3 Red-green implementation order

#### Red 1 — configured registry factories

Add a compiling registry stub/signature first, then extend
`examples/ember_pass_test.cpp` with runtime-failing duplicate/null/fresh-instance
assertions. If an API-presence compile contract is desired, use a dedicated
CMake `try_compile` check rather than making the normal CTest target fail to
build.

**Green:** implement only configured, collision-aware factories while keeping
legacy `add<T>()`.

#### Red 2 — strict transactional pipeline

Add unknown-middle-token, empty-element, unsupported-parenthesis, and
manager-unchanged assertions.

**Green:** resolve into temporary storage and atomically `replace` or `append`
passes only on success, preserving existing manager instrumentation.

#### Red 3 — seed derivation vectors

Add pinned vectors for root/function/site/purpose derivation, reverse compile
order, and parallel compilation.

**Green:** implement central versioned derivation and stable RNG helpers.

#### Red 4 — mutation/effect utilities

Add allocator boundary, CFG remap, frame non-overlap, VReg bound, and effect
classification tests.

**Green:** extract shared helpers and migrate one simple pass first.

#### Red 5 — checked execution

Add a deliberately broken pass and assert validation stops before regalloc or
emit with a structured report.

**Green:** implement checked run mode and ordinary-run limits.

#### Red 6 — existing pass migration

For each obfuscator, first add:

- no-op/changed preservation;
- same-seed equality;
- pinned-seed variation;
- baseline differential behavior;
- validation and serialization;
- exact growth boundary.

**Green:** migrate that pass to configured streams and shared mutation.

#### Red 7 — string serialization

Add a `StringDecrypt` function with distinct data/slice slots, serialize it,
reload it, and assert both offsets and runtime output.

**Green:** version Thin IR and serialize `data_temp_off`; only then migrate
`str_encrypt`.

#### Red 8 — profiles and CLI

Add CLI parsing/error tests and a candidate-rich script for each profile.

**Green:** profiles expand to ordinary registered pass sequences; no hidden
manager behavior.

#### Red 9 — integration

Compile a multi-function module with optimization + all obfuscators, reverse
function order, serialize/load, run with regalloc off/on, and compare complete
observable traces.

**Green:** resolve remaining composition defects.

### 9.4 Property/differential tests

Generate executable-by-construction scalar Thin IR functions over bounded CFGs
and inputs, then pass both structural and codegen verifiers. Use a trace-native
harness plus Ember's recoverable checkpoint/trap fixture—there is no existing
Thin IR interpreter. For every selected seed:

```text
baseline execute
transform
validate
serialize/deserialize
execute transformed
compare return, trap reason, globals, and side-effect trace
```

Random testing supplements reviewed identities; it does not replace proof of
MBA/width semantics.

### 9.5 Performance tests

Record per profile:

- compile time;
- IR instruction/block/VReg counts;
- frame and rodata growth;
- relocation-normalized emitted bytes (raw `CompiledFn::bytes` contain
  process-specific dispatch/globals/registry/native addresses);
- runtime of representative arithmetic, loop, call, bounds, string, and
  aggregate workloads.

Set explicit budgets before promoting any profile to default.

---

## 10. Implementation phases

### Phase 0 — baseline and correctness blockers

- Capture the current focused CTest baseline.
- Add regressions for every known optimizer/frame/serialization finding listed
  in §3.8, plus central slice/lambda pair VReg enumeration.
- Fix every failure for any pass proposed in `light` or `balanced`.

**Gate:** each profile member, the Thin IR backend, serializer, and regalloc are
individually green on the named repros; a profile cannot be promoted on a
merely generic suite pass.

### Phase 1 — shared extension-registry API improvements

- One reusable `ExtensionResult`/`ExtensionError` contract.
- Configured/collision-aware pass factories.
- Deterministic listing.
- Strict transactional pipeline parser.
- Honor host-provided analysis manager.
- Structured run reports.

This phase is a prerequisite for the companion plan's layout and strategy
registries; those registries reuse the foundation rather than implementing
parallel error/factory machinery.

**Gate:** `ember_pass` red-green cases green; existing callers unchanged.

### Phase 2 — shared pass-authoring utilities

- Stable seed derivation and substreams.
- `ThinIRMutation`.
- Public effects.
- Checked validation and ordinary-run limits.

**Gate:** utility boundary/property tests green.

### Phase 3 — migrate existing obfuscators

Suggested order:

1. `const_encode`
2. `mba_expand`
3. `block_split`
4. `opaque_pred`
5. `deadcode`
6. `subst`
7. `str_encrypt` after format fix

**Gate per pass:** semantic, seed, round-trip, regalloc, and limit matrices green.

### Phase 4 — profile/CLI integration

- Explicit profiles.
- Seed modes and diagnostics.
- `--require-ir-passes`.
- Independently controlled regalloc.
- Build manifest output.

**Gate:** CLI tests plus complete profile integration.

### Phase 5 — module-layout composition

Integrate optional module-layout seeds through the companion plan without
making module layout a function pass. Module signatures/domains and the layout
plan are established before function emission; finalized functions are placed
and published afterward.

**Gate:** the provider adapter derives independent pass/layout domains from one
root, the same target material produces matching function diversity and module
layout, and logical ABI remains stable.

### Phase 6 — future transform extensions

Only after measurement: flattening, block-order permutation, instruction
selection, and emission diversity.

---

## 11. File-level change map

| File/area | Planned change |
|---|---|
| `src/extension_registry.hpp` (new shared utility) | `ExtensionResult`/`ExtensionError`, collision policy, deterministic descriptors |
| `src/ember_pass_registry.hpp` | Pass-specific wrapper over shared configured factories |
| `src/ember_pass.hpp/.cpp` | Metadata, checked reports, validation/growth hooks, observer/gate separation |
| `src/ember_pass_pipeline.hpp` | Transactional strict parsing |
| `src/codegen.hpp/.cpp` | Add `CompileResult`; honor analyses; checked pass result/backend/fallback/transformed-IR status |
| `src/em_file.hpp`, `src/em_writer.*` | Carry transformed IR into the production v5 artifact path |
| `examples/ember_cli.cpp`, live/pipe helpers | Build v5 records from checked compile results; seed/profile/list/require-IR controls |
| `examples/ember_bundle.cpp`, `examples/ember_stub_main.cpp` | Propagate profile/seed/artifact policy through standalone builds |
| `bench/bench_codegen_paths.cpp` | Profile compile/runtime/code-size measurements |
| `src/thin_ir_mutation.*` (new) | Checked common mutation utilities |
| `src/thin_effects.*` (new) | Public exhaustive effect model |
| `src/seed_derivation.*` (new) | Versioned stable seed/substream derivation |
| `src/thin_ir_ser.*` | Version and serialize `data_temp_off`; validation regressions |
| `extensions/obf/*` | Immutable configuration and per-purpose streams |
| `extensions/opt/*` | Consume shared effects; confirmed correctness fixes only |
| `examples/ember_pass_test.cpp` | Factory/parser/report TDD |
| `examples/polymorphic_pass_test.cpp` (new) | Reproducibility, variation, differential, limits |
| `examples/custom_pass/*` | Use production utilities; add continuously built test |
| `CMakeLists.txt` | Register focused test targets |
| `docs/PASS_AUTHORING.md` | Configured factory, seed, checked-run contracts |

---

## 12. Acceptance criteria

The polymorphic engine is complete when:

- legacy pass registration remains source-compatible;
- configured extension factories support immutable seed/options;
- duplicate strict registrations and malformed pipelines fail clearly;
- same source/tool/config/seed yields byte-identical serialized Thin IR;
- two pinned seeds vary eligible output without probabilistic tests;
- every shipped obfuscator has semantic, trap/effect, serialization, regalloc,
  and limit coverage;
- changed output is validated before regalloc/emission;
- ordinary one-shot execution enforces deterministic resource limits;
- no growth pass can accidentally enter an uncontrolled fixed point;
- mutation/effect logic is shared by built-in and custom passes;
- `data_temp_off` round-trips before `str_encrypt` is called serialization-safe;
- tree-backend fallback is reported or rejected under required mode;
- profiles are ordinary explicit pipeline recipes;
- seed material controls diversity only and is not presented as authorization;
- full CTest and language suites remain green;
- compile time, code size, frame growth, and runtime overhead are measured and
  published for every promoted profile.

---

## 13. Recommended starting point

Start with these files, in order:

1. `examples/ember_pass_test.cpp` — write configured-factory and transactional
   parser failures first.
2. `src/ember_pass_registry.hpp` — add strict configured factories.
3. `src/ember_pass_pipeline.hpp` — make construction strict and transactional.
4. `src/seed_derivation.*` — pin reproducible derivation vectors.
5. `src/thin_ir_mutation.*` and `src/thin_effects.*` — centralize correctness.
6. `src/thin_ir_ser.*` — fix `data_temp_off` before string pass migration.
7. `extensions/obf/ext_obf.*` — migrate one pass at a time under the new tests.

That sequence improves the existing composable API rather than building around
it, and gives every later transform a verified foundation.
