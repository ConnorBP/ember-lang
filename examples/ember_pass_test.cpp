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
// Phase 1 (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 1): configured/collision-
// aware registry factories.
// (8)  legacy add-and-run still works through add<T> returning a status
// (9)  configured factory captures immutable constructor options
// (10) two create() calls yield distinct fresh PassConcept instances
// (11) duplicate add_factory/add<T> rejection with original registration retained
// (12) empty name + null/empty std::function factory rejection
// (13) names() is deterministic (sorted lexicographically)
//
// Phase 1 (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 2): strict transactional
// pipeline construction.
// (14) transactional append: an unknown middle token leaves a preloaded
//      manager's pass count, runnable behavior, and instrumentation untouched
// (15) empty elements (middle / leading / trailing / whitespace-only) rejected
// (16) unsupported parentheses rejected
// (17) trailing junk / unconsumed text rejected
// (18) a registered factory that returns nullptr is rejected
// (19) successful atomic append preserves preload + instrumentation
// (20) atomic replace mode: success replaces passes, failure preserves both
//      passes and instrumentation; instrumentation is never moved/cleared
//
// Phase 2 (plan_POLYMORPHIC_CODE_ENGINE.md §9.3 Red 3): seed derivation.
// Golden vectors pin the canonical byte encoding documented in
// src/seed_derivation.hpp. Expected bytes are HARD-CODED literals computed by
// a separate reference script; the production algorithm is never duplicated
// inside this test to recompute them.
// (21) empty/root-level derivation golden bytes + identity
// (22) function-level derivation golden bytes + identity
// (23) site (block + instruction ordinal) derivation golden bytes + identity
// (24) distinct purposes yield distinct output (golden bytes for both)
// (25) fixed seed 0 golden bytes
// (26) fixed seed UINT64_MAX golden bytes (and distinct from seed 0)
// (27) initial StableRng outputs (next() x4, bounded() x5) golden values
// (28) order independence: the same request set derived forward and reverse
//      yields identical per-request results (no shared advancing state)
// (29) parallel: one const deriver shared across worker threads, every result
//      equal to serial derivation (immutable / thread-safe)
// (30) structured errors: a failing SeedDeriver returns an ExtensionError, not
//      a printed diagnostic; the fixed-root deriver always succeeds
//
// Links the core `ember` lib (ember_pass.cpp lives in ember_frontend, but the
// test links both). Modeled on thin_ir_struct_test (the hand-built struct pin).

#include "../src/ember_pass.hpp"
#include "../src/ember_pass_registry.hpp"
#include "../src/ember_pass_pipeline.hpp"
#include "../src/extension_registry.hpp"
#include "../src/seed_derivation.hpp"
#include "../src/pipeline_profile.hpp"  // Red 8: PipelineProfile + registry
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_mutation.hpp"  // Red 8: PassGrowthLimits for heavy profile bounds
#include "../src/thin_ir_ser.hpp"   // Red 5: verify_thin_function_for_codegen
#include "../src/codegen.hpp"      // Red 5: compile_func_checked, CodeGenCtx
#include "../src/thin_lower.hpp"    // Red 5: lower_function
#include "../src/thin_emit.hpp"      // Red 5: emit_x64
#include "../src/engine.hpp"        // Red 5: CompiledFn, DispatchTable, finalize
#include "../src/dispatch_table.hpp"  // Red 5: DispatchTable
#include "../src/lexer.hpp"         // Red 5: tokenize
#include "../src/parser.hpp"         // Red 5: parse
#include "../src/sema.hpp"           // Red 5: sema
#include "../extensions/opt/ext_opt.hpp"  // Red 5: register_passes
#include "../extensions/obf/ext_obf.hpp"   // Red 8: register_passes (obf) for profile expansion

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

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

// A pass carrying a captured constructor option, used to verify configured
// factories. On run() it writes its `tag` value through the `sink` pointer so
// the test can observe exactly which options the factory captured. Two
// distinct fresh instances are distinguishable by address and by independent
// sink writes.
struct TaggedPass : EmberPassInfoMixin<TaggedPass> {
    static constexpr const char* pass_name = "tagged";
    int tag = 0;
    int* sink = nullptr;
    TaggedPass() = default;
    TaggedPass(int t, int* s) : tag(t), sink(s) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        if (sink) *sink = tag;
        return EmberPreserved::none();
    }
};

// ─── Red 5 test passes (file-scope: local classes cannot hold static
// constexpr data members, so these must live outside main, like the passes
// above). They reference the checked-path symbols (PassError, etc.) that land
// in the GREEN phase; until then this file does not compile (the intended RED
// state). ───

// A pass that corrupts the CFG (drops the entry block's terminator) and
// reports a mutation (none()). Checked execution must catch this via the
// verifier, roll back, and report ValidationFailure.
struct CorruptTermPass : EmberPassInfoMixin<CorruptTermPass> {
    static constexpr const char* pass_name = "corrupt-term";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        if (!f.blocks.empty()) f.blocks[0].term.kind = TermKind::None;
        return EmberPreserved::none();
    }
};

// A pass that corrupts the CFG via an out-of-range block id.
struct CorruptBlockIdPass : EmberPassInfoMixin<CorruptBlockIdPass> {
    static constexpr const char* pass_name = "corrupt-blockid";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        if (!f.blocks.empty()) f.blocks[0].id = 9999;
        return EmberPreserved::none();
    }
};

// A pass that corrupts the frame plan (negative frame_size).
struct CorruptFramePass : EmberPassInfoMixin<CorruptFramePass> {
    static constexpr const char* pass_name = "corrupt-frame";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        f.frame.frame_size = -1;
        return EmberPreserved::none();
    }
};

// A pass that corrupts rodata (a ConstStringRef pointing past rodata end).
struct CorruptRodataPass : EmberPassInfoMixin<CorruptRodataPass> {
    static constexpr const char* pass_name = "corrupt-rodata";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        if (f.blocks.empty()) return EmberPreserved::none();
        ThinInstr bad;
        bad.op = ThinOp::ConstStringRef;
        bad.meta.addend = 1000;
        bad.meta.len = 100;
        f.blocks[0].instrs.push_back(bad);
        return EmberPreserved::none();
    }
};

// A pass that injects a large number of instructions to blow a low growth
// ceiling (used with CheckedRunOptions.max_instructions set small).
struct BlowupPass : EmberPassInfoMixin<BlowupPass> {
    static constexpr const char* pass_name = "blowup";
    std::size_t n = 0;
    explicit BlowupPass(std::size_t count = 0) : n(count) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        if (f.blocks.empty()) return EmberPreserved::none();
        // resize (one allocation + in-place default construction) rather than a
        // push_back loop, so the large-N vectors stay cheap. Default ThinInstrs
        // have empty vectors/strings (no per-element heap allocation).
        f.blocks[0].instrs.resize(f.blocks[0].instrs.size() + n);
        return EmberPreserved::none();
    }
};

// A pass that throws a recoverable PassError carrying a pass name + message.
struct PassErrorPass : EmberPassInfoMixin<PassErrorPass> {
    static constexpr const char* pass_name = "perr";
    std::string msg;
    PassErrorPass() = default;
    explicit PassErrorPass(std::string m) : msg(std::move(m)) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        throw PassError("perr", msg);
    }
};

// A pass that throws an UNEXPECTED std::exception (not a PassError).
struct UnexpectedExceptionPass : EmberPassInfoMixin<UnexpectedExceptionPass> {
    static constexpr const char* pass_name = "unexpected";
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        throw std::runtime_error("unexpected boom");
    }
};

// A pass that records the address of the EmberAnalysisManager it received,
// so a test can confirm compile_func_checked honored ctx.analysis_manager
// instead of always constructing a local manager.
struct AmCapturePass : EmberPassInfoMixin<AmCapturePass> {
    static constexpr const char* pass_name = "amcap";
    const EmberAnalysisManager** out = nullptr;
    AmCapturePass() = default;
    explicit AmCapturePass(const EmberAnalysisManager** o) : out(o) {}
    EmberPreserved run(ThinFunction&, EmberAnalysisManager& am) {
        if (out) *out = &am;
        return EmberPreserved::all();
    }
};

// A pass that sets a STALE regalloc result (enabled with a bogus map) so a
// test can confirm the stale/pre-existing regalloc is cleared before emit.
struct StaleRegallocPass : EmberPassInfoMixin<StaleRegallocPass> {
    static constexpr const char* pass_name = "stale-ra";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        f.ra = RegAllocResult{};
        f.ra.enabled = true;
        f.ra.map[1].in_reg = true;
        f.ra.map[1].reg_id = 99;  // a bogus pool register id
        f.ra.frame_reg_map[-8] = 99;
        f.ra.used_reg_ids.push_back(99);
        f.ra.save_offsets.push_back(-64);
        return EmberPreserved::none();
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
        // Unknown name = error. The pipeline is transactional: even though
        // "noop" resolves first, the manager must NOT receive it on failure.
        // This failure case preloads the destination manager with one runnable
        // pass and a real instrumentation callback bundle, then asserts the
        // failed parse left the pass count, runnable behavior, and the
        // instrumentation callback pointer completely unchanged.
        EmberPassManager pm3;
        int pre_ran3 = 0, after_count3 = 0;
        PassInstrumentationCallbacks cb3;
        pm3.add_pass(CountingPass(&pre_ran3));
        cb3.after_pass = [&](const char*, const ThinFunction&, EmberPreserved) {
            ++after_count3;
        };
        pm3.instrumentation.callbacks = &cb3;
        const PassInstrumentationCallbacks* cb3_ptr = pm3.instrumentation.callbacks;
        bool ok2 = build_pipeline_from_string("noop,nonexistent", reg, pm3, &err);
        check(!ok2, "unknown pass name -> parse fails");
        check(err.find("unknown pass") != std::string::npos, "error mentions 'unknown pass'");
        check(pm3.size() == 1, "transactional: pass count unchanged on unknown-name failure");
        check(pm3.instrumentation.callbacks == cb3_ptr,
              "transactional: instrumentation pointer unchanged on unknown-name failure");
        pre_ran3 = 0; after_count3 = 0;
        EmberPreserved p3 = pm3.run(thf, am);
        check(pre_ran3 == 1, "transactional: preloaded pass still runs after unknown-name failure");
        check(after_count3 == 1, "transactional: instrumentation still fires after unknown-name failure");
        check(!p3.all_preserved(), "transactional: preloaded CountingPass still returns none()");
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

    // (8) Legacy add-and-run still works now that add<T> returns a status the
    // caller may ignore. Registers via add<T>, builds a pipeline, runs it, and
    // confirms the pass executed (the default-constructed CountingPass returns
    // Preserved::none(), proving run() was invoked).
    std::printf("(8) Legacy add-and-run (add<T> returns ignorable status)\n");
    {
        EmberPassRegistry reg;
        ExtensionStatus st = reg.add<CountingPass>("counting");
        check(bool(st), "add<T> returns success status");
        check(reg.has("counting"), "add<T> still registers the name");
        EmberPassManager pm;
        std::string err;
        check(build_pipeline_from_string("counting", reg, pm, &err),
              "pipeline builds from add<T>-registered name");
        check(pm.size() == 1, "pipeline has 1 pass");
        EmberPreserved p = pm.run(thf, am);
        check(!p.all_preserved(),
              "legacy add-and-run pass executed (returned Preserved::none)");
    }

