// ember_pass.cpp — Stage C: the pass manager run() + checked execution.
// See ember_pass.hpp for the design.

#include "ember_pass.hpp"
#include "thin_ir_ser.hpp"   // verify_thin_function_for_codegen (Red 5)
#include "safety.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace ember {

// Local helper: total instruction count across all blocks, saturating at
// SIZE_MAX on overflow (so a pathological function cannot wrap the growth
// check).
static std::size_t instruction_count(const ThinFunction& fn) {
    std::size_t total = 0;
    for (const auto& block : fn.blocks) {
        if (block.instrs.size() > std::numeric_limits<std::size_t>::max() - total)
            return std::numeric_limits<std::size_t>::max();
        total += block.instrs.size();
    }
    return total;
}

EmberPreserved EmberPassManager::run(ThinFunction& f, EmberAnalysisManager& am) {
    EmberPreserved overall = EmberPreserved::all();
    // Red 5: enforce the HARD growth ceilings in the ordinary one-shot run as
    // well as fixed-point execution. The ordinary run cannot report a
    // structured stop reason (it returns EmberPreserved), so on a ceiling
    // exceed it simply stops running further passes — no library-only stderr
    // (the host that needs the reason uses run_checked). The absolute cap and
    // the 10x-relative cap are both enforced, mirroring run_to_fixpoint.
    const std::size_t initial_size = instruction_count(f);
    const std::size_t growth_limit =
        initial_size > hard_max_ir_instructions / hard_max_ir_growth_factor
            ? hard_max_ir_instructions
            : initial_size * hard_max_ir_growth_factor;
    const std::size_t size_limit = std::min(
        hard_max_ir_instructions,
        std::max<std::size_t>(initial_size, growth_limit));
    for (auto& p : passes_) {
        safety::check_memory_limit();
        if (instruction_count(f) > size_limit) break;  // hard ceiling: stop
        const char* nm = p->name();
        // Required passes bypass the skip gate; optional passes consult
        // instrumentation (a before_pass callback returning false skips).
        bool should_run = p->is_required() ||
                          instrumentation.run_before_pass(nm, f);
        if (!should_run) continue;
        EmberPreserved preserved = p->run(f, am);
        instrumentation.run_after_pass(nm, f, preserved);
        overall.intersect(preserved);
        // Re-check after the pass: a pass that blew up the IR stops the run.
        if (instruction_count(f) > size_limit) break;
    }
    return overall;
}

EmberPreserved EmberPassManager::run_to_fixpoint(ThinFunction& f,
                                                 EmberAnalysisManager& am,
                                                 unsigned max_rounds) {
    const std::size_t initial_size = instruction_count(f);
    const std::size_t growth_limit =
        initial_size > hard_max_ir_instructions / hard_max_ir_growth_factor
            ? hard_max_ir_instructions
            : initial_size * hard_max_ir_growth_factor;
    // Enforce whichever ceiling is reached first: the absolute cap or 10x
    // growth relative to the function that entered the fixed-point pipeline.
    const std::size_t size_limit = std::min(
        hard_max_ir_instructions,
        std::max<std::size_t>(initial_size, growth_limit));
    const unsigned round_limit = std::min(max_rounds, hard_max_fixpoint_rounds);

    EmberPreserved overall = EmberPreserved::all();
    for (unsigned round = 0; round < round_limit; ++round) {
        safety::check_memory_limit();
        if (instruction_count(f) > size_limit) break;  // no stderr: just stop

        EmberPreserved round_result = run(f, am);
        overall.intersect(round_result);
        if (instruction_count(f) > size_limit) break;  // no stderr: just stop
        // If every pass in this round preserved everything, the pipeline has
        // converged — no pass changed anything.
        if (round_result.all_preserved()) break;
    }
    return overall;
}

// ─── Red 5: checked execution ───

