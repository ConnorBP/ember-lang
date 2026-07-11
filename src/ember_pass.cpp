// ember_pass.cpp — Stage C: the pass manager run() implementation.
// See ember_pass.hpp for the design.

#include "ember_pass.hpp"

#include <cstdio>

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
    EmberPreserved overall = EmberPreserved::all();
    for (unsigned round = 0; round < max_rounds; ++round) {
        EmberPreserved round_result = run(f, am);
        overall.intersect(round_result);
        // If every pass in this round preserved everything, the pipeline has
        // converged — no pass changed anything.
        if (round_result.all_preserved()) break;
    }
    return overall;
}

} // namespace ember
