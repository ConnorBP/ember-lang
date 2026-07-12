// Headless DSP validation for the future VST3 wrapper. No VST SDK is used:
// this host compiles Ember DSP, passes host-owned buffers through ext_audio,
// and differentially checks the JIT against straightforward C++ references.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "jit_memory.hpp"
#include "binding_builder.hpp"
#include "ext_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int64_t n_delay_buffer();
int64_t n_delay_size();

struct Module {
    ember::Program program;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::StructLayoutTable layouts;
    ember::GlobalsBlock globals;
    std::vector<uint8_t> global_storage;
    std::unique_ptr<ember::DispatchTable> dispatch;
    std::vector<ember::CompiledFn> functions;

    ~Module() {
        for (auto& fn : functions) {
            if (fn.exec) ember::free_executable(fn.exec);
        }
    }

    void* entry(const std::string& name) const {
        const auto it = slots.find(name);
        return it == slots.end() ? nullptr : dispatch->get(it->second);
    }
};

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    if (file) text << file.rdbuf();
    return text.str();
}

std::unique_ptr<Module> compile_dsp(const char* path) {
    const std::string source = read_file(path);
    if (source.empty()) {
        std::fprintf(stderr, "DSP harness: cannot read %s\n", path);
        return nullptr;
    }

    auto lexed = ember::tokenize(source, path);
    if (!lexed.ok) {
        std::fprintf(stderr, "DSP harness: lex failed for %s: %s\n",
                     path, lexed.error.c_str());
        return nullptr;
    }
    auto parsed = ember::parse(std::move(lexed.toks));
    if (!parsed.ok) {
        std::fprintf(stderr, "DSP harness: parse failed for %s: %s\n",
                     path, parsed.error.c_str());
        return nullptr;
    }

    auto module = std::make_unique<Module>();
    module->program = std::move(parsed.program);
    int slot = 0;
    for (auto& fn : module->program.funcs) {
        fn.slot = slot;
        module->slots[fn.name] = slot++;
    }

    ember::ext_audio::register_natives(module->natives);
    ember::BindingBuilder state_bindings;
    state_bindings.add("delay_buffer", ember::type_i64(), {},
                       reinterpret_cast<void*>(&n_delay_buffer));
    state_bindings.add("delay_size", ember::type_i64(), {},
                       reinterpret_cast<void*>(&n_delay_size));
    auto state_table = state_bindings.build();
    for (auto& item : state_table.natives)
        module->natives[item.first] = std::move(item.second);
    module->layouts = ember::build_struct_layouts(module->program);
    module->program.string_xor_key = 0;
    auto checked = ember::sema(module->program, module->natives, module->slots,
                               ember::PERM_FFI, nullptr, &module->layouts);
    if (!checked.ok) {
        std::fprintf(stderr, "DSP harness: sema failed for %s (%zu errors):\n",
                     path, checked.errors.size());
        for (const auto& error : checked.errors) {
            std::fprintf(stderr, "  line %u:%u: %s\n", error.line, error.col,
                         error.msg.c_str());
        }
        return nullptr;
    }

    // The current engine API exposes the parse/sema/codegen pieces directly;
    // this Module owns the same engine state a plugin instance will own.
    module->global_storage.assign(module->program.globals.size() * 8, 0);
    module->globals.base = reinterpret_cast<int64_t>(module->global_storage.data());
    ember::g_globals_for_codegen = &module->globals;
    module->dispatch = std::make_unique<ember::DispatchTable>(module->program.funcs.size());

    ember::CodeGenCtx context;
    context.globals_base = module->globals.base;
    context.dispatch_base = reinterpret_cast<int64_t>(module->dispatch->base());
    context.natives = &module->natives;
    context.script_slots = &module->slots;
    context.structs = &module->layouts;

    for (auto& fn : module->program.funcs) {
        ember::CompiledFn compiled = ember::compile_func(fn, context);
        if (!ember::finalize(compiled)) {
            std::fprintf(stderr, "DSP harness: executable allocation failed for %s\n",
                         fn.name.c_str());
            ember::g_globals_for_codegen = nullptr;
            return nullptr;
        }
        module->dispatch->set(fn.slot, compiled.entry);
        module->functions.push_back(std::move(compiled));
    }
    ember::g_globals_for_codegen = nullptr;
    return module;
}

using GainProcess = void (*)(int64_t, int64_t);
using DelayProcess = int64_t (*)(int64_t, int64_t, int64_t, int64_t);

int64_t ptr_i64(float* ptr) {
    return reinterpret_cast<int64_t>(ptr);
}

constexpr int64_t kFrames = 1024;
constexpr int64_t kChannels = 2;
constexpr int64_t kSamples = kFrames * kChannels;
constexpr int64_t kDelaySamples = 137;

