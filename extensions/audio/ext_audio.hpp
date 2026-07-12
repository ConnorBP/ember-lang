// ext_audio.hpp - realtime-safe typed audio context and raw buffer natives.
#pragma once

#include "sema.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_audio {

// A host-owned, allocation-free parameter event. Events are ordered by
// sample_offset before an AudioContext is handed to Ember.
struct ParameterChange {
    std::int64_t param_id {0};
    std::int64_t sample_offset {0};
    float value {0.0f};
};

// Stable event kinds used by Ember scripts. They intentionally do not expose
// the VST3 Event union: hosts translate their native event representation into
// this compact, allocation-free form.
enum AudioEventType : std::int64_t {
    kNoteOn = 0,
    kNoteOff = 1,
    kNoteExpression = 2,
    kController = 3,
};

struct AudioEvent {
    std::int64_t type {kNoteOn};
    std::int64_t channel {0};
    std::int64_t note {0};
    std::int64_t velocity {0}; // MIDI-style [0, 127]
    std::int64_t sample_offset {0};
};

// The typed audio-plane ABI shared by native hosts and Ember DSP scripts.
// Pointer fields address host-owned storage that remains valid for the whole
// process call. Buffer pointers address arrays of f32 or f64 channel pointers,
// according to sample_precision (32 or 64).
struct AudioContext {
    std::int64_t sample_rate {0};
    std::int64_t block_size {0};
    std::int64_t num_input_channels {0};
    std::int64_t num_output_channels {0};
    std::int64_t input_buffer_ptr {0};
    std::int64_t output_buffer_ptr {0};
    std::int64_t sample_precision {32};
    std::int64_t transport_playing {0};
    double transport_bpm {0.0};
    double transport_ppq {0.0};
    std::int64_t parameter_count {0};
    std::int64_t parameter_values_ptr {0};
    std::int64_t parameter_change_count {0};
    std::int64_t parameter_changes_ptr {0};
    std::int64_t event_count {0};
    std::int64_t events_ptr {0};
    std::int64_t output_event_count {0};
    std::int64_t output_event_capacity {0};
    std::int64_t output_events_ptr {0};
};

// Register typed AudioContext accessors and the legacy raw f32/f64/i32 buffer
// helpers. All bindings are PERM_FFI-gated because Ember i64 values carry raw
// host pointers. Typed accessors bounds-check channel, frame, and event indices.
void register_natives(std::unordered_map<std::string, NativeSig>& natives);

} // namespace ember::ext_audio
