#include "vst3_ember_processor.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstparameters.h"

#include "codegen.hpp"
#include "dispatch_table.hpp"
#include "engine.hpp"
#include "binding_builder.hpp"
#include "globals.hpp"
#include "ext_audio.hpp"
#include "ext_math.hpp"
#include "jit_memory.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
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
using SaveStateFn = int64_t (*)();
using LoadStateFn = void (*)(int64_t, int64_t);
using ReportSamplesFn = int64_t (*)();

// save_state() returns a pointer to this two-word descriptor. The bytes remain
// script-owned and only need to stay valid until the wrapper copies them.
struct ScriptStateBuffer {
    int64_t data {0};
    int64_t size {0};
};

constexpr uint32_t kStateMagic = 0x36564245; // "EBV6" little-endian
constexpr int32_t kStateVersion = 1;
constexpr auto kWatchInterval = std::chrono::milliseconds(250);
constexpr std::size_t kDefaultCrossfadeSamples = 64;
constexpr std::size_t kMaxCrossfadeSamples = 4096;

uint32_t hostValueBytes(const ember::Type* type,
                        const ember::StructLayoutTable& structs) {
    if (!type) return 8;
    if (type->is_slice) return 16;
    if (type->array_len > 0)
        return static_cast<uint32_t>(type->array_len) * hostValueBytes(type->elem.get(), structs);
    if (!type->struct_name.empty()) {
        const auto found = structs.find(type->struct_name);
        if (found != structs.end()) return static_cast<uint32_t>(found->second.size);
    }
    return static_cast<uint32_t>(std::max<std::size_t>(type->byte_size(), 1));
}

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