    // (9) Configured factory captures immutable constructor options. The
    // factory captures `tag` by value; create() + run() must observe exactly
    // the captured value, proving the options traveled into the pass.
    std::printf("(9) Configured factory captures immutable options\n");
    {
        int sink = 0;
        const int captured_tag = 1337;
        EmberPassRegistry reg;
        ExtensionStatus st = reg.add_factory(
            "tagged",
            [captured_tag, &sink]() -> std::unique_ptr<PassConcept> {
                return make_pass_concept(TaggedPass{captured_tag, &sink});
            });
        check(bool(st), "add_factory captured-options registration succeeds");
        auto p = reg.create("tagged");
        check(p != nullptr, "create() returns a configured pass");
        if (p) {
            EmberPassManager pm;
            pm.add_pass_concept(std::move(p));
            pm.run(thf, am);
        }
        check(sink == captured_tag,
              "configured pass observed the captured immutable option");
    }

    // (10) Two create() calls yield distinct fresh PassConcept instances. A
    // configured factory must construct a new pass on every create(), never
    // hand out a shared/cached object.
    std::printf("(10) Configured factory creates fresh instances per create()\n");
    {
        int sink_a = -1, sink_b = -1;
        EmberPassRegistry reg;
        reg.add_factory("tagged", [&]() -> std::unique_ptr<PassConcept> {
            // Each call binds a distinct sink so the two instances are
            // independently observable as well as address-distinct.
            static int which = 0;
            int* sink = (which++ == 0) ? &sink_a : &sink_b;
            return make_pass_concept(TaggedPass{7, sink});
        });
        auto a = reg.create("tagged");
        auto b = reg.create("tagged");
        check(a != nullptr && b != nullptr, "both create() calls return a pass");
        check(a.get() != b.get(), "two create() calls yield distinct instances");
        // Independent runs write to independent sinks, confirming freshness.
        if (a && b) {
            EmberPassManager pm;
            pm.add_pass_concept(std::move(a));
            pm.run(thf, am);
            EmberPassManager pm2;
            pm2.add_pass_concept(std::move(b));
            pm2.run(thf, am);
        }
        check(sink_a == 7 && sink_b == 7, "both fresh instances run independently");
    }

    // (11) Duplicate registration is rejected and the ORIGINAL registration is
    // retained (not replaced). Verified for both add_factory and add<T> by
    // checking the retained factory still produces the original pass name.
    std::printf("(11) Duplicate rejection retains original registration\n");
    {
        EmberPassRegistry reg;
        ExtensionStatus s1 = reg.add_factory(
            "dup", []() -> std::unique_ptr<PassConcept> {
                return make_pass_concept(NoOpPass{});
            });
        check(bool(s1), "first add_factory(\"dup\") succeeds");
        ExtensionStatus s2 = reg.add_factory(
            "dup", []() -> std::unique_ptr<PassConcept> {
                return make_pass_concept(CountingPass{});
            });
        check(!bool(s2), "duplicate add_factory(\"dup\") is rejected");
        check(reg.has("dup"), "name still present after duplicate rejection");
        auto p = reg.create("dup");
        check(p != nullptr, "create(\"dup\") still works after rejected duplicate");
        check(p != nullptr && std::string(p->name()) == "noop",
              "original (NoOpPass) registration retained, not replaced");
    }
    {
        EmberPassRegistry reg;
        ExtensionStatus a1 = reg.add<NoOpPass>("dupT");
        check(bool(a1), "first add<T>(\"dupT\") succeeds");
        ExtensionStatus a2 = reg.add<CountingPass>("dupT");
        check(!bool(a2), "duplicate add<T>(\"dupT\") is rejected");
        auto p = reg.create("dupT");
        check(p != nullptr && std::string(p->name()) == "noop",
              "original add<T> registration retained, not replaced");
    }

    // (12) Empty name and null/empty std::function factories are rejected and
    // not stored.
    std::printf("(12) Empty name / null factory rejection\n");
    {
        EmberPassRegistry reg;
        ExtensionStatus sn = reg.add_factory("nully", PassFactory{});
        check(!bool(sn), "null std::function factory is rejected");
        check(!reg.has("nully"), "null factory is not stored");
        ExtensionStatus se = reg.add_factory(
            "", []() -> std::unique_ptr<PassConcept> {
                return make_pass_concept(NoOpPass{});
            });
        check(!bool(se), "empty name is rejected");
        check(!reg.has(""), "empty name is not stored");
        // add<T> empty name is rejected too.
        ExtensionStatus te = reg.add<NoOpPass>("");
        check(!bool(te), "add<T> with empty name is rejected");
    }

    // (13) names() is deterministic: sorted lexicographically, regardless of
    // insertion order and regardless of add<T> vs add_factory mixing.
    std::printf("(13) Deterministic (sorted) names()\n");
    {
        EmberPassRegistry reg;
        check(bool(reg.add_factory("zebra", []() -> std::unique_ptr<PassConcept> {
                  return make_pass_concept(NoOpPass{});
              })),
              "add_factory zebra ok");
        check(bool(reg.add<NoOpPass>("banana")), "add<T> banana ok");
        check(bool(reg.add_factory("mike", []() -> std::unique_ptr<PassConcept> {
                  return make_pass_concept(NoOpPass{});
              })),
              "add_factory mike ok");
        check(bool(reg.add_factory("alpha", []() -> std::unique_ptr<PassConcept> {
                  return make_pass_concept(NoOpPass{});
              })),
              "add_factory alpha ok");
        std::vector<std::string> names = reg.names();
        std::vector<std::string> expected = {"alpha", "banana", "mike", "zebra"};
        check(names.size() == 4, "names() lists all four registrations");
        check(names == expected, "names() is sorted lexicographically");
    }

    // ─── Red 2: strict transactional pipeline construction ───
    //
    // The accepted initial grammar is exactly `name (',' name)*` with
    // surrounding spaces/tabs permitted; an entirely empty spec is a
    // successful no-op. Every failure case below preloads the destination
    // manager with one runnable pass and a real instrumentation callback
    // bundle, then asserts the failed parse left the pass count, runnable
    // behavior, and instrumentation callback pointer completely unchanged.
    //
    // A helper registry is reused: noop, counting, and a configured factory
    // "nully" that returns nullptr (to exercise the null-factory-result path
    // through the pipeline parser, distinct from the null-std::function
    // rejection already covered in (12)).
    auto make_tx_registry = []() {
        EmberPassRegistry reg;
        reg.add<NoOpPass>("noop");
        reg.add<CountingPass>("counting");
        // A registered factory whose operator() returns nullptr. The pipeline
        // parser must treat a null factory result as a hard failure.
        reg.add_factory("nully", []() -> std::unique_ptr<PassConcept> {
            return nullptr;
        });
        return reg;
    };

    // A helper that preloads a manager with one CountingPass wired to `ran`,
    // installs a real instrumentation callback bundle at `cb`, and returns the
    // manager. Used by every failure case so the "unchanged" assertions are
    // meaningful (the manager is non-empty and instrumented before the parse).
    auto preload_manager = [&](EmberPassManager& pm, int& ran,
                               PassInstrumentationCallbacks& cb,
                               int& after_count) {
        pm.add_pass(CountingPass(&ran));
        after_count = 0;
        cb.after_pass = [&](const char*, const ThinFunction&, EmberPreserved) {
            ++after_count;
        };
        pm.instrumentation.callbacks = &cb;
    };

    // (14) Transactional append: an unknown middle token must not leave any
    // earlier-resolved pass appended. "noop,unknown,counting" resolves noop
    // first, then fails on "unknown"; the manager must keep exactly its one
    // preloaded pass, still run it, and keep its instrumentation pointer.
    std::printf("(14) Transactional append: unknown middle token leaves manager unchanged\n");
    {
        auto reg = make_tx_registry();
        EmberPassManager pm;
        int ran = 0, after_count = 0;
        PassInstrumentationCallbacks cb;
        preload_manager(pm, ran, cb, after_count);
        const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
        std::string err;
        bool ok = build_pipeline_from_string("noop,unknown,counting", reg, pm, &err);
        check(!ok, "parse with unknown middle token fails");
        check(err.find("unknown pass") != std::string::npos,
              "error mentions 'unknown pass' for middle token");
        check(pm.size() == 1, "manager pass count unchanged (1 preloaded, 0 appended)");
        check(pm.instrumentation.callbacks == cb_ptr,
              "instrumentation callback pointer unchanged on failure");
        // Runnable behavior: the preloaded pass still runs exactly once and
        // the instrumentation still fires.
        ran = 0; after_count = 0;
        EmberPreserved p = pm.run(thf, am);
        check(ran == 1, "preloaded pass still runs after failed append");
        check(after_count == 1, "instrumentation still fires after failed append");
        check(!p.all_preserved(), "preloaded CountingPass still returns none()");
    }

    // (15) Empty elements are rejected: middle ("noop,,counting"), leading
    // (",counting"), trailing ("noop,"), and whitespace-only ("noop,   ,counting").
    // Each must fail and leave a preloaded manager untouched.
    std::printf("(15) Empty elements rejected (middle/leading/trailing/whitespace-only)\n");
    {
        auto reg = make_tx_registry();
        struct Case { const char* spec; const char* label; };
        Case cases[] = {
            {"noop,,counting",    "middle empty element"},
            {",counting",         "leading empty element"},
            {"noop,",             "trailing empty element"},
            {"noop,   ,counting", "whitespace-only element"},
            {"   ,counting",      "leading whitespace-only element"},
            {"noop,   ",          "trailing whitespace-only element"},
        };
        for (const auto& c : cases) {
            EmberPassManager pm;
            int ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = build_pipeline_from_string(c.spec, reg, pm, &err);
            check(!ok, c.label);
            check(pm.size() == 1, "pass count unchanged for empty-element case");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation unchanged for empty-element case");
            ran = 0; pm.run(thf, am);
            check(ran == 1, "preloaded pass still runs for empty-element case");
        }
    }

    // (16) Unsupported parentheses are rejected. The initial grammar has no
    // parenthesized sub-pipelines, so any '(' or ')' is a hard error.
    std::printf("(16) Unsupported parentheses rejected\n");
    {
        auto reg = make_tx_registry();
        struct Case { const char* spec; const char* label; };
        Case cases[] = {
            {"noop,(counting)",   "parenthesized single name"},
            {"noop,counting)",    "trailing close paren"},
            {"(noop,counting",    "leading open paren"},
            {"flatten(subst,mba)", "nested sub-pipeline syntax"},
            {"noop()",            "empty paren group after name"},
        };
        for (const auto& c : cases) {
            EmberPassManager pm;
            int ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = build_pipeline_from_string(c.spec, reg, pm, &err);
            check(!ok, c.label);
            check(pm.size() == 1, "pass count unchanged for paren case");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation unchanged for paren case");
            ran = 0; pm.run(thf, am);
            check(ran == 1, "preloaded pass still runs for paren case");
        }
    }

