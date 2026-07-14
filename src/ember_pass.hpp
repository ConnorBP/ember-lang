// ember_pass.hpp — Stage C: the composable pass system infrastructure.
//
// The pass interface (the LLVM half of the mixture design, see
// docs/spec/PASS_SYSTEM_DESIGN.md): concept-based polymorphism (no virtual
// base class), CRTP mix-in for metadata, PreservedAnalyses return contract,
// type-erased PassModel<T> at the storage boundary. The pass manager holds
// vector<unique_ptr<PassConcept>> and runs them in order with instrumentation
// callbacks.
//
// A pass is ANY type with:
//   EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
//   static constexpr const char* pass_name;
// It inherits EmberPassInfoMixin<Itself> for name() + is_required().
//
// Design refs:
//   docs/spec/PASS_SYSTEM_DESIGN.md §2-3, §5 (the pass interface + manager)
//   docs/LLVM_PASS_SYSTEM_RESEARCH.md §9 Patterns 1-5, 9-10 (the LLVM patterns)
//   src/binding_builder.hpp (the extension discovery half — see ember_pass_registry.hpp)

#pragma once

#include "thin_ir.hpp"       // ThinFunction (the IR passes operate on)

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ember {

// ─── PreservedAnalyses (the return contract) ───
// A pass returns what it preserved, not a bool changed. The pass manager
// intersects preserved sets across passes to invalidate analyses. For now this
// is a bool (ember has no analyses yet); the contract is "report what's valid."
// When analyses exist, this grows into a bitset of analysis IDs.
struct EmberPreserved {
    bool all_ = false;
    static EmberPreserved all()  { return {true}; }   // changed nothing
    static EmberPreserved none() { return {false}; }  // invalidated everything
    bool all_preserved() const { return all_; }
    // Intersect: the result preserves only what BOTH inputs preserve.
    void intersect(const EmberPreserved& other) {
        all_ = all_ && other.all_;
    }
};

// ─── Forward decl ───
class EmberAnalysisManager;

// ─── CRTP mix-in for pass metadata (EmberPassInfoMixin) ───
// Gives name() + is_required(). A pass inherits it via CRTP:
//   struct MyPass : EmberPassInfoMixin<MyPass> {
//       static constexpr const char* pass_name = "mypass";
//       EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am) { ... }
//   };
template <typename DerivedT>
struct EmberPassInfoMixin {
    static const char* name() { return DerivedT::pass_name; }
    // is_required(): must-run passes bypass skip gates (obfuscation passes
    // define is_required = true). Default: false. The derived type may
    // specialize: `static constexpr bool is_required = true;`.
    static constexpr bool is_required = false;
};

// ─── Type-erased pass concept (the storage boundary) ───
// PassConcept is the virtual interface; PassModel<T> wraps a concrete pass.
// The pass manager holds vector<unique_ptr<PassConcept>>. This is Sean Parent's
// "concept-based polymorphism" — no vtable-per-pass-class, value semantics for
// addPass, passes are simple structs with no boilerplate.
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

// ─── Pass instrumentation (the extensibility hook) ───
// The host registers callbacks for tracing, timing, gating, and validation.
// before_pass: return false to SKIP the pass (gating). Required passes
// (is_required == true) bypass this — the manager checks is_required first.
// after_pass: called after the pass runs (tracing, timing, validation).
struct PassInstrumentationCallbacks {
    std::function<bool(const char* pass_name, const ThinFunction&)> before_pass;
    std::function<void(const char* pass_name, const ThinFunction&, EmberPreserved)> after_pass;
};

class PassInstrumentation {
public:
    PassInstrumentationCallbacks* callbacks = nullptr;
    // Returns true if the pass should run (false = skip). Required passes
    // always run (the caller checks is_required before calling this).
    bool run_before_pass(const char* name, const ThinFunction& f) const {
        if (!callbacks || !callbacks->before_pass) return true;
        return callbacks->before_pass(name, f);
    }
    void run_after_pass(const char* name, const ThinFunction& f, EmberPreserved p) const {
        if (!callbacks || !callbacks->after_pass) return;
        callbacks->after_pass(name, f, p);
    }
};

// ─── The pass manager (composition) ───
// Holds vector<unique_ptr<PassConcept>>. addPass<T>(T{}) type-erases into a
// PassModel<T>. run() executes passes in order with instrumentation.
class EmberPassManager {
public:
    // Hard failsafes for fixed-point pipelines. Callers may request fewer
    // rounds, but cannot raise these process-protection ceilings.
    static constexpr unsigned hard_max_fixpoint_rounds = 32;
    static constexpr std::size_t hard_max_ir_instructions = 100000;
    static constexpr std::size_t hard_max_ir_growth_factor = 10;

    template <typename PassT>
    void add_pass(PassT p) {
        passes_.push_back(std::make_unique<PassModel<PassT>>(std::move(p)));
    }

    // Add an already-constructed type-erased pass (e.g. one created by the
    // registry). This is the path the pipeline string parser uses.
    void add_pass_concept(std::unique_ptr<PassConcept> p) {
        passes_.push_back(std::move(p));
    }

    // Atomically append a fully-resolved sequence of passes. Used by the
    // transactional pipeline parser: every name/factory is resolved into
    // temporary ownership first, and only on complete success is the whole
    // sequence moved into the manager. The caller's PassInstrumentation is
    // never touched (neither moved, cleared, nor replaced).
    void append_passes(std::vector<std::unique_ptr<PassConcept>> ps) {
        passes_.reserve(passes_.size() + ps.size());
        for (auto& p : ps) passes_.push_back(std::move(p));
    }

    // Atomically replace the pass sequence with a fully-resolved one. Used by
    // the transactional replace-pipeline parser: on complete success the old
    // passes are swapped out for the new sequence. The caller's
    // PassInstrumentation is never touched (neither moved, cleared, nor
    // replaced) — only the pass vector is exchanged.
    void replace_passes(std::vector<std::unique_ptr<PassConcept>> ps) {
        passes_ = std::move(ps);
    }

    // Run all passes once, in order. Returns the intersection of all preserved
    // sets. Consults instrumentation before/after each pass; a before_pass
    // callback returning false skips the pass (unless is_required).
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);

    // Run the whole pipeline repeatedly until no pass changes anything or
    // max_rounds is reached. max_rounds is clamped to 32, and the pipeline
    // stops if IR exceeds 100,000 instructions or 10x its initial size.
    // Returns the intersection of all preserved sets. A round is "no change"
    // if every pass returns Preserved::all().
    EmberPreserved run_to_fixpoint(ThinFunction& f, EmberAnalysisManager& am,
                                   unsigned max_rounds = 8);

    bool empty() const { return passes_.empty(); }
    size_t size() const { return passes_.size(); }

    // Instrumentation (set by the host; nullptr = no instrumentation).
    PassInstrumentation instrumentation;

private:
    std::vector<std::unique_ptr<PassConcept>> passes_;
};

// ─── Analysis manager (stub for the first Stage C cut) ───
// Ember has no analyses yet. Passes take EmberAnalysisManager& but don't
// request analyses yet (the first passes — const-prop, DCE, CSE — work by
// direct IR traversal). When LICM or a real regalloc needs analyses, this
// grows into the cached getResult<T>/invalidate pattern (see PASS_SYSTEM_DESIGN
// §6). The stub is here so the pass interface is stable from day one.
class EmberAnalysisManager {
public:
    // No-op for now. The API will be:
    //   template <typename AnalysisT>
    //   const typename AnalysisT::Result& getResult(ThinFunction& f);
    //   template <typename AnalysisT>
    //   const typename AnalysisT::Result* getCachedResult(ThinFunction& f) const;
    //   void invalidate(ThinFunction& f, const EmberPreserved& preserved);
};

} // namespace ember