std::size_t crossfadeSamples() {
    const char* configured = std::getenv("EMBER_VST3_CROSSFADE_SAMPLES");
    if (!configured || !*configured) return kDefaultCrossfadeSamples;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(configured, &end, 10);
    if (end == configured || *end != '\0') return kDefaultCrossfadeSamples;
    return static_cast<std::size_t>(std::min<unsigned long long>(
        value, kMaxCrossfadeSamples));
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
        return compileSource(readTextFile(path), path, error);
    }

    bool compileSource(const std::string& source, const std::string& path,
                       std::string& error) {
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
        const auto process32Slot = slots.find("process_f32") != slots.end()
            ? slots.find("process_f32") : slots.find("process");
        if (process32Slot == slots.end()) {
            error = "script does not define process_f32";
            return false;
        }

        auto findFunction = [this](const char* name) -> const ember::FuncDecl* {
            const auto it = std::find_if(program.funcs.begin(), program.funcs.end(),
                [name](const ember::FuncDecl& fn) { return fn.name == name; });
            return it == program.funcs.end() ? nullptr : &*it;
        };
        auto hasSignature = [](const ember::FuncDecl& fn,
                               std::initializer_list<const ember::Type*> params,
                               const ember::Type& result) {
            if (fn.params.size() != params.size() || !fn.ret->same(result)) return false;
            std::size_t index = 0;
            for (const ember::Type* type : params) {
                if (!fn.params[index++].ty->same(*type)) return false;
            }
            return true;
        };
        const char* process32Name = slots.find("process_f32") != slots.end()
            ? "process_f32" : "process";
        const auto* processFn = findFunction(process32Name);
        if (!processFn || !hasSignature(*processFn,
                {&ember::type_i64(), &ember::type_i64()}, ember::type_void())) {
            error = std::string(process32Name) +
                " must have signature fn " + process32Name +
                "(ctx: i64, frames: i64) -> void";
            return false;
        }
        if (const auto* process64Fn = findFunction("process_f64");
            process64Fn && !hasSignature(*process64Fn,
                {&ember::type_i64(), &ember::type_i64()}, ember::type_void())) {
            error = "process_f64 must have signature fn process_f64(ctx: i64, frames: i64) -> void";
            return false;
        }
        if (const auto* saveFn = findFunction("save_state");
            saveFn && !hasSignature(*saveFn, {}, ember::type_i64())) {
            error = "save_state must have signature fn save_state() -> i64";
            return false;
        }
        if (const auto* loadFn = findFunction("load_state");
            loadFn && !hasSignature(*loadFn,
                {&ember::type_i64(), &ember::type_i64()}, ember::type_void())) {
            error = "load_state must have signature fn load_state(ptr: i64, len: i64) -> void";
            return false;
        }
        for (const char* callback : {"get_latency", "get_tail"}) {
            if (const auto* fn = findFunction(callback);
                fn && !hasSignature(*fn, {}, ember::type_i64())) {
                error = std::string(callback) +
                    " must have signature fn " + callback + "() -> i64";
                return false;
            }
        }

        ember::ext_audio::register_natives(natives);
        ember::ext_math::register_natives(natives);
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

        uint32_t globalIndex = 0;
        uint32_t globalBytes = 0;
        for (auto& global : program.globals) {
            globalBytes = (globalBytes + 7u) & ~7u;
            globals.index[global.name] = globalIndex++;
            globals.offsets[global.name] = globalBytes;
            globals.sizes[global.name] = hostValueBytes(global.ty.get(), layouts);
            globals.types[global.name] = global.ty.get();
            globalBytes += globals.sizes[global.name];
        }
        globalStorage.assign(globalBytes, 0);
        globals.base = reinterpret_cast<int64_t>(globalStorage.data());
        ember::GlobalInitCtx initializers {globalStorage, globals.index, globals.types};
        initializers.offsets = &globals.offsets;
        initializers.sizes = &globals.sizes;
        initializers.structs = &layouts;
        ember::eval_global_initializers(program, initializers);

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
        process_f32 = reinterpret_cast<ProcessFn>(dispatch->get(process32Slot->second));
        if (!process_f32) {
            error = "compiled process_f32 entry is null";
            return false;
        }
        const auto process64 = slots.find("process_f64");
        if (process64 != slots.end())
            process_f64 = reinterpret_cast<ProcessFn>(dispatch->get(process64->second));

        const auto save = slots.find("save_state");
        if (save != slots.end())
            save_state = reinterpret_cast<SaveStateFn>(dispatch->get(save->second));
        const auto load = slots.find("load_state");
        if (load != slots.end())
            load_state = reinterpret_cast<LoadStateFn>(dispatch->get(load->second));
        const auto latency = slots.find("get_latency");
        if (latency != slots.end())
            get_latency = reinterpret_cast<ReportSamplesFn>(dispatch->get(latency->second));
        const auto tail = slots.find("get_tail");
        if (tail != slots.end())
            get_tail = reinterpret_cast<ReportSamplesFn>(dispatch->get(tail->second));
        return true;
    }

    void relinquishProcessPage() noexcept {
        for (auto& fn : functions) {
            if (fn.entry == reinterpret_cast<void*>(process_f32)) {
                fn.exec = nullptr;
                fn.entry = nullptr;
                break;
            }
        }
        // The f64 callback has a distinct executable page and is invoked by
        // audio-thread guards without going through process_dispatch_. Retire
        // it with the module object instead of allowing an unguarded delete.
    }

    ember::Program program;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::StructLayoutTable layouts;
    ember::GlobalsBlock globals;
    std::vector<uint8_t> globalStorage;
    std::unique_ptr<ember::DispatchTable> dispatch;
    std::vector<ember::CompiledFn> functions;
    ProcessFn process_f32 {nullptr};
    ProcessFn process_f64 {nullptr};
    SaveStateFn save_state {nullptr};
    LoadStateFn load_state {nullptr};
    ReportSamplesFn get_latency {nullptr};
    ReportSamplesFn get_tail {nullptr};
};

EmberProcessor::EmberProcessor()
    : crossfade_samples_(crossfadeSamples()) {}

EmberProcessor::~EmberProcessor() {
    stopHotReload();
}

FUnknown* EmberProcessor::createInstance(void*) {
    return static_cast<IAudioProcessor*>(new EmberProcessor);
}

tresult PLUGIN_API EmberProcessor::initialize(FUnknown* context) {
    const tresult result = SingleComponentEffect::initialize(context);
    if (result != kResultOk) return result;

    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    addEventInput(STR16("Event In"));
    addEventOutput(STR16("Event Out"));
    processContextRequirements.needTransportState().needTempo().needProjectTimeMusic();
    parameters.addParameter(new RangeParameter(
        STR16("Gain"), kGainParamId, nullptr, 0.0, 2.0, 1.0, 0,
        ParameterInfo::kCanAutomate));

    if (!loadEmberScript()) {
        SingleComponentEffect::terminate();
        return kResultFalse;
    }
    refreshLatencyAndTail();
    startHotReload();
    return kResultOk;
}

