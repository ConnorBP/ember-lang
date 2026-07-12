#pragma once

// A combined VST3 component/controller. SingleComponentEffect supplies the
// IComponent, IAudioProcessor and IEditController plumbing; this class owns
// one Ember JIT module per plug-in instance.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <cstdint>
#include <memory>

namespace EmberVst3 {

inline constexpr Steinberg::Vst::ParamID kGainParamId = 0;
extern const Steinberg::FUID kProcessorUID;

class EmberModule;

class EmberProcessor final : public Steinberg::Vst::SingleComponentEffect {
public:
    EmberProcessor();
    ~EmberProcessor() override;

    static Steinberg::FUnknown* createInstance(void*);

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numInputs,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOutputs) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 sampleSize) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

private:
    bool loadEmberScript();
    void bypass(Steinberg::Vst::ProcessData& data) const noexcept;

    std::unique_ptr<EmberModule> module_;
    float gain_ {1.0f};
    bool active_ {false};
};

} // namespace EmberVst3
