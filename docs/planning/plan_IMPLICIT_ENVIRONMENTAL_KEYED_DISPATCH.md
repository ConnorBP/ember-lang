# Plan — Composable implicit environmental keyed dispatch for Ember

> **Status: RED 1–10 DONE (2026-07-15); RED 11 composition covered by the
> integrated polymorphic/keyed path.** Versioned permutation math, ABI
> classification, logical/physical module layout, runtime resolver, `r15`
> keyed outer thunks, tree/Thin-IR same-module calls, atomic module-record and
> cross-module calls, host/lifecycle/thread/coroutine integration, keyed `.em`
> v6 capability metadata/loading, keyed single-function reload, and whole-
> generation replacement all ship. Focused test executables include
> `keyed_dispatch_math`, `module_layout`, `keyed_dispatch_runtime`,
> `keyed_dispatch_outer_thunk`, `keyed_dispatch_codegen`,
> `keyed_dispatch_modules`, `em_v6_keyed`, `keyed_dispatch_extensions`, and
> `keyed_dispatch_hot_reload`; intentionally heavy gates are not all default
> CTests. The body remains the design/security contract.
>
> **Reviewed baseline:** Ember `HEAD` was `2ac6a01`. The worktree was dirty and
> the review intentionally included those local changes. The observed
> per-module dispatch-slot-count publication and v5 `CallCrossModule`
> target-size validation in `module_registry.*`, `module_linker.hpp`,
> `em_loader.cpp`, and `thin_ir_ser.*` are uncommitted worktree behavior, not
> guarantees of `2ac6a01`. Implementation must rebase the plan against the
> current tree and preserve unrelated local work.
>
> **Primary objective:** let a target-derived machine key participate
> transitively in Ember control flow without embedding, sealing, hashing, or
> directly comparing an expected machine key. The build uses the target key to
> construct a physical execution topology; the executor independently derives a
> transient runtime key and uses the same versioned mathematics to navigate it.
>
> **Companion plan:**
> [`plan_POLYMORPHIC_CODE_ENGINE.md`](plan_POLYMORPHIC_CODE_ENGINE.md) covers
> target/build-seeded semantic code diversity. Machine-specific modules may use
> both plans, but keyed routing and IR diversification remain separate extension
> categories.

---

## 1. Executive recommendation

Implement **implicit key-dependent logical-to-physical dispatch**, not a key
comparison and not pointer encryption:

```text
Build for target T:
    K = transient target-derived route key
    physical_slot = P(K, module, ABI-domain, logical-ordinal)
    place compiled function at physical_slot
    discard K

Runtime on machine M:
    k = transient locally-derived route key
    physical_slot = P(k, module, ABI-domain, logical-ordinal)
    call physical dispatch entry
    discard k at the outer-call boundary

If M == T and derivation is stable: k == K → intended topology
If M != T:                         k != K → different safe topology
```

The artifact contains the **resulting key-influenced code/layout**, because that
is the mechanism, but it contains no:

- machine identifier;
- expected machine key;
- sealed machine key;
- key hash or verifier;
- `if (runtime_key == expected_key)` gate;
- direct key-derived raw pointer.

A wrong key must never produce an out-of-range access, incompatible ABI call,
null/wild function pointer, or undefined execution. It may call a different
function inside the exact same callable ABI domain or a same-ABI generated
padding/trap target. Not every wrong key is guaranteed to trap; this is implicit
keyed control-flow/layout, not authentication.

The current function-pass API remains appropriate for machine-derived
polymorphic transforms. Keyed dispatch additionally needs two composable,
module-scoped seam:

```cpp
register_keyed_dispatch(KeyedDispatchRegistry&, options);
```

A keyed-dispatch package contains its matching layout planner, versioned
permutation, runtime resolver/emitter, and validator. Keep these paired so a
host cannot combine incompatible versions; split registries only after a real
consumer needs independent composition.

Do not force module-wide physical layout through `ThinFunction` passes. Do not
redesign the existing pass API. Improve the common extension pattern with
configured immutable factories, strict name registration, deterministic
listing, and structured diagnostics.

---

## 2. Precise semantic model

### 2.1 Stable logical identity

Source, sema, exports, function references, lifecycle annotations, hot reload,
and cross-module links continue to identify a callable by stable logical
identity:

```cpp
struct LogicalCallableId {
    StableModuleId module;
    uint32_t logical_slot;
    uint64_t abi_fingerprint;
};
```

A physical dispatch index is never exposed to Ember source and never stored in
a function handle.

### 2.2 Keyed physical identity

Every logical callable belongs to one exact ABI domain. Within domain `D`, the
callable receives a stable ordinal `i`. Build and runtime use one pinned
bijection:

```text
physical_index = D.physical_base + P_v(K_or_k, D.salt, i, D.physical_count)
```

At build time `K` determines where the target is placed. At runtime `k`
determines where the caller looks. The runtime does not know whether `k` is
"right"; it simply follows the resulting topology.

### 2.3 Why logical and physical slots must be separated

Today Ember uses one slot for all of these:

- sema-resolved calls;
- dispatch array index;
- function handle value;
- export/name directory value;
- hot-reload publication index;
- cross-module `(module_id, slot)` identity.

Implicit keyed dispatch requires splitting that overloaded concept:

```text
logical slot  = stable language/link/reload identity
physical slot = build-specific dispatch storage position
```

Legacy/unkeyed mode uses identity layout and preserves existing behavior.

### 2.4 Wrong-key safety contract

For every key value and valid logical callable:

1. resolution terminates within a bounded amount of work;
2. the resulting index lies inside that callable's domain;
3. the selected entry has the exact same canonical calling ABI;
4. the entry is non-null and finalized RX code;
5. malformed metadata resolves to a compatible trap/padding target or is
   rejected before publication;
6. no expected-key comparison occurs.

This safety contract is stronger and more testable than intentional pointer
corruption.

### 2.5 Singleton domains

A domain containing one real function would otherwise have an identity
permutation. Give every domain at least one generated same-ABI padding target:

```text
physical_count = logical_function_count + padding_count
```

Initial design: exactly one padding ordinal per domain. A singleton real
function therefore forms a two-entry permutation with a compatible trap stub.

### 2.6 Visibility and optional domain labels

Partition at least by:

```text
module identity
public/private visibility class
exact ABI fingerprint
optional explicit dispatch-domain label
```

Private and public functions do not share a domain even if signatures match.
An optional generic annotation may request finer grouping:

```ember
@dispatch_domain("critical-math")
fn transform(x: i64) -> i64 { ... }
```

The annotation never weakens ABI partitioning and is not required for ordinary
source.

---

## 3. What this is—and is not

### 3.1 Correct description

- implicit environmental keying;
- key-dependent control-flow encoding;
- target-key-influenced module layout;
- transient runtime-key dispatch;
- finite-domain keyed routing.

### 3.2 Not asymmetric encryption

The analogy is useful—build-time and runtime values play different roles—but
this is not public-key cryptography. The artifact's physical topology is an
observable correctness oracle for analysis, and a local analyst on the target
machine can observe execution.

The design raises the work required to copy, patch, or reason about the
executor. It does not create an unextractable client-side secret.

### 3.3 No direct verifier

The runtime does not compute:

```cpp
if (k != embedded_K) fail();
if (hash(k) != embedded_hash) fail();
```

Instead `k` participates directly in routing arithmetic. Controlled wrong-key
traps arise because the wrong permutation reaches a padding target, not because
a comparison recognized a wrong machine.

