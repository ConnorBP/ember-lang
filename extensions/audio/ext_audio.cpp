// ext_audio.cpp - allocation-free access to host-owned DSP buffers.
#include "ext_audio.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"

#include <cstdint>

namespace ember::ext_audio {
namespace {

const AudioContext* audio_context(int64_t ptr) {
    return reinterpret_cast<const AudioContext*>(ptr);
}

AudioContext* audio_context_mut(int64_t ptr) {
    return reinterpret_cast<AudioContext*>(ptr);
}

int64_t n_audio_get_sample_rate(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->sample_rate : 0;
}

int64_t n_audio_get_block_size(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->block_size : 0;
}

int64_t n_audio_get_num_input_channels(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->num_input_channels : 0;
}

int64_t n_audio_get_num_output_channels(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->num_output_channels : 0;
}

float n_audio_load_sample(int64_t ptr, int64_t channel, int64_t index) {
    const auto* ctx = audio_context(ptr);
    if (!ctx || ctx->sample_precision != 32 ||
        channel < 0 || channel >= ctx->num_input_channels ||
        index < 0 || index >= ctx->block_size || !ctx->input_buffer_ptr)
        return 0.0f;
    auto* const* channels = reinterpret_cast<float* const*>(ctx->input_buffer_ptr);
    return channels[channel] ? channels[channel][index] : 0.0f;
}

void n_audio_store_sample(int64_t ptr, int64_t channel, int64_t index, float value) {
    auto* ctx = audio_context_mut(ptr);
    if (!ctx || ctx->sample_precision != 32 ||
        channel < 0 || channel >= ctx->num_output_channels ||
        index < 0 || index >= ctx->block_size || !ctx->output_buffer_ptr)
        return;
    auto** channels = reinterpret_cast<float**>(ctx->output_buffer_ptr);
    if (channels[channel]) channels[channel][index] = value;
}

double n_audio_load_sample_f64(int64_t ptr, int64_t channel, int64_t index) {
    const auto* ctx = audio_context(ptr);
    if (!ctx || ctx->sample_precision != 64 ||
        channel < 0 || channel >= ctx->num_input_channels ||
        index < 0 || index >= ctx->block_size || !ctx->input_buffer_ptr)
        return 0.0;
    auto* const* channels = reinterpret_cast<double* const*>(ctx->input_buffer_ptr);
    return channels[channel] ? channels[channel][index] : 0.0;
}

void n_audio_store_sample_f64(int64_t ptr, int64_t channel, int64_t index, double value) {
    auto* ctx = audio_context_mut(ptr);
    if (!ctx || ctx->sample_precision != 64 ||
        channel < 0 || channel >= ctx->num_output_channels ||
        index < 0 || index >= ctx->block_size || !ctx->output_buffer_ptr)
        return;
    auto** channels = reinterpret_cast<double**>(ctx->output_buffer_ptr);
    if (channels[channel]) channels[channel][index] = value;
}

float n_audio_get_parameter(int64_t ptr, int64_t param_id) {
    const auto* ctx = audio_context(ptr);
    if (!ctx || param_id < 0 || param_id >= ctx->parameter_count ||
        !ctx->parameter_values_ptr)
        return 0.0f;
    return reinterpret_cast<const float*>(ctx->parameter_values_ptr)[param_id];
}

int64_t n_audio_is_playing(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? (ctx->transport_playing != 0) : 0;
}

double n_audio_get_bpm(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->transport_bpm : 0.0;
}

double n_audio_get_ppq(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->transport_ppq : 0.0;
}

int64_t n_audio_get_param_change_count(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->parameter_change_count : 0;
}

const ParameterChange* parameter_change(int64_t ptr, int64_t index) {
    const auto* ctx = audio_context(ptr);
    if (!ctx || index < 0 || index >= ctx->parameter_change_count ||
        !ctx->parameter_changes_ptr)
        return nullptr;
    return reinterpret_cast<const ParameterChange*>(ctx->parameter_changes_ptr) + index;
}

int64_t n_audio_get_param_change_id(int64_t ptr, int64_t index) {
    const auto* change = parameter_change(ptr, index);
    return change ? change->param_id : 0;
}

int64_t n_audio_get_param_change_offset(int64_t ptr, int64_t index) {
    const auto* change = parameter_change(ptr, index);
    return change ? change->sample_offset : 0;
}

float n_audio_get_param_change_value(int64_t ptr, int64_t index) {
    const auto* change = parameter_change(ptr, index);
    return change ? change->value : 0.0f;
}

const AudioEvent* audio_event(int64_t ptr, int64_t index) {
    const auto* ctx = audio_context(ptr);
    if (!ctx || index < 0 || index >= ctx->event_count || !ctx->events_ptr)
        return nullptr;
    return reinterpret_cast<const AudioEvent*>(ctx->events_ptr) + index;
}

int64_t n_audio_get_event_count(int64_t ptr) {
    const auto* ctx = audio_context(ptr);
    return ctx ? ctx->event_count : 0;
}

int64_t n_audio_get_event_type(int64_t ptr, int64_t index) {
    const auto* event = audio_event(ptr, index);
    return event ? event->type : 0;
}

int64_t n_audio_get_event_channel(int64_t ptr, int64_t index) {
    const auto* event = audio_event(ptr, index);
    return event ? event->channel : 0;
}

int64_t n_audio_get_event_note(int64_t ptr, int64_t index) {
    const auto* event = audio_event(ptr, index);
    return event ? event->note : 0;
}

int64_t n_audio_get_event_velocity(int64_t ptr, int64_t index) {
    const auto* event = audio_event(ptr, index);
    return event ? event->velocity : 0;
}

int64_t n_audio_get_event_offset(int64_t ptr, int64_t index) {
    const auto* event = audio_event(ptr, index);
    return event ? event->sample_offset : 0;
}

void n_audio_add_event(int64_t ptr, int64_t type, int64_t channel,
                       int64_t note, int64_t velocity, int64_t offset) {
    auto* ctx = audio_context_mut(ptr);
    if (!ctx || !ctx->output_events_ptr || ctx->output_event_count < 0 ||
        ctx->output_event_count >= ctx->output_event_capacity ||
        (type < kNoteOn || type > kController) ||
        channel < 0 || channel > 15 || note < 0 || note > 127 ||
        velocity < 0 || velocity > 127 || offset < 0 || offset >= ctx->block_size)
        return;
    auto* events = reinterpret_cast<AudioEvent*>(ctx->output_events_ptr);
    events[ctx->output_event_count++] = {type, channel, note, velocity, offset};
}

float n_load_f32(int64_t ptr, int64_t index) {
    return reinterpret_cast<float*>(ptr)[index];
}

void n_store_f32(int64_t ptr, int64_t index, float value) {
    reinterpret_cast<float*>(ptr)[index] = value;
}

double n_load_f64(int64_t ptr, int64_t index) {
    return reinterpret_cast<double*>(ptr)[index];
}

void n_store_f64(int64_t ptr, int64_t index, double value) {
    reinterpret_cast<double*>(ptr)[index] = value;
}

int32_t n_load_i32(int64_t ptr, int64_t index) {
    return reinterpret_cast<int32_t*>(ptr)[index];
}

void n_store_i32(int64_t ptr, int64_t index, int32_t value) {
    reinterpret_cast<int32_t*>(ptr)[index] = value;
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder builder;
    const Type i32 = make_prim(Prim::I32);
    const Type i64 = type_i64();
    const Type f32 = type_f32();
    const Type f64 = type_f64();

    builder.add("audio_get_sample_rate", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_sample_rate), PERM_FFI);
    builder.add("audio_get_block_size", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_block_size), PERM_FFI);
    builder.add("audio_get_num_input_channels", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_num_input_channels), PERM_FFI);
    builder.add("audio_get_num_output_channels", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_num_output_channels), PERM_FFI);
    builder.add("audio_load_sample", f32, {i64, i64, i64},
                reinterpret_cast<void*>(&n_audio_load_sample), PERM_FFI);
    builder.add("audio_store_sample", type_void(), {i64, i64, i64, f32},
                reinterpret_cast<void*>(&n_audio_store_sample), PERM_FFI);
    builder.add("audio_load_sample_f64", f64, {i64, i64, i64},
                reinterpret_cast<void*>(&n_audio_load_sample_f64), PERM_FFI);
    builder.add("audio_store_sample_f64", type_void(), {i64, i64, i64, f64},
                reinterpret_cast<void*>(&n_audio_store_sample_f64), PERM_FFI);
    builder.add("audio_get_parameter", f32, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_parameter), PERM_FFI);
    builder.add("audio_is_playing", i64, {i64},
                reinterpret_cast<void*>(&n_audio_is_playing), PERM_FFI);
    builder.add("audio_get_bpm", f64, {i64},
                reinterpret_cast<void*>(&n_audio_get_bpm), PERM_FFI);
    builder.add("audio_get_ppq", f64, {i64},
                reinterpret_cast<void*>(&n_audio_get_ppq), PERM_FFI);
    builder.add("audio_get_param_change_count", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_param_change_count), PERM_FFI);
    builder.add("audio_get_param_change_id", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_param_change_id), PERM_FFI);
    builder.add("audio_get_param_change_offset", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_param_change_offset), PERM_FFI);
    builder.add("audio_get_param_change_value", f32, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_param_change_value), PERM_FFI);
    builder.add("audio_get_event_count", i64, {i64},
                reinterpret_cast<void*>(&n_audio_get_event_count), PERM_FFI);
    builder.add("audio_get_event_type", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_event_type), PERM_FFI);
    builder.add("audio_get_event_channel", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_event_channel), PERM_FFI);
    builder.add("audio_get_event_note", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_event_note), PERM_FFI);
    builder.add("audio_get_event_velocity", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_event_velocity), PERM_FFI);
    builder.add("audio_get_event_offset", i64, {i64, i64},
                reinterpret_cast<void*>(&n_audio_get_event_offset), PERM_FFI);
    builder.add("audio_add_event", type_void(), {i64, i64, i64, i64, i64, i64},
                reinterpret_cast<void*>(&n_audio_add_event), PERM_FFI);

    builder.add("load_f32",  f32, {i64, i64},
                reinterpret_cast<void*>(&n_load_f32), PERM_FFI);
    builder.add("store_f32", type_void(), {i64, i64, f32},
                reinterpret_cast<void*>(&n_store_f32), PERM_FFI);
    builder.add("load_f64",  f64, {i64, i64},
                reinterpret_cast<void*>(&n_load_f64), PERM_FFI);
    builder.add("store_f64", type_void(), {i64, i64, f64},
                reinterpret_cast<void*>(&n_store_f64), PERM_FFI);
    builder.add("load_i32",  i32, {i64, i64},
                reinterpret_cast<void*>(&n_load_i32), PERM_FFI);
    builder.add("store_i32", type_void(), {i64, i64, i32},
                reinterpret_cast<void*>(&n_store_i32), PERM_FFI);

    NativeTable table = builder.build();
    for (auto& item : table.natives) {
        natives[item.first] = std::move(item.second);
    }
}

} // namespace ember::ext_audio