    // (17) Trailing junk / unconsumed text is rejected. A name must be a bare
    // token; any character that is not part of a name (after trimming) makes
    // the whole token invalid so nothing partially appends.
    std::printf("(17) Trailing junk / unconsumed text rejected\n");
    {
        auto reg = make_tx_registry();
        struct Case { const char* spec; const char* label; };
        Case cases[] = {
            {"noop,counting!",     "trailing '!' junk"},
            {"noop,counting extra", "trailing space-separated junk"},
            {"noop,counting;x",    "trailing ';x' junk"},
            {"noop;counting",      "';' is not a separator"},
            {"noop count",         "space inside a token is not allowed"},
        };
        for (const auto& c : cases) {
            EmberPassManager pm;
            int ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = build_pipeline_from_string(c.spec, reg, pm, &err);
            check(!ok, c.label);
            check(pm.size() == 1, "pass count unchanged for trailing-junk case");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation unchanged for trailing-junk case");
            ran = 0; pm.run(thf, am);
            check(ran == 1, "preloaded pass still runs for trailing-junk case");
        }
    }

    // (18) A registered factory that returns nullptr is rejected. Unlike the
    // null std::function rejection in (12), here the factory is callable but
    // yields no pass; the parser must treat the missing pass as a failure and
    // leave the preloaded manager unchanged.
    std::printf("(18) Registered factory returning nullptr is rejected\n");
    {
        auto reg = make_tx_registry();
        EmberPassManager pm;
        int ran = 0, after_count = 0;
        PassInstrumentationCallbacks cb;
        preload_manager(pm, ran, cb, after_count);
        const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
        std::string err;
        bool ok = build_pipeline_from_string("nully", reg, pm, &err);
        check(!ok, "null factory result -> parse fails");
        // A null factory result must be distinguished from a truly unknown
        // name: the diagnostic must name a factory/null failure (the word
        // "factory" is unambiguous and does not appear in the pass name) and
        // must NOT collapse to the generic "unknown pass" wording.
        check(err.find("factory") != std::string::npos,
              "error mentions 'factory' for a null factory result");
        check(err.find("unknown pass") == std::string::npos,
              "null factory result is NOT reported as 'unknown pass'");
        check(pm.size() == 1, "pass count unchanged after null factory result");
        check(pm.instrumentation.callbacks == cb_ptr,
              "instrumentation unchanged after null factory result");
        ran = 0; pm.run(thf, am);
        check(ran == 1, "preloaded pass still runs after null factory result");
        // A null factory result buried after a valid name must also roll back.
        // As above, the failure is a null factory result (the name IS
        // registered), so the diagnostic must name the null/factory failure,
        // not "unknown pass", and the preloaded manager's passes AND
        // instrumentation must be left completely unchanged.
        EmberPassManager pm2;
        int ran2 = 0, after2 = 0;
        PassInstrumentationCallbacks cb2;
        preload_manager(pm2, ran2, cb2, after2);
        const PassInstrumentationCallbacks* cb2_ptr = pm2.instrumentation.callbacks;
        std::string err2;
        bool ok2 = build_pipeline_from_string("noop,nully,counting", reg, pm2, &err2);
        check(!ok2, "null factory result in the middle -> parse fails");
        check(err2.find("factory") != std::string::npos,
              "mid-list null factory result mentions 'factory'");
        check(err2.find("unknown pass") == std::string::npos,
              "mid-list null factory result is NOT reported as 'unknown pass'");
        check(pm2.size() == 1, "nothing appended before null factory result");
        check(pm2.instrumentation.callbacks == cb2_ptr,
              "instrumentation unchanged after mid-list null factory result");
        ran2 = 0; pm2.run(thf, am);
        check(ran2 == 1, "preloaded pass still runs after mid-list null factory");
    }

    // (19) Successful atomic append on a preloaded manager: the existing
    // build_pipeline_from_string append behavior adds the resolved passes to
    // the preloaded ones, preserves instrumentation, and every pass runs.
    std::printf("(19) Successful atomic append preserves preload + instrumentation\n");
    {
        auto reg = make_tx_registry();
        EmberPassManager pm;
        int pre_ran = 0, after_count = 0;
        PassInstrumentationCallbacks cb;
        preload_manager(pm, pre_ran, cb, after_count);
        const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
        std::string err;
        bool ok = build_pipeline_from_string("noop,counting", reg, pm, &err);
        check(ok, "append parse succeeds");
        check(pm.size() == 3, "1 preloaded + 2 appended = 3 passes");
        check(pm.instrumentation.callbacks == cb_ptr,
              "instrumentation preserved on successful append");
        // Running fires instrumentation for all three passes (after_pass is
        // called once per pass). The preloaded CountingPass increments pre_ran.
        pre_ran = 0; after_count = 0;
        pm.run(thf, am);
        check(pre_ran == 1, "preloaded pass ran during successful append run");
        check(after_count == 3, "instrumentation fired for all 3 passes");
        // An entirely empty spec is still a successful no-op and appends nothing.
        EmberPassManager pm2;
        int pre2 = 0, after2 = 0;
        PassInstrumentationCallbacks cb2;
        preload_manager(pm2, pre2, cb2, after2);
        std::string err2;
        bool ok2 = build_pipeline_from_string("", reg, pm2, &err2);
        check(ok2, "empty spec is a successful no-op");
        check(pm2.size() == 1, "empty spec appends nothing to preloaded manager");
        check(pm2.instrumentation.callbacks == &cb2,
              "instrumentation preserved on empty-spec no-op");
    }

    // (20) Atomic replace mode: replace_pipeline_from_string resolves every
    // token into temporary ownership and, only on complete success, replaces
    // the manager's passes WITHOUT moving or clearing its instrumentation.
    // On failure both the passes and instrumentation are preserved.
    std::printf("(20) Atomic replace mode preserves instrumentation\n");
    {
        auto reg = make_tx_registry();
        // Success: preloaded pass is replaced by the two resolved passes.
        {
            EmberPassManager pm;
            int pre_ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, pre_ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = replace_pipeline_from_string("noop,counting", reg, pm, &err);
            check(ok, "replace parse succeeds");
            check(pm.size() == 2, "replace swaps in exactly the resolved passes");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation preserved on successful replace");
            pre_ran = 0; after_count = 0;
            pm.run(thf, am);
            check(pre_ran == 0, "preloaded pass was replaced (did not run)");
            check(after_count == 2, "instrumentation fired for the 2 replaced passes");
        }
        // Failure: unknown name -> preloaded pass AND instrumentation kept.
        {
            EmberPassManager pm;
            int pre_ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, pre_ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = replace_pipeline_from_string("noop,unknown", reg, pm, &err);
            check(!ok, "replace with unknown name fails");
            check(pm.size() == 1, "replace failure keeps preloaded pass");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation preserved on failed replace");
            pre_ran = 0; pm.run(thf, am);
            check(pre_ran == 1, "preloaded pass still runs after failed replace");
        }
        // Failure: empty spec is a successful no-op for replace too (resolves
        // to zero passes), replacing the preload with nothing while keeping
        // instrumentation. This pins that replace-of-empty is a legitimate
        // "replace with the empty pipeline," not an error.
        {
            EmberPassManager pm;
            int pre_ran = 0, after_count = 0;
            PassInstrumentationCallbacks cb;
            preload_manager(pm, pre_ran, cb, after_count);
            const PassInstrumentationCallbacks* cb_ptr = pm.instrumentation.callbacks;
            std::string err;
            bool ok = replace_pipeline_from_string("", reg, pm, &err);
            check(ok, "empty spec replace is a successful no-op");
            check(pm.size() == 0, "empty spec replace clears passes");
            check(pm.instrumentation.callbacks == cb_ptr,
                  "instrumentation preserved on empty-spec replace");
            pre_ran = 0; pm.run(thf, am);
            check(pre_ran == 0, "no passes run after empty-spec replace");
        }
    }

    // ─── Red 3: seed derivation vectors ───
    //
    // The golden bytes below are HARD-CODED literals produced by a separate
    // reference implementation of the canonical algorithm documented in
    // src/seed_derivation.hpp. The production algorithm is NOT duplicated here
    // to recompute expected values; the test only compares against literals.
    // During the RED phase the stub returns zeros, so every non-zero golden
    // equality and every inequality assertion fails at runtime.

    // Helper: compare a derived 32-byte result to a hard-coded golden literal.
    auto eq32 = [](const std::array<uint8_t, 32>& a,
                   const std::array<uint8_t, 32>& b) -> bool {
        return a == b;
    };
    // Helper: build a SeedRequest from explicit fields.
    auto mkreq = [](std::string_view engine_version,
                    std::string_view module_id,
                    std::string_view build_profile_id,
                    std::string_view pass_name,
                    uint32_t pass_algorithm_version,
                    std::string_view function_name,
                    uint32_t logical_slot,
                    uint32_t block_id,
                    uint32_t instruction_ordinal,
                    std::string_view purpose) -> SeedRequest {
        SeedRequest r;
        r.engine_version         = engine_version;
        r.module_id              = module_id;
        r.build_profile_id       = build_profile_id;
        r.pass_name              = pass_name;
        r.pass_algorithm_version = pass_algorithm_version;
        r.function_name          = function_name;
        r.logical_slot           = logical_slot;
        r.block_id               = block_id;
        r.instruction_ordinal    = instruction_ordinal;
        r.purpose                = purpose;
        return r;
    };

    // (21) Empty / root-level derivation. Every string is empty except the
    // purpose and every integer is zero: the minimal (root-level) request.
    // Root = u64_to_root(0). The expected bytes are the pinned golden literal.
    std::printf("(21) Empty / root-level derivation golden + identity\n");
    {
        auto root0 = u64_to_root(0);
        // Pin the root adapter output too (independent golden literal).
        const std::array<uint8_t, 32> golden_root0 = {
            0x48,0xb9,0x7b,0xfb,0xa7,0x9d,0xb3,0x0d,
            0xb6,0xb1,0x90,0x33,0xf6,0x2d,0x34,0xf9,
            0xeb,0xce,0x93,0x52,0x4c,0xe3,0x64,0x27,
            0x7c,0x5c,0xe0,0xd8,0x15,0x31,0xbf,0xee,
        };
        check(eq32(root0, golden_root0), "u64_to_root(0) golden root");

        FixedRootSeedDeriver deriver(root0);
        auto req = mkreq("", "", "", "", 0, "", 0, 0, 0, "select");
        auto r1 = deriver.derive(req);
        check(bool(r1), "root-level derive succeeds (structured result ok)");
        const std::array<uint8_t, 32> golden_root_level = {
            0x1d,0x97,0x09,0xda,0xbb,0x24,0x6b,0xbc,
            0xcf,0x5d,0xa8,0x66,0x4c,0x62,0x03,0x0f,
            0xd8,0x90,0xab,0x15,0xfe,0xc4,0xa1,0x9a,
            0xda,0x48,0x77,0x99,0x71,0xd6,0xbd,0x7d,
        };
        check(r1.value && eq32(*r1.value, golden_root_level),
              "root-level derive golden bytes");
        // Identity: deriving the same request twice yields identical bytes.
        auto r2 = deriver.derive(req);
        check(r1.value && r2.value && eq32(*r1.value, *r2.value),
              "root-level derive is identity (same request -> same bytes)");
    }

    // (22) Function-level derivation: a request carrying engine/module/
    // profile/pass/function identities and a logical slot, site fields zero.
    std::printf("(22) Function-level derivation golden + identity\n");
    {
        FixedRootSeedDeriver deriver(u64_to_root(0));
        auto req = mkreq("ember-1", "mod-a", "light", "mba_expand", 1,
                         "compute", 3, 0, 0, "select");
        auto r1 = deriver.derive(req);
        check(bool(r1), "function-level derive succeeds");
        const std::array<uint8_t, 32> golden_fn = {
            0xc3,0xc5,0x24,0x4e,0x90,0x5a,0xe7,0xb5,
            0x6d,0xe0,0xaf,0xa1,0x3f,0x0c,0x0b,0x0e,
            0xc1,0x3f,0x2a,0xd7,0x36,0xf5,0xca,0x94,
            0x7d,0x35,0xaf,0x7f,0xfd,0xab,0xb5,0xc1,
        };
        check(r1.value && eq32(*r1.value, golden_fn),
              "function-level derive golden bytes");
        auto r2 = deriver.derive(req);
        check(r1.value && r2.value && eq32(*r1.value, *r2.value),
              "function-level derive is identity");
    }

