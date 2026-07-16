// ext_visualize.hpp - lock-free audio snapshots, analysis, and LLM export.
#pragma once

#include "sema.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_visualize {

void register_natives(std::unordered_map<std::string, NativeSig>& natives);

// Audio-thread producer. Copies a bounded mono mix into a lock-free snapshot;
// analysis and formatting are deferred to the UI/control thread.
void publish_f32(float* const* channels, std::int64_t channel_count,
                 std::int64_t frames) noexcept;
void publish_f64(double* const* channels, std::int64_t channel_count,
                 std::int64_t frames) noexcept;

void set_parameter_metadata(const float* values, const float* minimums,
                            const float* maximums, const char* const* names,
                            std::size_t count, std::string source_hash);

// C++ editor-side accessors over the same snapshot used by the natives.
void copy_spectrum(float* output, std::size_t bins);
void copy_waveform(float* output, std::size_t samples);
float rms_value();
float peak_value();

} // namespace ember::ext_visualize