The physical function ordering is nevertheless an **implicit correctness
oracle**: an analyst can recover the logical-to-physical permutation by
correlating code/function records with logical exports. This design avoids an
explicit key verifier; it does not make the target permutation secret, and
schema inspection cannot prove absence of every verifier surface.

### 3.4 Key-source strength is a separate concern

CPU model, CPUID strings, MachineGuid, and machine-id are identifiers and may be
cloneable. The dispatch system accepts opaque derived route material and does
not prescribe the provider.

Preferred providers, from stronger to weaker deployment posture:

1. random device secret sealed by TPM;
2. machine-scoped OS keystore/DPAPI secret;
3. activation-provisioned high-entropy device secret;
4. envlock stable hardware/software binding derivation;
5. optional CPUID-derived research mode.

No provider causes an expected key to be serialized into Ember artifacts.

---

## 4. Codebase audit

### 4.1 Dispatch and codegen files reviewed

- `src/ast.hpp`
- `src/parser.cpp`
- `src/sema.hpp`, `src/sema.cpp`
- `src/types.cpp`
- `src/codegen.hpp`, `src/codegen.cpp`
- `src/thin_ir.hpp`, `src/thin_lower.cpp`, `src/thin_emit.cpp`
- `src/thin_ir_ser.hpp`, `src/thin_ir_ser.cpp`
- `src/dispatch_table.hpp`
- `src/context.hpp`, `src/engine.hpp`, `src/engine.cpp`

### 4.2 Module and artifact files reviewed

- `src/module_registry.hpp`, `src/module_registry.cpp`
- `src/module_linker.hpp`
- `src/em_file.hpp`
- `src/em_writer.hpp`, `src/em_writer.cpp`
- `src/em_loader.hpp`, `src/em_loader.cpp`
- `src/em_type_codec.*`
- `examples/ember_bundle.cpp`
- `examples/ember_stub_main.cpp`
- `examples/ember_cli.cpp`

### 4.3 Runtime composition files reviewed

- `src/hot_reload.hpp`
- `src/lifecycle.hpp`
- `extensions/lifecycle/`
- `extensions/thread/`
- `extensions/coroutine/`
- `src/module_registry.*`
- function-reference and cross-module tests

### 4.4 Current call paths

| Path | Current target identity | Current physical resolution |
|---|---|---|
| Direct same-module script | compile-time logical slot | `dispatch_base + slot*8` |
| Indirect function handle | runtime i64 logical slot | allowlist then `dispatch_base + slot*8` |
| Lambda | local slot plus environment | allowlist then local dispatch |
| Direct cross-module | `(module_id, slot)` | registry table then target slot |
| Cross-module handle | tagged `(module_id, slot)` | target handle record/allowlist/table |
| Host by name/entry | name/entry slot | host obtains raw entry pointer |
| Lifecycle routine | retained slot | host indexes selected table |
| Thread/coroutine entry | retained slot | extension resolves and caches raw entry |
| Hot reload | stable slot | atomic replacement at same dispatch index |
| `call_raw` / executable pointer | arbitrary PERM_FFI capability | bypasses Ember dispatch entirely |

Every Ember-managed script path assumes logical slot equals physical array
index. `call_raw` is an explicit out-of-band PERM_FFI capability; keyed hosts
either disable it or document it as outside the keyed callable boundary.

### 4.5 Current dispatch storage

`DispatchTable` is a vector of atomic function pointers. It rejects null
publication and uses release/acquire operations. Generated code embeds the
backing array address.

`LoadedModule::dispatch` is currently a `std::vector<void*>`, not the exact same
atomic abstraction. A keyed design should converge JIT and loaded modules on a
shared publication-safe dispatch-storage interface before claiming identical
hot-reload/concurrency semantics.

### 4.6 Current function handles

- Intra-module handle: logical slot integer.
- Cross-module handle: tagged `(module_id, logical slot)`.
- Lambda: logical slot plus environment pointer.

This representation should remain logical. A physical slot must never be
captured in a handle.

### 4.7 Current module registry

The registry stores raw dispatch bases in a fixed-capacity vector whose base is
baked into generated code. Reload of an existing module name updates the table
pointer while preserving module ID.

Keyed dispatch needs a module record, not only a table pointer:

```text
mode
physical dispatch base/count
logical slot count
logical allowlist
key-independent layout/domain descriptors
strategy/version
```

No expected key belongs in that record.

Current same-name registry updates touch related metadata separately and are not
an atomic whole-record publication. The plan must fix this before concurrent
keyed module replacement.

### 4.8 Current `.em` format

V1–V5 persist one `slot_index` per function and assume it is both logical and
physical. V5 may contain validated Thin IR and re-emit x64 at load time.

A target-specific keyed module requires a new version because old readers would
misinterpret physical layout. Old formats remain identity layout.

### 4.9 Current `@obf_keyed`

The current feature is a CPUID equality gate in the tree emitter. It:

- bakes an expected CPUID signature;
- directly compares at function entry;
- traps on mismatch;
- has no Thin IR representation;
- forces tree-backend/raw fallback.

It is not the design in this plan. Keep it as a legacy/deprecated gate or rename
its documentation clearly. Do not reuse its semantics for implicit routing.

### 4.10 Current runtime hazards relevant to the plan

These must be addressed or explicitly fenced:

- registry replacement is not one atomic immutable-record publication;
- cross-module call geometry can become stale after table replacement;
- some host helpers retain raw function pointers rather than logical identity;
- thread/coroutine extensions cache entries that may outlive reload generations;
- lifecycle/thread/coroutine state lives in file-static globals
  (`g_routines`/`g_free` in `ext_lifecycle.cpp`, `g_ctx`/`g_dispatch_base`/
  `g_slot_count`/`g_threads` in `ext_thread.cpp`, and the equivalent
  file-scope registries in `ext_coroutine.cpp` and `ext_sync.cpp`) rather
  than per-runtime storage on a `ModuleInstance`;
- bare `fn` thread/coroutine APIs may call ABI-incompatible functions;
- raw outer thunks do not establish every checkpoint/reload/context invariant;
- cross-module numeric IDs remain registration-order dependent in persisted
  artifacts;
- a local CLI compilation path has historically baked borrowed allowlist
  storage whose owner may not survive compilation.

The keyed feature must not build new invariants atop these weak points without
first pinning ownership and lifetime tests.

---

## 5. Composable extension architecture

### 5.1 Keep extension categories explicit

The existing static, explicit registration model is good:

```cpp
ext_opt::register_passes(pass_registry);
ext_obf::register_passes(pass_registry, polymorphic_options);
ext_keyed_dispatch::register_keyed_dispatch(keyed_registry, options);
```

Linking a library is not discovery. The host decides which strategies exist.

### 5.2 Configured immutable factories

This phase depends on the shared extension-registry foundation delivered by
Phase 1 of the polymorphic plan:

```cpp
ExtensionStatus status =
    registry.add_factory("implicit-keyed-v1", [options] {
        return make_strategy_concept(ImplicitKeyedDispatchV1{options});
    });
```

Pass, profile, and keyed-dispatch registries use the same
`ExtensionResult`/`ExtensionError`, duplicate-name policy, configured-factory
rules, and deterministic descriptor ordering. Keyed dispatch must not clone
those mechanics into a second registry implementation.

### 5.3 Keyed-dispatch package and module-layout interface

