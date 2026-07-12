// custom_passes.hpp — declarations shared by the custom-pass examples.
#pragma once

#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"

#include <cstdint>

namespace ember::examples::custom_pass {

struct MinimalPass : EmberPassInfoMixin<MinimalPass> {
    static constexpr const char* pass_name = "example-minimal";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// A fixed seed is part of the pass configuration. EmberPassRegistry::add<T>()
// default-constructs a fresh pass, so the stock registration below uses this
// documented default.
struct NopInjectionPass : EmberPassInfoMixin<NopInjectionPass> {
    static constexpr const char* pass_name = "example-nop-injection";
    uint64_t seed = 0x454D424552ULL; // "EMBER"
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

struct BlockMergePass : EmberPassInfoMixin<BlockMergePass> {
    static constexpr const char* pass_name = "example-block-merge";
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager& am);
};

// Standard extension entry point. A host must call this explicitly; merely
// linking these files does not dynamically discover the passes.
inline void register_passes(EmberPassRegistry& reg) {
    reg.add<MinimalPass>(MinimalPass::pass_name);
    reg.add<NopInjectionPass>(NopInjectionPass::pass_name);
    reg.add<BlockMergePass>(BlockMergePass::pass_name);
}

} // namespace ember::examples::custom_pass
