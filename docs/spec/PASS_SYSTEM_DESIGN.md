# ember — composable pass architecture design (Stage C)

**Status:** design (pre-implementation). The research basis is `docs/LLVM_PASS_SYSTEM_RESEARCH.md` (LLVM 18.1.8 pass-manager patterns). The extension-pattern basis is `src/binding_builder.hpp` + the `register_natives` / `ember_add_extension` CMake pattern.

## 0. What this is

Stage C: IR optimization passes over `ThinFunction` (the thin three-address IR from Stage A). The architecture must be **composable** — custom codegen mods, custom optimizations, and custom obfuscations plug in modularly, the way LLVM passes do, but with a discovery pattern ember developers already know (the extension `register_natives` shape).

## 1. The two halves (why a mixture)

| Concern | Source pattern | Why |
|---|---|---|
| **What a pass IS** (the interface) | LLVM | A pass is an IR→IR transform that returns what it preserved and may request analyses. The `run(IR&, AM&) → PreservedAnalyses` shape models this; a function-pointer binding (`NativeSig`) does not. |
| **Discovery & wiring** (name→pass, per-lib registration) | Extension | `register_passes(EmberPassRegistry&)` per extension lib, same shape as `register_natives(NativeTable&)`. Developers already know it; it's proven in this tree. |
| **Composition** (how the host builds a pipeline) | LLVM | `EmberPassManager::addPass(Pass{})` — the host builds the pipeline order. |
| **Pipeline string** (user-specified) | LLVM | `"constprop,cse,dce"` parsed by a tiny comma/paren parser. The registry that maps names→passes is populated the extension way. |
| **Instrumentation** (tracing/timing/gating) | LLVM | `PassInstrumentationCallbacks` — the hook for debug tracing, per-pass timing, "skip if function < N instrs". |

## 2. The pass interface (the LLVM half)

### 2.1 No virtual base class — concept-based polymorphism

A pass is *any type* with a `run(ThinFunction&, EmberAnalysisManager&)` method returning `EmberPreserved`. Type erasure happens at the storage boundary (`unique_ptr<PassConcept>`), not at a vtable base class. This is LLVM's design (Sean Parent's "Inheritance Is The Base Class of Evil").

```cpp
// src/ember_pass.hpp

#pragma once
#include "thin_ir.hpp"       // ThinFunction
#include <memory>
#include <string>
#include <vector>

namespace ember {

// ─── PreservedAnalyses (the return contract) ───
// A pass returns what it preserved, not a bool changed. The pass manager
// intersects preserved sets across passes to invalidate analyses.
struct EmberPreserved {
    bool all_ = false;       // Preserved::all() — changed nothing
    // Future: a bitset of specific analysis IDs. For now, a bool is enough
    // (ember has no analyses yet); the contract is "report what's valid."
    static EmberPreserved all()  { return {true}; }
    static EmberPreserved none() { return {false}; }
    bool all_preserved() const { return all_; }
};

// ─── CRTP mix-in for pass metadata (PassInfoMixin) ───
// Gives name() + optional isRequired(). A pass inherits it via CRTP:
//   struct MyPass : EmberPassInfoMixin<MyPass> { ... };
template <typename DerivedT>
struct EmberPassInfoMixin {
    static const char* name() {
        // __func__ or a static string in the derived type.
        return DerivedT::pass_name;
    }
    // isRequired(): must-run passes bypass skip gates (obfuscation passes
    // define this to return true). Default: false. Detected via SFINAE
    // (if the derived type doesn't define it, it's false).
    static constexpr bool is_required = false;
};

// ─── Forward decl ───
class EmberAnalysisManager;

// ─── Type-erased pass concept (the storage boundary) ───
// PassConcept is the virtual interface; PassModel<T> wraps a concrete pass.
// The pass manager holds vector<unique_ptr<PassConcept>>.
struct PassConcept {
    virtual ~PassConcept() = default;
    virtual EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am) = 0;
    virtual const char* name() const = 0;
    virtual bool is_required() const = 0;
};

template <typename PassT>
struct PassModel : PassConcept {
    PassT pass_;
    explicit PassModel(PassT p) : pass_(std::move(p)) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am) override {
        return pass_.run(f, am);
    }
    const char* name() const override { return PassT::name(); }
    bool is_required() const override { return PassT::is_required; }
};

} // namespace ember
```

### 2.2 A concrete pass (5-line struct)

```cpp
struct ConstPropPass : EmberPassInfoMixin<ConstPropPass> {
    static constexpr const char* pass_name = "constprop";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am) {
        // ... fold constants ...
        return EmberPreserved::none();  // invalidated everything
    }
};
```

No boilerplate. No vtable declaration. The CRTP mix-in provides `name()` and `is_required`; `PassModel<ConstPropPass>` provides the type erasure.

## 3. The pass manager (composition)

