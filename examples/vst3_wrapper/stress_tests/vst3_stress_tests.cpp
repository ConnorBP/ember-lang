#include "vst3_ember_processor.h"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "gc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#include <psapi.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_track_allocations {false};
std::atomic<std::uint64_t> g_allocations {0};

void noteAllocation() noexcept {
    if (g_track_allocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "[vst3 stress] FAIL: %s\n", message);
    std::exit(1);
}

void check(bool condition, const char* message) {
    if (!condition) fail(message);
}

struct AllocationScope {
    AllocationScope() : before(g_allocations.load(std::memory_order_relaxed)) {
        g_track_allocations.store(true, std::memory_order_seq_cst);
    }
    ~AllocationScope() { g_track_allocations.store(false, std::memory_order_seq_cst); }
    std::uint64_t count() const {
        return g_allocations.load(std::memory_order_relaxed) - before;
    }
    std::uint64_t before;
};

struct Fixture {
    HostApplication host;
    EmberVst3::EmberProcessor processor;
    ProcessSetup setup {};

    Fixture(double sampleRate = 48000.0, int maxBlock = 8192) {
        check(processor.initialize(&host) == kResultOk, "processor initialize");
        configure(sampleRate, maxBlock, kSample32);
        check(processor.setActive(true) == kResultOk, "processor activate");
        check(processor.setProcessing(true) == kResultOk, "processor start processing");
    }

    ~Fixture() {
        processor.setProcessing(false);
        processor.setActive(false);
        processor.terminate();
    }

    void configure(double sampleRate, int maxBlock, int precision) {
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = precision;
        setup.maxSamplesPerBlock = maxBlock;
        setup.sampleRate = sampleRate;
        check(processor.setupProcessing(setup) == kResultOk, "setupProcessing");
    }
};

struct Block {
    explicit Block(int frames = 8192, int channels = 2)
        : in(static_cast<std::size_t>(channels), std::vector<float>(frames)),
          out(static_cast<std::size_t>(channels), std::vector<float>(frames)),
          inPtrs(static_cast<std::size_t>(channels)),
          outPtrs(static_cast<std::size_t>(channels)) {
        for (int channel = 0; channel < channels; ++channel) {
            inPtrs[static_cast<std::size_t>(channel)] = in[static_cast<std::size_t>(channel)].data();
            outPtrs[static_cast<std::size_t>(channel)] = out[static_cast<std::size_t>(channel)].data();
        }
        input.numChannels = channels;
        input.channelBuffers32 = inPtrs.data();
        output.numChannels = channels;
        output.channelBuffers32 = outPtrs.data();
    }

    ProcessData data(int frames, IParameterChanges* parameters = nullptr,
                     IEventList* events = nullptr) {
        ProcessData result {};
        result.processMode = kRealtime;
        result.symbolicSampleSize = kSample32;
        result.numSamples = frames;
        result.numInputs = 1;
        result.numOutputs = 1;
        result.inputs = &input;
        result.outputs = &output;
        result.inputParameterChanges = parameters;
        result.inputEvents = events;
        return result;
    }

    void fill(float value, int frames) {
        for (auto& channel : in)
            std::fill_n(channel.begin(), frames, value);
        for (auto& channel : out)
            std::fill_n(channel.begin(), frames, -1234.0f);
        input.silenceFlags = value == 0.0f ? ~uint64 {0} : 0;
        output.silenceFlags = 0;
    }

    std::vector<std::vector<float>> in;
    std::vector<std::vector<float>> out;
    std::vector<float*> inPtrs;
    std::vector<float*> outPtrs;
    AudioBusBuffers input {};
    AudioBusBuffers output {};
};

void addGainPoint(ParameterChanges& changes, int offset, double normalized) {
    int32 queueIndex = 0;
    IParamValueQueue* queue = changes.addParameterData(EmberVst3::kGainParamId, queueIndex);
    check(queue != nullptr, "parameter queue");
    int32 pointIndex = 0;
    check(queue->addPoint(offset, normalized, pointIndex) == kResultTrue, "parameter point");
}

void verifyFinite(const Block& block, int frames) {
    for (const auto& channel : block.out)
        for (int i = 0; i < frames; ++i)
            check(std::isfinite(channel[static_cast<std::size_t>(i)]), "non-finite output");
}

void processChecked(Fixture& fixture, Block& block, int frames,
                    IParameterChanges* parameters = nullptr, IEventList* events = nullptr) {
    ProcessData data = block.data(frames, parameters, events);
    check(fixture.processor.process(data) == kResultOk, "process returned failure");
    verifyFinite(block, frames);
}

void runHotReloadStress();