```cpp
struct ModuleCallable {
    std::string name;
    uint32_t logical_slot;
    uint64_t abi_fingerprint;
    Visibility visibility;
    std::string dispatch_domain;
};

struct BuildKeyView {
    const uint8_t* bytes;
    size_t size;
};

struct ModuleLayoutPlan {
    uint32_t logical_slot_count;
    uint32_t physical_slot_count;
    std::vector<DispatchDomain> domains;
    std::vector<LogicalRoute> logical_routes;
    std::vector<uint32_t> build_physical_placement;
};

struct ModuleLayoutConcept {
    virtual Result<ModuleLayoutPlan> plan(
        const ModuleManifest&,
        BuildKeyView transient_target_key) const = 0;
    virtual Result<void> validate(const ModuleLayoutPlan&) const = 0;
};
```

`build_physical_placement` exists only during build/load planning. It is not a
runtime decoded map and does not contain the key. The artifact necessarily
reflects the resulting physical placement.

### 5.4 Paired dispatch strategy

The package owns one versioned mathematical contract shared by build and
runtime:

```cpp
struct DispatchStrategyConcept {
    virtual StrategyDescriptor descriptor() const = 0;
    virtual uint32_t permute(BuildOrRuntimeKeyView,
                             const DispatchDomain&,
                             uint32_t ordinal) const = 0;
    virtual Result<void> emit_resolver(X64Emitter&,
                                       const ResolveSite&) const = 0;
    virtual Result<void> validate_domain(const DispatchDomain&) const = 0;
};
```

The concrete implementation is selected during compilation/loading and emitted
into ordinary x64. Generated code does not make a virtual C++ call on each
script call unless the prototype intentionally chooses a helper-based resolver.

### 5.5 Prototype versus hardened resolver

#### Prototype

Call a stable C-ABI helper:

```cpp
void* ember_resolve_keyed_dispatch(const DispatchRecord*,
                                    uint32_t logical_slot,
                                    uint64_t transient_route_word);
```

Advantages:

- easiest to test;
- one implementation of bounds and permutation;
- minimal initial emitter changes.

Disadvantages:

- extra native call per script edge;
- central hook point;
- resolver logic not inlined.

#### Intended optimized path

Inline the pinned finite-domain permutation and table load at each generated
call site. Keep the helper as a reference implementation and host-call path.

Both modes must produce identical physical indices for golden vectors.

---

## 6. Key lifecycle

### 6.1 Provider boundary

Use one provider-to-domain adapter shared with the polymorphic engine:

```cpp
struct DerivationRequest {
    std::string_view domain;
    std::span<const uint8_t> public_context;
};

class DerivedMaterialProvider {
public:
    virtual ~DerivedMaterialProvider() = default;
    virtual Result<std::array<uint8_t, 32>> derive(
        const DerivationRequest&) const = 0;
};

class DispatchKeyAdapter {
public:
    explicit DispatchKeyAdapter(
        std::shared_ptr<const DerivedMaterialProvider> provider);
    Result<uint64_t> route_word(const ModuleId&, uint32_t strategy_version,
                                std::string_view purpose) const;
};
```

The provider is immutable shared ownership held by the build configuration or
runtime for the full operation lifetime. It returns errors through the same
structured `Result` convention; required keyed mode never falls back silently.
The adapter domain-separates `ember/dispatch`, `ember/layout`, and
`ember/passes`, and folds route output only after deriving 256-bit material.
The root machine material remains inside the provider.

### 6.2 Build-time key

For a target-specific build:

1. the shared provider derives 256-bit target material for the requested domain;
2. the dispatch adapter derives route/layout words and the layout planner consumes them;
3. the polymorphic `SeedDeriver` adapter consumes a separate `ember/passes` domain;
4. build fills physical layout;
5. temporary key buffers are best-effort wiped;
6. no expected key/verifier enters the artifact.

### 6.3 Runtime key

At the safe outer host-to-script boundary:

1. the runtime-owned adapter derives the runtime route word or returns a structured failure;
2. the thunk installs it in a reserved callee-saved register;
3. nested script calls use it directly;
4. conforming native calls preserve it under the platform ABI;
5. thunk clears the transient register before restoring the caller's original
   register and returning.

### 6.4 Register choice

On Win64 x64, reserve `r15` in keyed mode:

- remove `r15` from the regalloc pool;
- keep `r14` as `context_t*`;
- outer keyed thunks install `r15`;
- prologues/epilogues preserve normal ABI expectations;
- generated calls treat `r15` as read-only route material.

Do not add the key to `context_t`. This prevents a long-lived ordinary field
from becoming the default storage path. Provider locals and the transient
register still exist during derivation/execution; document that honestly.

The current try/catch buffer already saves/restores `r15` in slot 5. In keyed
mode that slot preserves the invariant route word, not a regalloc value; in
legacy mode it keeps its current meaning. Add throw-to-catch keyed tests.

### 6.5 Recursion, re-entry, and callbacks

One outer call tree uses one route word. Recursive and direct nested script
calls inherit `r15`. A native-to-script re-entry must use a keyed re-entry thunk
that preserves or explicitly reinstalls the same route word.

### 6.6 Threads

Each independently entered OS thread:

- owns its own `context_t` under the normal model;
- invokes the provider at its outer keyed thunk;
- installs its own transient `r15` value;
- does not copy a key into process-global extension state.

The current in-context thread extension serializes shared-context calls. Its
file-static globals (`g_ctx`, `g_dispatch_base`, `g_slot_count`, `g_threads`)
make it a process-global singleton: only one runtime's thread store can exist
per process, and `thread_init` overwrites whatever a previous runtime
installed. It must stop caching raw entries and instead retain logical
callables plus a provider/runtime reference. Keyed `thread_init` receives a
per-runtime dispatch record/provider handle—writing into state owned by the
caller's `ModuleInstance`, not file-scope globals—and workers call a keyed
outer thunk; the existing signature remains an identity-mode wrapper that
targets the single active runtime's per-runtime state.

### 6.7 Coroutines

A suspended coroutine can retain machine registers in a fiber context. The
coroutine contract must explicitly save/restore `r15`, pin the code generation,
and avoid leaking one runtime's route state into another fiber. The current
coroutine extension's fiber store and setup state are file-static globals, so
two runtimes would share one fiber registry; move that state onto the
per-runtime container on `ModuleInstance`. Initially, `coroutine_init`
receives keyed-mode state—writing into per-runtime storage, not file-scope
globals—and `coroutine_start` returns a typed unsupported-mode error until
dedicated tests prove these invariants.

---

## 7. ABI-domain classification

### 7.1 Canonical fingerprint requirements

An ABI fingerprint must include every property that affects calling sequence:

- target architecture and calling convention version;
- return representation: void, GP scalar, XMM f32/f64, slice pair, lambda pair,
  or hidden aggregate destination;
- each parameter's canonical type and word count;
- GP/XMM/stack word class per position;
- aggregate size, alignment, and nominal identity;
- slice/lambda/function-handle distinctions;
- hidden environment/return words;
- context/keyed calling mode;
- coroutine/special-entry ABI properties.

Do not classify only by source spelling or argument count. Do not use
`std::hash`.

### 7.2 Visibility isolation

Public/exported and private functions remain in different domains. Otherwise a
wrong key could route an external call into a same-ABI private helper and expand
the callable surface.

### 7.3 Padding target generation

Generate one non-returning target per ABI domain or one universal target whose
entry ABI safely ignores all incoming argument classes and traps through
Ember's recoverable mechanism.

Prefer per-domain generated stubs when return/hidden-pointer conventions make a
universal stub difficult to prove.

