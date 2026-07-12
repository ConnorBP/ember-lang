#pragma once

// A combined VST3 component/controller. SingleComponentEffect supplies the
// IComponent, IAudioProcessor and IEditController plumbing; this class owns
// one Ember JIT module per plug-in instance.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "ext_audio.hpp"
#include "dispatch_table.hpp"
#include "parser.hpp"
#include "hot_reload.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

private:
    bool loadEmberScript();
    void startHotReload();
    void stopHotReload() noexcept;
    void watchScript();
    EmberModule* activatePendingModule(ember::HotReloadDomain::ExecutionGuard&) noexcept;
    void reclaimRetiredModule();
    void refreshLatencyAndTail() noexcept;
    void bypass(Steinberg::Vst::ProcessData& data) const noexcept;

    static constexpr std::size_t kParameterCount = 1;
    static constexpr std::size_t kMaxParameterChanges = 4096;
    static constexpr std::size_t kMaxEvents = 4096;
    static constexpr std::size_t kMaxStateBytes = 16 * 1024 * 1024;

    // process_dispatch_ is the one stable publication point used by the audio
    // thread. A ProcessPlan (EmberModule) owns its immutable JIT code, globals,
    // and private script dispatch table. The watcher only writes pending_; the
    // audio thread performs the release publication at a block boundary.
    ember::HotReloadDomain reload_domain_;
    ember::DispatchTable process_dispatch_ {1};
    std::atomic<EmberModule*> current_ {nullptr};
    std::atomic<EmberModule*> pending_ {nullptr};
    std::atomic<EmberModule*> retired_ {nullptr};

    std::thread watcher_;
    std::atomic<bool> stop_watcher_ {false};
    std::mutex watcher_mutex_;
    std::condition_variable watcher_cv_;
    std::string script_path_;
    std::string watched_source_;

    std::size_t crossfade_samples_ {64};
    std::vector<float> crossfade_storage_;
    std::array<float*, 2> crossfade_channels_ {{nullptr, nullptr}};

    // Kept until the implementation switches initial ownership into current_.
    std::unique_ptr<EmberModule> module_;
    std::array<float, kParameterCount> parameter_values_ {{1.0f}};
    std::array<ember::ext_audio::ParameterChange, kMaxParameterChanges> parameter_changes_ {};
    std::array<ember::ext_audio::AudioEvent, kMaxEvents> input_events_ {};
    std::array<ember::ext_audio::AudioEvent, kMaxEvents> output_events_ {};
    std::atomic<Steinberg::uint32> latency_samples_ {0};
    std::atomic<Steinberg::uint32> tail_samples_ {0};
    bool active_ {false};
};

} // namespace EmberVst3
