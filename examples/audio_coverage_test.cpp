// Direct coverage for typed audio contexts, events and raw sample helpers.
#include "ext_audio.hpp"
#include "ast.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

using namespace ember;
using namespace ember::ext_audio;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 1; } } while (0)

template <typename Fn>
static Fn native(const std::unordered_map<std::string, NativeSig>& n, const char* name) {
    const auto it = n.find(name); if (it == n.end() || !it->second.fn_ptr) std::abort();
    return reinterpret_cast<Fn>(it->second.fn_ptr);
}

int main() {
    std::unordered_map<std::string, NativeSig> n;
    ext_audio::register_natives(n);

    float in0[] = {0.25f, -0.5f, 1.0f, 0.0f};
    float in1[] = {0.5f, 0.75f, -1.0f, 0.125f};
    float out0[4]{}; float out1[4]{};
    float* inputs[] = {in0, in1}; float* outputs[] = {out0, out1};
    float params[] = {0.5f, 0.75f};
    ParameterChange changes[] = {{1, 2, 0.25f}, {0, 3, 0.9f}};
    AudioEvent events[] = {{kNoteOn, 2, 60, 100, 1}, {kController, 3, 74, 64, 2}};
    AudioEvent output_events[2]{};
    AudioContext ctx;
    ctx.sample_rate = 48000; ctx.block_size = 4;
    ctx.num_input_channels = 2; ctx.num_output_channels = 2;
    ctx.input_buffer_ptr = reinterpret_cast<int64_t>(inputs);
    ctx.output_buffer_ptr = reinterpret_cast<int64_t>(outputs);
    ctx.sample_precision = 32; ctx.transport_playing = 1;
    ctx.transport_bpm = 128.5; ctx.transport_ppq = 42.25;
    ctx.parameter_count = 2; ctx.parameter_values_ptr = reinterpret_cast<int64_t>(params);
    ctx.parameter_change_count = 2; ctx.parameter_changes_ptr = reinterpret_cast<int64_t>(changes);
    ctx.event_count = 2; ctx.events_ptr = reinterpret_cast<int64_t>(events);
    ctx.output_event_capacity = 2; ctx.output_events_ptr = reinterpret_cast<int64_t>(output_events);
    const int64_t p = reinterpret_cast<int64_t>(&ctx);

    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_sample_rate")(p) == 48000);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_block_size")(p) == 4);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_num_input_channels")(p) == 2);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_num_output_channels")(p) == 2);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_sample_rate")(0) == 0);

    const auto load = native<float(*)(int64_t, int64_t, int64_t)>(n, "audio_load_sample");
    const auto store = native<void(*)(int64_t, int64_t, int64_t, float)>(n, "audio_store_sample");
    CHECK(load(p, 0, 1) == -0.5f && load(p, 1, 2) == -1.0f);
    store(p, 0, 1, -0.25f); store(p, 1, 3, 0.5f);
    CHECK(out0[1] == -0.25f && out1[3] == 0.5f);
    CHECK(load(0, 0, 0) == 0.0f && load(p, -1, 0) == 0.0f && load(p, 0, 4) == 0.0f);
    store(0, 0, 0, 1.0f); store(p, 2, 0, 1.0f); store(p, 0, -1, 1.0f);

    const auto get_param = native<float(*)(int64_t, int64_t)>(n, "audio_get_parameter");
    CHECK(get_param(p, 0) == 0.5f && get_param(p, 1) == 0.75f);
    CHECK(get_param(0, 0) == 0.0f && get_param(p, 2) == 0.0f);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_is_playing")(p) == 1);
    CHECK(native<int64_t(*)(int64_t)>(n, "audio_is_playing")(0) == 0);
    CHECK(native<double(*)(int64_t)>(n, "audio_get_bpm")(p) == 128.5);
    CHECK(native<double(*)(int64_t)>(n, "audio_get_ppq")(p) == 42.25);

    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_param_change_count")(p) == 2);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_param_change_id")(p, 0) == 1);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_param_change_offset")(p, 1) == 3);
    CHECK(native<float(*)(int64_t, int64_t)>(n, "audio_get_param_change_value")(p, 0) == 0.25f);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_param_change_id")(p, 9) == 0);

    CHECK(native<int64_t(*)(int64_t)>(n, "audio_get_event_count")(p) == 2);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_type")(p, 0) == kNoteOn);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_channel")(p, 1) == 3);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_note")(p, 0) == 60);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_velocity")(p, 0) == 100);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_offset")(p, 1) == 2);
    CHECK(native<int64_t(*)(int64_t, int64_t)>(n, "audio_get_event_note")(p, 9) == 0);

    const auto add_event = native<void(*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)>(n, "audio_add_event");
    add_event(p, kNoteOff, 1, 61, 80, 3);
    CHECK(ctx.output_event_count == 1 && output_events[0].type == kNoteOff && output_events[0].note == 61);
    add_event(p, kController, 15, 127, 127, 0);
    CHECK(ctx.output_event_count == 2 && output_events[1].channel == 15);
    add_event(p, kNoteOn, 0, 1, 1, 0); // full
    add_event(p, 99, 0, 1, 1, 0); add_event(0, kNoteOn, 0, 1, 1, 0);
    CHECK(ctx.output_event_count == 2);

    double din0[] = {1.25, -2.5}; double dout0[2]{};
    double* dinputs[] = {din0}; double* doutputs[] = {dout0};
    AudioContext dctx; dctx.block_size = 2; dctx.num_input_channels = 1; dctx.num_output_channels = 1;
    dctx.input_buffer_ptr = reinterpret_cast<int64_t>(dinputs); dctx.output_buffer_ptr = reinterpret_cast<int64_t>(doutputs); dctx.sample_precision = 64;
    const int64_t dp = reinterpret_cast<int64_t>(&dctx);
    const auto load64 = native<double(*)(int64_t, int64_t, int64_t)>(n, "audio_load_sample_f64");
    const auto store64 = native<void(*)(int64_t, int64_t, int64_t, double)>(n, "audio_store_sample_f64");
    CHECK(load64(dp, 0, 1) == -2.5); store64(dp, 0, 0, 3.5); CHECK(dout0[0] == 3.5);
    CHECK(load64(p, 0, 0) == 0.0); store64(p, 0, 0, 9.0);

    float rawf[2]{}; double rawd[2]{}; int32_t rawi[2]{};
    native<void(*)(int64_t, int64_t, float)>(n, "store_f32")(reinterpret_cast<int64_t>(rawf), 1, 2.5f);
    CHECK(native<float(*)(int64_t, int64_t)>(n, "load_f32")(reinterpret_cast<int64_t>(rawf), 1) == 2.5f);
    native<void(*)(int64_t, int64_t, double)>(n, "store_f64")(reinterpret_cast<int64_t>(rawd), 0, 6.25);
    CHECK(native<double(*)(int64_t, int64_t)>(n, "load_f64")(reinterpret_cast<int64_t>(rawd), 0) == 6.25);
    native<void(*)(int64_t, int64_t, int32_t)>(n, "store_i32")(reinterpret_cast<int64_t>(rawi), 1, -17);
    CHECK(native<int32_t(*)(int64_t, int64_t)>(n, "load_i32")(reinterpret_cast<int64_t>(rawi), 1) == -17);

    std::puts("audio coverage: PASS");
    return 0;
}