bool EmberProcessor::loadEmberScript() {
    auto candidate = std::make_unique<EmberModule>();
    std::string error;
    script_path_ = scriptPath();
    watched_source_ = readTextFile(script_path_);
    if (!candidate->compileSource(watched_source_, script_path_, error)) {
        std::fprintf(stderr, "[Ember VST3] %s: %s\n", script_path_.c_str(), error.c_str());
        return false;
    }
    process_dispatch_.set(0, reinterpret_cast<void*>(candidate->process_f32));
    current_.store(candidate.release(), std::memory_order_release);
    return true;
}

void EmberProcessor::startHotReload() {
    stop_watcher_.store(false, std::memory_order_release);
    try {
        watcher_ = std::thread(&EmberProcessor::watchScript, this);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[Ember VST3] cannot start hot-reload watcher: %s\n", error.what());
    }
}

void EmberProcessor::stopHotReload() noexcept {
    stop_watcher_.store(true, std::memory_order_release);
    watcher_cv_.notify_all();
    if (watcher_.joinable()) watcher_.join();
}

void EmberProcessor::watchScript() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(watcher_mutex_);
            if (watcher_cv_.wait_for(lock, kWatchInterval, [this] {
                    return stop_watcher_.load(std::memory_order_acquire);
                }))
                break;
        }

        reclaimRetiredModule();
        // Keep one immutable candidate and one retired plan at most. This
        // bounds memory while the DAW is stopped or an unusually long block is
        // in flight; the latest file contents are picked up on the next poll.
        if (pending_.load(std::memory_order_acquire) ||
            retired_.load(std::memory_order_acquire))
            continue;

        const std::string source = readTextFile(script_path_);
        if (source == watched_source_) continue;
        // Remember failed content as well. A broken intermediate save emits one
        // diagnostic, remains un-published, and is retried only after another edit.
        watched_source_ = source;

        auto candidate = std::make_unique<EmberModule>();
        std::string error;
        if (!candidate->compileSource(source, script_path_, error)) {
            std::fprintf(stderr,
                         "[Ember VST3] hot reload failed for %s: %s; keeping last known-good module\n",
                         script_path_.c_str(), error.c_str());
            reclaimRetiredModule();
            continue;
        }

        EmberModule* rawCandidate = candidate.release();
        pending_.store(rawCandidate, std::memory_order_release);
        const auto publication = reload_domain_.publish(
            process_dispatch_, 0, reinterpret_cast<void*>(rawCandidate->process_f32));
        if (!publication.ok) {
            EmberModule* expected = rawCandidate;
            pending_.compare_exchange_strong(expected, nullptr,
                                             std::memory_order_acq_rel);
            delete rawCandidate;
            std::fprintf(stderr,
                         "[Ember VST3] hot reload publication failed; keeping last known-good module\n");
            continue;
        }

        // publish() owns the replaced process page now. Disown it immediately
        // so reclamation and the old ProcessPlan destructor cannot double-free.
        if (EmberModule* old = current_.load(std::memory_order_acquire))
            old->relinquishProcessPage();
        std::fprintf(stderr,
                     "[Ember VST3] compiled update for %s; awaiting block boundary (epoch %llu)\n",
                     script_path_.c_str(),
                     static_cast<unsigned long long>(publication.publication_epoch));
    }
    reclaimRetiredModule();
}