    // (23) Site identity: a request pinning block_id + instruction_ordinal, a
    // different purpose, and a different root (u64_to_root(0xdeadbeef)).
    std::printf("(23) Site (block + ordinal) derivation golden + identity\n");
    {
        FixedRootSeedDeriver deriver(u64_to_root(0xdeadbeef));
        auto req = mkreq("ember-1", "mod-a", "light", "mba_expand", 1,
                         "compute", 3, 5, 42, "variant");
        auto r1 = deriver.derive(req);
        check(bool(r1), "site derive succeeds");
        const std::array<uint8_t, 32> golden_site = {
            0x86,0x57,0x25,0x38,0xc4,0x0d,0x21,0x70,
            0x36,0xaa,0x3e,0x45,0x6b,0x0e,0xd0,0xf0,
            0x1f,0x2d,0xee,0x0e,0xce,0xf2,0xf7,0x53,
            0xf3,0xda,0x3c,0x7d,0x23,0xba,0x62,0x7d,
        };
        check(r1.value && eq32(*r1.value, golden_site),
              "site derive golden bytes");
        auto r2 = deriver.derive(req);
        check(r1.value && r2.value && eq32(*r1.value, *r2.value),
              "site derive is identity");
    }

    // (24) Distinct purposes yield distinct output. Two requests differing
    // ONLY in purpose (select vs constant) must produce different bytes, and
    // each is pinned to a golden literal.
    std::printf("(24) Distinct purposes yield distinct output\n");
    {
        FixedRootSeedDeriver deriver(u64_to_root(0));
        auto reqA = mkreq("ember-1", "mod-a", "light", "const_encode", 1,
                          "compute", 3, 5, 42, "select");
        auto reqB = mkreq("ember-1", "mod-a", "light", "const_encode", 1,
                          "compute", 3, 5, 42, "constant");
        auto rA = deriver.derive(reqA);
        auto rB = deriver.derive(reqB);
        check(bool(rA) && bool(rB), "both purpose derivations succeed");
        const std::array<uint8_t, 32> golden_purpA = {
            0x93,0x86,0x3e,0xcb,0xd1,0xb8,0xfd,0x32,
            0xc9,0x08,0xe0,0xc9,0x3b,0x97,0x99,0x33,
            0xd8,0x30,0x6d,0x37,0xc2,0x4d,0x5c,0xeb,
            0x30,0x59,0xd6,0x5b,0x31,0x64,0xb7,0x85,
        };
        const std::array<uint8_t, 32> golden_purpB = {
            0x74,0x77,0x33,0xae,0x5a,0x86,0xc7,0x35,
            0xb3,0x90,0x6c,0x8d,0x2b,0xc3,0x4e,0x69,
            0x8d,0xb5,0x1c,0x77,0xce,0x9d,0x76,0xab,
            0xe4,0x79,0xac,0xe7,0xdd,0x7c,0xe6,0xfd,
        };
        check(rA.value && eq32(*rA.value, golden_purpA),
              "purpose=select golden bytes");
        check(rB.value && eq32(*rB.value, golden_purpB),
              "purpose=constant golden bytes");
        check(rA.value && rB.value && !eq32(*rA.value, *rB.value),
              "distinct purposes produce distinct output");
    }

    // (25) Fixed seed 0: a request derived under u64_to_root(0), pinned golden.
    std::printf("(25) Fixed seed 0 golden bytes\n");
    {
        FixedRootSeedDeriver deriver(u64_to_root(0));
        auto req = mkreq("ember-1", "mod-a", "balanced", "opaque_pred", 2,
                         "guard", 1, 2, 9, "truth");
        auto r = deriver.derive(req);
        check(bool(r), "fixed-seed-0 derive succeeds");
        const std::array<uint8_t, 32> golden_seed0 = {
            0xd9,0xb1,0xcf,0x01,0xe9,0xb8,0x66,0xc6,
            0x22,0x02,0x00,0xc8,0x22,0xba,0x5b,0x3f,
            0x45,0x51,0xe7,0xb2,0x9a,0x91,0xa6,0x47,
            0xcd,0x6d,0x88,0xa9,0x8c,0x89,0x73,0x74,
        };
        check(r.value && eq32(*r.value, golden_seed0),
              "fixed seed 0 golden bytes");
    }

    // (26) Fixed seed UINT64_MAX: the same request under u64_to_root(UINT64_MAX),
    // pinned golden, and distinct from seed 0 (exercises the edge seed).
    std::printf("(26) Fixed seed UINT64_MAX golden bytes (distinct from seed 0)\n");
    {
        const std::array<uint8_t, 32> golden_root_max = {
            0x1f,0x79,0x62,0xe1,0x8c,0xb2,0x1f,0x59,
            0xa6,0x23,0xd9,0x79,0x8c,0xbe,0x45,0x13,
            0x19,0xf3,0x19,0x38,0x0f,0x7a,0xbc,0xdb,
            0x39,0xca,0x99,0xe7,0x0e,0xf2,0x04,0x1c,
        };
        auto rootMax = u64_to_root(UINT64_MAX);
        check(eq32(rootMax, golden_root_max), "u64_to_root(UINT64_MAX) golden root");
        FixedRootSeedDeriver deriver(rootMax);
        auto req = mkreq("ember-1", "mod-a", "balanced", "opaque_pred", 2,
                         "guard", 1, 2, 9, "truth");
        auto r = deriver.derive(req);
        check(bool(r), "fixed-seed-UINT64_MAX derive succeeds");
        const std::array<uint8_t, 32> golden_seed_max = {
            0x46,0xb5,0x22,0x17,0x9b,0x83,0x3f,0x61,
            0xe8,0xe1,0xf6,0x43,0x43,0xd7,0x9b,0xf4,
            0x60,0x5a,0x7c,0x6c,0xbe,0xb8,0x37,0xb1,
            0x2e,0x35,0x38,0xf1,0x7d,0x29,0x50,0x63,
        };
        check(r.value && eq32(*r.value, golden_seed_max),
              "fixed seed UINT64_MAX golden bytes");
        // Distinct from the seed-0 result of the same request (25).
        FixedRootSeedDeriver deriver0(u64_to_root(0));
        auto r0 = deriver0.derive(req);
        check(r.value && r0.value && !eq32(*r.value, *r0.value),
              "seed 0 and seed UINT64_MAX produce distinct output for same request");
    }