Add a distinct trap reason:

```cpp
TrapReason::KeyedDispatchPadding
```

The padding target does not compare a key. It simply occupies one physical
ordinal selected by the build permutation.

### 7.4 ABI tests as specification

Reuse and expand `binding_abi_test.cpp`/`win64_abi_test.cpp` cases for:

- 0–6 GP words;
- f32/f64 XMM positions;
- mixed GP/XMM arguments;
- slice and lambda pairs;
- by-value aggregates with partial final words;
- hidden aggregate returns;
- function handles;
- recursion and depth checks.

---

## 8. Permutation design

### 8.1 Required properties

For each domain size `n`, strategy version, salt, and key:

- `P` is a total bijection on `[0,n)`;
- runtime is bounded;
- no modulo/out-of-range result;
- build and runtime implementations are byte-for-byte specified;
- different domains use independent tweaks;
- arithmetic overflow behavior is explicit unsigned modular arithmetic;
- all `n` from 2 through configured maximum are supported.

### 8.2 Initial reference algorithm

For the first correctness prototype, a versioned affine permutation is easy to
prove and test:

```text
P(x) = (a*x + b) mod n
where gcd(a,n) = 1
and a,b derive from mix(key, domain_salt, strategy_version)
```

Select `a` deterministically until coprime with `n`. Use widened checked
multiplication or specified modular arithmetic.

This is not the final hardness target; it is the smallest bijection that lets
us validate the complete architecture.

### 8.3 Hardened strategy

Add a separate registered version using a small keyed Feistel/ARX permutation
over a power-of-two domain with bounded cycle-walking into `[0,n)`.

Requirements:

- hard cap on cycle-walk iterations;
- fallback that remains a bijection rather than producing an unchecked index;
- golden vectors for every supported domain size class;
- build/runtime helper and inline emitter parity.

Do not silently change algorithm `v1`; register `implicit-keyed-v2`.

### 8.4 Domain salt

The salt/tweak is public and may be stored. It should derive from stable public
module/domain/version metadata and optional build randomness. It is not a key
or verifier.

### 8.5 Stateful routing—deferred

A later mode may mix a per-thread route state with call history so wrong-key
divergence compounds. Defer it because it affects:

- recursion;
- exception unwinding;
- reentrant natives;
- threads;
- coroutine suspension;
- deterministic replay;
- hot reload.

Stateless domain permutation is the first complete milestone.

---

## 9. Compiler and module-build pipeline

### 9.1 Why per-function passes cannot do this alone

A `ThinFunction` pass sees one function. It does not know:

- all module signatures;
- domain membership;
- physical slot count;
- generated padding entries;
- final export/hot-reload geometry.

Add a module planning stage instead of widening every function pass into an
untyped global callback.

### 9.2 Required build order

```text
parse + sema
    ↓
assign stable logical slots
    ↓
build canonical module callable manifest
    ↓
classify exact ABI/visibility domains
    ↓
layout extension consumes transient target K
    ↓
allocate final physical dispatch storage
    ↓
compile functions with keyed CodeGenCtx descriptors
    ↓
run optional K-derived polymorphic passes
    ↓
finalize function pages
    ↓
place entries using build plan
    ↓
fill padding entries
    ↓
validate complete module
    ↓
publish/register once
```

The physical dispatch backing address must be final before generated code or
relocations embed it.

### 9.3 `CodeGenCtx` additions

Prefer one borrowed immutable descriptor rather than scattered raw fields:

```cpp
struct KeyedDispatchCodegen {
    const DispatchStrategyDescriptor* strategy = nullptr;
    const ModuleDispatchLayout* layout = nullptr;
    const ModuleRegistryView* registry = nullptr;
    RuntimeKeyLocation runtime_key = RuntimeKeyLocation::R15;
};

struct CodeGenCtx {
    // existing fields...
    const KeyedDispatchCodegen* keyed_dispatch = nullptr;
};
```

Null means exact legacy behavior.

### 9.4 Call lowering

Thin IR retains logical slots:

```text
CallScript.meta.slot          = logical slot
CallCrossModule.meta.slot     = target logical slot
CallIndirect handle           = logical identity
```

Keyed behavior is selected during emission via `CodeGenCtx`, not by rewriting
logical slot metadata into physical values.

### 9.5 Same-module direct call

Keyed mode replaces:

```text
call [dispatch_base + logical_slot*8]
```

with:

```text
logical_slot → domain/ordinal descriptor
P(r15, domain, ordinal) → physical index
load non-null entry from physical dispatch
call entry
```

Resolver scratch/setup occurs before user argument registers are finalized or
uses a verified call-stash plan so it cannot clobber RCX/RDX/R8/R9/XMM args.

### 9.6 Indirect calls and lambdas

- Validate the logical handle with the existing allowlist first.
- Resolve logical slot through the keyed strategy.
- Keep the environment pointer unchanged.
- Do not expose a physical slot through the function-handle value.

### 9.7 Cross-module calls

Registry lookup must yield a target `ModuleDispatchRecord`, not merely a raw
table base. The caller supplies its transient `r15`; target module/domain salts
produce target-specific physical resolution.

Imported stable identities are captured at link time as logical references and
rebound locally at each call site, never pre-resolved to a physical entry. The
linker records `CallCrossModule.meta.slot` as the target's logical slot and
binds the target `ModuleDispatchRecord` reference; physical resolution is
deferred to emission, where the caller's transient `r15` and the target
domain's salt rebind the imported logical ordinal into the target's physical
domain. No import table, relocation, or handle may store a resolved physical
index, because a wrong-key caller would then dereference a stale target
placement instead of following the live keyed topology.

The registry/linker must publish both counts per module, extending today's
single `set_dispatch_slot_count` into separate logical and physical counts:

```text
target_logical_slot_count   = the module's stable logical identity range
target_physical_slot_count  = the module's keyed dispatch storage size
```

A cross-module caller always speaks logical identity, so the load-time
validator range-checks the caller's `meta.slot` against the target's published
**logical** slot count, not the physical count. The keyed strategy then maps
the validated logical ordinal into the target's physical domain. This preserves
the current X1 check (`slot < target_slot_count`) as
`logical_slot < target_logical_slot_count` in keyed mode and leaves the legacy
identity path comparing against the single (logical==physical) count; the
physical count remains a separate invariant the loader confirms against the
published dispatch storage.

A keyed caller may call a legacy identity module through a strategy-defined
identity record. A legacy caller cannot call a keyed target because it has no
runtime-key contract; the JIT link/load stage must reject that edge at link
time rather than bake a legacy `mov r11,[registry+mod_id*8];
mov r11,[r11+slot*8]` sequence that would index a keyed physical table as if
it were logical. A keyed module record therefore carries a `DispatchMode` the
linker inspects before allowing a legacy caller to bind to it.

### 9.8 Host-to-script calls

Add safe keyed outer-call APIs:

```cpp
CallResult ember_call_keyed_void(ModuleInstance&, LogicalCallableId,
                                 context_t&, const DispatchKeyProvider&);
CallResult ember_call_keyed_i64(ModuleInstance&, LogicalCallableId,
                                context_t&, int64_t,
                                const DispatchKeyProvider&);
```

They establish:

- context register;
- transient route register;
- recoverable checkpoint;
- reload/generation guard;
- logical resolution;
- cleanup/wiping on normal and trapped exit.

Raw legacy thunks remain for unkeyed mode.