EmberModule* EmberProcessor::activatePendingModule(
    ember::HotReloadDomain::ExecutionGuard&) noexcept {
    // Publication and consumption both serialize through the domain mutex.
    // Taking this short-lived nested guard gives us a coherent snapshot of the
    // dispatch slot and pending ProcessPlan without adding another audio lock.
    std::optional<ember::HotReloadDomain::ExecutionGuard> publicationSnapshot;
    publicationSnapshot.emplace(reload_domain_.guard());
    EmberModule* candidate = pending_.load(std::memory_order_acquire);
    if (!candidate || process_dispatch_.get(0) != reinterpret_cast<void*>(candidate->process_f32))
        return nullptr;
    if (!pending_.compare_exchange_strong(candidate, nullptr,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        return nullptr;

    EmberModule* old = current_.load(std::memory_order_acquire);
    int64_t statePointer = 0;
    int64_t stateLength = 0;
    if (old && old->save_state) {
        const auto* state = reinterpret_cast<const ScriptStateBuffer*>(old->save_state());
        if (state && state->data && state->size > 0 &&
            state->size <= static_cast<int64_t>(kMaxStateBytes)) {
            statePointer = state->data;
            stateLength = state->size;
        }
    }
    if (candidate->load_state)
        candidate->load_state(statePointer, stateLength);

    current_.store(candidate, std::memory_order_release);
    const auto boundedReport = [](ReportSamplesFn callback) -> uint32 {
        if (!callback) return 0;
        const int64_t value = callback();
        return value <= 0 ? 0u : static_cast<uint32>(std::min<int64_t>(value, kMaxInt32u));
    };
    latency_samples_.store(boundedReport(candidate->get_latency), std::memory_order_release);
    tail_samples_.store(boundedReport(candidate->get_tail), std::memory_order_release);
    if (old) retired_.store(old, std::memory_order_release);
    std::fprintf(stderr, "[Ember VST3] activated update at block boundary\n");
    return old;
}

void EmberProcessor::reclaimRetiredModule() {
    // reclaim() is nonblocking. The module object can be destroyed only after
    // its retired process page was freed, which proves all pre-publication
    // ExecutionGuards (and therefore all old module calls) have left.
    if (reload_domain_.reclaim() == 0) return;
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
}

tresult PLUGIN_API EmberProcessor::terminate() {
    active_ = false;
    stopHotReload();

    delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    reload_domain_.quiesce();
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    delete current_.exchange(nullptr, std::memory_order_acq_rel);
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
    const tresult result = SingleComponentEffect::setupProcessing(setup);
    if (result != kResultOk) return result;

    // setupProcessing is a host control-thread call. Allocate the optional
    // old-output scratch here; process() only indexes this fixed buffer.
    const std::size_t frames = static_cast<std::size_t>(std::max<int32>(
        0, std::min<int32>(setup.maxSamplesPerBlock,
                           static_cast<int32>(crossfade_samples_))));
    try {
        crossfade_storage_.assign(frames * 2, 0.0f);
    } catch (...) {
        crossfade_storage_.clear();
    }
    if (frames > 0 && crossfade_storage_.size() == frames * 2) {
        crossfade_channels_[0] = crossfade_storage_.data();
        crossfade_channels_[1] = crossfade_storage_.data() + frames;
    } else {
        crossfade_channels_ = {{nullptr, nullptr}};
    }
    refreshLatencyAndTail();
    return kResultOk;
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
    if (sampleSize == kSample32) return kResultTrue;
    if (sampleSize != kSample64) return kResultFalse;
    EmberModule* current = current_.load(std::memory_order_acquire);
    return current && current->process_f64 ? kResultTrue : kResultFalse;
}

void EmberProcessor::refreshLatencyAndTail() noexcept {
    auto execution = reload_domain_.guard();
    EmberModule* current = current_.load(std::memory_order_acquire);
    const auto boundedReport = [](ReportSamplesFn callback) -> uint32 {
        if (!callback) return 0;
        const int64_t value = callback();
        return value <= 0 ? 0u : static_cast<uint32>(std::min<int64_t>(value, kMaxInt32u));
    };
    latency_samples_.store(boundedReport(current ? current->get_latency : nullptr),
                           std::memory_order_release);
    tail_samples_.store(boundedReport(current ? current->get_tail : nullptr),
                        std::memory_order_release);
}

uint32 PLUGIN_API EmberProcessor::getLatencySamples() {
    return latency_samples_.load(std::memory_order_acquire);
}

uint32 PLUGIN_API EmberProcessor::getTailSamples() {
    return tail_samples_.load(std::memory_order_acquire);
}

void EmberProcessor::bypass(ProcessData& data) const noexcept {
    if (data.numOutputs < 1 || !data.outputs) return;
    const bool hasInput = data.numInputs >= 1 && data.inputs;
    const int32 inputChannels = hasInput ? data.inputs[0].numChannels : 0;
    const int32 channels = std::min<int32>(2, data.outputs[0].numChannels);
    const bool sample64 = data.symbolicSampleSize == kSample64;
    const size_t sampleBytes = sample64 ? sizeof(Sample64) : sizeof(Sample32);
    void** outputBuffers = sample64
        ? reinterpret_cast<void**>(data.outputs[0].channelBuffers64)
        : reinterpret_cast<void**>(data.outputs[0].channelBuffers32);
    void** inputBuffers = nullptr;
    if (hasInput) {
        inputBuffers = sample64
            ? reinterpret_cast<void**>(data.inputs[0].channelBuffers64)
            : reinterpret_cast<void**>(data.inputs[0].channelBuffers32);
    }
    if (!outputBuffers) return;
    for (int32 channel = 0; channel < channels; ++channel) {
        void* output = outputBuffers[channel];
        void* input = inputBuffers && channel < inputChannels ? inputBuffers[channel] : nullptr;
        if (!output || data.numSamples <= 0) continue;
        if (input && input != output)
            std::memcpy(output, input, static_cast<size_t>(data.numSamples) * sampleBytes);
        else if (!input)
            std::memset(output, 0, static_cast<size_t>(data.numSamples) * sampleBytes);
    }
    data.outputs[0].silenceFlags = hasInput ? data.inputs[0].silenceFlags : 3u;
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

    std::size_t eventCount = 0;
    if (data.inputEvents) {
        const int32 count = data.inputEvents->getEventCount();
        for (int32 index = 0; index < count && eventCount < input_events_.size(); ++index) {
            Event source {};
            if (data.inputEvents->getEvent(index, source) != kResultTrue ||
                source.sampleOffset < 0 || source.sampleOffset >= data.numSamples)
                continue;
            ember::ext_audio::AudioEvent event;
            event.sample_offset = source.sampleOffset;
            switch (source.type) {
                case Event::kNoteOnEvent:
                    event.type = ember::ext_audio::kNoteOn;
                    event.channel = source.noteOn.channel;
                    event.note = source.noteOn.pitch;
                    event.velocity = static_cast<int64_t>(std::clamp(
                        source.noteOn.velocity, 0.0f, 1.0f) * 127.0f + 0.5f);
                    break;
                case Event::kNoteOffEvent:
                    event.type = ember::ext_audio::kNoteOff;
                    event.channel = source.noteOff.channel;
                    event.note = source.noteOff.pitch;
                    event.velocity = static_cast<int64_t>(std::clamp(
                        source.noteOff.velocity, 0.0f, 1.0f) * 127.0f + 0.5f);
                    break;
                case Event::kNoteExpressionValueEvent:
                    event.type = ember::ext_audio::kNoteExpression;
                    event.channel = 0;
                    event.note = source.noteExpressionValue.noteId;
                    event.velocity = static_cast<int64_t>(std::clamp(
                        source.noteExpressionValue.value, 0.0, 1.0) * 127.0 + 0.5);
                    break;
                case Event::kLegacyMIDICCOutEvent:
                    event.type = ember::ext_audio::kController;
                    event.channel = source.midiCCOut.channel;
                    event.note = source.midiCCOut.controlNumber;
                    event.velocity = source.midiCCOut.value;
                    break;
                default:
                    continue;
            }
            input_events_[eventCount++] = event;
        }
    }

    if (data.numSamples <= 0) return kResultOk;
    EmberModule* blockModule = current_.load(std::memory_order_acquire);
    const bool sample64 = data.symbolicSampleSize == kSample64;
    const bool sample32 = data.symbolicSampleSize == kSample32;
    if (!active_ || !blockModule || (!sample32 && !sample64) ||
        (sample64 && !blockModule->process_f64) ||
        data.numOutputs != 1 || !data.outputs || data.outputs[0].numChannels != 2 ||
        (data.numInputs != 0 && data.numInputs != 1) ||
        (data.numInputs == 1 && (!data.inputs || data.inputs[0].numChannels != 2))) {
        bypass(data);
        return kResultOk;
    }

    void** outputBuffers = sample64
        ? reinterpret_cast<void**>(data.outputs[0].channelBuffers64)
        : reinterpret_cast<void**>(data.outputs[0].channelBuffers32);
    void** inputBuffers = nullptr;
    if (data.numInputs == 1) {
        inputBuffers = sample64
            ? reinterpret_cast<void**>(data.inputs[0].channelBuffers64)
            : reinterpret_cast<void**>(data.inputs[0].channelBuffers32);
    }
    if (!outputBuffers) {
        bypass(data);
        return kResultOk;
    }
    for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
        if (!outputBuffers[channel] || (inputBuffers && !inputBuffers[channel])) {
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
    context.num_input_channels = data.numInputs == 1 ? data.inputs[0].numChannels : 0;
    context.num_output_channels = data.outputs[0].numChannels;
    context.input_buffer_ptr = reinterpret_cast<int64_t>(inputBuffers);
    context.output_buffer_ptr = reinterpret_cast<int64_t>(outputBuffers);
    context.sample_precision = sample64 ? 64 : 32;
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
    context.event_count = static_cast<int64_t>(eventCount);
    context.events_ptr = reinterpret_cast<int64_t>(input_events_.data());
    context.output_event_capacity = static_cast<int64_t>(output_events_.size());
    context.output_events_ptr = reinterpret_cast<int64_t>(output_events_.data());

    // The guard is acquired before the block-boundary publication. It pins the
    // old page if a publication occurs here, covering save_state(), the optional
    // old-processor crossfade render, and the new processor call.
    {
        auto execution = reload_domain_.guard();
        EmberModule* old = activatePendingModule(execution);
        EmberModule* current = current_.load(std::memory_order_acquire);
        ProcessFn processFn = reinterpret_cast<ProcessFn>(process_dispatch_.get(0));

        const std::size_t fadeFrames = !sample64 && old && old->process_f32 &&
            crossfade_channels_[0]
            ? std::min<std::size_t>({crossfade_samples_,
                                     static_cast<std::size_t>(data.numSamples),
                                     crossfade_storage_.size() / 2})
            : 0;
        if (fadeFrames > 0) {
            ember::ext_audio::AudioContext oldContext = context;
            oldContext.block_size = static_cast<int64_t>(fadeFrames);
            oldContext.output_buffer_ptr = reinterpret_cast<int64_t>(crossfade_channels_.data());
            oldContext.output_event_capacity = 0;
            oldContext.output_events_ptr = 0;
            old->process_f32(reinterpret_cast<int64_t>(&oldContext),
                             static_cast<int64_t>(fadeFrames));
        }

        ProcessFn selectedProcess = sample64
            ? (current ? current->process_f64 : nullptr) : processFn;
        if (!current || !selectedProcess) {
            bypass(data);
            return kResultOk;
        }
        selectedProcess(reinterpret_cast<int64_t>(&context), data.numSamples);

        // Linear old->new blend over the first N f32 samples. Both processors
        // see identical inputs and automation; only the new module remains active.
        for (std::size_t frame = 0; frame < fadeFrames; ++frame) {
            const float newWeight = static_cast<float>(frame + 1) /
                                    static_cast<float>(fadeFrames);
            const float oldWeight = 1.0f - newWeight;
            for (std::size_t channel = 0; channel < crossfade_channels_.size(); ++channel) {
                float* output = data.outputs[0].channelBuffers32[channel];
                output[frame] = crossfade_channels_[channel][frame] * oldWeight +
                                output[frame] * newWeight;
            }
        }
    }
    if (changeCount > 0) {
        for (std::size_t index = 0; index < changeCount; ++index) {
            const auto id = parameter_changes_[index].param_id;
            if (id >= 0 && static_cast<std::size_t>(id) < parameter_values_.size())
                parameter_values_[static_cast<std::size_t>(id)] =
                    parameter_changes_[index].value;
        }
    }
    if (data.outputEvents) {
        for (int64_t index = 0; index < context.output_event_count; ++index) {
            const auto& source = output_events_[static_cast<std::size_t>(index)];
            Event event {};
            event.busIndex = 0;
            event.sampleOffset = static_cast<int32>(source.sample_offset);
            switch (source.type) {
                case ember::ext_audio::kNoteOn:
                    event.type = Event::kNoteOnEvent;
                    event.noteOn.channel = static_cast<int16>(source.channel);
                    event.noteOn.pitch = static_cast<int16>(source.note);
                    event.noteOn.velocity = static_cast<float>(source.velocity) / 127.0f;
                    event.noteOn.noteId = -1;
                    break;
                case ember::ext_audio::kNoteOff:
                    event.type = Event::kNoteOffEvent;
                    event.noteOff.channel = static_cast<int16>(source.channel);
                    event.noteOff.pitch = static_cast<int16>(source.note);
                    event.noteOff.velocity = static_cast<float>(source.velocity) / 127.0f;
                    event.noteOff.noteId = -1;
                    break;
                case ember::ext_audio::kNoteExpression:
                    event.type = Event::kNoteExpressionValueEvent;
                    event.noteExpressionValue.noteId = static_cast<int32>(source.note);
                    event.noteExpressionValue.value = static_cast<double>(source.velocity) / 127.0;
                    break;
                case ember::ext_audio::kController:
                    event.type = Event::kLegacyMIDICCOutEvent;
                    event.midiCCOut.channel = static_cast<int8>(source.channel);
                    event.midiCCOut.controlNumber = static_cast<uint8>(source.note);
                    event.midiCCOut.value = static_cast<int8>(source.velocity);
                    break;
                default:
                    continue;
            }
            data.outputEvents->addEvent(event);
        }
    }
    // Conservatively clear silence when Ember ran: an instrument may generate
    // signal from events even when there is no input or the input is silent.
    data.outputs[0].silenceFlags = blockRemainsSilent ? 3u : 0u;
    return kResultOk;
}

tresult PLUGIN_API EmberProcessor::setState(IBStream* state) {
    if (!state) return kInvalidArgument;
    IBStreamer stream(state, kLittleEndian);
    uint32 magic = 0;
    int32 version = 0;
    float saved = 1.0f;
    int64 scriptSize = 0;
    if (!stream.readInt32u(magic) || magic != kStateMagic ||
        !stream.readInt32(version) || version != kStateVersion ||
        !stream.readFloat(saved) || !stream.readInt64(scriptSize) ||
        scriptSize < 0 || scriptSize > static_cast<int64>(kMaxStateBytes))
        return kResultFalse;

    std::vector<uint8_t> scriptState(static_cast<std::size_t>(scriptSize));
    if (scriptSize > 0 && stream.readRaw(scriptState.data(), scriptSize) != scriptSize)
        return kResultFalse;

    parameter_values_[0] = std::clamp(saved, 0.0f, 2.0f);
    setParamNormalized(kGainParamId, parameter_values_[0] / 2.0f);
    auto execution = reload_domain_.guard();
    EmberModule* current = current_.load(std::memory_order_acquire);
    if (current && current->load_state) {
        current->load_state(reinterpret_cast<int64_t>(scriptState.data()), scriptSize);
    }
    return kResultOk;
}

tresult PLUGIN_API EmberProcessor::getState(IBStream* state) {
    if (!state) return kInvalidArgument;

    const uint8_t* scriptBytes = nullptr;
    int64 scriptSize = 0;
    auto execution = reload_domain_.guard();
    EmberModule* current = current_.load(std::memory_order_acquire);
    if (current && current->save_state) {
        const auto* saved = reinterpret_cast<const ScriptStateBuffer*>(current->save_state());
        if (saved && saved->data && saved->size > 0 &&
            saved->size <= static_cast<int64_t>(kMaxStateBytes)) {
            scriptBytes = reinterpret_cast<const uint8_t*>(saved->data);
            scriptSize = saved->size;
        }
    }

    IBStreamer stream(state, kLittleEndian);
    if (!stream.writeInt32u(kStateMagic) || !stream.writeInt32(kStateVersion) ||
        !stream.writeFloat(parameter_values_[0]) || !stream.writeInt64(scriptSize))
        return kResultFalse;
    if (scriptSize > 0 && stream.writeRaw(scriptBytes, scriptSize) != scriptSize)
        return kResultFalse;
    return kResultOk;
}

} // namespace EmberVst3