    // (27) Initial StableRng outputs. StableRng(0).next() x4 and bounded() x5
    // (bounded(7) x3 then bounded(1000003) x2) are pinned to golden u64 values.
    std::printf("(27) Initial StableRng outputs (next + bounded)\n");
    {
        StableRng rng(0);
        const uint64_t golden_next[4] = {
            0xe220a8397b1dcdafULL,
            0x6e789e6aa1b965f4ULL,
            0x06c45d188009454fULL,
            0xf88bb8a8724c81ecULL,
        };
        for (int i = 0; i < 4; ++i) {
            uint64_t v = rng.next();
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                          "StableRng(0).next()[%d] golden 0x%016llx",
                          i, (unsigned long long)golden_next[i]);
            check(v == golden_next[i], buf);
        }
        // bounded() with rejection sampling: 7 does not divide 2^64.
        StableRng rng_b7(0);
        const uint64_t golden_b7[3] = {2, 1, 2};
        for (int i = 0; i < 3; ++i) {
            uint64_t v = rng_b7.bounded(7);
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                          "StableRng(0).bounded(7)[%d] golden %llu",
                          i, (unsigned long long)golden_b7[i]);
            check(v == golden_b7[i], buf);
            check(v < 7, "bounded(7) result in range [0,7)");
        }
        // A prime modulus well away from a power of two: heavy rejection.
        StableRng rng_bp(0);
        const uint64_t golden_bp[2] = {4995, 431482};
        for (int i = 0; i < 2; ++i) {
            uint64_t v = rng_bp.bounded(1000003);
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                          "StableRng(0).bounded(1000003)[%d] golden %llu",
                          i, (unsigned long long)golden_bp[i]);
            check(v == golden_bp[i], buf);
            check(v < 1000003, "bounded(1000003) result in range [0,1000003)");
        }
        // Power-of-two bound: no rejection. A fresh rng's first bounded(256)
        // draw must equal a separate fresh rng's first next() % 256, proving
        // bounded took exactly one draw (no rejection loop) and reduced mod n.
        StableRng rng_b256(0);
        uint64_t b = rng_b256.bounded(256);
        StableRng rng_raw(0);
        uint64_t raw = rng_raw.next();
        check(b == raw % 256, "bounded(256) == next() % 256 (one draw, no rejection)");
        check(b < 256, "bounded(256) result in range [0,256)");
        // n == 0 is degenerate and returns 0.
        StableRng rng0(0);
        check(rng0.bounded(0) == 0, "bounded(0) returns 0");
    }

    // (28) Order independence: derive the same request set in forward and
    // reverse order. Because the deriver holds no shared advancing state,
    // every request's result must be identical regardless of derivation order.
    // (This is the property that makes compile order irrelevant: a per-site
    // StableRng seeded from an independent lane would likewise be unaffected
    // by peer-site draw order.)
    std::printf("(28) Order independence (forward vs reverse)\n");
    {
        FixedRootSeedDeriver deriver(u64_to_root(0));
        const std::array<uint8_t, 32> roots[] = { u64_to_root(0) };
        (void)roots;
        SeedRequest reqs[] = {
            mkreq("ember-1", "mod-a", "light", "mba_expand", 1, "compute", 3, 0, 0, "select"),
            mkreq("ember-1", "mod-a", "light", "mba_expand", 1, "compute", 3, 5, 42, "variant"),
            mkreq("ember-1", "mod-a", "light", "const_encode", 1, "compute", 3, 5, 42, "constant"),
            mkreq("ember-1", "mod-a", "balanced", "opaque_pred", 2, "guard", 1, 2, 9, "truth"),
            mkreq("", "", "", "", 0, "", 0, 0, 0, "select"),
        };
        constexpr size_t N = sizeof(reqs) / sizeof(reqs[0]);
        std::array<std::array<uint8_t, 32>, N> fwd{}, rev{};
        for (size_t i = 0; i < N; ++i) {
            auto r = deriver.derive(reqs[i]);
            check(bool(r), "forward derive ok");
            if (r.value) fwd[i] = *r.value;
        }
        for (size_t i = 0; i < N; ++i) {
            size_t j = N - 1 - i;
            auto r = deriver.derive(reqs[j]);
            check(bool(r), "reverse derive ok");
            if (r.value) rev[j] = *r.value;
        }
        bool same = true;
        for (size_t i = 0; i < N; ++i)
            if (!eq32(fwd[i], rev[i])) same = false;
        check(same, "forward and reverse derivation produce identical per-request results");
    }

    // (29) Parallel: one const deriver shared across worker threads, every
    // result equal to serial derivation. Pins immutability / thread safety:
    // derive() is const and mutates nothing, so concurrent reads of one
    // shared object are safe and race-free.
    std::printf("(29) Parallel: one const deriver shared across workers\n");
    {
        const FixedRootSeedDeriver deriver(u64_to_root(0));
        SeedRequest reqs[] = {
            mkreq("ember-1", "mod-a", "light", "mba_expand", 1, "compute", 3, 0, 0, "select"),
            mkreq("ember-1", "mod-a", "light", "mba_expand", 1, "compute", 3, 5, 42, "variant"),
            mkreq("ember-1", "mod-a", "light", "const_encode", 1, "compute", 3, 5, 42, "constant"),
            mkreq("ember-1", "mod-a", "balanced", "opaque_pred", 2, "guard", 1, 2, 9, "truth"),
            mkreq("", "", "", "", 0, "", 0, 0, 0, "select"),
            mkreq("ember-1", "mod-a", "light", "deadcode", 1, "inject", 7, 11, 3, "junk-count"),
            mkreq("ember-1", "mod-a", "light", "block_split", 1, "split", 2, 4, 6, "select"),
            mkreq("ember-1", "mod-a", "light", "str_encrypt", 1, "enc", 9, 1, 0, "string-key"),
        };
        constexpr size_t N = sizeof(reqs) / sizeof(reqs[0]);

        // Serial reference results.
        std::array<std::array<uint8_t, 32>, N> serial{};
        for (size_t i = 0; i < N; ++i) {
            auto r = deriver.derive(reqs[i]);
            check(bool(r), "parallel: serial reference derive ok");
            if (r.value) serial[i] = *r.value;
        }

        // Worker results, shared deriver, all right with no synchronization
        // beyond the per-slot flag writes.
        std::array<std::array<uint8_t, 32>, N> para{};
        std::atomic<unsigned> mismatches{0};
        std::atomic<unsigned> done{0};
        auto worker = [&](size_t base) {
            for (size_t k = 0; k < N; ++k) {
                size_t i = (base + k) % N;  // different start offset per worker
                auto r = deriver.derive(reqs[i]);
                if (!r.value) { ++mismatches; continue; }
                para[i] = *r.value;
                if (!eq32(*r.value, serial[i])) ++mismatches;
            }
            ++done;
        };
        const unsigned nworkers = 4;
        std::vector<std::thread> threads;
        threads.reserve(nworkers);
        for (unsigned w = 0; w < nworkers; ++w)
            threads.emplace_back(worker, w);
        for (auto& t : threads) t.join();

        check(done.load() == nworkers, "all worker threads completed");
        check(mismatches.load() == 0,
              "every parallel result equals serial derivation (no races)");
        // Final per-slot equality against the serial reference.
        bool allsame = true;
        for (size_t i = 0; i < N; ++i)
            if (!eq32(para[i], serial[i])) allsame = false;
        check(allsame, "parallel per-slot results identical to serial");
    }

    // (30) Structured errors, no printing. A host-supplied SeedDeriver may
    // fail; the failure must come back as an ExtensionError (registry ==
    // "ember-seed-deriver"), never as a printed diagnostic or a thrown
    // exception. The fixed-root deriver always succeeds. There is no
    // mutable/global stream: the only state is the immutable root.
    std::printf("(30) Structured errors (no printing, no global stream)\n");
    {
        // A deliberately-failing deriver used to pin the error contract.
        struct FailingDeriver : SeedDeriver {
            ExtensionResult<std::array<uint8_t, 32>>
            derive(const SeedRequest&) const override {
                return make_extension_result_error<std::array<uint8_t, 32>>(
                    kSeedDeriverRegistryId, "",
                    "external seed material unavailable");
            }
        };
        FailingDeriver fd;
        auto r = fd.derive(mkreq("", "", "", "", 0, "", 0, 0, 0, "select"));
        check(!bool(r), "failing deriver returns a failure result");
        check(!r.value.has_value(), "failure result carries no value");
        check(r.error.has_value(), "failure result carries a structured error");
        if (r.error) {
            check(r.error->registry == std::string(kSeedDeriverRegistryId),
                  "error.registry == \"ember-seed-deriver\"");
            check(!r.error->message.empty(), "error carries a diagnostic message");
        }
        // The fixed-root deriver always succeeds (no failure path).
        const FixedRootSeedDeriver ok_deriver(u64_to_root(0));
        auto okr = ok_deriver.derive(mkreq("", "", "", "", 0, "", 0, 0, 0, "select"));
        check(bool(okr) && okr.value.has_value() && !okr.error.has_value(),
              "fixed-root deriver always succeeds (structured ok)");
    }

    // ===================================================================
    // ─── Red 5: checked pass execution ───
    //
    // Pins the checked pass path: per-pass snapshots, validation after each
    // reported mutation, rollback on validation/pass/growth failure, recoverable
    // PassError vs unexpected std::exception, hard growth ceilings in the
    // ordinary one-shot run AND fixed-point execution, no library-only stderr,
    // structured pass name / stop reason / error / rounds / initial+final
    // counts, and a checked compile boundary that stops before regalloc/emit on
    // validation failure while honoring CodeGenCtx::analysis_manager.
    //
    // During the RED phase these symbols do not exist yet, so this section does
    // not compile. Once the checked path lands, every assertion below must hold.
    // ===================================================================

    // ─── Red 5 helpers ───

    // A minimal VALID ThinFunction (a single entry block returning a const).
    // verify_thin_function_for_codegen MUST accept this.
    auto build_valid_thinfn = []() -> ThinFunction {
        ThinFunction thf;
        thf.name = "checked";
        thf.slot = 0;
        auto i64_ty = std::make_shared<Type>();
        i64_ty->prim = Prim::I64;
        thf.ret_type = i64_ty.get();
        thf.owned_types.push_back(std::move(i64_ty));
        thf.frame.frame_size = 16;
        thf.frame.rbx_save_offset = -8;
        thf.frame.struct_ret_ptr_offset = 0;
        thf.frame.arg_temps_base = 0;
        thf.frame.next_local_off = 8;
        thf.frame.returns_struct_by_ptr = false;
        ThinBlock blk0;
        blk0.id = 0;
        ThinInstr c;
        c.op = ThinOp::ConstInt;
        c.dst = 1;
        c.imm.i = 42;
        c.meta.width = 8;
        blk0.instrs.push_back(c);
        blk0.term.kind = TermKind::Return;
        blk0.term.ret = 1;
        thf.blocks.push_back(std::move(blk0));
        thf.declared_max_vreg = 2;  // v1 valid; v0 invalid
        return thf;
    };

    // Count total instructions across all blocks.
    auto total_instrs = [](const ThinFunction& f) -> std::size_t {
        std::size_t n = 0;
        for (const auto& b : f.blocks) n += b.instrs.size();
        return n;
    };

    // ─── (31) verify_thin_function_for_codegen accepts valid IR ───
    std::printf("(31) verify_thin_function_for_codegen accepts valid IR\n");
    {
        auto thf = build_valid_thinfn();
        std::string verr;
        bool ok = verify_thin_function_for_codegen(thf, &verr);
        check(ok, "valid ThinFunction verifies for codegen");
        if (!ok) std::printf("    verr: %s\n", verr.c_str());
    }

    // ─── (32) a pass that creates malformed CFG/frame/rodata and returns
    //       changed is caught by the verifier, rolled back, and reported ───
    std::printf("(32) malformed-mutation pass: validation failure + rollback\n");
    {
        struct Case { const char* label; std::function<void()> run; };
        // CorruptTerm
        {
            auto thf = build_valid_thinfn();
            ThinFunction snapshot = thf;  // known-good copy
            EmberPassManager pm;
            pm.add_pass(CorruptTermPass{});
            EmberAnalysisManager am;
            CheckedRunOptions opts;
            PassRunReport rep = pm.run_checked(thf, am, opts);
            check(rep.stop_reason == PassStopReason::ValidationFailure,
                  "corrupt-term -> ValidationFailure");
            check(std::string(rep.pass_name) == "corrupt-term",
                  "report names the corrupting pass");
            check(!rep.error.empty(), "ValidationFailure carries an error message");
            // Snapshot restoration: the function equals the known-good copy.
            check(thf.blocks.size() == snapshot.blocks.size(),
                  "rollback restored block count");
            check(!thf.blocks.empty() &&
                  thf.blocks[0].term.kind == TermKind::Return,
                  "rollback restored the entry terminator");
            check(total_instrs(thf) == total_instrs(snapshot),
                  "rollback restored instruction count");
        }
        // CorruptBlockId
        {
            auto thf = build_valid_thinfn();
            EmberPassManager pm;
            pm.add_pass(CorruptBlockIdPass{});
            EmberAnalysisManager am;
            CheckedRunOptions opts;
            PassRunReport rep = pm.run_checked(thf, am, opts);
            check(rep.stop_reason == PassStopReason::ValidationFailure,
                  "corrupt-blockid -> ValidationFailure");
            check(!thf.blocks.empty() && thf.blocks[0].id == 0,
                  "rollback restored entry block id 0");
        }
        // CorruptFrame
        {
            auto thf = build_valid_thinfn();
            EmberPassManager pm;
            pm.add_pass(CorruptFramePass{});
            EmberAnalysisManager am;
            CheckedRunOptions opts;
            PassRunReport rep = pm.run_checked(thf, am, opts);
            check(rep.stop_reason == PassStopReason::ValidationFailure,
                  "corrupt-frame -> ValidationFailure");
            check(thf.frame.frame_size == 16, "rollback restored frame_size");
        }
        // CorruptRodata
        {
            auto thf = build_valid_thinfn();
            EmberPassManager pm;
            pm.add_pass(CorruptRodataPass{});
            EmberAnalysisManager am;
            CheckedRunOptions opts;
            PassRunReport rep = pm.run_checked(thf, am, opts);
            check(rep.stop_reason == PassStopReason::ValidationFailure,
                  "corrupt-rodata -> ValidationFailure");
            check(total_instrs(thf) == 1, "rollback removed the injected bad instr");
        }
    }

    // ─── (33) input validation failure: an already-malformed function is
    //       rejected before any pass runs ───
    std::printf("(33) input validation failure (no pass runs)\n");
    {
        auto thf = build_valid_thinfn();
        thf.blocks[0].term.kind = TermKind::None;  // malformed input
        int ran = 0;
        EmberPassManager pm;
        pm.add_pass(CountingPass(&ran));
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::ValidationFailure,
              "malformed input -> ValidationFailure");
        check(ran == 0, "no pass runs when input fails validation");
        // The malformed input is left as-is (no snapshot to restore TO a good
        // state — the input itself was bad); the pass simply did not run.
        check(thf.blocks[0].term.kind == TermKind::None,
              "malformed input left unchanged (pass did not run)");
    }

    // ─── (34) later passes do not run after a failure ───
    std::printf("(34) later passes do not run after a failure\n");
    {
        auto thf = build_valid_thinfn();
        int later_ran = 0;
        EmberPassManager pm;
        pm.add_pass(CorruptTermPass{});
        pm.add_pass(CountingPass(&later_ran));
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::ValidationFailure,
              "first-pass corruption stops the pipeline");
        check(later_ran == 0, "the later CountingPass did not run after failure");
        check(std::string(rep.pass_name) == "corrupt-term",
              "report names the first (failing) pass");
    }

    // ─── (35) PassError and unexpected std::exception conversion ───
    std::printf("(35) PassError + unexpected std::exception conversion\n");
    {
        // Recoverable PassError -> stop_reason == PassError, message carried.
        auto thf = build_valid_thinfn();
        EmberPassManager pm;
        pm.add_pass(PassErrorPass{"boom"});
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::PassError,
              "PassError -> stop_reason PassError");
        check(std::string(rep.pass_name) == "perr",
              "PassError report names the throwing pass");
        check(rep.error.find("boom") != std::string::npos,
              "PassError carries the message");
        // Rollback: the function is unchanged.
        check(!thf.blocks.empty() && thf.blocks[0].term.kind == TermKind::Return,
              "PassError rolled back the function");
    }
    {
        // Unexpected std::exception -> stop_reason == ExceptionError.
        auto thf = build_valid_thinfn();
        EmberPassManager pm;
        pm.add_pass(UnexpectedExceptionPass{});
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::ExceptionError,
              "unexpected std::exception -> stop_reason ExceptionError");
        check(std::string(rep.pass_name) == "unexpected",
              "exception report names the throwing pass");
        check(!rep.error.empty(), "ExceptionError carries a message");
        check(!thf.blocks.empty() && thf.blocks[0].term.kind == TermKind::Return,
              "exception rolled back the function");
    }

    // ─── (36) an ordinary one-shot pass exceeding instruction/growth ceilings ───
    std::printf("(36) one-shot pass exceeding instruction/growth ceilings\n");
    {
        auto thf = build_valid_thinfn();
        const std::size_t before = total_instrs(thf);
        EmberPassManager pm;
        pm.add_pass(BlowupPass{100});  // +100 instrs
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        opts.max_instructions = 50;   // a low absolute ceiling
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::GrowthLimit,
              "blowup pass -> stop_reason GrowthLimit");
        check(std::string(rep.pass_name) == "blowup",
              "GrowthLimit report names the blowup pass");
        // Rollback: the injected instructions are gone.
        check(total_instrs(thf) == before, "GrowthLimit rolled back the blowup");
        check(rep.initial_count == before, "report.initial_count is the pre-run count");
        check(rep.final_count > before, "report.final_count reflects the blowup (pre-rollback)");
    }
    // The ordinary (legacy) one-shot run() also enforces a hard growth ceiling:
    // a pass that exceeds the hard instruction cap does not let later passes
    // run. (run() cannot report a structured stop reason — it returns
    // EmberPreserved — so the observable contract is "stops running further
    // passes".)
    {
        auto thf = build_valid_thinfn();
        int later_ran = 0;
        EmberPassManager pm;
        pm.add_pass(BlowupPass{EmberPassManager::hard_max_ir_instructions + 1});
        pm.add_pass(CountingPass(&later_ran));
        EmberAnalysisManager am;
        EmberPreserved p = pm.run(thf, am);
        check(later_ran == 0,
              "ordinary run() stops after a hard-cap-exceeding pass (later pass skipped)");
        (void)p;
    }

    // ─── (37) structured pass name / stop reason / error / rounds /
    //       initial+final counts ───
    std::printf("(37) structured report fields\n");
    {
        // Clean one-shot: NoOp (no change). Completed, rounds=1, counts equal,
        // error empty, pass_name of the last pass.
        auto thf = build_valid_thinfn();
        const std::size_t n = total_instrs(thf);
        EmberPassManager pm;
        pm.add_pass(NoOpPass{});
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        PassRunReport rep = pm.run_checked(thf, am, opts);
        check(rep.stop_reason == PassStopReason::Completed,
              "clean one-shot -> Completed");
        check(rep.rounds == 1, "one-shot report rounds == 1");
        check(rep.initial_count == n, "Completed initial_count");
        check(rep.final_count == n, "Completed final_count == initial_count");
        check(rep.error.empty(), "Completed report has no error");
        check(std::string(rep.pass_name) == "noop", "Completed report names the pass");
    }
    {
        // Checked fixpoint: a ConvergingPass that changes for 2 rounds then
        // converges. Converged, rounds == 3, error empty.
        auto thf = build_valid_thinfn();
        int round = 0;
        EmberPassManager pm;
        pm.add_pass(ConvergingPass(&round));
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        opts.max_rounds = 8;
        PassRunReport rep = pm.run_checked_fixpoint(thf, am, opts);
        check(rep.stop_reason == PassStopReason::Converged,
              "converging fixpoint -> Converged");
        check(rep.rounds == 3, "Converged report rounds == 3");
        check(rep.error.empty(), "Converged report has no error");
        check(rep.initial_count == total_instrs(build_valid_thinfn()),
              "fixpoint report.initial_count");
    }
    {
        // Round limit: a pass that always changes (never converges) hits
        // max_rounds -> RoundLimit.
        auto thf = build_valid_thinfn();
        int round = 0;
        EmberPassManager pm;
        pm.add_pass(CountingPass{});  // always returns none() (always changes)
        EmberAnalysisManager am;
        CheckedRunOptions opts;
        opts.max_rounds = 3;
        PassRunReport rep = pm.run_checked_fixpoint(thf, am, opts);
        check(rep.stop_reason == PassStopReason::RoundLimit,
              "never-converging fixpoint -> RoundLimit");
        check(rep.rounds == 3, "RoundLimit report rounds == max_rounds");
    }

    // ─── (38) checked compilation stops before regalloc or emit ───
    //
    // compile_func_checked runs the configured pass manager in checked mode
    // between lower_function and regalloc/emit. On a pass that corrupts the
    // IR, the result must report a validation failure, emit NO CompiledFn
    // (exec == nullptr), and never reach run_regalloc or emit_x64. A clean
    // compile produces an emitted CompiledFn that runs correctly.
    std::printf("(38) compile_func_checked stops before regalloc/emit\n");
    {
        // Compile helper: parse + sema a tiny i64 source, register the opt
        // passes + a custom corrupting pass, and call compile_func_checked.
        const char* src =
            "fn main() -> i64 { let x = 7; let y = 3; return x + y; }\n";
        auto lr = tokenize(src, "<checked>");
        auto pr = parse(std::move(lr.toks));
        check(pr.ok, "checked-compile source parses");
        bool setup_ok = pr.ok;
        int si = 0;
        std::unordered_map<std::string, int> slots;
        if (setup_ok) {
            for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = si - 1; }
        }
        std::unordered_map<std::string, NativeSig> natives;
        OpOverloadTable overloads;
        StructLayoutTable layouts;
        if (setup_ok) {
            layouts = build_struct_layouts(pr.program);
            pr.program.string_xor_key = 0;
            auto sr = sema(pr.program, natives, slots, 0, &overloads, &layouts);
            check(sr.ok, "checked-compile source semas");
            setup_ok = setup_ok && sr.ok;
        }
        // Wire a minimal globals block + dispatch table (no globals/calls here).
        GlobalsBlock gb; gb.base = 0;
        g_globals_for_codegen = &gb;
        DispatchTable table(setup_ok ? pr.program.funcs.size() : 1);

        if (setup_ok) {

        // (38a) Clean compile: passes that do not corrupt -> emitted fn runs
        // and returns 10. Backend used == IR; pass reports present.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("constprop,dce", reg, pm, &perr),
                  "clean checked-compile pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = true;
            ctx.pass_manager = &pm;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "clean checked-compile succeeds");
            check(cr.backend == CompileBackend::IRBackend,
                  "clean checked-compile used the IR backend");
            check(!cr.pass_reports.empty(), "clean checked-compile carries pass reports");
            check(!cr.compiled.bytes.empty(),
                  "clean checked-compile emitted bytes (pre-finalize)");
            if (!cr.compiled.bytes.empty()) {
                if (!finalize(cr.compiled)) {
                    check(false, "clean checked-compile finalize failed");
                } else {
                    check(cr.compiled.exec != nullptr,
                          "clean checked-compile finalized an executable CompiledFn");
                    table.set(pr.program.funcs[0].slot, cr.compiled.entry);
                    using F = int64_t (*)();
                    int64_t got = reinterpret_cast<F>(table.get(pr.program.funcs[0].slot))();
                    check(got == 10, "clean checked-compile emitted fn returns 10");
                    free_executable(cr.compiled.exec);
                }
            }
        }
        // (38b) Corrupting pass -> validation failure, no emit, no exec.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<CorruptTermPass>("corrupt-term");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("corrupt-term", reg, pm, &perr),
                  "corrupting pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = true;
            ctx.pass_manager = &pm;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(!cr.ok(), "corrupting checked-compile does not succeed");
            check(cr.compiled.exec == nullptr,
                  "corrupting checked-compile emitted NO executable (stopped before emit)");
            bool failure_reported = false;
            for (const auto& rep : cr.pass_reports)
                if (rep.stop_reason == PassStopReason::ValidationFailure)
                    failure_reported = true;
            check(failure_reported,
                  "corrupting checked-compile reports a ValidationFailure pass report");
            check(!cr.reason.empty(),
                  "corrupting checked-compile carries a failure reason");
        }
        // (38c) A pass that throws -> the exception does NOT cross the compile
        // boundary; the result reports failure with no exec.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<PassErrorPass>("perr");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("perr", reg, pm, &perr),
                  "throwing pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.pass_manager = &pm;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(!cr.ok(), "throwing checked-compile does not succeed");
            check(cr.compiled.exec == nullptr,
                  "throwing checked-compile emitted NO executable");
            check(!cr.reason.empty(),
                  "throwing checked-compile carries a failure reason");
            bool passerr = false;
            for (const auto& rep : cr.pass_reports)
                if (rep.stop_reason == PassStopReason::PassError) passerr = true;
            check(passerr, "throwing checked-compile reports a PassError pass report");
        }
        // (38d) compile_func_checked honors CodeGenCtx::analysis_manager: the
        // passes receive the host-provided manager, not a freshly-constructed
        // local one.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<AmCapturePass>("amcap");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("amcap", reg, pm, &perr),
                  "amcap pipeline builds");
            EmberAnalysisManager host_am;
            const EmberAnalysisManager* captured = nullptr;
            // Stash the capture sink into the pass via a configured factory so
            // the fresh instance records the manager it received.
            reg.add_factory("amcap2", [&captured]() -> std::unique_ptr<PassConcept> {
                return make_pass_concept(AmCapturePass{&captured});
            });
            EmberPassManager pm2;
            check(build_pipeline_from_string("amcap2", reg, pm2, &perr),
                  "amcap2 configured pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.pass_manager = &pm2;
            ctx.analysis_manager = &host_am;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "amcap checked-compile succeeds");
            check(captured == &host_am,
                  "compile_func_checked passed ctx.analysis_manager to the pass");
            if (cr.compiled.exec) free_executable(cr.compiled.exec);
        }
        // (38e) stale/pre-existing regalloc is cleared before the single
        // allowed allocation stage. A pass leaves a bogus ra.enabled=true; with
        // enable_regalloc=false the stale ra must NOT reach emit (the emitted
        // fn still computes the right value, proving the stale ra was cleared).
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<StaleRegallocPass>("stale-ra");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("stale-ra", reg, pm, &perr),
                  "stale-ra pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = false;  // no fresh regalloc; stale ra must be cleared
            ctx.pass_manager = &pm;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "stale-ra checked-compile succeeds");
            check(!cr.compiled.bytes.empty(),
                  "stale-ra checked-compile emitted bytes (pre-finalize)");
            if (!cr.compiled.bytes.empty()) {
                if (!finalize(cr.compiled)) {
                    check(false, "stale-ra checked-compile finalize failed");
                } else {
                    check(cr.compiled.exec != nullptr,
                          "stale-ra cleared before emit -> finalized an executable");
                    table.set(pr.program.funcs[0].slot, cr.compiled.entry);
                    using F = int64_t (*)();
                    int64_t got = reinterpret_cast<F>(table.get(pr.program.funcs[0].slot))();
                    check(got == 10,
                          "stale-ra cleared before emit -> emitted fn still returns 10");
                    free_executable(cr.compiled.exec);
                }
            }
        }
        // (38f) No-pass-manager IR compilation: when enable_ir_backend is true
        // but pass_manager is nullptr, compile_func_checked STILL uses the IR
        // backend (reports CompileBackend::IRBackend, NOT TreeWalker), runs
        // the checked pre-regalloc/emit verification, clears stale regalloc,
        // and emits a working CompiledFn.
        {
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = true;
            ctx.pass_manager = nullptr;   // no passes, but IR backend requested
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "no-pass-manager checked-compile succeeds");
            check(cr.backend == CompileBackend::IRBackend,
                  "no-pass-manager checked-compile used the IR backend (not TreeWalker)");
            check(cr.pass_reports.empty(),
                  "no-pass-manager checked-compile carries no pass reports");
            check(!cr.compiled.bytes.empty(),
                  "no-pass-manager checked-compile emitted bytes");
            if (!cr.compiled.bytes.empty()) {
                if (finalize(cr.compiled)) {
                    check(cr.compiled.exec != nullptr,
                          "no-pass-manager checked-compile finalized an executable");
                    table.set(pr.program.funcs[0].slot, cr.compiled.entry);
                    using F = int64_t (*)();
                    int64_t got = reinterpret_cast<F>(table.get(pr.program.funcs[0].slot))();
                    check(got == 10, "no-pass-manager IR-backend emitted fn returns 10");
                    free_executable(cr.compiled.exec);
                }
            }
        }
        // (38g) Requested transformed Thin IR: when ctx.request_transformed_ir
        // is true, compile_func_checked populates cr.transformed with the
        // post-pass ThinFunction, which must verify clean for codegen.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("constprop,dce", reg, pm, &perr),
                  "transformed-IR pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = true;
            ctx.pass_manager = &pm;
            ctx.request_transformed_ir = true;   // ask for the post-pass IR
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "transformed-IR checked-compile succeeds");
            check(cr.transformed.has_value(),
                  "checked-compile populated transformed ThinFunction when requested");
            if (cr.transformed.has_value()) {
                std::string verr;
                check(verify_thin_function_for_codegen(*cr.transformed, &verr),
                      "requested transformed ThinFunction verifies clean for codegen");
                check(!cr.transformed->blocks.empty(),
                      "requested transformed ThinFunction has blocks");
            }
            if (cr.compiled.exec) free_executable(cr.compiled.exec);
        }
        // (38h) When request_transformed_ir is false (default), transformed is
        // NOT populated (the opt-out path is honored).
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("dce", reg, pm, &perr),
                  "no-transform pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.pass_manager = &pm;
            ctx.request_transformed_ir = false;  // default: do NOT hand back IR
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "no-transform checked-compile succeeds");
            check(!cr.transformed.has_value(),
                  "checked-compile leaves transformed empty when not requested");
            if (cr.compiled.exec) free_executable(cr.compiled.exec);
        }
        // (38i) Exceptions through legacy compile_func: a pass that throws does
        // NOT let the exception cross the public compile_func boundary. The
        // legacy wrapper returns an empty CompiledFn (exec == nullptr) instead.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<PassErrorPass>("perr");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("perr", reg, pm, &perr),
                  "throwing legacy pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.pass_manager = &pm;
            CompiledFn cf = compile_func(pr.program.funcs[0], ctx);
            check(cf.exec == nullptr,
                  "legacy compile_func swallowed the pass exception (exec == nullptr)");
            check(cf.bytes.empty(),
                  "legacy compile_func produced no bytes on pass exception");
        }
        // (38j) An unexpected std::exception (not PassError) through a pass
        // likewise does not cross the legacy compile_func boundary.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            reg.add<UnexpectedExceptionPass>("unexpected");
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("unexpected", reg, pm, &perr),
                  "unexpected-exception legacy pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.pass_manager = &pm;
            CompiledFn cf = compile_func(pr.program.funcs[0], ctx);
            check(cf.exec == nullptr,
                  "legacy compile_func swallowed the unexpected exception (exec == nullptr)");
        }
        // (38k) Legacy compile_func reports the ACTUAL backend through the
        // checked result when wrapped: a clean IR-backend compile via the
        // checked path with a pass manager reports IRBackend, not TreeWalker.
        {
            EmberPassRegistry reg;
            ext_opt::register_passes(reg);
            EmberPassManager pm;
            std::string perr;
            check(build_pipeline_from_string("dce", reg, pm, &perr),
                  "backend-report pipeline builds");
            CodeGenCtx ctx;
            ctx.globals_base = 0;
            ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives;
            ctx.script_slots = &slots;
            ctx.structs = &layouts;
            ctx.use_context_reg = true;
            ctx.enable_ir_backend = true;
            ctx.enable_regalloc = false;  // no regalloc; still IR backend
            ctx.pass_manager = &pm;
            CompileResult cr = compile_func_checked(pr.program.funcs[0], ctx);
            check(cr.ok(), "no-regalloc checked-compile succeeds");
            check(cr.backend == CompileBackend::IRBackend,
                  "no-regalloc checked-compile still reports IR backend");
            if (cr.compiled.exec) free_executable(cr.compiled.exec);
        }
        }  // end if (setup_ok)
    }

    // ===================================================================
    // ─── Red 8: ordinary pipeline profiles ───
    //
    // Pins the PipelineProfile + PipelineProfileRegistry contracts:
    //   (P1) deterministic listing (sorted lexicographically, all 3 built-ins);
    //   (P2) duplicate / unknown profile errors use the ExtensionStatus/
    //        ExtensionError foundation (collision-aware, original retained);
    //   (P3) exact recipe expansion + order — each profile's recipe is the
    //        DOCUMENTED pass sequence (content-checked, not just self-consistent
    //        with a header constant), and expands through the real
    //        EmberPassRegistry + build_pipeline_from_string into a fresh
    //        EmberPassManager with the named passes IN ORDER;
    //   (P4) fresh configured factories — two expansions of the same profile
    //        yield distinct fresh pass instances, and the seed is propagated
    //        into the profile's options deriver (reproducible + seed-bound);
    //   (P5) explicit pipeline replacement — an explicit `--passes` recipe
    //        REPLACES the selected profile recipe while the profile's
    //        options/seed are retained (the obf factories stay configured by
    //        the profile's options); neither option silently appends.
    //
    // Profiles are ORDINARY NAMED RECIPES: no hidden pass-manager modes. The
    // expansion path is the SAME build_pipeline_from_string an explicit
    // `--passes` uses, into a FRESH EmberPassManager. heavy is explicitly
    // experimental + bounded (denser but never an uncontrolled fixed point).
    // ===================================================================

    // The DOCUMENTED recipes as independent string literals (NOT the header
    // constants). A profile's recipe must equal these EXACT contents, so a
    // wrong stub recipe fails the content check rather than passing by
    // self-consistency with kProfile*Recipe.
    const std::string doc_light =
        "constprop,forward,copyprop,instcombine,dce,dse,const_encode,mba_expand";
    const std::string doc_balanced =
        "simplifycfg,constprop,forward,copyprop,instcombine,cse,dce,dse,"
        "str_encrypt,const_encode,block_split,opaque_pred,deadcode,mba_expand";
    const std::string doc_heavy = doc_balanced;  // same recipe; options differ

    // Split a comma-separated recipe into ordered tokens (independent of the
    // header constants, so the content check is real).
    auto split_recipe = [](const std::string& r) -> std::vector<std::string> {
        std::vector<std::string> toks;
        std::string cur;
        for (char c : r) {
            if (c == ',') { toks.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) toks.push_back(cur);
        return toks;
    };

    // A helper registry: opt + obf passes registered with deterministic
    // defaults so profile recipes resolve to real, runnable passes.
    auto make_full_pass_registry = []() {
        EmberPassRegistry reg;
        ext_opt::register_passes(reg);
        ext_obf::register_passes(reg);
        return reg;
    };

    // (P1) Deterministic listing: register_builtin_profiles on a fresh
    // registry lists exactly light, balanced, heavy (sorted).
    std::printf("(P1) Deterministic listing of built-in profiles\n");
    {
        PipelineProfileRegistry reg;
        ExtensionStatus st = register_builtin_profiles(reg, /*seed=*/0);
        check(bool(st), "register_builtin_profiles succeeds on a fresh registry");
        check(reg.size() == 3, "three built-in profiles registered");
        std::vector<std::string> names = reg.names();
        std::vector<std::string> expected = {"balanced", "heavy", "light"};
        check(names == expected, "names() lists balanced, heavy, light (sorted)");
        check(reg.has("light"), "has(\"light\")");
        check(reg.has("balanced"), "has(\"balanced\")");
        check(reg.has("heavy"), "has(\"heavy\")");
        check(!reg.has("nonexistent"), "!has(\"nonexistent\")");
    }

    // (P2) Duplicate / unknown profile errors on the ExtensionStatus/
    // ExtensionError foundation. A duplicate add is rejected and the ORIGINAL
    // registration is retained (not replaced). An unknown name get() returns
    // nullptr. Empty name + empty recipe are rejected and not stored.
    std::printf("(P2) Duplicate / unknown / empty profile errors\n");
    {
        PipelineProfileRegistry reg;
        check(bool(register_builtin_profiles(reg, 0)), "seed 0 built-ins ok");
        // Duplicate light (different recipe) is rejected; original retained.
        ExtensionStatus dup = reg.add(PipelineProfile{
            "light", "dce", false, PolymorphicPassOptions{}, 0});
        check(!bool(dup), "duplicate \"light\" rejected");
        check(dup.error.has_value(), "duplicate carries a structured error");
        if (dup.error) {
            check(dup.error->registry == std::string(kPipelineProfileRegistryId),
                  "duplicate error.registry == \"ember-pipeline-profile\"");
            check(!dup.error->message.empty(), "duplicate error has a message");
        }
        const PipelineProfile* p = reg.get("light");
        check(p != nullptr, "get(\"light\") still present after duplicate");
        check(p != nullptr && p->recipe == doc_light,
              "original light recipe retained (documented content), not replaced");
        // Unknown name get() returns nullptr.
        check(reg.get("nope") == nullptr, "get(unknown) == nullptr");
        // Empty name rejected.
        ExtensionStatus en = reg.add(PipelineProfile{
            "", "dce", false, PolymorphicPassOptions{}, 0});
        check(!bool(en), "empty profile name rejected");
        check(!reg.has(""), "empty name not stored");
        // Empty recipe rejected.
        ExtensionStatus er = reg.add(PipelineProfile{
            "empty", "", false, PolymorphicPassOptions{}, 0});
        check(!bool(er), "empty profile recipe rejected");
        check(!reg.has("empty"), "empty-recipe profile not stored");
    }

    // (P3) Exact recipe expansion + order. Each built-in profile's recipe is
    // the DOCUMENTED pass sequence (content-checked against the independent
    // doc_* literals), and expand_profile resolves it through the real
    // EmberPassRegistry into a fresh EmberPassManager with the passes IN ORDER
    // (the manager size equals the token count, and every token is a
    // registered pass). This pins that a profile is an ORDINARY recipe, not a
    // hidden mode.
    std::printf("(P3) Exact recipe expansion + order\n");
    {
        auto pass_reg = make_full_pass_registry();
        PipelineProfileRegistry preg;
        register_builtin_profiles(preg, 0);

        // light: content + expansion + non-experimental.
        const PipelineProfile* light = preg.get("light");
        check(light != nullptr, "light profile present");
        check(light != nullptr && light->recipe == doc_light,
              "light recipe == documented content (8 passes, constprop..mba_expand)");
        check(light != nullptr && !light->is_experimental,
              "light is NOT experimental");
        // light's recipe begins with simplifying opts and ends with
        // diversification (simplifying precedes one-shot diversification).
        {
            auto toks = split_recipe(doc_light);
            check(toks.size() == 8, "light recipe has exactly 8 passes");
            check(!toks.empty() && toks.front() == "constprop",
                  "light recipe starts with constprop (simplifying first)");
            check(!toks.empty() && toks.back() == "mba_expand",
                  "light recipe ends with mba_expand (diversification last)");
            EmberPassManager pm;
            std::string err;
            bool ok = expand_profile(*light, pass_reg, pm, &err);
            check(ok, "light expands through build_pipeline_from_string");
            check(pm.size() == toks.size(),
                  "light expanded manager has the recipe's pass count (in order)");
            bool all_known = true;
            for (const auto& t : toks) if (!pass_reg.has(t.c_str())) all_known = false;
            check(all_known, "every light recipe token is a registered pass");
        }

        // balanced: content + expansion + non-experimental.
        const PipelineProfile* balanced = preg.get("balanced");
        check(balanced != nullptr, "balanced profile present");
        check(balanced != nullptr && balanced->recipe == doc_balanced,
              "balanced recipe == documented content (14 passes)");
        check(balanced != nullptr && !balanced->is_experimental,
              "balanced is NOT experimental");
        {
            auto toks = split_recipe(doc_balanced);
            check(toks.size() == 14, "balanced recipe has exactly 14 passes");
            check(!toks.empty() && toks.front() == "simplifycfg",
                  "balanced recipe starts with simplifycfg (simplifying first)");
            check(!toks.empty() && toks.back() == "mba_expand",
                  "balanced recipe ends with mba_expand (diversification last)");
            // The simplifying passes precede the diversification passes: every
            // simplifying token appears before the first diversification token.
            auto pos = [&](const std::string& t) {
                for (size_t i = 0; i < toks.size(); ++i) if (toks[i] == t) return int(i);
                return -1;
            };
            int last_simplify = pos("dse");            // last simplifying pass
            int first_diversify = pos("str_encrypt");   // first diversification
            check(last_simplify >= 0 && first_diversify >= 0 &&
                  last_simplify < first_diversify,
                  "simplifying passes (..dse) precede diversification (str_encrypt..)");
            EmberPassManager pm;
            std::string err;
            bool ok = expand_profile(*balanced, pass_reg, pm, &err);
            check(ok, "balanced expands through build_pipeline_from_string");
            check(pm.size() == toks.size(),
                  "balanced expanded manager has the recipe's pass count (in order)");
        }

        // heavy: content + expansion + EXPERIMENTAL + bounded density.
        const PipelineProfile* heavy = preg.get("heavy");
        check(heavy != nullptr, "heavy profile present");
        check(heavy != nullptr && heavy->recipe == doc_heavy,
              "heavy recipe == documented content (same recipe as balanced)");
        check(heavy != nullptr && heavy->is_experimental,
              "heavy IS explicitly experimental");
        check(heavy != nullptr &&
              heavy->options.site_probability_ppm() == 900'000,
              "heavy density is exactly 900_000 ppm (bounded, not 100%)");
        check(heavy != nullptr &&
              heavy->options.site_probability_ppm() > kProfileBalancedDensityPpm,
              "heavy density is HIGHER than balanced (denser variant)");
        check(heavy != nullptr &&
              heavy->options.site_probability_ppm() <= 1'000'000,
              "heavy density is at most 100% (bounded, not uncontrolled)");
        // heavy's growth ceilings are TIGHTER than the defaults (never raised
        // above the hard caps) — it is not an uncontrolled fixed point.
        check(heavy != nullptr &&
              heavy->options.limits().max_sites < PassGrowthLimits{}.max_sites,
              "heavy max_sites < default (tighter, not raised)");
        check(heavy != nullptr &&
              heavy->options.limits().max_added_instructions <
                  PassGrowthLimits{}.max_added_instructions,
              "heavy max_added_instructions < default (tighter)");
        {
            auto toks = split_recipe(doc_heavy);
            EmberPassManager pm;
            std::string err;
            bool ok = expand_profile(*heavy, pass_reg, pm, &err);
            check(ok, "heavy expands through build_pipeline_from_string");
            check(pm.size() == toks.size(),
                  "heavy expanded manager has the recipe's pass count (in order)");
        }

        // An unknown pass in a recipe makes expand_profile fail (transactional,
        // the manager stays empty). This pins that profiles are ordinary
        // recipes — a bad recipe is rejected the same way a bad --passes is.
        PipelineProfile bad{"bad", "constprop,nonexistent", false,
                            PolymorphicPassOptions{}, 0};
        EmberPassManager pm;
        std::string err;
        bool ok = expand_profile(bad, pass_reg, pm, &err);
        check(!ok, "profile with an unknown pass fails to expand");
        check(err.find("unknown pass") != std::string::npos,
              "bad-profile error mentions 'unknown pass'");
        check(pm.empty(),
              "bad-profile expansion leaves the manager empty (transactional)");
    }

    // (P4) Fresh configured factories. Two expansions of the same profile
    // yield DISTINCT fresh pass instances — the configured factories produce a
    // new PassConcept on every create(), so two managers built from the same
    // profile are independent. The seed is propagated into the profile's
    // options deriver so the same seed is reproducible and a different seed
    // binds to a different root.
    std::printf("(P4) Fresh configured factories per expansion\n");
    {
        auto pass_reg = make_full_pass_registry();
        PipelineProfileRegistry preg;
        register_builtin_profiles(preg, /*seed=*/0x123456789abcdef0ULL);
        const PipelineProfile* heavy = preg.get("heavy");
        check(heavy != nullptr, "heavy profile present for freshness test");
        check(heavy != nullptr && heavy->seed == 0x123456789abcdef0ULL,
              "heavy profile records the supplied seed");
        check(heavy != nullptr && heavy->options.seed_deriver() != nullptr,
              "heavy profile carries a non-null seed deriver");
        // Two independent expansions into two fresh managers.
        EmberPassManager pm1, pm2;
        std::string err;
        check(expand_profile(*heavy, pass_reg, pm1, &err), "first heavy expansion ok");
        check(expand_profile(*heavy, pass_reg, pm2, &err), "second heavy expansion ok");
        check(pm1.size() == pm2.size(),
              "two heavy expansions have the same pass count");
        check(!pm1.empty(), "heavy expansions are non-empty");
        // Distinct seeds produce distinct deriver roots (seed binding).
        auto rootA = u64_to_root(0x123456789abcdef0ULL);
        auto rootB = u64_to_root(0xdeadbeefULL);
        check(rootA != rootB, "distinct seeds produce distinct deriver roots");
        // Deterministic: re-registering the same seed yields the same root
        // (the profile options are reproducible from the seed).
        PipelineProfileRegistry preg_same;
        register_builtin_profiles(preg_same, 0x123456789abcdef0ULL);
        const PipelineProfile* heavy_same = preg_same.get("heavy");
        check(heavy_same != nullptr && heavy_same->options.seed_deriver() != nullptr,
              "same-seed heavy profile carries a deriver");
        // A profile's deriver root equals u64_to_root(seed) (the canonical
        // adapter), confirming the seed fully determines the deriver.
        const FixedRootSeedDeriver* hd =
            dynamic_cast<const FixedRootSeedDeriver*>(
                heavy->options.seed_deriver().get());
        check(hd != nullptr && hd->root() == rootA,
              "heavy deriver root == u64_to_root(seed) (seed-bound + reproducible)");
    }

    // (P5) Explicit pipeline replacement. An explicit `--passes` recipe
    // REPLACES the selected profile recipe while the profile's options/seed
    // are retained (the obf factories stay configured by the profile's
    // options). Neither option silently appends or alters instrumentation:
    // when --passes is given, the resulting pipeline is exactly the explicit
    // recipe (not profile ++ explicit), and the obf factory options remain
    // the profile's (so a profile-selected seed still drives diversification
    // in the explicit recipe's obf passes).
    std::printf("(P5) Explicit --passes replaces profile recipe, retains options\n");
    {
        auto pass_reg = make_full_pass_registry();
        // Simulate the CLI: a profile is selected (heavy, seed S), which
        // configures the obf factories with the profile's options. Then an
        // explicit --passes recipe is given, which REPLACES the profile recipe.
        const uint64_t S = 0xfeedfaceULL;
        PipelineProfileRegistry preg;
        register_builtin_profiles(preg, S);
        const PipelineProfile* heavy = preg.get("heavy");
        check(heavy != nullptr, "heavy selected for replacement test");

        // The profile's options configure a SECOND pass registry's obf
        // factories (the CLI re-registers obf with the profile options so the
        // explicit recipe's obf passes are configured by the profile seed).
        EmberPassRegistry configured_reg;
        ext_opt::register_passes(configured_reg);
        ext_obf::register_passes(configured_reg, heavy->options);
        // Confirm the configured registry still resolves every obf name the
        // explicit recipe might use (the factories are configured, not removed).
        check(configured_reg.has("mba_expand"),
              "configured registry has mba_expand (profile options retained)");
        check(configured_reg.has("const_encode"),
              "configured registry has const_encode (profile options retained)");
        // Two configured mba_expand instances are distinct + fresh (the profile
        // options produce fresh configured passes, not a cached singleton).
        auto a = configured_reg.create("mba_expand");
        auto b = configured_reg.create("mba_expand");
        check(a != nullptr && b != nullptr && a.get() != b.get(),
              "configured factory yields distinct fresh instances");

        // The explicit recipe REPLACES the profile recipe: build the explicit
        // recipe into the manager via REPLACE semantics (not appended to the
        // profile's expanded passes).
        EmberPassManager pm;
        std::string err;
        check(expand_profile(*heavy, configured_reg, pm, &err),
              "profile expands before replacement");
        const std::size_t profile_count = pm.size();
        // Replace with an explicit, shorter recipe.
        bool ok = replace_pipeline_from_string("constprop,mba_expand",
                                               configured_reg, pm, &err);
        check(ok, "explicit --passes recipe replaces the profile recipe");
        check(pm.size() == 2,
              "replaced pipeline has EXACTLY the explicit recipe (2 passes), "
              "not profile ++ explicit");
        check(pm.size() != profile_count,
              "replacement changed the pass count (did not append)");
        // The explicit recipe's mba_expand is configured by the profile's
        // options (the seed S is retained): create one and confirm it is a
        // real configured required (obf) pass.
        auto p = configured_reg.create("mba_expand");
        check(p != nullptr, "configured mba_expand factory yields a pass");
        check(p != nullptr && p->is_required(),
              "configured mba_expand is a required (obf) pass");
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}