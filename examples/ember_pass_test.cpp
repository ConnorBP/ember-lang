// ember_pass_test.cpp — Stage C: the composable pass system infrastructure test.
//
// Pins the pass system infrastructure (no real passes yet — just the manager,
// registry, pipeline parser, instrumentation, and PreservedAnalyses):
// (1) registry: add<PassT>("name") + create("name") + has() + names()
// (2) pass manager: add_pass + run + the pass actually ran
// (3) PreservedAnalyses: all()/none()/intersect()
// (4) instrumentation: before_pass (skip) + after_pass (tracing)
// (5) is_required: required passes bypass the skip gate
// (6) pipeline string parser: "a,b,c" → manager with 3 passes
// (7) run_to_fixpoint: converges when no pass changes anything
//
// Links the core `ember` lib (ember_pass.cpp lives in ember_frontend, but the
// test links both). Modeled on thin_ir_struct_test (the hand-built struct pin).

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/ember_pass_pipeline.hpp"
#include "../src/thin_ir.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ember;

static int failures = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) failures++;
}

// ─── Test passes ───

// A no-op pass that preserves everything (changes nothing).
struct NoOpPass : EmberPassInfoMixin<NoOpPass> {
    static constexpr const char* pass_name = "noop";
    int* ran_count_ = nullptr;
    explicit NoOpPass(int* ran = nullptr) : ran_count_(ran) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        if (ran_count_) ++*ran_count_;
        return EmberPreserved::all();
    }
};

// A pass that "changes" the IR (returns Preserved::none) and increments a counter.
struct CountingPass : EmberPassInfoMixin<CountingPass> {
    static constexpr const char* pass_name = "counting";
    int* ran_count_ = nullptr;
    explicit CountingPass(int* ran = nullptr) : ran_count_(ran) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        if (ran_count_) ++*ran_count_;
        return EmberPreserved::none();
    }
};

// A required pass (bypasses skip gates).
struct RequiredPass : EmberPassInfoMixin<RequiredPass> {
    static constexpr const char* pass_name = "required";
    static constexpr bool is_required = true;
    int* ran_count_ = nullptr;
    explicit RequiredPass(int* ran = nullptr) : ran_count_(ran) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        if (ran_count_) ++*ran_count_;
        return EmberPreserved::none();
    }
};

// A pass that changes for N-1 rounds then converges (for run_to_fixpoint test).
struct ConvergingPass : EmberPassInfoMixin<ConvergingPass> {
    static constexpr const char* pass_name = "converging";
    int* round_;
    explicit ConvergingPass(int* r) : round_(r) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        ++*round_;
        if (*round_ < 3) return EmberPreserved::none();  // changed
        return EmberPreserved::all();  // converged
    }
};