void EmberPassManager::resolve_ceilings_(const CheckedRunOptions& opts,
                                         std::size_t initial_count,
                                         std::size_t& abs_cap,
                                         std::size_t& growth_cap) const {
    // Absolute instruction cap: caller may request SMALLER than the hard cap,
    // never larger. opts.max_instructions == 0 means "use the hard cap".
    abs_cap = (opts.max_instructions == 0 ||
               opts.max_instructions > hard_max_ir_instructions)
                  ? hard_max_ir_instructions
                  : opts.max_instructions;
    // Growth factor: caller may request a TIGHTER ratio (smaller numerator or
    // larger denominator), never a looser one than the hard 10x. 0 numerator
    // means "use the hard factor".
    uint32_t num = opts.growth_numerator == 0 ? hard_max_ir_growth_factor
                                              : opts.growth_numerator;
    uint32_t den = opts.growth_denominator == 0 ? 1u : opts.growth_denominator;
    // Clamp the requested ratio to the hard factor: num/den <= hard/1, i.e.
    // num <= hard * den. If the request is looser, fall back to the hard ratio.
    if (uint64_t(num) > uint64_t(hard_max_ir_growth_factor) * uint64_t(den)) {
        num = hard_max_ir_growth_factor;
        den = 1;
    }
    // growth_cap = max(initial, initial*num/den), clamped to abs_cap.
    std::size_t grown = initial_count;
    if (den != 0) {
        uint64_t g = (uint64_t(initial_count) * uint64_t(num)) / uint64_t(den);
        if (g > std::numeric_limits<std::size_t>::max())
            g = std::numeric_limits<std::size_t>::max();
        grown = std::max<std::size_t>(initial_count, std::size_t(g));
    }
    growth_cap = std::min(grown, abs_cap);
}

bool EmberPassManager::checked_round_(ThinFunction& f, EmberAnalysisManager& am,
                                       ThinFunction& snapshot,
                                       const CheckedRunOptions& opts,
                                       PassRunReport& rep) {
    const std::size_t initial_count = rep.initial_count;
    std::size_t abs_cap = 0, growth_cap = 0;
    resolve_ceilings_(opts, initial_count, abs_cap, growth_cap);
    // Input validation: reject an already-malformed function before any pass.
    if (opts.validate_input) {
        std::string verr;
        if (!verify_thin_function_for_codegen(f, &verr)) {
            rep.stop_reason = PassStopReason::ValidationFailure;
            rep.pass_name = passes_.empty() ? "" : passes_.front()->name();
            rep.error = "input validation failed: " + verr;
            rep.final_count = instruction_count(f);
            return false;
        }
    }
    bool round_converged = true;
    for (auto& p : passes_) {
        safety::check_memory_limit();
        const char* nm = p->name();
        rep.pass_name = nm;
        bool should_run = p->is_required() ||
                          instrumentation.run_before_pass(nm, f);
        if (!should_run) continue;
        // Run the pass with the snapshot protecting the pre-pass state. A
        // PassError is a recoverable, expected failure; any other
        // std::exception is unexpected.
        EmberPreserved preserved = EmberPreserved::all();
        try {
            preserved = p->run(f, am);
        } catch (const PassError& e) {
            f = snapshot;  // rollback
            rep.stop_reason = PassStopReason::PassError;
            rep.pass_name = e.pass_name.empty() ? nm : e.pass_name;
            rep.error = e.message;
            rep.final_count = instruction_count(f);
            return false;
        } catch (const std::exception& e) {
            f = snapshot;  // rollback
            rep.stop_reason = PassStopReason::ExceptionError;
            rep.pass_name = nm;
            rep.error = e.what();
            rep.final_count = instruction_count(f);
            return false;
        }
        instrumentation.run_after_pass(nm, f, preserved);
        if (!preserved.all_preserved()) round_converged = false;
        // Growth ceiling: a pass that exceeded the instruction/growth cap is
        // rolled back and reported. The final_count in the report reflects the
        // blown-up size (pre-rollback) so the host can see how far over.
        const std::size_t post_count = instruction_count(f);
        if (post_count > growth_cap) {
            rep.stop_reason = PassStopReason::GrowthLimit;
            rep.pass_name = nm;
            rep.error = "instruction/growth ceiling exceeded (" +
                        std::to_string(post_count) + " > " +
                        std::to_string(growth_cap) + ")";
            rep.final_count = post_count;   // pre-rollback count
            f = snapshot;                    // rollback the blowup
            return false;
        }
        // Validation after each REPORTED mutation. A pass that returned all()
        // claimed it changed nothing, so re-validating would only re-check the
        // already-valid pre-pass state (skipped for efficiency). A pass that
        // returned none() claimed a mutation — verify the result.
        if (opts.validate_after_each_mutation && !preserved.all_preserved()) {
            std::string verr;
            if (!verify_thin_function_for_codegen(f, &verr)) {
                rep.stop_reason = PassStopReason::ValidationFailure;
                rep.pass_name = nm;
                rep.error = "pass produced invalid IR: " + verr;
                rep.final_count = instruction_count(f);
                f = snapshot;  // rollback
                return false;
            }
        }
        // Refresh the snapshot so a LATER pass failure rolls back only to the
        // last-verified state, not to the round start. (Each pass is validated
        // in isolation against the running snapshot of the prior good state;
        // on a later failure we restore the last verified function, which is
        // the correct “prior good state” for a transactional pipeline.)
        snapshot = f;
    }
    rep.stop_reason = round_converged ? PassStopReason::Converged
                                      : PassStopReason::Completed;
    rep.final_count = instruction_count(f);
    return true;
}

