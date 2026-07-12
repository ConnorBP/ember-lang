// minimal_pass.cpp — the smallest useful Ember pass skeleton.
#include "custom_passes.hpp"

namespace ember::examples::custom_pass {

EmberPreserved MinimalPass::run(ThinFunction& f, EmberAnalysisManager&) {
    // Passes receive one ThinFunction. Blocks own ordinary instructions and a
    // separate terminator. This skeleton deliberately changes neither.
    for (auto& block : f.blocks) {
        for (auto& instruction : block.instrs) {
            (void)instruction;
            // Inspect or transform `instruction` here.
        }
    }

    // all() means the IR was not changed. A mutating pass returns none() with
    // today's all-or-nothing analysis-preservation model.
    return EmberPreserved::all();
}

} // namespace ember::examples::custom_pass