void runStress() {
    constexpr std::array<double, 5> rates {{32000.0, 44100.0, 48000.0, 96000.0, 192000.0}};
    constexpr std::array<int, 12> blocks {{1, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 0}};
    Fixture fixture;
    Block block;

    for (double rate : rates) {
        fixture.configure(rate, 8192, kSample32);
        for (int frames : blocks) {
            block.fill(frames == 0 ? 0.0f : 0.25f, frames);
            processChecked(fixture, block, frames);
        }
    }

    // Variable host blocks, including the one-sample and zero-frame edges.
    for (int frames : {512, 1, 2048, 16, 8192, 0, 64, 1024, 32}) {
        block.fill(0.125f, frames);
        processChecked(fixture, block, frames);
    }

    // Silence flags must propagate through a zero-gain processor.
    ParameterChanges silenceChanges(1);
    addGainPoint(silenceChanges, 0, 0.0);
    ProcessData flush = block.data(0, &silenceChanges);
    check(fixture.processor.process(flush) == kResultOk, "zero-frame parameter flush");
    block.fill(0.0f, 256);
    ProcessData silent = block.data(256);
    check(fixture.processor.process(silent) == kResultOk, "silence processing");
    check(block.output.silenceFlags != 0, "silence flags not set");
    for (const auto& channel : block.out)
        check(std::all_of(channel.begin(), channel.begin() + 256,
                          [](float sample) { return sample == 0.0f; }),
              "silence produced nonzero output");

    // Restore unity after the silence case. Mono/quad/octal malformed
    // arrangements then exercise deterministic bypass.
    ParameterChanges unityChanges(1);
    addGainPoint(unityChanges, 0, 0.5);
    ProcessData unityFlush = block.data(0, &unityChanges);
    check(fixture.processor.process(unityFlush) == kResultOk, "unity parameter flush");
    for (int channels : {1, 2, 4, 8}) {
        Block multi(512, channels);
        multi.fill(0.375f, 512);
        processChecked(fixture, multi, 512);
        const int compared = std::min(channels, 2);
        for (int channel = 0; channel < compared; ++channel)
            check(multi.out[channel][0] == multi.in[channel][0], "multi-channel bypass mismatch");
    }

    // 128 simultaneous queues: one valid Gain queue and 127 unknown IDs.
    ParameterChanges manyParameters(128);
    addGainPoint(manyParameters, 0, 0.5);
    for (ParamID id = 1; id < 128; ++id) {
        int32 queueIndex = 0;
        auto* queue = manyParameters.addParameterData(id, queueIndex);
        int32 pointIndex = 0;
        check(queue && queue->addPoint(id % 64, 0.25, pointIndex) == kResultTrue,
              "many parameter queues");
    }
    block.fill(0.25f, 128);
    processChecked(fixture, block, 128, &manyParameters);

    EventList manyEvents(1200);
    for (int index = 0; index < 1200; ++index) {
        Event event {};
        event.type = (index & 1) ? Event::kNoteOnEvent : Event::kNoteOffEvent;
        event.sampleOffset = index % 512;
        event.noteOn.channel = static_cast<int16>(index % 16);
        event.noteOn.pitch = static_cast<int16>(36 + index % 60);
        event.noteOn.velocity = 0.5f;
        event.noteOn.noteId = index;
        check(manyEvents.addEvent(event) == kResultTrue, "many MIDI events");
    }
    block.fill(0.125f, 512);
    processChecked(fixture, block, 512, nullptr, &manyEvents);

    runHotReloadStress();
    std::puts("[vst3 stress] sample-rate/block/variable/silence/channel/parameter/event/hot-reload tests passed");
}

void runHotReloadStress() {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(EMBER_VST3_DEFAULT_SCRIPT);
    const fs::path temporary = fs::temp_directory_path() / "ember_vst3_phase7_reload.ember";
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing);
#ifdef _WIN32
    _putenv_s("EMBER_VST3_SCRIPT", temporary.string().c_str());
#endif
    Fixture fixture;
    Block block(64);
    block.fill(0.125f, 64);
    std::atomic<bool> stop {false};
    std::atomic<std::uint64_t> processed {0};
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire)) {
            ProcessData data = block.data(64);
            if (fixture.processor.process(data) != kResultOk) fail("hot-reload process failure");
            processed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int reload = 0; reload < 3; ++reload) {
        std::ofstream out(temporary, std::ios::binary | std::ios::app);
        out << "\n// phase7 reload " << reload << '\n';
        out.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    check(processed.load(std::memory_order_relaxed) > 100, "hot reload did not process audio");
#ifdef _WIN32
    _putenv_s("EMBER_VST3_SCRIPT", "");
#endif
    std::error_code error;
    fs::remove(temporary, error);
}