A host that needs a resolved entry pointer (lifecycle tick, thread entry, or
FFI hand-off) must obtain it through a keyed resolver, never by indexing the
dispatch storage with a bare logical slot:

```cpp
Result<void*> resolve_entry_keyed(ModuleInstance&, LogicalCallableId,
                                  const DispatchKeyProvider&);
Result<void*> resolve_entry_by_name_keyed(ModuleInstance&,
                                          std::string_view name,
                                          const DispatchKeyProvider&);
```

`resolve_entry_keyed` validates the logical identity against the module's
published logical count, derives the route word from the provider, applies the
strategy permutation `P(k, domain, ordinal)`, and returns the finalized entry
from the target's physical dispatch record. `resolve_entry_by_name_keyed` maps
an export name to a `LogicalCallableId` first, then performs the same
resolution; for a loaded V6 module this is the §11.5
`resolve_entry_by_name(name, transient_route_word)` path. Both return a
structured failure before yielding any pointer if the module is keyed and no
provider is supplied, the logical slot exceeds the published logical count, or
the domain/ABI fingerprint does not match.

A keyed module must forbid raw logical indexing of its dispatch storage. No
host path may read `physical_slots[logical_slot]` directly: the physical array
is permutation-ordered, so a bare logical index reaches an arbitrary same-domain
entry rather than the intended callable. The existing `LoadedModule::entry_by_name`
and bare `entry()` accessors return unkeyed raw entries and are valid only for
legacy identity-layout modules; a keyed `ModuleInstance` must not expose them
unsurrounded, or must wrap them so every lookup routes through the keyed
resolver. The legacy `ember_call_void`/`ember_call_i64` thunks that accept a
pre-resolved `void* entry` are an unkeyed-mode convenience and are not a
sanctioned keyed entry path.

---

## 10. Runtime ownership and publication

### 10.1 `ModuleDispatchRecord`

```cpp
struct ModuleDispatchRecord {
    DispatchMode mode;
    uint32_t strategy_version;
    const std::atomic<void*>* physical_slots;
    uint32_t physical_slot_count;
    uint32_t logical_slot_count;
    const LogicalRoute* logical_routes;
    const DispatchDomain* domains;
    uint32_t domain_count;
    const uint8_t* logical_allowlist;
};
```

No expected key or key digest appears.

### 10.2 Atomic module publication

Publish an immutable record through one atomic registry entry:

```cpp
std::atomic<const ModuleDispatchRecord*> current;
```

Build/validate the complete new generation privately, then release-store one
record pointer. Readers acquire-load one coherent generation. Retire old records
and pages only after all guards drain.

This replaces separate observable updates of table pointer, allowlist, and
counts.

### 10.3 `ModuleInstance`

Introduce a host-owned object that keeps all lifetimes together:

- stable module identity;
- logical exports/signatures;
- current immutable generation record;
- physical dispatch storage;
- globals;
- executable pages;
- hot-reload domain;
- provider/strategy ownership references;
- lifecycle/thread/coroutine state scoped to this runtime.

The current extensions store their mutable state in file-static globals:
`g_routines`/`g_free` in `ext_lifecycle.cpp`, `g_ctx`/`g_dispatch_base`/
`g_slot_count`/`g_threads` in `ext_thread.cpp`, and the equivalent
file-scope fiber registries and setup pointers in `ext_coroutine.cpp` and
`ext_sync.cpp`. These are process-global singletons—two `ModuleInstance`s in
one process share (and clobber) the same routine table, thread registry, and
fiber store. Keyed dispatch cannot tolerate that, because two runtimes may
carry different dispatch records, providers, and route words, and a
file-static `g_ctx` or `g_dispatch_base` can only record one runtime's
values. Move every extension's mutable state off the file-scope globals and
onto a per-runtime container owned by `ModuleInstance`. The extension
init/reset APIs (`thread_init`, `coroutine_init`, `lifecycle::reset`, etc.)
must receive and populate a per-runtime state struct instead of writing to
file-scope variables; the existing zero-argument or single-context signatures
become identity-mode wrappers that target the single active runtime's
per-runtime state. No keyed extension path may read or write a file-static
global.

Avoid adding more process-global extension state.

### 10.4 Publication invariant

No keyed module becomes reachable until:

- every logical route is total and unique;
- every domain is valid;
- every physical position is filled exactly once;
- every real entry's ABI matches its domain;
- every padding entry is finalized;
- all executable pages are RX;
- registry generation publication succeeds.

---

## 11. Target-specific `.em` modules

### 11.1 Separate optional mode

Executor-only keyed dispatch does not require a new module format if modules
remain legacy logical content and the executor wraps their callable surface.

A module compiled **for the target machine** does require a versioned format,
because its stored function order/physical placement is influenced by target
`K`.

### 11.2 Important deployment distinction

If source/IR is recompiled and laid out from the local runtime key on whatever
machine launches it, the module will adapt to that machine and run correctly.
That is not target-specific binding.

Target-specific module mode therefore requires one of:

- precompile on the intended target;
- build service receives target-derived build material;
- activation produces a target-specific artifact;
- executor contains a prebuilt key-influenced topology that the generic module
  cannot bypass.

### 11.3 New format version

Introduce a new `.em` version rather than changing V1–V5 meaning. The exact
number should be chosen against current repository state; this plan calls it
**V6**.

V6 is gated on Thin IR blob version 2. A V6 module that carries validated Thin
IR must serialize that IR under blob-v2—the polymorphic plan's `data_temp_off`
serialization fix—because blob-v1 loses `data_temp_off` and cannot round-trip a
function whose keyed placement depends on the complete IR. The loader rejects a
V6 record whose embedded IR blob declares v1; it does not silently fall back to
the v1 `data_temp_off = 0` path, since that would re-emit a keyed function from
incomplete metadata. This dependency is the reason the file map's
`thin_ir_ser.*` entry consumes the polymorphic plan's blob-version fix before
V6 work begins.

V6 should retain validated Thin IR where possible and add:

- dispatch strategy ID/version;
- logical and physical slot counts;
- public domain descriptors;
- logical route descriptors;
- function records stored in physical order or with validated physical
  placement;
- padding records/positions;
- canonical ABI fingerprints;
- a secure capability matrix declaring every runtime feature the module
  requires, so the loader can validate the module against the host's supported
  capabilities before any allocation.

It must not add:

- target key;
- expected runtime key;
- key hash/verifier;
- raw machine fingerprint;
- a runtime predecoded permutation map used instead of `P(k,...)`.

The physical order itself is the build-time key's influence and is necessarily
observable in the artifact.

The capability matrix is the V6 header's requirements declaration, not a
verifier. It lists each runtime feature the module depends on—keyed dispatch
runtime, blob-v2 Thin IR re-emit, registered strategy ID/version, dispatch
mode, and the ABI domain set—under a versioned, extensible numbering so future
features add declared capabilities without a format break. The loader checks
every declared capability against the host's supported set before any
allocation; an unsupported, unrecognized, or self-contradictory requirement is
a structured load failure, never a silent downgrade to raw-x86 or to an
identity-layout path. This generalizes the existing v5 secure-default
`allow_raw_x86` gate to a whole-module requirements contract: a V6 module that
requires keyed dispatch cannot load on a host lacking the keyed runtime, and a
V6 function whose IR is blob-v2 cannot be re-emitted by a host whose Thin IR
deserializer only knows blob-v1. The matrix carries no key material.

### 11.4 Loader behavior

The loader:

1. parses and bounds-checks all disk-controlled sizes;
2. validates the capability matrix against the host's supported runtime
   features; an unsupported, unrecognized, or self-contradictory requirement is
   a structured load failure before any allocation, never a silent fallback to
   raw-x86 or to an identity-layout path;
3. validates logical slot density/uniqueness;
4. validates domain membership and ABI fingerprints;
5. validates physical placement uniqueness and complete coverage;
6. validates exactly configured padding coverage;
7. allocates stable physical storage;
8. validates/re-emits V6 Thin IR before RW allocation where possible, and
   rejects an embedded blob-v1 IR per §11.3;
9. patches relocations against the final module record/storage;
10. seals pages RX;
11. publishes only after complete success.

The loader does not derive a runtime key to reorder the module. Reordering with
local `k` would make a wrong machine rebuild a self-consistent topology and
therefore defeat target-specific behavior. The loader never silently falls back
to raw-x86: a V6 function that cannot be satisfied by the host's capabilities
(blob-v2 Thin IR the host cannot re-emit, a strategy the host did not register,
or raw-x86 content under the secure default) fails with a structured error at
the capability-matrix or re-emit gate, not by swapping in a degraded code path.

### 11.5 Names and exports

Name directories and exports map to logical slots. `entry_by_name()` cannot
return an unkeyed raw entry for a keyed module; it must not perform raw logical
indexing of the physical dispatch array. The load-side resolver is the §9.8
keyed name path:

```cpp
resolve_entry_by_name(name, transient_route_word)
```

or require safe keyed call APIs. Either way, raw logical indexing is forbidden
for keyed modules: every name-to-entry lookup applies the strategy permutation
before touching dispatch storage.

### 11.6 Raw-x86 policy

Preserve existing secure-default policy and extend it to V6:

- validated all-IR artifacts accepted under secure defaults;
- raw-x86 fallback requires explicit trust/signature policy
  (`EmLoadPolicy{allow_raw_x86=true}`), the same opt-in the v1–v4 and v5
  raw-x86 paths already require;
- keyed format does not weaken this rule;
- the loader never silently falls back to raw-x86, to an identity-layout path,
  or to any degraded re-emit mode; an unsatisfiable capability is a structured
  load failure, not a quiet downgrade. This generalizes the existing
  "reject before executable allocation" secure default to the V6 capability
  matrix: a V6 module that declares a requirement the host cannot meet is
  rejected at step 2, with no executable page allocated and no partial
  publication.

### 11.7 Signing

Artifact signatures authenticate the resulting V6 bytes after target-specific
layout. Signature verification is orthogonal to environmental routing and does
not carry the machine key.

---

## 12. Hot reload

### 12.1 Stable identity

Reload still identifies a function by logical slot and exact canonical ABI.
Physical position is derived from the deployment target key and immutable domain
metadata.

### 12.2 Reload build flow

```text
logical callable + replacement source
    ↓
verify unchanged signature/domain
    ↓
transiently derive target build route word
    ↓
physical = P(K, domain, ordinal)
    ↓
compile/finalize replacement
    ↓
publish at physical slot under generation guard
```

No stored expected key confirms that the reload provider supplied the same key.
Supplying a different key remains memory-safe but may replace a different
same-domain destination and produce incorrect semantics. Deployment policy owns
provider correctness.

### 12.3 Multi-slot transactions

A change to domain membership or strategy/layout cannot be a single-slot hot
reload. It requires building and atomically publishing a complete new module
generation.

### 12.4 Delayed work

Threads, lifecycle routines, and suspended coroutines retain logical identities
and resolve when they actually enter. They must not cache raw entries across a
reload unless they hold a generation guard for the entire lifetime.

---

## 13. Polymorphic-engine composition

Use domain-separated derived material:

```text
K_route  = derive(machine_root, "ember/dispatch", module, strategy_version)
K_pass   = derive(machine_root, "ember/passes", module, profile, build_id)
K_layout = derive(machine_root, "ember/layout", module, layout_version)
```

The polymorphic pass engine consumes only `K_pass`-derived seed material. The
module planner consumes only `K_layout`/`K_route` as specified. None of these
values is persisted as an expected key.

Recommended order:

```text
stable logical manifest
    ↓
ABI domains
    ↓
target-key module layout plan
    ↓
per-function polymorphic passes with independent derived seed
    ↓
regalloc
    ↓
emit/finalize
    ↓
physical publication
```

Function diversity is not enforcement on its own. Dispatch topology is the
implicit runtime coupling.

---

## 14. TDD strategy

### 14.1 New focused targets

```text
keyed_dispatch_math          permutation/domain property tests
keyed_dispatch_codegen       tree + Thin IR call lowering
keyed_dispatch_runtime       outer thunk/provider/register lifetime
keyed_dispatch_modules       JIT/cross-module/handles/hot reload
em_v6_keyed                  target-specific artifact writer/loader
keyed_dispatch_extensions    lifecycle/thread/coroutine integration
```

Existing adjacent gates:

```text
binding_abi
win64_abi
function_refs
cross_module_handles
v0_5_live_modules
v0_6_hot_reload
thread_safety
in_context_threads
thin_ir_ser
em_v5_ir
em_v5_mixed
em_loader_hardening
bundler_test
```

Suggested command:

```bash
cmake -S . -B build-tdd -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DEMBER_BUILD_VST3=OFF
cmake --build build-tdd
ctest --test-dir build-tdd --output-on-failure \
  -R "keyed_dispatch|em_v6_keyed|binding_abi|function_refs|cross_module_handles|hot_reload"
```

Final gate runs full CTest.

### 14.2 Four mandatory buckets

#### Standard / should succeed

- Identity strategy preserves legacy behavior.
- Correct target/runtime key routes every logical callable correctly.
- Same target key reproduces physical layout.
- Different pinned target keys produce different physical layouts.
- Direct, indirect, lambda, host, and cross-module calls agree.
- Tree and Thin IR backends agree.
- V6 write/load/run works with correct runtime key.
- Hot reload preserves logical handles.

#### Should fail

- Duplicate/overlapping physical placements.
- Missing logical route or padding entry.
- ABI fingerprint mismatch.
- Cross-visibility domain membership.
- Unsupported strategy version.
- Legacy caller linking to keyed target.
- Keyed host call with unavailable provider.
- Raw logical indexing of a keyed module's dispatch storage by a host path.
- Malformed V6 descriptor before executable allocation.
- V6 capability matrix declaring a feature the host does not support.
- V6 capability matrix with an unrecognized or self-contradictory requirement.
- Silent raw-x86 or identity-layout fallback when a V6 capability is
  unsatisfiable (the loader must fail, not degrade).
- V6 record whose embedded Thin IR blob declares v1.
- Cross-module import table or relocation storing a resolved physical index.
- Physical table publication with null/unfinalized entry.

#### Edge

- Key route words `0` and `UINT64_MAX`.
- Domain sizes 2 through configured maximum.
- Singleton real function plus padding.
- First and last logical/physical slots.
- Empty module and no callable functions.
- Every aggregate/slice/float calling class.
- Recursion, nested native re-entry, deep call stacks.
- Concurrent contexts with different providers.
- Two `ModuleInstance`s in one process with independent extension state.
- Reload while old calls remain guarded.

#### Regression

- No physical slot appears in a function handle.
- No host path indexes keyed dispatch storage by raw logical slot.
- Wrong keys remain within the finite physical range, and each selected entry's
  stored ABI fingerprint and visibility domain exactly match the caller; this
  is not satisfied by a tautological contiguous-range assertion alone.