PassRunReport EmberPassManager::run_checked(ThinFunction& f,
                                            EmberAnalysisManager& am,
                                            const CheckedRunOptions& opts) {
    PassRunReport rep;
    rep.initial_count = instruction_count(f);
    ThinFunction snapshot = f;  // deep copy for rollback
    // One shot: a single round. checked_round_ fills stop_reason/pass_name/
    // error/final_count. For a clean one-shot we report Completed (not
    // Converged) so the caller can distinguish a single run from a fixpoint.
    bool more = checked_round_(f, am, snapshot, opts, rep);
    rep.rounds = 1;
    if (more) {
        // A clean one-shot run: override the round driver's Converged/Completed
        // distinction — one-shot completion is always Completed.
        rep.stop_reason = PassStopReason::Completed;
    }
    return rep;
}

PassRunReport EmberPassManager::run_checked_fixpoint(ThinFunction& f,
                                                     EmberAnalysisManager& am,
                                                     const CheckedRunOptions& opts) {
    PassRunReport rep;
    rep.initial_count = instruction_count(f);
    const unsigned round_limit =
        std::min(opts.max_rounds, hard_max_fixpoint_rounds);
    ThinFunction snapshot = f;
    for (unsigned round = 0; round < round_limit; ++round) {
        safety::check_memory_limit();
        rep.rounds = round + 1;
        PassRunReport round_rep = rep;  // carry initial_count into the round
        bool more = checked_round_(f, am, snapshot, opts, round_rep);
        // Promote the round's outcome into the pipeline report.
        rep.stop_reason = round_rep.stop_reason;
        rep.pass_name = round_rep.pass_name;
        rep.error = round_rep.error;
        rep.final_count = round_rep.final_count;
        if (!more) {
            // Failure (ValidationFailure / GrowthLimit / PassError /
            // ExceptionError): snapshot already restored inside the round.
            return rep;
        }
        // A clean round that converged (no pass changed anything) ends the
        // fixpoint cleanly.
        if (round_rep.stop_reason == PassStopReason::Converged) {
            rep.stop_reason = PassStopReason::Converged;
            return rep;
        }
        // Otherwise the round changed something; refresh the round-start
        // snapshot and run another round.
        snapshot = f;
    }
    // Hit the round limit without converging.
    rep.stop_reason = PassStopReason::RoundLimit;
    return rep;
}

} // namespace ember