```cpp
// src/ember_pass.hpp (continued)

class EmberPassManager {
public:
    template <typename PassT>
    void add_pass(PassT p) {
        passes_.push_back(std::make_unique<PassModel<PassT>>(std::move(p)));
    }

    // Run all passes to a fixed point (each pass runs until it stops changing,
    // then the next pass runs; the whole pipeline runs once — no outer fixed-
    // point loop unless the host requests one via run_to_fixpoint()).
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);

    // Run the whole pipeline repeatedly until no pass changes anything (for
    // converging optimization pipelines). Returns the intersection of all
    // preserved sets.
    EmberPreserved run_to_fixpoint(ThinFunction& f, EmberAnalysisManager& am,
                                   unsigned max_rounds = 8);

    bool empty() const { return passes_.empty(); }
    size_t size() const { return passes_.size(); }

private:
    std::vector<std::unique_ptr<PassConcept>> passes_;
};
```

The `run()` loop consults `PassInstrumentationCallbacks` (§5) before/after each pass: `runBeforePass` (can skip) → `pass.run()` → `runAfterPass`.

## 4. Discovery & wiring (the extension half)

### 4.1 The registry (name → pass factory)

```cpp
// src/ember_pass_registry.hpp

#pragma once
#include "ember_pass.hpp"
#include <functional>
#include <string>
#include <unordered_map>

namespace ember {

// A pass factory: creates a unique_ptr<PassConcept> for a named pass.
// This is the "BindingBuilder::add" equivalent — one call per pass.
using PassFactory = std::function<std::unique_ptr<PassConcept>()>;

class EmberPassRegistry {
public:
    // Register a pass by name. The factory creates a fresh instance each call.
    // This is the register_passes() shape extensions already know:
    //   void register_passes(EmberPassRegistry& reg) {
    //       reg.add<ConstPropPass>("constprop");
    //       reg.add<CSEPass>("cse");
    //   }
    template <typename PassT>
    void add(const char* name) {
        factories_[name] = []() -> std::unique_ptr<PassConcept> {
            return std::make_unique<PassModel<PassT>>(PassT{});
        };
    }

    // Look up a pass by name. Returns nullptr if not found.
    std::unique_ptr<PassConcept> create(const char* name) const;

    // List all registered pass names (for --list-passes).
    std::vector<std::string> names() const;

private:
    std::unordered_map<std::string, PassFactory> factories_;
};

} // namespace ember
```

### 4.2 The per-extension `register_passes` (mirrors `register_natives`)

```cpp
// extensions/opt/ext_opt.cpp (a new extension lib, ember_ext_opt)
namespace ember::ext_opt {

void register_passes(EmberPassRegistry& reg) {
    reg.add<ConstPropPass>("constprop");
    reg.add<DeadCodeElimPass>("dce");
    reg.add<CSEPass>("cse");
    reg.add<LICMPass>("licm");
}

} // namespace ember::ext_opt
```

The host wires it the same way it wires `register_natives`:

```cpp
EmberPassRegistry pass_reg;
ext_opt::register_passes(pass_reg);
// ext_obf::register_passes(pass_reg);  // obfuscation passes (future)
```

### 4.3 The CMake pattern (mirrors `ember_add_extension`)

```cmake
# CMakeLists.txt — same shape as the existing ember_add_extension:
ember_add_extension(opt extensions/opt/ext_opt.cpp)   # → ember_ext_opt
ember_add_extension(obf  extensions/obf/ext_obf.cpp)   # → ember_ext_obf (future)
```

The consumer links whichever pass extensions it wants:

```cmake
target_link_libraries(ember_cli PRIVATE ... ember_ext_opt ...)
```

### 4.4 The pipeline string parser (tiny)

```cpp
// src/ember_pass_pipeline.hpp

// Parse "constprop,cse,dce" (or "flatten(subst,mba)" nested) into an
// EmberPassManager by looking up each name in the registry.
// Returns false + err on an unknown name.
bool build_pipeline_from_string(const std::string& spec,
                                const EmberPassRegistry& reg,
                                EmberPassManager& out,
                                std::string* err);
```

This is ~40 lines (scan for `,()`, build a flat list for now — nesting is trivial with one IR unit). The registry maps each name to a factory; the parser calls `factory()` and `add_pass`-es the result.

## 5. Instrumentation (the extensibility hook)

```cpp
// src/ember_pass.hpp (continued)

struct PassInstrumentationCallbacks {
    // beforePass: return false to SKIP the pass (gating). Required passes
    // (is_required == true) bypass this — the manager checks is_required
    // before consulting the callback.
    std::function<bool(const char* pass_name, const ThinFunction&)> before_pass;
    // afterPass: called after the pass runs (tracing, timing, validation).
    std::function<void(const char* pass_name, const ThinFunction&,
                       EmberPreserved)> after_pass;
};

class PassInstrumentation {
public:
    PassInstrumentationCallbacks* callbacks = nullptr;
    bool run_before_pass(const char* name, const ThinFunction& f) const;
    void run_after_pass(const char* name, const ThinFunction& f,
                        EmberPreserved p) const;
};
```

The host registers callbacks for:
- **Tracing**: `before_pass`/`after_pass` print the pass name + a dump of the IR.
- **Timing**: `after_pass` records the pass's duration (compile-time budgeting).
- **Gating**: `before_pass` returns false for "skip obfuscation if function < N instrs."
- **Validation**: `after_pass` asserts IR invariants in debug builds.

