# LLVM Pass System Research — For Ember's Composable Pass Architecture

> **Purpose:** Research how LLVM organizes its optimization pass system modularly, in order to design a composable pass architecture for ember — a C-style JIT scripting language whose passes operate on a thin IR (`ThinFunction`).
>
> **Primary source:** LLVM 18.1.8 source tree at `E:\DEVELOPER\LLVM_18_1_8`.
> **Supplementary:** LLVM online docs (WritingAnLLVMNewPMPass, NewPassManager), the Hikari obfuscator (legacy PM), and the Pluto-Obfuscator (new PM) project.
>
> **Scope:** Research only. No ember source code is written here. The closing section extracts design patterns for ember.

---

## Table of Contents

1. [Pass Base Classes](#1-pass-base-classes)
2. [Pass Manager](#2-pass-manager)
3. [Pass Registration & Pipeline Parsing](#3-pass-registration--pipeline-parsing)
4. [Pass Kind Taxonomy](#4-pass-kind-taxonomy)
5. [Pass Instrumentation](#5-pass-instrumentation)
6. [Adaptors & Cross-Granularity Composition](#6-adaptors--cross-granularity-composition)
7. [Custom / Out-of-Tree Passes](#7-custom--out-of-tree-passes)
8. [Obfuscation-Specific: How Real Obfuscators Structure Their Passes](#8-obfuscation-specific-how-real-obfuscators-structure-their-passes)
9. [Design Patterns Extracted for Ember](#9-design-patterns-extracted-for-ember)

---

## 1. Pass Base Classes

### The key insight: there is no pass interface

The single most important design decision in LLVM's new pass manager (NPM) is stated in the opening comment of `llvm/include/llvm/IR/PassManager.h`:

> *"There is no 'pass' interface in LLVM per se. Instead, an instance of any class which supports a method to 'run' it over a unit of IR can be used as a pass."*

This is **concept-based polymorphism** (a.k.a. type erasure via templates), deliberately avoiding inheritance from a virtual base class. The design is credited to Sean Parent's "Inheritance Is The Base Class of Evil" talk. The consequences:

- A pass is *any type* with a `run(IRUnit&, AnalysisManager&, ...)` method returning `PreservedAnalyses`.
- There is no `class Pass { virtual ... }` to inherit from.
- Polymorphism is achieved at the storage boundary (a `unique_ptr<PassConcept>`) rather than at the type boundary.

### PassInfoMixin — the CRTP boilerplate provider

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~391–409)

```cpp
/// A CRTP mix-in to automatically provide informational APIs needed for passes.
template <typename DerivedT> struct PassInfoMixin {
  /// Gets the name of the pass we are mixed into.
  static StringRef name() {
    static_assert(std::is_base_of<PassInfoMixin, DerivedT>::value,
                  "Must pass the derived type as the template argument!");
    StringRef Name = getTypeName<DerivedT>();
    Name.consume_front("llvm::");
    return Name;
  }

  void printPipeline(raw_ostream &OS,
                     function_ref<StringRef(StringRef)> MapClassName2PassName) {
    StringRef ClassName = DerivedT::name();
    auto PassName = MapClassName2PassName(ClassName);
    OS << PassName;
  }
};
```

`PassInfoMixin<DerivedT>` is a CRTP (Curiously Recurring Template Parameter) mix-in. It provides two things for free:
1. **`static StringRef name()`** — derives a pass name from the type name via `getTypeName<DerivedT>()`. This is how the pass manager and instrumentation identify passes by name without a virtual `getName()`.
2. **`printPipeline()`** — renders the pass into the textual pipeline representation (e.g., for `-print-pipeline`).

> **Version note:** The LLVM 18.1.8 tree has only `PassInfoMixin`. The online docs for a *later* (trunk/23.x) LLVM mention `OptionalPassInfoMixin` and `RequiredPassInfoMixin`, which wrap `PassInfoMixin` and encode whether a pass can be skipped (via the `isRequired()` SFINAE hook). In 18.1.8, the skippable-vs-required distinction is handled entirely by the optional `static bool isRequired()` method (detected via SFINAE in `PassModel::passIsRequiredImpl`, see §2). **For ember, the 18.1.8 model is the relevant reference.**

### AnalysisInfoMixin — the analysis variant

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~412–431)

```cpp
/// A CRTP mix-in that provides informational APIs needed for analysis passes.
/// Automatically mixes in PassInfoMixin.
template <typename DerivedT>
struct AnalysisInfoMixin : PassInfoMixin<DerivedT> {
  /// Returns an opaque, unique ID for this analysis type.
  /// Requires the derived type to provide a static AnalysisKey member called Key.
  static AnalysisKey *ID() {
    static_assert(std::is_base_of<AnalysisInfoMixin, DerivedT>::value, ...);
    return &DerivedT::Key;
  }
};
```

The difference from `PassInfoMixin`: `AnalysisInfoMixin` adds an **identity** mechanism. Each analysis pass has a `static AnalysisKey Key;` member, and `ID()` returns the address of that key. Because the key is a static data member with stable address, it serves as a unique type-erased identifier — you can store `AnalysisKey*` in maps and look analyses up without knowing their concrete C++ type.

### The AnalysisKey identity primitive

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~75–90)

```cpp
/// A special type used by analysis passes to provide an address that
/// identifies that particular analysis pass type.
struct alignas(8) AnalysisKey {};

/// A special type used to provide an address that identifies a set of
/// related analyses (e.g. "all analyses that only rely on the CFG").
struct alignas(8) AnalysisSetKey {};
```

`AnalysisKey` is an empty struct. Its **address** is its identity (not its value). The `alignas(8)` guarantees the low 3 bits of the address are zero, so the pointer is safe to use in pointer-tagging schemes. This is the foundation of type-erased analysis lookup.

### How a pass's IR unit type (Module, Function, Loop) is declared

There is **no explicit declaration** of the IR unit type. It is **inferred from the `run()` method signature**. A pass is a "function pass" because it has:

```cpp
PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
```

A "module pass" has:

```cpp
PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
```

A "loop pass" has:

```cpp
PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                      LoopStandardAnalysisResults &AR, LPMUpdater &U);
```

The `run()` signature *is* the type declaration. The `addPass()` template on `PassManager<IRUnitT>` wraps the concrete pass into a `PassModel<IRUnitT, PassT, ...>` that type-erases it behind `PassConcept<IRUnitT, ...>`. Mismatches (e.g., adding a function pass to a `ModulePassManager` directly) are caught at compile time because the `run()` signatures won't line up.

### PassInfoMixin vs. legacy Pass

For contrast, the **legacy** pass manager (`llvm/include/llvm/Pass.h`) uses classical inheritance:

```cpp
// Legacy PM (paraphrased from llvm/include/llvm/Pass.h, llvm/include/llvm/FunctionPass.h)
class Pass {
  virtual void getAnalysisUsage(AnalysisUsage &) const;  // declare dependencies
  virtual StringRef getPassName() const;                  // name
  ...
};
class FunctionPass : public Pass {
  virtual bool runOnFunction(Function &F) = 0;   // returns bool (changed?)
};
```

| Aspect | Legacy `Pass` | New PM `PassInfoMixin` |
|--------|---------------|------------------------|
| Polymorphism | Virtual inheritance from `FunctionPass`/`ModulePass` | Concept-based; no base class, `run()` duck-typed |
| Name | Virtual `getPassName()` | Static `name()` via CRTP + `getTypeName` |
| Dependencies | `getAnalysisUsage()` declares what analyses are needed | `AM.getResult<AnalysisT>()` called inside `run()` (lazy, on demand) |
| Return | `bool` (did it change?) | `PreservedAnalyses` (what's still valid?) |
| Identity | `static char ID` + `INITIALIZE_PASS` macros | For analyses: `static AnalysisKey Key` + `AnalysisInfoMixin::ID()`; transforms have no runtime ID |
| Registration | `RegisterPass<X>` / `INITIALIZE_PASS_BEGIN..END` | `PassRegistry.def` macros + `PassBuilder` callbacks (see §3) |

The NPM's central philosophical shift: **invalidation is the transformation's responsibility**, encoded in the return value, rather than the analysis manager's responsibility via `getAnalysisUsage`.

---

## 2. Pass Manager

### PassManager<IRUnitT> — the core container

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~489–600)

```cpp
template <typename IRUnitT,
          typename AnalysisManagerT = AnalysisManager<IRUnitT>,
          typename... ExtraArgTs>
class PassManager : public PassInfoMixin<
    PassManager<IRUnitT, AnalysisManagerT, ExtraArgTs...>> {
public:
  explicit PassManager() = default;
  ...
  /// Run all of the passes in this manager over the given unit of IR.
  PreservedAnalyses run(IRUnitT &IR, AnalysisManagerT &AM,
                        ExtraArgTs... ExtraArgs);

  template <typename PassT>
  std::enable_if_t<!std::is_same<PassT, PassManager>::value>
  addPass(PassT &&Pass);

  bool isEmpty() const { return Passes.empty(); }
  static bool isRequired() { return true; }   // pass managers are always required

protected:
  using PassConceptT = detail::PassConcept<IRUnitT, AnalysisManagerT, ExtraArgTs...>;
  std::vector<std::unique_ptr<PassConceptT>> Passes;
};
```

Key structural points:

1. **`PassManager` is itself a pass.** It inherits from `PassInfoMixin<PassManager<...>>`, so a `FunctionPassManager` is a valid function pass and can be nested inside a `ModulePassManager` (via an adaptor). This is what makes pipelines recursive.

2. **Passes are stored as `std::vector<std::unique_ptr<PassConceptT>>`.** The concrete pass type is erased behind `PassConcept` (an abstract class with a pure virtual `run`). The `addPass()` method wraps the concrete pass in a `PassModel` (the concrete implementation of `PassConcept`). This is the Sean Parent pattern: value semantics externally, type-erased internally.

3. **`ExtraArgTs...`** lets certain pass managers thread extra arguments to every pass. The `LoopPassManager` uses this: `PassManager<Loop, LoopAnalysisManager, LoopStandardAnalysisResults &, LPMUpdater &>`, so every loop pass's `run()` receives the standard analysis results and a loop-nest updater in addition to the loop and its analysis manager.

### How run() dispatches to passes

**File:** `llvm/include/llvm/IR/PassManager.h` (the `run()` body, lines ~526–570)

```cpp
PreservedAnalyses run(IRUnitT &IR, AnalysisManagerT &AM, ExtraArgTs... ExtraArgs) {
  PreservedAnalyses PA = PreservedAnalyses::all();

  // Request PassInstrumentation from analysis manager.
  PassInstrumentation PI =
      detail::getAnalysisResult<PassInstrumentationAnalysis>(
          AM, IR, std::tuple<ExtraArgTs...>(ExtraArgs...));

  // (debug-info format conversion elided)

  for (auto &Pass : Passes) {
    // BeforePass callbacks — can skip the pass entirely.
    if (!PI.runBeforePass<IRUnitT>(*Pass, IR))
      continue;

    PreservedAnalyses PassPA = Pass->run(IR, AM, ExtraArgs...);

    // Update the analysis manager as each pass runs and potentially
    // invalidates analyses.
    AM.invalidate(IR, PassPA);

    // AfterPass callbacks.
    PI.runAfterPass<IRUnitT>(*Pass, IR, PassPA);

    // Intersect the preserved analyses to compute the aggregate preserved set.
    PA.intersect(std::move(PassPA));
  }

  // Mark all analyses on this IR unit as preserved (invalidation already done).
  PA.preserveSet<AllAnalysesOn<IRUnitT>>();
  return PA;
}
```

The dispatch loop is the heart of the system. For each pass:

1. **`runBeforePass`** — consults `PassInstrumentation` callbacks (§5). A callback returning `false` **skips the pass entirely** (this is how opt-bisect and the `optnone` attribute work). Required passes (`isRequired() == true`) bypass the skip check.
2. **`Pass->run(IR, AM, ExtraArgs...)`** — the virtual call through the type-erased `PassConcept`. This is the *only* virtual dispatch per pass run.
3. **`AM.invalidate(IR, PassPA)`** — the pass returns a `PreservedAnalyses` describing what's still valid; the analysis manager walks its cached results and drops the invalid ones.
4. **`runAfterPass`** — instrumentation callbacks fire after the pass completes.
5. **`PA.intersect(PassPA)`** — the pass manager accumulates the *intersection* of all passes' preserved sets. If pass A preserves the dominator tree and pass B doesn't, the aggregate result is "dominator tree not preserved."

### How the pass manager knows which IR unit each pass operates on

It's baked into the **template parameter** `IRUnitT`. A `PassManager<Module>` only accepts passes whose `run(Module&, ModuleAnalysisManager&)` compiles. A `PassManager<Function>` only accepts function passes. The `addPass<P>()` template instantiates `PassModel<IRUnitT, P, ...>` which has:

```cpp
PreservedAnalysesT run(IRUnitT &IR, AnalysisManagerT &AM,
                       ExtraArgTs... ExtraArgs) override {
  return Pass.run(IR, AM, ExtraArgs...);
}
```

If `Pass.run(IRUnitT&, ...)` doesn't exist or has the wrong signature, it's a compile error. The IR-unit binding is static, not dynamic.

### The type-erasure mechanics (PassConcept / PassModel)

**File:** `llvm/include/llvm/IR/PassManagerInternal.h`

```cpp
template <typename IRUnitT, typename AnalysisManagerT, typename... ExtraArgTs>
struct PassConcept {
  virtual ~PassConcept() = default;
  virtual PreservedAnalyses run(IRUnitT &IR, AnalysisManagerT &AM,
                                ExtraArgTs... ExtraArgs) = 0;
  virtual void printPipeline(...) = 0;
  virtual StringRef name() const = 0;
  virtual bool isRequired() const = 0;   // SFINAE-detected from PassT
};

template <typename IRUnitT, typename PassT, typename PreservedAnalysesT,
          typename AnalysisManagerT, typename... ExtraArgTs>
struct PassModel : PassConcept<IRUnitT, AnalysisManagerT, ExtraArgTs...> {
  explicit PassModel(PassT Pass) : Pass(std::move(Pass)) {}
  PreservedAnalysesT run(IRUnitT &IR, AnalysisManagerT &AM,
                         ExtraArgTs... ExtraArgs) override {
    return Pass.run(IR, AM, ExtraArgs...);
  }
  StringRef name() const override { return PassT::name(); }

  // SFINAE: if PassT has static isRequired(), call it; else default false.
  template <typename T> using has_required_t = decltype(std::declval<T&>().isRequired());
  bool isRequired() const override { return passIsRequiredImpl<PassT>(); }
  PassT Pass;
};
```

`PassConcept` is the abstract interface; `PassModel<IRUnitT, PassT, ...>` is the concrete wrapper holding the actual pass by value. The `isRequired()` SFINAE detection (`passIsRequiredImpl`) is notable: a pass *may* define `static bool isRequired()`, and if it does, it's exempt from skipping (opt-bisect, optnone). If it doesn't, the default is `false` (skippable). This is how `AlwaysInlinerPass` (which defines `isRequired() { return true; }`) runs even on `optnone` functions.

### Convenience typedefs

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~602–610)

```cpp
extern template class PassManager<Module>;
using ModulePassManager = PassManager<Module>;

extern template class PassManager<Function>;
using FunctionPassManager = PassManager<Function>;
```

The `extern template` declarations force the template to be instantiated in one translation unit (the corresponding `.cpp`), which is critical on Windows for stable `AnalysisKey`/`AnalysisSetKey` addresses.

---

## 3. Pass Registration & Pipeline Parsing

### PassRegistry.def — the central pass table

**File:** `llvm/lib/Passes/PassRegistry.def`

This is a **macro-based registry** — a `.def` file with no include guard, designed to be `#include`'d multiple times with different macro definitions active. Each entry is a macro invocation, not a struct/class. The macros are:

- `MODULE_ANALYSIS("name", ConstructorExpr)` — a module-level analysis
- `MODULE_PASS("name", ConstructorExpr())` — a module transformation pass
- `MODULE_PASS_WITH_PARAMS("name", "ClassName", lambda, parser, "params")` — a parameterized module pass
- `FUNCTION_ANALYSIS("name", ConstructorExpr())` — a function-level analysis
- `FUNCTION_PASS("name", ConstructorExpr())` — a function transformation pass
- `CGSCC_ANALYSIS`, `CGSCC_PASS`, `LOOP_PASS`, `LOOPNEST_PASS`, etc.

Examples from the file:

```cpp
// Analyses
MODULE_ANALYSIS("callgraph", CallGraphAnalysis())
FUNCTION_ANALYSIS("domtree", DominatorTreeAnalysis())
FUNCTION_ANALYSIS("loops", LoopAnalysis())

// Transformation passes
FUNCTION_PASS("instcombine", InstCombinePass())           // wait — see below
FUNCTION_PASS("dce", DCEPass())
FUNCTION_PASS("sroa", SROAPass())
FUNCTION_PASS("adce", ADCEPass())
FUNCTION_PASS("helloworld", HelloWorldPass())              // the tutorial pass
MODULE_PASS("globalopt", GlobalOptPass())
MODULE_PASS("always-inline", AlwaysInlinerPass())

// Parameterized passes (pass options parsed from pipeline text)
MODULE_PASS_WITH_PARAMS(
    "asan", "AddressSanitizerPass",
    [](AddressSanitizerOptions Opts) { return AddressSanitizerPass(Opts); },
    parseASanPassOptions, "kernel")
```

The pattern: **the string name ("instcombine") is bound to a constructor expression (`InstCombinePass()`)**. The constructor expression is evaluated when the pass is added to a pipeline.

### How the .def file becomes parse logic

**File:** `llvm/lib/Passes/PassBuilder.cpp` (the `parseModulePass` / `parseFunctionPass` methods)

The `.def` file is `#include`'d *inside* the parse functions, with the macros locally redefined to generate `if`-chains:

```cpp
Error PassBuilder::parseModulePass(ModulePassManager &MPM, const PipelineElement &E) {
  auto &Name = E.Name;
  auto &InnerPipeline = E.InnerPipeline;

  // First: handle nested pass managers (module(...), function(...), cgscc(...))
  if (!InnerPipeline.empty()) {
    if (Name == "module")    { ... parseModulePassPipeline(...) ... }
    if (Name == "cgscc")     { ... parseCGSCCPassPipeline -> adaptor ... }
    if (Name == "function")  { ... parseFunctionPassPipeline -> adaptor ... }
    // then: custom pipeline-parsing callbacks
    for (auto &C : ModulePipelineParsingCallbacks)
      if (C(Name, MPM, InnerPipeline)) return Error::success();
    return error;
  }

  // Then: expand the macro registry into if-chains.
#define MODULE_PASS(NAME, CREATE_PASS)                                         \
  if (Name == NAME) {                                                          \
    MPM.addPass(CREATE_PASS);                                                  \
    return Error::success();                                                   \
  }
#define MODULE_PASS_WITH_PARAMS(NAME, CLASS, CREATE_PASS, PARSER, PARAMS)      \
  if (checkParametrizedPassName(Name, NAME)) {                                 \
    auto Params = parsePassParameters(PARSER, Name, NAME);                     \
    MPM.addPass(CREATE_PASS(Params.get()));                                    \
    return Error::success();                                                   \
  }
#define MODULE_ANALYSIS(NAME, CREATE_PASS)                                     \
  if (Name == "require<" NAME ">") {                                           \
    MPM.addPass(RequireAnalysisPass<decltype(CREATE_PASS), Module>());         \
    return Error::success();                                                   \
  }                                                                            \
  if (Name == "invalidate<" NAME ">") {                                        \
    MPM.addPass(InvalidateAnalysisPass<decltype(CREATE_PASS)>>());             \
    return Error::success();                                                   \
  }
  // ... plus FUNCTION_PASS, LOOP_PASS redefined to wrap in adaptors ...
#include "PassRegistry.def"

  // Finally: plugin callbacks (for out-of-tree passes, §7)
  for (auto &C : ModulePipelineParsingCallbacks)
    if (C(Name, MPM, InnerPipeline)) return Error::success();
  return make_error<StringError>(...);
}
```

So the pipeline parser is, at its core, a long `if (Name == "x") { MPM.addPass(XPass()); }` chain, generated by re-`#include`'ing `PassRegistry.def` with the macros locally defined. The `require<analysis>` and `invalidate<analysis>` special forms are synthesized from the same analysis registry entries.

Notable: **when a function pass appears in a module-level parser**, the `FUNCTION_PASS` macro is redefined in the module parser context to wrap it in `createModuleToFunctionPassAdaptor`:

```cpp
#define FUNCTION_PASS(NAME, CREATE_PASS)                                       \
  if (Name == NAME) {                                                          \
    MPM.addPass(createModuleToFunctionPassAdaptor(CREATE_PASS));               \
    return Error::success();                                                   \
  }
```

This is how `-passes=instcombine` (no explicit `function(...)` wrapper) works — the parser auto-wraps it. The "implicit nesting" shortcut documented in NewPassManager.md.

### The pipeline text format & parsePipelineText

**File:** `llvm/lib/Passes/PassBuilder.cpp` (`parsePipelineText`, line ~1308)

The textual pipeline format is comma-separated names with parentheses for nesting:

```
module(function(instcombine,sroa),dce,cgscc(inliner,function(...)))
```

`parsePipelineText` is a small recursive-descent parser that splits on `,()` and builds a tree of `PipelineElement`:

```cpp
struct PipelineElement {
  StringRef Name;
  std::vector<PipelineElement> InnerPipeline;
};
```

The algorithm: scan for `,`, `(`, `)`. On `(`, push a new inner pipeline onto a stack; on `)`, pop. The result is a tree. `parseModulePassPipeline` then walks the tree, calling `parseModulePass` for each element, which handles nesting (`module`, `function`, `cgscc`, `loop`) by recursing into the inner pipeline and wrapping in the appropriate adaptor, and handles leaf names via the macro-generated `if`-chain above.

### Top-level parsePassPipeline & implicit manager creation

**File:** `llvm/lib/Passes/PassBuilder.cpp` (line ~1963)

```cpp
Error PassBuilder::parsePassPipeline(ModulePassManager &MPM, StringRef PipelineText) {
  auto Pipeline = parsePipelineText(PipelineText);
  if (!Pipeline) return ...;
  // If the first element is not a module pass, auto-wrap.
  // (e.g., "instcombine" -> "function(instcombine)")
  ...
  if (auto Err = parseModulePassPipeline(MPM, *Pipeline)) return Err;
  return Error::success();
}
```

The top-level entry always builds a `ModulePassManager` (the outermost IR unit is a Module). If the user writes `-passes=instcombine` (a function pass), the parser detects the mismatch and implicitly wraps it in `function(...)`, which the module parser then wraps in `createModuleToFunctionPassAdaptor`.

### The full driver setup

**File:** `llvm/tools/opt/NewPMDriver.cpp` (lines ~415–460) and `llvm/include/llvm/Passes/PassBuilder.h`

The canonical setup sequence (from the NewPassManager doc and the opt driver):

```cpp
LoopAnalysisManager LAM;
FunctionAnalysisManager FAM;
CGSCCAnalysisManager CGAM;
ModuleAnalysisManager MAM;

PassBuilder PB;                                  // (or with TargetMachine, PGO, PIC)
PB.registerModuleAnalyses(MAM);                  // populate MAM from PassRegistry.def
PB.registerCGSCCAnalyses(CGAM);
PB.registerFunctionAnalyses(FAM);
PB.registerLoopAnalyses(LAM);
PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);    // wire the analysis-manager proxies (§6)

ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
MPM.run(MyModule, MAM);
```

`registerFunctionAnalyses(FAM)` reads the `FUNCTION_ANALYSIS` entries from `PassRegistry.def` and registers each analysis with the manager via `FAM.registerPass([&] { return DominatorTreeAnalysis(); })`.

---

## 4. Pass Kind Taxonomy

LLVM distinguishes three kinds of passes, but the distinction is **structural/conventional**, not enforced by a base class:

### (a) Analysis passes

An analysis pass computes information *about* IR and caches it for other passes to query. It does not mutate the IR. Conventions:

- Inherits `AnalysisInfoMixin<DerivedT>` (which gives it `ID()`).
- Has a `using Result = SomeType;` typedef — the type of the cached result object.
- Its `run()` returns a `Result` (not `PreservedAnalyses`).
- Has a `static AnalysisKey Key;` member (the identity).
- The `Result` type optionally has an `invalidate(IR, PA, Invalidator&)` method; if present (SFINAE-detected in `ResultHasInvalidateMethod`), invalidation is delegated to the result; otherwise the default invalidation (preserved-or-not in `PA`) is used.

**Concrete example — `DominatorTreeAnalysis`:**

**File:** `llvm/include/llvm/IR/Dominators.h` (lines ~290–305)

```cpp
/// Analysis pass which computes a DominatorTree.
class DominatorTreeAnalysis : public AnalysisInfoMixin<DominatorTreeAnalysis> {
  friend AnalysisInfoMixin<DominatorTreeAnalysis>;
  static AnalysisKey Key;

public:
  /// Provide the result typedef for this analysis pass.
  using Result = DominatorTree;

  /// Run the analysis pass over a function and produce a dominator tree.
  DominatorTree run(Function &F, FunctionAnalysisManager &);
};
```

The `DominatorTree` result has a custom `invalidate`:

```cpp
class DominatorTree : public DominatorTreeBase<BasicBlock, false> {
  ...
  /// Handle invalidation explicitly.
  bool invalidate(Function &F, const PreservedAnalyses &PA,
                  FunctionAnalysisManager::Invalidator &);
};
```

The `AnalysisResultModel` (in `PassManagerInternal.h`) has two specializations: one where `ResultT` has an `invalidate` method (delegates to it), and one where it doesn't (uses the default: invalidated unless preserved or covered by a preserved set). This SFINAE split (`ResultHasInvalidateMethod`) lets simple analyses opt out of writing invalidation logic.

### (b) Transformation passes

A transformation pass mutates the IR. Conventions:

- Inherits `PassInfoMixin<DerivedT>` (gives it `name()` and `printPipeline()`).
- Its `run()` returns `PreservedAnalyses`.
- It requests analyses it needs via `AM.getResult<AnalysisT>(IR)` inside `run()`.
- It *may* define `static bool isRequired() { return true; }` to be unskippable.

**Concrete example — `InstCombinePass`:**

**File:** `llvm/include/llvm/Transforms/InstCombine/InstCombine.h`

```cpp
class InstCombinePass : public PassInfoMixin<InstCombinePass> {
private:
  InstructionWorklist Worklist;
  InstCombineOptions Options;
public:
  explicit InstCombinePass(InstCombineOptions Opts = {});
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
```

### (c) Utility passes

These are scaffolding passes, not really analyses or transformations in the usual sense. Examples in `PassManager.h`:

- **`RequireAnalysisPass<AnalysisT, IRUnitT>`** — a pass whose `run()` just calls `AM.getResult<AnalysisT>(IR)` to force an analysis to be computed, then returns `PreservedAnalyses::all()`. This is what `require<domtree>` in a pipeline expands to.
- **`InvalidateAnalysisPass<AnalysisT>`** — a pass that returns `PA` with `PA.abandon<AnalysisT>()`, forcing a specific analysis to be invalidated.
- **`InvalidateAllAnalysesPass`** — returns `PreservedAnalyses::none()` (invalidates everything).
- **`RepeatedPass<PassT>`** — wraps another pass and runs it N times.
- **Printer passes** (`DominatorTreePrinterPass`, `PrintFunctionPass`) — take a `raw_ostream&` and print the IR/analysis. These are how `-passes=print<domtree>` works.

### How transformation passes request analysis results

**File:** `llvm/include/llvm/IR/PassManager.h` (`AnalysisManager::getResult`) and concrete usage in `llvm/lib/Transforms/Scalar/ADCE.cpp`

Inside a transformation pass's `run()`:

```cpp
// From ADCE.cpp (lines ~725–744)
PreservedAnalyses ADCEPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // getCachedResult: get the domtree if already computed, but don't compute it.
  auto *DT = FAM.getCachedResult<DominatorTreeAnalysis>(F);
  // getResult: compute PostDomTree on demand if not cached.
  auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);

  ADCEChanged Changed = AggressiveDeadCodeElimination(F, DT, PDT)
                            .performDeadCodeElimination();
  if (!Changed.ChangedAnything)
    return PreservedAnalyses::all();           // changed nothing -> preserve all

  PreservedAnalyses PA;
  if (!Changed.ChangedControlFlow) {
    PA.preserveSet<CFGAnalyses>();             // didn't touch CFG -> preserve CFG analyses
    if (!Changed.ChangedNonDebugInstr) {
      PA.preserve<MemorySSAAnalysis>();        // only debug changes -> preserve MemorySSA
    }
  }
  PA.preserve<DominatorTreeAnalysis>();         // ADCE updates the domtree itself
  PA.preserve<PostDominatorTreeAnalysis>();
  return PA;
}
```

The two access methods on `AnalysisManager`:

```cpp
template <typename PassT>
typename PassT::Result &getResult(IRUnitT &IR, ExtraArgTs... ExtraArgs);      // runs if not cached

template <typename PassT>
typename PassT::Result *getCachedResult(IRUnitT &IR) const;                   // never runs; null if absent
```

`getResult` is lazy: it looks up `PassT::ID()` in the results cache (`AnalysisResults` map keyed by `{AnalysisKey*, IRUnitT*}`); on miss, it runs the analysis's `run()` and caches the result. The analysis must have been *registered* with the manager first (`registerPass`), or this asserts.

### PreservedAnalyses — the invalidation protocol

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~140–290)

`PreservedAnalyses` is a set of preserved analysis IDs (and sets). A transformation returns it to say "these analyses are still valid after my changes." Notable API:

- `PreservedAnalyses::all()` — everything preserved (pass changed nothing).
- `PreservedAnalyses::none()` — nothing preserved (invalidate everything).
- `PA.preserve<AnalysisT>()` — explicitly mark one analysis preserved.
- `PA.preserveSet<AllAnalysesOn<Function>>()` — mark all function analyses preserved.
- `PA.preserveSet<CFGAnalyses>()` — mark all CFG-only analyses preserved.
- `PA.abandon<AnalysisT>()` — explicitly invalidate one analysis even if a set covers it.
- `PA.intersect(OtherPA)` — combine two preserved sets (the result preserves only what *both* preserved). This is how the pass manager aggregates across passes.

The `getChecker<AnalysisT>()` method returns a `PreservedAnalysisChecker` that an analysis's `invalidate()` calls: `PAC.preserved()`, `PAC.preservedSet<AllAnalysesOn<IRUnit>>()`, etc., to decide whether to invalidate itself.

This is the NPM's replacement for legacy `getAnalysisUsage()`: instead of *declaring* dependencies upfront, a pass *reports* what it preserved after the fact, and the analysis manager figures out the transitive invalidation.

---

## 5. Pass Instrumentation

### The two classes

**File:** `llvm/include/llvm/IR/PassInstrumentation.h`

There are two classes:

1. **`PassInstrumentationCallbacks`** — holds the registered callbacks. This is the *registration* surface.
2. **`PassInstrumentation`** — a lightweight, copyable handle obtained *per pass-manager run* (it's the result of `PassInstrumentationAnalysis`). It invokes the callbacks.

### The callback types

```cpp
using BeforePassFunc              = bool(StringRef, Any);   // return false = skip pass
using BeforeSkippedPassFunc       = void(StringRef, Any);
using BeforeNonSkippedPassFunc    = void(StringRef, Any);
using AfterPassFunc               = void(StringRef, Any, const PreservedAnalyses &);
using AfterPassInvalidatedFunc    = void(StringRef, const PreservedAnalyses &);
using BeforeAnalysisFunc          = void(StringRef, Any);
using AfterAnalysisFunc           = void(StringRef, Any);
using AnalysisInvalidatedFunc     = void(StringRef, Any);
using AnalysesClearedFunc         = void(StringRef);
```

The `Any` wraps `const IRUnitT*` (the IR being operated on, type-erased). Each callback gets the pass name (a `StringRef`) and the IR.

### Registration API

```cpp
class PassInstrumentationCallbacks {
public:
  template <typename CallableT> void registerShouldRunOptionalPassCallback(CallableT C);
  template <typename CallableT> void registerBeforeSkippedPassCallback(CallableT C);
  template <typename CallableT> void registerBeforeNonSkippedPassCallback(CallableT C);
  template <typename CallableT> void registerAfterPassCallback(CallableT C, bool ToFront = false);
  template <typename CallableT> void registerAfterPassInvalidatedCallback(CallableT C, ...);
  template <typename CallableT> void registerBeforeAnalysisCallback(CallableT C);
  template <typename CallableT> void registerAfterAnalysisCallback(CallableT C, ...);
  template <typename CallableT> void registerAnalysisInvalidatedCallback(CallableT C);
  template <typename CallableT> void registerAnalysesClearedCallback(CallableT C);
};
```

### The instrumentation entry points (called by the pass manager)

`PassInstrumentation` provides templated entry points the pass manager calls:

```cpp
template <typename IRUnitT, typename PassT>
bool runBeforePass(const PassT &Pass, const IRUnitT &IR) const;       // false = skip

template <typename IRUnitT, typename PassT>
void runAfterPass(const PassT &Pass, const IRUnitT &IR, const PreservedAnalyses &PA) const;

template <typename IRUnitT, typename PassT>
void runBeforeAnalysis(const PassT &Analysis, const IRUnitT &IR) const;
// ... runAfterAnalysis, runAfterPassInvalidated, runAnalysisInvalidated, runAnalysesCleared
```

The `runBeforePass` logic: if the pass is *not* required (`isRequired(Pass)` is false via SFINAE), the `ShouldRunOptionalPassCallbacks` are polled — any returning `false` causes the pass to be skipped. Then either `BeforeSkippedPassCallbacks` or `BeforeNonSkippedPassCallbacks` fire depending on the outcome. This is how **opt-bisect** (run only the first N passes) and the **`optnone` attribute** (skip optimization on a function) are implemented — as instrumentation callbacks, not as special-case pass-manager logic.

### How instrumentation is delivered to pass managers

Instrumentation is itself an **analysis**:

```cpp
// File: llvm/include/llvm/IR/PassManager.h
class PassInstrumentationAnalysis
    : public AnalysisInfoMixin<PassInstrumentationAnalysis> {
  static AnalysisKey Key;
  PassInstrumentationCallbacks *Callbacks;
public:
  using Result = PassInstrumentation;
  template <typename IRUnitT, typename AnalysisManagerT, typename... ExtraArgTs>
  Result run(IRUnitT &, AnalysisManagerT &, ExtraArgTs &&...) {
    return PassInstrumentation(Callbacks);
  }
};
```

So a pass manager gets its `PassInstrumentation` by calling `AM.getResult<PassInstrumentationAnalysis>(IR)` at the top of its `run()` (visible in the dispatch loop in §2). The `Callbacks` pointer is owned externally (by `PassBuilder`), and `PassInstrumentationAnalysis` just hands out copyable handles to it.

### The built-in instrumentations

**File:** `llvm/include/llvm/Passes/StandardInstrumentations.h` and `llvm/lib/Passes/StandardInstrumentations.cpp`

LLVM ships a set of instrumentations that all register callbacks on a `PassInstrumentationCallbacks`:

- **`PrintPassInstrumentation`** — `-print-after-all` / `-print-before-all`: dumps IR before/after each pass.
- **`OptNoneInstrumentation`** — implements the `optnone` function attribute by returning `false` from a `ShouldRunOptionalPass` callback.
- **`OptPassGateInstrumentation`** — an LLVMContext-level gate to skip optional passes.
- **`PrintIRInstrumentation`** — `-dump-pass-pipeline` / IR dumping.
- Pass timing (`PassTimingInfo`), opt-bisect (`OptBisect`), etc.

Each of these is a class with a `registerCallbacks(PassInstrumentationCallbacks &PIC)` method. `PassBuilder` owns one `PassInstrumentationCallbacks` and passes it to whoever wants to register.

### Why this matters for ember

This is the **extensibility hook**. A JIT that wants to add tracing, profiling, per-pass timing, or a "skip this pass for this function" policy can do so *without modifying the pass manager*. You register a callback on the callbacks object, and every pass run consults it. This is a clean observer pattern layered on top of the pass manager.

---

## 6. Adaptors & Cross-Granularity Composition

### The problem

LLVM has four IR granularities: **Module → (CGSCC →) Function → Loop**. A `ModulePassManager` can only directly hold module passes. To run a function pass on every function in a module, you need an **adaptor** — a module pass that, in its `run()`, iterates the module's functions and runs the function pass on each.

### ModuleToFunctionPassAdaptor

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~1216–1260) and `llvm/lib/IR/PassManager.cpp` (the `run()`, line ~103)

```cpp
class ModuleToFunctionPassAdaptor
    : public PassInfoMixin<ModuleToFunctionPassAdaptor> {
public:
  using PassConceptT = detail::PassConcept<Function, FunctionAnalysisManager>;
  explicit ModuleToFunctionPassAdaptor(std::unique_ptr<PassConceptT> Pass,
                                       bool EagerlyInvalidate);
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
private:
  std::unique_ptr<PassConceptT> Pass;
  bool EagerlyInvalidate;
};

/// Deduction helper — wraps any function pass in the adaptor.
template <typename FunctionPassT>
ModuleToFunctionPassAdaptor
createModuleToFunctionPassAdaptor(FunctionPassT &&Pass, bool EagerlyInvalidate = false);
```

Its `run()` (from `PassManager.cpp`):

```cpp
PreservedAnalyses ModuleToFunctionPassAdaptor::run(Module &M, ModuleAnalysisManager &AM) {
  // Get the FunctionAnalysisManager via the proxy analysis.
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  PassInstrumentation PI = AM.getResult<PassInstrumentationAnalysis>(M);

  PreservedAnalyses PA = PreservedAnalyses::all();
  for (Function &F : M) {
    if (F.isDeclaration()) continue;
    if (!PI.runBeforePass<Function>(*Pass, F)) continue;
    PreservedAnalyses PassPA = Pass->run(F, FAM);
    FAM.invalidate(F, EagerlyInvalidate ? PreservedAnalyses::none() : PassPA);
    PI.runAfterPass(*Pass, F, PassPA);
    PA.intersect(std::move(PassPA));
  }
  PA.preserveSet<AllAnalysesOn<Function>>();
  PA.preserve<FunctionAnalysisManagerModuleProxy>();
  return PA;
}
```

The adaptor does several critical things:
1. **Fetches the inner analysis manager via a proxy analysis** (`FunctionAnalysisManagerModuleProxy`) — this is how the function-level analysis manager is made available inside a module-level pass (see the proxies below).
2. **Iterates the IR sub-units** (every function in the module).
3. **Skips declarations** (functions without bodies).
4. **Runs instrumentation** before/after each function pass run.
5. **Invalidates the inner manager** per-function after each pass — *this is key*: function analyses are invalidated at the function level immediately, not held until the module pass completes. The `EagerlyInvalidate` flag forces full invalidation to reduce peak memory.
6. **Aggregates** preserved analyses, and preserves the proxy + all function analyses (since invalidation was handled inline).

### FunctionToLoopPassAdaptor

**File:** `llvm/include/llvm/Transforms/Scalar/LoopPassManager.h`

Similar pattern, but more complex because loops form a nest and can be created/deleted during processing:

```cpp
class FunctionToLoopPassAdaptor : public PassInfoMixin<FunctionToLoopPassAdaptor> {
public:
  using PassConceptT = detail::PassConcept<Loop, LoopAnalysisManager,
                                           LoopStandardAnalysisResults &, LPMUpdater &>;
  explicit FunctionToLoopPassAdaptor(std::unique_ptr<PassConceptT> Pass,
                                     bool UseMemorySSA = false, ...,
                                     bool LoopNestMode = false)
      : Pass(std::move(Pass)), ... {
    // The adaptor pre-canonicalizes every loop before running the user's passes:
    LoopCanonicalizationFPM.addPass(LoopSimplifyPass());
    LoopCanonicalizationFPM.addPass(LCSSAPass());
  }
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
private:
  std::unique_ptr<PassConceptT> Pass;
  FunctionPassManager LoopCanonicalizationFPM;
  ...
};
```

The loop adaptor maintains a **worklist** of loops (innermost-first, postorder) and an `LPMUpdater` that loop passes use to report "I deleted this loop" or "I added new child/sibling loops" — the worklist is updated accordingly and the pipeline revisits affected loops. This is the most complex adaptor because the set of IR sub-units is *mutable during the run*.

### The analysis-manager proxies

**File:** `llvm/include/llvm/IR/PassManager.h` (lines ~680–870)

To let a function pass access module-level analyses (and vice versa), LLVM uses **proxy analyses**:

- **`InnerAnalysisManagerProxy<FunctionAnalysisManager, Module>`** (aliased `FunctionAnalysisManagerModuleProxy`) — a *module* analysis whose `Result` gives you the `FunctionAnalysisManager`. This is how `ModuleToFunctionPassAdaptor` gets the FAM. The result is an RAII object: when it's destroyed, it `clear()`s the inner AM (the inner analyses' validity is tied to the proxy result's lifetime).
- **`OuterAnalysisManagerProxy<ModuleAnalysisManager, Function>`** (aliased `ModuleAnalysisManagerFunctionProxy`) — a *function* analysis whose `Result` exposes the `ModuleAnalysisManager` *read-only* (`getCachedResult` only — you can't trigger a module analysis from within a function pass, to keep results deterministic). This is how a function pass reads, e.g., `GlobalsAA`.

`PassBuilder::crossRegisterProxies(LAM, FAM, CGAM, MAM)` registers all these proxies on all the managers so they can find each other.

### The composition ladder

From `llvm/docs/NewPassManager.html` — the full set of adaptors and the nesting they enable:

```cpp
// loop -> function
FPM.addPass(createFunctionToLoopPassAdaptor(LoopFooPass()));
// function -> module
MPM.addPass(createModuleToFunctionPassAdaptor(FunctionFooPass()));
// loop -> function -> module
MPM.addPass(createModuleToFunctionPassAdaptor(createFunctionToLoopPassAdaptor(LoopFooPass())));
// function -> cgscc -> module
MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(createCGSCCToFunctionPassAdaptor(FunctionFooPass())));
// loop -> function -> cgscc -> module
MPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(
    createCGSCCToFunctionPassAdaptor(createFunctionToLoopPassAdaptor(LoopFooPass()))));
```

And because `PassManager<IRUnitT>` is itself a pass of that IR unit, you can group:

```cpp
FunctionPassManager FPM;
FPM.addPass(InstSimplifyPass());
MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));   // run the whole FPM on each function
```

**This is the core composability mechanism.** An adaptor is just a pass at the outer granularity whose `run()` iterates the inner granularity. The same `addPass` / `run` / instrumentation / invalidation machinery applies uniformly.

---

## 7. Custom / Out-of-Tree Passes

### Minimum boilerplate for a custom function pass (new PM)

Distilled from `llvm/include/llvm/Transforms/Utils/HelloWorld.h` + `.cpp` and the LLVM tutorial:

**Header (`HelloWorld.h`):**
```cpp
#ifndef LLVM_TRANSFORMS_UTILS_HELLOWORLD_H
#define LLVM_TRANSFORMS_UTILS_HELLOWORLD_H
#include "llvm/IR/PassManager.h"
namespace llvm {
class HelloWorldPass : public PassInfoMixin<HelloWorldPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // namespace llvm
#endif
```

**Source (`HelloWorld.cpp`):**
```cpp
#include "llvm/Transforms/Utils/HelloWorld.h"
using namespace llvm;
PreservedAnalyses HelloWorldPass::run(Function &F, FunctionAnalysisManager &AM) {
  errs() << F.getName() << "\n";
  return PreservedAnalyses::all();   // changed nothing -> preserve all analyses
}
```

That's the *entire* pass. No registration macro, no `INITIALIZE_PASS`, no `char ID`. The `run()` signature alone makes it a function pass.

**In-tree registration** (so `-passes=helloworld` works): add one line to `llvm/lib/Passes/PassRegistry.def` in the `FUNCTION_PASS` section, plus an `#include` of the header in `PassBuilder.cpp`:

```cpp
// PassRegistry.def
FUNCTION_PASS("helloworld", HelloWorldPass())
```

That's it. The macro-expansion in `parseFunctionPass` generates the `if (Name == "helloworld") { FPM.addPass(HelloWorldPass()); }` branch.

### Out-of-tree registration via the plugin API

**Files:** `llvm/include/llvm/Passes/PassPlugin.h`, `llvm/examples/Bye/Bye.cpp`, `llvm/lib/Passes/PassPlugin.cpp`

For a pass that lives in a separate shared library (loaded with `-load-pass-plugin=...`), LLVM defines a C-ABI plugin entry point:

```cpp
// llvm/include/llvm/Passes/PassPlugin.h
#define LLVM_PLUGIN_API_VERSION 1
extern "C" {
struct PassPluginLibraryInfo {
  uint32_t APIVersion;
  const char *PluginName;
  const char *PluginVersion;
  void (*RegisterPassBuilderCallbacks)(PassBuilder &);
};
}
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo();
```

**The canonical example — `llvm/examples/Bye/Bye.cpp`:**

```cpp
struct Bye : PassInfoMixin<Bye> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    if (!runBye(F)) return PreservedAnalyses::all();
    return PreservedAnalyses::none();
  }
};

llvm::PassPluginLibraryInfo getByePluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Bye", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // (1) Register into a default pipeline extension point:
            PB.registerVectorizerStartEPCallback(
                [](llvm::FunctionPassManager &PM, OptimizationLevel Level) {
                  PM.addPass(Bye());
                });
            // (2) Register a pipeline-parsing callback so -passes=goodbye works:
            PB.registerPipelineParsingCallback(
                [](StringRef Name, llvm::FunctionPassManager &PM,
                   ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "goodbye") { PM.addPass(Bye()); return true; }
                  return false;
                });
          }};
}

#ifndef LLVM_BYE_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() { return getByePluginInfo(); }
#endif
```

**The two registration mechanisms** a plugin uses:

1. **`registerPipelineParsingCallback`** — lets `-passes=my-pass-name` resolve to `addPass(MyPass())`. The callback receives the name, the target pass manager (typed by IR unit — there are overloads for Module/Function/CGSCC/Loop/MachineFunction), and the inner pipeline elements. It returns `true` if it handled the name. This is how out-of-tree passes get their own pipeline names.

2. **`register*EPCallback`** (extension-point callbacks) — injects the pass into a *default* pipeline at a named point (e.g., `VectorizerStartEP`, `OptimizerLastEP`, `PipelineStartEP`). This is how a plugin can add a pass to every `-O2` run without the user writing an explicit `-passes` string. There are ~12 EPs covering module/function/loop/cgscc and LTO phases (see the full list in `PassBuilder.h`).

Additionally, plugins can use:
- **`registerAnalysisRegistrationCallback`** — register custom analyses with an analysis manager.
- **`registerParseTopLevelPipelineCallback`** — handle the entire top-level pipeline.

### How plugins get loaded

**File:** `llvm/lib/Passes/PassPlugin.cpp`

```cpp
Expected<PassPlugin> PassPlugin::Load(const std::string &Filename) {
  auto Library = sys::DynamicLibrary::getPermanentLibrary(Filename.c_str(), &Error);
  ...
  intptr_t getDetailsFn = (intptr_t)Library.getAddressOfSymbol("llvmGetPassPluginInfo");
  ...
  P.Info = reinterpret_cast<decltype(llvmGetPassPluginInfo)*>(getDetailsFn)();
  if (P.Info.APIVersion != LLVM_PLUGIN_API_VERSION) return error;
  if (!P.Info.RegisterPassBuilderCallbacks) return error;
  return P;
}
```

The loader `dlopen`s the library, looks up `llvmGetPassPluginInfo`, checks the API version, and stores the info struct. The driver then calls `Plugin.registerPassBuilderCallbacks(PB)` (visible in `llvm/tools/opt/NewPMDriver.cpp` line ~429: `for (auto &PassPlugin : PassPlugins) PassPlugin.registerPassBuilderCallbacks(PB);`).

For **statically-linked** plugins (built with `LLVM_${NAME}_LINK_INTO_TOOLS=ON`), the mechanism is the `Extension.def` / `HANDLE_EXTENSION` macro:

```cpp
// in NewPMDriver.cpp
#define HANDLE_EXTENSION(Ext) get##Ext##PluginInfo().RegisterPassBuilderCallbacks(PB);
#include "llvm/Support/Extension.def"
```

### Minimum boilerplate for a custom analysis pass (new PM)

```cpp
// Header
class MyAnalysis : public AnalysisInfoMixin<MyAnalysis> {
  friend AnalysisInfoMixin<MyAnalysis>;
  static AnalysisKey Key;          // identity
public:
  using Result = MyAnalysisResult; // the cached result type
  Result run(Function &F, FunctionAnalysisManager &AM);
};
// Source
AnalysisKey MyAnalysis::Key;       // define the static key (one TU, stable address)
MyAnalysisResult MyAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  // compute and return the result
  return MyAnalysisResult(...);
}
```

Register it as `FUNCTION_ANALYSIS("my-analysis", MyAnalysis())` in `PassRegistry.def`, or via `registerAnalysisRegistrationCallback` in a plugin. Then any function pass can call `AM.getResult<MyAnalysis>(F)`.

---

## 8. Obfuscation-Specific: How Real Obfuscators Structure Their Passes

I examined two real obfuscation projects to see how obfuscation passes are structured in practice. They represent the two eras: **Hikari** (legacy pass manager, LLVM 8-era) and **Pluto-Obfuscator** (new pass manager, LLVM 12/14).

### Hikari — legacy PM, custom "scheduler" ModulePass

**Source:** cloned from `github.com/HikariObfuscator/Hikari` (developer branch).

**Directory layout:**
```
llvm/include/llvm/Transforms/Obfuscation/
    AntiClassDump.h, AntiDebugging.h, AntiHook.h,
    BogusControlFlow.h, Flattening.h, Substitution.h,
    FunctionCallObfuscate.h, FunctionWrapper.h, IndirectBranch.h,
    StringEncryption.h, Split.h, Utils.h, CryptoUtils.h, Obfuscation.h
llvm/lib/Transforms/Obfuscation/
    (matching .cpp files) + Obfuscation.cpp (the scheduler)
```

**Each obfuscation pass is a legacy `FunctionPass`** (or `ModulePass` for module-wide ones). The headers declare factory functions, not classes:

```cpp
// BogusControlFlow.h (paraphrased)
namespace llvm {
  FunctionPass *createBogusControlFlowPass();
  FunctionPass *createBogusControlFlowPass(bool flag);
  void initializeBogusControlFlowPass(PassRegistry &Registry);
}
```

The implementation is a `struct : FunctionPass` with `INITIALIZE_PASS`:

```cpp
// BogusControlFlow.cpp
struct BogusControlFlow : public FunctionPass {
  static char ID;
  bool flag;
  BogusControlFlow() : FunctionPass(ID) { this->flag = true; }
  bool runOnFunction(Function &F) override { ... }
  virtual void addBogusFlow(BasicBlock *basicBlock, Function &F) { ... }
};
char BogusControlFlow::ID = 0;
INITIALIZE_PASS(BogusControlFlow, "bcfobf", "Enable BogusControlFlow.", true, true)
FunctionPass *llvm::createBogusControlFlowPass() { return new BogusControlFlow(); }
```

**The composition mechanism — a hand-written "scheduler":**

Hikari does *not* compose passes via the pass manager's pipeline. Instead, `Obfuscation.cpp` defines a single `ModulePass` called `Obfuscation` that **manually invokes** each obfuscation pass in a fixed, hand-tuned order:

```cpp
// Obfuscation.cpp (the "Hikari's own Pass Scheduler")
struct Obfuscation : public ModulePass {
  bool runOnModule(Module &M) override {
    // Module-level passes first
    if (EnableAllObfuscation || EnableAntiClassDump) {
      ModulePass *P = createAntiClassDumpPass(); P->doInitialization(M); P->runOnModule(M); delete P;
    }
    // ... FunctionCallObfuscate (iterate functions), AntiHook, AntiDebugging, StringEncryption ...

    // Then per-function passes, in a specific order:
    for (Function &F : M) {
      if (F.isDeclaration()) continue;
      FunctionPass *P;
      P = createSplitBasicBlockPass(...);    P->runOnFunction(F); delete P;  // 1. split
      P = createBogusControlFlowPass(...);   P->runOnFunction(F); delete P;  // 2. bogus CFG
      P = createFlatteningPass(...);         P->runOnFunction(F); delete P;  // 3. flatten
      P = createSubstitutionPass(...);       P->runOnFunction(F); delete P;  // 4. subst
    }
    // Post-run: indirect branching, function wrapper, cleanup of hikari_ helpers
    ...
    return true;
  }
};
```

The order is deliberate and commented: **split → bogus control flow → flattening → substitution** is the canonical obfuscation ordering (split increases block count, bogus-CFG adds opaque predicates, flattening rewrites the CFG into a dispatch loop, substitution rewrites arithmetic). The scheduler also notes *why* it's manual: *"Because currently there is no way to add dependency to transform passes."*

**The classic IR transforms:**

- **Instruction Substitution** (`Substitution.cpp`): replaces `add a, b` with an equivalent sequence. Has a table of variant functions selected randomly:
  ```cpp
  funcAdd[0] = &Substitution::addNeg;        // a + b  ==  a - (-b)
  funcAdd[1] = &Substitution::addDoubleNeg;  // a + b  ==  -(-a - b)
  funcAdd[2] = &Substitution::addRand;
  funcAdd[3] = &Substitution::addRand2;
  ```
  Concrete `addNeg`: `op = BinaryOperator::CreateNeg(bo->getOperand(1)); BinaryOperator::Create(Instruction::Sub, bo->getOperand(0), op)`. It walks every `BinaryOperator` and dispatches on opcode (`Add`, `Sub`, `And`, `Or`, `Xor`), choosing a random equivalent form.

- **Bogus Control Flow** (`BogusControlFlow.cpp`): for each basic block, with probability `boguscf-prob`, inserts an opaque-always-true predicate (`y < 10 || x*(x+1) % 2 == 0` using two global values) that branches to either the original block or a cloned-and-junked "altered" block, then loops back. The altered block is a copy of the original with junk instructions inserted. Runs `boguscf-loop` times.

- **Control Flow Flattening** (`Flattening.cpp`): lowers switches, saves all original basic blocks, creates a `dispatchBB` with a `switch` on an `alloca`'d "switch variable", assigns each original block a random case value, and rewrites every block's terminator to update the switch variable and branch back to `dispatchBB`. The result is a single dispatch loop — the original CFG structure is destroyed.

**Hikari's attribute-driven targeting:** each pass checks `toObfuscate(flag, F, "fla")` — a utility that reads an annotation/attribute on the function (e.g., `__attribute__((annotate("fla")))`) to decide whether to obfuscate that particular function. This is per-function opt-in.

### Pluto-Obfuscator — new PM, the modern pattern

**Source:** cloned from `github.com/bluesadi/Pluto-Obfuscator` (dev branch), LLVM 12.0.1 + 14.0.6 variants.

**This is the new-pass-manager pattern** — and the most directly relevant to ember.

**Each pass is a `PassInfoMixin` struct in a `Pluto` namespace:**

```cpp
// llvm/include/llvm/Transforms/Obfuscation/Flattening.h
#pragma once
#include "llvm/Passes/PassBuilder.h"
using namespace llvm;
namespace Pluto {
struct Flattening : PassInfoMixin<Flattening> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
    static bool isRequired() { return true; }   // obfuscation must always run
};
} // namespace Pluto
```

```cpp
// llvm/include/llvm/Transforms/Obfuscation/MbaObfuscation.h
namespace Pluto {
struct MbaObfuscation : PassInfoMixin<MbaObfuscation> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
    static bool isRequired() { return true; }
    void substituteConstant(Instruction *I, int i);
    void substitute(BinaryOperator *BI);
    Value *substituteAdd(BinaryOperator *BI);
    Value *substituteSub(BinaryOperator *BI);
    Value *substituteAnd(BinaryOperator *BI);
    Value *substituteOr(BinaryOperator *BI);
    Value *substituteXor(BinaryOperator *BI);
};
} // namespace Pluto
```

The `run()` is the transform. Pluto's `Flattening::run` does the same dispatch-loop transform as Hikari's but returns `PreservedAnalyses::none()` (it changed everything) and uses `IRBuilder`/modern IR construction:

```cpp
PreservedAnalyses Pluto::Flattening::run(Function &F, FunctionAnalysisManager &AM) {
    if (F.size() <= 1) return PreservedAnalyses::none();   // nothing to flatten
    std::vector<BasicBlock *> origBB;
    for (BasicBlock &BB : F) origBB.push_back(&BB);
    origBB.erase(origBB.begin());                          // skip entry
    BasicBlock &entryBB = F.getEntryBlock();
    // split entry if it ends in a conditional branch
    if (auto *br = dyn_cast<BranchInst>(entryBB.getTerminator()))
        if (br->isConditional()) { ... splitBasicBlock ... }
    // create dispatch + return blocks, alloca a switch var, build a SwitchInst,
    // assign random case values, rewrite each block to update the var and branch back
    BasicBlock *dispatchBB = BasicBlock::Create(context, "dispatchBB", &F, &entryBB);
    AllocaInst *swVarPtr = new AllocaInst(Type::getInt32Ty(context), 0, "swVar.ptr", ...);
    SwitchInst *swInst = SwitchInst::Create(swVar, swDefault, 0, dispatchBB);
    for (BasicBlock *BB : origBB) { swInst->addCase(..., BB); ... }
    ...
}
```

**Composition mechanism — a custom `buildObfuscationPipeline` in PassBuilder.cpp:**

Pluto modifies `llvm/lib/Passes/PassBuilder.cpp` to add a function that reads a `-passes=fla,mba,...` list and builds the pipeline with proper adaptors:

```cpp
// PassBuilder.cpp (Pluto's modification)
static cl::list<std::string> Passes("passes", cl::CommaSeparated, cl::Hidden, ...);

ModulePassManager buildObfuscationPipeline() {
    ModulePassManager MPM;
    FunctionPassManager FPM;
    for (auto pass : Passes) {
        if (pass == "fla") { FPM.addPass(LowerSwitchWrapper()); FPM.addPass(Pluto::Flattening()); }
        if (pass == "icl") { MPM.addPass(Pluto::IndirectCalls()); }                       // module-level
        if (pass == "ibr") { FPM.addPass(Pluto::IndirectBranches()); }
        if (pass == "mba") { FPM.addPass(Pluto::MbaObfuscation()); }
        if (pass == "hlw") { FPM.addPass(Pluto::HelloWorld()); }
        if (pass == "str") { MPM.addPass(Pluto::StringEncryption()); }                    // module-level
    }
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));   // function passes -> module
    return MPM;
}
```

This is the new-PM composition pattern in action:
- **Function-level obfuscations** (`fla`, `ibr`, `mba`) are added to a `FunctionPassManager`.
- **Module-level obfuscations** (`icl`, `str`) go straight on the `ModulePassManager`.
- The FPM is wrapped in `createModuleToFunctionPassAdaptor` and added to the MPM — exactly the composition ladder from §6.
- `LowerSwitchWrapper` (a `LowerSwitchPass` subclass with `isRequired()=true`) runs *before* `Flattening` because flattening requires lowered switches — this is the kind of ordering dependency Hikari handled manually; Pluto handles it by just putting the prerequisite pass first in the same FPM.

**Build integration:** Pluto adds the obfuscation passes as an LLVM component library:
```cmake
# llvm/lib/Transforms/Obfuscation/CMakeLists.txt
add_llvm_component_library(LLVMObfuscation
  CryptoUtils.cpp Utils.cpp HelloWorld.cpp Flattening.cpp
  MbaUtils.cpp MbaObfuscation.cpp SymbolObfuscation.cpp
  StringEncryption.cpp IndirectCalls.cpp IndirectBranches.cpp
  LINK_COMPONENTS Core Support Analysis TransformUtils
  LINK_LIBS z3 )
```

**Pluto's pass set (from the pipeline builder):**
| Short name | Pass | Granularity | Transform |
|------------|------|-------------|-----------|
| `fla` | `Pluto::Flattening` | Function | Control-flow flattening (dispatch switch) |
| `ibr` | `Pluto::IndirectBranches` | Function | Rewrite direct branches to indirect |
| `icl` | `Pluto::IndirectCalls` | Module | Rewrite direct calls to indirect |
| `mba` | `Pluto::MbaObfuscation` | Function | Mixed Boolean-Arithmetic substitution of add/sub/and/or/xor |
| `str` | `Pluto::StringEncryption` | Module | Encrypt string literals + decrypt at use sites |
| `hlw` | `Pluto::HelloWorld` | Function | Tutorial no-op |

**Notable design observations comparing the two:**

1. **Hikari (legacy PM)** composes via a *hand-written scheduler* that calls `runOnFunction` directly, because the legacy PM doesn't give it a clean way to express "run these function passes in this order, with this module-level prelude." Ordering dependencies are encoded in the source order of the scheduler.

2. **Pluto (new PM)** composes via the *normal pass-manager machinery* — `addPass` to an FPM, wrap in `createModuleToFunctionPassAdaptor`, add to MPM. Ordering is the `addPass` order. Prerequisite passes (like `LowerSwitch` before `Flattening`) are just earlier `addPass` calls in the same FPM. This is strictly cleaner and is the pattern ember should follow.

3. **Both use `isRequired() { return true; }`** for obfuscation passes — obfuscation must run even on `optnone` functions (you don't want someone disabling obfuscation with an attribute). In the new PM this means the pass is exempt from `PassInstrumentation`'s skip callbacks.

4. **Both are per-function opt-in via annotations** (Hikari's `toObfuscate(flag, F, "fla")` reads an `annotate` attribute; Pluto's passes check a flag). This is orthogonal to the pass system — it's a filter inside `run()`.

5. **Neither uses analysis invalidation meaningfully.** Obfuscation passes return `PreservedAnalyses::none()` (or `all()` if they bailed out early), because they aggressively mutate the CFG/IR. There's no obfuscation pass that returns a fine-grained `preserve<DomTree>()` — the transforms are too destructive. This simplifies the design: obfuscation passes are "fire and forget" w.r.t. analyses.

---

## 9. Design Patterns Extracted for Ember

This section distills the LLVM patterns into design guidance for ember's composable pass architecture (passes operating on `ThinFunction`, supporting custom codegen mods, optimizations, and obfuscations modularly). **No ember code is written here — these are architectural recommendations.**

### Pattern 1: No pass interface — use concept-based polymorphism

Don't define a `class EmberPass { virtual void run(...) = 0; }`. Instead, a pass is *any type* with a `run(ThinFunction&, EmberAnalysisManager&)` method returning a preservation set. Erase the concrete type at the storage boundary (`unique_ptr<PassConcept<ThinFunction>>` wrapping a `PassModel<ThinFunction, ConcretePass>`).

**Why:** This avoids a vtable-per-pass-class, gives value semantics for `addPass`, and lets passes be simple structs with no boilerplate. It's the single biggest readability win of LLVM's NPM.

**Cost:** One virtual call per pass run (the `PassConcept::run` dispatch). For a JIT running a handful of passes per function, this is negligible.

### Pattern 2: CRTP mix-in for pass metadata (`PassInfoMixin`)

Provide an `EmberPassInfoMixin<Derived>` that gives `static const char* name()` (or use `__func__`/a macro) and any other metadata. Passes inherit it via CRTP:

```
struct MyPass : EmberPassInfoMixin<MyPass> {
    EmberPreserved run(ThinFunction&, EmberAM&);
};
```

If ember has a single IR unit (`ThinFunction`), there's only one pass-manager template instantiation and the IR-unit-type parameter can be dropped or fixed. If ember later adds a module/program unit, the template parameterization is ready.

### Pattern 3: `PreservedAnalyses` as the return contract

A pass returns what it preserved, not a `bool changed`. Even if ember's initial analysis set is tiny, encoding the return as a preservation set future-proofs the design:
- `Preserved::all()` — changed nothing.
- `Preserved::none()` — invalidated everything (the obfuscation default).
- `Preserved::set<X>()` — preserved a specific analysis.

The pass manager intersects preserved sets across passes. This is what lets an optimization pass that doesn't touch the CFG say "I preserved the CFG-dependent analyses" and avoid recomputing them.

**For a thin IR with few analyses, this can start simple** (a bool or an enum) and grow into the full set-based model as ember adds analyses. The key is that the *contract* is "report what's valid," not "report whether you changed."

### Pattern 4: Analysis passes via `AnalysisKey` + `AnalysisInfoMixin`

If ember has analyses (e.g., a liveness map, a use-def chain, a CFG summary for obfuscation), model them as the LLVM pattern:
- An `AnalysisKey` (an empty struct whose *address* is the identity).
- An `AnalysisInfoMixin<Derived>` providing `ID()`.
- A `using Result = ...` typedef.
- A `run(ThinFunction&, EmberAM&)` returning the result.
- The analysis manager caches results keyed by `{AnalysisKey*, ThinFunction*}` and invalidates based on the preservation set.

Transforms request analyses via `AM.getResult<MyAnalysis>(F)`. This is lazy and cached. **This is the mechanism that lets obfuscation passes request a CFG summary without recomputing it each time, and lets optimization passes share a liveness analysis.**

### Pattern 5: Pass manager is itself a pass

Make `EmberPassManager` inherit from `EmberPassInfoMixin<EmberPassManager>` so it's a valid pass. This enables nesting and is essential if ember ever has multiple IR granularities (program → function). Even with one granularity, it lets you build a named sub-pipeline and treat it as a unit.

### Pattern 6: Adaptors for cross-granularity composition

If ember only has `ThinFunction`, this is deferred. But the *pattern* to remember: an adaptor is an outer-granularity pass whose `run()` iterates the inner granularity and runs an inner pass manager on each, fetching the inner analysis manager via a proxy. If ember adds a `ThinModule`/program unit holding multiple `ThinFunction`s, `ModuleToFunctionPassAdaptor` is the exact model to copy.

### Pattern 7: Macro-registry OR callback-registry for name → pass binding

For ember to support a string-specified pipeline (`-passes=flatten,subst,mba`), bind names to constructors. Two options from LLVM:

- **Macro `.def` file** (`PASS("name", Constructor)`): simplest, compile-time, in-tree only. Good if ember's passes are all in-tree.
- **`registerPipelineParsingCallback`**: runtime, plugin-friendly. A callback gets `(name, passManager, innerPipeline)` and returns `true` if it handled the name. This is how out-of-tree passes (e.g., a user-supplied obfuscation plugin) register their names.

**Recommendation for ember:** Start with a simple registry (a map from `string` to a `function<EmberPass()>` factory, or a switch statement), and expose a `registerPass(name, factory)` API that mirrors `registerPipelineParsingCallback`. This lets extensions register custom passes at runtime without modifying core. The callback form (not just a map) is preferable because it lets a single callback handle parameterized pass names (like LLVM's `asan<kernel>`).

### Pattern 8: Pipeline text parser (comma + parens nesting)

Ember's pipeline string can reuse LLVM's tiny `parsePipelineText` design: scan for `,()`, build a tree of `{Name, InnerPipeline}` elements, then walk it with per-level parse functions. This gives users `flatten(subst,mba)`-style nesting for free. For a single-IR-unit system the nesting is trivial, but the parser is ~40 lines and future-proof.

### Pattern 9: PassInstrumentation as the extensibility hook

Borrow the `PassInstrumentationCallbacks` + `PassInstrumentation` split:
- A callbacks object holds registered `beforePass`/`afterPass`/`beforeAnalysis`/`afterAnalysis` callbacks.
- The pass manager obtains a `PassInstrumentation` handle and calls `runBeforePass` (which can return false to skip) / `runAfterPass` around each pass.

This lets ember add, *without modifying the pass manager*:
- **Tracing/logging** (print each pass name + the IR before/after).
- **Per-pass timing** (a JIT-relevant feature for compile-time budgets).
- **Pass gating** (skip a pass for a given function based on a policy — e.g., "don't obfuscate functions under N instructions").
- **Validation** (assert IR invariants after each pass in debug builds).

**This is especially valuable for a JIT**, where you may want to skip expensive obfuscation passes when compiling in a "fast" mode, or time-budget the pipeline. The instrumentation callback is the single hook for all of this.

### Pattern 10: `isRequired()` for must-run passes

Borrow the SFINAE-detected `static bool isRequired()` convention. Obfuscation passes that must always run (even if a gating callback says "skip") define `isRequired() { return true; }`. The pass manager's `runBeforePass` consults this: required passes bypass the skip check. This is how Pluto marks all its obfuscation passes — and it's the right semantics for ember's obfuscation passes too (you don't want a JIT budget gate accidentally disabling security passes).

### Pattern 11: Obfuscation pass structure (from Pluto)

For ember's obfuscation passes specifically, follow Pluto's new-PM pattern:
- Each obfuscation is a `struct : EmberPassInfoMixin<X>` with `run(ThinFunction&, EmberAM&)` and `isRequired() { return true; }`.
- The `run()` does an early bail (`return Preserved::all()`) if the function doesn't qualify (too small, marked no-obfuscate, etc.).
- Otherwise it performs the transform and returns `Preserved::none()` (obfuscation is destructive to analyses).
- **Ordering dependencies are expressed by `addPass` order in the pipeline builder**, not by a hand-written scheduler. If flattening requires lowered switches (or ember's equivalent), put the lowering pass first in the same pass manager.
- Per-function opt-in (an attribute/flag check inside `run()`) is orthogonal to the pass system — it's a filter, not a pass-manager concern.

### Pattern 12: Compose function + module passes via adaptors (if ember grows)

If ember grows a program/module unit, the composition is:

```
EmberProgramPassManager PPM;
EmberFunctionPassManager FPM;
FPM.addPass(SubstitutionPass());
FPM.addPass(FlatteningPass());
PPM.addPass(createProgramToFunctionPassAdaptor(std::move(FPM)));
PPM.addPass(StringEncryptionPass());   // program-level
```

This is exactly Pluto's `buildObfuscationPipeline`. The adaptor iterates functions; program-level passes run once. **This is the pattern that makes "custom codegen mods, optimizations, and obfuscations" composable**: they're all just passes at some granularity, glued by adaptors.

### Summary: the minimal composable architecture for ember

Distilled to its essence, the architecture ember should adopt:

1. **One pass concept**: type-erased `run(ThinFunction&, EmberAM&) -> Preserved`. No pass base class.
2. **One CRTP mix-in**: `EmberPassInfoMixin<Derived>` for `name()` and optional `isRequired()`.
3. **One pass manager**: `EmberPassManager` holding `vector<unique_ptr<PassConcept>>`, with a `run()` loop that does before-pass → run → invalidate → after-pass, intersecting preserved sets.
4. **One analysis manager** (when needed): caches `AnalysisKey* → Result` per `ThinFunction*`, with `getResult<T>()` (lazy) and `getCachedResult<T>()` (peek). Invalidates via the preservation set.
5. **One callbacks object**: `PassInstrumentationCallbacks` with before/after pass/analysis hooks. The pass manager consults it; extensions register on it.
6. **One registry**: name → pass factory, populated in-tree and via a `registerPipelineParsingCallback`-style API for extensions.
7. **A tiny pipeline parser**: comma/paren nesting → tree → walk.
8. **Adaptors deferred** until ember has >1 IR granularity, but designed for (the pass-manager-is-a-pass property is already there).

This gives ember: passes that are 5-line structs; a pipeline string like `codegen,instcombine,flatten,subst` that just works; custom/extension passes registered at runtime; tracing/timing/gating without touching the manager; and a clean growth path to analyses and multi-granularity composition — all directly modeled on the LLVM 18.1.8 source tree.

---

## Appendix: Key File Index in the LLVM 18.1.8 Tree

| Topic | File |
|-------|------|
| Pass manager core | `llvm/include/llvm/IR/PassManager.h` |
| Type-erasure internals | `llvm/include/llvm/IR/PassManagerInternal.h` |
| PassManager::run dispatch | `llvm/include/llvm/IR/PassManager.h` (the `run()` method body) |
| ModuleToFunctionPassAdaptor::run | `llvm/lib/IR/PassManager.cpp` (line ~103) |
| PassInfoMixin / AnalysisInfoMixin / AnalysisKey | `llvm/include/llvm/IR/PassManager.h` (lines ~75–431) |
| PreservedAnalyses | `llvm/include/llvm/IR/PassManager.h` (lines ~140–290) |
| PassInstrumentation + Callbacks | `llvm/include/llvm/IR/PassInstrumentation.h` |
| Standard instrumentations | `llvm/include/llvm/Passes/StandardInstrumentations.h` |
| PassBuilder (pipeline parsing API) | `llvm/include/llvm/Passes/PassBuilder.h` |
| PassBuilder (parsing impl, macro expansion) | `llvm/lib/Passes/PassBuilder.cpp` (`parseModulePass` ~1365, `parseFunctionPass` ~1672, `parsePipelineText` ~1308) |
| Pass registry (.def) | `llvm/lib/Passes/PassRegistry.def` |
| Pass plugin API | `llvm/include/llvm/Passes/PassPlugin.h` |
| Pass plugin loader | `llvm/lib/Passes/PassPlugin.cpp` |
| Plugin example (Bye) | `llvm/examples/Bye/Bye.cpp`, `CMakeLists.txt` |
| HelloWorld pass (tutorial) | `llvm/include/llvm/Transforms/Utils/HelloWorld.h`, `llvm/lib/Transforms/Utils/HelloWorld.cpp` |
| Concrete transform pass (InstCombine) | `llvm/include/llvm/Transforms/InstCombine/InstCombine.h` |
| Concrete analysis pass (DominatorTree) | `llvm/include/llvm/IR/Dominators.h` (`DominatorTreeAnalysis` ~line 290) |
| Analysis usage in a transform (ADCE) | `llvm/lib/Transforms/Scalar/ADCE.cpp` (line ~725) |
| Loop pass manager + FunctionToLoopPassAdaptor | `llvm/include/llvm/Transforms/Scalar/LoopPassManager.h` |
| opt driver (plugin loading, EP registration) | `llvm/tools/opt/NewPMDriver.cpp` (line ~415) |
| Default pipelines (O2 etc.) | `llvm/lib/Passes/PassBuilderPipelines.cpp` |

### External sources consulted

- LLVM docs: `llvm.org/docs/WritingAnLLVMNewPMPass.html` (Writing an LLVM Pass, new PM), `llvm.org/docs/NewPassManager.html` (Using the New Pass Manager).
- Hikari obfuscator: `github.com/HikariObfuscator/Hikari` (developer branch) — legacy PM obfuscation, custom scheduler, BogusControlFlow/Flattening/Substitution implementations.
- Pluto-Obfuscator: `github.com/bluesadi/Pluto-Obfuscator` (dev branch, LLVM 12.0.1) — new PM obfuscation, `PassInfoMixin` passes, `buildObfuscationPipeline` composition, Flattening/MbaObfuscation/IndirectBranches/IndirectCalls/StringEncryption implementations.
