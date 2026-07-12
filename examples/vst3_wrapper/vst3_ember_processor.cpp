#include "vst3_ember_processor.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstparameters.h"

#include "codegen.hpp"
#include "dispatch_table.hpp"
#include "engine.hpp"
#include "binding_builder.hpp"
#include "ext_audio.hpp"
#include "jit_memory.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace EmberVst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

const FUID kProcessorUID(0xE34B51D2, 0xA44148B8, 0x93D690AD, 0x036CEBD1);

namespace {

using ProcessFn = void (*)(int64_t, int64_t);

std::string readTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    if (file) text << file.rdbuf();
    return text.str();
}

std::string scriptPath() {
    if (const char* configured = std::getenv("EMBER_VST3_SCRIPT")) {
        if (*configured) return configured;
    }
#ifdef EMBER_VST3_DEFAULT_SCRIPT
    return EMBER_VST3_DEFAULT_SCRIPT;
#else
    return "gain_vst.ember";
#endif
}

} // namespace

// All allocations, parsing, semantic checks and executable-page creation happen
// in initialize(), never on the audio thread. The dispatch table and JIT pages
// remain immutable while the component is active.
class EmberModule {
public:
    ~EmberModule() {
        for (auto& fn : functions) {
            if (fn.exec) ember::free_executable(fn.exec);
        }
    }

    bool compile(const std::string& path, std::string& error) {
        const std::string source = readTextFile(path);
        if (source.empty()) {
            error = "cannot read or empty script: " + path;
            return false;
        }

        auto lexed = ember::tokenize(source, path.c_str());
        if (!lexed.ok) {
            error = "lex error: " + lexed.error;
            return false;
        }
        auto parsed = ember::parse(std::move(lexed.toks));
        if (!parsed.ok) {
            error = "parse error: " + parsed.error;
            return false;
        }
        program = std::move(parsed.program);

        int nextSlot = 0;
        for (auto& fn : program.funcs) {
            fn.slot = nextSlot;
            slots[fn.name] = nextSlot++;
        }
        if (slots.find("process") == slots.end()) {
            error = "script does not define process";
            return false;
        }

        ember::ext_audio::register_natives(natives);
        layouts = ember::build_struct_layouts(program);
        program.string_xor_key = 0;
        auto checked = ember::sema(program, natives, slots, ember::PERM_FFI, nullptr, &layouts);
        if (!checked.ok) {
            std::ostringstream message;
            message << "semantic validation failed";
            for (const auto& item : checked.errors)
                message << "\n  " << item.line << ':' << item.col << ": " << item.msg;
            error = message.str();
            return false;
        }

        const auto globalsLayout = ember::build_struct_layouts(program);
        (void)globalsLayout;
        globalStorage.assign(program.globals.size() * 8, 0);
        globals.base = reinterpret_cast<int64_t>(globalStorage.data());
        uint32_t globalIndex = 0;
        for (auto& global : program.globals) {
            globals.index[global.name] = globalIndex++;
            globals.offsets[global.name] = int32_t((globalIndex - 1) * 8);
            globals.sizes[global.name] = 8;
            globals.types[global.name] = global.ty.get();
        }

        dispatch = std::make_unique<ember::DispatchTable>(program.funcs.size());
        ember::CodeGenCtx context;
        context.globals_base = globals.base;
        context.globals_index = &globals.index;
        context.globals_offsets = &globals.offsets;
        context.globals_types = &globals.types;
        context.dispatch_base = reinterpret_cast<int64_t>(dispatch->base());
        context.natives = &natives;
        context.script_slots = &slots;
        context.structs = &layouts;

        functions.reserve(program.funcs.size());
        for (auto& fn : program.funcs) {
            ember::CompiledFn compiled = ember::compile_func(fn, context);
            if (!ember::finalize(compiled)) {
                error = "executable allocation failed for " + fn.name;
                return false;
            }
            dispatch->set(fn.slot, compiled.entry);
            functions.push_back(std::move(compiled));
        }
        process = reinterpret_cast<ProcessFn>(dispatch->get(slots.at("process")));
        if (!process) {
            error = "compiled process entry is null";
            return false;
        }
        return true;
    }

    ember::Program program;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::StructLayoutTable layouts;
    ember::GlobalsBlock globals;
    std::vector<uint8_t> globalStorage;
    std::unique_ptr<ember::DispatchTable> dispatch;
    std::vector<ember::CompiledFn> functions;
    ProcessFn process {nullptr};
};

