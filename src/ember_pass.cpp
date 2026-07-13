// ember_pass.cpp — Stage C: the pass manager run() implementation.
// See ember_pass.hpp for the design.

#include "ember_pass.hpp"
#include "safety.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace ember {

EmberPreserved EmberPassManager::run(ThinFunction& f, EmberAnalysisManager& am) {
    EmberPreserved overall = EmberPreserved::all();
    for (auto& p : passes_) {
        const char* nm = p->name();
        // Required passes bypass the skip gate; optional passes consult
        // instrumentation (a before_pass callback returning false skips).
        bool should_run = p->is_required() ||
                          instrumentation.run_before_pass(nm, f);
        if (!should_run) continue;
        EmberPreserved preserved = p->run(f, am);
        instrumentation.run_after_pass(nm, f, preserved);
        overall.intersect(preserved);
    }
    return overall;
}

EmberPreserved EmberPassManager::run_to_fixpoint(ThinFunction& f,
                                                 EmberAnalysisManager& am,
                                                 unsigned max_rounds) {
    auto instruction_count = [](const ThinFunction& fn) -> std::size_t {
        std::size_t total = 0;
        for (const auto& block : fn.blocks) {
            if (block.instrs.size() > std::numeric_limits<std::size_t>::max() - total)
                return std::numeric_limits<std::size_t>::max();
            total += block.instrs.size();
        }
        return total;
    };

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
        if (instruction_count(f) > size_limit) {
            std::fprintf(stderr,
                "ember pass pipeline stopped: IR size cap exceeded before round %u "
                "(limit=%zu instructions)\n",
                round + 1, size_limit);
            break;
        }

        EmberPreserved round_result = run(f, am);
        overall.intersect(round_result);
        if (instruction_count(f) > size_limit) {
            std::fprintf(stderr,
                "ember pass pipeline stopped: IR size cap exceeded after round %u "
                "(limit=%zu instructions)\n",
                round + 1, size_limit);
            break;
        }
        // If every pass in this round preserved everything, the pipeline has
        // converged — no pass changed anything.
        if (round_result.all_preserved()) break;
    }
    return overall;
}

} // namespace ember