// Ember's script-to-script ABI supports the delay process's six arguments;
// a four-argument Ember adapter is the C++ call boundary. These natives bind
// that adapter to the current preallocated delay line without allocation.
int64_t g_delay_samples = kDelaySamples;
float* g_delay_buffer = nullptr;

int64_t n_delay_buffer() {
    return reinterpret_cast<int64_t>(g_delay_buffer);
}

int64_t n_delay_size() {
    return g_delay_samples;
}

std::vector<float> make_input(int64_t channels = kChannels) {
    std::vector<float> input(static_cast<size_t>(kFrames * channels), 0.0f);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (int64_t channel = 0; channel < channels; ++channel) {
        for (int64_t frame = 0; frame < kFrames; ++frame) {
            const double phase = 2.0 * pi * (220.0 + 110.0 * double(channel)) *
                                 double(frame) / 48000.0;
            const double scale = 0.8 / double(channel + 1);
            input[size_t(channel * kFrames + frame)] = float(std::sin(phase) * scale);
        }
    }
    return input;
}

std::vector<float> reference_gain(
    const std::vector<float>& input, int64_t channels,
    const std::vector<ember::ext_audio::ParameterChange>& changes = {}) {
    std::vector<float> output(input.size());
    float gain = 0.5f;
    size_t change_index = 0;
    for (int64_t frame = 0; frame < kFrames; ++frame) {
        while (change_index < changes.size() &&
               changes[change_index].sample_offset <= frame) {
            if (changes[change_index].param_id == 0) gain = changes[change_index].value;
            ++change_index;
        }
        for (int64_t channel = 0; channel < channels; ++channel) {
            const size_t index = size_t(channel * kFrames + frame);
            output[index] = input[index] * gain;
        }
    }
    return output;
}

std::vector<float> run_gain(
    GainProcess process, const std::vector<float>& input, int64_t channels,
    int64_t block_size,
    const std::vector<ember::ext_audio::ParameterChange>& changes = {}) {
    std::vector<float> output(input.size(), 0.0f);
    std::vector<float*> input_channels(static_cast<size_t>(channels));
    std::vector<float*> output_channels(static_cast<size_t>(channels));
    float parameter_values[] = {0.5f};
    size_t next_change = 0;

    for (int64_t offset = 0; offset < kFrames; offset += block_size) {
        const int64_t frames = std::min(block_size, kFrames - offset);
        for (int64_t channel = 0; channel < channels; ++channel) {
            input_channels[size_t(channel)] =
                const_cast<float*>(input.data()) + channel * kFrames + offset;
            output_channels[size_t(channel)] = output.data() + channel * kFrames + offset;
        }

        std::vector<ember::ext_audio::ParameterChange> block_changes;
        while (next_change < changes.size() &&
               changes[next_change].sample_offset < offset + frames) {
            auto change = changes[next_change++];
            if (change.sample_offset >= offset) {
                change.sample_offset -= offset;
                block_changes.push_back(change);
            }
        }

        ember::ext_audio::AudioContext context;
        context.sample_rate = 48000;
        context.block_size = frames;
        context.num_input_channels = channels;
        context.num_output_channels = channels;
        context.input_buffer_ptr = reinterpret_cast<int64_t>(input_channels.data());
        context.output_buffer_ptr = reinterpret_cast<int64_t>(output_channels.data());
        context.transport_playing = 1;
        context.transport_bpm = 123.0;
        context.transport_ppq = 17.25;
        context.parameter_count = 1;
        context.parameter_values_ptr = reinterpret_cast<int64_t>(parameter_values);
        context.parameter_change_count = static_cast<int64_t>(block_changes.size());
        context.parameter_changes_ptr = reinterpret_cast<int64_t>(block_changes.data());
        process(reinterpret_cast<int64_t>(&context), frames);
        if (!block_changes.empty()) parameter_values[0] = block_changes.back().value;
    }
    return output;
}

std::vector<float> reference_delay(const std::vector<float>& input,
                                   int64_t block_size) {
    std::vector<float> output(input.size(), 0.0f);
    for (int64_t channel = 0; channel < kChannels; ++channel) {
        std::vector<float> delay(size_t(kDelaySamples), 0.0f);
        int64_t position = 0;
        const int64_t base = channel * kFrames;
        for (int64_t offset = 0; offset < kFrames; offset += block_size) {
            const int64_t frames = std::min(block_size, kFrames - offset);
            for (int64_t i = 0; i < frames; ++i) {
                const float sample = input[size_t(base + offset + i)];
                const float delayed = delay[size_t(position)];
                delay[size_t(position)] = sample;
                output[size_t(base + offset + i)] = sample + delayed * 0.5f;
                position = (position + 1) % kDelaySamples;
            }
        }
    }
    return output;
}

