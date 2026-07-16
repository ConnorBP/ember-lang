#pragma once

// A combined VST3 component/controller. SingleComponentEffect supplies the
// IComponent, IAudioProcessor and IEditController plumbing; this class owns
// one Ember JIT module per plug-in instance.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/gui/iplugview.h"

#include "ext_audio.hpp"
#include "parser.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace EmberVst3 {

inline constexpr Steinberg::Vst::ParamID kGainParamId = 0;
inline constexpr Steinberg::Vst::ParamID kParameter1Id = 1;
inline constexpr Steinberg::Vst::ParamID kParameter2Id = 2;
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
    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    std::size_t parameterCount() const noexcept { return plugin_parameter_count_; }
    float parameterValue(std::size_t index) const noexcept;
    float parameterMinimum(std::size_t index) const noexcept;
    float parameterMaximum(std::size_t index) const noexcept;
    const char* parameterName(std::size_t index) const noexcept;
    void setParameterFromEditor(std::size_t index, float value);
    bool renderScriptUi();
    void drawDefaultVisualizations();

private:
    bool loadEmberScript();
    void startHotReload();
    void stopHotReload() noexcept;
    void watchScript();
    std::shared_ptr<EmberModule> activatePendingModule() noexcept;
    void reclaimRetiredModule();
    void refreshLatencyAndTail() noexcept;
    void bypass(Steinberg::Vst::ProcessData& data) const noexcept;

    static constexpr std::size_t kParameterCount = 3;
    static constexpr std::size_t kMaxParameterChanges = 4096;
    static constexpr std::size_t kMaxEvents = 4096;
    static constexpr std::size_t kMaxStateBytes = 16 * 1024 * 1024;

    // A ProcessPlan (EmberModule) owns its immutable JIT code, globals, and
    // private dispatch table. Access these shared_ptrs only through the C++11
    // atomic shared_ptr free functions. Every caller takes an owning snapshot,
    // so a watcher can retire a module without racing an audio-thread crossfade.
    std::shared_ptr<EmberModule> current_;
    std::shared_ptr<EmberModule> pending_;
    std::shared_ptr<EmberModule> retired_;

    std::thread watcher_;
    std::atomic<bool> stop_watcher_ {false};
    std::mutex watcher_mutex_;
    std::condition_variable watcher_cv_;
    std::string script_path_;
    std::string watched_source_;

    std::size_t crossfade_samples_ {64};
    std::vector<float> crossfade_storage_;
    std::array<float*, 2> crossfade_channels_ {{nullptr, nullptr}};

    // Legacy compatibility owner; normal module ownership lives in the atomic
    // shared_ptr snapshots above.
    std::unique_ptr<EmberModule> module_;
    std::array<float, kParameterCount> parameter_values_ {{1.0f, 0.0f, 0.0f}};
    std::array<float, kParameterCount> parameter_minimums_ {{0.0f, 0.0f, 0.0f}};
    std::array<float, kParameterCount> parameter_maximums_ {{2.0f, 1.0f, 1.0f}};
    std::size_t plugin_parameter_count_ {1};
    bool gain_profile_ {true};
    std::array<ember::ext_audio::ParameterChange, kMaxParameterChanges> parameter_changes_ {};
    std::array<ember::ext_audio::AudioEvent, kMaxEvents> input_events_ {};
    std::array<ember::ext_audio::AudioEvent, kMaxEvents> output_events_ {};
    std::atomic<Steinberg::uint32> latency_samples_ {0};
    std::atomic<Steinberg::uint32> tail_samples_ {0};
    bool active_ {false};
};

} // namespace EmberVst3