## 6. The analysis manager (deferred but designed for)

Ember has no analyses yet (the thin IR is Stage A; no liveness, no use-def, no CFG summary). But the interface is designed so analyses slot in without rewriting the pass manager:

```cpp
// src/ember_analysis.hpp (future — not implemented in the first Stage C cut)

class EmberAnalysisManager {
public:
    // getResult<T>(F): lazy + cached. The analysis runs on first request;
    // subsequent requests return the cached result. Invalidated by the
    // preservation set when a transform invalidates it.
    // getCachedResult<T>(F): peek (no run if not cached).
    template <typename AnalysisT>
    const typename AnalysisT::Result& getResult(ThinFunction& f);
    template <typename AnalysisT>
    const typename AnalysisT::Result* getCachedResult(ThinFunction& f) const;
    // invalidate(F, preserved): drop results that the preservation set
    // doesn't cover.
    void invalidate(ThinFunction& f, const EmberPreserved& preserved);
};
```

For the first Stage C cut, `EmberAnalysisManager` is a stub (empty class) — passes take `EmberAnalysisManager&` but don't request analyses yet. The first passes (const-prop, DCE, CSE) work by direct IR traversal (they don't need liveness or use-def). When LICM or a real regalloc needs analyses, the manager grows.

## 7. How it composes with the existing codegen

```cpp
// In compile_func (src/codegen.cpp), after lower_function produces the
// ThinFunction and BEFORE emit_x64:
if (ctx.enable_ir_backend && ctx.pass_manager && !ctx.pass_manager->empty()) {
    EmberAnalysisManager am;  // stub for now
    ctx.pass_manager->run(thf, am);
}
CompiledFn cf = emit_x64(thf, ctx);
```

The pass manager is an optional field on `CodeGenCtx` (default nullptr = no passes, same as today). Behind a flag (`enable_ir_passes`), the host sets it. The CLI exposes it via `--passes=constprop,cse,dce`.

## 8. Migration without a flag-day rewrite

- **Step 1 — SHIPPED (2026-07-11).** `src/ember_pass.hpp` + `src/ember_pass.cpp` + `src/ember_pass_registry.hpp` + `src/ember_pass_pipeline.hpp` — the infrastructure (pass manager, registry, pipeline parser, instrumentation, PreservedAnalyses). `examples/ember_pass_test.cpp` (ctest `ember_pass`) pins it with 25 checks across 7 sections.
- **Step 2 — SHIPPED (2026-07-11).** `extensions/opt/ext_opt.{hpp,cpp}` (a new `ember_ext_opt` extension lib) + `ConstPropPass` + `DeadCodeElimPass` + `CSEPass`. `examples/ir_passes_test.cpp` (ctest `ir_passes`) verifies each pass is value-preserving + instr-count-reducing.
- **Step 3 — SHIPPED (2026-07-11).** Pass manager wired into `CodeGenCtx` (src/codegen.hpp + codegen.cpp); CLI `--passes constprop,cse,dce` (examples/ember_cli.cpp); benchmark harness `EMBER_IR_PASS` env var (bench/bench_codegen_paths.cpp). End-to-end verified: code-size reductions visible (constprop_fold 406→318B, dce 382→326B, cse 418→404B).
- **Step 4 — FUTURE.** `EmberAnalysisManager` (when a pass needs it) + `LICMPass` (the first pass that needs a CFG/loop analysis).
- **Step 5 — FUTURE.** Obfuscation passes (`extensions/obf/ext_obf.cpp`) — `SubstitutionPass`, `FlatteningPass`, `MBAPass`, with `is_required = true` (bypass skip gates).

Each step is independently testable: the gate is the benchmark (does the pass reduce instr count / runtime?) + a correctness test (the pass is value-preserving — the IR path still produces the same i64 return).

## 9. What this does NOT do (YAGNI for the first cut)

- **No adaptors** (ModuleToFunctionPassAdaptor) — ember has one IR unit (`ThinFunction`). If a `ThinModule`/program unit is added later, the pass-manager-is-a-pass property (§3) enables nesting.
- **No pass dependencies** (`getAnalysis<DominatorTree>()`) — the first passes don't need analyses. The `Am&` parameter is there for when they do.
- **No pass plugins** (`.so` loading) — passes are in-tree static libs, same as extensions. The `registerPipelineParsingCallback` runtime callback form is future (if out-of-tree passes are ever needed).
- **No `PreservedAnalyses` bitset** — a bool for now. Grows into a bitset when analyses exist and invalidation matters.

## 10. Cross-references

- `docs/LLVM_PASS_SYSTEM_RESEARCH.md` §9 (the 12 design patterns this implements).
- `src/binding_builder.hpp` (the extension discovery pattern `register_passes` mirrors).
- `docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md` §4.5 (the Stage-1 pass interface this upgrades to IR→IR).
- `src/thin_ir.hpp` (the `ThinFunction` the passes operate on).
- `src/codegen.hpp` `CodeGenCtx` (where `pass_manager` plugs in).