std::vector<float> run_delay(DelayProcess process, const std::vector<float>& input,
                             int64_t block_size) {
    std::vector<float> output(input.size(), 0.0f);
    for (int64_t channel = 0; channel < kChannels; ++channel) {
        std::vector<float> delay(size_t(kDelaySamples), 0.0f);
        int64_t position = 0;
        const int64_t base = channel * kFrames;
        for (int64_t offset = 0; offset < kFrames; offset += block_size) {
            const int64_t frames = std::min(block_size, kFrames - offset);
            g_delay_buffer = delay.data();
            position = process(
                ptr_i64(const_cast<float*>(input.data()) + base + offset),
                ptr_i64(output.data() + base + offset), frames, position);
        }
    }
    return output;
}

bool bit_equal(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    return lhs.size() == rhs.size() &&
           std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(float)) == 0;
}

bool near_equal(const std::vector<float>& lhs, const std::vector<float>& rhs,
                float tolerance) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::fabs(lhs[i] - rhs[i]) > tolerance) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3) {
        std::fprintf(stderr, "usage: vst_dsp_harness <gain_plugin.ember> <delay_plugin.ember>\n");
        return 2;
    }

    auto gain_module = compile_dsp(argv[1]);
    auto delay_module = compile_dsp(argv[2]);
    if (!gain_module || !delay_module) return 1;

    auto gain = reinterpret_cast<GainProcess>(gain_module->entry("process"));
    // process_host is a C++ ABI adapter which calls the required six-argument
    // Ember `process` entry through Ember's own script-to-script ABI.
    auto delay = reinterpret_cast<DelayProcess>(delay_module->entry("process_host"));
    if (!gain || !delay || !delay_module->entry("process")) {
        std::fprintf(stderr, "DSP harness: process entry point missing\n");
        return 1;
    }

    const std::vector<float> input = make_input();
    const std::vector<float> gain_reference = reference_gain(input, kChannels);
    const std::vector<int64_t> blocks = {1024, 256, 64, 1};
    int failures = 0;

    std::vector<float> gain_canonical;
    std::vector<float> delay_canonical;
    for (const int64_t block : blocks) {
        const auto gain_output = run_gain(gain, input, kChannels, block);
        if (gain_canonical.empty()) gain_canonical = gain_output;
        const bool gain_ok = bit_equal(gain_output, gain_reference) &&
                             bit_equal(gain_output, gain_canonical);
        std::printf("[%s] gain: 1024 frames as %lld x %lld (bit exact)\n",
                    gain_ok ? "PASS" : "FAIL",
                    static_cast<long long>(kFrames / block),
                    static_cast<long long>(block));
        if (!gain_ok) ++failures;

        const auto delay_reference = reference_delay(input, block);
        const auto delay_output = run_delay(delay, input, block);
        if (delay_canonical.empty()) delay_canonical = delay_output;
        const bool delay_ok = near_equal(delay_output, delay_reference, 1.0e-6f) &&
                              near_equal(delay_output, delay_canonical, 1.0e-6f);
        std::printf("[%s] delay: 1024 frames as %lld x %lld (tol 1e-6)\n",
                    delay_ok ? "PASS" : "FAIL",
                    static_cast<long long>(kFrames / block),
                    static_cast<long long>(block));
        if (!delay_ok) ++failures;
    }

    for (const int64_t channels : {1LL, 2LL, 4LL}) {
        const auto multichannel_input = make_input(channels);
        const auto expected = reference_gain(multichannel_input, channels);
        const auto actual = run_gain(gain, multichannel_input, channels, 64);
        const bool ok = bit_equal(actual, expected);
        std::printf("[%s] typed AudioContext: %lld channel(s)\n",
                    ok ? "PASS" : "FAIL", static_cast<long long>(channels));
        if (!ok) ++failures;
    }

    const std::vector<ember::ext_audio::ParameterChange> automation = {
        {0, 0, 0.25f}, {0, 127, 1.0f}, {0, 512, 1.5f}, {0, 901, 0.0f}};
    const auto automation_reference = reference_gain(input, kChannels, automation);
    std::vector<float> automation_canonical;
    for (const int64_t block : blocks) {
        const auto output = run_gain(gain, input, kChannels, block, automation);
        if (automation_canonical.empty()) automation_canonical = output;
        const bool ok = bit_equal(output, automation_reference) &&
                        bit_equal(output, automation_canonical);
        std::printf("[%s] automation: block %lld (sample accurate)\n",
                    ok ? "PASS" : "FAIL", static_cast<long long>(block));
        if (!ok) ++failures;
    }

    std::printf("\nheadless DSP harness: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