EmberProcessor::EmberProcessor() = default;
EmberProcessor::~EmberProcessor() = default;

FUnknown* EmberProcessor::createInstance(void*) {
    return static_cast<IAudioProcessor*>(new EmberProcessor);
}

tresult PLUGIN_API EmberProcessor::initialize(FUnknown* context) {
    const tresult result = SingleComponentEffect::initialize(context);
    if (result != kResultOk) return result;

    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    processContextRequirements.needTransportState().needTempo().needProjectTimeMusic();
    parameters.addParameter(new RangeParameter(
        STR16("Gain"), kGainParamId, nullptr, 0.0, 2.0, 1.0, 0,
        ParameterInfo::kCanAutomate));

    if (!loadEmberScript()) {
        SingleComponentEffect::terminate();
        return kResultFalse;
    }
    return kResultOk;
}

bool EmberProcessor::loadEmberScript() {
    auto candidate = std::make_unique<EmberModule>();
    std::string error;
    const std::string path = scriptPath();
    if (!candidate->compile(path, error)) {
        std::fprintf(stderr, "[Ember VST3] %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    module_ = std::move(candidate);
    return true;
}

tresult PLUGIN_API EmberProcessor::terminate() {
    active_ = false;
    module_.reset();
    return SingleComponentEffect::terminate();
}

tresult PLUGIN_API EmberProcessor::setActive(TBool state) {
    // This gain plug-in needs no scratch memory. The active transition is the
    // allocation boundary for future DSP state; process() itself never grows it.
    active_ = state != 0;
    return SingleComponentEffect::setActive(state);
}

tresult PLUGIN_API EmberProcessor::setProcessing(TBool) {
    return kResultOk;
}

tresult PLUGIN_API EmberProcessor::setupProcessing(ProcessSetup& setup) {
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API EmberProcessor::setBusArrangements(
    SpeakerArrangement* inputs, int32 numInputs,
    SpeakerArrangement* outputs, int32 numOutputs) {
    if (!inputs || !outputs || numInputs != 1 || numOutputs != 1 ||
        inputs[0] != SpeakerArr::kStereo || outputs[0] != SpeakerArr::kStereo)
        return kResultFalse;
    return SingleComponentEffect::setBusArrangements(inputs, numInputs, outputs, numOutputs);
}

tresult PLUGIN_API EmberProcessor::canProcessSampleSize(int32 sampleSize) {
    return sampleSize == kSample32 ? kResultTrue : kResultFalse;
}

void EmberProcessor::bypass(ProcessData& data) const noexcept {
    if (data.numInputs < 1 || data.numOutputs < 1 || !data.inputs || !data.outputs) return;
    const int32 channels = std::min<int32>(2, std::min(data.inputs[0].numChannels,
                                                       data.outputs[0].numChannels));
    for (int32 channel = 0; channel < channels; ++channel) {
        float* input = data.inputs[0].channelBuffers32[channel];
        float* output = data.outputs[0].channelBuffers32[channel];
        if (input && output && input != output && data.numSamples > 0)
            std::memcpy(output, input, static_cast<size_t>(data.numSamples) * sizeof(float));
    }
    data.outputs[0].silenceFlags = data.inputs[0].silenceFlags;
}

tresult PLUGIN_API EmberProcessor::process(ProcessData& data) {
    // Flatten all valid VST parameter points into fixed-capacity host storage.
    // Insertion keeps the queue globally ordered by sample offset, which lets
    // Ember consume every parameter's events in one allocation-free pass.
    std::size_t changeCount = 0;
    if (data.inputParameterChanges) {
        const int32 queueCount = data.inputParameterChanges->getParameterCount();
        for (int32 queueIndex = 0; queueIndex < queueCount; ++queueIndex) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(queueIndex);
            if (!queue) continue;
            const ParamID id = queue->getParameterId();
            if (id >= parameter_values_.size()) continue;

            for (int32 pointIndex = 0;
                 pointIndex < queue->getPointCount() && changeCount < parameter_changes_.size();
                 ++pointIndex) {
                int32 sampleOffset = 0;
                ParamValue normalized = 0.0;
                if (queue->getPoint(pointIndex, sampleOffset, normalized) != kResultTrue)
                    continue;
                const float value =
                    static_cast<float>(std::clamp(normalized, 0.0, 1.0) * 2.0);
                // VST3 permits a zero-sample parameter-flush call. Preserve
                // its final value even though there is no sample event to emit.
                if (data.numSamples <= 0) {
                    parameter_values_[id] = value;
                    continue;
                }
                if (sampleOffset < 0 || sampleOffset >= data.numSamples) continue;

                ember::ext_audio::ParameterChange change;
                change.param_id = static_cast<int64_t>(id);
                change.sample_offset = sampleOffset;
                // Gain's VST normalized [0, 1] range maps to Ember's [0, 2].
                change.value = value;
                std::size_t insert = changeCount;
                while (insert > 0 &&
                       parameter_changes_[insert - 1].sample_offset > change.sample_offset) {
                    parameter_changes_[insert] = parameter_changes_[insert - 1];
                    --insert;
                }
                parameter_changes_[insert] = change;
                ++changeCount;
            }
        }
    }

    if (data.numSamples <= 0) return kResultOk;
    if (!active_ || !module_ || !module_->process || data.symbolicSampleSize != kSample32 ||
        data.numInputs != 1 || data.numOutputs != 1 || !data.inputs || !data.outputs ||
        data.inputs[0].numChannels != 2 || data.outputs[0].numChannels != 2 ||
        !data.inputs[0].channelBuffers32 || !data.outputs[0].channelBuffers32) {
        bypass(data);
        return kResultOk;
    }

    for (int32 channel = 0; channel < data.inputs[0].numChannels; ++channel) {
        if (!data.inputs[0].channelBuffers32[channel] ||
            !data.outputs[0].channelBuffers32[channel]) {
            bypass(data);
            return kResultOk;
        }
    }

    const bool blockStartsSilent = parameter_values_[0] == 0.0f;
    bool blockRemainsSilent = blockStartsSilent;
    for (std::size_t index = 0; index < changeCount; ++index)
        blockRemainsSilent = blockRemainsSilent && parameter_changes_[index].value == 0.0f;

    ember::ext_audio::AudioContext context;
    context.sample_rate = static_cast<int64_t>(processSetup.sampleRate);
    context.block_size = data.numSamples;
    context.num_input_channels = data.inputs[0].numChannels;
    context.num_output_channels = data.outputs[0].numChannels;
    context.input_buffer_ptr = reinterpret_cast<int64_t>(data.inputs[0].channelBuffers32);
    context.output_buffer_ptr = reinterpret_cast<int64_t>(data.outputs[0].channelBuffers32);
    if (data.processContext) {
        context.transport_playing =
            (data.processContext->state & ProcessContext::kPlaying) != 0 ? 1 : 0;
        if ((data.processContext->state & ProcessContext::kTempoValid) != 0)
            context.transport_bpm = data.processContext->tempo;
        if ((data.processContext->state & ProcessContext::kProjectTimeMusicValid) != 0)
            context.transport_ppq = data.processContext->projectTimeMusic;
    }
    context.parameter_count = static_cast<int64_t>(parameter_values_.size());
    context.parameter_values_ptr = reinterpret_cast<int64_t>(parameter_values_.data());
    context.parameter_change_count = static_cast<int64_t>(changeCount);
    context.parameter_changes_ptr = reinterpret_cast<int64_t>(parameter_changes_.data());

    module_->process(reinterpret_cast<int64_t>(&context), data.numSamples);
    if (changeCount > 0) {
        for (std::size_t index = 0; index < changeCount; ++index) {
            const auto id = parameter_changes_[index].param_id;
            if (id >= 0 && static_cast<std::size_t>(id) < parameter_values_.size())
                parameter_values_[static_cast<std::size_t>(id)] =
                    parameter_changes_[index].value;
        }
    }
    data.outputs[0].silenceFlags = blockRemainsSilent
        ? 3u : data.inputs[0].silenceFlags;
    return kResultOk;
}

tresult PLUGIN_API EmberProcessor::setState(IBStream* state) {
    if (!state) return kInvalidArgument;
    IBStreamer stream(state, kLittleEndian);
    float saved = 1.0f;
    if (!stream.readFloat(saved)) return kResultFalse;
    parameter_values_[0] = std::clamp(saved, 0.0f, 2.0f);
    setParamNormalized(kGainParamId, parameter_values_[0] / 2.0f);
    return kResultOk;
}

tresult PLUGIN_API EmberProcessor::getState(IBStream* state) {
    if (!state) return kInvalidArgument;
    IBStreamer stream(state, kLittleEndian);
    return stream.writeFloat(parameter_values_[0]) ? kResultOk : kResultFalse;
}

} // namespace EmberVst3