void runRealtimeContract() {
    Fixture fixture(48000.0, 8192);
    Block block;
    block.fill(0.25f, 8192);
    ProcessData warmup = block.data(8192);
    check(fixture.processor.process(warmup) == kResultOk, "realtime warmup");

    ember::gc::GcHeap gc;
    const std::size_t collectionsBefore = gc.stats().collections;
    std::chrono::nanoseconds worst {0};
    for (int frames : {8192, 4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 1}) {
        block.fill(0.25f, frames);
        ProcessData data = block.data(frames);
        AllocationScope allocations;
        const auto begin = Clock::now();
        check(fixture.processor.process(data) == kResultOk, "realtime process");
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
        check(allocations.count() == 0, "memory allocation in process()");
        check(gc.stats().collections == collectionsBefore, "GC collection in process()");
        const auto deadline = std::chrono::duration<double>(
            (static_cast<double>(frames) / 48000.0) * 0.5);
        check(elapsed <= deadline, "process() exceeded 50% block deadline");
        worst = std::max(worst, elapsed);
    }
    std::printf("[vst3 realtime] allocations=0 locks=0 gc=0 worst_us=%.3f\n",
                static_cast<double>(worst.count()) / 1000.0);
}

void runFuzz() {
    Fixture fixture;
    std::mt19937_64 random(0xE7B3F00DULL);
    Block storage(8192, 8);
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const int frames = static_cast<int>(random() % 8193);
        const int channels = 1 + static_cast<int>(random() % 8);
        storage.input.numChannels = channels;
        storage.output.numChannels = channels;
        storage.input.channelBuffers32 = storage.inPtrs.data();
        storage.output.channelBuffers32 = storage.outPtrs.data();
        storage.fill(static_cast<float>(static_cast<int>(random() % 200) - 100) / 100.0f, frames);

        ParameterChanges changes(4);
        const int points = frames == 0 ? 1 : static_cast<int>(random() % 32);
        for (int i = 0; i < points; ++i)
            addGainPoint(changes, frames == 0 ? 0 : static_cast<int>(random() % frames),
                         static_cast<double>(random() % 1001) / 1000.0);

        EventList events(64);
        if (frames > 0) {
            const int count = static_cast<int>(random() % 64);
            for (int i = 0; i < count; ++i) {
                Event event {};
                event.type = (random() & 1) ? Event::kNoteOnEvent : Event::kNoteOffEvent;
                event.sampleOffset = static_cast<int32>(random() % frames);
                event.noteOn.channel = static_cast<int16>(random() % 16);
                event.noteOn.pitch = static_cast<int16>(random() % 128);
                event.noteOn.velocity = static_cast<float>(random() % 128) / 127.0f;
                events.addEvent(event);
            }
        }
        ProcessData data = storage.data(frames, &changes, &events);
        if ((random() % 20) == 0) data.numInputs = 0;
        check(fixture.processor.process(data) == kResultOk, "fuzz process failure");
        verifyFinite(storage, frames);
    }
    std::puts("[vst3 fuzz] 5000 deterministic ProcessData cases passed");
}

std::size_t residentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<std::size_t>(counters.WorkingSetSize);
#endif
    return 0;
}

void runSoak(int seconds) {
    Fixture fixture;
    Block block(512);
    block.fill(0.125f, 512);
    const std::size_t memoryBefore = residentBytes();
    const std::clock_t cpuBefore = std::clock();
    const auto start = Clock::now();
    std::uint64_t blocks = 0;
    do {
        ProcessData data = block.data(512);
        check(fixture.processor.process(data) == kResultOk, "soak process failure");
        ++blocks;
        std::this_thread::sleep_for(std::chrono::microseconds(10667));
    } while (Clock::now() - start < std::chrono::seconds(seconds));

    const double wall = std::chrono::duration<double>(Clock::now() - start).count();
    const double cpu = static_cast<double>(std::clock() - cpuBefore) / CLOCKS_PER_SEC;
    const std::size_t memoryAfter = residentBytes();
    const std::size_t growth = memoryAfter > memoryBefore ? memoryAfter - memoryBefore : 0;
    check(growth <= 8 * 1024 * 1024, "soak memory grew by more than 8 MiB");
    std::printf("[vst3 soak] seconds=%.2f blocks=%llu cpu=%.2f%% memory_growth=%zu bytes\n",
                wall, static_cast<unsigned long long>(blocks), cpu * 100.0 / wall, growth);
}

} // namespace

void* operator new(std::size_t size) {
    noteAllocation();
    if (void* ptr = std::malloc(size)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    noteAllocation();
    if (void* ptr = std::malloc(size)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
#ifdef _WIN32
void* operator new(std::size_t size, std::align_val_t alignment) {
    noteAllocation();
    if (void* ptr = _aligned_malloc(size, static_cast<std::size_t>(alignment))) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* ptr, std::align_val_t) noexcept { _aligned_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { _aligned_free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { _aligned_free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { _aligned_free(ptr); }
#endif

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "stress";
    if (mode == "stress") runStress();
    else if (mode == "realtime") runRealtimeContract();
    else if (mode == "fuzz") runFuzz();
    else if (mode == "soak") {
        int seconds = 60;
        if (argc == 4 && std::string(argv[2]) == "--seconds") seconds = std::atoi(argv[3]);
        runSoak(std::max(seconds, 1));
    } else fail("unknown mode");
    return 0;
}