- Wrong keys never cross ABI/visibility classes.
- No raw null/wild call on padding or malformed data.
- Registry publication is atomic whole-record publication.
- Thread/coroutine/lifecycle paths do not cache stale raw entries.
- No keyed extension path reads or writes a file-static global; all state is
  per-runtime on `ModuleInstance`.
- Unkeyed output/call sequence remains unchanged when feature is disabled.
- No V6 load path silently degrades to raw-x86 or identity layout.
- V1–V5 artifacts retain identity-layout behavior.

### 14.3 Red-green implementation order

#### Red 1 — permutation properties

Create property tests for determinism, bijection, bounds, domain separation,
and all domain sizes.

**Green:** implement reference permutation in one core helper.

#### Red 2 — ABI classifier

Add canonical fingerprints for every existing ABI test fixture and collision
negative cases.

**Green:** implement core classifier with stable byte encoding.

#### Red 3 — logical/physical layout

Build a module with several same-ABI functions and singleton domains. Assert
correct physical placement under two pinned target keys and complete padding.

**Green:** implement layout registry/plan and strict validation.

#### Red 4 — runtime resolver

For correct and many wrong keys, assert outputs are always in-range and same
ABI domain; selected wrong keys should demonstrate alternate/padding routing.

**Green:** implement C-ABI reference resolver and immutable dispatch record.

#### Red 5 — keyed outer thunk

Assert provider invocation once per outer call, `r14` correctness, `r15`
installation/clearing/restoration, recursive inheritance, and provider failure.

**Green:** implement keyed engine thunks and remove `r15` from keyed regalloc.

#### Red 6 — same-module calls

Add direct/indirect/lambda tests through both backends.

**Green:** emit keyed resolution while preserving argument registers and depth
accounting.

#### Red 7 — module registry and cross calls

Add keyed JIT-to-JIT and legacy/keyed compatibility tests.

**Green:** publish immutable module dispatch records atomically and update all
cross-module call/handle paths.

#### Red 8 — host/lifecycle/thread/coroutine

First assert no stale raw pointer is retained across reload.

**Green:** migrate each extension to logical callable resolution. Keep coroutine
keying disabled until full register/generation tests pass.

#### Red 9 — V6 artifact

Add format round-trip, malformed descriptor, correct/wrong runtime key,
capability-matrix validation (supported/unsupported/unrecognized), no silent
raw-x86 or identity-layout fallback, and no expected-key-field tests.

**Green:** implement V6 writer/loader with a secure capability matrix and
never-silent-fallback rejection, without runtime re-layout.

#### Red 10 — hot reload

Assert correct physical replacement, logical handle stability, old-page pinning,
and wrong build-provider safety.

**Green:** integrate logical→physical derivation with generation publication.

#### Red 11 — polymorphic composition

Combine machine-derived pass seeds, keyed physical layout, V6 load, cross-module
calls, and reload.

**Green:** resolve only observed composition defects.

### 14.4 Property/fuzz tests

For random keys, salts, domain sizes, and ordinals:

```text
assert P is a permutation
assert every output is within domain
assert build/runtime helper parity
assert inline emitter/helper parity
assert wrong key never crosses ABI/visibility domain
```

For malformed V6 bytes, assert rejection before any RX page or registry
publication. Extend the existing `.em` fuzz harness with V6 descriptor cases
(or add `fuzz_em_v6_loader`) alongside hand-built negatives. For a V6
capability the host cannot satisfy, assert a structured
rejection before any RX page or registry publication, with no raw-x86 or
identity-layout fallback.

### 14.5 Wrong-key assertions

Do not merely assert "result differs." Also assert:

- no OOB read;
- no incompatible function call;
- no raw access violation;
- no partial module publication;
- padding uses recoverable Ember trap;
- all selected destinations belong to the declared domain.

### 14.6 Static/artifact assertions

Tests should inspect the V6 schema and public structures to confirm they contain
no field representing:

- expected target key;
- machine fingerprint;
- key hash/verifier;
- direct comparison constant.

This does not claim generated code reveals nothing; it proves the architecture
is routing-based rather than an explicit verifier.

### 14.7 Performance measurements

Measure:

- keyed call overhead for helper and inline modes;
- code-size increase per call site;
- register-pressure effect from reserving `r15`;
- cross-module overhead;
- build/layout time;
- hot-reload publication time;
- thread/coroutine entry cost.

Set an overhead budget before making keyed dispatch a default for any host.

---

## 15. Implementation phases

### Phase 0 — contract and baseline — DONE

- Freeze logical identity, exact ABI domains, key lifecycle, and wrong-key
  safety semantics.
- Capture current adjacent CTest baseline.
- Clearly separate legacy `@obf_keyed` from implicit routing.

**Gate:** reviewed specification and unchanged legacy tests.

### Phase 1 — core mathematical/runtime prototype — DONE

- Stable ABI fingerprint.
- Reference permutation.
- Domain descriptors and layout validation.
- Padding stubs.
- Reference resolver.
- Identity strategy.

**Gate:** exhaustive/property tests for domains and wrong-key memory safety.

### Phase 2 — module planning on the shared registry foundation — DONE

Prerequisite: polymorphic-plan Phase 1 (`ExtensionResult`, configured factory,
collision policy, deterministic descriptor listing).

- One configured `KeyedDispatchRegistry`; each entry pairs its layout planner,
  permutation, resolver/emitter, and validator.
- Two-stage module build manifest/plan.
- Shared structured diagnostics and deterministic listing.

**Gate:** identity/no-extension compatibility and two pinned keyed layouts.

### Phase 3 — same-module JIT integration — DONE

- Keyed outer thunks/provider.
- `r15` reservation.
- Tree and Thin IR direct/indirect/lambda emission.
- Full ABI matrix.

**Gate:** correct key semantics, wrong-key domain safety, legacy byte/behavior
compatibility.

### Phase 4 — atomic module registry and cross-module integration — DONE

- Immutable `ModuleDispatchRecord` publication.
- Logical cross-module slots/handles.
- Stable module records and generation guards.
- Reject legacy-to-keyed unsupported edges.

**Gate:** bidirectional JIT module and handle tests under correct/wrong keys.

### Phase 5 — target-specific V6 modules — DONE

- New format/schema with a versioned capability matrix.
- V5-compatible validated IR body where possible.
- Physical-order loading without runtime re-layout.
- Capability-matrix validation against host features; never-silent-fallback
  raw-x86 rejection.
- Keyed call APIs for loaded modules.
- Signing/secure-default compatibility.

**Gate:** V6 hardening suite and V1–V5 full compatibility.

### Phase 6 — reload and runtime extensions — DONE

- Logical hot reload to target physical slot.
- Lifecycle routines resolve at invocation.
- Threads derive key at their outer boundary.
- Coroutine register/generation pinning or explicit keyed-mode rejection.

**Gate:** delayed-start/suspension/reload concurrency tests.

### Phase 7 — inline hardened strategy — DONE for the shipped implicit-keyed-v1 helper/emitter contract

- Registered `implicit-keyed-v2` PRP.
- Inline x64 resolution.
- Helper/inline golden parity.
- Performance/regalloc tuning.

**Gate:** complete correctness plus measured overhead budget.

### Phase 8 — polymorphic composition — DONE

- Domain-separated machine-derived pass seed.
- Target-specific module code diversity.
- Combined build manifest and deployment tooling.

**Gate:** end-to-end executor + module + reload + cross-module tests.

---