int main() {
    std::printf("=== ember_pass_test: Stage C pass system infrastructure ===\n");

    // Build a minimal ThinFunction (the passes don't touch it, but run() needs one).
    ThinFunction thf;
    thf.name = "test";
    thf.slot = 0;
    ThinBlock blk;
    blk.id = 0;
    blk.term.kind = TermKind::Return;
    thf.blocks.push_back(blk);
    EmberAnalysisManager am;

    // (1) Registry.
    std::printf("(1) Registry\n");
    {
        EmberPassRegistry reg;
        reg.add<NoOpPass>("noop");
        reg.add<CountingPass>("counting");
        check(reg.has("noop"), "has(\"noop\")");
        check(!reg.has("nonexistent"), "!has(\"nonexistent\")");
        auto p = reg.create("noop");
        check(p != nullptr, "create(\"noop\") != nullptr");
        check(reg.create("nonexistent") == nullptr, "create(\"nonexistent\") == nullptr");
        auto names = reg.names();
        check(names.size() == 2, "names().size() == 2");
    }

    // (2) Pass manager: add_pass + run + the pass ran.
    std::printf("(2) Pass manager\n");
    {
        int ran = 0;
        EmberPassManager pm;
        pm.add_pass(CountingPass(&ran));
        check(pm.size() == 1, "size() == 1 after add_pass");
        EmberPreserved p = pm.run(thf, am);
        check(ran == 1, "pass ran exactly once");
        check(!p.all_preserved(), "Preserved::none() returned (not all_preserved)");
    }

    // (3) PreservedAnalyses.
    std::printf("(3) PreservedAnalyses\n");
    {
        EmberPreserved a = EmberPreserved::all();
        EmberPreserved n = EmberPreserved::none();
        check(a.all_preserved(), "all().all_preserved()");
        check(!n.all_preserved(), "!none().all_preserved()");
        a.intersect(n);
        check(!a.all_preserved(), "all().intersect(none()) = not all_preserved");
        EmberPreserved a2 = EmberPreserved::all();
        a2.intersect(EmberPreserved::all());
        check(a2.all_preserved(), "all().intersect(all()) = all_preserved");
    }

    // (4) Instrumentation: before_pass (skip) + after_pass (tracing).
    std::printf("(4) Instrumentation\n");
    {
        int ran = 0;
        int after_count = 0;
        EmberPassManager pm;
        pm.add_pass(CountingPass(&ran));
        PassInstrumentationCallbacks cb;
        cb.before_pass = [](const char*, const ThinFunction&) { return false; };  // skip all
        cb.after_pass = [&](const char*, const ThinFunction&, EmberPreserved) { ++after_count; };
        pm.instrumentation.callbacks = &cb;
        pm.run(thf, am);
        check(ran == 0, "before_pass=skip -> pass did not run");
        check(after_count == 0, "after_pass not called (pass was skipped)");
    }
    {
        int ran = 0;
        int after_count = 0;
        EmberPassManager pm;
        pm.add_pass(CountingPass(&ran));
        PassInstrumentationCallbacks cb;
        cb.before_pass = [](const char*, const ThinFunction&) { return true; };  // allow all
        cb.after_pass = [&](const char*, const ThinFunction&, EmberPreserved) { ++after_count; };
        pm.instrumentation.callbacks = &cb;
        pm.run(thf, am);
        check(ran == 1, "before_pass=allow -> pass ran");
        check(after_count == 1, "after_pass called once");
    }

    // (5) is_required: required passes bypass the skip gate.
    std::printf("(5) is_required\n");
    {
        int ran = 0;
        EmberPassManager pm;
        pm.add_pass(RequiredPass(&ran));
        PassInstrumentationCallbacks cb;
        cb.before_pass = [](const char*, const ThinFunction&) { return false; };  // skip all
        pm.instrumentation.callbacks = &cb;
        pm.run(thf, am);
        check(ran == 1, "required pass ran despite before_pass=skip");
    }

    // (6) Pipeline string parser.
    std::printf("(6) Pipeline string parser\n");
    {
        EmberPassRegistry reg;
        reg.add<NoOpPass>("noop");
        reg.add<CountingPass>("counting");
        EmberPassManager pm;
        std::string err;
        bool ok = build_pipeline_from_string("noop,counting", reg, pm, &err);
        check(ok, "parse \"noop,counting\" succeeded");
        check(pm.size() == 2, "pipeline has 2 passes");
        // Empty spec = no passes.
        EmberPassManager pm2;
        check(build_pipeline_from_string("", reg, pm2, &err), "empty spec succeeds");
        check(pm2.empty(), "empty spec = no passes");
        // Unknown name = error.
        EmberPassManager pm3;
        bool ok2 = build_pipeline_from_string("noop,nonexistent", reg, pm3, &err);
        check(!ok2, "unknown pass name -> parse fails");
        check(err.find("unknown pass") != std::string::npos, "error mentions 'unknown pass'");
        // Whitespace trimming.
        EmberPassManager pm4;
        bool ok3 = build_pipeline_from_string(" noop , counting ", reg, pm4, &err);
        check(ok3, "parse with whitespace succeeds");
        check(pm4.size() == 2, "whitespace-trimmed pipeline has 2 passes");
    }

    // (7) run_to_fixpoint: converges when no pass changes anything.
    std::printf("(7) run_to_fixpoint\n");
    {
        // NoOpPass returns all() (no change) -> converges after 1 round.
        int ran = 0;
        EmberPassManager pm;
        pm.add_pass(NoOpPass(&ran));
        pm.run_to_fixpoint(thf, am, 8);
        check(ran == 1, "NoOpPass converges after 1 round (ran once)");
    }
    {
        // A pass that changes for 2 rounds then stops. ConvergingPass (defined
        // above) runs 3 times: first 2 return none() (changed), 3rd returns
        // all() (converged).
        int round = 0;
        EmberPassManager pm;
        pm.add_pass(ConvergingPass(&round));
        pm.run_to_fixpoint(thf, am, 8);
        check(round == 3, "ConvergingPass ran 3 rounds (2 changed + 1 converged)");
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