## 16. File-level change map

| File/area | Planned change |
|---|---|
| `src/extension_registry.hpp` (shared prerequisite) | Common configured factory/result/diagnostic contract from polymorphic Phase 1 |
| `src/dispatch_abi.*` (new) | Stable canonical ABI fingerprints/domains |
| `src/keyed_dispatch.*` (new) | Strategy descriptors, permutation, resolver, validation |
| `src/module_layout.*` (new) | Manifest and ephemeral build plan owned by keyed package |
| `src/dispatch_table.hpp` | Physical storage abstraction; preserve legacy identity wrapper |
| `src/context.hpp` | New padding trap reason only; no persistent key field |
| `src/engine.*` | Safe keyed outer-call thunks, entry/name resolvers, provider lifecycle |
| `src/codegen.hpp` | Borrowed immutable keyed-dispatch descriptor |
| `src/codegen.cpp` | Tree backend keyed direct/indirect/cross resolution |
| `src/thin_ir.hpp` | Keep logical slots; append only required strategy metadata |
| `src/thin_lower.cpp` | Preserve logical identities and ABI-domain references |
| `src/thin_emit.cpp` | Helper then inline keyed resolver; reserve `r15` |
| `src/regalloc.cpp` | Remove `r15` from pool in keyed mode |
| `src/thin_ir_ser.*` | First consume the polymorphic plan's single blob-version fix for `data_temp_off`; keyed mode keeps function IR logical and adds no strategy/domain fields unless a later concrete ThinOp requires them |
| `src/em_file.hpp` / V6 module section | New target-specific format; own keyed strategy/domain/layout metadata at module scope; versioned capability matrix in the V6 header |
| `src/module_registry.*` | Atomic immutable module dispatch-record publication |
| `src/module_linker.hpp` | Keyed/legacy compatibility and logical export wiring |
| `src/em_writer.*` | Physical layout serialization with no key/verifier field |
| `src/em_loader.*` | Strict keyed layout validation, capability-matrix validation against host features, never-silent-fallback raw-x86 rejection, and final publication |
| `src/hot_reload.hpp` | Logical→physical reload and generation transaction |
| `extensions/lifecycle/*` | Move file-static `g_routines`/`g_free` to per-runtime state on `ModuleInstance`; store logical callable, resolve per invocation |
| `extensions/call_raw/*` | Explicit PERM_FFI out-of-band exception or keyed-host prohibition |
| `extensions/thread/*` | Move file-static `g_ctx`/`g_dispatch_base`/`g_slot_count`/`g_threads` to per-runtime state on `ModuleInstance`; provider-aware logical resolution, no cached stale entry |
| `extensions/coroutine/*` | Move file-static fiber store/setup globals to per-runtime state on `ModuleInstance`; `r15`/generation preservation or fail-closed unsupported mode |
| `examples/ember_cli.cpp` | Strategy/provider/target-build options |
| `examples/ember_bundle.cpp` | Keyed dependency closure and target-specific artifact support |
| `examples/ember_stub_main.cpp` | Keyed runtime provider + registry construction |
| `CMakeLists.txt` | Focused test targets |

---

## 17. API sketches

### 17.1 Host setup

```cpp
KeyedDispatchRegistry keyed;
ext_keyed_dispatch::register_keyed_dispatch(keyed, options);

BuildConfiguration build;
build.keyed_dispatch = keyed.create("implicit-keyed-v1");
build.target_key_provider = &provider; // consumed transiently during build
```

### 17.2 Runtime setup

```cpp
Runtime runtime;
runtime.register_keyed_dispatch(keyed.create("implicit-keyed-v1"));
runtime.set_dispatch_key_provider(provider);

CallResult r = runtime.call_i64(module, "main", context, 42);
```

The host supplies a provider, not an expected key.

### 17.3 Optional machine-derived polymorphism

```cpp
auto pass_deriver = make_seed_deriver(
    provider, "ember/passes/v1", module_id);
if (!pass_deriver) return pass_deriver.error();

PolymorphicPassOptions pass_options;
pass_options.seed_deriver = pass_deriver.value();
ext_obf::register_passes(pass_registry, pass_options);
```

`make_seed_deriver` is the provider-to-polymorphic adapter shared by both plans.
It owns the immutable provider for the pipeline lifetime, domain-separates each
pass/function/site request, and preserves 256-bit material until the individual
pass stream is initialized.

---

## 18. Acceptance criteria

The feature is complete only when:

- source calls, exports, handles, lifecycle entries, and reload use stable
  logical identities;
- physical dispatch positions are build-specific implementation details;
- no expected machine key, fingerprint, key hash, or direct verifier is stored;
- target `K` influences physical layout and runtime `k` influences resolution
  through the same pinned strategy;
- correct-key builds execute correctly across all supported call paths;
- every wrong-key resolution is bounded, in-range, and exact-ABI/visibility
  compatible;
- singleton domains are key-dependent through compatible padding;
- malformed metadata fails before executable allocation/publication;
- no keyed call path performs raw pointer decoding or unchecked table indexing;
- no host path performs raw logical indexing of keyed dispatch storage; entry
  and name resolution go through keyed resolvers only;
- runtime key material is transient to provider locals and reserved call-tree
  state, not `context_t` or module records;
- regalloc cannot allocate the reserved key register;
- registry publication is an atomic immutable-generation swap;
- hot reload, lifecycle, threads, and coroutines do not retain stale raw entries;
- extension state (lifecycle routines, thread registry, coroutine fibers) is
  per-runtime on `ModuleInstance`, not in file-static globals, so two runtimes
  in one process carry independent dispatch records and extension stores;
- V1–V5 remain identity-layout compatible;
- target-specific V6 loading does not re-layout from local runtime `k`;
- a V6 module carrying Thin IR serializes under blob-v2, and the loader rejects
  an embedded blob-v1 IR;
- a V6 header carries a secure capability matrix declaring every required
  runtime feature, and the loader validates it against the host's supported
  capabilities before any allocation; an unsupported, unrecognized, or
  contradictory requirement is a structured load failure;
- imported cross-module stable identities remain logical references and are
  rebound locally at each call site, with no physical index stored in an import
  table, relocation, or handle;
- existing secure-default raw-x86 policy is preserved and extended to V6; the
  loader never silently falls back to raw-x86, identity layout, or a degraded
  re-emit mode—an unsatisfiable capability fails before executable allocation;
- strategy/layout extensions reuse the shared `ExtensionResult` configured
  registry foundation delivered by the polymorphic plan;
- complete focused, adversarial, property, integration, and full CTest gates
  pass;
- overhead is measured and accepted before enabling the feature by default.

---

## 19. Recommended starting point

Start with the smallest verifiable vertical slice:

1. `examples/keyed_dispatch_math_test.cpp` — write bijection/domain/padding
   tests first.
2. `src/dispatch_abi.*` — pin exact callable classes.
3. `src/keyed_dispatch.*` — implement the reference strategy/resolver.
4. `examples/module_layout_test.cpp` — prove logical/physical separation under
   two pinned target keys.
5. `src/module_layout.*` — add the configured module extension seam.
6. `src/engine.*` — install a transient route word at a safe keyed outer thunk.
7. `src/thin_emit.cpp` — convert one same-module direct call path.

Do not begin with `.em`, hot reload, or threads. First prove that build-time `K`
and runtime `k` navigate the same exact-ABI finite domain, that wrong keys stay
memory-safe, and that disabled mode remains unchanged. Then expand one call path
at a time under TDD.
